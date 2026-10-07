#pragma once
// ============================================================================
//  einkui/include/element.h
//
//  Layout contract
//  ---------------
//  Parent::layoutChildren(ctx, inner):
//    inner = the content rectangle (already inset by padding).
//    For each child, cursor = {slotX, slotY, 0, 0} = exact pixel where
//    the child's margin-box top-left goes.
//    placeAt(cursor) → child.x = cursor.x + margin, child.y = cursor.y + margin.
//
//  width_ / height_  = user-set fixed size (0 = auto).  NEVER written by layout.
//  layoutW_ / layoutH_ = set by Parent before measuring child.  0 = not set.
//    measure() uses: fixedW > 0 → fixedW, else layoutW > 0 → layoutW, else fill area.
//
//  VG icon convention
//  ------------------
//  If label starts with '&', the rest is a VG asset name.
//  Text after a space following the asset name is drawn as a caption.
// ============================================================================

#include <stdint.h>
#include <string>
#include <vector>
#include <memory>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <cmath>

#include "rect.h"
#include "style.h"
#include "callbacks.h"
#include "theme.h"

struct TTouchFrame;

namespace einkui {

// ============================================================================
//  DrawCtx
//
//  Type-erased drawing surface + typography/shape helpers shared by all
//  elements.  Raw primitives are function pointers filled by makeDrawCtx();
//  the helper methods below build the einkui look on top of them.
// ============================================================================
struct DrawCtx {
    void* d = nullptr;

    int16_t (*width) (void*) = nullptr;
    int16_t (*height)(void*) = nullptr;

    void (*fillScreen)   (void*, uint16_t)                                          = nullptr;
    void (*fillRect)     (void*, int16_t,int16_t,int16_t,int16_t,uint16_t)         = nullptr;
    void (*drawRect)     (void*, int16_t,int16_t,int16_t,int16_t,uint16_t)         = nullptr;
    void (*fillRoundRect)(void*, int16_t,int16_t,int16_t,int16_t,int16_t,uint16_t) = nullptr;
    void (*drawRoundRect)(void*, int16_t,int16_t,int16_t,int16_t,int16_t,uint16_t) = nullptr;
    void (*drawLine)     (void*, int16_t,int16_t,int16_t,int16_t,uint16_t)         = nullptr;
    void (*drawFastHLine)(void*, int16_t,int16_t,int16_t,uint16_t)                 = nullptr;
    void (*drawFastVLine)(void*, int16_t,int16_t,int16_t,uint16_t)                 = nullptr;
    void (*drawPixel)    (void*, int16_t,int16_t,uint16_t)                         = nullptr;

    void (*setTextSize)  (void*, uint8_t)                                                     = nullptr;
    void (*setTextColor) (void*, uint16_t)                                                    = nullptr;
    void (*setCursor)    (void*, int16_t,int16_t)                                             = nullptr;
    void (*printStr)     (void*, const char*)                                                 = nullptr;
    void (*printChar)    (void*, char)                                                        = nullptr;
    void (*getTextBounds)(void*, const char*, int16_t*,int16_t*,uint16_t*,uint16_t*)         = nullptr;
    void (*setFont)      (void*, const GFXfont*)                                              = nullptr;

    void (*drawVG)(void* disp, const uint8_t* data,
                   int16_t x, int16_t y, int16_t w, int16_t h,
                   uint16_t fg, uint16_t bg)                                                  = nullptr;

    const Theme* theme = nullptr;   // nullptr → defaultTheme()

    int16_t W() const { return width(d);  }
    int16_t H() const { return height(d); }
    const Theme& th() const { return theme ? *theme : defaultTheme(); }

    // ---- typography ----------------------------------------------------------
    // Select the font for a text role (TEXT_BODY …).  Legacy callers that pass
    // a classic text size (1/2/3) get the matching role, so old code still works.
    void useText(uint8_t role) const {
        if (role >= TEXT_ROLES) role = TEXT_DISPLAY;
        const Theme& t = th();
        const GFXfont* f = t.font[role];
        if (setFont) setFont(d, f);
        setTextSize(d, f ? 1 : t.legacySize[role]);
    }

    struct Metrics { int16_t cap; int16_t desc; int16_t line; };
    // Cap height / descender / line advance for a role (cached per theme).
    Metrics metrics(uint8_t role) const {
        if (role >= TEXT_ROLES) role = TEXT_DISPLAY;
        const Theme* t = &th();
        if (mCacheTheme_ != t) { for (auto& m : mCache_) m.line = 0; mCacheTheme_ = t; }
        Metrics& m = mCache_[role];
        if (m.line == 0) {
            useText(role);
            int16_t x1=0,y1=0; uint16_t w=0,h=0;
            getTextBounds(d, "H", &x1, &y1, &w, &h);
            int16_t cap = (int16_t)h;
            getTextBounds(d, "Hgy", &x1, &y1, &w, &h);
            int16_t desc = (int16_t)h - cap;
            if (desc < 0) desc = 0;
            m.cap = cap; m.desc = desc;
            m.line = int16_t(cap + desc + std::max<int16_t>(2, cap / 2));
        }
        return m;
    }

    int16_t textWidth(const char* s, uint8_t role) const {
        if (!s || !*s) return 0;
        useText(role);
        int16_t x1=0,y1=0; uint16_t w=0,h=0;
        getTextBounds(d, s, &x1, &y1, &w, &h);
        return int16_t(w + (x1 > 0 ? x1 : 0));
    }

    // Draw text with its baseline at (x, baseY).
    void text(int16_t x, int16_t baseY, const char* s, uint8_t role, uint16_t col) const {
        if (!s || !*s) return;
        useText(role);
        setTextColor(d, col);
        if (th().font[role < TEXT_ROLES ? role : TEXT_DISPLAY]) setCursor(d, x, baseY);
        else {   // classic font: cursor is the glyph top-left
            setCursor(d, x, int16_t(baseY - metrics(role).cap));
        }
        printStr(d, s);
    }

