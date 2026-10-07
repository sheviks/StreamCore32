#pragma once
// ============================================================================
//  einkui/elements/slider.h
//
//  Slider — horizontal (default) or vertical (STYLE_VERTICAL) value selector.
//
//  Horizontal layouts (chosen automatically from the element height):
//
//    stacked (tall enough)        inline (short)            bare (label+value hidden)
//    ┌──────────────────────┐     ┌──────────────────────┐   ┌──────────────────────┐
//    │ Bass          +3.0 dB│     │ Vol ━━━━━━━○┄┄┄┄ 65% │   │ ━━━━━━━━━○┄┄┄┄┄┄┄┄┄┄ │
//    │ ━━━━━━━━━○┄┄┄┄┄┄┄┄┄┄ │     └──────────────────────┘   └──────────────────────┘
//    └──────────────────────┘
//
//  The filled part of the track is solid, the rest is a dotted "grey" track,
//  and the knob is a white disc with a black ring.  Touch input is accepted
//  on the track band only (full width, knob height), so tapping the label
//  never moves the value.
//
//  Vertical (STYLE_VERTICAL): value on top, label at the bottom, track between.
// ============================================================================
#include "../include/element.h"
#include "FT6X36.h"
#include <cstdio>
#include <cmath>

namespace einkui {

class Slider : public Element {
public:
    explicit Slider(const char* label,
                    float minV=0.f, float maxV=1.f,
                    uint16_t fixedW=0, uint16_t fixedH=32)
        : Element(label, 0, fixedW, fixedH),
          min_(minV), max_(maxV), value_(minV) { padding_ = 4; }

    Slider& setMin     (float v)       { min_=v;          return *this; }
    Slider& setMax     (float v)       { max_=v;          return *this; }
    Slider& setStep    (float v)       { step_=v;         return *this; }
    Slider& setValue   (float v)       { value_=clamp(v); return *this; }
    Slider& setUnit    (const char* u) { unit_=u;         return *this; }
    Slider& setDecimals(uint8_t d)     { decimals_=d;     return *this; }
    Slider& setGetValue(float(*fn)())  { getFn_=fn;       return *this; }
    // Show a leading "+" for positive values (nice for dB / offsets).
    Slider& showSign(bool v = true)    { sign_=v;         return *this; }
    // Knob radius in px (default 6). 0 = no knob (progress-bar look).
    Slider& setKnob(uint8_t r)         { knobR_=r;        return *this; }
    // Thickness of the bar in px (default 4).
    Slider& setTrack(uint8_t t)        { track_h_=t ? t : 1; return *this; }

    float value()      const { return value_; }
    bool  redrawOnMove() const override { return true; }
    float freshValue()       { if (getFn_) value_=clamp(getFn_()); return value_; }

    // =========================================================================
    Rect measure(DrawCtx& ctx, Rect cursor, Rect area, uint16_t pStyle) override {
        if (style_ & STYLE_DISPLAY_FIXED) return Rect(x,y,w,h);
        placeAt(cursor, pStyle);
        DrawCtx::Metrics m = ctx.metrics(textSize_);
        cap_ = m.cap;
        const bool vert = (style_ & STYLE_VERTICAL) != 0;
        int16_t knobD = int16_t(knobR_ * 2 + 1);
        int16_t natural = (showLbl() || showVal()) && !vert
                        ? int16_t(m.cap + 8 + knobD + padding_)
                        : int16_t(knobD + padding_ * 2);
        w = resolveW(area);
        h = resolveH(natural);
        if (w < 1) w = 1;
        if (h < 1) h = 1;

        labelW_ = showLbl() ? ctx.textWidth(label_.c_str(), textSize_) : 0;
        if (showVal()) {
            char a[24], b[24];
            fmtValueAt(a, sizeof(a), max_); fmtValueAt(b, sizeof(b), min_);
            valueW_ = std::max(ctx.textWidth(a, TEXT_BOLD), ctx.textWidth(b, TEXT_BOLD));
        } else valueW_ = 0;

        computeGeometry();
        return Rect(x,y,w,h);
    }

