#pragma once
// ============================================================================
//  einkui/pages/player_page.h
//
//  PlayerPage — pre-built "now playing" screen, laid out like the StreamCore32
//  web UI player (best with einkui::webTheme(): monospaced, square, flat):
//
//    ‹ NOW PLAYING                          ← btnBack · caption
//    ┌────────────────────────────┐
//    │░░░░░░░ STREAMCORE32 ░░░░░░░│         ← Artwork (only if there is room)
//    └────────────────────────────┘
//    DIGITAL LOVE                           ← title   (TEXT_TITLE)
//    Daft Punk                              ← artist  (TEXT_BODY)
//    Discovery                              ← album   (TEXT_CAPTION)
//    1:23 ━━━━━━━──────────────── 4:56      ← elapsed · progress · total
//     ⇄      ⏮      ⏸      ⏭      ↻         ← shuffle · prev · play · next · repeat
//    VOL ━━━━━━━━━━──────────── 42%         ← volume
//    ┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄
//    Source:  SD                            ← footer
//    Quality: FLAC 16/44.1
//    ┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄
//    UP NEXT  Harder, Better (+4)     ^     ← upNext (tap: queue)
//
//  Landscape: artwork on the left, everything else in a column on the right.
//  All handles are public so callbacks can be attached directly.
// ============================================================================
#include "../include/element.h"
#include "../elements/button.h"
#include "../elements/slider.h"
#include "../elements/text_display.h"
#include "../elements/icon.h"
#include <functional>

namespace einkui {

// ----------------------------------------------------------------------------
//  Artwork — cover placeholder: light grey square with a name in it (the web
//  UI's "STREAMCORE32" box).  Hidden when its slot is too small.
// ----------------------------------------------------------------------------
class Artwork : public Element {
public:
    explicit Artwork(const char* text = "STREAMCORE32") : Element(text, 0, 0, 0) {}

    Rect measure(DrawCtx& ctx, Rect cursor, Rect area, uint16_t pStyle) override {
        (void)ctx;
        if (style_ & STYLE_DISPLAY_FIXED) return Rect(x,y,w,h);
        placeAt(cursor, pStyle);
        w = resolveW(area);
        h = resolveH(0);
        return Rect(x,y,w,h);
    }

    void draw(DrawCtx& ctx, Colors colors) override {
        if (style_ & STYLE_DISABLED) return;
        Colors c = resolveColors(colors);
        int16_t sz = std::min(w, h);
        if (sz < kMin) return;                    // not enough room → hide
        int16_t ax = int16_t(x + (w - sz) / 2);
        ctx.fillRect(ctx.d, ax, y, sz, sz, c.bg);
        ctx.dither(int16_t(ax + 1), int16_t(y + 1), int16_t(sz - 2), int16_t(sz - 2), c.fg, 1);
        ctx.drawRoundRect(ctx.d, ax, y, sz, sz, ctx.th().radius, c.fg);
        // the name on a clean band
        int16_t th = int16_t(ctx.metrics(TEXT_CAPTION).line + 4);
        int16_t by = int16_t(y + (sz - th) / 2);
        ctx.fillRect(ctx.d, int16_t(ax + 1), by, int16_t(sz - 2), th, c.bg);
        ctx.textBox(int16_t(ax + 2), by, int16_t(sz - 4), th, label_.c_str(), TEXT_CAPTION, c.fg,
                    TEXT_HALIGN_CENTER, TEXT_VALIGN_MIDDLE);
    }
    static constexpr int16_t kMin = 56;
};

// ----------------------------------------------------------------------------
//  QueueBar — "UP NEXT  <next title> (+n)  ^" bar, a button.
// ----------------------------------------------------------------------------
class QueueBar : public Button {
public:
    QueueBar() : Button("UP NEXT", 0, 22) { ghost(); }
    void setPeek(const std::string& s) { peek_ = s; }
    const std::string& peek() const { return peek_; }

    Rect measure(DrawCtx& ctx, Rect cursor, Rect area, uint16_t pStyle) override {
        (void)ctx;
        if (style_ & STYLE_DISPLAY_FIXED) return Rect(x,y,w,h);
        placeAt(cursor, pStyle);
        w = resolveW(area);               // full width (a Button would fit its text)
        h = resolveH(22);
        return Rect(x,y,w,h);
    }