    // Draw text inside a box.  halign/valign use the TEXT_HALIGN_* / TEXT_VALIGN_*
    // constants.  Vertical centring uses the cap height, so every label sits on
    // the same optical centre regardless of descenders.  Text that does not fit
    // is shortened with "..." when ellipsis is true.
    void textBox(int16_t bx, int16_t by, int16_t bw, int16_t bh, const char* s,
                 uint8_t role, uint16_t col,
                 uint8_t halign = TEXT_HALIGN_LEFT, uint8_t valign = TEXT_VALIGN_MIDDLE,
                 bool ellipsis = true) const
    {
        if (!s || !*s || bw <= 0) return;
        char buf[96];
        const char* out = s;
        int16_t tw = textWidth(s, role);
        if (ellipsis && tw > bw) {
            out = fitText(s, role, bw, buf, sizeof(buf));
            tw  = textWidth(out, role);
        }
        Metrics m = metrics(role);
        int16_t tx;
        if      (halign == TEXT_HALIGN_CENTER) tx = int16_t(bx + (bw - tw) / 2);
        else if (halign == TEXT_HALIGN_RIGHT)  tx = int16_t(bx + bw - tw);
        else                                   tx = bx;
        int16_t base;
        if      (valign == TEXT_VALIGN_TOP)    base = int16_t(by + m.cap);
        else if (valign == TEXT_VALIGN_BOTTOM) base = int16_t(by + bh - m.desc);
        else                                   base = int16_t(by + (bh + m.cap) / 2);
        text(tx, base, out, role, col);
    }

    // Shorten s with "..." so it fits into maxW pixels.  Returns buf (or s).
    const char* fitText(const char* s, uint8_t role, int16_t maxW,
                        char* buf, size_t len) const {
        size_t n = strlen(s);
        if (n + 4 > len) n = len - 4;
        while (n > 0) {
            size_t k = n;
            while (k > 0 && s[k-1] == ' ') --k;            // no space before "..."
            while (k > 0 && (uint8_t(s[k]) & 0xC0) == 0x80) --k; // keep UTF-8 intact
            memcpy(buf, s, k); memcpy(buf + k, "...", 4);
            if (textWidth(buf, role) <= maxW) return buf;
            n = k > 0 ? k - 1 : 0;
        }
        return "";
    }

    // Kept for backwards compatibility (centres with the *current* font).
    void textBounds(const char* s, uint8_t sz, uint16_t& tw, uint16_t& th_) const {
        useText(sz);
        int16_t x1=0, y1=0;
        getTextBounds(d, s, &x1, &y1, &tw, &th_);
    }
    void textBoundsFull(const char* s, uint8_t sz,
                        uint16_t& tw, uint16_t& th_,
                        int16_t& x1, int16_t& y1) const {
        useText(sz);
        x1=0; y1=0;
        getTextBounds(d, s, &x1, &y1, &tw, &th_);
    }
    void drawCentered(int16_t bx, int16_t by, int16_t bw, int16_t bh,
                      const char* s) const {
        if (!s || !*s) return;
        uint16_t tw=0, th_=0; int16_t x1=0, y1=0;
        getTextBounds(d, s, &x1, &y1, &tw, &th_);
        int16_t boxTop  = by + (bh - (int16_t)th_) / 2;
        int16_t boxLeft = bx + (bw - (int16_t)tw) / 2;
        setCursor(d, int16_t(boxLeft - x1), int16_t(boxTop - y1));
        printStr(d, s);
    }

    // ---- shapes ----------------------------------------------------------------
    void fillCircle(int16_t cx, int16_t cy, int16_t r, uint16_t col) const {
        if (r <= 0) { drawPixel(d, cx, cy, col); return; }
        fillRoundRect(d, int16_t(cx-r), int16_t(cy-r), int16_t(2*r+1), int16_t(2*r+1), r, col);
    }
    void drawCircle(int16_t cx, int16_t cy, int16_t r, uint16_t col) const {
        if (r <= 0) { drawPixel(d, cx, cy, col); return; }
        drawRoundRect(d, int16_t(cx-r), int16_t(cy-r), int16_t(2*r+1), int16_t(2*r+1), r, col);
    }

    // 1-bit "grey": ordered dither of col over the area.
    //   level 1 = 25 %,  2 = 50 % (checkerboard),  3 = 75 %
    void dither(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t col,
                uint8_t level = 1) const {
        for (int16_t yy = y; yy < y + h; ++yy)
            for (int16_t xx = x; xx < x + w; ++xx)
                if (ditherOn(xx, yy, level)) drawPixel(d, xx, yy, col);
    }
    // Dithered rounded rectangle (fills only pixels inside the rounded shape).
    void ditherRound(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r,
                     uint16_t col, uint8_t level = 1) const {
        if (r * 2 > w) r = w / 2;
        if (r * 2 > h) r = h / 2;
        for (int16_t yy = 0; yy < h; ++yy) {
            int16_t inset = 0;
            int16_t dy = yy < r ? int16_t(r - yy) : (yy >= h - r ? int16_t(yy - (h - r - 1)) : 0);
            if (dy > 0) {
                float dx = float(r) - sqrtf(float(r*r - (dy-0.5f)*(dy-0.5f) > 0 ? r*r - (dy-0.5f)*(dy-0.5f) : 0));
                inset = int16_t(dx + 0.5f);
            }
            for (int16_t xx = int16_t(x + inset); xx < x + w - inset; ++xx)
                if (ditherOn(xx, int16_t(y + yy), level)) drawPixel(d, xx, int16_t(y + yy), col);
        }
    }
    static bool ditherOn(int16_t x, int16_t y, uint8_t level) {
        switch (level) {
        case 1:  return ((x + 2*y) & 3) == 0;
        case 2:  return ((x + y) & 1) == 0;
        default: return ((x + 2*y) & 3) != 0;
        }
    }
    // Dotted 1px horizontal rule — reads as a light grey line on e-paper.
    void hairline(int16_t x, int16_t y, int16_t w, uint16_t col) const {
        for (int16_t xx = x; xx < x + w; ++xx)
            if (((xx + y) & 1) == 0) drawPixel(d, xx, y, col);
    }
    void vHairline(int16_t x, int16_t y, int16_t h, uint16_t col) const {
        for (int16_t yy = y; yy < y + h; ++yy)
            if (((x + yy) & 1) == 0) drawPixel(d, x, yy, col);
    }

