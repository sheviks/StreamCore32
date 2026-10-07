#pragma once
// ============================================================================
//  einkui/elements/widgets.h
//
//  Additional visually-rich elements for einkui.
//
//  ProgressArc   — circular progress gauge (dotted track, solid round-capped arc)
//  Badge         — small pill label, e.g. notification count or status chip
//  Card          — rounded container with shadow and an optional title
//  Sparkline     — inline mini bar-chart or line graph from a float array
//  Divider       — horizontal rule / section header, or vertical rule
//  Meter         — segmented level meter (signal, battery, …)
// ============================================================================
#include "../include/element.h"
#include <cstdio>
#include <cstring>
#include <cmath>

namespace einkui {

// ============================================================================
//  ProgressArc
//  value in [0..1].  Sized to min(w,h).  The label (e.g. "72%") is drawn in
//  the centre in the title font, an optional caption below it.
//
//    auto* arc = page->add(new ProgressArc(0.72f, 56, 56)).get();
//    arc->setLabel("72%");
//    arc->setCaption("CPU");
// ============================================================================
class ProgressArc : public Element {
public:
    explicit ProgressArc(float value=0.f, uint16_t fixedW=48, uint16_t fixedH=48,
                         uint8_t thickness=5)
        : Element("", 0, fixedW, fixedH), value_(value), thickness_(thickness) {}

    ProgressArc& setValue(float v) { value_ = v<0.f?0.f:(v>1.f?1.f:v); return *this; }
    ProgressArc& setLabel(const char* l){ label_=l; return *this; }
    ProgressArc& setCaption(const char* c){ caption_=c ? c : ""; return *this; }
    ProgressArc& setThickness(uint8_t t){ thickness_=t; return *this; }
    float value() const { return value_; }

    Rect measure(DrawCtx& ctx, Rect cursor, Rect area, uint16_t pStyle) override {
        (void)ctx;
        if (style_ & STYLE_DISPLAY_FIXED) return Rect(x,y,w,h);
        placeAt(cursor, pStyle);
        w = resolveW(area);
        h = resolveH(int16_t(height_ > 0 ? height_ : width_ > 0 ? width_ : 48));
        if (w < 1) w = 1;
        if (h < 1) h = 1;
        return Rect(x,y,w,h);
    }

    void draw(DrawCtx& ctx, Colors colors) override {
        if (style_ & STYLE_DISABLED) return;
        Colors c = resolveColors(colors);
        drawBackground(ctx, c);

        int16_t cx = int16_t(x + w/2);
        int16_t cy = int16_t(y + h/2);
        int16_t r  = int16_t((std::min(w,h) - 2) / 2);
        if (r < 6) return;
        int16_t t = std::min<int16_t>(thickness_, int16_t(r / 2));

        ctx.ditherArc(cx, cy, r, int16_t(r - t + 1), 0.f, 360.f, c.fg, 2);
        if (value_ > 0.001f) ctx.arc(cx, cy, r, t, 0.f, value_ * 360.f, c.fg);

        // biggest font whose label fits inside the ring
        uint8_t role = TEXT_CAPTION;
        int16_t room = int16_t(2 * (r - t) - 6);
        for (uint8_t cand : { TEXT_DISPLAY, TEXT_TITLE, TEXT_BOLD })
            if (ctx.textWidth(label_.c_str(), cand) <= room &&
                ctx.metrics(cand).cap * 2 <= room) { role = cand; break; }
        if (!label_.empty()) {
            int16_t bh = ctx.metrics(role).cap;
            int16_t by = caption_.empty() ? int16_t(cy - bh/2) : int16_t(cy - bh + 1);
            ctx.textBox(int16_t(cx - r + t), by, int16_t(2*(r - t)), bh, label_.c_str(), role, c.fg,
                        TEXT_HALIGN_CENTER, TEXT_VALIGN_TOP, false);
        }
        if (!caption_.empty()) {
            ctx.textBox(int16_t(cx - r + t), int16_t(cy + 3), int16_t(2*(r - t)), ctx.metrics(TEXT_CAPTION).cap,
                        caption_.c_str(), TEXT_CAPTION, c.fg, TEXT_HALIGN_CENTER, TEXT_VALIGN_TOP);
        }
    }

private:
    float       value_;
    uint8_t     thickness_;
    std::string caption_;
};

// ============================================================================
//  Badge — pill chip.  Outline by default, ->inverted() for a filled chip.
//
//    page->add(new Badge("NEW",  0, 16));
//    page->add(new Badge("3",    0, 16))->inverted();
// ============================================================================
class Badge : public Element {
public:
    explicit Badge(const char* text="", uint16_t fixedW=0, uint16_t fixedH=16)
        : Element(text, STYLE_BORDER | STYLE_ROUND_CORNER, fixedW, fixedH)
    {
        radius_  = 8;
        padding_ = 6;
        margin_  = 1;
        textSize_ = TEXT_CAPTION;
    }

