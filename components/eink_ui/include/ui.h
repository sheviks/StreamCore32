#pragma once
// ============================================================================
//  einkui/include/ui.h
// ============================================================================
#include <memory>
#include <vector>
#include <functional>
#include <cstring>
#include <cstdio>
#include "element.h"
#include "../include/status_bar.h"

struct TTouchFrame;

#ifndef GUI_EPD_UPDATE
#  define GUI_EPD_UPDATE(ctx_d)
#endif
#ifndef GUI_EPD_UPDATE_WINDOW
#  define GUI_EPD_UPDATE_WINDOW(ctx_d,x,y,w,h) GUI_EPD_UPDATE(ctx_d)
#endif

namespace einkui {

class UI {
public:
    explicit UI(Colors colors = {}) : colors_(colors) {}

    Parent* addPage(const char* name,
                    uint16_t style = STYLE_DISPLAY_FLEX | STYLE_VERTICAL) {
        auto p = std::make_shared<Parent>(name, style);
        pages_.push_back(p);
        if (!active_) active_ = p;
        return p.get();
    }
    void addPage(std::shared_ptr<Parent> p) {
        if (!p) return;
        pages_.push_back(p);
        if (!active_) active_ = p;
    }

    void setPage(const char* name) {
        for (auto& p : pages_)
            if (p->label() == name) { active_=p; dirty_=true; return; }
    }
    void setPage(Parent* raw) {
        for (auto& p : pages_)
            if (p.get()==raw) { active_=p; dirty_=true; return; }
    }
    void setPage(std::shared_ptr<Parent> p) { active_=p; dirty_=true; }

    Parent* activePage() const { return active_.get(); }

    // ---- layout & draw -------------------------------------------------------
    // pageArea() returns the rect available to pages (below the status bar).
    Rect pageArea(DrawCtx& ctx) const {
        int16_t W = ctx.W(), H = ctx.H();
        int16_t barH = statusBar_ ? int16_t(statusBar_->fixedH()) : 0;
        return Rect(0, barH, W, int16_t(H - barH));
    }

    void layout(DrawCtx& ctx, Rect area = {}) {
        if (!active_) return;
        if (area.empty()) area = pageArea(ctx);
        Rect cursor(area.x, area.y, 0, 0);
        active_->measure(ctx, cursor, area, 0);
    }

    void draw(DrawCtx& ctx, Rect area = {}, bool fullUpdate = true) {
        if (!active_) return;
        if (area.empty()) area = pageArea(ctx);
        ctx.fillScreen(ctx.d, colors_.bg);
        layout(ctx, area);
        active_->draw(ctx, colors_);
        // Draw status bar on top (after page, so it covers any overflow)
        if (statusBar_) {
            Rect barArea(0, 0, ctx.W(), int16_t(statusBar_->fixedH()));
            statusBar_->measure(ctx, Rect(0,0,0,0), barArea, 0);
            statusBar_->draw(ctx, colors_);
        }
        if (fullUpdate) { GUI_EPD_UPDATE(ctx.d); partialUpdateCount_ = 0; }
        dirty_ = false;
    }

    // Redraw a single element.  Touch feedback always uses a fast partial
    // window refresh; every kMaxPartials partial refreshes (or when partial is
    // false) a full refresh clears accumulated ghosting.
    static constexpr uint8_t kMaxPartials = 40;
    void redraw(DrawCtx& ctx, Element* elem, bool partial = true) {
        if (!elem) return;
        // clip to the screen — the driver works with unsigned coordinates
        Rect r = Rect(elem->x, elem->y, elem->w, elem->h) & Rect(0, 0, ctx.W(), ctx.H());
        if (r.empty()) return;
        ctx.fillRect(ctx.d, elem->x, elem->y, elem->w, elem->h, colors_.bg);
        elem->draw(ctx, colors_);
        if (partial && partialUpdateCount_ < kMaxPartials) {
            GUI_EPD_UPDATE_WINDOW(ctx.d, r.x, r.y, r.w, r.h);
            partialUpdateCount_++;
            return;
        }
        partialUpdateCount_ = 0;
        GUI_EPD_UPDATE(ctx.d);
    }

