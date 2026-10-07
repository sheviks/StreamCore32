#pragma once
// ============================================================================
//  einkui/elements/circular_slider.h
//
//  CircularSlider — arc-style knob for EPD displays.
//
//  Draws a circular track from startAngle to endAngle (degrees, clockwise
//  from top = 0°).  The filled arc shows the current value.  A small
//  thumb circle marks the current position.  The centre optionally shows
//  the value text and/or a label below it.
//
//  Default arc: 225° … 135° clockwise (270° sweep, open at the bottom — classic knob)
//
//  Touch: drag anywhere on the element; the angle relative to the centre
//  is mapped to the value.  Only angles within the arc are accepted.
//
//  Usage:
//    auto* k = page->add(new CircularSlider("Vol", 0.f, 100.f, 0, 80, 80));
//    k->setValue(75.f).setUnit("%").setDecimals(0);
//    k->cb().onChange = [](void*, Element*, const char* v){ setVolume(atoi(v)); };
//
//    // Full circle (e.g. pan):
//    auto* pan = page->add(new CircularSlider("Pan", -1.f, 1.f, 0, 64, 64));
//    pan->setArc(0, 360);
//
//    // Thin ring style:
//    k->setThickness(4);
// ============================================================================
#include "../include/element.h"
#include "FT6X36.h"
#include <cstdio>
#include <cmath>

namespace einkui {

class CircularSlider : public Element {
public:
    explicit CircularSlider(const char* label  = "",
                            float       minV   = 0.f,
                            float       maxV   = 100.f,
                            float       initV  = 0.f,
                            uint16_t    fixedW = 72,
                            uint16_t    fixedH = 72)
        : Element(label, 0, fixedW, fixedH),
          min_(minV), max_(maxV), value_(clamp(initV))
    {}

    // ---- configuration -------------------------------------------------------
    CircularSlider& setValue   (float v)       { value_=clamp(v);  return *this; }
    CircularSlider& setMin     (float v)       { min_=v;           return *this; }
    CircularSlider& setMax     (float v)       { max_=v;           return *this; }
    CircularSlider& setStep    (float v)       { step_=v;          return *this; }
    CircularSlider& setUnit    (const char* u) { unit_=u;          return *this; }
    CircularSlider& setDecimals(uint8_t d)     { decimals_=d;      return *this; }
    CircularSlider& setThickness(uint8_t t)    { thickness_=t;     return *this; }
    /// Arc start and end in degrees, clockwise from top (0 = 12 o'clock).
    /// Default: 225 … 135  (270° sweep, gap at the bottom).
    CircularSlider& setArc(int16_t startDeg, int16_t endDeg) {
        arcStart_ = startDeg; arcEnd_ = endDeg; return *this;
    }
    /// Show the numeric value in the centre.
    CircularSlider& showValue(bool v = true)   { showValue_ = v;   return *this; }

    float value() const { return value_; }
    bool  redrawOnMove() const override { return true; }

    // ---- measure -------------------------------------------------------------
    Rect measure(DrawCtx& ctx, Rect cursor, Rect area, uint16_t pStyle) override {
        (void)ctx;
        if (style_ & STYLE_DISPLAY_FIXED) return Rect(x,y,w,h);
        placeAt(cursor, pStyle);
        w = resolveW(area);
        h = resolveH(int16_t(height_ > 0 ? height_ : width_ > 0 ? width_ : 72));
        if (w < 1) w = 1;
        if (h < 1) h = 1;
        return Rect(x,y,w,h);
    }

