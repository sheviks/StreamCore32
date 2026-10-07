#pragma once
// ============================================================================
//  einkui/elements/select.h
//
//  Select — scrollable list of string options.  Draws the selected value with
//  a small triangle indicator.  On tap, expands an in-place option list;
//  tapping an option fires onChange and collapses.
//
//  Options are provided via:
//    a) push_back at build time:  sel->addOption("A"); sel->addOption("B");
//    b) a count/name interface:   sel->setSource(count, nameFn);
//       where nameFn(index) returns const char*.
//
//  The expanded list renders inline (no popup/overlay), so it works without
//  z-ordering magic.  Keep option count <= ~8 for usability on small displays.
// ============================================================================
#include "../include/element.h"
#include "FT6X36.h"
#include <vector>
#include <string>

namespace einkui {

class Select : public Element {
public:
    explicit Select(const char* label="",
                    uint16_t fixedW=0, uint16_t fixedH=0)
        : Element(label, STYLE_BORDER, fixedW, fixedH) {}

    // ---- options ------------------------------------------------------------
    Select& addOption(const char* opt) { options_.push_back(opt); return *this; }
    Select& clearOptions()             { options_.clear(); expanded_=false; return *this; }

    // Dynamic source: provide count + name function instead of static list
    Select& setSource(size_t (*countFn)(), const char* (*nameFn)(size_t)) {
        countFn_ = countFn; nameFn_ = nameFn; return *this;
    }

    size_t      selected()    const { return selected_; }
    const char* selectedName()const {
        if (nameFn_) return nameFn_(selected_);
        if (selected_ < options_.size()) return options_[selected_].c_str();
        return "";
    }
    void setSelected(size_t i) { selected_ = i < optCount() ? i : 0; }

    Select& onChange(StringFn fn, void* ctx=nullptr){ cb().onChange=fn; cb().ctx=ctx; return *this; }

    // ---- measure ------------------------------------------------------------
    Rect measure(DrawCtx& ctx, Rect cursor, Rect area, uint16_t pStyle) override {
        if (style_ & STYLE_DISPLAY_FIXED) return Rect(x,y,w,h);
        placeAt(cursor, pStyle);
        rowH_ = std::max<int16_t>(int16_t(ctx.th().rowH - 2), int16_t(ctx.metrics(textSize_).line + padding_*2));
        hdrH_ = height_ > 0 ? int16_t(height_) : rowH_;
        int16_t listH = expanded_ ? int16_t(rowH_ * (int16_t)optCount() + 6) : 0;
        w = resolveW(area);
        h = int16_t(hdrH_ + listH);
        if (w < 1) w = 1;
        if (h < 1) h = 1;
        return Rect(x,y,w,h);
    }

    // ---- draw ---------------------------------------------------------------
    void draw(DrawCtx& ctx, Colors colors) override {
        if (style_ & STYLE_DISABLED) return;
        Colors c = resolveColors(colors);
        const Theme& t = ctx.th();
        const int16_t r = t.radiusSm;
        const int16_t px = 8;

        // ---- field ---------------------------------------------------------------
        ctx.fillRoundRect(ctx.d, x, y, w, hdrH_, r, expanded_ ? c.fg : c.bg);
        ctx.drawRoundRect(ctx.d, x, y, w, hdrH_, r, c.fg);
        uint16_t fg = expanded_ ? c.bg : c.fg, bg = expanded_ ? c.fg : c.bg;

        const int16_t chev = 12;
        int16_t right = int16_t(x + w - px);
        ctx.icon(expanded_ ? "ui_chevron_up" : "ui_chevron_down",
                 int16_t(right - chev/2), int16_t(y + hdrH_/2), chev, fg, bg);
        right = int16_t(right - chev - 6);

        int16_t lw = 0;
        if (!label_.empty() && !(style_ & STYLE_HIDE_LABEL)) {
            lw = ctx.textWidth(label_.c_str(), textSize_);
            ctx.textBox(int16_t(x + px), y, lw, hdrH_, label_.c_str(), textSize_, fg);
            lw += 10;
        }
        const char* val = selectedName();
        if (val && *val)
            ctx.textBox(int16_t(x + px + lw), y, int16_t(right - x - px - lw), hdrH_, val,
                        TEXT_BOLD, fg, lw ? TEXT_HALIGN_RIGHT : TEXT_HALIGN_LEFT);

        if (!expanded_) return;

        // ---- option panel (with shadow) ------------------------------------------
        int16_t py = int16_t(y + hdrH_ + 2);
        int16_t ph = int16_t(h - hdrH_ - 2);
        Rect body = ctx.surface(x, py, w, ph, r, c.fg, c.bg, false);
        int16_t oy = int16_t(body.y + 2);
        size_t cnt = optCount();
        for (size_t i = 0; i < cnt; ++i) {
            bool sel = (i == selected_);
            const char* name = optName(i);
            int16_t tx = int16_t(body.x + px + 18);
            if (sel) ctx.icon("ui_check", int16_t(body.x + px + 6), int16_t(oy + rowH_/2), 12, c.fg, c.bg);
            ctx.textBox(tx, oy, int16_t(body.x + body.w - px - tx), rowH_, name,
                        sel ? TEXT_BOLD : textSize_, c.fg);
            if (i + 1 < cnt) ctx.hairline(int16_t(body.x + px), int16_t(oy + rowH_ - 1), int16_t(body.w - px*2), c.fg);
            oy += rowH_;
        }
    }

    // ---- touch --------------------------------------------------------------
    bool onTouch(DrawCtx& /*ctx*/, const TTouchFrame& tf,
                 std::shared_ptr<Element>& focused) override
    {
        uint8_t ev=(uint8_t)tf.p[0].event;
        uint16_t tx=tf.p[0].x, ty=tf.p[0].y;

        if (ev==0 && hitTest(tx,ty)) {
            focused=std::shared_ptr<Element>(this,[](Element*){});
            return true;
        }
        if (ev==1 && focused.get()==this) {
            focused=nullptr;
            if (!releaseHit(tx,ty)) return true;
            if (ty < uint16_t(y)) ty = uint16_t(y);
            if (!expanded_) { expanded_=true; return true; }
            // Check header row → collapse
            if (ty < uint16_t(y + hdrH_)) { expanded_=false; return true; }
            // Check option rows
            if (ty < uint16_t(y + hdrH_ + 4)) return true;
            size_t idx = (ty - uint16_t(y + hdrH_ + 4)) / uint16_t(rowH_);
            if (idx < optCount()) {
                selected_ = idx;
                expanded_ = false;
                if (hasCb() && cb_->onChange) cb_->onChange(cb_->ctx, this, optName(idx));
            }
            return true;
        }
        return false;
    }

private:
    std::vector<std::string> options_;
    size_t  selected_ = 0;
    bool    expanded_ = false;
    int16_t rowH_     = 26;
    int16_t hdrH_     = 26;
    size_t  (*countFn_)()              = nullptr;
    const char* (*nameFn_)(size_t)    = nullptr;

    size_t optCount() const {
        if (countFn_) return countFn_();
        return options_.size();
    }
    const char* optName(size_t i) const {
        if (nameFn_) return nameFn_(i);
        if (i < options_.size()) return options_[i].c_str();
        return "";
    }

};

} // namespace einkui