    Rect measure(DrawCtx& ctx, Rect cursor, Rect area, uint16_t pStyle) override {
        if (style_ & STYLE_DISPLAY_FIXED) return Rect(x,y,w,h);
        placeAt(cursor, pStyle);
        int16_t tw = ctx.textWidth(label_.c_str(), textSize_);
        h = resolveH(int16_t(ctx.metrics(textSize_).cap + 8));
        w = resolveW(area);
        if (width_==0 && layoutW_==0) w = int16_t(tw + padding_*2);
        if (w < h) w = h;  // at least round
        radius_ = uint8_t(h/2);
        return Rect(x,y,w,h);
    }

    void draw(DrawCtx& ctx, Colors colors) override {
        if (style_ & STYLE_DISABLED) return;
        Colors c = resolveColors(colors);
        ctx.fillRoundRect(ctx.d, x, y, w, h, radius_, c.bg);
        ctx.drawRoundRect(ctx.d, x, y, w, h, radius_, c.fg);
        ctx.textBox(x, y, w, h, label_.c_str(), textSize_, c.fg,
                    TEXT_HALIGN_CENTER, TEXT_VALIGN_MIDDLE, false);
    }
};

// ============================================================================
//  Card — rounded container with the theme's hard shadow.
//  If a title is given it is drawn bold in the top-left with a dotted rule
//  under it; children go into body().
//
//    auto* card = page->add(new Card("Track Info", 0, 60)).get();
//    card->body()->add(new TextDisplay("Pink Floyd"));
// ============================================================================
class Card : public Parent {
public:
    explicit Card(const char* title="", uint16_t fixedW=0, uint16_t fixedH=0)
        : Parent(title, STYLE_DISPLAY_FLEX | STYLE_VERTICAL | STYLE_BORDER | STYLE_ROUND_CORNER)
    {
        width_=fixedW; height_=fixedH;
        radius_=8; padding_=0; spacing_=0;
        showHeader_ = (title && title[0] != '\0');
    }

    Parent* body() {
        if (!body_) buildBody();
        return body_;
    }

    Card& setHeaderHeight(uint8_t h){ headerH_=h; return *this; }
    Card& hideHeader()              { showHeader_=false; return *this; }

    Rect measure(DrawCtx& ctx, Rect cursor, Rect area, uint16_t pStyle) override {
        if (!body_) buildBody();
        int16_t s = ctx.th().shadow;
        // header spacer + shadow room are handled through the body's margins
        headerSpace_->setHeight(showHeader_ && !label_.empty() ? headerH_ : 0);
        body_->setMargin(0);
        // Body stretches only when the card itself has a fixed height;
        // otherwise the card wraps its content (+ shadow).
        body_->flexGrow(height_ > 0 || layoutH_ > 0);
        padding_ = 0;
        Rect r = Parent::measure(ctx, cursor, area, pStyle);
        if (height_ == 0 && layoutH_ == 0) { h = int16_t(h + s); r.h = h; }
        return r;
    }

