#pragma once
// ============================================================================
//  einkui/include/rect.h
//
//  Minimal axis-aligned rectangle.  All layout maths lives here.
//  Zero heap usage: pure value type, no virtuals, no allocation.
// ============================================================================
#include <stdint.h>
#include <algorithm>

namespace einkui {

struct Rect {
    int16_t x = 0, y = 0, w = 0, h = 0;

    constexpr Rect() = default;
    constexpr Rect(int16_t x, int16_t y, int16_t w, int16_t h)
        : x(x), y(y), w(w), h(h) {}

    // right / bottom edges (exclusive)
    constexpr int16_t x2() const { return x + w; }
    constexpr int16_t y2() const { return y + h; }

    constexpr bool empty() const { return w <= 0 || h <= 0; }

    constexpr bool contains(int16_t px, int16_t py) const {
        return px >= x && py >= y && px < x2() && py < y2();
    }
    constexpr bool containsX(int16_t px) const { return px >= x && px < x2(); }
    constexpr bool containsY(int16_t py) const { return py >= y && py < y2(); }

    // intersection (& / &=)
    Rect& operator&=(const Rect& r) {
        int16_t nx1 = std::max(x, r.x);
        int16_t ny1 = std::max(y, r.y);
        int16_t nx2 = std::min(x2(), r.x2());
        int16_t ny2 = std::min(y2(), r.y2());
        x = nx1; y = ny1;
        w = std::max<int16_t>(0, nx2 - nx1);
        h = std::max<int16_t>(0, ny2 - ny1);
        return *this;
    }
    Rect operator&(const Rect& r) const { Rect t = *this; t &= r; return t; }

    // union (| / |=)
    Rect& operator|=(const Rect& r) {
        if (r.empty()) return *this;
        if (empty()) { *this = r; return *this; }
        int16_t nx1 = std::min(x, r.x);
        int16_t ny1 = std::min(y, r.y);
        int16_t nx2 = std::max(x2(), r.x2());
        int16_t ny2 = std::max(y2(), r.y2());
        x = nx1; y = ny1;
        w = nx2 - nx1; h = ny2 - ny1;
        return *this;
    }
    Rect operator|(const Rect& r) const { Rect t = *this; t |= r; return t; }

    constexpr bool operator==(const Rect& r) const {
        return x == r.x && y == r.y && w == r.w && h == r.h;
    }
    constexpr bool operator!=(const Rect& r) const { return !(*this == r); }

    // Expand by margin on all sides
    Rect expanded(int16_t m) const {
        return { int16_t(x-m), int16_t(y-m), int16_t(w+2*m), int16_t(h+2*m) };
    }
    // Inner area, shrunk by padding on all sides
    Rect inset(int16_t p) const {
        int16_t nw = std::max<int16_t>(0, w - 2*p);
        int16_t nh = std::max<int16_t>(0, h - 2*p);
        return { int16_t(x+p), int16_t(y+p), nw, nh };
    }
};

// Colors: one struct, two values – no dynamic allocation ever.
struct Colors {
    uint16_t fg = 0;  // black  (EPD_BLACK)
    uint16_t bg = 1;  // white  (EPD_WHITE)
};

} // namespace einkui