    void draw(DrawCtx& ctx, Colors colors) override {
        if (style_ & STYLE_DISABLED) return;
        Colors c = resolveColors(colors);
        uint16_t fg = c.fg, bg = c.bg;
        ctx.fillRect(ctx.d, x, y, w, h, pressed() ? fg : bg);
        if (pressed()) std::swap(fg, bg);
        ctx.hairline(x, y, w, fg);
        const int16_t pad = 4, cs = 10;
        int16_t lw = ctx.textWidth(label_.c_str(), TEXT_BOLD);
        ctx.textBox(int16_t(x + pad), int16_t(y + 1), lw, int16_t(h - 1), label_.c_str(), TEXT_BOLD, fg,
                    TEXT_HALIGN_LEFT, TEXT_VALIGN_MIDDLE);
        int16_t px = int16_t(x + pad + lw + 8);
        int16_t pw = int16_t(x + w - pad - cs - 6 - px);
        if (pw > 8 && !peek_.empty())
            ctx.textBox(px, int16_t(y + 1), pw, int16_t(h - 1), peek_.c_str(), TEXT_CAPTION, fg,
                        TEXT_HALIGN_LEFT, TEXT_VALIGN_MIDDLE);
        ctx.icon("ui_chevron_right", int16_t(x + w - pad - cs / 2), int16_t(y + h / 2), cs, fg, bg);
    }
private:
    std::string peek_;
};

// ----------------------------------------------------------------------------
//  OrientParent — flex container that calls a hook with the orientation of
//  the area it is about to fill, so a page can switch row/column layouts.
// ----------------------------------------------------------------------------
class OrientParent : public Parent {
public:
    using Hook = std::function<void(OrientParent*, bool landscape, Rect area)>;
    explicit OrientParent(Hook h)
        : Parent("", STYLE_DISPLAY_FLEX | STYLE_VERTICAL | STYLE_HIDE_LABEL), hook_(h) {}
    Rect measure(DrawCtx& ctx, Rect cursor, Rect area, uint16_t pStyle) override {
        int16_t hh = height_ > 0 ? int16_t(height_) : (layoutH_ > 0 ? int16_t(layoutH_) : area.h);
        int16_t ww = layoutW_ > 0 ? int16_t(layoutW_) : area.w;
        if (hook_) hook_(this, ww > hh * 6 / 5, Rect(area.x, area.y, ww, hh));
        return Parent::measure(ctx, cursor, area, pStyle);
    }
private:
    Hook hook_;
};

// a dotted rule between blocks
class Rule : public Element {
public:
    Rule() : Element("", STYLE_NO_FILL | STYLE_HIDE_LABEL, 0, 3) {}
    void draw(DrawCtx& ctx, Colors colors) override {
        if (style_ & STYLE_DISABLED) return;
        Colors c = resolveColors(colors);
        ctx.hairline(x, int16_t(y + h / 2), w, c.fg);
    }
};

class PlayerPage {
public:
    // Public element handles — attach your callbacks here
    Button*      btnBack    = nullptr;
    TextDisplay* caption    = nullptr;   // "NOW PLAYING"
    Artwork*     artwork    = nullptr;
    TextDisplay* title      = nullptr;
    TextDisplay* artist     = nullptr;
    TextDisplay* album      = nullptr;
    TextDisplay* elapsed    = nullptr;   // "1:23"
    TextDisplay* total      = nullptr;   // "4:56"
    Slider*      progress   = nullptr;   // 0..1 normalised
    Button*      btnShuffle = nullptr;
    Button*      btnPrev    = nullptr;
    Button*      btnPlay    = nullptr;
    Button*      btnNext    = nullptr;
    Button*      btnRepeat  = nullptr;
    Slider*      volume     = nullptr;   // 0..100
    TextDisplay* volLabel   = nullptr;   // "42%"
    TextDisplay* source     = nullptr;   // footer values
    TextDisplay* quality    = nullptr;
    QueueBar*    upNext     = nullptr;
    // Containers, e.g. for partial redraws of one row
    Parent*      progressRow  = nullptr;  // elapsed · progress · total
    Parent*      transportRow = nullptr;  // shuffle · prev · play · next · repeat
    Parent*      volumeRow    = nullptr;  // VOL · volume · 42%
    Parent*      footer       = nullptr;  // source / quality