    // Thick arc with round caps.  Angles in degrees, clockwise from 12 o'clock.
    void arc(int16_t cx, int16_t cy, int16_t r, int16_t thick,
             float a0, float a1, uint16_t col) const {
        if (a1 < a0) std::swap(a0, a1);
        if (thick < 1) thick = 1;
        const float kPi = 3.14159265f;
        // concentric 1px arcs give an exact thickness ...
        for (int16_t k = 0; k < thick; ++k) {
            float rr = float(r - k);
            int steps = int((a1 - a0) * kPi / 180.f * rr * 1.5f) + 2;
            for (int i = 0; i <= steps; ++i) {
                float a = (a0 + (a1 - a0) * float(i) / float(steps)) * kPi / 180.f;
                drawPixel(d, int16_t(lroundf(float(cx) + rr * sinf(a))),
                             int16_t(lroundf(float(cy) - rr * cosf(a))), col);
            }
        }
        // ... and discs at both ends give round caps
        if (thick >= 3) {
            float rm = float(r) - float(thick - 1) * 0.5f;
            int16_t cr = int16_t((thick - 1) / 2);
            for (float a : { a0, a1 }) {
                float ar = a * kPi / 180.f;
                fillCircle(int16_t(lroundf(float(cx) + rm * sinf(ar))),
                           int16_t(lroundf(float(cy) - rm * cosf(ar))), cr, col);
            }
        }
    }
    // Dithered annulus segment (a "grey" track for knobs / gauges).
    void ditherArc(int16_t cx, int16_t cy, int16_t rOut, int16_t rIn,
                   float a0, float a1, uint16_t col, uint8_t level = 2) const {
        const float kPi = 3.14159265f;
        float sweep = a1 - a0; if (sweep < 0) sweep += 360.f;
        for (int16_t yy = -rOut; yy <= rOut; ++yy)
            for (int16_t xx = -rOut; xx <= rOut; ++xx) {
                int32_t d2 = int32_t(xx)*xx + int32_t(yy)*yy;
                if (d2 > int32_t(rOut)*rOut || d2 < int32_t(rIn)*rIn) continue;
                if (!ditherOn(int16_t(cx+xx), int16_t(cy+yy), level)) continue;
                if (sweep < 359.9f) {
                    float a = atan2f(float(xx), float(-yy)) * 180.f / kPi;
                    if (a < 0) a += 360.f;
                    float off = a - a0; while (off < 0) off += 360.f; while (off >= 360.f) off -= 360.f;
                    if (off > sweep) continue;
                }
                drawPixel(d, int16_t(cx+xx), int16_t(cy+yy), col);
            }
    }

    // Card / button surface with the theme's hard shadow.
    // Returns the body rect (content area) — shifted when pressed.
    //   filled=true → body is solid fg (primary button); content colours must be swapped.
    Rect surface(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r,
                 uint16_t fg, uint16_t bg, bool pressed, bool filled = false,
                 bool border = true) const {
        int16_t s = th().shadow;
        if (w <= s * 2 || h <= s * 2) s = 0;
        int16_t bw = int16_t(w - s), bh = int16_t(h - s);
        if (r * 2 > bw) r = bw / 2;
        if (r * 2 > bh) r = bh / 2;
        if (pressed) {
            fillRoundRect(d, int16_t(x + s), int16_t(y + s), bw, bh, r, fg);
            return Rect(int16_t(x + s), int16_t(y + s), bw, bh);
        }
        if (s > 0 && border) fillRoundRect(d, int16_t(x + s), int16_t(y + s), bw, bh, r, fg);
        fillRoundRect(d, x, y, bw, bh, r, filled ? fg : bg);
        if (border) drawRoundRect(d, x, y, bw, bh, r, fg);
        return Rect(x, y, bw, bh);
    }

    // ---- icons -------------------------------------------------------------------
    bool drawVGByName(const char* name,
                      int16_t x, int16_t y, int16_t w, int16_t h,
                      uint16_t fg, uint16_t bg) const;
    // Square icon centred at (cx, cy).
    bool icon(const char* name, int16_t cx, int16_t cy, int16_t size,
              uint16_t fg, uint16_t bg) const {
        return drawVGByName(name, int16_t(cx - size/2), int16_t(cy - size/2), size, size, fg, bg);
    }

private:
    mutable Metrics      mCache_[TEXT_ROLES] = {};
    mutable const Theme* mCacheTheme_ = nullptr;
};

template <class DisplayT>
DrawCtx makeDrawCtx(DisplayT& disp);

// ============================================================================
//  Element
// ============================================================================
class Element : public Rect {
public:
    Element() = default;
    explicit Element(const char* label, uint16_t style=0,
                     uint16_t fixedW=0, uint16_t fixedH=0)
        : label_(label ? label : ""), style_(style),
          width_(fixedW), height_(fixedH) {}
    virtual ~Element() = default;

    // ---- fluent setters -------------------------------------------------------
    Element& setLabel   (const char* l){ label_=l?l:"";  return *this; }
    Element& setWidth   (uint16_t v)   { width_=v;        return *this; }
    Element& setHeight  (uint16_t v)   { height_=v;       return *this; }
    Element& setSize    (uint16_t pw, uint16_t ph){ width_=pw; height_=ph; return *this; }
    Element& setPadding (uint8_t  v)   { padding_=v;      return *this; }
    Element& setMargin  (uint8_t  v)   { margin_=v;       return *this; }
    Element& setSpacing (uint8_t  v)   { spacing_=v;      return *this; }
    Element& setRadius  (uint8_t  v)   { radius_=v; style_|=STYLE_ROUND_CORNER; return *this; }
    Element& setTextSize(uint8_t  v)   { textSize_=v;     return *this; }
    Element& addStyle   (uint16_t s)   { style_|=s;       return *this; }
    Element& clearStyle (uint16_t s)   { style_&=~s;      return *this; }

    Element& border()    { return addStyle(STYLE_BORDER);   }
    Element& noBorder()  { return clearStyle(STYLE_BORDER); }
    Element& inverted()  { return addStyle(STYLE_INVERTED); }
    Element& noFill()    { return addStyle(STYLE_NO_FILL);  }
    Element& disabled()  { return addStyle(STYLE_DISABLED); }
    Element& rounded(uint8_t r=4){ return setRadius(r); }
    Element& listRow()   { return addStyle(STYLE_LIST_ROW); }
    // Icon size for "&asset" labels (0 = auto: fits the element, max 24px)
    Element& setIconSize(uint8_t v){ iconSize_=v; return *this; }

