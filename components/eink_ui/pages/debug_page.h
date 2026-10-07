#pragma once
// ============================================================================
//  einkui/pages/debug_page.h
//
//  DebugPage — scrolling text log.  Fixed circular buffer on the stack
//  (no heap for lines themselves).  Caller pushes lines; page renders
//  the last N that fit.
//
//  Usage:
//    DebugPage dbg;
//    ui.addPage(dbg.root());
//    dbg.push("boot OK");
//    // in logger callback:
//    dbg.push(line.c_str());
// ============================================================================
#include "../include/element.h"
#include <array>
#include <cstring>
#include <cstdio>
#include <string>

namespace einkui {

// Number of log lines kept in the circular buffer.
// Reduce if stack is tight (each line is 80 chars).
static constexpr size_t DBG_LINES = 16;
static constexpr size_t DBG_LINE_LEN = 80;

class DebugPage : public Element {
public:
    DebugPage() : Element("", 0, 0, 0) {
        for (auto& l : lines_) l[0] = '\0';
    }

    void push(const char* line) {
        strncpy(lines_[head_], line, DBG_LINE_LEN-1);
        lines_[head_][DBG_LINE_LEN-1] = '\0';
        head_ = (head_+1) % DBG_LINES;
        if (count_ < DBG_LINES) ++count_;
        ++total_;
    }

    void clear() { head_=0; count_=0; total_=0; for (auto& l:lines_) l[0]='\0'; }

    std::shared_ptr<Parent> root() {
        if (!root_) {
            root_ = std::make_shared<Parent>("debug", STYLE_DISPLAY_FLEX|STYLE_VERTICAL);
            root_->add(std::shared_ptr<DebugPage>(this,[](DebugPage*){}));
        }
        return root_;
    }

    // ---- measure ------------------------------------------------------------
    Rect measure(DrawCtx& ctx, Rect cursor, Rect area, uint16_t pStyle) override {
        (void)ctx;
        if (style_ & STYLE_DISPLAY_FIXED) return Rect(x,y,w,h);
        placeAt(cursor, pStyle);
        w = resolveW(area);
        h = height_ > 0 ? int16_t(height_) : int16_t(area.y2() - y - margin_);
        return Rect(x,y,w,h);
    }

    // ---- draw ---------------------------------------------------------------
    //   Log                     4 lines
    //   ┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄
    //   01 │ boot OK
    //   02 │ wifi: connected            (monospace, newest at the bottom)
    void draw(DrawCtx& ctx, Colors colors) override {
        if (style_ & STYLE_DISABLED) return;
        Colors c = resolveColors(colors);
        ctx.fillRect(ctx.d, x, y, w, h, c.bg);

        const int16_t px = 8;
        // header
        int16_t hdrH = title_.empty() ? int16_t(0) : int16_t(ctx.metrics(TEXT_TITLE).cap + 14);
        if (hdrH) {
        ctx.textBox(int16_t(x + px), y, int16_t(w - px*2), hdrH,
                    ctx.th().upperTitles ? upperText(title_.c_str()).c_str() : title_.c_str(), TEXT_TITLE, c.fg);
        char cnt[16]; snprintf(cnt, sizeof(cnt), "%u line%s", (unsigned)count_, count_ == 1 ? "" : "s");
        ctx.textBox(int16_t(x + px), y, int16_t(w - px*2), hdrH, cnt, TEXT_CAPTION, c.fg, TEXT_HALIGN_RIGHT);
        ctx.hairline(int16_t(x + px), int16_t(y + hdrH - 1), int16_t(w - px*2), c.fg);
        }

        DrawCtx::Metrics m = ctx.metrics(TEXT_MONO);
        int16_t lineH = int16_t(m.cap + m.desc + 4);
        int16_t top   = int16_t(y + hdrH + 4);
        int16_t maxLines = int16_t((y + h - top - 2) / lineH);
        if (maxLines < 1) return;
        int16_t shown = int16_t(std::min<size_t>(count_, size_t(maxLines)));
        size_t  first = count_ - size_t(shown);          // index (0 = oldest kept)
        int16_t gutter = ctx.textWidth("000", TEXT_MONO);
        int16_t tx = int16_t(x + px + gutter + 8);
        for (int16_t i = 0; i < shown; ++i) {
            size_t idx = (head_ + DBG_LINES - count_ + first + size_t(i)) % DBG_LINES;
            int16_t ly = int16_t(top + i * lineH);
            char num[8]; snprintf(num, sizeof(num), "%02u", (unsigned)((total_ - count_ + first + size_t(i) + 1) % 1000));
            ctx.textBox(int16_t(x + px), ly, gutter, lineH, num, TEXT_MONO, c.fg, TEXT_HALIGN_RIGHT);
            ctx.vHairline(int16_t(tx - 5), ly, lineH, c.fg);
            ctx.textBox(tx, ly, int16_t(x + w - px - tx), lineH, lines_[idx], TEXT_MONO, c.fg);
        }
    }

    // Header title ("Log" by default); empty string hides the header.
    void setTitle(const char* t) { title_ = t ? t : ""; }

private:
    char   lines_[DBG_LINES][DBG_LINE_LEN];
    size_t head_  = 0;
    size_t count_ = 0;
    size_t total_ = 0;            // lines pushed since clear() (for numbering)
    std::string title_ = "Log";
    std::shared_ptr<Parent> root_;
};

} // namespace einkui
