#pragma once
// ============================================================================
//  einkui/elements/keyboard.h
//
//  Keyboard — full QWERTY or numeric on-screen keyboard.
//
//  Layout (QWERTY, fills the area it is given — full page is typical):
//
//    WiFi password                      ✕      ← prompt, ✕ = cancel
//   ╭──────────────────────────────────╮
//   │ hunter2▌                         │       ← text field (shows the tail)
//   ╰──────────────────────────────────╯
//    1 2 3 4 5 6 7 8 9 0
//    q w e r t y u i o p
//     a s d f g h j k l
//    ⇧  z x c v b n m  ⌫
//    [     space      ][  OK  ]
//
//  Letters are lower case; ⇧ capitalises the next letter (tap twice for
//  caps-lock).  The key under the finger is shown inverted while pressed.
//
//  Callbacks:
//    onConfirm(ctx, this, text)  — OK pressed
//    onCancel (ctx, this)        — ✕ pressed
//
//  Usage:
//    auto* kb = ui.addPage("keyboard");
//    auto* k  = kb->add(new Keyboard()).get();
//    k->setPrompt("WiFi password");
//    k->onConfirm = [](void*, Element*, const char* text){ ... };
// ============================================================================
#include "../include/element.h"
#include "FT6X36.h"
#include <string>
#include <cstring>
#include <vector>

namespace einkui {

class Keyboard : public Element {
public:
    enum class Mode { QWERTY, NUMERIC };

    explicit Keyboard(Mode mode = Mode::QWERTY)
        : Element("", 0, 0, 0), mode_(mode) { padding_ = 4; }

    // Callbacks
    StringFn onConfirm = nullptr;  // fired when OK pressed
    TouchFn  onCancel  = nullptr;  // fired when ✕ pressed
    void*    cbCtx     = nullptr;

    void    setText (const std::string& t) { text_ = t; }
    const std::string& text() const        { return text_; }
    void    setPrompt(const char* p)       { prompt_ = p ? p : ""; }
    void    setMode(Mode m)                { mode_ = m; }
    void    cancelTouch() override         { pressedKey_ = -1; pressedSpecial_.clear(); }

    // ---- measure ------------------------------------------------------------
    Rect measure(DrawCtx& ctx, Rect cursor, Rect area, uint16_t pStyle) override {
        (void)ctx;
        if (style_ & STYLE_DISPLAY_FIXED) return Rect(x,y,w,h);
        placeAt(cursor, pStyle);
        w = resolveW(area);
        if      (height_  > 0) h = int16_t(height_);
        else if (layoutH_ > 0) h = int16_t(layoutH_);
        else                   h = int16_t(area.y2() - y - margin_);
        if (w < 1) w = 1;
        if (h < 1) h = 1;
        return Rect(x,y,w,h);
    }

    // ---- draw ---------------------------------------------------------------
    void draw(DrawCtx& ctx, Colors colors) override {
        if (style_ & STYLE_DISABLED) return;
        Colors c = resolveColors(colors);
        const Theme& t = ctx.th();
        ctx.fillRect(ctx.d, x, y, w, h, c.bg);

        const int16_t ix = int16_t(x + padding_), iw = int16_t(w - padding_*2);
        int16_t cy = int16_t(y + padding_);

        // Compact chrome when the keyboard is short (landscape)
        const bool compact = h < 200;
        // ---- header: prompt + close -------------------------------------------
        headerH_ = compact ? 18 : 24;
        closeRect_ = Rect(int16_t(ix + iw - headerH_), cy, headerH_, headerH_);
        ctx.textBox(ix, cy, int16_t(iw - headerH_ - 4), headerH_, prompt_.c_str(),
                    compact ? TEXT_BOLD : TEXT_TITLE, c.fg);
        if (pressedSpecial_ == "CANCEL") {
            ctx.fillCircle(int16_t(closeRect_.x + headerH_/2), int16_t(cy + headerH_/2), int16_t(headerH_/2), c.fg);
            ctx.icon("ui_close", int16_t(closeRect_.x + headerH_/2), int16_t(cy + headerH_/2), 12, c.bg, c.fg);
        } else {
            ctx.icon("ui_close", int16_t(closeRect_.x + headerH_/2), int16_t(cy + headerH_/2), 12, c.fg, c.bg);
        }
        cy = int16_t(cy + headerH_ + (compact ? 1 : 4));

        // ---- text field ---------------------------------------------------------
        const int16_t fh = compact ? 20 : 28;
        ctx.fillRoundRect(ctx.d, ix, cy, iw, fh, t.radiusSm, c.bg);
        ctx.drawRoundRect(ctx.d, ix, cy, iw, fh, t.radiusSm, c.fg);
        ctx.drawRoundRect(ctx.d, int16_t(ix+1), int16_t(cy+1), int16_t(iw-2), int16_t(fh-2), int16_t(t.radiusSm-1), c.fg);
        {
            // Show the end of the text when it is wider than the field
            const int16_t tx = int16_t(ix + 8), tw = int16_t(iw - 18);
            const char* s = text_.c_str();
            while (*s && ctx.textWidth(s, TEXT_BODY) > tw) ++s;
            int16_t used = ctx.textWidth(s, TEXT_BODY);
            ctx.textBox(tx, cy, tw, fh, s, TEXT_BODY, c.fg, TEXT_HALIGN_LEFT, TEXT_VALIGN_MIDDLE, false);
            DrawCtx::Metrics m = ctx.metrics(TEXT_BODY);
            ctx.fillRect(ctx.d, int16_t(tx + used + 1), int16_t(cy + (fh - m.cap)/2 - 2), 2, int16_t(m.cap + 4), c.fg);
        }
        cy = int16_t(cy + fh + (compact ? 3 : 6));

        keys_.clear();
        Rect keyArea(ix, cy, iw, int16_t(y + h - padding_ - cy));
        if (mode_ == Mode::NUMERIC) layoutNumeric(keyArea);
        else                        layoutQwerty (keyArea);
        for (size_t i = 0; i < keys_.size(); ++i) drawKey(ctx, c, keys_[i], int(i) == pressedKey_);
    }

