#pragma once
// ============================================================================
//  einkui/elements/button.h
//
//  Button — pressable surface with a centred label or "&icon" / "&icon Text".
//
//  Variants (setVariant):
//    Outline  (default) rounded card with the theme's hard shadow.  When
//             pressed it "sinks" into the shadow and inverts.
//    Filled   solid black primary action with white content.
//    Ghost    no chrome; inverts while pressed.  Good for icon buttons.
//    List     full-width list row: label left, optional trailing text and a
//             chevron on the right, dotted divider at the bottom.
//
//  Minimum usage:
//    auto btn = page->add(new Button("OK", 80, 32));
//    btn->onTap([](void*, Element*){ /* tap action */ });
//
//    page->add(new Button("&ui_play"))->filled().pill();   // round play button
// ============================================================================
#include "../include/element.h"
#include "FT6X36.h"

namespace einkui {

class Button : public Element {
public:
    enum class Variant : uint8_t { Outline, Filled, Ghost, List };

    Button() : Element("", STYLE_BORDER) { init(); }
    explicit Button(const char* label, uint16_t fixedW=0, uint16_t fixedH=0)
        : Element(label, STYLE_BORDER, fixedW, fixedH) { init(); }

    Button& onTap(TouchFn fn, void* ctx=nullptr){ cb().onTouchUp=fn; cb().ctx=ctx; return *this; }
    bool    pressed() const { return pressed_; }
    void    cancelTouch() override { pressed_ = false; }

    Button& setVariant(Variant v) {
        variant_ = v;
        if (v == Variant::List) { textLeft(); addStyle(STYLE_LIST_ROW); }
        return *this;
    }
    Button& filled()  { return setVariant(Variant::Filled); }
    Button& ghost()   { return setVariant(Variant::Ghost);  }
    Button& asListRow(){ return setVariant(Variant::List);  }
    // Fully rounded ends (a circle when the button is square).
    Button& pill()    { setRadius(255); return *this; }
    // Small text shown right-aligned in List rows (e.g. current value).
    Button& setTrailing(const char* t) { trailing_ = t ? t : ""; return *this; }
    // List rows show a chevron by default; disable for action rows.
    Button& showChevron(bool v) { chevron_ = v; return *this; }
    // Dimmed content (50 % dither): "off" state of toggles such as shuffle,
    // like the web UI's low-opacity icons.
    Button& dim(bool on = true) { dim_ = on; return *this; }
    bool    dimmed() const { return dim_; }
    Variant variant() const { return variant_; }

    Rect measure(DrawCtx& ctx, Rect cursor, Rect area, uint16_t pStyle) override {
        if (style_ & STYLE_DISPLAY_FIXED) return Rect(x,y,w,h);
        placeAt(cursor, pStyle);
        const bool isIcon = !label_.empty() && label_[0] == '&' && !strchr(label_.c_str(), ' ');
        int16_t contentH = variant_ == Variant::List ? int16_t(ctx.th().rowH)
                         : int16_t(ctx.metrics(textSize_).line + padding_ * 2 + shadowOf(ctx));
        w = resolveW(area);
        h = resolveH(contentH);
        if (width_ == 0 && layoutW_ == 0 && variant_ != Variant::List) {
            // fit content when no width is given
            int16_t cw = isIcon ? h : int16_t(ctx.textWidth(label_.c_str(), textSize_) + padding_ * 4);
            w = int16_t(cw + shadowOf(ctx));
        }
        if (w < 1) w = 1;
        if (h < 1) h = 1;
        return Rect(x,y,w,h);
    }

