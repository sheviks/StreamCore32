#pragma once
// ============================================================================
//  einkui/elements/toggle.h
//
//  Toggle — binary on/off switch.  Label on the left, a pill switch on the
//  right:  off = dotted grey track with a white knob on the left,
//          on  = solid black track with a white knob on the right.
//  The whole row is the touch target.  Fires onChange("1") or onChange("0").
// ============================================================================
#include "../include/element.h"
#include "FT6X36.h"

namespace einkui {

class Toggle : public Element {
public:
    explicit Toggle(const char* label, bool initialValue = false,
                    uint16_t fixedW=0, uint16_t fixedH=0)
        : Element(label, 0, fixedW, fixedH), on_(initialValue) { padding_ = 4; }

    bool value() const { return on_; }
    void cancelTouch() override { pressed_ = false; }
    void setValue(bool v) { on_ = v; }

    Toggle& onChange(StringFn fn, void* ctx=nullptr){ cb().onChange=fn; cb().ctx=ctx; return *this; }

    Rect measure(DrawCtx& ctx, Rect cursor, Rect area, uint16_t pStyle) override {
        if (style_ & STYLE_DISPLAY_FIXED) return Rect(x,y,w,h);
        placeAt(cursor, pStyle);
        DrawCtx::Metrics m = ctx.metrics(textSize_);
        pillH_ = std::max<int16_t>(14, int16_t(m.cap + 8));
        pillW_ = int16_t(pillH_ * 2 - 2);
        w = resolveW(area);
        if (width_==0 && layoutW_==0)
            w = int16_t(ctx.textWidth(label_.c_str(), textSize_) + pillW_ + 12 + padding_*2);
        h = resolveH(std::max<int16_t>(ctx.th().rowH, int16_t(pillH_ + padding_*2)));
        if (w < 1) w = 1;
        if (h < 1) h = 1;
        return Rect(x,y,w,h);
    }

    void draw(DrawCtx& ctx, Colors colors) override {
        if (style_ & STYLE_DISABLED) return;
        Colors c = resolveColors(colors);
        drawBackground(ctx, c);

        int16_t px = int16_t(x + w - padding_ - pillW_);
        int16_t py = int16_t(y + (h - pillH_) / 2);
        int16_t r  = int16_t(pillH_ / 2);

        // Label (left, clipped before the switch)
        Rect lbl(int16_t(x + padding_), y, int16_t(px - x - padding_ - 8), h);
        drawLabelIn(ctx, lbl, c.fg, c.bg);

        // Track
        if (on_) {
            ctx.fillRoundRect(ctx.d, px, py, pillW_, pillH_, r, c.fg);
        } else {
            ctx.fillRoundRect(ctx.d, px, py, pillW_, pillH_, r, c.bg);
            ctx.ditherRound(int16_t(px+1), int16_t(py+1), int16_t(pillW_-2), int16_t(pillH_-2),
                            int16_t(r-1), c.fg, 2);
            ctx.drawRoundRect(ctx.d, px, py, pillW_, pillH_, r, c.fg);
        }
        // Knob
        int16_t kr = int16_t(r - 3);
        int16_t kcx = on_ ? int16_t(px + pillW_ - r) : int16_t(px + r - 1);
        int16_t kcy = int16_t(py + r);
        if (!on_) ctx.fillCircle(kcx, kcy, int16_t(kr + 1), c.fg);   // ring around knob
        ctx.fillCircle(kcx, kcy, kr, c.bg);
        if (pressed_) ctx.drawCircle(kcx, kcy, int16_t(kr - 2), c.fg);

        drawRowDivider(ctx, c);
    }

    bool onTouch(DrawCtx& /*ctx*/, const TTouchFrame& tf,
                 std::shared_ptr<Element>& focused) override
    {
        uint8_t ev=(uint8_t)tf.p[0].event;
        uint16_t tx=tf.p[0].x, ty=tf.p[0].y;
        if (ev==0 && hitTest(tx,ty)) {
            focused=std::shared_ptr<Element>(this,[](Element*){});
            pressed_ = true;
            return true;
        }
        if (ev==1 && focused.get()==this) {
            focused=nullptr;
            pressed_ = false;
            if (releaseHit(tx,ty)) {
                on_ = !on_;
                if (hasCb() && cb_->onChange) cb_->onChange(cb_->ctx, this, on_ ? "1" : "0");
            }
            return true;
        }
        return false;
    }

private:
    bool    on_      = false;
    bool    pressed_ = false;
    int16_t pillW_   = 28;
    int16_t pillH_   = 15;
};

} // namespace einkui