    void draw(DrawCtx& ctx, Colors colors) override {
        if (style_ & STYLE_DISABLED) return;
        Colors c = resolveColors(colors);
        Rect body = ctx.surface(x, y, w, h, radius_, c.fg, c.bg, false);
        if (showHeader_ && !label_.empty()) {
            ctx.textBox(int16_t(body.x + 8), body.y, int16_t(body.w - 16), headerH_,
                        label_.c_str(), TEXT_BOLD, c.fg);
            ctx.hairline(int16_t(body.x + 8), int16_t(body.y + headerH_ - 1), int16_t(body.w - 16), c.fg);
        }
        for (auto& child : children_) child->draw(ctx, c);
    }

private:
    Parent*  body_        = nullptr;
    Element* headerSpace_ = nullptr;
    bool     showHeader_  = true;
    uint8_t  headerH_     = 18;

    void buildBody() {
        auto* hs = new Element("", STYLE_NO_FILL | STYLE_HIDE_LABEL, 0, headerH_);
        hs->setMargin(0);
        headerSpace_ = hs;
        children_.push_back(std::shared_ptr<Element>(hs));
        auto* b = new Parent("", STYLE_DISPLAY_FLEX | STYLE_VERTICAL | STYLE_NO_FILL);
        b->setPadding(5).setSpacing(0).flexGrow();
        body_ = b;
        children_.push_back(std::shared_ptr<Parent>(b));
    }
};

// ============================================================================
//  Sparkline — compact bar or line chart from a raw float array (no copy).
//
//    static float history[32] = { ... };
//    auto* sp = page->add(new Sparkline(history, 32, 0,100, 60, 24)).get();
//    sp->setMode(Sparkline::Mode::Line);
// ============================================================================
class Sparkline : public Element {
public:
    enum class Mode { Bar, Line };

    Sparkline(const float* data, uint16_t count,
              float minV=0.f, float maxV=1.f,
              uint16_t fixedW=60, uint16_t fixedH=20)
        : Element("", 0, fixedW, fixedH),
          data_(data), count_(count), minV_(minV), maxV_(maxV) {}

    Sparkline& setMode(Mode m)             { mode_=m; return *this; }
    Sparkline& setLabel(const char* l)     { label_=l; return *this; }
    Sparkline& setRange(float lo, float hi){ minV_=lo; maxV_=hi; return *this; }

    void setData(const float* d, uint16_t count){ data_=d; count_=count; }

    Rect measure(DrawCtx& ctx, Rect cursor, Rect area, uint16_t pStyle) override {
        (void)ctx;
        if (style_ & STYLE_DISPLAY_FIXED) return Rect(x,y,w,h);
        placeAt(cursor, pStyle);
        w = resolveW(area);
        h = resolveH(int16_t(height_ > 0 ? height_ : 24));
        if (w<1) w=1;
        if (h<1) h=1;
        return Rect(x,y,w,h);
    }