    // =========================================================================
    void draw(DrawCtx& ctx, Colors colors) override {
        if (style_ & STYLE_DISABLED) return;
        if (getFn_) value_ = clamp(getFn_());
        Colors c = resolveColors(colors);
        drawBackground(ctx, c);

        char valBuf[24]; fmtValue(valBuf, sizeof(valBuf));
        const bool vert = (style_ & STYLE_VERTICAL) != 0;

        if (!vert) {
            if (stacked_) {
                Rect top(int16_t(x + padding_), int16_t(y + padding_), int16_t(w - padding_*2), int16_t(cap_ + 2));
                if (showVal()) {
                    ctx.textBox(top.x, top.y, top.w, top.h, valBuf, TEXT_BOLD, c.fg,
                                TEXT_HALIGN_RIGHT, TEXT_VALIGN_TOP);
                }
                if (showLbl()) {
                    int16_t lw = int16_t(top.w - (showVal() ? valueW_ + 8 : 0));
                    ctx.textBox(top.x, top.y, lw, top.h, label_.c_str(), textSize_, c.fg,
                                TEXT_HALIGN_LEFT, TEXT_VALIGN_TOP);
                }
            } else {
                if (showLbl())
                    ctx.textBox(int16_t(x + padding_), track_.y - 8, labelW_, int16_t(track_.h + 16),
                                label_.c_str(), textSize_, c.fg);
                if (showVal())
                    ctx.textBox(int16_t(x + w - padding_ - valueW_), track_.y - 8, valueW_, int16_t(track_.h + 16),
                                valBuf, TEXT_BOLD, c.fg, TEXT_HALIGN_RIGHT);
            }
        } else {
            if (showVal())
                ctx.textBox(x, int16_t(y + padding_), w, cap_, valBuf, TEXT_BOLD, c.fg,
                            TEXT_HALIGN_CENTER, TEXT_VALIGN_TOP);
            if (showLbl())
                ctx.textBox(x, int16_t(y + h - padding_ - cap_), w, cap_, label_.c_str(), textSize_, c.fg,
                            TEXT_HALIGN_CENTER, TEXT_VALIGN_TOP);
        }

        drawTrack(ctx, c, vert);
        drawRowDivider(ctx, c);
    }

    // =========================================================================
    bool onTouch(DrawCtx& /*ctx*/, const TTouchFrame& tf,
                 std::shared_ptr<Element>& focused) override
    {
        uint8_t  ev = (uint8_t)tf.p[0].event;
        uint16_t tx = tf.p[0].x, ty = tf.p[0].y;
        if (ev == 0 && (touchExpand() ? touch_.expanded(kTouchSlop) : touch_).contains(int16_t(tx), int16_t(ty))) {
            focused = std::shared_ptr<Element>(this, [](Element*){});
            updateFromTouch(tx, ty);
            return true;
        }
        if (ev == 2 && focused.get() == this) { updateFromTouch(tx, ty); return true; }
        if (ev == 1 && focused.get() == this) {
            updateFromTouch(tx, ty);
            fireChange();
            focused = nullptr;
            return true;
        }
        return false;
    }

private:
    float       min_=0.f, max_=1.f, step_=0.f, value_=0.f;
    const char* unit_     = nullptr;
    uint8_t     decimals_ = 0;
    bool        sign_     = false;
    uint8_t     knobR_    = 6;
    float     (*getFn_)() = nullptr;

    int16_t cap_ = 8, labelW_ = 0, valueW_ = 0;
    bool    stacked_ = false;
    Rect    track_;   // the visible bar (thin)
    Rect    touch_;   // touch target (bar band, knob height)

    uint8_t track_h_ = 4;

    bool showLbl() const { return !label_.empty() && !(style_ & STYLE_HIDE_LABEL); }
    bool showVal() const { return !(style_ & STYLE_HIDE_VALUE); }

    float clamp(float v) const { return v<min_?min_:(v>max_?max_:v); }
    float snap(float v)  const {
        if (step_<=0.f) return clamp(v);
        return clamp(min_ + std::round((v-min_)/step_)*step_);
    }
    float ratio() const {
        float r = (max_>min_) ? (value_-min_)/(max_-min_) : 0.f;
        return r < 0.f ? 0.f : (r > 1.f ? 1.f : r);
    }
    void fmtValue(char* buf, size_t len) const { fmtValueAt(buf,len,value_); }
    void fmtValueAt(char* buf, size_t len, float v) const {
        const char* sg = (sign_ && v > 0.f) ? "+" : "";
        if (decimals_==0) snprintf(buf,len,"%s%.0f%s", sg, (double)v, unit_?unit_:"");
        else              snprintf(buf,len,"%s%.*f%s", sg, (int)decimals_,(double)v,unit_?unit_:"");
    }

