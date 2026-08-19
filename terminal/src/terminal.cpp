#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "descriptors.hpp"
#include "keyboard_scancode.hpp"
#include "ui.hpp"
#include "window.hpp"
#include "../crt/syscall.hpp"
#include "../desktop_protocol.hpp"

namespace {

constexpr uint32_t kCols = 80;
constexpr uint32_t kRows = 25;
constexpr uint32_t kGlyphSize = 8;
constexpr uint32_t kPadding = 8;
constexpr uint32_t kWidth = kCols * kGlyphSize + kPadding * 2;
constexpr uint32_t kHeight = kRows * kGlyphSize + kPadding * 2;
constexpr size_t kCellCount = static_cast<size_t>(kCols) * kRows;
constexpr uint32_t kVtyType =
    static_cast<uint32_t>(descriptor_defs::Type::Vty);

uint32_t g_vty = kInvalidDescriptor;
uint32_t g_shell = 0;
neutrino::ui::Window g_desktop;
uint32_t* g_pixels = nullptr;
bool g_running = true;
bool g_cursor_visible = true;
bool g_previous_cursor_visible = true;
uint32_t g_cursor_ticks = 0;
descriptor_defs::VtyCell g_cells[kCellCount]{};
descriptor_defs::VtyCell g_previous_cells[kCellCount]{};
descriptor_defs::VtyInfo g_previous_info{};
bool g_have_previous = false;

void append_decimal(char* output, size_t capacity, uint64_t value) {
    char reverse[24];
    size_t count = 0;
    do {
        reverse[count++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value != 0 && count < sizeof(reverse));
    size_t used = strlen(output);
    while (count != 0 && used + 1 < capacity) {
        output[used++] = reverse[--count];
    }
    output[used] = '\0';
}

void fill(uint32_t x, uint32_t y, uint32_t width, uint32_t height,
          uint32_t color) {
    neutrino::ui::Canvas canvas(g_pixels, kWidth, kHeight);
    canvas.fill({static_cast<int32_t>(x), static_cast<int32_t>(y),
                 static_cast<int32_t>(width), static_cast<int32_t>(height)}, color);
}

void draw_glyph(uint32_t x, uint32_t y, uint8_t character,
                uint32_t foreground) {
    neutrino::ui::Canvas canvas(g_pixels, kWidth, kHeight);
    canvas.glyph(static_cast<int32_t>(x), static_cast<int32_t>(y), character,
                 foreground);
}

void send_damage(uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
    if (!g_desktop.present({static_cast<int32_t>(x), static_cast<int32_t>(y),
                            static_cast<int32_t>(width),
                            static_cast<int32_t>(height)})) {
        g_running = false;
    }
}

void draw_cell(uint32_t column, uint32_t row,
               const descriptor_defs::VtyInfo& info) {
    const auto& cell = g_cells[static_cast<size_t>(row) * kCols + column];
    uint32_t x = kPadding + column * kGlyphSize;
    uint32_t y = kPadding + row * kGlyphSize;
    uint32_t foreground = cell.fg;
    uint32_t background = cell.bg;
    bool cursor = column == info.cursor_x && row == info.cursor_y &&
                  g_cursor_visible;
    if (cursor) {
        uint32_t swap = foreground;
        foreground = background;
        background = swap;
    }
    fill(x, y, kGlyphSize, kGlyphSize, background);
    draw_glyph(x, y, cell.ch, foreground);
    if ((cell.flags & descriptor_defs::kTextCellUnderline) != 0) {
        fill(x, y + kGlyphSize - 1, kGlyphSize, 1, foreground);
    }
}

bool refresh_vty() {
    descriptor_defs::VtyInfo info{};
    if (descriptor_get_property(
            g_vty,
            static_cast<uint32_t>(descriptor_defs::Property::VtyInfo),
            &info, sizeof(info)) != 0 ||
        info.cols != kCols || info.rows != kRows ||
        info.cell_bytes != sizeof(descriptor_defs::VtyCell) ||
        descriptor_get_property(
            g_vty,
            static_cast<uint32_t>(descriptor_defs::Property::VtyCells),
            g_cells, sizeof(g_cells)) != 0) {
        return false;
    }
    uint32_t min_column = kCols;
    uint32_t min_row = kRows;
    uint32_t max_column = 0;
    uint32_t max_row = 0;
    bool changed = false;
    if (!g_have_previous) fill(0, 0, kWidth, kHeight, 0x101419);
    for (uint32_t row = 0; row < kRows; ++row) {
        for (uint32_t column = 0; column < kCols; ++column) {
            size_t index = static_cast<size_t>(row) * kCols + column;
            bool cell_changed = !g_have_previous ||
                memcmp(&g_cells[index], &g_previous_cells[index],
                       sizeof(g_cells[index])) != 0;
            bool was_cursor = g_have_previous && g_previous_cursor_visible &&
                column == g_previous_info.cursor_x &&
                row == g_previous_info.cursor_y;
            bool is_cursor = g_cursor_visible && column == info.cursor_x &&
                row == info.cursor_y;
            if (!cell_changed && was_cursor == is_cursor) continue;
            draw_cell(column, row, info);
            if (column < min_column) min_column = column;
            if (column > max_column) max_column = column;
            if (row < min_row) min_row = row;
            if (row > max_row) max_row = row;
            changed = true;
        }
    }
    memcpy(g_previous_cells, g_cells, sizeof(g_cells));
    g_previous_info = info;
    g_previous_cursor_visible = g_cursor_visible;
    bool first_frame = !g_have_previous;
    g_have_previous = true;
    if (first_frame) {
        send_damage(0, 0, kWidth, kHeight);
    } else if (changed) {
        send_damage(kPadding + min_column * kGlyphSize,
                    kPadding + min_row * kGlyphSize,
                    (max_column - min_column + 1) * kGlyphSize,
                    (max_row - min_row + 1) * kGlyphSize);
    }
    return g_running;
}

void inject(const void* data, size_t size) {
    if (data == nullptr || size == 0) return;
    (void)descriptor_set_property(
        g_vty,
        static_cast<uint32_t>(descriptor_defs::Property::VtyInjectInput),
        data, size);
}

void handle_keyboard(const desktop_protocol::KeyboardMessage& message) {
    if ((message.flags & descriptor_defs::kKeyboardFlagPressed) == 0) return;
    descriptor_defs::KeyboardEvent event{
        static_cast<uint8_t>(message.scancode),
        static_cast<uint8_t>(message.flags),
        static_cast<uint8_t>(message.mods), 0};
    if (keyboard::is_extended(event)) {
        const char* sequence = nullptr;
        switch (event.scancode) {
            case keyboard::kScancodeUp: sequence = "\x1b[A"; break;
            case keyboard::kScancodeDown: sequence = "\x1b[B"; break;
            case keyboard::kScancodeRight: sequence = "\x1b[C"; break;
            case keyboard::kScancodeLeft: sequence = "\x1b[D"; break;
            default: break;
        }
        if (sequence != nullptr) inject(sequence, 3);
        return;
    }
    char value = keyboard::scancode_to_char(event.scancode, event.mods);
    if (value != 0) inject(&value, 1);
}

void handle_event(const desktop_protocol::MessageHeader& header) {
    if (header.token != g_desktop.token()) return;
    if (desktop_protocol::valid(header,
                                desktop_protocol::MessageType::Keyboard,
                                sizeof(desktop_protocol::KeyboardMessage))) {
        handle_keyboard(
            reinterpret_cast<const desktop_protocol::KeyboardMessage&>(header));
    } else if (desktop_protocol::valid(header,
                                       desktop_protocol::MessageType::Close,
                                       sizeof(desktop_protocol::CloseMessage))) {
        g_running = false;
    }
}

void poll_events() {
    uint8_t message[sizeof(desktop_protocol::KeyboardMessage)]{};
    for (;;) {
        long count = g_desktop.next_event(message, sizeof(message));
        if (count == 0) return;
        if (count < 0) { g_running = false; return; }
        handle_event(*reinterpret_cast<desktop_protocol::MessageHeader*>(message));
    }
}

bool connect_desktop() {
    if (!g_desktop.open(kWidth, kHeight, "Terminal", "neutrino.terminal."))
        return false;
    g_pixels = g_desktop.pixels();
    return g_pixels != nullptr;
}

void cleanup() {
    if (g_shell != 0) {
        (void)process_control(g_shell, PROCESS_CONTROL_KILL, 0);
        (void)process_wait_child(g_shell, true);
    }
    if (g_vty != kInvalidDescriptor) descriptor_close(g_vty);
    g_desktop.close();
}

}  // namespace

int main(uint64_t, uint64_t) {
    if (!connect_desktop()) return 1;
    uint64_t vty_flags =
        static_cast<uint64_t>(descriptor_defs::Flag::Readable) |
        static_cast<uint64_t>(descriptor_defs::Flag::Writable);
    long vty = descriptor_open(
        kVtyType, 0, vty_flags,
        static_cast<uint64_t>(descriptor_defs::VtyOpen::Attach));
    if (vty < 0) {
        cleanup();
        return 1;
    }
    g_vty = static_cast<uint32_t>(vty);
    descriptor_defs::VtyInfo info{};
    if (descriptor_get_property(
            g_vty,
            static_cast<uint32_t>(descriptor_defs::Property::VtyInfo),
            &info, sizeof(info)) != 0 || info.id == 0 ||
        info.cols != kCols || info.rows != kRows) {
        cleanup();
        return 1;
    }

    char shell_args[32] = "vty=";
    append_decimal(shell_args, sizeof(shell_args), info.id);
    ProcessStdioConfig stdio{g_vty, g_vty, g_vty, 0};
    long shell = child_with_stdio("@sys/binary/shell.elf", shell_args, 0,
                                  "@sys", &stdio);
    if (shell < 0) {
        cleanup();
        return 1;
    }
    g_shell = static_cast<uint32_t>(shell);
    (void)refresh_vty();

    while (g_running) {
        poll_events();
        if ((g_previous_info.flags & descriptor_defs::kVtyCursorBlink) != 0) {
            if (++g_cursor_ticks >= 31) {
                g_cursor_ticks = 0;
                g_cursor_visible = !g_cursor_visible;
            }
        } else if (!g_cursor_visible) {
            g_cursor_visible = true;
            g_cursor_ticks = 0;
        }
        if (!refresh_vty()) break;
        long child_status = process_wait_child(g_shell, true);
        if (child_status >= 0 || child_status == -1) {
            g_shell = 0;
            break;
        }
        (void)sleep_ms(16);
    }
    cleanup();
    return 0;
}