    // ---- draw ----------------------------------------------------------------
    void draw(DrawCtx& ctx, Colors colors) override {
        if (style_ & STYLE_DISABLED) return;
        Colors c = resolveColors(colors);
        drawBackground(ctx, c);

        int16_t cx  = int16_t(x + w / 2);
        int16_t cy  = int16_t(y + h / 2);
        int16_t kr  = std::max<int16_t>(4, int16_t(thickness_ / 2 + 3));   // knob radius
        int16_t r   = int16_t(std::min(w, h) / 2 - kr + thickness_ / 2 - 1);
        if (r < 8) return;
        int16_t t   = std::min<int16_t>(thickness_, int16_t(r / 2));

        float sweep = arcSweep();
        float ratio = (max_ > min_) ? (value_ - min_) / (max_ - min_) : 0.f;
        ratio = ratio < 0.f ? 0.f : (ratio > 1.f ? 1.f : ratio);
        float a0    = float(arcStart_);
        float valEnd = a0 + ratio * sweep;

        // dotted track + solid value arc (round caps)
        ctx.ditherArc(cx, cy, r, int16_t(r - t + 1), a0, a0 + sweep, c.fg, 2);
        if (ratio > 0.001f) ctx.arc(cx, cy, r, t, a0, valEnd, c.fg);

        // knob: white disc with black ring, centred on the track
        float rm = float(r) - float(t - 1) * 0.5f;
        float ta = degToRad(valEnd);
        int16_t tx = int16_t(lroundf(float(cx) + rm * sinf(ta)));
        int16_t ty = int16_t(lroundf(float(cy) - rm * cosf(ta)));
        ctx.fillCircle(tx, ty, kr, c.fg);
        ctx.fillCircle(tx, ty, int16_t(kr - 2), c.bg);

        // centre: value (title/display font) + label caption
        int16_t inner = int16_t(r - t - 2);
        if (inner < 6) return;
        char vbuf[24]; fmtValue(vbuf, sizeof(vbuf));
        uint8_t role = TEXT_CAPTION;
        for (uint8_t cand : { TEXT_DISPLAY, TEXT_TITLE, TEXT_BOLD })
            if (ctx.textWidth(vbuf, cand) <= inner * 2 - 8 && ctx.metrics(cand).cap * 2 <= inner) { role = cand; break; }
        bool lbl = !label_.empty() && !(style_ & STYLE_HIDE_LABEL);
        int16_t capV = ctx.metrics(role).cap, capL = ctx.metrics(TEXT_CAPTION).cap;
        if (showValue_) {
            char buf[24]; fmtValue(buf, sizeof(buf));
            int16_t top = lbl ? int16_t(cy - (capV + 4 + capL) / 2) : int16_t(cy - capV / 2);
            ctx.textBox(int16_t(cx - inner), top, int16_t(inner * 2), capV, buf, role, c.fg,
                        TEXT_HALIGN_CENTER, TEXT_VALIGN_TOP, false);
            if (lbl) ctx.textBox(int16_t(cx - inner), int16_t(top + capV + 4), int16_t(inner * 2), capL,
                                 label_.c_str(), TEXT_CAPTION, c.fg, TEXT_HALIGN_CENTER, TEXT_VALIGN_TOP);
        } else if (lbl) {
            ctx.textBox(int16_t(cx - inner), int16_t(cy - capL / 2), int16_t(inner * 2), capL,
                        label_.c_str(), TEXT_CAPTION, c.fg, TEXT_HALIGN_CENTER, TEXT_VALIGN_TOP);
        }
    }

    // ---- touch ---------------------------------------------------------------
    bool onTouch(DrawCtx& /*ctx*/, const TTouchFrame& tf,
                 std::shared_ptr<Element>& focused) override
    {
        uint8_t  ev = (uint8_t)tf.p[0].event;
        uint16_t tx = tf.p[0].x, ty = tf.p[0].y;

        if (ev == 0 && hitTest(tx, ty)) {
            focused = std::shared_ptr<Element>(this, [](Element*){});
            updateFromTouch(tx, ty);
            return true;
        }
        if (ev == 2 && focused.get() == this) {
            updateFromTouch(tx, ty);
            return true;
        }
        if (ev == 1 && focused.get() == this) {
            updateFromTouch(tx, ty);
            fireChange();
            focused = nullptr;
            return true;
        }
        return false;
    }

private:
    float       min_       = 0.f;
    float       max_       = 100.f;
    float       step_      = 0.f;
    float       value_     = 0.f;
    const char* unit_      = nullptr;
    uint8_t     decimals_  = 0;
    uint8_t     thickness_ = 5;
    int16_t     arcStart_  = 225;   // degrees clockwise from top
    int16_t     arcEnd_    = 135;   // 225 → 135 clockwise = 270° sweep, gap at the bottom
    bool        showValue_ = true;