    Element& displayBlock(){ style_=(style_&~(STYLE_DISPLAY_FLEX|STYLE_DISPLAY_FIXED))|STYLE_DISPLAY_BLOCK; return *this; }
    Element& displayFlex() { style_=(style_&~(STYLE_DISPLAY_BLOCK|STYLE_DISPLAY_FIXED))|STYLE_DISPLAY_FLEX; return *this; }
    Element& flexRow()     { return clearStyle(STYLE_VERTICAL); }
    Element& flexColumn()  { return addStyle(STYLE_VERTICAL);   }
    Element& flexWrap()    { return addStyle(STYLE_FLEX_WRAP);  }
    Element& justifyBetween(){ return addStyle(STYLE_JUSTIFY_SB); }
    Element& justifyCenter() { style_=(style_&~STYLE_JUSTIFY_END)|STYLE_JUSTIFY_CENTER; return *this; }
    Element& justifyEnd()    { style_=(style_&~STYLE_JUSTIFY_CENTER)|STYLE_JUSTIFY_END; return *this; }
    // Cross-axis alignment of this element inside a flex row/column
    Element& alignCenter()   { style2_=(style2_&~ALIGN_SELF_MASK)|ALIGN_SELF_CENTER; return *this; }
    Element& alignEnd()      { style2_=(style2_&~ALIGN_SELF_MASK)|ALIGN_SELF_END;    return *this; }

    // Horizontal text alignment
    Element& textLeft()   { style2_=(style2_&~TEXT_HALIGN_MASK)|TEXT_HALIGN_LEFT;   return *this; }
    Element& textCenter() { style2_=(style2_&~TEXT_HALIGN_MASK)|TEXT_HALIGN_CENTER; return *this; }
    Element& textRight()  { style2_=(style2_&~TEXT_HALIGN_MASK)|TEXT_HALIGN_RIGHT;  return *this; }
    // Vertical text alignment within the element height
    Element& textMiddle() { style2_=(style2_&~TEXT_VALIGN_MASK)|TEXT_VALIGN_MIDDLE; return *this; }
    Element& textTop()    { style2_=(style2_&~TEXT_VALIGN_MASK)|TEXT_VALIGN_TOP;    return *this; }
    Element& textBottom() { style2_=(style2_&~TEXT_VALIGN_MASK)|TEXT_VALIGN_BOTTOM; return *this; }
    Element& flexGrow(bool on=true) { if (on) style2_|=STYLE2_FLEX_GROW; else style2_&=~STYLE2_FLEX_GROW; return *this; }
    // Enable gesture handling on this element.
    // onGesture(ctx, sender, gestureId) fires when a gesture occurs while
    // no child has focus.  gestureId uses the GESTURE_* constants.
    Element& setOnGesture(GestureFn fn, void* gctx=nullptr) {
        cb().onGesture = fn;
        if (gctx) cb().ctx = gctx;
        return *this;
    }

    Callbacks& cb() { if (!cb_) cb_=std::make_unique<Callbacks>(); return *cb_; }
    bool hasCb() const { return cb_ != nullptr; }

    const std::string& label()  const { return label_;    }
    uint16_t  style()           const { return style_;    }
    uint8_t   style2()          const { return style2_;   }
    uint16_t  fixedW()          const { return width_;    }
    uint16_t  fixedH()          const { return height_;   }
    uint8_t   padding()         const { return padding_;  }
    uint8_t   margin()          const { return margin_;   }
    uint8_t   spacing()         const { return spacing_;  }
    uint8_t   textSize()        const { return textSize_; }

    void setLayoutW(uint16_t v) { layoutW_ = v; }

    // ---- touch hit testing ------------------------------------------------------
    //  Fingers are big and touch panels are imprecise near the edges, so
    //  einkui dispatches a press in two passes (see Parent::onTouch):
    //    1. exact element bounds
    //    2. if nothing was hit: bounds grown to kMinTouch px + kTouchSlop
    //  A release counts as "inside" with an even larger tolerance, so a
    //  slightly rolled finger still triggers the tap.
    static constexpr int16_t kMinTouch  = 44;   // minimum touch target (px)
    static constexpr int16_t kTouchSlop = 6;    // extra margin in pass 2
    static constexpr int16_t kLiftSlop  = 16;   // tolerance for the release point
    static bool& touchExpand() { static bool v = false; return v; }

    Rect touchRect(int16_t extra) const {
        Rect r(x, y, w, h);
        if (r.w < kMinTouch) { r.x = int16_t(r.x - (kMinTouch - r.w) / 2); r.w = kMinTouch; }
        if (r.h < kMinTouch) { r.y = int16_t(r.y - (kMinTouch - r.h) / 2); r.h = kMinTouch; }
        return r.expanded(extra);
    }
    // Press test (exact, or generous during the second pass)
    bool hitTest(uint16_t tx, uint16_t ty) const {
        if (touchExpand()) return touchRect(kTouchSlop).contains(int16_t(tx), int16_t(ty));
        return contains(int16_t(tx), int16_t(ty));
    }
    // Release test: generous; (0,0) = controller sent no coordinates → accept
    bool releaseHit(uint16_t tx, uint16_t ty) const {
        if (tx == 0 && ty == 0) return true;
        return touchRect(kLiftSlop).contains(int16_t(tx), int16_t(ty));
    }
    // Elements whose look changes while the finger moves (sliders, knobs …)
    // return true so UI redraws them on Contact frames.  Plain buttons don't,
    // which avoids slow e-paper refreshes while the finger rests.
    virtual bool redrawOnMove() const { return false; }
    // Called by UI when a touch on this element was abandoned (its release
    // got lost).  Reset pressed/drag state here; never fire actions.
    virtual void cancelTouch() {}
    void setLayoutH(uint16_t v) { layoutH_ = v; }

    // ---- virtuals ------------------------------------------------------------
    virtual Rect measure(DrawCtx& ctx, Rect cursor, Rect area, uint16_t pStyle);
    virtual void draw   (DrawCtx& ctx, Colors colors);
    virtual bool onTouch(DrawCtx& ctx, const TTouchFrame& tf,
                         std::shared_ptr<Element>& focused);

