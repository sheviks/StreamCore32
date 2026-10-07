#pragma once
// ============================================================================
//  einkui/elements/status_bar.h
//
//  StatusBar — thin fixed bar drawn at the top of every page.
//
//  Layout (left → right):
//    [  time  ]  [  wifi  ] [  vol  ] [  batt  ]
//
//  All values are pulled via getter functions registered by the caller.
//  If a getter is not set, that indicator is simply omitted.
//
//  The bar is drawn as plain pixels (no flex layout) so it never participates
//  in page layout — it overlays the top of the screen on every draw().
//
//  Usage:
//    auto* sb = new einkui::StatusBar(16);   // 16px height (default)
//    sb->setTimeGetter([]{ return "14:32"; });
//    sb->setWifiGetter([]{ return 3; });      // 0=off, 1-4 = signal strength
//    sb->setVolumeGetter([]{ return 2; });    // 0=mute, 1=low, 2=mid, 3=high
//    sb->setBatteryGetter([]{ return 75; });  // 0..100 %
//    ui.setStatusBar(sb);
//
//  UI integration:
//    UI::draw() draws the active page in {0, barH, W, H-barH},
//    then draws the status bar in {0, 0, W, barH}.
//    The status bar area is never filled by fillScreen — it's always redrawn
//    on top.
// ============================================================================
#include "../include/element.h"
#include <cstdio>
#include <cmath>
#include <functional>

namespace einkui {

class StatusBar : public Element {
public:
    explicit StatusBar(uint8_t height = 16)
        : Element("", STYLE_NO_FILL, 0, height)
    {}

    // ---- value getters -------------------------------------------------------
    // Called on every draw() — return the current live value.

    /// Returns a time string, e.g. "14:32"
    StatusBar& setTimeGetter   (std::function<const char*()> fn) { timeFn_   = fn; return *this; }
    StatusBar& setWifiGetter   (std::function<int()>         fn) { wifiFn_   = fn; return *this; }
    StatusBar& setVolumeGetter (std::function<int()>         fn) { volFn_    = fn; return *this; }
    StatusBar& setBatteryGetter(std::function<int()>         fn) { battFn_   = fn; return *this; }
    StatusBar& setChargingGetter(std::function<bool()>       fn) { chargeFn_ = fn; return *this; }

    // ---- measure -------------------------------------------------------------
    // The status bar always fills the full display width at the top.
    Rect measure(DrawCtx& ctx, Rect cursor, Rect area, uint16_t) override {
        (void)cursor; (void)ctx;
        x = area.x;
        y = area.y;
        w = area.w;
        h = int16_t(height_ > 0 ? height_ : 16);
        return Rect(x, y, w, h);
    }

    // ---- draw ----------------------------------------------------------------
    void draw(DrawCtx& ctx, Colors colors) override {
        Colors c = resolveColors(colors);
        ctx.fillRect(ctx.d, x, y, w, h, c.bg);
        ctx.hairline(x, int16_t(y + h - 1), w, c.fg);

        const int16_t pad = 5;
        const int16_t iH  = int16_t(h - 4);              // icon box height
        const int16_t cy  = int16_t(y + (h - 1) / 2);    // vertical centre
        int16_t rightX = int16_t(x + w - pad);

        if (battFn_) {
            bool charging = chargeFn_ && chargeFn_();
            rightX = drawBattery(ctx, rightX, cy, iH, battFn_(), charging, c);
            rightX = int16_t(rightX - 7);
        }
        if (wifiFn_) {
            rightX = drawWifi(ctx, rightX, cy, iH, wifiFn_(), c);
            rightX = int16_t(rightX - 7);
        }
        if (volFn_) {
            rightX = drawVolume(ctx, rightX, cy, iH, volFn_(), c);
            rightX = int16_t(rightX - 7);
        }
        if (timeFn_) {
            const char* t = timeFn_();
            if (t && *t)
                ctx.textBox(int16_t(x + pad), y, int16_t(rightX - x - pad), int16_t(h - 1), t, TEXT_BOLD, c.fg);
        }
    }

private:
    std::function<const char*()> timeFn_;
    std::function<int()>         wifiFn_;
    std::function<int()>         volFn_;
    std::function<int()>         battFn_;
    std::function<bool()>        chargeFn_;