    // ---- touch ---------------------------------------------------------------
    //  Robust against what real e-paper + FT6X36 setups produce:
    //   • the frame handler runs in the touch task and blocks while the panel
    //     refreshes, so frames get merged: a quick tap may arrive as a single
    //     LiftUp frame, or a LiftUp may never arrive.
    //   • release points drift a few pixels from the press point.
    void onTouchFrame(DrawCtx& ctx, const TTouchFrame& tfIn) {
        if (!active_) return;
        TTouchFrame tf = tfIn;
        uint8_t ev = (uint8_t)tf.p[0].event;
        if (ev > 2) return;                                   // NoEvent

#ifdef EINKUI_TOUCH_DEBUG
        printf("[einkui] touch ev=%u x=%u y=%u n=%u gest=0x%02X focused=%p\n",
               ev, tf.p[0].x, tf.p[0].y, tf.touches, tf.gestureId, (void*)focused_.get());
#endif
        if (ev == 0 || ev == 2) { lastX_ = tf.p[0].x; lastY_ = tf.p[0].y; }

        // (1) New press while an old element is still focused → the previous
        //     release was lost.  Cancel it quietly (no action) and redraw it.
        if (ev == 0 && focused_) {
            auto stale = focused_;
            focused_ = nullptr;
            stale->cancelTouch();
            redraw(ctx, stale.get(), true);
        }

        // (2) Release for a touch whose press we never saw (merged while the
        //     panel was busy) → treat it as a complete tap.  Repeated identical
        //     release frames are ignored.
        if (ev == 1 && !focused_ && !touchActive_ &&
            !(tf.p[0].x == lastLiftX_ && tf.p[0].y == lastLiftY_)) {
            TTouchFrame down = tf;
            down.p[0].event = TRawEvent::PressDown;
            down.touches    = 1;
            down.gestureId  = 0;
            active_->onTouch(ctx, down, focused_);
        }
        if (ev == 1) { lastLiftX_ = tf.p[0].x; lastLiftY_ = tf.p[0].y; touchActive_ = false; }
        else touchActive_ = true;
        // Some controllers report no coordinates on lift: use the last point.
        if (ev == 1 && tf.p[0].x == 0 && tf.p[0].y == 0) { tf.p[0].x = lastX_; tf.p[0].y = lastY_; }

        std::shared_ptr<Element> prevFocused = focused_;
        std::shared_ptr<Parent>  pageBeforeTouch = active_;  // snapshot active page

        active_->onTouch(ctx, tf, focused_);

        // If the touch handler changed the active page (e.g. navigation on tap),
        // the display already shows the new page — do NOT redraw any element from
        // the old page on top of it.
        if (active_ != pageBeforeTouch) {
            focused_ = nullptr;   // old focus is stale
            if (dirty_) draw(ctx);  // setPage() without draw() in the handler
            return;
        }
        // A handler rebuilt part of the page (e.g. a list) and asked for a
        // full redraw via markDirty(): don't touch the old element, it may be
        // gone already.
        if (dirty_) {
            focused_ = nullptr;
            draw(ctx);
            return;
        }

        // Redraw the element that just changed state:
        //  - PressDown (ev=0): pressed look
        //  - LiftUp    (ev=1): released look
        //  - Move      (ev=2): only for elements that change while dragging
        Element* toRedraw = nullptr;
        if      (ev == 0 && focused_)                              toRedraw = focused_.get();
        else if (ev == 1 && prevFocused)                           toRedraw = prevFocused.get();
        else if (ev == 2 && focused_ && focused_->redrawOnMove())  toRedraw = focused_.get();

        if (toRedraw) redraw(ctx, toRedraw, /*partial=*/true);

        if (!focused_ && ev==0 && tf.gestureId && onGesture) onGesture(tf.gestureId);
    }

    // ---- status bar ----------------------------------------------------------
    // Attach a StatusBar to be drawn over every page.
    // The bar consumes the top barHeight() pixels; pages are laid out below.
    void setStatusBar(StatusBar* sb) { statusBar_ = sb; }
    StatusBar* statusBar() const { return statusBar_; }

    Colors& colors()  { return colors_; }
    // Swap black/white for the whole UI (dark mode).  Call draw() afterwards.
    void setInverted(bool inv) {
        if ((colors_.fg != 0) != inv) std::swap(colors_.fg, colors_.bg);
        dirty_ = true;
    }
    bool    dirty()   const { return dirty_; }
    void    markDirty()     { dirty_ = true;  }
    std::shared_ptr<Element> focused() const { return focused_; }

    std::function<void(uint8_t gestureId)> onGesture;

private:
    Colors                               colors_;
    StatusBar*                           statusBar_ = nullptr;
    std::vector<std::shared_ptr<Parent>> pages_;
    std::shared_ptr<Parent>              active_;
    std::shared_ptr<Element>             focused_;
    bool                                 dirty_ = true;
    uint8_t                              partialUpdateCount_ = 0;
    bool                                 touchActive_ = false;
    uint16_t                             lastX_ = 0, lastY_ = 0;
    uint16_t                             lastLiftX_ = 0xFFFF, lastLiftY_ = 0xFFFF;
};

} // namespace einkui