    void drawBackground(DrawCtx& ctx, Colors c) const;
    void drawLabel     (DrawCtx& ctx, Colors c) const;
    // Draw label/icon into an explicit box (used by elements with custom chrome)
    void drawLabelIn   (DrawCtx& ctx, Rect box, uint16_t fg, uint16_t bg) const;
    // Dotted divider along the bottom edge when STYLE_LIST_ROW is set
    void drawRowDivider(DrawCtx& ctx, Colors c) const {
        if (style_ & STYLE_LIST_ROW) ctx.hairline(x, int16_t(y + h - 1), w, c.fg);
    }
    // Resolve the icon size for an "&asset" label inside a box of inner size
    int16_t iconSizeFor(int16_t innerW, int16_t innerH) const {
        if (iconSize_) return iconSize_;
        int16_t s = std::min<int16_t>(innerW, innerH);
        return std::min<int16_t>(s, 24);
    }

protected:
    void placeAt(Rect cursor, uint16_t /*pStyle*/) {
        x = cursor.x + (int16_t)margin_;
        y = cursor.y + (int16_t)margin_;
    }

    // Resolve the width to use during measure():
    //   user fixed > layout suggestion > fill area
    int16_t resolveW(Rect area) const {
        if (width_   > 0) return (int16_t)width_;
        if (layoutW_ > 0) return (int16_t)layoutW_;
        return std::max<int16_t>(1, (int16_t)(area.w - (x - area.x)));
    }
    // Resolve height: user fixed > layout suggestion > fit content
    int16_t resolveH(int16_t contentH) const {
        if (height_  > 0) return (int16_t)height_;
        if (layoutH_ > 0) return (int16_t)layoutH_;
        return contentH;
    }

    Colors resolveColors(Colors base) const {
        if (style_ & STYLE_INVERTED) std::swap(base.fg, base.bg);
        return base;
    }

    std::string label_    = "";
    uint16_t    style_    = 0;
    uint8_t     style2_   = 0;
    uint16_t    width_    = 0;
    uint16_t    height_   = 0;
    uint16_t    layoutW_  = 0;
    uint16_t    layoutH_  = 0;
    uint8_t     padding_  = 2;
    uint8_t     margin_   = 1;
    uint8_t     spacing_  = 0;
    uint8_t     radius_   = 0;
    uint8_t     textSize_ = 1;   // text role, see TEXT_BODY …
    uint8_t     iconSize_ = 0;

    std::unique_ptr<Callbacks> cb_;
};

// ---- Element implementations ------------------------------------------------

inline Rect Element::measure(DrawCtx& ctx, Rect cursor, Rect area, uint16_t pStyle) {
    if (style_ & STYLE_DISPLAY_FIXED) return Rect(x,y,w,h);
    placeAt(cursor, pStyle);
    int16_t th = ctx.metrics(textSize_).line;
    w = resolveW(area);
    h = resolveH((int16_t)(std::max<int16_t>(th, 4) + padding_*2));
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    return Rect(x,y,w,h);
}

inline void Element::draw(DrawCtx& ctx, Colors colors) {
    if (style_ & STYLE_DISABLED) return;
    Colors c = resolveColors(colors);
    drawBackground(ctx, c);
    drawLabel(ctx, c);
    drawRowDivider(ctx, c);
}

inline bool Element::onTouch(DrawCtx& /*ctx*/, const TTouchFrame& tf,
                               std::shared_ptr<Element>& focused)
{
    if (!hasCb()) return false;
    uint8_t  ev = (uint8_t)tf.p[0].event;
    uint16_t tx = tf.p[0].x, ty = tf.p[0].y;
    Callbacks& cb = *cb_;

    if (ev == 0 && (cb.onTouchDown || cb.onTouchUp || cb.onTouchMove) && hitTest(tx, ty)) {
        focused = std::shared_ptr<Element>(this, [](Element*){});
        if (cb.onTouchDown) cb.onTouchDown(cb.ctx, this);
        return true;
    }
    if (ev == 1 && focused.get() == this) {
        focused = nullptr;
        if (cb.onTouchUp && releaseHit(tx, ty)) cb.onTouchUp(cb.ctx, this);
        return true;
    }
    if (ev == 2 && focused.get() == this) {
        if (cb.onTouchMove) cb.onTouchMove(cb.ctx, this);
        return true;
    }
    return false;
}

inline void Element::drawBackground(DrawCtx& ctx, Colors c) const {
    if (!(style_ & STYLE_NO_FILL)) {
        if (style_ & STYLE_ROUND_CORNER) ctx.fillRoundRect(ctx.d,x,y,w,h,radius_,c.bg);
        else                              ctx.fillRect     (ctx.d,x,y,w,h,c.bg);
    }
    if (style_ & STYLE_BORDER) {
        if (style_ & STYLE_ROUND_CORNER) ctx.drawRoundRect(ctx.d,x,y,w,h,radius_,c.fg);
        else                              ctx.drawRect     (ctx.d,x,y,w,h,c.fg);
    }
}

inline void Element::drawLabel(DrawCtx& ctx, Colors c) const {
    if (label_.empty() || (style_ & (STYLE_HIDE_LABEL|STYLE_DISABLED))) return;
    Rect box(int16_t(x + padding_), int16_t(y + padding_),
             int16_t(w - padding_*2), int16_t(h - padding_*2));
    drawLabelIn(ctx, box, c.fg, c.bg);
}

// Label rendering shared by all elements:
//   "Text"              → text, aligned by TEXT_HALIGN / TEXT_VALIGN
//   "&asset"            → VG icon centred in the box
//   "&asset Caption"    → icon + caption side by side (centred as a group
//                         when TEXT_HALIGN_CENTER, else left aligned)
inline void Element::drawLabelIn(DrawCtx& ctx, Rect box, uint16_t fg, uint16_t bg) const {
    if (label_.empty() || box.w <= 0 || box.h <= 0) return;
    const uint8_t halign = style2_ & TEXT_HALIGN_MASK;
    const uint8_t valign = style2_ & TEXT_VALIGN_MASK;

    if (label_[0] != '&') {
        ctx.textBox(box.x, box.y, box.w, box.h, label_.c_str(), textSize_, fg, halign, valign);
        return;
    }

    // ---- VG icon label ---------------------------------------------------------
    const char* rest  = label_.c_str() + 1;
    const char* space = strchr(rest, ' ');
    char assetName[32];
    const char* caption = nullptr;
    size_t len = space ? size_t(space - rest) : strlen(rest);
    len = std::min<size_t>(len, sizeof(assetName) - 1);
    memcpy(assetName, rest, len); assetName[len] = '\0';
    if (space && space[1]) caption = space + 1;

    int16_t isz = iconSizeFor(box.w, box.h);
    if (isz < 4) isz = 4;
    int16_t cy = int16_t(box.y + box.h / 2);

    if (!caption) {
        int16_t cx = (halign == TEXT_HALIGN_LEFT && (style_ & STYLE_LIST_ROW))
                   ? int16_t(box.x + isz / 2) : int16_t(box.x + box.w / 2);
        ctx.icon(assetName, cx, cy, isz, fg, bg);
        return;
    }
    const int16_t gap = 6;
    int16_t tw = ctx.textWidth(caption, textSize_);
    int16_t groupW = int16_t(isz + gap + tw);
    if (groupW > box.w) { groupW = box.w; tw = int16_t(box.w - isz - gap); }
    int16_t gx = (halign == TEXT_HALIGN_CENTER) ? int16_t(box.x + (box.w - groupW) / 2)
               : (halign == TEXT_HALIGN_RIGHT)  ? int16_t(box.x + box.w - groupW)
               : box.x;
    ctx.icon(assetName, int16_t(gx + isz / 2), cy, isz, fg, bg);
    ctx.textBox(int16_t(gx + isz + gap), box.y, tw, box.h, caption, textSize_, fg,
                TEXT_HALIGN_LEFT, valign);
}

// ============================================================================
//  Parent
// ============================================================================
class Parent : public Element {
public:
    // Parent is a layout container — it never draws its own label.
    // STYLE_HIDE_LABEL is forced on here so internal names like "hdr", "body",
    // "files" never appear rendered on screen.  Use Card or TextDisplay if you
    // need a visible title on a container.
    Parent() : Element("", STYLE_DISPLAY_FLEX | STYLE_VERTICAL | STYLE_HIDE_LABEL) {}
    explicit Parent(const char* label,
                    uint16_t style = STYLE_DISPLAY_FLEX | STYLE_VERTICAL)
        : Element(label, style | STYLE_HIDE_LABEL) {}