    PlayerPage() {
        root_ = std::make_shared<Parent>("Player", STYLE_DISPLAY_FLEX | STYLE_VERTICAL);
        Parent* p = root_.get();
        p->setPadding(6).setSpacing(3);

        // ---- header -----------------------------------------------------------
        auto* hdr = p->add(new Parent("hdr", STYLE_DISPLAY_FLEX)).get();
        hdr->setHeight(22).setPadding(0).setSpacing(4).setMargin(0);
        btnBack = hdr->add(new Button("&ui_back", 22, 22)).get();
        btnBack->ghost().setIconSize(12).setMargin(0);
        caption = hdr->add(new TextDisplay("NOW PLAYING")).get();
        caption->setTextSize(TEXT_BOLD).textLeft().flexGrow().setPadding(0).setMargin(0);

        // ---- body: artwork + info.  Portrait: stacked.  Landscape: side by side.
        auto* body = new OrientParent([this](OrientParent* bp, bool landscape, Rect area){
            applyOrientation(bp, landscape, area);
        });
        p->add(std::shared_ptr<Parent>(body));
        body->flexGrow().setPadding(0).setSpacing(3).setMargin(0);

        artwork = body->add(new Artwork()).get();
        artwork->setMargin(0);

        info_ = body->add(new Parent("info", STYLE_DISPLAY_FLEX | STYLE_VERTICAL)).get();
        info_->setPadding(0).setSpacing(3).setMargin(0);
        Parent* q = info_;

        // ---- track info (left aligned, title in capitals) -----------------------
        title = q->add(new TextDisplay("TITLE")).get();
        title->setTextSize(TEXT_TITLE).textLeft().setPadding(0).setMargin(0);
        artist = q->add(new TextDisplay("Artist")).get();
        artist->setTextSize(TEXT_BODY).textLeft().setPadding(0).setMargin(0);
        album = q->add(new TextDisplay("Album")).get();
        album->setTextSize(TEXT_CAPTION).textLeft().setPadding(0).setMargin(0);

        // ---- progress: elapsed · bar · total ----------------------------------
        auto* prow = q->add(new Parent("prog", STYLE_DISPLAY_FLEX)).get();
        prow->setHeight(16).setPadding(0).setSpacing(5).setMargin(0);
        progressRow = prow;
        elapsed = prow->add(new TextDisplay("0:00", 28, 16)).get();
        elapsed->setTextSize(TEXT_CAPTION).setPadding(0).setMargin(0);
        progress = prow->add(new Slider("", 0.f, 1.f, 0, 16)).get();
        progress->addStyle(STYLE_HIDE_LABEL | STYLE_HIDE_VALUE);
        progress->setKnob(0).setTrack(3).setDecimals(2).setPadding(0);
        progress->flexGrow().setMargin(0);
        total = prow->add(new TextDisplay("0:00", 28, 16)).get();
        total->setTextSize(TEXT_CAPTION).textRight().setPadding(0).setMargin(0);

        // ---- transport: plain icons, evenly spread (web: flex auto) ----------
        transportRow = q->add(new Parent("transport", STYLE_DISPLAY_FLEX)).get();
        transportRow->setHeight(42).setPadding(0).setSpacing(0).setMargin(0);
        auto mk = [this](const char* icon, uint8_t size) {
            Button* b = transportRow->add(new Button(icon, 0, 42)).get();
            b->ghost().setIconSize(size);
            b->flexGrow().setMargin(0).setPadding(1);
            return b;
        };
        btnShuffle = mk("&shuffle", 16);   // modes at ~80 %, like the web UI
        btnPrev    = mk("&ui_prev", 20);
        btnPlay    = mk("&ui_play", 24);
        btnNext    = mk("&ui_next", 20);
        btnRepeat  = mk("&repeat", 16);

        // ---- volume: VOL · bar · 42% -----------------------------------------
        auto* vrow = q->add(new Parent("vol", STYLE_DISPLAY_FLEX)).get();
        vrow->setHeight(16).setPadding(0).setSpacing(5).setMargin(0);
        volumeRow = vrow;
        auto* vl = vrow->add(new TextDisplay("VOL", 22, 16)).get();
        vl->setTextSize(TEXT_CAPTION).setPadding(0).setMargin(0);
        volume = vrow->add(new Slider("Vol", 0.f, 100.f, 0, 16)).get();
        volume->addStyle(STYLE_HIDE_LABEL | STYLE_HIDE_VALUE);
        volume->setStep(1.f).setUnit("%").setDecimals(0).setKnob(0).setTrack(3).setPadding(0);
        volume->flexGrow().setMargin(0);
        volLabel = vrow->add(new TextDisplay("0%", 28, 16)).get();
        volLabel->setTextSize(TEXT_CAPTION).textRight().setPadding(0).setMargin(0);

        // ---- footer: Source / Quality ------------------------------------------
        p->add(new Rule())->setMargin(0);
        footer = p->add(new Parent("footer", STYLE_DISPLAY_FLEX | STYLE_VERTICAL)).get();
        footer->setPadding(0).setSpacing(0).setMargin(0);
        source  = footRow("Source:");
        quality = footRow("Quality:");

        // ---- up next ---------------------------------------------------------------
        upNext = p->add(new QueueBar()).get();
        upNext->setMargin(0);
    }