    // All draw helpers draw leftwards from rightX and return the new left edge.

    // ---- Battery: "80%" + rounded body with proportional fill + nub ---------
    static int16_t drawBattery(DrawCtx& ctx, int16_t rightX, int16_t cy, int16_t iH,
                               int pct, bool charging, Colors c)
    {
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        int16_t bH = std::max<int16_t>(7, int16_t(iH - 3));
        int16_t bW = int16_t(bH * 2 + 3);
        int16_t nW = 2, nH = std::max<int16_t>(3, int16_t(bH / 2));
        int16_t bx = int16_t(rightX - bW - nW);
        int16_t by = int16_t(cy - bH / 2);
        ctx.drawRoundRect(ctx.d, bx, by, bW, bH, 2, c.fg);
        ctx.fillRect(ctx.d, int16_t(bx + bW), int16_t(cy - nH / 2), nW, nH, c.fg);
        int16_t fw = int16_t((pct * (bW - 4) + 50) / 100);
        if (fw > 0) ctx.fillRect(ctx.d, int16_t(bx + 2), int16_t(by + 2), fw, int16_t(bH - 4), c.fg);
        if (charging) {
            // small lightning bolt, knocked out of the fill
            int16_t mx = int16_t(bx + bW / 2), my = cy;
            uint16_t col = fw > bW / 2 ? c.bg : c.fg;
            ctx.drawLine(ctx.d, int16_t(mx + 1), int16_t(my - 3), int16_t(mx - 2), int16_t(my), col);
            ctx.drawLine(ctx.d, int16_t(mx - 2), int16_t(my), int16_t(mx + 2), int16_t(my), col);
            ctx.drawLine(ctx.d, int16_t(mx + 2), int16_t(my), int16_t(mx - 1), int16_t(my + 3), col);
        }
        char buf[6]; snprintf(buf, sizeof(buf), "%d%%", pct);
        int16_t tw = ctx.textWidth(buf, TEXT_CAPTION);
        int16_t tx = int16_t(bx - 4 - tw);
        ctx.text(tx, int16_t(cy + ctx.metrics(TEXT_CAPTION).cap / 2), buf, TEXT_CAPTION, c.fg);
        return tx;
    }

    // ---- WiFi: classic fan, active rings solid, inactive dotted --------------
    //  strength: 0 = disconnected (dotted, with a slash), 1..4
    static int16_t drawWifi(DrawCtx& ctx, int16_t rightX, int16_t cy, int16_t iH,
                            int strength, Colors c)
    {
        int16_t R  = std::max<int16_t>(6, int16_t(iH - 1));     // outer radius
        int16_t ox = int16_t(rightX - R);                        // fan apex x
        int16_t oy = int16_t(cy + R / 2);                        // fan apex y
        int levels = 3;
        int lit = strength <= 0 ? 0 : (strength >= 4 ? 3 : (strength == 3 ? 3 : strength));
        ctx.fillCircle(ox, int16_t(oy - 1), 1, c.fg);
        for (int i = 1; i <= levels; ++i) {
            int16_t r = int16_t(1 + i * (R - 1) / levels);
            if (i <= lit) ctx.arc(ox, oy, r, 2, -45.f, 45.f, c.fg);
            else          ctx.ditherArc(ox, oy, r, int16_t(r - 1), -45.f, 45.f, c.fg, 2);
        }
        if (strength <= 0)
            ctx.drawLine(ctx.d, int16_t(ox - R/2), int16_t(oy - R), int16_t(ox + R/2), oy, c.fg);
        return int16_t(ox - R * 3 / 4);
    }

    // ---- Volume: speaker icon (mute / low / high) ----------------------------
    static int16_t drawVolume(DrawCtx& ctx, int16_t rightX, int16_t cy, int16_t iH,
                              int level, Colors c)
    {
        int16_t sz = std::max<int16_t>(10, int16_t(iH + 1));
        const char* name = level <= 0 ? "volume_x" : (level == 1 ? "ui_vol_low" : "ui_vol_high");
        ctx.icon(name, int16_t(rightX - sz / 2), cy, sz, c.fg, c.bg);
        return int16_t(rightX - sz);
    }
};

} // namespace einkui