    // ---- touch --------------------------------------------------------------
    bool onTouch(DrawCtx& ctx, const TTouchFrame& tf,
                 std::shared_ptr<Element>& focused) override
    {
        (void)ctx;
        uint8_t  ev=(uint8_t)tf.p[0].event;
        uint16_t tx=tf.p[0].x, ty=tf.p[0].y;

        if (ev==0) {
            if (touchExpand() || !contains(tx,ty)) return false;
            focused = std::shared_ptr<Element>(this,[](Element*){});
            pressedKey_ = keyAt(tx, ty);
            pressedSpecial_ = closeRect_.expanded(10).contains(tx, ty) ? "CANCEL" : "";
            return true;
        }
        if (ev==1 && focused.get()==this) {
            focused = nullptr;
            int k = pressedKey_;
            bool cancel = !pressedSpecial_.empty();
            pressedKey_ = -1;
            pressedSpecial_.clear();
            if (cancel) {
                if ((closeRect_.expanded(18).contains(tx, ty) || (tx == 0 && ty == 0)) && onCancel) onCancel(cbCtx, this);
                return true;
            }
            // accept the release on the pressed key or anywhere near it
            if (k >= 0 && (keyAt(tx, ty) == k || keys_[size_t(k)].r.expanded(8).contains(int16_t(tx), int16_t(ty))
                           || (tx == 0 && ty == 0)))
                activate(keys_[size_t(k)]);
            return true;
        }
        return false;
    }

private:
    Mode        mode_;
    std::string text_;
    std::string prompt_;
    uint8_t     shift_     = 0;      // 0 = off, 1 = next letter, 2 = caps lock
    int16_t     headerH_   = 24;
    Rect        closeRect_;
    int         pressedKey_ = -1;
    std::string pressedSpecial_;

    enum class Kind : uint8_t { Char, Shift, Del, Space, Ok };
    struct Key { Rect r; char ch; Kind kind; };
    std::vector<Key> keys_;

    static constexpr int16_t kGap = 3;

    // ---- layouts ----------------------------------------------------------------
    void layoutQwerty(Rect a) {
        static const char* rows[3] = { "1234567890", "qwertyuiop", "asdfghjkl" };
        const int16_t rowsN = 5;
        int16_t kh = int16_t((a.h - kGap * (rowsN - 1)) / rowsN);
        if (kh > 34) kh = 34;
        float unit = float(a.w + kGap) / 10.f;           // key pitch
        int16_t kw = int16_t(unit - kGap);
        int16_t ky = int16_t(a.y + a.h - (kh * rowsN + kGap * (rowsN - 1)));  // bottom-anchored

        for (int r = 0; r < 3; ++r) {
            const char* line = rows[r];
            int n = (int)strlen(line);
            float x0 = float(a.x) + (10 - n) * unit / 2.f;
            for (int i = 0; i < n; ++i) {
                char ch = line[i];
                if (r > 0 && shift_) ch = char(ch - 'a' + 'A');
                keys_.push_back({Rect(int16_t(x0 + i*unit), ky, kw, kh), ch, Kind::Char});
            }
            ky = int16_t(ky + kh + kGap);
        }
        // row 4: shift (1.5) zxcvbnm del (1.5)
        {
            const char* line = "zxcvbnm";
            int16_t wide = int16_t(unit * 1.5f - kGap);
            keys_.push_back({Rect(a.x, ky, wide, kh), 0, Kind::Shift});
            float x0 = float(a.x) + unit * 1.5f;
            for (int i = 0; i < 7; ++i) {
                char ch = line[i];
                if (shift_) ch = char(ch - 'a' + 'A');
                keys_.push_back({Rect(int16_t(x0 + i*unit), ky, kw, kh), ch, Kind::Char});
            }
            keys_.push_back({Rect(int16_t(a.x + a.w - wide), ky, wide, kh), 0, Kind::Del});
            ky = int16_t(ky + kh + kGap);
        }
        // row 5: space (7) OK (3)
        int16_t okW = int16_t(unit * 3 - kGap);
        keys_.push_back({Rect(a.x, ky, int16_t(a.w - okW - kGap), kh), ' ', Kind::Space});
        keys_.push_back({Rect(int16_t(a.x + a.w - okW), ky, okW, kh), 0, Kind::Ok});
    }

