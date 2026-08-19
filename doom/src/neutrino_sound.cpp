#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "descriptors.hpp"
#include "syscall.hpp"

extern "C" {
#include "deh_str.h"
#include "i_sound.h"
#include "m_misc.h"
#include "w_wad.h"
#include "z_zone.h"
}

namespace {

constexpr uint32_t kAudioOutputType =
    static_cast<uint32_t>(descriptor_defs::Type::AudioOutput);
constexpr uint32_t kOutputRate = 48000;
constexpr size_t kMixerChannels = 16;
constexpr size_t kFramesPerChunk = 1024;
constexpr size_t kTargetQueueBytes = 12 * 1024;
constexpr int kGainScale = 256;
constexpr int kGainDenominator = 127 * 254;
constexpr size_t kMusicVoices = 24;
constexpr uint32_t kMusTicksPerSecond = 140;
constexpr uint32_t kReleaseStep = 32;

struct CachedSound {
    const uint8_t* samples;
    uint32_t length;
    uint32_t step_q16;
};

struct MixerChannel {
    CachedSound* sound;
    uint64_t cursor_q16;
    int left_gain;
    int right_gain;
    bool playing;
};

struct Song {
    const uint8_t* data;
    size_t score_start;
    size_t score_end;
};

struct MusicChannel {
    uint8_t velocity;
    uint8_t volume;
    uint8_t pan;
    uint8_t program;
    uint8_t pitch;
};

struct MusicVoice {
    uint32_t phase;
    uint32_t step;
    uint32_t age;
    uint16_t envelope;
    uint8_t channel;
    uint8_t note;
    uint8_t velocity;
    bool active;
    bool releasing;
};

uint32_t g_audio = UINT32_MAX;
MixerChannel g_channels[kMixerChannels]{};
alignas(16) int16_t g_mix_buffer[kFramesPerChunk * 2]{};
bool g_use_sfx_prefix = true;
bool g_sound_initialized = false;
bool g_music_initialized = false;
Song* g_song = nullptr;
size_t g_song_cursor = 0;
uint64_t g_samples_until_event = 0;
uint32_t g_tick_remainder = 0;
MusicChannel g_music_channels[16]{};
MusicVoice g_music_voices[kMusicVoices]{};
uint32_t g_note_steps[128]{};
int g_music_volume = 127;
bool g_music_playing = false;
bool g_music_paused = false;
bool g_music_looping = false;

int clamp_sample(int64_t value) {
    if (value < -32768) return -32768;
    if (value > 32767) return 32767;
    return static_cast<int>(value);
}

bool get_audio_status(descriptor_defs::AudioStatusInfo& status) {
    return g_audio != UINT32_MAX &&
           descriptor_get_property(
               g_audio,
               static_cast<uint32_t>(descriptor_defs::Property::AudioStatus),
               &status, sizeof(status)) == 0;
}

void audio_control(uint32_t command) {
    if (g_audio == UINT32_MAX) return;
    descriptor_defs::AudioControlInfo control{command, 0};
    (void)descriptor_set_property(
        g_audio,
        static_cast<uint32_t>(descriptor_defs::Property::AudioControl),
        &control, sizeof(control));
}

bool open_audio() {
    if (g_audio != UINT32_MAX) return true;

    long opened = descriptor_open(kAudioOutputType, 0);
    if (opened < 0) return false;
    g_audio = static_cast<uint32_t>(opened);

    descriptor_defs::AudioFormatInfo format{};
    if (descriptor_get_property(
            g_audio,
            static_cast<uint32_t>(descriptor_defs::Property::AudioFormat),
            &format, sizeof(format)) != 0 ||
        format.sample_rate != kOutputRate || format.channels != 2 ||
        format.bits_per_sample != 16 || format.frame_bytes != 4) {
        descriptor_close(g_audio);
        g_audio = UINT32_MAX;
        return false;
    }
    return true;
}

void close_audio_if_unused() {
    if (g_audio == UINT32_MAX ||
        g_sound_initialized || g_music_initialized) {
        return;
    }
    audio_control(descriptor_defs::kAudioCommandFlush);
    descriptor_close(g_audio);
    g_audio = UINT32_MAX;
}

void get_lump_name(sfxinfo_t* sound, char* name, size_t capacity) {
    if (sound->link != nullptr) sound = sound->link;
    if (g_use_sfx_prefix) {
        M_snprintf(name, capacity, "ds%s", DEH_String(sound->name));
    } else {
        M_StringCopy(name, DEH_String(sound->name), capacity);
    }
}

CachedSound* cache_sound(sfxinfo_t* info) {
    if (info->driver_data != nullptr) {
        return static_cast<CachedSound*>(info->driver_data);
    }
    if (info->lumpnum < 0) return nullptr;

    auto* lump =
        static_cast<const uint8_t*>(W_CacheLumpNum(info->lumpnum, PU_STATIC));
    uint32_t lump_length = static_cast<uint32_t>(W_LumpLength(info->lumpnum));
    if (lump == nullptr || lump_length < 8 || lump[0] != 0x03 ||
        lump[1] != 0x00) {
        return nullptr;
    }

    uint32_t sample_rate =
        static_cast<uint32_t>(lump[2]) |
        (static_cast<uint32_t>(lump[3]) << 8);
    uint32_t length =
        static_cast<uint32_t>(lump[4]) |
        (static_cast<uint32_t>(lump[5]) << 8) |
        (static_cast<uint32_t>(lump[6]) << 16) |
        (static_cast<uint32_t>(lump[7]) << 24);
    if (sample_rate == 0 || length > lump_length - 8 || length <= 48) {
        return nullptr;
    }

    // Match DMX/Chocolate Doom by dropping the 16 padding samples at each
    // end of the effect.
    auto* cached = static_cast<CachedSound*>(malloc(sizeof(CachedSound)));
    if (cached == nullptr) return nullptr;
    cached->samples = lump + 24;
    cached->length = length - 32;
    cached->step_q16 = static_cast<uint32_t>(
        (static_cast<uint64_t>(sample_rate) << 16) / kOutputRate);
    if (cached->step_q16 == 0) cached->step_q16 = 1;
    info->driver_data = cached;
    return cached;
}

void set_channel_params(MixerChannel& channel, int volume, int separation) {
    if (volume < 0) volume = 0;
    if (volume > 127) volume = 127;
    if (separation < 0) separation = 0;
    if (separation > 254) separation = 254;
    channel.left_gain =
        volume * (254 - separation) * kGainScale / kGainDenominator;
    channel.right_gain =
        volume * separation * kGainScale / kGainDenominator;
}

bool any_channel_playing() {
    for (const auto& channel : g_channels) {
        if (channel.playing) return true;
    }
    return false;
}

void reset_music_channels() {
    for (auto& channel : g_music_channels) {
        channel.velocity = 127;
        channel.volume = 127;
        channel.pan = 64;
        channel.program = 0;
        channel.pitch = 128;
    }
}

void build_note_steps() {
    // Q32 phase increments. 69433 / 65536 approximates one equal-tempered
    // semitone, anchored at A4 = 440 Hz.
    g_note_steps[69] = static_cast<uint32_t>(
        (UINT64_C(440) << 32) / kOutputRate);
    for (size_t note = 70; note < 128; ++note) {
        g_note_steps[note] = static_cast<uint32_t>(
            (static_cast<uint64_t>(g_note_steps[note - 1]) * 69433) >> 16);
    }
    for (int note = 68; note >= 0; --note) {
        g_note_steps[note] = static_cast<uint32_t>(
            (static_cast<uint64_t>(g_note_steps[note + 1]) << 16) / 69433);
    }
}

uint32_t pitched_step(uint8_t note, uint8_t pitch) {
    uint64_t step = g_note_steps[note];
    // MUS pitch bend is centered at 128 and spans roughly two semitones.
    int bend = static_cast<int>(pitch) - 128;
    int factor = 65536 + bend * 94;
    if (factor < 1) factor = 1;
    return static_cast<uint32_t>(step * static_cast<uint32_t>(factor) >> 16);
}

void update_channel_pitch(uint8_t channel) {
    for (auto& voice : g_music_voices) {
        if (voice.active && voice.channel == channel) {
            voice.step = pitched_step(
                voice.note, g_music_channels[channel].pitch);
        }
    }
}

void release_channel(uint8_t channel, int note) {
    for (auto& voice : g_music_voices) {
        if (voice.active && voice.channel == channel &&
            (note < 0 || voice.note == static_cast<uint8_t>(note))) {
            voice.releasing = true;
        }
    }
}

void note_on(uint8_t channel, uint8_t note, uint8_t velocity) {
    release_channel(channel, note);

    MusicVoice* selected = nullptr;
    for (auto& voice : g_music_voices) {
        if (!voice.active) {
            selected = &voice;
            break;
        }
        if (selected == nullptr || voice.envelope < selected->envelope ||
            (voice.envelope == selected->envelope &&
             voice.age > selected->age)) {
            selected = &voice;
        }
    }
    if (selected == nullptr) return;
    *selected = MusicVoice{
        0,
        pitched_step(note, g_music_channels[channel].pitch),
        0,
        32767,
        channel,
        note,
        velocity,
        true,
        false,
    };
}

bool song_byte(uint8_t& value) {
    if (g_song == nullptr || g_song_cursor >= g_song->score_end) return false;
    value = g_song->data[g_song_cursor++];
    return true;
}

void stop_music_state() {
    g_music_playing = false;
    g_music_paused = false;
    g_samples_until_event = 0;
    memset(g_music_voices, 0, sizeof(g_music_voices));
}

void restart_song() {
    if (g_song == nullptr) {
        stop_music_state();
        return;
    }
    g_song_cursor = g_song->score_start;
    g_samples_until_event = 0;
    g_tick_remainder = 0;
    reset_music_channels();
    memset(g_music_voices, 0, sizeof(g_music_voices));
    g_music_playing = true;
}

bool process_music_events() {
    // A valid MUS stream may contain several zero-delay event groups. Bound
    // work here so malformed WAD data cannot trap the audio update loop.
    for (size_t groups = 0; groups < 1024; ++groups) {
        bool last = false;
        do {
            uint8_t descriptor;
            if (!song_byte(descriptor)) {
                stop_music_state();
                return false;
            }
            last = (descriptor & 0x80) != 0;
            uint8_t channel = descriptor & 0x0f;
            uint8_t type = (descriptor >> 4) & 0x07;
            uint8_t first = 0;
            uint8_t second = 0;

            switch (type) {
                case 0:
                    if (!song_byte(first)) goto malformed;
                    release_channel(channel, first & 0x7f);
                    break;
                case 1:
                    if (!song_byte(first)) goto malformed;
                    if ((first & 0x80) != 0) {
                        if (!song_byte(second)) goto malformed;
                        g_music_channels[channel].velocity = second & 0x7f;
                    }
                    note_on(channel, first & 0x7f,
                            g_music_channels[channel].velocity);
                    break;
                case 2:
                    if (!song_byte(first)) goto malformed;
                    g_music_channels[channel].pitch = first;
                    update_channel_pitch(channel);
                    break;
                case 3:
                    if (!song_byte(first)) goto malformed;
                    if (first == 10 || first == 11) {
                        release_channel(channel, -1);
                    } else if (first == 14) {
                        g_music_channels[channel] =
                            MusicChannel{127, 127, 64, 0, 128};
                        release_channel(channel, -1);
                    }
                    break;
                case 4:
                    if (!song_byte(first) || !song_byte(second)) goto malformed;
                    second &= 0x7f;
                    if (first == 0) {
                        g_music_channels[channel].program = second;
                    } else if (first == 3) {
                        g_music_channels[channel].volume = second;
                    } else if (first == 4) {
                        g_music_channels[channel].pan = second;
                    }
                    break;
                case 6:
                    if (g_music_looping) {
                        restart_song();
                        return true;
                    }
                    stop_music_state();
                    return false;
                default:
                    goto malformed;
            }
        } while (!last);

        uint32_t ticks = 0;
        uint8_t value;
        do {
            if (!song_byte(value) || ticks > (UINT32_MAX >> 7)) goto malformed;
            ticks = (ticks << 7) | (value & 0x7f);
        } while ((value & 0x80) != 0);

        uint64_t scaled =
            static_cast<uint64_t>(ticks) * kOutputRate + g_tick_remainder;
        g_samples_until_event = scaled / kMusTicksPerSecond;
        g_tick_remainder =
            static_cast<uint32_t>(scaled % kMusTicksPerSecond);
        if (g_samples_until_event != 0) return true;
    }

malformed:
    stop_music_state();
    return false;
}

int music_wave(MusicVoice& voice) {
    uint32_t phase = voice.phase;
    voice.phase += voice.step;
    ++voice.age;

    if (voice.channel == 15) {
        // A cheap deterministic noise source makes percussion distinct.
        uint32_t noise = phase ^ (phase >> 7) ^ (voice.age * 1103515245u);
        return (noise & 0x80000000u) != 0 ? 24576 : -24576;
    }

    uint32_t position = phase >> 16;
    int triangle = position < 32768
        ? static_cast<int>(position * 2) - 32768
        : 98303 - static_cast<int>(position * 2);

    // Give reed/brass-like program families a brighter square component.
    uint8_t family = g_music_channels[voice.channel].program >> 3;
    if (family == 7 || family == 8 || family == 10) {
        int square = (phase & 0x80000000u) != 0 ? 16384 : -16384;
        return (triangle + square) / 2;
    }
    return triangle;
}

void mix_music_frame(int64_t& left, int64_t& right) {
    if (!g_music_playing || g_music_paused) return;

    while (g_samples_until_event == 0 && g_music_playing) {
        if (!process_music_events()) break;
    }
    if (!g_music_playing) return;

    for (auto& voice : g_music_voices) {
        if (!voice.active) continue;
        auto& channel = g_music_channels[voice.channel];
        int64_t sample = music_wave(voice);
        sample = sample * voice.velocity * channel.volume * g_music_volume *
                 voice.envelope /
                 (INT64_C(127) * 127 * 127 * 32767 * 6);
        left += sample * (127 - channel.pan) / 64;
        right += sample * channel.pan / 64;

        if (voice.channel == 15 && voice.age > kOutputRate / 8) {
            voice.releasing = true;
        }
        if (voice.releasing) {
            if (voice.envelope <= kReleaseStep) {
                voice.active = false;
            } else {
                voice.envelope -= kReleaseStep;
            }
        }
    }
    --g_samples_until_event;
}

bool any_audio_playing() {
    return any_channel_playing() ||
           (g_music_playing && !g_music_paused);
}

void mix_chunk() {
    memset(g_mix_buffer, 0, sizeof(g_mix_buffer));
    for (size_t frame = 0; frame < kFramesPerChunk; ++frame) {
        int64_t left = 0;
        int64_t right = 0;
        for (auto& channel : g_channels) {
            if (!channel.playing || channel.sound == nullptr) continue;
            uint64_t sample_index = channel.cursor_q16 >> 16;
            if (sample_index >= channel.sound->length) {
                channel.playing = false;
                continue;
            }
            int sample =
                (static_cast<int>(channel.sound->samples[sample_index]) - 128)
                << 8;
            left += static_cast<int64_t>(sample) * channel.left_gain /
                    kGainScale;
            right += static_cast<int64_t>(sample) * channel.right_gain /
                     kGainScale;
            channel.cursor_q16 += channel.sound->step_q16;
        }
        mix_music_frame(left, right);
        g_mix_buffer[frame * 2] =
            static_cast<int16_t>(clamp_sample(left));
        g_mix_buffer[frame * 2 + 1] =
            static_cast<int16_t>(clamp_sample(right));
    }
}

boolean init_sound(boolean use_sfx_prefix) {
    if (!open_audio()) return false;
    g_sound_initialized = true;
    g_use_sfx_prefix = use_sfx_prefix;
    memset(g_channels, 0, sizeof(g_channels));
    return true;
}

void shutdown_sound() {
    memset(g_channels, 0, sizeof(g_channels));
    g_sound_initialized = false;
    close_audio_if_unused();
}

int get_sfx_lump_num(sfxinfo_t* info) {
    char name[9]{};
    get_lump_name(info, name, sizeof(name));
    return W_GetNumForName(name);
}

void update_sound() {
    if (g_audio == UINT32_MAX) return;
    // AudioStatus refreshes HDA's DMA position and stops the cyclic stream
    // after an underrun. Keep polling even when Doom has no active mixer
    // channels, otherwise the controller can replay stale ring contents.
    descriptor_defs::AudioStatusInfo status{};
    if (!get_audio_status(status)) return;
    if (!any_audio_playing()) return;
    uint64_t queued = status.queued_bytes;
    while (queued < kTargetQueueBytes && any_audio_playing()) {
        mix_chunk();
        long written =
            descriptor_write(g_audio, g_mix_buffer, sizeof(g_mix_buffer));
        if (written <= 0) break;
        queued += static_cast<uint64_t>(written);
    }
}

void update_sound_params(int channel, int volume, int separation) {
    if (channel < 0 || static_cast<size_t>(channel) >= kMixerChannels) return;
    set_channel_params(g_channels[channel], volume, separation);
}

int start_sound(sfxinfo_t* info, int channel, int volume, int separation) {
    if (g_audio == UINT32_MAX || channel < 0 ||
        static_cast<size_t>(channel) >= kMixerChannels) {
        return -1;
    }
    CachedSound* sound = cache_sound(info);
    if (sound == nullptr) return -1;
    auto& output = g_channels[channel];
    output.sound = sound;
    output.cursor_q16 = 0;
    output.playing = true;
    set_channel_params(output, volume, separation);
    return channel;
}

void stop_sound(int channel) {
    if (channel < 0 || static_cast<size_t>(channel) >= kMixerChannels) return;
    g_channels[channel].playing = false;
}

boolean sound_is_playing(int channel) {
    if (channel < 0 || static_cast<size_t>(channel) >= kMixerChannels) {
        return false;
    }
    auto& output = g_channels[channel];
    if (!output.playing || output.sound == nullptr) return false;
    if ((output.cursor_q16 >> 16) >= output.sound->length) {
        output.playing = false;
        return false;
    }
    return true;
}

void precache_sounds(sfxinfo_t*, int) {}

boolean init_music() {
    if (!open_audio()) return false;
    g_music_initialized = true;
    build_note_steps();
    reset_music_channels();
    return true;
}

void shutdown_music() {
    stop_music_state();
    g_song = nullptr;
    g_music_initialized = false;
    close_audio_if_unused();
}

void set_music_volume(int volume) {
    if (volume < 0) volume = 0;
    if (volume > 127) volume = 127;
    g_music_volume = volume;
}

void pause_music() {
    g_music_paused = true;
}

void resume_music() {
    if (g_music_playing) g_music_paused = false;
}

void* register_song(void* data, int length) {
    if (data == nullptr || length < 16) return nullptr;
    auto* bytes = static_cast<const uint8_t*>(data);
    if (bytes[0] != 'M' || bytes[1] != 'U' ||
        bytes[2] != 'S' || bytes[3] != 0x1a) {
        return nullptr;
    }

    size_t score_length =
        static_cast<size_t>(bytes[4]) |
        (static_cast<size_t>(bytes[5]) << 8);
    size_t score_start =
        static_cast<size_t>(bytes[6]) |
        (static_cast<size_t>(bytes[7]) << 8);
    size_t data_length = static_cast<size_t>(length);
    if (score_start >= data_length ||
        score_length > data_length - score_start) {
        return nullptr;
    }

    auto* song = static_cast<Song*>(malloc(sizeof(Song)));
    if (song == nullptr) return nullptr;
    *song = Song{bytes, score_start, score_start + score_length};
    return song;
}

void unregister_song(void* handle) {
    if (handle == nullptr) return;
    if (g_song == handle) {
        stop_music_state();
        g_song = nullptr;
    }
    free(handle);
}

void play_song(void* handle, boolean looping) {
    if (handle == nullptr || !g_music_initialized) return;
    g_song = static_cast<Song*>(handle);
    g_music_looping = looping;
    restart_song();
}

void stop_song() {
    stop_music_state();
}

boolean music_is_playing() {
    return g_music_playing;
}

void poll_music() {
    // When sound effects are disabled there is no sound-module Update call.
    if (!g_sound_initialized) update_sound();
}

snddevice_t g_sound_devices[] = {
    SNDDEVICE_SB,
    SNDDEVICE_PAS,
    SNDDEVICE_GUS,
    SNDDEVICE_WAVEBLASTER,
    SNDDEVICE_SOUNDCANVAS,
    SNDDEVICE_AWE32,
};

}  // namespace

extern "C" {

sound_module_t DG_sound_module = {
    g_sound_devices,
    static_cast<int>(sizeof(g_sound_devices) / sizeof(g_sound_devices[0])),
    init_sound,
    shutdown_sound,
    get_sfx_lump_num,
    update_sound,
    update_sound_params,
    start_sound,
    stop_sound,
    sound_is_playing,
    precache_sounds,
};

music_module_t DG_music_module = {
    g_sound_devices,
    static_cast<int>(sizeof(g_sound_devices) / sizeof(g_sound_devices[0])),
    init_music,
    shutdown_music,
    set_music_volume,
    pause_music,
    resume_music,
    register_song,
    unregister_song,
    play_song,
    stop_song,
    music_is_playing,
    poll_music,
};

}