    // ---- helpers -------------------------------------------------------------
    static float degToRad(float d) { return d * 3.14159265f / 180.f; }

    // The total sweep of the arc in degrees (always positive, wraps 360)
    float arcSweep() const {
        float s = float(arcEnd_ - arcStart_);
        if (s <= 0.f) s += 360.f;
        return s;
    }

    float clamp(float v) const { return v<min_?min_:(v>max_?max_:v); }
    float snap  (float v) const {
        if (step_ <= 0.f) return clamp(v);
        return clamp(min_ + std::round((v-min_)/step_)*step_);
    }

    void fmtValue(char* buf, size_t len) const {
        if (decimals_==0) snprintf(buf,len,"%.0f%s", (double)value_,unit_?unit_:"");
        else              snprintf(buf,len,"%.*f%s",(int)decimals_,(double)value_,unit_?unit_:"");
    }

    // Draw an arc from aDeg to bDeg (degrees clockwise from top).
    // Uses line segments between kSteps sampled points.
    void drawArcLine(DrawCtx& ctx,
                     int16_t cx, int16_t cy, int16_t r,
                     float aDeg, float bDeg, uint16_t color) const
    {
        constexpr int kSteps = 64;
        float sweep = bDeg - aDeg;
        if (sweep <= 0.f) sweep += 360.f;
        if (sweep > 360.f) sweep = 360.f;

        int16_t px0=0, py0=0;
        for (int s = 0; s <= kSteps; ++s) {
            float frac  = float(s) / float(kSteps);
            float angle = degToRad(aDeg + frac * sweep);
            int16_t px  = int16_t(cx + float(r) * sinf(angle));
            int16_t py  = int16_t(cy - float(r) * cosf(angle));
            if (s > 0) ctx.drawLine(ctx.d, px0, py0, px, py, color);
            px0=px; py0=py;
        }
    }

    void updateFromTouch(uint16_t tx, uint16_t ty) {
        int16_t cx = x + w/2;
        int16_t cy = y + h/2;
        // Angle from centre to touch point, clockwise from top
        float dx = float(int16_t(tx) - cx);
        float dy = float(int16_t(ty) - cy);  // positive = down
        // atan2(x,y) gives angle from +y axis (top) clockwise
        float angleDeg = atan2f(dx, -dy) * 180.f / 3.14159265f;
        if (angleDeg < 0.f) angleDeg += 360.f;

        float sweep = arcSweep();
        // Normalise to the arc: offset from arcStart_
        float offset = angleDeg - float(arcStart_);
        while (offset < 0.f)    offset += 360.f;
        while (offset > 360.f)  offset -= 360.f;

        // Clamp to sweep range
        if (offset > sweep) {
            // Snap to whichever end is closer
            offset = (offset - sweep < 360.f - offset) ? sweep : 0.f;
        }

        float ratio = sweep > 0.f ? offset / sweep : 0.f;
        value_ = snap(min_ + ratio * (max_ - min_));

        if (hasCb() && cb_->onChange) {
            char buf[24]; fmtValue(buf, sizeof(buf));
            cb_->onChange(cb_->ctx, this, buf);
        }
    }

    void fireChange() {
        if (!hasCb() || !cb_->onChange) return;
        char buf[24]; fmtValue(buf, sizeof(buf));
        cb_->onChange(cb_->ctx, this, buf);
    }
};

} // namespace einkui