#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../crt/syscall.hpp"
#include "keyboard_scancode.hpp"
#include "descriptors.hpp"

namespace {

constexpr size_t kMaxKeys = 32;
constexpr size_t kMaxKeyLength = 64;
constexpr size_t kMaxValueLength = 128;

void write(long console, const char* text) {
    if (console >= 0 && text != nullptr) {
        descriptor_write(static_cast<uint32_t>(console), text, strlen(text));
    }
}

void copy_text(char* out, size_t out_size, const char* text) {
    if (out == nullptr || out_size == 0) return;
    size_t i = 0;
    while (i + 1 < out_size && text != nullptr && text[i] != '\0') {
        out[i] = text[i];
        ++i;
    }
    out[i] = '\0';
}

bool clear(long console) {
    long type = descriptor_get_type(static_cast<uint32_t>(console));
    descriptor_defs::Property property =
        type == static_cast<long>(descriptor_defs::Type::Vty)
            ? descriptor_defs::Property::VtyClear
            : descriptor_defs::Property::ConsoleClear;
    return descriptor_set_property(static_cast<uint32_t>(console),
                                   static_cast<uint32_t>(property), nullptr, 0) == 0;
}

long key_at(bool machine, size_t index, char* key, size_t size) {
    return machine ? machine_settings_key_at(index, key, size)
                   : settings_key_at(index, key, size);
}

long get_value(bool machine, const char* key, char* value, size_t size) {
    return machine ? machine_settings_get(key, value, size)
                   : settings_get(key, value, size);
}

long set_value(bool machine, const char* key, const char* value) {
    return machine ? machine_settings_set(key, value) : settings_set(key, value);
}

size_t load_keys(bool machine, char keys[kMaxKeys][kMaxKeyLength]) {
    size_t count = 0;
    while (count < kMaxKeys && key_at(machine, count, keys[count], kMaxKeyLength) > 0) {
        ++count;
    }
    return count;
}

bool read_line(long console, char* out, size_t size) {
    long keyboard_handle = descriptor_open(
        static_cast<uint32_t>(descriptor_defs::Type::Keyboard), 0);
    if (keyboard_handle < 0 || out == nullptr || size == 0) return false;
    size_t length = 0;
    out[0] = '\0';
    for (;;) {
        descriptor_defs::KeyboardEvent events[8]{};
        long bytes = descriptor_read(static_cast<uint32_t>(keyboard_handle), events, sizeof(events));
        if (bytes <= 0) { yield(); continue; }
        for (size_t i = 0; i < static_cast<size_t>(bytes) / sizeof(events[0]); ++i) {
            if (!keyboard::is_pressed(events[i]) || keyboard::is_extended(events[i])) continue;
            char ch = keyboard::scancode_to_char(events[i].scancode, events[i].mods);
            if (ch == '\n' || ch == '\r') { write(console, "\n"); descriptor_close(keyboard_handle); return true; }
            if (ch == '\b') { if (length) { --length; out[length] = '\0'; write(console, "\b \b"); } continue; }
            if (ch < 0x20 || ch > 0x7e || length + 1 >= size) continue;
            out[length++] = ch; out[length] = '\0';
            char echo[2] = {ch, '\0'}; write(console, echo);
        }
    }
}

void draw(long console, bool machine, long machine_access, size_t selected,
          char keys[kMaxKeys][kMaxKeyLength], size_t count, const char* status) {
    clear(console);
    write(console, "Hive - settings browser\n\n");
    write(console, machine ? "Machine settings" : "Your settings");
    if (machine) write(console, machine_access == 2 ? " (read/write)" : " (read-only)");
    write(console, "\n\n");
    if (count == 0) write(console, "  [no settings]\n");
    for (size_t i = 0; i < count; ++i) {
        char value[kMaxValueLength]{};
        long result = get_value(machine, keys[i], value, sizeof(value));
        write(console, i == selected ? "> " : "  ");
        write(console, keys[i]); write(console, " = ");
        write(console, result > 0 ? value : "<unavailable>"); write(console, "\n");
    }
    write(console, "\nUp/Down select  Enter edit  a add  Tab switch hive  q quit\n");
    if (!machine_access) write(console, "Machine settings are unavailable (SystemReadSettings required).\n");
    if (status != nullptr && status[0] != '\0') { write(console, status); write(console, "\n"); }
}

}  // namespace

int main(uint64_t arg_ptr, uint64_t) {
    const char* args = reinterpret_cast<const char*>(arg_ptr);
    if (args != nullptr && args[0] != '\0') return 1;
    long console = process_get_standard_descriptor(1);
    if (console < 0) console = descriptor_open(static_cast<uint32_t>(descriptor_defs::Type::Console), 0);
    if (console < 0) return 1;
    long keyboard_handle = descriptor_open(static_cast<uint32_t>(descriptor_defs::Type::Keyboard), 0);
    if (keyboard_handle < 0) return 1;

    long machine_access = machine_settings_access();
    bool machine = false;
    size_t selected = 0;
    char status[96]{};
    char keys[kMaxKeys][kMaxKeyLength]{};
    size_t count = 0;
    bool redraw = true;
    for (;;) {
        if (redraw) {
            count = load_keys(machine, keys);
            if (selected >= count && count != 0) selected = count - 1;
            draw(console, machine, machine_access, selected, keys, count, status);
            status[0] = '\0';
            redraw = false;
        }
        descriptor_defs::KeyboardEvent event{};
        long bytes = descriptor_read(static_cast<uint32_t>(keyboard_handle), &event, sizeof(event));
        if (bytes != static_cast<long>(sizeof(event)) || !keyboard::is_pressed(event)) { yield(); continue; }
        int32_t dx = 0, dy = 0;
        if (keyboard::is_arrow_key(event, dx, dy)) {
            if (dy < 0 && selected) { --selected; redraw = true; }
            if (dy > 0 && selected + 1 < count) { ++selected; redraw = true; }
            continue;
        }
        if (keyboard::is_extended(event)) continue;
        char ch = keyboard::scancode_to_char(event.scancode, event.mods);
        if (ch == 'q' || ch == 'Q') break;
        if (ch == '\t' && machine_access) { machine = !machine; selected = 0; redraw = true; continue; }
        if (ch != '\n' && ch != '\r' && ch != 'a' && ch != 'A') continue;
        if (machine && machine_access != 2) { copy_text(status, sizeof(status), "Machine hive is read-only."); redraw = true; continue; }
        char key[kMaxKeyLength]{};
        if ((ch == 'a' || ch == 'A')) {
            clear(console); write(console, machine ? "Machine key: " : "Your key: ");
            if (!read_line(console, key, sizeof(key)) || key[0] == '\0') { copy_text(status, sizeof(status), "Invalid key."); redraw = true; continue; }
        } else if (count != 0) {
            copy_text(key, sizeof(key), keys[selected]);
        } else { continue; }
        char value[kMaxValueLength]{};
        clear(console); write(console, key); write(console, " value: ");
        if (!read_line(console, value, sizeof(value))) { copy_text(status, sizeof(status), "Input failed."); redraw = true; continue; }
        if (set_value(machine, key, value) == 0) copy_text(status, sizeof(status), "Saved.");
        else copy_text(status, sizeof(status), machine ? "Save denied or failed." : "Save failed.");
        redraw = true;
    }
    descriptor_close(static_cast<uint32_t>(keyboard_handle));
    return 0;
}