    template <class ElemT>
    std::shared_ptr<ElemT> add(ElemT* e) {
        auto sp = std::shared_ptr<ElemT>(e);
        children_.push_back(sp);
        return sp;
    }
    void add(std::shared_ptr<Element> e) { if(e) children_.push_back(std::move(e)); }

    template <class ElemT>
    ElemT* ptr(ElemT* e) {
        children_.push_back(std::shared_ptr<ElemT>(e));
        return e;
    }

    void remove(Element* e) {
        children_.erase(std::remove_if(children_.begin(), children_.end(),
            [e](const std::shared_ptr<Element>& s){ return s.get()==e; }),
            children_.end());
    }
    void clearChildren() { children_.clear(); }
    const std::vector<std::shared_ptr<Element>>& children() const { return children_; }
    size_t childCount() const { return children_.size(); }

    Rect measure(DrawCtx& ctx, Rect cursor, Rect area, uint16_t pStyle) override;
    void draw   (DrawCtx& ctx, Colors colors)                            override;
    bool onTouch(DrawCtx& ctx, const TTouchFrame& tf,
                 std::shared_ptr<Element>& focused)                      override;

protected:
    std::vector<std::shared_ptr<Element>> children_;

private:
    void layoutChildrenFlex(DrawCtx& ctx, Rect inner);
    void layoutChildren    (DrawCtx& ctx, Rect inner);
};

inline Rect Parent::measure(DrawCtx& ctx, Rect cursor, Rect area, uint16_t pStyle) {
    if (!(style_ & STYLE_DISPLAY_FIXED)) {
        placeAt(cursor, pStyle);
        w = resolveW(area);
        // Height: use fixed value if set, else layout hint, else 0 (wrap to content)
        if      (height_  > 0) h = (int16_t)height_;
        else if (layoutH_ > 0) h = (int16_t)layoutH_;
        else                   h = 0;
    }

    const bool vert = (style_ & STYLE_VERTICAL) != 0;

    Rect inner;
    inner.x = x + (int16_t)padding_;
    inner.y = y + (int16_t)padding_;
    inner.w = std::max<int16_t>(0, w - (int16_t)padding_*2);

    // inner.h for layout:
    //   - Fixed height (height_>0 or layoutH_>0): use exactly that minus padding.
    //   - Auto height (wraps content): give children the remaining available area
    //     so they can fill it, but ONLY for vertical containers.  A horizontal
    //     container with no fixed height wraps its tallest child, so children
    //     should NOT get a cross-axis hint derived from the whole screen height —
    //     that would make every auto-height TextDisplay fill the screen.
    if (h > 0) {
        inner.h = std::max<int16_t>(0, h - (int16_t)padding_*2);
    } else if (vert) {
        // Vertical auto-height: pass remaining screen space so flex-grow works
        // (area.y2() - inner.y, not area.h - inner.y: the latter subtracted
        //  the status-bar height twice and cut pages short.)
        inner.h = std::max<int16_t>(0, int16_t(area.y2() - inner.y - (int16_t)padding_));
    } else {
        // Horizontal auto-height: children determine their own height from content.
        // Use 0 so the cross-axis hint inside layoutChildrenFlex falls back to
        // each child's own fixed/content height.
        inner.h = 0;
    }

    layoutChildren(ctx, inner);

    // Wrap height to content when not fixed
    if (height_ == 0 && layoutH_ == 0) {
        h = 0;
        for (auto& c : children_)
            h = std::max<int16_t>(h,
                c->y + c->h + (int16_t)c->margin() + (int16_t)padding_ - y);
        if (h < 1) h = 1;
    }
    return Rect(x,y,w,h);
}

inline void Parent::draw(DrawCtx& ctx, Colors colors) {
    if (style_ & STYLE_DISABLED) return;
    Colors c = resolveColors(colors);
    drawBackground(ctx, c);
    drawLabel(ctx, c);
    for (auto& child : children_) child->draw(ctx, c);
}

inline bool Parent::onTouch(DrawCtx& ctx, const TTouchFrame& tf,
                              std::shared_ptr<Element>& focused)
{
    // depth guard: the enlarged second hit pass runs only at the top level
    static int depth = 0;
    struct Depth { Depth(){ ++depth; } ~Depth(){ --depth; } } guard;

    uint8_t ev = (uint8_t)tf.p[0].event;

    if (focused && (ev == 1 || ev == 2)) {
        focused->onTouch(ctx, tf, focused);
        return true;
    }

    // Gesture events (ev==0 with gestureId set, no focused element):
    // Route to the first child that has onGesture set.
    if (ev == 0 && !focused && tf.gestureId) {
        for (auto& child : children_) {
            if (child->hasCb() && child->cb().onGesture) {
                child->cb().onGesture(child->cb().ctx, child.get(), tf.gestureId);
                return true;
            }
        }
        // Also check self
        if (hasCb() && cb_->onGesture) {
            cb_->onGesture(cb_->ctx, this, tf.gestureId);
            return true;
        }
    }

    for (auto it = children_.rbegin(); it != children_.rend(); ++it) {
        if ((*it)->style() & STYLE_DISABLED) continue;
        if ((*it)->onTouch(ctx, tf, focused)) return true;
    }
    if (Element::onTouch(ctx, tf, focused)) return true;

    // Second pass for presses that hit nothing exactly: try again with
    // enlarged touch targets (only at the outermost level, once).
    if (ev == 0 && !touchExpand() && depth == 1) {
        touchExpand() = true;
        bool hit = false;
        for (auto it = children_.rbegin(); it != children_.rend() && !hit; ++it) {
            if ((*it)->style() & STYLE_DISABLED) continue;
            hit = (*it)->onTouch(ctx, tf, focused);
        }
        touchExpand() = false;
        return hit;
    }
    return false;
}

inline void Parent::layoutChildren(DrawCtx& ctx, Rect inner) {
    layoutChildrenFlex(ctx, inner);
}

inline void Parent::layoutChildrenFlex(DrawCtx& ctx, Rect inner) {
    const bool    vert = (style_ & STYLE_VERTICAL) != 0;
    const int16_t sp   = (int16_t)spacing_;
    const size_t  n    = children_.size();
    if (n == 0) return;

    int16_t cx = inner.x;
    int16_t cy = inner.y;

    struct Info {
        std::shared_ptr<Element> el;
        int16_t mainSize;   // h if vert, w if horiz — measured natural size incl margins
        bool    grow;
    };
    std::vector<Info> infos;
    infos.reserve(n);

    int16_t totalFixed = 0;  // sum of (mainSize + spacing) for non-grow children
    int16_t growCount  = 0;

    for (size_t i = 0; i < n; ++i) {
        auto& c = children_[i];
        int16_t m = (int16_t)c->margin();

        // Set cross-axis hint so children can fill the cross axis.
        // For vertical containers:  children get the full inner width.
        // For horizontal containers: only push a height hint when inner.h > 0
        //   (i.e. the container has a known height).  When inner.h == 0 the
        //   container is auto-height and children must size from their own
        //   content — we must NOT set layoutH here or they balloon to 0→content
        //   incorrectly on a second measure pass.
        if (vert) {
            int16_t crossAvail = std::max<int16_t>(0, inner.w - m*2);
            c->setLayoutW((uint16_t)crossAvail);
        } else {
            if (inner.h > 0) {
                int16_t crossAvail = std::max<int16_t>(0, inner.h - m*2);
                c->setLayoutH((uint16_t)crossAvail);
            }
            // inner.h == 0 → leave layoutH_ at whatever was set (fixed) or 0 (auto)
        }

        // Probe-measure to get natural main-axis size
        Rect probeCursor(cx, cy, 0, 0);
        c->measure(ctx, probeCursor, inner, style_);

        bool grow = (c->style2() & STYLE2_FLEX_GROW) != 0;

        // For grow children the natural size is irrelevant to the distribution
        // formula — record 0 so it doesn't inflate totalFixed or grownContent.
        // For fixed children record the actual measured size.
        int16_t mainSize = grow ? 0
                                : (vert ? c->h : c->w) + m * 2;

        infos.push_back({c, mainSize, grow});

        if (grow) {
            ++growCount;
        } else {
            totalFixed += mainSize;
        }
        // Spacing between ALL children pairs (n-1 gaps total)
        if (i + 1 < n) totalFixed += sp;
    }

    // Distribute remaining space to flex-grow children
    int16_t mainAvail = vert ? inner.h : inner.w;
    int16_t extra     = 0;
    int16_t remaining = int16_t(mainAvail - totalFixed);
    if (growCount > 0) {
        if (remaining > 0) extra = remaining / growCount;
        if (extra < 0) extra = 0;
    }

    // justify-content (only meaningful when nothing grows and space is left)
    int16_t lead = 0, between = 0;
    if (growCount == 0 && remaining > 0 && mainAvail > 0) {
        const uint16_t j = style_ & STYLE_JUSTIFY_SB;
        if      (j == STYLE_JUSTIFY_SB && n > 1) between = int16_t(remaining / int16_t(n - 1));
        else if (j == STYLE_JUSTIFY_CENTER)      lead    = int16_t(remaining / 2);
        else if (j == STYLE_JUSTIFY_END)         lead    = remaining;
    }
    if (vert) cy = int16_t(cy + lead); else cx = int16_t(cx + lead);

    // Place each child at its final position
    for (size_t i = 0; i < infos.size(); ++i) {
        auto& info = infos[i];
        auto& c    = info.el;
        int16_t m  = (int16_t)c->margin();

        if (info.grow && mainAvail > 0) {
            // grow children share the free space; with none left they shrink
            // to 1px instead of pushing siblings off-screen
            int16_t grownContent = std::max<int16_t>(1, int16_t((info.mainSize - m*2) + extra));
            if (vert) c->setLayoutH((uint16_t)grownContent);
            else      c->setLayoutW((uint16_t)grownContent);
        }

        // Cursor points to slot top-left BEFORE margin
        Rect placeCursor(cx, cy, 0, 0);
        c->measure(ctx, placeCursor, inner, style_);

        // align-self on the cross axis (center / end) for children that are
        // smaller than the container — measured again at the shifted cursor so
        // nested containers lay out their own children correctly.
        uint8_t as = c->style2() & ALIGN_SELF_MASK;
        if (as == ALIGN_SELF_CENTER || as == ALIGN_SELF_END) {
            int16_t crossAvail = vert ? inner.w : inner.h;
            int16_t crossUsed  = int16_t((vert ? c->w : c->h) + m * 2);
            int16_t off = int16_t(crossAvail - crossUsed);
            if (as == ALIGN_SELF_CENTER) off = int16_t(off / 2);
            if (off > 0 && crossAvail > 0) {
                Rect shifted = vert ? Rect(int16_t(cx + off), cy, 0, 0) : Rect(cx, int16_t(cy + off), 0, 0);
                c->measure(ctx, shifted, inner, style_);
            }
        }

        // Advance cursor past rendered size + both margins + spacing
        int16_t used = (vert ? c->h : c->w) + m * 2;
        if (i + 1 < infos.size()) used = int16_t(used + sp + between);
        if (vert) cy += used;
        else      cx += used;
    }
}

} // namespace einkui