    void computeGeometry() {
        const bool vert = (style_ & STYLE_VERTICAL) != 0;
        int16_t kr = knobR_;
        if (!vert) {
            int16_t knobBand = int16_t(kr * 2 + 1);
            stacked_ = (showLbl() || showVal()) && h >= cap_ + 6 + knobBand;
            int16_t tx = int16_t(x + padding_), tw = int16_t(w - padding_ * 2);
            int16_t bandY, bandH;
            if (stacked_) {
                bandY = int16_t(y + padding_ + cap_ + 4);
                bandH = int16_t(y + h - bandY - (padding_ > 2 ? 2 : 0));
            } else {
                bandY = y; bandH = h;
                if (showLbl()) { tx = int16_t(tx + labelW_ + 10); tw = int16_t(tw - labelW_ - 10); }
                if (showVal()) tw = int16_t(tw - valueW_ - 10);
            }
            if (tw < kr * 2 + 4) { tx = int16_t(x + padding_); tw = int16_t(w - padding_ * 2); }
            int16_t ty = int16_t(bandY + (bandH - int16_t(track_h_)) / 2);
            track_ = Rect(tx, ty, tw, int16_t(track_h_));
            touch_ = Rect(tx, int16_t(std::min<int16_t>(ty - kr - 2, bandY)), tw,
                          int16_t(std::max<int16_t>(int16_t(track_h_) + kr * 2 + 4, bandH)));
            touch_ &= Rect(x, y, w, h);
        } else {
            int16_t top = int16_t(y + padding_ + (showVal() ? cap_ + 6 : 0));
            int16_t bot = int16_t(y + h - padding_ - (showLbl() ? cap_ + 6 : 0));
            int16_t tx  = int16_t(x + (w - int16_t(track_h_)) / 2);
            track_ = Rect(tx, top, int16_t(track_h_), int16_t(std::max<int16_t>(4, bot - top)));
            touch_ = Rect(x, top, w, track_.h);
        }
    }

    void drawTrack(DrawCtx& ctx, Colors c, bool vert) const {
        Rect tr = track_;
        if (tr.w <= 0 || tr.h <= 0) return;
        int16_t kr = knobR_;
        float r = ratio();
        if (!vert) {
            int16_t usable = int16_t(tr.w - kr * 2);
            int16_t kx = int16_t(tr.x + kr + lroundf(r * float(usable > 0 ? usable : 0)));
            int16_t cy = int16_t(tr.y + tr.h / 2);
            // grey remainder
            const int16_t rr = ctx.th().radius ? int16_t(tr.h / 2) : 0;
            if (rr)
                ctx.ditherRound(tr.x, tr.y, tr.w, tr.h, rr, c.fg, 2);
            else  // square theme: the rest is a hairline, the value a solid bar
                ctx.fillRect(ctx.d, tr.x, cy, tr.w, 1, c.fg);
            // filled part
            int16_t fw = int16_t(kx - tr.x);
            if (fw > 0) ctx.fillRoundRect(ctx.d, tr.x, tr.y, fw, tr.h, rr, c.fg);
            if (kr > 0) knob(ctx, c, kx, cy, kr);
        } else {
            int16_t usable = int16_t(tr.h - kr * 2);
            int16_t ky = int16_t(tr.y + tr.h - kr - lroundf(r * float(usable > 0 ? usable : 0)));
            int16_t cx = int16_t(tr.x + tr.w / 2);
            ctx.ditherRound(tr.x, tr.y, tr.w, tr.h, int16_t(tr.w / 2), c.fg, 2);
            int16_t fh = int16_t(tr.y + tr.h - ky);
            if (fh > 0) ctx.fillRoundRect(ctx.d, tr.x, ky, tr.w, fh, int16_t(tr.w / 2), c.fg);
            if (kr > 0) knob(ctx, c, cx, ky, kr);
        }
    }
    static void knob(DrawCtx& ctx, Colors c, int16_t cx, int16_t cy, int16_t r) {
        ctx.fillCircle(cx, cy, r, c.fg);
        ctx.fillCircle(cx, cy, int16_t(r - 2), c.bg);
    }

    void updateFromTouch(uint16_t tx, uint16_t ty) {
        const bool vert = (style_ & STYLE_VERTICAL) != 0;
        Rect tr = track_;
        int16_t kr = knobR_;
        float rr;
        if (!vert) {
            int16_t usable = int16_t(tr.w - kr * 2);
            rr = usable > 0 ? float(int16_t(tx) - tr.x - kr) / float(usable) : 0.f;
        } else {
            int16_t usable = int16_t(tr.h - kr * 2);
            rr = usable > 0 ? 1.f - float(int16_t(ty) - tr.y - kr) / float(usable) : 0.f;
        }
        rr = rr < 0.f ? 0.f : (rr > 1.f ? 1.f : rr);
        value_ = snap(min_ + rr * (max_ - min_));
        if (hasCb() && cb_->onChange) {
            char buf[24]; fmtValue(buf,sizeof(buf));
            cb_->onChange(cb_->ctx, this, buf);
        }
    }
    void fireChange() {
        if (!hasCb() || !cb_->onChange) return;
        char buf[24]; fmtValue(buf,sizeof(buf));
        cb_->onChange(cb_->ctx, this, buf);
    }
};

} // namespace einkui
