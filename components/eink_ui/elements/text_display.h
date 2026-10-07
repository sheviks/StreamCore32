#pragma once
// ============================================================================
//  einkui/elements/text_display.h
//
//  TextDisplay — shows static text, or pulls live text from a getter function.
//
//  Usage:
//    // Static
//    page->add(new TextDisplay("Hello"));
//
//    // Dynamic (refreshed every draw)
//    auto* t = page->add(new TextDisplay("Artist"));
//    t->setGetValue([]{ return "Pink Floyd"; });
//    t->textCenter().setTextSize(TEXT_TITLE);   // TEXT_CAPTION/BODY/BOLD/TITLE/DISPLAY
// ============================================================================
#include "../include/element.h"

namespace einkui {

class TextDisplay : public Element {
public:
    explicit TextDisplay(const char* label="",
                         uint16_t fixedW=0, uint16_t fixedH=0)
        : Element(label, 0, fixedW, fixedH) {}

    // Live value getter.  If set, draw() will use this instead of label_.
    TextDisplay& setGetValue(const char* (*fn)()) { getFn_ = fn; return *this; }

    // Step down to a smaller font (TITLE → BOLD → BODY …) before shortening
    // the text with "..." when it does not fit the width.
    TextDisplay& autoShrink(bool v = true) { autoShrink_ = v; return *this; }

    // For heap-allocated strings: set directly before redraw.
    void setDynLabel(const std::string& s) { dynLabel_ = s; }

    Rect measure(DrawCtx& ctx, Rect cursor, Rect area, uint16_t pStyle) override {
        if (style_ & STYLE_DISPLAY_FIXED) return Rect(x,y,w,h);
        placeAt(cursor, pStyle);
        w = resolveW(area);
        h = resolveH(int16_t(ctx.metrics(textSize_).line + padding_*2));
        if (w < 1) w = 1;
        if (h < 1) h = 1;
        return Rect(x,y,w,h);
    }

    void draw(DrawCtx& ctx, Colors colors) override {
        if (style_ & STYLE_DISABLED) return;
        Colors c = resolveColors(colors);
        drawBackground(ctx, c);
        const char* s = currentText();
        std::string up;
        if (s && textSize_ == TEXT_TITLE && ctx.th().upperTitles) {
            up = upperText(s);
            s = up.c_str();
        }
        if (s && *s) {
            int16_t iw = int16_t(w - padding_*2);
            uint8_t role = textSize_;
            if (autoShrink_) {
                static const uint8_t order[] = { TEXT_DISPLAY, TEXT_TITLE, TEXT_BOLD, TEXT_BODY, TEXT_CAPTION };
                bool started = false;
                for (uint8_t r : order) {
                    if (r == textSize_) started = true;
                    if (!started) continue;
                    role = r;
                    if (ctx.textWidth(s, r) <= iw) break;
                }
            }
            // Text that does not fit is shortened with "..." (never wraps mid-word)
            ctx.textBox(int16_t(x + padding_), int16_t(y + padding_),
                        iw, int16_t(h - padding_*2), s, role, c.fg,
                        style2_ & TEXT_HALIGN_MASK, style2_ & TEXT_VALIGN_MASK);
        }
        drawRowDivider(ctx, c);
    }

    // TextDisplay is not interactive by default; onTouch falls through.

private:
    const char* (*getFn_)() = nullptr;
    std::string dynLabel_;
    bool        autoShrink_ = false;

    const char* currentText() const {
        if (getFn_) return getFn_();
        if (!dynLabel_.empty()) return dynLabel_.c_str();
        return label_.c_str();
    }
};

} // namespace einkui