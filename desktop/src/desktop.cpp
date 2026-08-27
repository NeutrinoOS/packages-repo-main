#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../crt/syscall.hpp"
#include "../desktop_protocol.hpp"
#include "font8x8_basic.hpp"
#include "compositor.hpp"
#include "ui.hpp"

namespace {

constexpr uint32_t kKeyboardType =
    static_cast<uint32_t>(descriptor_defs::Type::Keyboard);
constexpr uint32_t kAudioOutputType =
    static_cast<uint32_t>(descriptor_defs::Type::AudioOutput);
constexpr uint32_t kTitleHeight = neutrino::ui::Metrics::title_height;
constexpr uint32_t kMaxWindows = 6;
constexpr uint32_t kMaxLaunchers = 10;
constexpr uint32_t kFallbackCursorWidth = 12;
constexpr uint32_t kFallbackCursorHeight = 18;
constexpr uint32_t kMaxCursorDimension = 64;
constexpr size_t kMaxCursorPixels =
    static_cast<size_t>(kMaxCursorDimension) * kMaxCursorDimension;
constexpr size_t kMaxCursorFileSize = 16 * 1024;
constexpr size_t kReceiveBufferSize = 2048;
constexpr size_t kMaxLauncherFileSize = 1024;
constexpr const char* kLauncherDirectory = "@sys/config/desktop/launchers";

using Rect = neutrino::ui::Rect;
using SceneDamage = neutrino::ui::DamageRegion<kMaxWindows * 2>;

struct Theme {
    uint32_t background = neutrino::ui::Palette::desktop_top;
    uint32_t background_low = neutrino::ui::Palette::desktop_bottom;
    uint32_t taskbar = 0x111a22;
    uint32_t panel = neutrino::ui::Palette::surface;
    uint32_t title = neutrino::ui::Palette::chrome;
    uint32_t title_inactive = neutrino::ui::Palette::chrome_inactive;
    uint32_t accent = neutrino::ui::Palette::accent;
    uint32_t taskbar_height = neutrino::ui::Metrics::taskbar_height;
    char cursor_bitmap[192] = "@sys/share/desktop/bitmaps/cur.bmp";
    uint32_t cursor_scale_low = 1;
    uint32_t cursor_scale_720p = 2;
    uint32_t cursor_scale_2k = 3;
    uint32_t cursor_scale_4k = 4;
};

struct Launcher {
    char label[32];
    char path[96];
    char args[160];
};

struct CursorImage {
    bool loaded;
    uint32_t width = kFallbackCursorWidth;
    uint32_t height = kFallbackCursorHeight;
    uint32_t scale = 1;
    uint32_t pixels[kMaxCursorPixels];
    uint8_t opaque[kMaxCursorPixels];
};

struct Window {
    bool used;
    uint32_t id;
    uint32_t process;
    uint32_t event_pipe;
    uint32_t surface_handle;
    uint64_t token;
    const uint32_t* pixels;
    uint32_t surface_width;
    uint32_t surface_height;
    uint32_t width;
    uint32_t height;
    int32_t x;
    int32_t y;
    char title[49];
};

descriptor_defs::FramebufferInfo g_fb{};
uint32_t g_framebuffer = kInvalidDescriptor;
uint8_t* g_pixels = nullptr;
uint32_t g_bytes_per_pixel = 0;
bool g_gpu_fill_available = true;
Theme g_theme{};
Launcher g_launchers[kMaxLaunchers]{};
size_t g_launcher_count = 0;
CursorImage g_cursor{};
neutrino::ui::WindowStack<Window, kMaxWindows> g_windows{};
uint32_t g_next_window_id = 1;
int32_t g_cursor_x = 0;
int32_t g_cursor_y = 0;
uint8_t g_previous_buttons = 0;
bool g_menu_open = false;
uint32_t g_volume = 100;
uint32_t g_last_nonzero_volume = 100;
int32_t g_drag_index = -1;
int32_t g_drag_offset_x = 0;
int32_t g_drag_offset_y = 0;
int32_t g_resize_index = -1;
Rect g_clip{};
uint8_t g_receive_buffer[kReceiveBufferSize]{};
size_t g_receive_used = 0;
uint8_t g_cursor_file[kMaxCursorFileSize]{};

constexpr int32_t kVolumeWidgetWidth = 136;
constexpr int32_t kVolumeSliderLeft = 49;
constexpr int32_t kVolumeSliderWidth = 76;

void copy_text(char* output, size_t capacity, const char* input) {
    if (output == nullptr || capacity == 0) return;
    size_t i = 0;
    if (input != nullptr) {
        while (input[i] != '\0' && i + 1 < capacity) {
            output[i] = input[i];
            ++i;
        }
    }
    output[i] = '\0';
}

void copy_fixed_text(char* output, size_t output_capacity,
                     const char* input, size_t input_capacity) {
    if (output == nullptr || output_capacity == 0) return;
    size_t i = 0;
    while (i < input_capacity && input[i] != '\0' &&
           i + 1 < output_capacity) {
        output[i] = input[i];
        ++i;
    }
    output[i] = '\0';
}

void decimal_text(char* output, size_t capacity, uint32_t value) {
    if (output == nullptr || capacity == 0) return;
    char reversed[11]{};
    size_t digits = 0;
    do {
        reversed[digits++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value != 0 && digits < sizeof(reversed));
    size_t written = 0;
    while (digits != 0 && written + 1 < capacity) {
        output[written++] = reversed[--digits];
    }
    output[written] = '\0';
}

bool equal_text(const char* left, const char* right) {
    if (left == nullptr || right == nullptr) return false;
    while (*left != '\0' && *right != '\0' && *left == *right) {
        ++left;
        ++right;
    }
    return *left == *right;
}

uint32_t hex_color(const char* text, uint32_t fallback) {
    uint32_t value = 0;
    for (size_t i = 0; i < 6; ++i) {
        char ch = text[i];
        uint32_t digit = 0;
        if (ch >= '0' && ch <= '9') digit = static_cast<uint32_t>(ch - '0');
        else if (ch >= 'a' && ch <= 'f') digit = static_cast<uint32_t>(ch - 'a' + 10);
        else if (ch >= 'A' && ch <= 'F') digit = static_cast<uint32_t>(ch - 'A' + 10);
        else return fallback;
        value = (value << 4) | digit;
    }
    return text[6] == '\0' ? value : fallback;
}

uint32_t blend_color(uint32_t top, uint32_t bottom, uint32_t amount,
                     uint32_t range) {
    uint32_t inverse = range - amount;
    uint32_t red = (((top >> 16) & 0xffu) * inverse +
                    ((bottom >> 16) & 0xffu) * amount) / range;
    uint32_t green = (((top >> 8) & 0xffu) * inverse +
                      ((bottom >> 8) & 0xffu) * amount) / range;
    uint32_t blue = ((top & 0xffu) * inverse + (bottom & 0xffu) * amount) /
                    range;
    return (red << 16) | (green << 8) | blue;
}

uint32_t decimal(const char* text, uint32_t fallback) {
    uint32_t value = 0;
    bool have_digit = false;
    while (*text >= '0' && *text <= '9') {
        have_digit = true;
        value = value * 10 + static_cast<uint32_t>(*text++ - '0');
    }
    return have_digit && *text == '\0' ? value : fallback;
}

void apply_setting(const char* key, const char* value) {
    if (equal_text(key, "background")) g_theme.background = hex_color(value, g_theme.background);
    else if (equal_text(key, "background_low")) g_theme.background_low = hex_color(value, g_theme.background_low);
    else if (equal_text(key, "taskbar")) g_theme.taskbar = hex_color(value, g_theme.taskbar);
    else if (equal_text(key, "panel")) g_theme.panel = hex_color(value, g_theme.panel);
    else if (equal_text(key, "title")) g_theme.title = hex_color(value, g_theme.title);
    else if (equal_text(key, "title_inactive")) g_theme.title_inactive = hex_color(value, g_theme.title_inactive);
    else if (equal_text(key, "accent")) g_theme.accent = hex_color(value, g_theme.accent);
    else if (equal_text(key, "taskbar_height")) {
        uint32_t height = decimal(value, g_theme.taskbar_height);
        if (height >= 30 && height <= 80) g_theme.taskbar_height = height;
    } else if (equal_text(key, "cursor_bitmap")) {
        copy_text(g_theme.cursor_bitmap, sizeof(g_theme.cursor_bitmap), value);
    } else if (equal_text(key, "cursor_scale_low")) {
        uint32_t scale = decimal(value, g_theme.cursor_scale_low);
        if (scale >= 1 && scale <= 8) g_theme.cursor_scale_low = scale;
    } else if (equal_text(key, "cursor_scale_720p")) {
        uint32_t scale = decimal(value, g_theme.cursor_scale_720p);
        if (scale >= 1 && scale <= 8) g_theme.cursor_scale_720p = scale;
    } else if (equal_text(key, "cursor_scale_2k")) {
        uint32_t scale = decimal(value, g_theme.cursor_scale_2k);
        if (scale >= 1 && scale <= 8) g_theme.cursor_scale_2k = scale;
    } else if (equal_text(key, "cursor_scale_4k")) {
        uint32_t scale = decimal(value, g_theme.cursor_scale_4k);
        if (scale >= 1 && scale <= 8) g_theme.cursor_scale_4k = scale;
    }
}

void load_config() {
    constexpr const char* kKeys[] = {
        "desktop.background", "desktop.background_low", "desktop.taskbar",
        "desktop.panel", "desktop.title", "desktop.title_inactive",
        "desktop.accent", "desktop.taskbar_height", "desktop.cursor_bitmap",
        "desktop.cursor_scale_low", "desktop.cursor_scale_720p",
        "desktop.cursor_scale_2k", "desktop.cursor_scale_4k",
    };
    for (const char* key : kKeys) {
        char value[256]{};
        if (settings_get(key, value, sizeof(value)) <= 0) continue;
        apply_setting(key + sizeof("desktop.") - 1, value);
    }
}

void apply_launcher_setting(Launcher& launcher, const char* key,
                            const char* value) {
    if (equal_text(key, "label")) {
        copy_text(launcher.label, sizeof(launcher.label), value);
    } else if (equal_text(key, "path")) {
        copy_text(launcher.path, sizeof(launcher.path), value);
    } else if (equal_text(key, "args")) {
        copy_text(launcher.args, sizeof(launcher.args), value);
    }
}

bool load_launcher(uint32_t directory, const char* name, Launcher& launcher) {
    long file = file_open_at(directory, name);
    if (file < 0) return false;
    char bytes[kMaxLauncherFileSize + 1]{};
    long count = file_read(static_cast<uint32_t>(file), bytes,
                           kMaxLauncherFileSize);
    file_close(static_cast<uint32_t>(file));
    if (count <= 0) return false;
    bytes[count] = '\0';
    char* line = bytes;
    for (long i = 0; i <= count; ++i) {
        if (bytes[i] != '\n' && bytes[i] != '\r' && bytes[i] != '\0') continue;
        bytes[i] = '\0';
        if (line[0] != '\0' && line[0] != '#') {
            char* separator = line;
            while (*separator != '\0' && *separator != '=') ++separator;
            if (*separator == '=') {
                *separator = '\0';
                apply_launcher_setting(launcher, line, separator + 1);
            }
        }
        line = bytes + i + 1;
    }
    return launcher.label[0] != '\0' && launcher.path[0] != '\0';
}

void load_launchers() {
    g_launcher_count = 0;
    long directory = directory_open(kLauncherDirectory);
    if (directory < 0) return;
    DirEntry entry{};
    while (g_launcher_count < kMaxLaunchers &&
           directory_read(static_cast<uint32_t>(directory), &entry) > 0) {
        if ((entry.flags & 1u) != 0 || entry.name[0] == '\0') continue;
        Launcher launcher{};
        if (load_launcher(static_cast<uint32_t>(directory), entry.name, launcher)) {
            g_launchers[g_launcher_count++] = launcher;
        }
    }
    directory_close(static_cast<uint32_t>(directory));
}

bool set_system_volume(uint32_t volume) {
    if (volume > 100) volume = 100;
    long audio = descriptor_open(kAudioOutputType, 0);
    if (audio < 0) return false;
    descriptor_defs::AudioControlInfo control{
        descriptor_defs::kAudioCommandSetVolume,
        static_cast<int32_t>(volume)};
    bool changed = descriptor_set_property(
                       static_cast<uint32_t>(audio),
                       static_cast<uint32_t>(descriptor_defs::Property::AudioControl),
                       &control, sizeof(control)) == 0;
    descriptor_close(static_cast<uint32_t>(audio));
    if (!changed) return false;
    g_volume = volume;
    if (volume != 0) g_last_nonzero_volume = volume;
    return true;
}

void adjust_system_volume(int32_t delta) {
    int32_t volume = static_cast<int32_t>(g_volume) + delta;
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    (void)set_system_volume(static_cast<uint32_t>(volume));
}

void toggle_system_mute() {
    if (g_volume != 0) {
        (void)set_system_volume(0);
    } else {
        (void)set_system_volume(g_last_nonzero_volume);
    }
}

bool handle_volume_key(const descriptor_defs::KeyboardEvent& event) {
    if ((event.flags & descriptor_defs::kKeyboardFlagPressed) == 0) return false;
    const bool extended =
        (event.flags & descriptor_defs::kKeyboardFlagExtended) != 0;
    if (extended && event.scancode == 0x20) {
        toggle_system_mute();
        return true;
    }
    if (extended && event.scancode == 0x2e) {
        adjust_system_volume(-5);
        return true;
    }
    if (extended && event.scancode == 0x30) {
        adjust_system_volume(5);
        return true;
    }
    return false;
}

uint16_t little_u16(const uint8_t* bytes) {
    return static_cast<uint16_t>(bytes[0]) |
           static_cast<uint16_t>(static_cast<uint16_t>(bytes[1]) << 8);
}

uint32_t little_u32(const uint8_t* bytes) {
    return static_cast<uint32_t>(bytes[0]) |
           (static_cast<uint32_t>(bytes[1]) << 8) |
           (static_cast<uint32_t>(bytes[2]) << 16) |
           (static_cast<uint32_t>(bytes[3]) << 24);
}

uint8_t expand_5_bit(uint16_t value) {
    return static_cast<uint8_t>((static_cast<uint32_t>(value) * 255u + 15u) /
                                31u);
}

uint32_t cursor_scale_for_resolution() {
    if (g_fb.height >= 2160) return g_theme.cursor_scale_4k;
    if (g_fb.height >= 1440) return g_theme.cursor_scale_2k;
    if (g_fb.height >= 720) return g_theme.cursor_scale_720p;
    return g_theme.cursor_scale_low;
}

bool load_cursor_bitmap() {
    g_cursor = {};
    g_cursor.scale = cursor_scale_for_resolution();
    if (g_theme.cursor_bitmap[0] == '\0') return false;

    long file = file_open(g_theme.cursor_bitmap);
    if (file < 0) return false;
    size_t size = 0;
    while (size < sizeof(g_cursor_file)) {
        long count = file_read(static_cast<uint32_t>(file),
                               g_cursor_file + size,
                               sizeof(g_cursor_file) - size);
        if (count < 0) {
            file_close(static_cast<uint32_t>(file));
            return false;
        }
        if (count == 0) break;
        size += static_cast<size_t>(count);
    }
    file_close(static_cast<uint32_t>(file));

    constexpr size_t kMaskEnd = 70;
    if (size < kMaskEnd || g_cursor_file[0] != 'B' ||
        g_cursor_file[1] != 'M') return false;

    uint32_t declared_size = little_u32(g_cursor_file + 2);
    uint32_t pixel_offset = little_u32(g_cursor_file + 10);
    uint32_t dib_size = little_u32(g_cursor_file + 14);
    if (declared_size > size || declared_size < kMaskEnd || dib_size < 56 ||
        dib_size > size - 14 || pixel_offset < 14 + dib_size ||
        pixel_offset > declared_size) return false;

    int32_t signed_width = static_cast<int32_t>(little_u32(g_cursor_file + 18));
    int32_t signed_height = static_cast<int32_t>(little_u32(g_cursor_file + 22));
    if (signed_width <= 0 || signed_width > static_cast<int32_t>(kMaxCursorDimension) ||
        signed_height == 0 ||
        signed_height > static_cast<int32_t>(kMaxCursorDimension) ||
        signed_height < -static_cast<int32_t>(kMaxCursorDimension) ||
        little_u16(g_cursor_file + 26) != 1 ||
        little_u16(g_cursor_file + 28) != 16 ||
        little_u32(g_cursor_file + 30) != 3 ||
        little_u32(g_cursor_file + 54) != 0x7c00 ||
        little_u32(g_cursor_file + 58) != 0x03e0 ||
        little_u32(g_cursor_file + 62) != 0x001f ||
        little_u32(g_cursor_file + 66) != 0x8000) return false;

    uint32_t width = static_cast<uint32_t>(signed_width);
    uint32_t height = signed_height < 0
                          ? static_cast<uint32_t>(-signed_height)
                          : static_cast<uint32_t>(signed_height);
    size_t row_bytes = (static_cast<size_t>(width) * 2u + 3u) & ~size_t{3};
    size_t pixel_bytes = row_bytes * height;
    if (pixel_bytes > declared_size - pixel_offset) return false;

    for (uint32_t y = 0; y < height; ++y) {
        uint32_t source_y = signed_height < 0 ? y : height - 1u - y;
        const uint8_t* row = g_cursor_file + pixel_offset + source_y * row_bytes;
        for (uint32_t x = 0; x < width; ++x) {
            uint16_t value = little_u16(row + static_cast<size_t>(x) * 2u);
            size_t index = static_cast<size_t>(y) * width + x;
            g_cursor.opaque[index] = (value & 0x8000u) != 0;
            uint8_t red = expand_5_bit(static_cast<uint16_t>((value >> 10) & 0x1fu));
            uint8_t green = expand_5_bit(static_cast<uint16_t>((value >> 5) & 0x1fu));
            uint8_t blue = expand_5_bit(static_cast<uint16_t>(value & 0x1fu));
            g_cursor.pixels[index] = (static_cast<uint32_t>(red) << 16) |
                                     (static_cast<uint32_t>(green) << 8) | blue;
        }
    }
    g_cursor.loaded = true;
    g_cursor.width = width;
    g_cursor.height = height;
    return true;
}

int32_t cursor_width() {
    return static_cast<int32_t>(g_cursor.width * g_cursor.scale);
}

int32_t cursor_height() {
    return static_cast<int32_t>(g_cursor.height * g_cursor.scale);
}

Rect cursor_rect() {
    return {g_cursor_x, g_cursor_y, cursor_width(), cursor_height()};
}

uint32_t scale_channel(uint8_t value, uint8_t bits) {
    if (bits == 0) return 0;
    uint32_t maximum = bits >= 32 ? UINT32_MAX : ((1u << bits) - 1u);
    return static_cast<uint32_t>(
        (static_cast<uint64_t>(value) * maximum + 127u) / 255u);
}

uint32_t native_color(uint32_t rgb) {
    uint8_t red = static_cast<uint8_t>(rgb >> 16);
    uint8_t green = static_cast<uint8_t>(rgb >> 8);
    uint8_t blue = static_cast<uint8_t>(rgb);
    return (scale_channel(red, g_fb.red_mask_size) << g_fb.red_mask_shift) |
           (scale_channel(green, g_fb.green_mask_size) << g_fb.green_mask_shift) |
           (scale_channel(blue, g_fb.blue_mask_size) << g_fb.blue_mask_shift);
}

bool intersects(const Rect& left, const Rect& right) {
    return left.x < right.x + right.width && right.x < left.x + left.width &&
           left.y < right.y + right.height && right.y < left.y + left.height;
}

Rect clipped(Rect rect) {
    if (rect.x < 0) {
        rect.width += rect.x;
        rect.x = 0;
    }
    if (rect.y < 0) {
        rect.height += rect.y;
        rect.y = 0;
    }
    int32_t width = static_cast<int32_t>(g_fb.width);
    int32_t height = static_cast<int32_t>(g_fb.height);
    if (rect.x + rect.width > width) rect.width = width - rect.x;
    if (rect.y + rect.height > height) rect.height = height - rect.y;
    if (rect.width < 0) rect.width = 0;
    if (rect.height < 0) rect.height = 0;
    return rect;
}

void put_pixel(int32_t x, int32_t y, uint32_t value) {
    if (x < g_clip.x || y < g_clip.y ||
        x >= g_clip.x + g_clip.width || y >= g_clip.y + g_clip.height) return;
    uint8_t* pixel = g_pixels + static_cast<size_t>(y) * g_fb.pitch +
                     static_cast<size_t>(x) * g_bytes_per_pixel;
    for (uint32_t i = 0; i < g_bytes_per_pixel; ++i) {
        pixel[i] = static_cast<uint8_t>(value >> (i * 8u));
    }
}

void fill_rect(int32_t x, int32_t y, int32_t width, int32_t height, uint32_t rgb) {
    Rect rect = clipped({x, y, width, height});
    if (!intersects(rect, g_clip)) return;
    int32_t start_x = rect.x > g_clip.x ? rect.x : g_clip.x;
    int32_t start_y = rect.y > g_clip.y ? rect.y : g_clip.y;
    int32_t end_x = rect.x + rect.width < g_clip.x + g_clip.width
                        ? rect.x + rect.width : g_clip.x + g_clip.width;
    int32_t end_y = rect.y + rect.height < g_clip.y + g_clip.height
                        ? rect.y + rect.height : g_clip.y + g_clip.height;
    uint32_t value = native_color(rgb);
    const uint64_t pixels = static_cast<uint64_t>(end_x - start_x) *
                            static_cast<uint64_t>(end_y - start_y);
    if (g_gpu_fill_available && g_bytes_per_pixel == sizeof(uint32_t) &&
        pixels >= 1024) {
        const descriptor_defs::FramebufferFill fill{
            static_cast<uint32_t>(start_x), static_cast<uint32_t>(start_y),
            static_cast<uint32_t>(end_x - start_x),
            static_cast<uint32_t>(end_y - start_y), value};
        if (framebuffer_fill(g_framebuffer, &fill) == 0) return;
        // Unsupported systems retain the software renderer without paying for
        // a failed descriptor operation on every future rectangle.
        g_gpu_fill_available = false;
    }
    if (g_bytes_per_pixel == sizeof(uint32_t)) {
        for (int32_t row = start_y; row < end_y; ++row) {
            auto* pixels = reinterpret_cast<uint32_t*>(
                g_pixels + static_cast<size_t>(row) * g_fb.pitch) + start_x;
            for (int32_t column = start_x; column < end_x; ++column) {
                *pixels++ = value;
            }
        }
        return;
    }
    for (int32_t row = start_y; row < end_y; ++row) {
        for (int32_t column = start_x; column < end_x; ++column) {
            uint8_t* pixel = g_pixels + static_cast<size_t>(row) * g_fb.pitch +
                             static_cast<size_t>(column) * g_bytes_per_pixel;
            for (uint32_t byte = 0; byte < g_bytes_per_pixel; ++byte) {
                pixel[byte] = static_cast<uint8_t>(value >> (byte * 8u));
            }
        }
    }
}

void draw_char(int32_t x, int32_t y, char character, uint32_t foreground,
               uint32_t scale = 1) {
    uint8_t code = static_cast<uint8_t>(character);
    if (code >= 128 || scale == 0) return;
    for (int32_t row = 0; row < 8; ++row) {
        uint8_t glyph_row = font8x8_basic[code][row];
        for (int32_t column = 0; column < 8; ++column) {
            if ((glyph_row & (1u << column)) != 0) {
                fill_rect(x + column * static_cast<int32_t>(scale),
                          y + row * static_cast<int32_t>(scale),
                          static_cast<int32_t>(scale),
                          static_cast<int32_t>(scale), foreground);
            }
        }
    }
}

void draw_text(int32_t x, int32_t y, const char* text, uint32_t foreground,
               uint32_t scale = 1) {
    if (text == nullptr) return;
    while (*text != '\0') {
        draw_char(x, y, *text++, foreground, scale);
        x += static_cast<int32_t>(8u * scale);
    }
}

void draw_frame(int32_t x, int32_t y, int32_t width, int32_t height,
                uint32_t value) {
    fill_rect(x, y, width, 1, value);
    fill_rect(x, y + height - 1, width, 1, value);
    fill_rect(x, y, 1, height, value);
    fill_rect(x + width - 1, y, 1, height, value);
}

Rect window_rect(const Window& window) {
    return neutrino::ui::window_chrome(
        window.x, window.y, static_cast<int32_t>(window.width),
        static_cast<int32_t>(window.height)).outer;
}

Rect window_damage_rect(const Window& window) {
    return neutrino::ui::window_chrome(
        window.x, window.y, static_cast<int32_t>(window.width),
        static_cast<int32_t>(window.height)).damage;
}

void draw_window(const Window& window, bool focused) {
    int32_t width = static_cast<int32_t>(window.width);
    int32_t height = static_cast<int32_t>(window.height);
    auto chrome = neutrino::ui::window_chrome(window.x, window.y, width, height);
    // Chrome belongs exclusively to the compositor.  Every client therefore
    // receives identical focus treatment regardless of its rendering cadence.
    fill_rect(chrome.shadow.x, chrome.shadow.y, chrome.shadow.width,
              chrome.shadow.height, neutrino::ui::Palette::shadow);
    fill_rect(window.x + 5, window.y + 6, width,
              height + kTitleHeight, 0x071016);
    fill_rect(chrome.titlebar.x, chrome.titlebar.y, chrome.titlebar.width,
              chrome.titlebar.height,
              focused ? g_theme.title : g_theme.title_inactive);
    if (focused) fill_rect(window.x, window.y, width, 3, g_theme.accent);
    draw_frame(window.x, window.y, width, height + kTitleHeight,
               neutrino::ui::Palette::outline);
    draw_text(window.x + 10, window.y + 11, window.title,
              focused ? neutrino::ui::Palette::chrome_text : 0xc4cdd2);
    fill_rect(chrome.close_button.x, chrome.close_button.y,
              chrome.close_button.width, chrome.close_button.height,
              focused ? neutrino::ui::Palette::danger : 0x53616a);
    draw_frame(chrome.close_button.x, chrome.close_button.y,
               chrome.close_button.width, chrome.close_button.height,
               neutrino::ui::Palette::outline);
    draw_text(window.x + width - 21, window.y + 11, "x",
              neutrino::ui::Palette::chrome_text);

    Rect content = chrome.content;
    if (!intersects(content, g_clip)) return;
    int32_t start_x = content.x > g_clip.x ? content.x : g_clip.x;
    int32_t start_y = content.y > g_clip.y ? content.y : g_clip.y;
    int32_t end_x = content.x + content.width < g_clip.x + g_clip.width
                        ? content.x + content.width : g_clip.x + g_clip.width;
    int32_t end_y = content.y + content.height < g_clip.y + g_clip.height
                        ? content.y + content.height : g_clip.y + g_clip.height;
    bool direct_copy = g_bytes_per_pixel == sizeof(uint32_t) &&
                       g_fb.red_mask_size == 8 && g_fb.red_mask_shift == 16 &&
                       g_fb.green_mask_size == 8 && g_fb.green_mask_shift == 8 &&
                       g_fb.blue_mask_size == 8 && g_fb.blue_mask_shift == 0 &&
                       window.surface_width == window.width &&
                       window.surface_height == window.height;
    if (direct_copy) {
        size_t row_bytes = static_cast<size_t>(end_x - start_x) *
                           sizeof(uint32_t);
        size_t source_x = static_cast<size_t>(start_x - content.x);
        for (int32_t y = start_y; y < end_y; ++y) {
            size_t source_y = static_cast<size_t>(y - content.y);
            const uint32_t* source = window.pixels +
                                     source_y * window.surface_width +
                                     source_x;
            uint8_t* destination = g_pixels +
                                   static_cast<size_t>(y) * g_fb.pitch +
                                   static_cast<size_t>(start_x) *
                                       sizeof(uint32_t);
            memcpy(destination, source, row_bytes);
        }
    } else {
        for (int32_t y = start_y; y < end_y; ++y) {
            uint32_t source_y =
                static_cast<uint32_t>(y - content.y) * window.surface_height /
                window.height;
            for (int32_t x = start_x; x < end_x; ++x) {
                uint32_t source_x =
                    static_cast<uint32_t>(x - content.x) * window.surface_width /
                    window.width;
                put_pixel(x, y, native_color(
                    window.pixels[source_y * window.surface_width + source_x]));
            }
        }
    }
    for (int32_t inset = 3; inset <= 9; inset += 3) {
        fill_rect(window.x + width - inset - 1,
                  window.y + static_cast<int32_t>(kTitleHeight) + height - 2,
                  inset, 1, 0x65757f);
        fill_rect(window.x + width - 2,
                  window.y + static_cast<int32_t>(kTitleHeight) + height -
                      inset - 1,
                  1, inset, 0x65757f);
    }
}

bool cursor_pixel(int32_t x, int32_t y) {
    if (x == 0 && y < 15) return true;
    if ((y == x * 2 || y == x * 2 + 1) && y < 16) return true;
    if (y >= 10 && y <= 16 && x >= 3 && x <= 5) return true;
    return y >= 14 && y <= 17 && x >= 6 && x <= 8;
}

void draw_cursor() {
    if (g_cursor.loaded) {
        for (uint32_t y = 0; y < g_cursor.height; ++y) {
            for (uint32_t x = 0; x < g_cursor.width; ++x) {
                size_t index = static_cast<size_t>(y) * g_cursor.width + x;
                if (g_cursor.opaque[index] == 0) continue;
                fill_rect(g_cursor_x + static_cast<int32_t>(x * g_cursor.scale),
                          g_cursor_y + static_cast<int32_t>(y * g_cursor.scale),
                          static_cast<int32_t>(g_cursor.scale),
                          static_cast<int32_t>(g_cursor.scale),
                          g_cursor.pixels[index]);
            }
        }
        return;
    }
    for (int32_t y = 0; y < static_cast<int32_t>(kFallbackCursorHeight); ++y) {
        for (int32_t x = 0; x < static_cast<int32_t>(kFallbackCursorWidth); ++x) {
            if (!cursor_pixel(x, y)) continue;
            bool edge = x == 0 || y == x * 2 || y == x * 2 + 1 ||
                        y == static_cast<int32_t>(kFallbackCursorHeight) - 1;
            fill_rect(g_cursor_x + x * static_cast<int32_t>(g_cursor.scale),
                      g_cursor_y + y * static_cast<int32_t>(g_cursor.scale),
                      static_cast<int32_t>(g_cursor.scale),
                      static_cast<int32_t>(g_cursor.scale),
                      edge ? 0x000000 : 0xffffff);
        }
    }
}

int32_t start_menu_height() {
    if (g_launcher_count == 0) return 88;
    return 51 + static_cast<int32_t>(g_launcher_count) * 37;
}

int32_t start_menu_y(int32_t taskbar_y) {
    int32_t menu_y = taskbar_y - start_menu_height();
    return menu_y < 0 ? 0 : menu_y;
}

int32_t volume_widget_x() {
    return static_cast<int32_t>(g_fb.width) - kVolumeWidgetWidth - 8;
}

void render(Rect damage) {
    damage = clipped(damage);
    if (damage.width <= 0 || damage.height <= 0) return;
    g_clip = damage;
    int32_t desktop_height = static_cast<int32_t>(
        g_fb.height - g_theme.taskbar_height);
    // A small fixed number of strips is visually smoother than flat slabs,
    // while retaining clipped rectangle fills instead of a per-pixel blend.
    constexpr int32_t kBackgroundBands = 12;
    for (int32_t band = 0; band < kBackgroundBands; ++band) {
        int32_t top = band * desktop_height / kBackgroundBands;
        int32_t bottom = (band + 1) * desktop_height / kBackgroundBands;
        fill_rect(0, top, static_cast<int32_t>(g_fb.width), bottom - top,
                  blend_color(g_theme.background, g_theme.background_low,
                              static_cast<uint32_t>(band),
                              kBackgroundBands - 1));
    }

    int32_t taskbar_y =
        static_cast<int32_t>(g_fb.height - g_theme.taskbar_height);
    fill_rect(0, taskbar_y, static_cast<int32_t>(g_fb.width),
              static_cast<int32_t>(g_theme.taskbar_height), g_theme.taskbar);
    fill_rect(0, taskbar_y, static_cast<int32_t>(g_fb.width), 1, g_theme.accent);
    fill_rect(8, taskbar_y + 8, 92,
              static_cast<int32_t>(g_theme.taskbar_height) - 16, g_theme.accent);
    draw_frame(8, taskbar_y + 8, 92,
               static_cast<int32_t>(g_theme.taskbar_height) - 16,
               neutrino::ui::Palette::outline);
    draw_text(18, taskbar_y + 20, "NEUTRINO",
              neutrino::ui::Palette::ink);
    const int32_t volume_x = volume_widget_x();
    fill_rect(volume_x, taskbar_y + 8, kVolumeWidgetWidth,
              static_cast<int32_t>(g_theme.taskbar_height) - 16, 0x2b3943);
    draw_frame(volume_x, taskbar_y + 8, kVolumeWidgetWidth,
               static_cast<int32_t>(g_theme.taskbar_height) - 16, 0x52616b);
    draw_text(volume_x + 8, taskbar_y + 20, "VOL",
              neutrino::ui::Palette::chrome_text);
    fill_rect(volume_x + kVolumeSliderLeft, taskbar_y + 18,
              kVolumeSliderWidth, 8, 0x16232b);
    const int32_t filled = static_cast<int32_t>(
        g_volume * static_cast<uint32_t>(kVolumeSliderWidth) / 100u);
    if (filled != 0) {
        fill_rect(volume_x + kVolumeSliderLeft, taskbar_y + 18, filled, 8,
                  g_theme.accent);
    }
    char volume_text[5]{};
    decimal_text(volume_text, sizeof(volume_text), g_volume);
    draw_text(volume_x + 101, taskbar_y + 20, volume_text,
              neutrino::ui::Palette::chrome_text);
    for (size_t i = 0; i < g_windows.size(); ++i) {
        draw_window(g_windows[i], i + 1 == g_windows.size());
    }
    if (g_menu_open) {
        int32_t menu_y = start_menu_y(taskbar_y);
        int32_t menu_height = start_menu_height();
        fill_rect(11, menu_y + 4, 220, menu_height,
                  neutrino::ui::Palette::shadow);
        fill_rect(8, menu_y, 220, menu_height, g_theme.panel);
        draw_frame(8, menu_y, 220, menu_height, neutrino::ui::Palette::outline);
        fill_rect(9, menu_y + 1, 218, 30, g_theme.title);
        fill_rect(9, menu_y + 1, 218, 3, g_theme.accent);
        draw_text(20, menu_y + 12, "Applications",
                  neutrino::ui::Palette::chrome_text);
        if (g_launcher_count == 0) {
            draw_text(28, menu_y + 51, "No applications configured",
                      neutrino::ui::Palette::ink);
        }
        for (size_t i = 0; i < g_launcher_count; ++i) {
            int32_t item_y = menu_y + 40 + static_cast<int32_t>(i) * 37;
            fill_rect(17, item_y, 202, 37, 0xdfe8ed);
            draw_frame(17, item_y, 202, 37, 0xaab8c1);
            draw_text(28, item_y + 15, g_launchers[i].label,
                      neutrino::ui::Palette::ink);
        }
    }
    draw_cursor();
    descriptor_defs::FramebufferRect present{
        static_cast<uint32_t>(damage.x), static_cast<uint32_t>(damage.y),
        static_cast<uint32_t>(damage.width), static_cast<uint32_t>(damage.height)};
    (void)framebuffer_present(g_framebuffer, &present);
}

void render_damage(const SceneDamage& damage) {
    for (size_t i = 0; i < damage.size(); ++i) render(damage[i]);
}

int32_t window_at(int32_t x, int32_t y) {
    for (int32_t i = static_cast<int32_t>(g_windows.size()) - 1; i >= 0; --i) {
        Rect rect = window_rect(g_windows[i]);
        if (x >= rect.x && y >= rect.y &&
            x < rect.x + rect.width && y < rect.y + rect.height) return i;
    }
    return -1;
}

void send_close(Window& window) {
    desktop_protocol::CloseMessage message{
        desktop_protocol::header(desktop_protocol::MessageType::Close,
                                 sizeof(message), window.id, window.token)};
    (void)descriptor_write(window.event_pipe, &message, sizeof(message));
}

void remove_window(uint32_t index) {
    if (index >= g_windows.size()) return;
    descriptor_close(g_windows[index].event_pipe);
    descriptor_close(g_windows[index].surface_handle);
    SceneDamage damage;
    (void)g_windows.erase(index, window_damage_rect, damage);
    render_damage(damage);
}

void launch_app(size_t index) {
    if (index >= g_launcher_count) return;
    const Launcher& launcher = g_launchers[index];
    const char* args = launcher.args[0] == '\0' ? nullptr : launcher.args;
    (void)child(launcher.path, args, 0, "/");
}

bool valid_name(const char* name, size_t capacity) {
    bool terminated = false;
    for (size_t i = 0; i < capacity; ++i) {
        char ch = name[i];
        if (ch == '\0') {
            terminated = i != 0;
            break;
        }
        bool safe = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                    (ch >= '0' && ch <= '9') || ch == '.' || ch == '-' ||
                    ch == '_';
        if (!safe) return false;
    }
    return terminated;
}

void handle_create(const desktop_protocol::CreateMessage& message) {
    desktop_protocol::CreatedMessage response{
        desktop_protocol::header(desktop_protocol::MessageType::Created,
                                 sizeof(response)), -1, 0};
    uint64_t writable = static_cast<uint64_t>(descriptor_defs::Flag::Writable) |
                        static_cast<uint64_t>(descriptor_defs::Flag::Async);
    long reply = pipe_open_existing(writable, message.reply_pipe_id);
    if (reply < 0) return;
    if (!desktop_protocol::valid(message.header,
                                 desktop_protocol::MessageType::Create,
                                 sizeof(message)) ||
        message.process_id == 0 ||
        message.header.token == 0 ||
        message.width < 64 || message.height < 64 ||
        message.width > desktop_protocol::kMaxSurfaceWidth ||
        message.height > desktop_protocol::kMaxSurfaceHeight ||
        message.pixel_format != desktop_protocol::kPixelFormatArgb8888 ||
        !valid_name(message.surface_name, sizeof(message.surface_name))) {
        descriptor_write(static_cast<uint32_t>(reply), &response, sizeof(response));
        descriptor_close(static_cast<uint32_t>(reply));
        return;
    }
    size_t surface_bytes = static_cast<size_t>(message.width) * message.height * 4;
    long surface = !g_windows.full()
                       ? shared_memory_open(message.surface_name, surface_bytes) : -1;
    descriptor_defs::SharedMemoryInfo info{};
    if (surface < 0 ||
        shared_memory_get_info(static_cast<uint32_t>(surface), &info) != 0 ||
        info.base == 0 || info.length < surface_bytes) {
        if (surface >= 0) descriptor_close(static_cast<uint32_t>(surface));
        descriptor_write(static_cast<uint32_t>(reply), &response, sizeof(response));
        descriptor_close(static_cast<uint32_t>(reply));
        return;
    }
    Window window{};
    window.used = true;
    window.id = g_next_window_id++;
    if (window.id == 0) window.id = g_next_window_id++;
    window.process = message.process_id;
    window.event_pipe = static_cast<uint32_t>(reply);
    window.surface_handle = static_cast<uint32_t>(surface);
    window.token = message.header.token;
    window.pixels = reinterpret_cast<const uint32_t*>(info.base);
    window.surface_width = message.width;
    window.surface_height = message.height;
    window.width = message.width;
    window.height = message.height;
    window.x = (static_cast<int32_t>(g_fb.width) -
                static_cast<int32_t>(message.width)) / 2;
    window.y = (static_cast<int32_t>(g_fb.height - g_theme.taskbar_height) -
                static_cast<int32_t>(message.height + kTitleHeight)) / 2;
    if (window.y < 0) window.y = 0;
    if (message.title[0] == '\0') {
        copy_text(window.title, sizeof(window.title), "Application");
    } else {
        copy_fixed_text(window.title, sizeof(window.title), message.title,
                        sizeof(message.title));
    }
    SceneDamage damage;
    if (!g_windows.push(window, window_damage_rect, damage)) {
        descriptor_close(window.surface_handle);
        descriptor_close(window.event_pipe);
        return;
    }
    response.header.window_id = window.id;
    response.header.token = window.token;
    response.status = 0;
    descriptor_write(window.event_pipe, &response, sizeof(response));
    render_damage(damage);
}

int32_t find_window(uint32_t id) {
    for (size_t i = 0; i < g_windows.size(); ++i) {
        if (g_windows[i].id == id) return static_cast<int32_t>(i);
    }
    return -1;
}

bool accumulate_damage(const desktop_protocol::MessageHeader& header,
                       Rect (&pending)[kMaxWindows]) {
    if (header.magic != desktop_protocol::kMessageMagic ||
        header.version != desktop_protocol::kVersion ||
        header.type != desktop_protocol::MessageType::Damage ||
        header.size != sizeof(desktop_protocol::DamageMessage)) {
        return false;
    }
    int32_t index = find_window(header.window_id);
    if (index < 0 || header.token == 0 ||
        header.token != g_windows[index].token) {
        return true;
    }
    const auto& damage =
        reinterpret_cast<const desktop_protocol::DamageMessage&>(header);
    Window& window = g_windows[index];
    if (damage.x >= window.surface_width ||
        damage.y >= window.surface_height) {
        return true;
    }
    uint32_t width = damage.width;
    uint32_t height = damage.height;
    if (width > window.surface_width - damage.x) {
        width = window.surface_width - damage.x;
    }
    if (height > window.surface_height - damage.y) {
        height = window.surface_height - damage.y;
    }
    uint32_t left = damage.x * window.width / window.surface_width;
    uint32_t top = damage.y * window.height / window.surface_height;
    uint32_t right =
        ((damage.x + width) * window.width + window.surface_width - 1) /
        window.surface_width;
    uint32_t bottom =
        ((damage.y + height) * window.height + window.surface_height - 1) /
        window.surface_height;
    Rect translated{window.x + static_cast<int32_t>(left),
                    window.y + static_cast<int32_t>(kTitleHeight + top),
                    static_cast<int32_t>(right - left),
                    static_cast<int32_t>(bottom - top)};
    pending[index] = unite(pending[index], translated);
    return true;
}

void flush_damage(Rect (&pending)[kMaxWindows]) {
    Rect combined{};
    for (uint32_t i = 0; i < kMaxWindows; ++i) {
        if (pending[i].width > 0 && pending[i].height > 0) {
            combined = unite(combined, pending[i]);
            pending[i] = {};
        }
    }
    if (combined.width > 0 && combined.height > 0) {
        // A render composites every visible window.  Presenting once per
        // damaged client repeats that whole job when video and a game update
        // in the same pipe batch; combine the batch into one compositor pass.
        render(combined);
    }
}

void handle_message(const desktop_protocol::MessageHeader& header) {
    if (header.magic != desktop_protocol::kMessageMagic ||
        header.version != desktop_protocol::kVersion) return;
    if (header.type == desktop_protocol::MessageType::Create &&
        header.size == sizeof(desktop_protocol::CreateMessage)) {
        handle_create(reinterpret_cast<const desktop_protocol::CreateMessage&>(header));
        return;
    }
    int32_t index = find_window(header.window_id);
    if (index < 0) return;
    if (header.token == 0 || header.token != g_windows[index].token) return;
    if (header.type == desktop_protocol::MessageType::Damage &&
        header.size == sizeof(desktop_protocol::DamageMessage)) {
        const auto& damage =
            reinterpret_cast<const desktop_protocol::DamageMessage&>(header);
        Window& window = g_windows[index];
        if (damage.x >= window.surface_width || damage.y >= window.surface_height) return;
        uint32_t width = damage.width;
        uint32_t height = damage.height;
        if (width > window.surface_width - damage.x) width = window.surface_width - damage.x;
        if (height > window.surface_height - damage.y) height = window.surface_height - damage.y;
        uint32_t left = damage.x * window.width / window.surface_width;
        uint32_t top = damage.y * window.height / window.surface_height;
        uint32_t right =
            ((damage.x + width) * window.width + window.surface_width - 1) /
            window.surface_width;
        uint32_t bottom =
            ((damage.y + height) * window.height + window.surface_height - 1) /
            window.surface_height;
        render({window.x + static_cast<int32_t>(left),
                window.y + static_cast<int32_t>(kTitleHeight + top),
                static_cast<int32_t>(right - left),
                static_cast<int32_t>(bottom - top)});
    } else if (header.type == desktop_protocol::MessageType::Destroy &&
               header.size == sizeof(desktop_protocol::DestroyMessage)) {
        remove_window(static_cast<uint32_t>(index));
    }
}

void consume_server_pipe(uint32_t pipe) {
    long bytes = descriptor_read(pipe, g_receive_buffer + g_receive_used,
                                 sizeof(g_receive_buffer) - g_receive_used);
    if (bytes <= 0) return;
    g_receive_used += static_cast<size_t>(bytes);
    size_t consumed = 0;
    Rect pending_damage[kMaxWindows]{};
    while (g_receive_used - consumed >= sizeof(desktop_protocol::MessageHeader)) {
        auto* header = reinterpret_cast<const desktop_protocol::MessageHeader*>(
            g_receive_buffer + consumed);
        if (header->size < sizeof(*header) || header->size > kReceiveBufferSize) {
            ++consumed;
            continue;
        }
        if (g_receive_used - consumed < header->size) break;
        if (!accumulate_damage(*header, pending_damage)) {
            // Create/destroy messages can change window indices, so commit
            // any accumulated repaint before applying them.
            flush_damage(pending_damage);
            handle_message(*header);
        }
        consumed += header->size;
    }
    // Shared surfaces contain the producer's newest pixels.  Rendering once
    // per read drops redundant queued frame notifications instead of playing
    // stale video frames one by one when a producer outruns the compositor.
    flush_damage(pending_damage);
    if (consumed != 0) {
        for (size_t i = consumed; i < g_receive_used; ++i) {
            g_receive_buffer[i - consumed] = g_receive_buffer[i];
        }
        g_receive_used -= consumed;
    }
    if (g_receive_used == sizeof(g_receive_buffer)) g_receive_used = 0;
}

bool handle_keyboard(uint32_t keyboard) {
    descriptor_defs::KeyboardEvent events[16]{};
    long bytes = descriptor_read(keyboard, events, sizeof(events));
    if (bytes <= 0) return true;
    size_t count = static_cast<size_t>(bytes) / sizeof(events[0]);
    for (size_t i = 0; i < count; ++i) {
        bool pressed =
            (events[i].flags & descriptor_defs::kKeyboardFlagPressed) != 0;
        if (pressed && events[i].scancode == 0x58) return false;  // F12
        if (handle_volume_key(events[i])) {
            const int32_t taskbar_y = static_cast<int32_t>(
                g_fb.height - g_theme.taskbar_height);
            render({volume_widget_x(), taskbar_y, kVolumeWidgetWidth,
                    static_cast<int32_t>(g_theme.taskbar_height)});
            continue;
        }
        if (pressed && events[i].scancode == 0x01 && g_menu_open) {
            g_menu_open = false;
            int32_t taskbar_y =
                static_cast<int32_t>(g_fb.height - g_theme.taskbar_height);
            render({8, start_menu_y(taskbar_y), 220, start_menu_height()});
            continue;
        }
        Window* focused = g_windows.focused();
        if (focused == nullptr) continue;
        desktop_protocol::KeyboardMessage message{
            desktop_protocol::header(desktop_protocol::MessageType::Keyboard,
                                     sizeof(message), focused->id, focused->token),
            events[i].scancode, events[i].mods, events[i].flags};
        (void)descriptor_write(focused->event_pipe, &message, sizeof(message));
    }
    return true;
}

void clamp_cursor() {
    if (g_cursor_x < 0) g_cursor_x = 0;
    if (g_cursor_y < 0) g_cursor_y = 0;
    int32_t max_x = static_cast<int32_t>(g_fb.width) - 1;
    int32_t max_y = static_cast<int32_t>(g_fb.height) - 1;
    if (g_cursor_x > max_x) g_cursor_x = max_x;
    if (g_cursor_y > max_y) g_cursor_y = max_y;
}

void handle_mouse(uint32_t mouse) {
    descriptor_defs::MouseEvent events[24]{};
    long bytes = descriptor_read(mouse, events, sizeof(events));
    if (bytes <= 0) return;
    size_t count = static_cast<size_t>(bytes) / sizeof(events[0]);
    Rect old_cursor = cursor_rect();
    Rect damage{};
    Rect geometry_old{};
    Rect geometry_new{};
    bool geometry_changed = false;
    for (size_t i = 0; i < count; ++i) {
        g_cursor_x += events[i].dx;
        g_cursor_y -= events[i].dy;
        clamp_cursor();
        bool left_pressed = (events[i].buttons & 1u) != 0 &&
                            (g_previous_buttons & 1u) == 0;
        bool left_released = (events[i].buttons & 1u) == 0 &&
                             (g_previous_buttons & 1u) != 0;
        int32_t taskbar_y =
            static_cast<int32_t>(g_fb.height - g_theme.taskbar_height);
        if (left_pressed && g_cursor_y >= taskbar_y + 7) {
            if (g_cursor_x >= 8 && g_cursor_x < 100) {
                g_menu_open = !g_menu_open;
                damage = unite(damage, {8, start_menu_y(taskbar_y), 220,
                                        start_menu_height() +
                                        static_cast<int32_t>(g_theme.taskbar_height)});
            } else if (g_cursor_x >= volume_widget_x() &&
                       g_cursor_x < volume_widget_x() + kVolumeWidgetWidth) {
                int32_t slider_left = volume_widget_x() + kVolumeSliderLeft;
                if (g_cursor_x < slider_left) {
                    toggle_system_mute();
                } else {
                    int32_t value = (g_cursor_x - slider_left) * 100 /
                                    kVolumeSliderWidth;
                    if (value > 100) value = 100;
                    (void)set_system_volume(static_cast<uint32_t>(value));
                }
                damage = unite(damage, {volume_widget_x(), taskbar_y,
                                        kVolumeWidgetWidth,
                                        static_cast<int32_t>(g_theme.taskbar_height)});
            }
        } else if (left_pressed && g_menu_open && g_cursor_x >= 17 &&
                   g_cursor_x < 219) {
            int32_t relative_y = g_cursor_y - start_menu_y(taskbar_y) - 40;
            if (relative_y >= 0) {
                size_t index = static_cast<size_t>(relative_y / 37);
                if (index < g_launcher_count) {
                    g_menu_open = false;
                    launch_app(index);
                    damage = unite(damage, {8, start_menu_y(taskbar_y), 220,
                                            start_menu_height()});
                }
            }
        } else if (left_pressed) {
            int32_t index = window_at(g_cursor_x, g_cursor_y);
            if (index >= 0) {
                SceneDamage focus_damage;
                (void)g_windows.raise(static_cast<size_t>(index),
                                      window_damage_rect, focus_damage);
                for (size_t j = 0; j < focus_damage.size(); ++j)
                    damage = unite(damage, focus_damage[j]);
                Window& window = *g_windows.focused();
                auto chrome = neutrino::ui::window_chrome(
                    window.x, window.y, static_cast<int32_t>(window.width),
                    static_cast<int32_t>(window.height));
                bool on_resize = neutrino::ui::contains(
                    chrome.resize_handle, g_cursor_x, g_cursor_y);
                if (on_resize) {
                    g_resize_index =
                        static_cast<int32_t>(g_windows.size() - 1);
                } else if (neutrino::ui::contains(
                               chrome.titlebar, g_cursor_x, g_cursor_y)) {
                    if (neutrino::ui::contains(
                            chrome.close_button, g_cursor_x, g_cursor_y)) {
                        send_close(window);
                    } else {
                        g_drag_index =
                            static_cast<int32_t>(g_windows.size() - 1);
                        g_drag_offset_x = g_cursor_x - window.x;
                        g_drag_offset_y = g_cursor_y - window.y;
                    }
                }
            }
        }
        if (g_drag_index >= 0 && (events[i].buttons & 1u) != 0) {
            Window& window = g_windows[g_drag_index];
            Rect old = window_damage_rect(window);
            if (!geometry_changed) geometry_old = old;
            window.x = g_cursor_x - g_drag_offset_x;
            window.y = g_cursor_y - g_drag_offset_y;
            int32_t min_x =
                40 - static_cast<int32_t>(window.width);
            int32_t max_x = static_cast<int32_t>(g_fb.width) - 40;
            if (window.x < min_x) window.x = min_x;
            if (window.x > max_x) window.x = max_x;
            int32_t max_y =
                taskbar_y - static_cast<int32_t>(kTitleHeight) - 100;
            if (window.y < 0) window.y = 0;
            if (window.y > max_y) window.y = max_y;
            geometry_new = window_damage_rect(window);
            geometry_changed = true;
        }
        if (g_resize_index >= 0 && (events[i].buttons & 1u) != 0) {
            Window& window = g_windows[g_resize_index];
            Rect old = window_damage_rect(window);
            if (!geometry_changed) geometry_old = old;
            int32_t requested_width = g_cursor_x - window.x + 1;
            int32_t requested_height =
                g_cursor_y - window.y - static_cast<int32_t>(kTitleHeight) + 1;
            int32_t max_width = static_cast<int32_t>(g_fb.width);
            int32_t max_height =
                taskbar_y - window.y - static_cast<int32_t>(kTitleHeight);
            if (requested_width < 160) requested_width = 160;
            if (requested_height < 100) requested_height = 100;
            if (requested_width > max_width) requested_width = max_width;
            if (requested_height > max_height) requested_height = max_height;
            window.width = static_cast<uint32_t>(requested_width);
            window.height = static_cast<uint32_t>(requested_height);
            geometry_new = window_damage_rect(window);
            geometry_changed = true;
        }
        if (left_released) {
            g_drag_index = -1;
            g_resize_index = -1;
        }
        g_previous_buttons = events[i].buttons;
    }
    Rect new_cursor = cursor_rect();
    if (geometry_changed) {
        // Recompose the old and new window bounds independently.  Joining
        // them into one rectangle makes a fast diagonal drag repaint every
        // pixel between the two positions, including unchanged content.
        render(unite(unite(damage, geometry_old), old_cursor));
        render(unite(geometry_new, new_cursor));
        return;
    }
    if (damage.width > 0 && damage.height > 0) {
        render(unite(unite(damage, old_cursor), new_cursor));
        return;
    }

    // Repainting the bounding box of an entire batch makes diagonal pointer
    // movement increasingly expensive as the cursor travels farther.  At high
    // fullscreen resolutions that can starve input processing, while purely
    // horizontal or vertical movement only repaints a thin strip.  Restore the
    // old cursor area and draw the new one as two small regions instead.
    render(old_cursor);
    if (old_cursor.x != new_cursor.x || old_cursor.y != new_cursor.y) {
        render(new_cursor);
    }
}

bool initialize_server(uint32_t& pipe_out, uint32_t& registry_out) {
    uint64_t flags = static_cast<uint64_t>(descriptor_defs::Flag::Readable) |
                     static_cast<uint64_t>(descriptor_defs::Flag::Async);
    long pipe = pipe_open_new(flags);
    if (pipe < 0) return false;
    descriptor_defs::PipeInfo pipe_info{};
    if (pipe_get_info(static_cast<uint32_t>(pipe), &pipe_info) != 0 ||
        pipe_info.id == 0) {
        descriptor_close(static_cast<uint32_t>(pipe));
        return false;
    }
    long registry = shared_memory_open(desktop_protocol::kRegistryName,
                                       sizeof(desktop_protocol::Registry));
    descriptor_defs::SharedMemoryInfo info{};
    if (registry < 0 ||
        shared_memory_get_info(static_cast<uint32_t>(registry), &info) != 0 ||
        info.base == 0 || info.length < sizeof(desktop_protocol::Registry)) {
        if (registry >= 0) descriptor_close(static_cast<uint32_t>(registry));
        descriptor_close(static_cast<uint32_t>(pipe));
        return false;
    }
    auto* published = reinterpret_cast<desktop_protocol::Registry*>(info.base);
    *published = {desktop_protocol::kRegistryMagic, desktop_protocol::kVersion,
                  0, pipe_info.id, process_id()};
    pipe_out = static_cast<uint32_t>(pipe);
    registry_out = static_cast<uint32_t>(registry);
    return true;
}

}  // namespace

int main(uint64_t, uint64_t) {
    load_config();
    load_launchers();
    long session = graphical_session_open();
    if (session < 0) return 1;
    descriptor_defs::GraphicalSessionInfo session_info{};
    if (graphical_session_get_info(static_cast<uint32_t>(session), &session_info) != 0 ||
        session_info.abi_major != descriptor_defs::kGraphicalSessionAbiMajor) {
        descriptor_close(static_cast<uint32_t>(session));
        return 1;
    }
    long framebuffer = framebuffer_open_slot(session_info.display_slot);
    if (framebuffer < 0) {
        descriptor_close(static_cast<uint32_t>(session));
        return 1;
    }
    g_framebuffer = static_cast<uint32_t>(framebuffer);
    if (framebuffer_get_info(g_framebuffer, &g_fb) != 0 ||
        g_fb.virtual_base == 0 || g_fb.width < 320 || g_fb.height < 200 ||
        g_fb.bpp < 16 || g_fb.bpp > 32 || (g_fb.bpp % 8) != 0) {
        descriptor_close(g_framebuffer);
        descriptor_close(static_cast<uint32_t>(session));
        return 1;
    }
    g_pixels = reinterpret_cast<uint8_t*>(g_fb.virtual_base);
    g_bytes_per_pixel = g_fb.bpp / 8u;
    (void)load_cursor_bitmap();

    long keyboard = descriptor_open(kKeyboardType, 0);
    long mouse = mouse_open();
    uint32_t server_pipe = kInvalidDescriptor;
    uint32_t registry = kInvalidDescriptor;
    if (keyboard < 0 || mouse < 0 || !initialize_server(server_pipe, registry) ||
        graphical_session_set_active(static_cast<uint32_t>(session), true) != 0) {
        if (registry != kInvalidDescriptor) descriptor_close(registry);
        if (server_pipe != kInvalidDescriptor) descriptor_close(server_pipe);
        if (mouse >= 0) descriptor_close(static_cast<uint32_t>(mouse));
        if (keyboard >= 0) descriptor_close(static_cast<uint32_t>(keyboard));
        descriptor_close(g_framebuffer);
        descriptor_close(static_cast<uint32_t>(session));
        return 1;
    }
    g_cursor_x = static_cast<int32_t>(g_fb.width) / 2;
    g_cursor_y = static_cast<int32_t>(g_fb.height) / 2;
    render({0, 0, static_cast<int32_t>(g_fb.width),
            static_cast<int32_t>(g_fb.height)});

    bool running = true;
    while (running) {
        descriptor_defs::DescriptorWait waits[3]{
            {static_cast<uint32_t>(keyboard), descriptor_defs::kWaitRead, 0, 0},
            {static_cast<uint32_t>(mouse), descriptor_defs::kWaitRead, 0, 0},
            {server_pipe, descriptor_defs::kWaitRead, 0, 0},
        };
        if (descriptor_wait(waits, 3) < 0) {
            yield();
            continue;
        }
        if ((waits[0].revents & descriptor_defs::kWaitRead) != 0) {
            running = handle_keyboard(static_cast<uint32_t>(keyboard));
        }
        if (running && (waits[1].revents & descriptor_defs::kWaitRead) != 0) {
            handle_mouse(static_cast<uint32_t>(mouse));
        }
        if (running && (waits[2].revents & descriptor_defs::kWaitRead) != 0) {
            consume_server_pipe(server_pipe);
        }
        while (process_wait_child(0, true) >= 0) {}
    }

    for (size_t i = 0; i < g_windows.size(); ++i) send_close(g_windows[i]);
    descriptor_defs::SharedMemoryInfo registry_info{};
    (void)shared_memory_get_info(registry, &registry_info);
    auto* published = reinterpret_cast<desktop_protocol::Registry*>(
        registry_info.base);
    if (published != nullptr) published->server_pipe_id = 0;
    (void)graphical_session_set_active(static_cast<uint32_t>(session), false);
    for (size_t i = 0; i < g_windows.size(); ++i) {
        descriptor_close(g_windows[i].event_pipe);
        descriptor_close(g_windows[i].surface_handle);
    }
    descriptor_close(registry);
    descriptor_close(server_pipe);
    descriptor_close(static_cast<uint32_t>(mouse));
    descriptor_close(static_cast<uint32_t>(keyboard));
    descriptor_close(g_framebuffer);
    descriptor_close(static_cast<uint32_t>(session));
    return 0;
}