    std::shared_ptr<Parent> root() { return root_; }

    void setMeta(const char* a, const char* t, const char* al) {
        if (artist)  artist->setDynLabel(a  && *a  ? a  : " ");
        if (title)   title ->setDynLabel(t  && *t  ? t  : " ");
        if (album)   album ->setDynLabel(al && *al ? al : " ");
    }
    void setSource(const char* src, const char* q) {
        if (source)  source ->setDynLabel(src && *src ? src : "-");
        if (quality) quality->setDynLabel(q   && *q   ? q   : "-");
    }

    void setProgress(float v) { if (progress) progress->setValue(v); }
    void setVolume(float v) {
        if (volume) volume->setValue(v);
        if (volLabel) {
            char b[8];
            snprintf(b, sizeof b, "%d%%", int(v + 0.5f));
            volLabel->setDynLabel(b);
        }
    }
    // Elapsed / total time captions, e.g. setTimes("2:41", "6:23")
    void setTimes(const char* el, const char* tot) {
        if (elapsed) elapsed->setDynLabel(el  && *el  ? el  : " ");
        if (total)   total  ->setDynLabel(tot && *tot ? tot : " ");
    }
    // Switch the play button between ▶ and ❚❚
    void setPlaying(bool playing) { if (btnPlay) btnPlay->setLabel(playing ? "&ui_pause" : "&ui_play"); }
    void setPlayLabel(const char* l) { if (btnPlay) btnPlay->setLabel(l); }

private:
    std::shared_ptr<Parent> root_;
    Parent* info_ = nullptr;

    TextDisplay* footRow(const char* label) {
        auto* row = footer->add(new Parent(label, STYLE_DISPLAY_FLEX)).get();
        row->setHeight(13).setPadding(0).setSpacing(4).setMargin(0);
        auto* l = row->add(new TextDisplay(label, 52, 13)).get();
        l->setTextSize(TEXT_CAPTION).setPadding(0).setMargin(0);
        auto* v = row->add(new TextDisplay("-", 0, 13)).get();
        v->setTextSize(TEXT_CAPTION).textLeft().flexGrow().setPadding(0).setMargin(0);
        return v;
    }

    // Portrait: artwork on top (only when there is room), info below.
    // Landscape: square artwork on the left, info column on the right.
    void applyOrientation(OrientParent* body, bool landscape, Rect area) {
        if (landscape) {
            body->clearStyle(STYLE_VERTICAL).setSpacing(8);
            artwork->flexGrow(false);
            int16_t art = std::min<int16_t>(int16_t(area.h - 2), int16_t(area.w * 2 / 5));
            if (art < 0) art = 0;
            artwork->setWidth(uint16_t(art));
            artwork->setHeight(uint16_t(art));
            info_->flexGrow(true);
            info_->setSpacing(2);
        } else {
            body->addStyle(STYLE_VERTICAL).setSpacing(3);
            artwork->setWidth(0).setHeight(0);
            artwork->flexGrow(true);
            info_->flexGrow(false);
            info_->setSpacing(5);
        }
    }
};

} // namespace einkui