    void draw(DrawCtx& ctx, Colors colors) override {
        if (style_ & STYLE_DISABLED) return;
        Colors c = resolveColors(colors);
        drawBackground(ctx, c);
        if (!data_ || count_ == 0) return;

        int16_t topPad = 0;
        if (!label_.empty()) {
            int16_t cap = ctx.metrics(TEXT_CAPTION).cap;
            ctx.textBox(x, y, w, cap, label_.c_str(), TEXT_CAPTION, c.fg, TEXT_HALIGN_LEFT, TEXT_VALIGN_TOP);
            topPad = int16_t(cap + 4);
        }
        Rect pa(x, int16_t(y + topPad), w, int16_t(h - topPad - 2));
        if (pa.w <= 0 || pa.h <= 0) return;
        float range = maxV_ - minV_;
        if (range <= 0.f) range = 1.f;
        int16_t base = int16_t(pa.y2());   // baseline row
        ctx.hairline(pa.x, base, pa.w, c.fg);

        if (mode_ == Mode::Bar) {
            float pitch = float(pa.w + 2) / float(count_);
            int16_t bw = std::max<int16_t>(1, int16_t(pitch) - 2);
            for (uint16_t i = 0; i < count_; ++i) {
                float norm = (data_[i] - minV_) / range;
                norm = norm<0.f?0.f:(norm>1.f?1.f:norm);
                int16_t bh = std::max<int16_t>(2, int16_t(norm * float(pa.h)));
                int16_t bx = int16_t(pa.x + i * pitch);
                int16_t rr = bw >= 4 ? 2 : 0;
                ctx.fillRoundRect(ctx.d, bx, int16_t(base - bh), bw, int16_t(bh + rr), rr, c.fg);
            }
            ctx.drawFastHLine(ctx.d, pa.x, int16_t(base + 1), pa.w, c.bg); // clip rounded bottoms
        } else {
            int16_t px_prev=0, py_prev=0;
            for (uint16_t i = 0; i < count_; ++i) {
                float norm = (data_[i] - minV_) / range;
                norm = norm<0.f?0.f:(norm>1.f?1.f:norm);
                int16_t px = int16_t(pa.x + float(pa.w-1) * i / float(count_-1 > 0 ? count_-1 : 1));
                int16_t py = int16_t(base - 2 - norm * float(pa.h-3));
                // dotted area under the curve
                for (int16_t yy = int16_t(py + 2); yy < base; ++yy)
                    if (DrawCtx::ditherOn(px, yy, 1)) ctx.drawPixel(ctx.d, px, yy, c.fg);
                if (i > 0) {
                    ctx.drawLine(ctx.d, px_prev, py_prev, px, py, c.fg);
                    ctx.drawLine(ctx.d, px_prev, int16_t(py_prev+1), px, int16_t(py+1), c.fg);
                    // fill dither for skipped columns
                    for (int16_t xx = int16_t(px_prev + 1); xx < px; ++xx) {
                        int16_t ly = int16_t(py_prev + (py - py_prev) * (xx - px_prev) / (px - px_prev));
                        for (int16_t yy = int16_t(ly + 2); yy < base; ++yy)
                            if (DrawCtx::ditherOn(xx, yy, 1)) ctx.drawPixel(ctx.d, xx, yy, c.fg);
                    }
                }
                px_prev=px; py_prev=py;
            }
            ctx.fillCircle(px_prev, py_prev, 2, c.fg);
        }
    }

private:
    const float* data_;
    uint16_t     count_;
    float        minV_, maxV_;
    Mode         mode_ = Mode::Bar;
};

// ============================================================================
//  Divider
//    new Divider()                 → dotted horizontal rule
//    new Divider("Audio")          → section header: bold caption + rule
//    new Divider("", false, true)  → vertical rule
// ============================================================================
class Divider : public Element {
public:
    explicit Divider(const char* label="", bool showLabel=true, bool vertical=false,
                     uint16_t fixedW=0, uint16_t fixedH=0)
        : Element(label, 0, fixedW, fixedH), showLabel_(showLabel)
    {
        if (vertical) style_ |= STYLE_VERTICAL;
        padding_ = 1;
        textSize_ = TEXT_CAPTION;
    }

    Rect measure(DrawCtx& ctx, Rect cursor, Rect area, uint16_t pStyle) override {
        if (style_ & STYLE_DISPLAY_FIXED) return Rect(x,y,w,h);
        placeAt(cursor, pStyle);
        if (style_ & STYLE_VERTICAL) {
            w = resolveW(area);
            if (width_==0 && layoutW_==0) w = 8;
            h = resolveH(area.h);
        } else {
            w = resolveW(area);
            bool lbl = showLabel_ && !label_.empty();
            h = resolveH(lbl ? int16_t(ctx.metrics(textSize_).cap + 10) : 7);
        }
        if (w<1) w=1;
        if (h<1) h=1;
        return Rect(x,y,w,h);
    }