    void draw(DrawCtx& ctx, Colors colors) override {
        if (style_ & STYLE_DISABLED) return;
        Colors c = resolveColors(colors);
        const Theme& t = ctx.th();
        int16_t r = (style_ & STYLE_ROUND_CORNER) ? int16_t(radius_) : int16_t(t.radiusSm);
        uint16_t fg = c.fg, bg = c.bg;
        Rect body(x, y, w, h);

        switch (variant_) {
        case Variant::Outline:
            if (!(style_ & STYLE_BORDER)) {               // borderless outline = ghost
                if (pressed_) { ctx.fillRoundRect(ctx.d, x, y, w, h, r, c.fg); std::swap(fg, bg); }
                break;
            }
            body = ctx.surface(x, y, w, h, r, c.fg, c.bg, pressed_);
            if (pressed_) std::swap(fg, bg);
            break;
        case Variant::Filled:
            body = ctx.surface(x, y, w, h, r, c.fg, c.bg, pressed_, /*filled*/true);
            std::swap(fg, bg);
            break;
        case Variant::Ghost:
            if (pressed_) {
                ctx.fillRoundRect(ctx.d, x, y, w, h, std::min<int16_t>(r, std::min(w, h) / 2), c.fg);
                std::swap(fg, bg);
            }
            break;
        case Variant::List:
            if (pressed_) {
                ctx.fillRoundRect(ctx.d, x, y, w, int16_t(h - 1), t.radiusSm, c.fg);
                std::swap(fg, bg);
            }
            break;
        }

        Rect inner(int16_t(body.x + padding_), int16_t(body.y + padding_),
                   int16_t(body.w - padding_ * 2), int16_t(body.h - padding_ * 2));

        if (variant_ == Variant::List) {
            inner.x = int16_t(body.x + 4); inner.w = int16_t(body.w - 8);
            int16_t right = int16_t(inner.x + inner.w);
            if (chevron_) {
                const int16_t cs = 12;
                ctx.icon("ui_chevron_right", int16_t(right - cs / 2), int16_t(body.y + body.h / 2), cs, fg, bg);
                right = int16_t(right - cs - 6);
            }
            if (!trailing_.empty()) {
                int16_t tw = ctx.textWidth(trailing_.c_str(), TEXT_BODY);
                ctx.textBox(int16_t(right - tw), body.y, tw, body.h, trailing_.c_str(), TEXT_BODY, fg,
                            TEXT_HALIGN_RIGHT, TEXT_VALIGN_MIDDLE);
                right = int16_t(right - tw - 8);
            }
            inner.w = int16_t(right - inner.x);
            inner.y = body.y; inner.h = body.h;
        }
        drawLabelIn(ctx, inner, fg, bg);
        if (dim_ && !pressed_) ctx.dither(inner.x, inner.y, inner.w, inner.h, bg, 2);
        if (variant_ == Variant::List && !pressed_) drawRowDivider(ctx, c);
    }

    bool onTouch(DrawCtx& ctx, const TTouchFrame& tf,
                 std::shared_ptr<Element>& focused) override
    {
        (void)ctx;
        uint8_t  ev = (uint8_t)tf.p[0].event;
        uint16_t tx = tf.p[0].x, ty = tf.p[0].y;
        if (ev == 0 && hitTest(tx,ty)) {
            pressed_ = true;
            focused  = std::shared_ptr<Element>(this,[](Element*){});
            if (hasCb() && cb_->onTouchDown) cb_->onTouchDown(cb_->ctx, this);
            return true;
        }
        if (ev == 1 && focused.get() == this) {
            bool wasInside = releaseHit(tx,ty);
            pressed_ = false;
            focused = nullptr;
            if (hasCb() && cb_->onTouchUp && wasInside) cb_->onTouchUp(cb_->ctx, this);
            return true;
        }
        return false;
    }

private:
    bool        pressed_ = false;
    bool        chevron_ = true;
    bool        dim_     = false;
    Variant     variant_ = Variant::Outline;
    std::string trailing_;

    void init() { padding_ = 4; textCenter(); }
    int16_t shadowOf(DrawCtx& ctx) const {
        return (variant_ == Variant::Outline || variant_ == Variant::Filled) ? int16_t(ctx.th().shadow) : 0;
    }
};

} // namespace einkui