// ============================================================================
//  makeDrawCtx
// ============================================================================
#include "vg_assets.h"
#include "eink_vg.h"

namespace einkui {

// UTF-8 → Latin-1 (ISO-8859-1).  Bytes that are not valid UTF-8 are copied
// unchanged, so passing an already-Latin-1 string is harmless.  Code points
// above U+00FF become '?'.  Output is truncated to len-1 bytes.
inline const char* utf8ToLatin1(const char* s, char* buf, size_t len) {
    if (!s) { buf[0] = 0; return buf; }
    size_t o = 0;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(s);
    while (*p && o + 1 < len) {
        uint8_t ch = *p;
        if (ch < 0x80) { buf[o++] = char(ch); ++p; continue; }
        if ((ch & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
            uint16_t cp = uint16_t(((ch & 0x1F) << 6) | (p[1] & 0x3F));
            buf[o++] = cp <= 0xFF ? char(cp) : '?'; p += 2; continue;
        }
        if ((ch & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
            buf[o++] = '?'; p += 3; continue;
        }
        if ((ch & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80) {
            buf[o++] = '?'; p += 4; continue;
        }
        buf[o++] = char(ch); ++p;   // stray byte: pass through (Latin-1 input)
    }
    buf[o] = 0;
    return buf;
}

inline bool DrawCtx::drawVGByName(const char* name,
                                   int16_t bx, int16_t by, int16_t bw, int16_t bh,
                                   uint16_t fg, uint16_t bg) const {
    if (!drawVG) return false;
    for (size_t i = 0; i < size_t(vg_assets_count); ++i) {
        if (strcmp(vg_assets[i].name, name) == 0) {
            drawVG(d, vg_assets[i].data, bx, by, bw, bh, fg, bg);
            return true;
        }
    }
    return false;
}

template <class DisplayT>
DrawCtx makeDrawCtx(DisplayT& disp) {
    DrawCtx c;
    c.d = &disp;
    c.width  = [](void* p){ return static_cast<DisplayT*>(p)->width();  };
    c.height = [](void* p){ return static_cast<DisplayT*>(p)->height(); };
    c.fillScreen    = [](void* p, uint16_t col)
        { static_cast<DisplayT*>(p)->fillScreen(col); };
    c.fillRect      = [](void* p, int16_t x, int16_t y, int16_t w, int16_t h, uint16_t col)
        { static_cast<DisplayT*>(p)->fillRect(x,y,w,h,col); };
    c.drawRect      = [](void* p, int16_t x, int16_t y, int16_t w, int16_t h, uint16_t col)
        { static_cast<DisplayT*>(p)->drawRect(x,y,w,h,col); };
    c.fillRoundRect = [](void* p, int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, uint16_t col)
        { static_cast<DisplayT*>(p)->fillRoundRect(x,y,w,h,r,col); };
    c.drawRoundRect = [](void* p, int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, uint16_t col)
        { static_cast<DisplayT*>(p)->drawRoundRect(x,y,w,h,r,col); };
    c.drawLine      = [](void* p, int16_t x0,int16_t y0,int16_t x1,int16_t y1, uint16_t col)
        { static_cast<DisplayT*>(p)->drawLine(x0,y0,x1,y1,col); };
    c.drawFastHLine = [](void* p, int16_t x, int16_t y, int16_t w, uint16_t col)
        { static_cast<DisplayT*>(p)->drawFastHLine(x,y,w,col); };
    c.drawFastVLine = [](void* p, int16_t x, int16_t y, int16_t h, uint16_t col)
        { static_cast<DisplayT*>(p)->drawFastVLine(x,y,h,col); };
    c.drawPixel     = [](void* p, int16_t x, int16_t y, uint16_t col)
        { static_cast<DisplayT*>(p)->drawPixel(x,y,col); };
    c.setTextSize   = [](void* p, uint8_t s)
        { static_cast<DisplayT*>(p)->setTextSize(s); };
    c.setTextColor  = [](void* p, uint16_t col)
        { static_cast<DisplayT*>(p)->setTextColor(col); };
    c.setCursor     = [](void* p, int16_t x, int16_t y)
        { static_cast<DisplayT*>(p)->setCursor(x,y); };
    // Text goes through utf8ToLatin1() so "Größe", "°C", "ä" render with the
    // Latin-1 glyphs of the bundled fonts (and measure correctly).
    c.printStr      = [](void* p, const char* s) {
        char buf[128]; const char* t = utf8ToLatin1(s, buf, sizeof(buf));
        auto* dp = static_cast<DisplayT*>(p);
        for (const char* q = t; *q; ++q) dp->write(uint8_t(*q));
    };
    c.printChar     = [](void* p, char ch)
        { static_cast<DisplayT*>(p)->write(uint8_t(ch)); };
    c.getTextBounds = [](void* p, const char* s, int16_t* x1,int16_t* y1, uint16_t* w, uint16_t* h) {
        char buf[128]; const char* t = utf8ToLatin1(s, buf, sizeof(buf));
        static_cast<DisplayT*>(p)->getTextBounds(t, 0, 0, x1, y1, w, h);
    };
    c.setFont       = [](void* p, const GFXfont* f)
        { static_cast<DisplayT*>(p)->setFont(f); };
    c.drawVG        = [](void* p, const uint8_t* data,
                          int16_t x, int16_t y, int16_t w, int16_t h,
                          uint16_t fg, uint16_t bg)
        { drawVG_Q1(*static_cast<DisplayT*>(p), data, x, y, w, h, fg, bg); };
    return c;
}

} // namespace einkui