    void draw(DrawCtx& ctx, Colors colors) override {
        if (style_ & STYLE_DISABLED) return;
        Colors c = resolveColors(colors);
        if (style_ & STYLE_VERTICAL) {
            ctx.vHairline(int16_t(x + w/2), y, h, c.fg);
            return;
        }
        if (showLabel_ && !label_.empty()) {
            // Section header: CAPTION text bottom-left, rule to the right
            int16_t cap = ctx.metrics(textSize_).cap;
            int16_t base = int16_t(y + h - 3);
            int16_t tw = ctx.textWidth(label_.c_str(), TEXT_BOLD);
            ctx.text(x, base, label_.c_str(), TEXT_BOLD, c.fg);
            int16_t lx = int16_t(x + tw + 6);
            if (lx < x + w) ctx.hairline(lx, int16_t(base - cap/2), int16_t(x + w - lx), c.fg);
        } else {
            ctx.hairline(x, int16_t(y + h/2), w, c.fg);
        }
    }

private:
    bool showLabel_;
};

// ============================================================================
//  Meter — N rounded segments, filled up to `value`.  If a label is set it is
//  drawn to the left and the segments use the remaining width.
//
//    auto* m = page->add(new Meter(0.75f, 5, 80, 16)).get();
//    m->setLabel("Signal");
// ============================================================================
class Meter : public Element {
public:
    Meter(float value=0.f, uint8_t segments=5,
          uint16_t fixedW=80, uint16_t fixedH=16)
        : Element("", 0, fixedW, fixedH),
          value_(value), segments_(segments > 0 ? segments : 1) {}

    Meter& setValue   (float v) { value_ = v<0.f?0.f:(v>1.f?1.f:v); return *this; }
    Meter& setSegments(uint8_t s){ segments_=s>0?s:1; return *this; }
    Meter& setLabel   (const char* l){ label_=l; return *this; }
    float  value() const { return value_; }

    Rect measure(DrawCtx& ctx, Rect cursor, Rect area, uint16_t pStyle) override {
        (void)ctx;
        if (style_ & STYLE_DISPLAY_FIXED) return Rect(x,y,w,h);
        placeAt(cursor, pStyle);
        w = resolveW(area);
        h = resolveH(int16_t(height_ > 0 ? height_ : 14));
        if (w<1) w=1;
        if (h<1) h=1;
        return Rect(x,y,w,h);
    }

    void draw(DrawCtx& ctx, Colors colors) override {
        if (style_ & STYLE_DISABLED) return;
        Colors c = resolveColors(colors);
        drawBackground(ctx, c);

        int16_t sx0 = x, sw = w;
        int filled = int(value_ * float(segments_) + 0.5f);
        if (filled > (int)segments_) filled = (int)segments_;
        if (!label_.empty()) {
            char pct[8]; snprintf(pct, sizeof(pct), "%d%%", int(value_ * 100.f + 0.5f));
            int16_t lw = ctx.textWidth(label_.c_str(), TEXT_BODY);
            int16_t pw = ctx.textWidth("100%", TEXT_BOLD);
            ctx.textBox(x, y, lw, h, label_.c_str(), TEXT_BODY, c.fg);
            ctx.textBox(int16_t(x + w - pw), y, pw, h, pct, TEXT_BOLD, c.fg, TEXT_HALIGN_RIGHT);
            sx0 = int16_t(x + lw + 8);
            sw  = int16_t(w - lw - pw - 16);
        }
        const int16_t gap = 3;
        int16_t segW = int16_t((sw - gap*(int16_t)(segments_-1)) / (int16_t)segments_);
        if (segW < 2) segW = 2;
        int16_t segH = std::min<int16_t>(h, 10);
        int16_t sy = int16_t(y + (h - segH) / 2);
        int16_t r = std::min<int16_t>(3, int16_t(segH / 2));
        for (int i = 0; i < (int)segments_; ++i) {
            int16_t sx = int16_t(sx0 + i*(segW + gap));
            if (i < filled) ctx.fillRoundRect(ctx.d, sx, sy, segW, segH, r, c.fg);
            else {
                ctx.ditherRound(sx, sy, segW, segH, r, c.fg, 2);
                ctx.drawRoundRect(ctx.d, sx, sy, segW, segH, r, c.fg);
            }
        }
    }

private:
    float   value_;
    uint8_t segments_;
};

} // namespace einkui
