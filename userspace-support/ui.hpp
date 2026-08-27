#pragma once

#include <stddef.h>
#include <stdint.h>

#include "font8x8_basic.hpp"

namespace neutrino::ui {

struct Rect { int32_t x, y, width, height; };

struct Palette {
    static constexpr uint32_t desktop_top = 0x173f58;
    static constexpr uint32_t desktop_bottom = 0x0d2637;
    static constexpr uint32_t surface = 0xf3f6f8;
    static constexpr uint32_t surface_raised = 0xffffff;
    static constexpr uint32_t panel = 0xe8eef2;
    static constexpr uint32_t ink = 0x17232c;
    static constexpr uint32_t ink_muted = 0x65737d;
    static constexpr uint32_t chrome = 0x263641;
    static constexpr uint32_t chrome_inactive = 0x3c4b55;
    static constexpr uint32_t chrome_text = 0xf7fafb;
    static constexpr uint32_t accent = 0x44b7d7;
    static constexpr uint32_t accent_pressed = 0x2588a7;
    static constexpr uint32_t outline = 0x0b151c;
    static constexpr uint32_t shadow = 0x08131b;
    static constexpr uint32_t danger = 0xd95d66;
};

struct Metrics {
    static constexpr uint32_t title_height = 30;
    static constexpr uint32_t taskbar_height = 48;
    static constexpr uint32_t spacing = 8;
    static constexpr uint32_t control_height = 30;
    static constexpr uint32_t shadow_offset = 5;
};

inline Rect intersect(Rect a, Rect b) {
    int32_t left = a.x > b.x ? a.x : b.x;
    int32_t top = a.y > b.y ? a.y : b.y;
    int32_t right = a.x + a.width < b.x + b.width ? a.x + a.width : b.x + b.width;
    int32_t bottom = a.y + a.height < b.y + b.height ? a.y + a.height : b.y + b.height;
    if (right <= left || bottom <= top) return {0, 0, 0, 0};
    return {left, top, right - left, bottom - top};
}

inline bool contains(Rect rect, int32_t x, int32_t y) {
    return x >= rect.x && y >= rect.y && x < rect.x + rect.width &&
           y < rect.y + rect.height;
}

inline int32_t centered_text_y(Rect rect, uint32_t scale = 1) {
    const int32_t height = static_cast<int32_t>(8u * scale);
    return rect.y + (rect.height - height) / 2;
}

class Canvas {
public:
    Canvas(uint32_t* pixels, uint32_t width, uint32_t height, uint32_t stride = 0)
        : pixels_(pixels), width_(width), height_(height), stride_(stride ? stride : width),
          clip_{0, 0, static_cast<int32_t>(width), static_cast<int32_t>(height)} {}

    void set_clip(Rect clip) {
        clip_ = intersect(clip, {0, 0, static_cast<int32_t>(width_), static_cast<int32_t>(height_)});
    }
    void fill(Rect rect, uint32_t color) {
        rect = intersect(rect, clip_);
        for (int32_t y = 0; y < rect.height; ++y) {
            uint32_t* row = pixels_ + static_cast<size_t>(rect.y + y) * stride_ + rect.x;
            for (int32_t x = 0; x < rect.width; ++x) row[x] = color & 0x00ffffffu;
        }
    }
    void frame(Rect rect, uint32_t color) {
        if (rect.width <= 0 || rect.height <= 0) return;
        fill({rect.x, rect.y, rect.width, 1}, color);
        fill({rect.x, rect.y + rect.height - 1, rect.width, 1}, color);
        fill({rect.x, rect.y, 1, rect.height}, color);
        fill({rect.x + rect.width - 1, rect.y, 1, rect.height}, color);
    }
    void glyph(int32_t x, int32_t y, uint8_t ch, uint32_t color, uint32_t scale = 1) {
        if (ch >= 128) ch = '?';
        for (uint32_t row = 0; row < 8; ++row) {
            uint8_t bits = font8x8_basic[ch][row];
            for (uint32_t col = 0; col < 8; ++col) {
                if (bits & (1u << col)) fill({x + static_cast<int32_t>(col * scale), y + static_cast<int32_t>(row * scale), static_cast<int32_t>(scale), static_cast<int32_t>(scale)}, color);
            }
        }
    }
    void text(int32_t x, int32_t y, const char* value, uint32_t color, uint32_t scale = 1) {
        if (!value) return;
        while (*value) { glyph(x, y, static_cast<uint8_t>(*value++), color, scale); x += static_cast<int32_t>(8 * scale); }
    }
    void panel(Rect rect) {
        fill(rect, Palette::surface); frame(rect, Palette::outline);
        if (rect.width > 2 && rect.height > 2) frame({rect.x + 1, rect.y + 1, rect.width - 2, rect.height - 2}, Palette::surface_raised);
    }
    void button(Rect rect, const char* label, bool pressed = false, bool emphasized = false) {
        uint32_t background = pressed ? Palette::accent_pressed : emphasized ? Palette::accent : Palette::panel;
        uint32_t foreground = emphasized || pressed ? Palette::chrome_text : Palette::ink;
        fill(rect, background); frame(rect, pressed ? Palette::outline : Palette::ink_muted);
        size_t length = 0; while (label && label[length]) ++length;
        text(rect.x + (rect.width - static_cast<int32_t>(length * 8)) / 2,
             centered_text_y(rect), label, foreground);
    }
private:
    uint32_t* pixels_; uint32_t width_, height_, stride_; Rect clip_;
};

}  // namespace neutrino::ui
