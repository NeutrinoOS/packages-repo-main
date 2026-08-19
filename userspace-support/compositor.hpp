#pragma once

#include <stddef.h>

#include "ui.hpp"

namespace neutrino::ui {

struct WindowChrome {
    Rect outer;
    Rect damage;
    Rect shadow;
    Rect titlebar;
    Rect content;
    Rect close_button;
    Rect resize_handle;
};

inline WindowChrome window_chrome(int32_t x, int32_t y,
                                  int32_t content_width,
                                  int32_t content_height) {
    int32_t title_height = static_cast<int32_t>(Metrics::title_height);
    int32_t outer_height = content_height + title_height;
    return {
        {x, y, content_width, outer_height},
        {x, y, content_width + 5, outer_height + 6},
        {x + 3, y + 4, content_width + 2, outer_height + 2},
        {x, y, content_width, title_height},
        {x, y + title_height, content_width, content_height},
        {x + content_width - 27, y + 6, 20, 18},
        {x + content_width - 14, y + outer_height - 14, 14, 14},
    };
}

inline Rect unite(Rect left, Rect right) {
    if (left.width <= 0 || left.height <= 0) return right;
    if (right.width <= 0 || right.height <= 0) return left;
    int32_t x = left.x < right.x ? left.x : right.x;
    int32_t y = left.y < right.y ? left.y : right.y;
    int32_t end_x = left.x + left.width > right.x + right.width
                        ? left.x + left.width : right.x + right.width;
    int32_t end_y = left.y + left.height > right.y + right.height
                        ? left.y + left.height : right.y + right.height;
    return {x, y, end_x - x, end_y - y};
}

template <size_t Capacity>
class DamageRegion {
public:
    void invalidate(Rect rect) {
        if (rect.width <= 0 || rect.height <= 0) return;
        for (size_t i = 0; i < count_; ++i) {
            if (intersect(rects_[i], rect).width > 0) {
                rects_[i] = unite(rects_[i], rect);
                return;
            }
        }
        if (count_ < Capacity) rects_[count_++] = rect;
        else rects_[Capacity - 1] = unite(rects_[Capacity - 1], rect);
    }

    size_t size() const { return count_; }
    Rect operator[](size_t index) const { return rects_[index]; }
    void clear() { count_ = 0; }

private:
    Rect rects_[Capacity]{};
    size_t count_ = 0;
};

// A compact back-to-front stack. Focus is an explicit property of stack
// ordering, and every operation invalidates all decorated bounds whose focus
// appearance or visibility changes.
template <typename Item, size_t Capacity>
class WindowStack {
public:
    size_t size() const { return count_; }
    bool empty() const { return count_ == 0; }
    bool full() const { return count_ == Capacity; }
    Item& operator[](size_t index) { return items_[index]; }
    const Item& operator[](size_t index) const { return items_[index]; }
    Item* focused() { return count_ == 0 ? nullptr : &items_[count_ - 1]; }
    const Item* focused() const {
        return count_ == 0 ? nullptr : &items_[count_ - 1];
    }

    template <typename Bounds, size_t DamageCapacity>
    bool push(const Item& item, Bounds bounds,
              DamageRegion<DamageCapacity>& damage) {
        if (full()) return false;
        if (Item* old_focus = focused()) damage.invalidate(bounds(*old_focus));
        items_[count_++] = item;
        damage.invalidate(bounds(items_[count_ - 1]));
        return true;
    }

    template <typename Bounds, size_t DamageCapacity>
    bool raise(size_t index, Bounds bounds,
               DamageRegion<DamageCapacity>& damage) {
        if (index >= count_) return false;
        if (index + 1 == count_) return true;
        damage.invalidate(bounds(items_[count_ - 1]));
        damage.invalidate(bounds(items_[index]));
        Item selected = items_[index];
        for (size_t i = index; i + 1 < count_; ++i) items_[i] = items_[i + 1];
        items_[count_ - 1] = selected;
        damage.invalidate(bounds(items_[count_ - 1]));
        return true;
    }

    template <typename Bounds, size_t DamageCapacity>
    bool erase(size_t index, Bounds bounds,
               DamageRegion<DamageCapacity>& damage) {
        if (index >= count_) return false;
        bool removed_focus = index + 1 == count_;
        damage.invalidate(bounds(items_[index]));
        for (size_t i = index; i + 1 < count_; ++i) items_[i] = items_[i + 1];
        items_[--count_] = {};
        if (removed_focus && focused()) damage.invalidate(bounds(*focused()));
        return true;
    }

private:
    Item items_[Capacity]{};
    size_t count_ = 0;
};

}  // namespace neutrino::ui