    void layoutNumeric(Rect a) {
        int16_t kw = int16_t((a.w - kGap * 2) / 3);
        int16_t kh = int16_t((a.h - kGap * 3) / 4);
        if (kh > 44) kh = 44;
        int16_t ky = int16_t(a.y + a.h - (kh * 4 + kGap * 3));
        for (int i = 1; i <= 9; ++i) {
            int16_t col = int16_t((i-1) % 3), row = int16_t((i-1) / 3);
            keys_.push_back({Rect(int16_t(a.x + col*(kw+kGap)), int16_t(ky + row*(kh+kGap)), kw, kh),
                             char('0'+i), Kind::Char});
        }
        int16_t y3 = int16_t(ky + 3*(kh+kGap));
        keys_.push_back({Rect(a.x, y3, kw, kh), 0, Kind::Del});
        keys_.push_back({Rect(int16_t(a.x + kw + kGap), y3, kw, kh), '0', Kind::Char});
        keys_.push_back({Rect(int16_t(a.x + 2*(kw + kGap)), y3, kw, kh), 0, Kind::Ok});
    }

    // ---- key rendering ----------------------------------------------------------
    void drawKey(DrawCtx& ctx, Colors c, const Key& k, bool pressed) const {
        const Theme& t = ctx.th();
        int16_t r = std::min<int16_t>(t.radiusSm, int16_t(std::min(k.r.w / 4, k.r.h / 3)));
        bool filled = (k.kind == Kind::Ok) || (k.kind == Kind::Shift && shift_ == 2);
        if (pressed) filled = !filled;
        uint16_t fg = filled ? c.bg : c.fg, bg = filled ? c.fg : c.bg;

        // keycap: body + 1px thicker bottom edge
        ctx.fillRoundRect(ctx.d, k.r.x, k.r.y, k.r.w, k.r.h, r, c.fg);
        if (!filled)
            ctx.fillRoundRect(ctx.d, int16_t(k.r.x+1), int16_t(k.r.y+1), int16_t(k.r.w-2), int16_t(k.r.h-3),
                              std::max<int16_t>(0, int16_t(r-1)), c.bg);

        int16_t cx = int16_t(k.r.x + k.r.w/2), cy = int16_t(k.r.y + (k.r.h - 1)/2);
        int16_t isz = std::min<int16_t>(16, int16_t(std::min(k.r.w, k.r.h) - 8));
        const bool num = (mode_ == Mode::NUMERIC);
        switch (k.kind) {
        case Kind::Char: {
            char s[2] = { k.ch, 0 };
            ctx.textBox(k.r.x, k.r.y, k.r.w, int16_t(k.r.h - 1), s, num ? TEXT_TITLE : TEXT_BODY, fg,
                        TEXT_HALIGN_CENTER, TEXT_VALIGN_MIDDLE, false);
            break; }
        case Kind::Shift:
            ctx.icon(shift_ ? "ui_shift_on" : "ui_shift", cx, cy, isz, fg, bg);
            break;
        case Kind::Del:
            ctx.icon("ui_backspace", cx, cy, isz, fg, bg);
            break;
        case Kind::Space:
            ctx.textBox(k.r.x, k.r.y, k.r.w, int16_t(k.r.h - 1), "space", TEXT_CAPTION, fg,
                        TEXT_HALIGN_CENTER, TEXT_VALIGN_MIDDLE, false);
            break;
        case Kind::Ok:
            ctx.textBox(k.r.x, k.r.y, k.r.w, int16_t(k.r.h - 1), "OK", TEXT_BOLD, fg,
                        TEXT_HALIGN_CENTER, TEXT_VALIGN_MIDDLE, false);
            break;
        }
    }

    int keyAt(uint16_t tx, uint16_t ty) const {
        for (size_t i = 0; i < keys_.size(); ++i) {
            // generous hit box: include half the gap around each key
            Rect hit = keys_[i].r.expanded(kGap / 2 + 1);
            if (hit.contains(int16_t(tx), int16_t(ty))) return int(i);
        }
        return -1;
    }

    void activate(const Key& k) {
        switch (k.kind) {
        case Kind::Char:
        case Kind::Space:
            text_ += k.ch;
            if (shift_ == 1 && k.kind == Kind::Char) shift_ = 0;
            break;
        case Kind::Shift:
            shift_ = uint8_t((shift_ + 1) % 3);
            break;
        case Kind::Del:
            if (!text_.empty()) {
                // remove one UTF-8 code point
                size_t n = text_.size() - 1;
                while (n > 0 && (uint8_t(text_[n]) & 0xC0) == 0x80) --n;
                text_.erase(n);
            }
            break;
        case Kind::Ok:
            if (onConfirm) onConfirm(cbCtx, this, text_.c_str());
            break;
        }
    }
};

} // namespace einkui
