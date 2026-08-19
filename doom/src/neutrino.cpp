#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "descriptors.hpp"
#include "desktop_protocol.hpp"
#include "doomgeneric.h"
#include "doomkeys.h"
#include "keyboard_scancode.hpp"
#include "syscall.hpp"

namespace {

constexpr uint32_t kKeyboardType =
    static_cast<uint32_t>(descriptor_defs::Type::Keyboard);
constexpr size_t kMaxArgs = 32;
constexpr size_t kArgumentBytes = 1024;

uint32_t g_session = UINT32_MAX;
uint32_t g_framebuffer = UINT32_MAX;
uint32_t g_keyboard = UINT32_MAX;
uint32_t g_desktop_server = UINT32_MAX;
uint32_t g_desktop_events = UINT32_MAX;
uint32_t g_desktop_surface = UINT32_MAX;
uint32_t g_desktop_window = 0;
uint64_t g_desktop_token = 0;
uint32_t* g_desktop_pixels = nullptr;
bool g_desktop_mode = false;
bool g_active = false;
const char* g_desktop_connect_error = "not attempted";
descriptor_defs::FramebufferInfo g_info{};
uint32_t g_bytes_per_pixel = 0;
uint32_t g_scale = 1;
uint32_t g_left = 0;
uint32_t g_top = 0;

struct KeyQueueEntry {
    int pressed;
    unsigned char key;
};

constexpr size_t kKeyQueueSize = 64;
KeyQueueEntry g_key_queue[kKeyQueueSize]{};
size_t g_key_read = 0;
size_t g_key_write = 0;
uint8_t g_event_bytes[512]{};
size_t g_event_used = 0;

uint32_t channel_bits(uint8_t value, uint8_t size, uint8_t shift) {
    if (size == 0 || size > 8 || shift >= 32) return 0;
    uint32_t maximum = (1u << size) - 1u;
    return ((static_cast<uint32_t>(value) * maximum + 127u) / 255u) << shift;
}

uint32_t native_pixel(uint32_t argb) {
    uint8_t red = static_cast<uint8_t>(argb >> 16);
    uint8_t green = static_cast<uint8_t>(argb >> 8);
    uint8_t blue = static_cast<uint8_t>(argb);
    return channel_bits(red, g_info.red_mask_size, g_info.red_mask_shift) |
           channel_bits(green, g_info.green_mask_size, g_info.green_mask_shift) |
           channel_bits(blue, g_info.blue_mask_size, g_info.blue_mask_shift);
}

void store_pixel(uint8_t* output, uint32_t pixel) {
    for (uint32_t byte = 0; byte < g_bytes_per_pixel; ++byte) {
        output[byte] = static_cast<uint8_t>(pixel >> (byte * 8));
    }
}

void queue_key(int pressed, unsigned char key) {
    if (key == 0) return;
    size_t next = (g_key_write + 1) % kKeyQueueSize;
    if (next == g_key_read) {
        g_key_read = (g_key_read + 1) % kKeyQueueSize;
    }
    g_key_queue[g_key_write] = {pressed, key};
    g_key_write = next;
}

void append_decimal(char* output, size_t capacity, uint64_t value) {
    if (output == nullptr || capacity == 0) return;
    char reverse[24];
    size_t count = 0;
    do {
        reverse[count++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value != 0 && count < sizeof(reverse));
    size_t used = strlen(output);
    while (count != 0 && used + 1 < capacity) output[used++] = reverse[--count];
    output[used] = '\0';
}

unsigned char doom_key(const descriptor_defs::KeyboardEvent& event) {
    switch (event.scancode) {
        case 0x01: return KEY_ESCAPE;
        case 0x0E: return KEY_BACKSPACE;
        case 0x0F: return KEY_TAB;
        case 0x1C: return KEY_ENTER;
        case 0x1D: return KEY_FIRE;
        case 0x2A:
        case 0x36: return KEY_RSHIFT;
        case 0x38: return KEY_RALT;
        case 0x39: return KEY_USE;
        case 0x3A: return KEY_CAPSLOCK;
        case 0x3B: return KEY_F1;
        case 0x3C: return KEY_F2;
        case 0x3D: return KEY_F3;
        case 0x3E: return KEY_F4;
        case 0x3F: return KEY_F5;
        case 0x40: return KEY_F6;
        case 0x41: return KEY_F7;
        case 0x42: return KEY_F8;
        case 0x43: return KEY_F9;
        case 0x44: return KEY_F10;
        case 0x45: return KEY_NUMLOCK;
        case 0x46: return KEY_SCRLCK;
        case 0x47: return event.flags & descriptor_defs::kKeyboardFlagExtended
                              ? KEY_HOME : '7';
        case 0x48: return event.flags & descriptor_defs::kKeyboardFlagExtended
                              ? KEY_UPARROW : '8';
        case 0x49: return event.flags & descriptor_defs::kKeyboardFlagExtended
                              ? KEY_PGUP : '9';
        case 0x4B: return event.flags & descriptor_defs::kKeyboardFlagExtended
                              ? KEY_LEFTARROW : '4';
        case 0x4D: return event.flags & descriptor_defs::kKeyboardFlagExtended
                              ? KEY_RIGHTARROW : '6';
        case 0x4F: return event.flags & descriptor_defs::kKeyboardFlagExtended
                              ? KEY_END : '1';
        case 0x50: return event.flags & descriptor_defs::kKeyboardFlagExtended
                              ? KEY_DOWNARROW : '2';
        case 0x51: return event.flags & descriptor_defs::kKeyboardFlagExtended
                              ? KEY_PGDN : '3';
        case 0x52: return event.flags & descriptor_defs::kKeyboardFlagExtended
                              ? KEY_INS : '0';
        case 0x53: return event.flags & descriptor_defs::kKeyboardFlagExtended
                              ? KEY_DEL : '.';
        case 0x57: return KEY_F11;
        case 0x58: return KEY_F12;
        default: break;
    }
    char ch = keyboard::scancode_to_char(event.scancode, event.mods);
    if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    return static_cast<unsigned char>(ch);
}

void handle_desktop_event(const desktop_protocol::MessageHeader& header) {
    if (desktop_protocol::valid(header,
                                desktop_protocol::MessageType::Keyboard,
                                sizeof(desktop_protocol::KeyboardMessage)) &&
        header.token == g_desktop_token) {
        const auto& message =
            reinterpret_cast<const desktop_protocol::KeyboardMessage&>(header);
        descriptor_defs::KeyboardEvent event{
            static_cast<uint8_t>(message.scancode),
            static_cast<uint8_t>(message.flags),
            static_cast<uint8_t>(message.mods), 0};
        queue_key((message.flags & descriptor_defs::kKeyboardFlagPressed) != 0,
                  doom_key(event));
    } else if (desktop_protocol::valid(
                   header, desktop_protocol::MessageType::Close,
                   sizeof(desktop_protocol::CloseMessage)) &&
               header.token == g_desktop_token) {
        exit(0);
    }
}

void poll_desktop_events() {
    long bytes = descriptor_read(
        g_desktop_events, g_event_bytes + g_event_used,
        sizeof(g_event_bytes) - g_event_used);
    if (bytes <= 0) return;
    g_event_used += static_cast<size_t>(bytes);
    size_t consumed = 0;
    while (g_event_used - consumed >= sizeof(desktop_protocol::MessageHeader)) {
        auto* header = reinterpret_cast<const desktop_protocol::MessageHeader*>(
            g_event_bytes + consumed);
        if (header->size < sizeof(*header) || header->size > sizeof(g_event_bytes)) {
            ++consumed;
            continue;
        }
        if (g_event_used - consumed < header->size) break;
        handle_desktop_event(*header);
        consumed += header->size;
    }
    if (consumed != 0) {
        memmove(g_event_bytes, g_event_bytes + consumed, g_event_used - consumed);
        g_event_used -= consumed;
    }
    if (g_event_used == sizeof(g_event_bytes)) g_event_used = 0;
}

void poll_keyboard() {
    if (g_desktop_mode) {
        poll_desktop_events();
        return;
    }
    descriptor_defs::KeyboardEvent events[16]{};
    long bytes = descriptor_read(g_keyboard, events, sizeof(events));
    if (bytes <= 0) return;
    size_t count = static_cast<size_t>(bytes) / sizeof(events[0]);
    for (size_t i = 0; i < count; ++i) {
        queue_key(
            (events[i].flags & descriptor_defs::kKeyboardFlagPressed) != 0,
            doom_key(events[i]));
    }
}

void cleanup() {
    if (g_desktop_mode && g_desktop_server != UINT32_MAX &&
        g_desktop_window != 0) {
        desktop_protocol::DestroyMessage message{
            desktop_protocol::header(desktop_protocol::MessageType::Destroy,
                                     sizeof(message), g_desktop_window,
                                     g_desktop_token)};
        (void)descriptor_write(g_desktop_server, &message, sizeof(message));
    }
    if (g_active && g_session != UINT32_MAX) {
        (void)graphical_session_set_active(g_session, false);
    }
    if (g_desktop_events != UINT32_MAX) descriptor_close(g_desktop_events);
    if (g_desktop_server != UINT32_MAX) descriptor_close(g_desktop_server);
    if (g_desktop_surface != UINT32_MAX) descriptor_close(g_desktop_surface);
    if (g_keyboard != UINT32_MAX) descriptor_close(g_keyboard);
    if (g_framebuffer != UINT32_MAX) descriptor_close(g_framebuffer);
    if (g_session != UINT32_MAX) descriptor_close(g_session);
    g_desktop_events = g_desktop_server = g_desktop_surface = UINT32_MAX;
    g_keyboard = g_framebuffer = g_session = UINT32_MAX;
    g_desktop_window = 0;
    g_desktop_token = 0;
    g_desktop_pixels = nullptr;
    g_desktop_mode = false;
    g_active = false;
}

[[noreturn]] void fail(const char* message) {
    fprintf(stderr, "doom: %s\n", message);
    exit(1);
}

int parse_arguments(const char* raw, char* storage, char** arguments) {
    arguments[0] = const_cast<char*>("doom");
    int count = 1;
    size_t used = 0;
    const char* cursor = raw;
    while (cursor != nullptr && *cursor != '\0' &&
           static_cast<size_t>(count) < kMaxArgs) {
        while (*cursor == ' ' || *cursor == '\t' ||
               *cursor == '\r' || *cursor == '\n') {
            ++cursor;
        }
        if (*cursor == '\0') break;
        char quote = 0;
        if (*cursor == '\'' || *cursor == '"') quote = *cursor++;
        arguments[count++] = storage + used;
        while (*cursor != '\0' &&
               (quote != 0 ? *cursor != quote
                           : (*cursor != ' ' && *cursor != '\t' &&
                              *cursor != '\r' && *cursor != '\n'))) {
            if (used + 1 >= kArgumentBytes) fail("arguments are too long");
            storage[used++] = *cursor++;
        }
        if (quote != 0 && *cursor == quote) ++cursor;
        storage[used++] = '\0';
    }
    return count;
}

bool connect_desktop() {
    g_desktop_connect_error = "opening registry";
    long registry_handle =
        shared_memory_open(desktop_protocol::kRegistryName,
                           sizeof(desktop_protocol::Registry));
    if (registry_handle < 0) return false;
    descriptor_defs::SharedMemoryInfo registry_info{};
    if (shared_memory_get_info(static_cast<uint32_t>(registry_handle),
                               &registry_info) != 0 ||
        registry_info.base == 0 ||
        registry_info.length < sizeof(desktop_protocol::Registry)) {
        descriptor_close(static_cast<uint32_t>(registry_handle));
        g_desktop_connect_error = "reading registry";
        return false;
    }
    const auto registry = *reinterpret_cast<const desktop_protocol::Registry*>(
        registry_info.base);
    descriptor_close(static_cast<uint32_t>(registry_handle));
    if (registry.magic != desktop_protocol::kRegistryMagic ||
        registry.version != desktop_protocol::kVersion ||
        registry.server_pipe_id == 0) {
        g_desktop_connect_error = "validating registry";
        return false;
    }

    uint64_t write_flags =
        static_cast<uint64_t>(descriptor_defs::Flag::Writable) |
        static_cast<uint64_t>(descriptor_defs::Flag::Async);
    long server = pipe_open_existing(write_flags, registry.server_pipe_id);
    if (server < 0) {
        g_desktop_connect_error = "opening server pipe";
        return false;
    }
    uint64_t read_flags =
        static_cast<uint64_t>(descriptor_defs::Flag::Readable) |
        static_cast<uint64_t>(descriptor_defs::Flag::Async);
    long events = pipe_open_new(read_flags);
    descriptor_defs::PipeInfo event_info{};
    long event_info_result = events < 0
                                 ? -1
                                 : pipe_get_info(static_cast<uint32_t>(events),
                                                 &event_info);
    if (events < 0 || event_info_result != 0 || event_info.id == 0) {
        if (events >= 0) descriptor_close(static_cast<uint32_t>(events));
        descriptor_close(static_cast<uint32_t>(server));
        g_desktop_connect_error = events < 0
                                      ? "opening reply pipe"
                                      : event_info_result != 0
                                            ? "reading reply pipe id"
                                            : "receiving an empty reply pipe id";
        return false;
    }

    uint64_t token = 0;
    if (random_get(&token, sizeof(token)) != static_cast<long>(sizeof(token)) ||
        token == 0) {
        descriptor_close(static_cast<uint32_t>(events));
        descriptor_close(static_cast<uint32_t>(server));
        g_desktop_connect_error = "generating token";
        return false;
    }
    char surface_name[48] = "neutrino.doom.";
    append_decimal(surface_name, sizeof(surface_name), process_id());
    size_t surface_name_length = strlen(surface_name);
    if (surface_name_length + 1 >= sizeof(surface_name)) {
        descriptor_close(static_cast<uint32_t>(events));
        descriptor_close(static_cast<uint32_t>(server));
        g_desktop_connect_error = "formatting surface name";
        return false;
    }
    surface_name[surface_name_length] = '.';
    surface_name[surface_name_length + 1] = '\0';
    append_decimal(surface_name, sizeof(surface_name), token);
    constexpr size_t kSurfaceBytes =
        static_cast<size_t>(DOOMGENERIC_RESX) * DOOMGENERIC_RESY * 4;
    long surface = shared_memory_open(surface_name, kSurfaceBytes);
    descriptor_defs::SharedMemoryInfo surface_info{};
    if (surface < 0 ||
        shared_memory_get_info(static_cast<uint32_t>(surface), &surface_info) != 0 ||
        surface_info.base == 0 || surface_info.length < kSurfaceBytes) {
        if (surface >= 0) descriptor_close(static_cast<uint32_t>(surface));
        descriptor_close(static_cast<uint32_t>(events));
        descriptor_close(static_cast<uint32_t>(server));
        g_desktop_connect_error = "mapping surface";
        return false;
    }
    memset(reinterpret_cast<void*>(surface_info.base), 0, kSurfaceBytes);

    desktop_protocol::CreateMessage request{};
    request.header =
        desktop_protocol::header(desktop_protocol::MessageType::Create,
                                 sizeof(request), 0, token);
    request.process_id = process_id();
    request.reply_pipe_id = event_info.id;
    request.width = DOOMGENERIC_RESX;
    request.height = DOOMGENERIC_RESY;
    request.pixel_format = desktop_protocol::kPixelFormatArgb8888;
    strlcpy(request.surface_name, surface_name, sizeof(request.surface_name));
    strlcpy(request.title, "Doom", sizeof(request.title));
    if (descriptor_write(static_cast<uint32_t>(server), &request,
                         sizeof(request)) != static_cast<long>(sizeof(request))) {
        descriptor_close(static_cast<uint32_t>(surface));
        descriptor_close(static_cast<uint32_t>(events));
        descriptor_close(static_cast<uint32_t>(server));
        g_desktop_connect_error = "sending create request";
        return false;
    }
    desktop_protocol::CreatedMessage response{};
    size_t response_bytes = 0;
    // A readable pipe with no current writer reports transient EOF. The
    // desktop cannot open its writer until it processes our create request,
    // so keep the read endpoint alive while the compositor event loop catches
    // up instead of incorrectly falling back to fullscreen.
    for (uint32_t waited_ms = 0;
         response_bytes < sizeof(response) && waited_ms < 2000;
         ++waited_ms) {
        long received = descriptor_read(
            static_cast<uint32_t>(events),
            reinterpret_cast<uint8_t*>(&response) + response_bytes,
            sizeof(response) - response_bytes);
        if (received > 0) {
            response_bytes += static_cast<size_t>(received);
        } else {
            (void)sleep_ms(1);
        }
    }
    if (response_bytes != sizeof(response) ||
        !desktop_protocol::valid(response.header,
                                 desktop_protocol::MessageType::Created,
                                 sizeof(response)) ||
        response.status != 0 || response.header.window_id == 0 ||
        response.header.token != token) {
        descriptor_close(static_cast<uint32_t>(surface));
        descriptor_close(static_cast<uint32_t>(events));
        descriptor_close(static_cast<uint32_t>(server));
        g_desktop_connect_error = response_bytes == sizeof(response)
                                      ? "create request rejected"
                                      : "waiting for create response";
        return false;
    }
    g_desktop_server = static_cast<uint32_t>(server);
    g_desktop_events = static_cast<uint32_t>(events);
    g_desktop_surface = static_cast<uint32_t>(surface);
    g_desktop_window = response.header.window_id;
    g_desktop_token = token;
    g_desktop_pixels = reinterpret_cast<uint32_t*>(surface_info.base);
    g_desktop_mode = true;
    g_desktop_connect_error = "none";
    return true;
}

bool connect_desktop_with_retry(uint32_t attempts, uint32_t delay_ms) {
    for (uint32_t attempt = 0; attempt < attempts; ++attempt) {
        if (connect_desktop()) return true;
        if (attempt + 1 < attempts) (void)sleep_ms(delay_ms);
    }
    return false;
}

void close_raw_graphics() {
    if (g_keyboard != UINT32_MAX) descriptor_close(g_keyboard);
    if (g_framebuffer != UINT32_MAX) descriptor_close(g_framebuffer);
    if (g_session != UINT32_MAX) descriptor_close(g_session);
    g_keyboard = g_framebuffer = g_session = UINT32_MAX;
    g_active = false;
}

}  // namespace

extern "C" void DG_Init() {
    atexit(cleanup);
    if (connect_desktop_with_retry(20, 50)) return;

    long session = graphical_session_open();
    if (session < 0) fail("graphical session unavailable");
    g_session = static_cast<uint32_t>(session);

    descriptor_defs::GraphicalSessionInfo session_info{};
    if (graphical_session_get_info(g_session, &session_info) != 0 ||
        session_info.abi_major != descriptor_defs::kGraphicalSessionAbiMajor) {
        fail("incompatible graphical session");
    }
    long framebuffer = framebuffer_open_slot(session_info.display_slot);
    if (framebuffer < 0) fail("framebuffer unavailable");
    g_framebuffer = static_cast<uint32_t>(framebuffer);
    if (framebuffer_get_info(g_framebuffer, &g_info) != 0 ||
        g_info.virtual_base == 0 || g_info.width < DOOMGENERIC_RESX ||
        g_info.height < DOOMGENERIC_RESY || g_info.pitch == 0) {
        fail("unsupported framebuffer geometry");
    }
    g_bytes_per_pixel = (g_info.bpp + 7u) / 8u;
    if (g_bytes_per_pixel < 2 || g_bytes_per_pixel > 4) {
        fail("unsupported framebuffer pixel format");
    }
    uint32_t scale_x = g_info.width / DOOMGENERIC_RESX;
    uint32_t scale_y = g_info.height / DOOMGENERIC_RESY;
    g_scale = scale_x < scale_y ? scale_x : scale_y;
    if (g_scale > 4) g_scale = 4;
    g_left = (g_info.width - DOOMGENERIC_RESX * g_scale) / 2;
    g_top = (g_info.height - DOOMGENERIC_RESY * g_scale) / 2;

    long keyboard = descriptor_open(kKeyboardType, 0);
    if (keyboard < 0) fail("keyboard unavailable");
    g_keyboard = static_cast<uint32_t>(keyboard);
    memset(reinterpret_cast<void*>(
               static_cast<uintptr_t>(g_info.virtual_base)),
           0, static_cast<size_t>(g_info.pitch) * g_info.height);
}

extern "C" void DG_DrawFrame() {
    if (g_desktop_mode) {
        memcpy(g_desktop_pixels, DG_ScreenBuffer,
               static_cast<size_t>(DOOMGENERIC_RESX) * DOOMGENERIC_RESY *
                   sizeof(uint32_t));
        desktop_protocol::DamageMessage damage{
            desktop_protocol::header(desktop_protocol::MessageType::Damage,
                                     sizeof(damage), g_desktop_window,
                                     g_desktop_token),
            0, 0, DOOMGENERIC_RESX, DOOMGENERIC_RESY};
        if (descriptor_write(g_desktop_server, &damage, sizeof(damage)) !=
            static_cast<long>(sizeof(damage))) {
            fail("desktop connection lost");
        }
        poll_keyboard();
        return;
    }
    auto* framebuffer = reinterpret_cast<uint8_t*>(
        static_cast<uintptr_t>(g_info.virtual_base));
    for (uint32_t y = 0; y < DOOMGENERIC_RESY; ++y) {
        for (uint32_t sy = 0; sy < g_scale; ++sy) {
            uint8_t* row = framebuffer +
                           static_cast<size_t>(g_top + y * g_scale + sy) *
                               g_info.pitch +
                           static_cast<size_t>(g_left) * g_bytes_per_pixel;
            for (uint32_t x = 0; x < DOOMGENERIC_RESX; ++x) {
                uint32_t pixel =
                    native_pixel(DG_ScreenBuffer[y * DOOMGENERIC_RESX + x]);
                for (uint32_t sx = 0; sx < g_scale; ++sx) {
                    store_pixel(row + static_cast<size_t>(
                                          x * g_scale + sx) *
                                          g_bytes_per_pixel,
                                pixel);
                }
            }
        }
    }
    descriptor_defs::FramebufferRect damage{
        g_left, g_top, DOOMGENERIC_RESX * g_scale,
        DOOMGENERIC_RESY * g_scale};
    if (!g_active) {
        // Activation copies the completed backing buffer to scanout. Delaying
        // it until the first frame keeps WAD/startup errors visible instead of
        // briefly replacing the console with an empty graphical session.
        if (graphical_session_set_active(g_session, true) != 0) {
            // The compositor may have been busy creating or repainting another
            // window during the startup handshake.  Never fight an active
            // desktop for fullscreen ownership; release the raw fallback and
            // make one final compositor connection attempt instead.
            close_raw_graphics();
            if (connect_desktop_with_retry(40, 50)) {
                memcpy(g_desktop_pixels, DG_ScreenBuffer,
                       static_cast<size_t>(DOOMGENERIC_RESX) *
                           DOOMGENERIC_RESY * sizeof(uint32_t));
                desktop_protocol::DamageMessage damage{
                    desktop_protocol::header(
                        desktop_protocol::MessageType::Damage,
                        sizeof(damage), g_desktop_window, g_desktop_token),
                    0, 0, DOOMGENERIC_RESX, DOOMGENERIC_RESY};
                if (descriptor_write(g_desktop_server, &damage,
                                     sizeof(damage)) !=
                    static_cast<long>(sizeof(damage))) {
                    fail("desktop connection lost");
                }
                poll_keyboard();
                return;
            }
            fail(g_desktop_connect_error);
        }
        g_active = true;
    } else if (framebuffer_present(g_framebuffer, &damage) != 0) {
        fail("unable to present framebuffer");
    }
    poll_keyboard();
}

extern "C" void DG_SleepMs(uint32_t milliseconds) {
    (void)sleep_ms(milliseconds);
}

extern "C" uint32_t DG_GetTicksMs() {
    NeutrinoWallTime now{};
    if (time_get(&now) != 0) return 0;
    return static_cast<uint32_t>(
        static_cast<uint64_t>(now.unix_seconds) * 1000ull +
        now.nanoseconds / 1000000ull);
}

extern "C" int DG_GetKey(int* pressed, unsigned char* key) {
    poll_keyboard();
    if (g_key_read == g_key_write) return 0;
    *pressed = g_key_queue[g_key_read].pressed;
    *key = g_key_queue[g_key_read].key;
    g_key_read = (g_key_read + 1) % kKeyQueueSize;
    return 1;
}

extern "C" void DG_SetWindowTitle(const char*) {}

int main(uint64_t argument_pointer, uint64_t) {
    char storage[kArgumentBytes]{};
    char* arguments[kMaxArgs]{};
    int count = parse_arguments(
        reinterpret_cast<const char*>(argument_pointer), storage, arguments);
    doomgeneric_Create(count, arguments);
    for (;;) doomgeneric_Tick();
}
