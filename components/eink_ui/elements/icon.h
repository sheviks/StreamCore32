#pragma once
// ============================================================================
//  einkui/elements/icon.h
//
//  Icon — renders a VG bytecode asset (from vg_assets.h / eink_vg.h).
//  Can be made tappable by setting a callback.
//
//  Usage:
//    // by name (looked up in vg_assets table at draw time)
//    auto* ic = page->add(new Icon("ui_play", 36, 36));
//    ic->cb().onTouchUp = [](void*, Element*){ ... };
//
//    // by pointer (no lookup)
//    auto* ic = page->add(new Icon(MY_ICON_DATA, 36, 36));
// ============================================================================
#include "../include/element.h"
#include "FT6X36.h"
#include "eink_vg.h"    // drawVG_Q1
#include "vg_assets.h"  // vg_assets[], vg_assets_count

namespace einkui {

class Icon : public Element {
public:
    // Lookup by name at draw time
    explicit Icon(const char* name, uint16_t fixedW=36, uint16_t fixedH=36)
        : Element(name, 0, fixedW, fixedH) {}

    // Direct data pointer (no name lookup)
    explicit Icon(const uint8_t* data, uint16_t fixedW=36, uint16_t fixedH=36,
                  const char* name="")
        : Element(name, 0, fixedW, fixedH), data_(data) {}

    Icon& onTap(TouchFn fn, void* ctx=nullptr){ cb().onTouchUp=fn; cb().ctx=ctx; return *this; }
    void  cancelTouch() override { pressed_ = false; }

    Rect measure(DrawCtx& /*ctx*/, Rect cursor, Rect area, uint16_t pStyle) override {
        (void)area;
        if (style_ & STYLE_DISPLAY_FIXED) return Rect(x,y,w,h);
        placeAt(cursor, pStyle);
        w = int16_t(width_  > 0 ? width_  : 36);
        h = int16_t(height_ > 0 ? height_ : 36);
        return Rect(x,y,w,h);
    }

    void draw(DrawCtx& ctx, Colors colors) override {
        if (style_ & STYLE_DISABLED) return;
        Colors c = resolveColors(colors);
        drawBackground(ctx, c);

        const uint8_t* d = resolveData();
        if (!d) return;

        // drawVG_Q1 expects a real display pointer — get it back from ctx
        // We call it via a forwarding lambda to stay type-erased.
        // ctx.d points to the actual display.  drawVG_Q1 is a template:
        //   template<class T> void drawVG_Q1(T& display, ...)
        // so we need to call it through the concrete display type.
        // We store a callback for this at construction via setDrawVGFn().
        int16_t iw = int16_t(w - padding_*2), ih = int16_t(h - padding_*2);
        int16_t sz = std::min(iw, ih);           // keep icons square (no cropping)
        int16_t ix = int16_t(x + padding_ + (iw - sz) / 2);
        int16_t iy = int16_t(y + padding_ + (ih - sz) / 2);
        if (pressed_) { ctx.fillRoundRect(ctx.d, x, y, w, h, std::min(w, h) / 2, c.fg); std::swap(c.fg, c.bg); }
        if (drawVGFn_)    drawVGFn_(ctx.d, d, ix, iy, sz, sz, c.fg, c.bg);
        else if (ctx.drawVG) ctx.drawVG(ctx.d, d, ix, iy, sz, sz, c.fg, c.bg);
    }

    // Optional: ctx.drawVG is used by default.  Call this only if you need a
    // specific display type different from the one passed to makeDrawCtx():
    //   icon->setDrawVGFn<MyDisplay>();
    template <class DisplayT>
    void setDrawVGFn() {
        drawVGFn_ = [](void* dp, const uint8_t* data,
                        int16_t x, int16_t y, int16_t w, int16_t h,
                        uint16_t fg, uint16_t bg)
        {
            drawVG_Q1(*static_cast<DisplayT*>(dp), data, x, y, w, h, fg, bg);
        };
    }

    bool onTouch(DrawCtx& ctx, const TTouchFrame& tf,
                 std::shared_ptr<Element>& focused) override {
        (void)ctx;
        if (!hasCb()) return false;
        
        uint8_t ev=(uint8_t)tf.p[0].event;
        uint16_t tx=tf.p[0].x, ty=tf.p[0].y;
        if (ev==0 && hitTest(tx,ty)) {
            focused=std::shared_ptr<Element>(this,[](Element*){});
            pressed_ = true;
            if (cb_->onTouchDown) cb_->onTouchDown(cb_->ctx, this);
            return true;
        }
        if (ev==1 && focused.get()==this) {
            pressed_ = false;
            focused=nullptr;
            if (cb_->onTouchUp && releaseHit(tx,ty)) cb_->onTouchUp(cb_->ctx, this);
            return true;
        }
        return false;
    }

private:
    const uint8_t* data_ = nullptr;
    bool pressed_ = false;

    using DrawVGFn = void(*)(void* display, const uint8_t* data,
                              int16_t x, int16_t y, int16_t w, int16_t h,
                              uint16_t fg, uint16_t bg);
    DrawVGFn drawVGFn_ = nullptr;

    const uint8_t* resolveData() const {
        if (data_) return data_;
        const char* n = label_.c_str();
        if (*n == '&') ++n;                      // accept "&name" as well
        for (size_t i=0; i<size_t(vg_assets_count); ++i)
            if (strcmp(n, vg_assets[i].name) == 0) return vg_assets[i].data;
        return nullptr;
    }
};

} // namespace einkui