#pragma once
// ============================================================================
//  einkui/pages/icon_menu.h
//
//  IconMenu — adaptive icon grid main menu.
//
//  ---- Layout -----------------------------------------------------------------
//  Uses a single Grid(cols, rows) that fills the page.  Grid dimensions auto-
//  adapt to display rotation:
//    Portrait  (W < H): use configured portrait cols/rows
//    Landscape (W ≥ H): swap cols↔rows so icons use the wider axis
//
//  ---- Pagination -------------------------------------------------------------
//  If more icons are registered than cols×rows, they are split into pages.
//  Only the current page's icons are placed into the grid.
//  Page indicator dots are shown at the bottom when there are multiple pages.
//
//  ---- Gestures ---------------------------------------------------------------
//  GESTURE_MOVE_UP   / GESTURE_MOVE_DOWN  → scroll pages (previous / next)
//  GESTURE_ZOOM_IN   → show fewer icons per page (grow cells: cols-1, rows-1)
//  GESTURE_ZOOM_OUT  → show more icons per page  (shrink cells: cols+1, rows+1)
//
//  ---- Usage ------------------------------------------------------------------
//    auto* menu = new einkui::IconMenu();
//    menu->setPortraitGrid(2, 3);   // 2 cols × 3 rows in portrait
//    menu->setTitle("Home");
//    menu->add(IconMenu::MUSIC,    "Player",   [&]{ ui.setPage("Player"); ui.draw(ctx); });
//    menu->add(IconMenu::SETTINGS, "Settings", [&]{ ui.setPage("Settings"); ui.draw(ctx); });
//    // ...add as many as you like — they paginate automatically
//    ui.addPage(menu->root());
//    // Register ui + ctx so gestures can trigger redraws:
//    menu->setUIContext(&ui, &ctx);
// ============================================================================

#include "../include/element.h"
#include "../include/grid.h"
#include "../elements/text_display.h"
#include <functional>
#include <vector>
#include <cstring>
#include <cmath>

namespace einkui {

// Forward declare UI to avoid circular include — caller passes pointers
class UI;

// ============================================================================
//  IconCell — one tappable icon+label tile
// ============================================================================
class IconCell : public Element {
public:
    enum IconType {
        MUSIC, SETTINGS, FILES, DEBUG, WIFI, POWER, HEART,
        VG,     // draw using label_ as a VG asset name (label must start with '&')
        CUSTOM  // caller-supplied DrawFn
    };
    using DrawFn = void(*)(DrawCtx&, int16_t cx, int16_t cy,
                            int16_t sz, uint16_t fg, uint16_t bg);

    IconCell(IconType icon, const char* label, std::function<void()> action)
        : Element(label, STYLE_BORDER, 0, 0)
        , icon_(icon), action_(action), customDraw_(nullptr)
    { padding_ = 3; radius_ = 8; margin_ = 0; }

    IconCell(DrawFn fn, const char* label, std::function<void()> action)
        : Element(label, STYLE_BORDER, 0, 0)
        , icon_(CUSTOM), action_(action), customDraw_(fn)
    { padding_ = 3; radius_ = 8; margin_ = 0; }

    bool pressed() const { return pressed_; }
    void cancelTouch() override { pressed_ = false; }

    // Override the text shown below the icon (when different from label_,
    // e.g. when label_ holds a VG asset name like "&home")
    void setDisplayLabel(const char* s) { displayLabel_ = s ? s : ""; }

    Rect measure(DrawCtx&, Rect cursor, Rect area, uint16_t) override {
        if (style_ & STYLE_DISPLAY_FIXED) return Rect(x,y,w,h);
        placeAt(cursor, 0);
        w = resolveW(area);
        if      (height_  > 0) h = int16_t(height_);
        else if (layoutH_ > 0) h = int16_t(layoutH_);
        else                   h = w;
        if (w < 1) w = 1;
        if (h < 1) h = 1;
        return Rect(x,y,w,h);
    }

    void draw(DrawCtx& ctx, Colors colors) override {
        if (style_ & STYLE_DISABLED) return;
        Colors c = resolveColors(colors);
        const Theme& t = ctx.th();

        // Tile surface with hard shadow; sinks + inverts while pressed
        Rect body = ctx.surface(x, y, w, h, t.radius, c.fg, c.bg, pressed_);
        uint16_t fg = pressed_ ? c.bg : c.fg, bg = pressed_ ? c.fg : c.bg;

        const char* lbl = displayLabel_.empty() ? label_.c_str() : displayLabel_.c_str();
        if (lbl && *lbl == '&') lbl = nullptr;            // VG name, not a caption
        DrawCtx::Metrics m = ctx.metrics(TEXT_BODY);
        int16_t lblH = lbl && *lbl ? int16_t(m.cap + m.desc + 8) : 0;

        // Wide, short tiles (landscape): icon left of the label
        if (lblH && body.w * 10 > body.h * 17) {
            int16_t isz = std::min<int16_t>(16, int16_t(body.h - 12));
            int16_t tw  = ctx.textWidth(lbl, TEXT_BODY);
            int16_t gw  = std::min<int16_t>(int16_t(isz + 6 + tw), int16_t(body.w - 8));
            int16_t gx  = int16_t(body.x + (body.w - gw) / 2);
            int16_t cyy = int16_t(body.y + body.h / 2);
            if (icon_ == CUSTOM && customDraw_) customDraw_(ctx, int16_t(gx + isz/2), cyy, isz, fg, bg);
            else drawBuiltinIcon(ctx, int16_t(gx + isz/2), cyy, isz, fg, bg);
            ctx.textBox(int16_t(gx + isz + 6), body.y, int16_t(gw - isz - 6), body.h, lbl, TEXT_BODY, fg);
            return;
        }
        // Icon: centred in the area above the label
        int16_t areaH = int16_t(body.h - lblH - 6);
        int16_t sz = int16_t(std::min<int16_t>(body.w * 2 / 3, areaH) * 17 / 20);
        if (sz > 44) sz = 44;
        if (sz < 8)  sz = 8;
        int16_t icx = int16_t(body.x + body.w / 2);
        int16_t icy = int16_t(body.y + 6 + areaH / 2 + (lblH ? 2 : 0));

        if (icon_ == CUSTOM && customDraw_)
            customDraw_(ctx, icx, icy, sz, fg, bg);
        else
            drawBuiltinIcon(ctx, icx, icy, sz, fg, bg);

        if (lblH)
            ctx.textBox(int16_t(body.x + 4), int16_t(body.y + body.h - lblH - 2), int16_t(body.w - 8), lblH,
                        lbl, TEXT_BODY, fg, TEXT_HALIGN_CENTER, TEXT_VALIGN_MIDDLE);
    }

    bool onTouch(DrawCtx&, const TTouchFrame& tf,
                 std::shared_ptr<Element>& focused) override
    {
        uint8_t ev = (uint8_t)tf.p[0].event;
        uint16_t tx = tf.p[0].x, ty = tf.p[0].y;
        if (ev == 0 && hitTest(tx,ty)) {
            pressed_ = true;
            focused  = std::shared_ptr<Element>(this, [](Element*){});
            return true;
        }
        if (ev == 1 && focused.get() == this) {
            bool inside = releaseHit(tx,ty);
            pressed_ = false; focused = nullptr;
            if (inside && action_) action_();
            return true;
        }
        return false;
    }

private:
    IconType              icon_;
    std::function<void()> action_;
    DrawFn                customDraw_;
    bool        pressed_      = false;
    std::string displayLabel_;   // if set, shown below icon instead of label_

    // Built-in icons use the Feather VG assets when they are compiled in and
    // fall back to the primitive drawings below otherwise.
    static const char* vgNameFor(IconType t) {
        switch (t) {
        case MUSIC:    return "ui_note";
        case SETTINGS: return "settings";
        case FILES:    return "ui_folder";
        case DEBUG:    return "terminal";
        case WIFI:     return "wifi";
        case POWER:    return "power";
        case HEART:    return "heart";
        default:       return nullptr;
        }
    }

    void drawBuiltinIcon(DrawCtx& ctx, int16_t cx, int16_t cy,
                         int16_t sz, uint16_t fg, uint16_t bg) const
    {
        if (const char* n = vgNameFor(icon_))
            if (ctx.icon(n, cx, cy, sz, fg, bg)) return;
        switch (icon_) {
        case MUSIC:    drawMusic   (ctx,cx,cy,sz,fg); break;
        case SETTINGS: drawSettings(ctx,cx,cy,sz,fg); break;
        case FILES:    drawFiles   (ctx,cx,cy,sz,fg); break;
        case DEBUG:    drawDebug   (ctx,cx,cy,sz,fg); break;
        case WIFI:     drawWifi    (ctx,cx,cy,sz,fg); break;
        case POWER:    drawPower   (ctx,cx,cy,sz,fg); break;
        case HEART:    drawHeart   (ctx,cx,cy,sz,fg); break;
        case VG:       drawVGIcon  (ctx,cx,cy,sz,fg,bg); break;
        default: break;
        }
    }

    // VG icon: label_ must be "&assetName" — drawn centred in the icon area
    void drawVGIcon(DrawCtx& ctx, int16_t cx, int16_t cy,
                    int16_t sz, uint16_t fg, uint16_t bg) const {
        if (label_.empty() || label_[0] != '&') return;
        const char* name = label_.c_str() + 1;  // skip '&'
        int16_t half = sz / 2;
        ctx.drawVGByName(name,
                         int16_t(cx - half), int16_t(cy - half),
                         sz, sz, fg, bg);
    }

    static void drawMusic(DrawCtx& ctx, int16_t cx, int16_t cy,
                          int16_t sz, uint16_t fg) {
        int16_t r = std::max<int16_t>(3, sz/5);
        int16_t hx = int16_t(cx - sz/4), hy = int16_t(cy + sz/4);
        ctx.fillRoundRect(ctx.d, int16_t(hx-r), int16_t(hy-r+1), int16_t(r*2), int16_t(r*2-1), r-1, fg);
        int16_t sx = int16_t(hx+r-1), st = int16_t(hy - sz*3/5);
        ctx.drawFastVLine(ctx.d, sx, st, int16_t(hy-st), fg);
        ctx.drawLine(ctx.d, sx, st, int16_t(sx+sz/4), int16_t(st+sz/6), fg);
        ctx.drawLine(ctx.d, int16_t(sx+sz/4), int16_t(st+sz/6), int16_t(sx+sz/6), int16_t(st+sz/3), fg);
    }
    static void drawSettings(DrawCtx& ctx, int16_t cx, int16_t cy,
                              int16_t sz, uint16_t fg) {
        int16_t ro = sz/2, ri = int16_t(ro*6/10);
        ctx.drawRoundRect(ctx.d, int16_t(cx-ro), int16_t(cy-ro), int16_t(ro*2), int16_t(ro*2), ro, fg);
        ctx.drawRoundRect(ctx.d, int16_t(cx-ri/2), int16_t(cy-ri/2), ri, ri, int16_t(ri/2), fg);
        for (int i = 0; i < 8; ++i) {
            float a = float(i) * 3.14159f / 4.f;
            int16_t x0 = int16_t(cx + float(ro)*sinf(a)), y0 = int16_t(cy - float(ro)*cosf(a));
            int16_t x1 = int16_t(cx + float(ro+sz/6)*sinf(a)), y1 = int16_t(cy - float(ro+sz/6)*cosf(a));
            ctx.drawLine(ctx.d, x0, y0, x1, y1, fg);
        }
    }
    static void drawFiles(DrawCtx& ctx, int16_t cx, int16_t cy,
                          int16_t sz, uint16_t fg) {
        int16_t bw=int16_t(sz*9/10), bh=int16_t(sz*6/10);
        int16_t bx=int16_t(cx-bw/2), by=int16_t(cy-bh/2+sz/10);
        ctx.drawRect(ctx.d, bx, by, bw, bh, fg);
        int16_t tw=int16_t(bw*4/10), th=int16_t(sz/7);
        ctx.drawRect(ctx.d, bx, int16_t(by-th), tw, int16_t(th+1), fg);
        for (int i=1; i<=2; ++i)
            ctx.drawFastHLine(ctx.d, int16_t(bx+sz/8), int16_t(by+bh*i/3), int16_t(bw-sz/4), fg);
    }
    static void drawDebug(DrawCtx& ctx, int16_t cx, int16_t cy,
                          int16_t sz, uint16_t fg) {
        int16_t bw=int16_t(sz*9/10), bh=int16_t(sz*7/10);
        int16_t bx=int16_t(cx-bw/2), by=int16_t(cy-bh/2);
        ctx.drawRect(ctx.d, bx, by, bw, bh, fg);
        ctx.drawFastHLine(ctx.d, bx, int16_t(by+sz/6), bw, fg);
        int16_t px=int16_t(bx+sz/8), py=int16_t(by+bh/2), ps=int16_t(sz/7);
        ctx.drawLine(ctx.d, px, int16_t(py-ps), int16_t(px+ps), py, fg);
        ctx.drawLine(ctx.d, int16_t(px+ps), py, px, int16_t(py+ps), fg);
        ctx.fillRect(ctx.d, int16_t(px+ps+sz/8), int16_t(py-ps/2), int16_t(sz/6), int16_t(ps+1), fg);
    }
    static void drawWifi(DrawCtx& ctx, int16_t cx, int16_t cy,
                         int16_t sz, uint16_t fg) {
        int16_t dot=std::max<int16_t>(2,sz/10), acy=int16_t(cy+sz/6);
        ctx.fillRoundRect(ctx.d, int16_t(cx-dot), int16_t(acy-dot), int16_t(dot*2), int16_t(dot*2), dot, fg);
        for (int ring=1; ring<=3; ++ring) {
            int16_t r=int16_t(sz*ring/(3*2));
            int16_t px0=0, py0=0;
            for (int s=0; s<=16; ++s) {
                float a = (-3.f*3.14159f/4.f) + float(s)/16.f*(3.f*3.14159f/2.f);
                int16_t px=int16_t(cx+float(r)*sinf(a)), py=int16_t(acy-float(r)*cosf(a));
                if (s>0) ctx.drawLine(ctx.d, px0, py0, px, py, fg);
                px0=px; py0=py;
            }
        }
    }
    static void drawPower(DrawCtx& ctx, int16_t cx, int16_t cy,
                          int16_t sz, uint16_t fg) {
        int16_t r=sz/2;
        float gap=3.14159f/6.f;
        int16_t px0=0, py0=0;
        for (int s=0; s<=24; ++s) {
            float a=(3.14159f/2.f+gap)+float(s)/24.f*(2.f*3.14159f-2.f*gap);
            int16_t px=int16_t(cx+float(r)*cosf(a)), py=int16_t(cy-float(r)*sinf(a));
            if (s>0) ctx.drawLine(ctx.d, px0, py0, px, py, fg);
            px0=px; py0=py;
        }
        ctx.drawFastVLine(ctx.d, cx, int16_t(cy-r*8/10), int16_t(r*8/10+r/4), fg);
    }
    static void drawHeart(DrawCtx& ctx, int16_t cx, int16_t cy,
                          int16_t sz, uint16_t fg) {
        int16_t r=std::max<int16_t>(3,sz/4), by=int16_t(cy-sz/8);
        ctx.drawRoundRect(ctx.d, int16_t(cx-r*2+1), int16_t(by-r), int16_t(r*2), int16_t(r*2), r, fg);
        ctx.drawRoundRect(ctx.d, int16_t(cx-1), int16_t(by-r), int16_t(r*2), int16_t(r*2), r, fg);
        ctx.drawLine(ctx.d, int16_t(cx-r*2+1), int16_t(by+r/2), cx, int16_t(cy+sz*4/10), fg);
        ctx.drawLine(ctx.d, int16_t(cx+r*2-1), int16_t(by+r/2), cx, int16_t(cy+sz*4/10), fg);
    }
};

// ============================================================================
//  PageDots — small row of filled/empty circles showing current page
// ============================================================================
class PageDots : public Element {
public:
    explicit PageDots(uint8_t current=0, uint8_t total=1)
        : Element("", 0, 0, 10), current_(current), total_(total) {}
    void set(uint8_t cur, uint8_t tot){ current_=cur; total_=tot; }

    Rect measure(DrawCtx&, Rect cursor, Rect area, uint16_t) override {
        if (style_ & STYLE_DISPLAY_FIXED) return Rect(x,y,w,h);
        placeAt(cursor, 0);
        w = resolveW(area);
        h = resolveH(10);
        return Rect(x,y,w,h);
    }
    void draw(DrawCtx& ctx, Colors colors) override {
        if (total_ <= 1) return;
        Colors c = resolveColors(colors);
        // current page = wide pill, others = small dots
        const int16_t d = 5, pill = 14, gap = 5;
        int16_t totalW = int16_t(pill + (total_ - 1) * (d + gap));
        int16_t sx = int16_t(x + (w - totalW) / 2);
        int16_t cy2 = int16_t(y + h/2);
        for (uint8_t i = 0; i < total_; ++i) {
            int16_t ww = (i == current_) ? pill : d;
            if (i == current_) ctx.fillRoundRect(ctx.d, sx, int16_t(cy2 - d/2), ww, d, d/2, c.fg);
            else               ctx.drawRoundRect(ctx.d, sx, int16_t(cy2 - d/2), ww, d, d/2, c.fg);
            sx = int16_t(sx + ww + gap);
        }
    }
private:
    uint8_t current_, total_;
};

// ============================================================================
//  AdaptiveParent — Parent subclass that calls a pre-measure hook.
//  Used as the icon menu root so rotation can be detected before each layout.
// ============================================================================
class AdaptiveParent : public Parent {
public:
    using Hook = std::function<void()>;
    explicit AdaptiveParent(Hook h)
        : Parent("", STYLE_DISPLAY_FLEX | STYLE_VERTICAL | STYLE_HIDE_LABEL)
        , hook_(h) {}
    Rect measure(DrawCtx& ctx, Rect cursor, Rect area, uint16_t pStyle) override {
        if (hook_) hook_();
        return Parent::measure(ctx, cursor, area, pStyle);
    }
private:
    Hook hook_;
};

// ============================================================================
//  IconMenu
// ============================================================================
class IconMenu {
public:
    using IconType = IconCell::IconType;
    using DrawFn   = IconCell::DrawFn;

    // Portrait defaults: 2 cols × 3 rows.
    // Landscape swaps: 3 cols × 2 rows.
    explicit IconMenu(uint8_t portraitCols=2, uint8_t portraitRows=3)
        : pCols_(portraitCols > 0 ? portraitCols : 2)
        , pRows_(portraitRows > 0 ? portraitRows : 3)
    {
        buildRoot();
    }

    // ---- configure grid size ------------------------------------------------
    // Call before or after adding icons — takes effect on next draw.
    void setPortraitGrid(uint8_t cols, uint8_t rows) {
        pCols_ = cols > 0 ? cols : 1;
        pRows_ = rows > 0 ? rows : 1;
    }

    // ---- title shown at the top of the page ---------------------------------
    void setTitle(const char* t) {
        if (!titleElem_)
            titleElem_ = root_->add(new TextDisplay(t)).get();
        titleElem_->setDynLabel(t);
        titleElem_->setHeight(26).textLeft().setTextSize(TEXT_TITLE).clearStyle(STYLE_DISABLED);
        titleElem_->setPadding(2);
    }

    // ---- connect to the UI engine for gesture-triggered redraws -------------
    // Pass pointers to the global ui and ctx objects.
    // IconMenu does NOT own these — caller must ensure they outlive the menu.
    void setUIContext(UI* ui, DrawCtx* ctx) { ui_ = ui; ctx_ = ctx; }

    // ---- EntryHandle — safe proxy returned by add() -----------------------
    // Lets you configure an entry after adding it without returning a raw
    // pointer that might be invalidated by a later rebuild.
    //
    //   auto h = menu->add(IconMenu::VG, "Grid", [&]{ ... });
    //   h.setVGLabel("&grid_icon");   // set label used for VG rendering
    //   h.setDisplayLabel("Grid");    // text shown below the icon
    //
    struct EntryHandle {
        IconMenu* menu;
        size_t    idx;

        // Override the icon label (used as VG asset name when icon==VG)
        // This is what you set when you want "&assetname" for the VG draw.
        EntryHandle& setVGLabel(const char* vgLabel) {
            if (menu && idx < menu->entries_.size()) {
                menu->entries_[idx].vgLabel = vgLabel ? vgLabel : "";
                menu->lastEntryCount_ = SIZE_MAX;  // force rebuild
            }
            return *this;
        }
        // Override the display label shown below the icon
        EntryHandle& setDisplayLabel(const char* lbl) {
            if (menu && idx < menu->entries_.size()) {
                menu->entries_[idx].label = lbl ? lbl : "";
                menu->lastEntryCount_ = SIZE_MAX;
            }
            return *this;
        }
        // Convenience: for VG icons, set both the VG asset name and display label.
        //   h.setVG("&home", "Home");
        EntryHandle& setVG(const char* vgAsset, const char* displayLabel) {
            setVGLabel(vgAsset);
            setDisplayLabel(displayLabel);
            return *this;
        }

        bool valid() const { return menu && idx < menu->entries_.size(); }
    };

    // ---- add icons ----------------------------------------------------------
    // Returns an EntryHandle — NEVER a null pointer.
    // Chain methods on the handle to configure the entry.
    EntryHandle add(IconType icon, const char* label, std::function<void()> action) {
        size_t idx = entries_.size();
        entries_.push_back({nullptr, icon, nullptr, label, "", action});
        lastEntryCount_ = SIZE_MAX;  // force rebuild on next measure
        return {this, idx};
    }
    EntryHandle add(DrawFn fn, const char* label, std::function<void()> action) {
        size_t idx = entries_.size();
        entries_.push_back({nullptr, IconType::CUSTOM, fn, label, "", action});
        lastEntryCount_ = SIZE_MAX;
        return {this, idx};
    }

    std::shared_ptr<Parent> root() { return root_; }

private:
    // ---- stored icon descriptors -------------------------------------------
    struct Entry {
        IconCell*             cell;       // null until built
        IconType              icon;
        DrawFn                customFn;
        std::string           label;      // display label shown below icon
        std::string           vgLabel;    // VG asset name e.g. "&home" (used when icon==VG)
        std::function<void()> action;
    };
    std::vector<Entry> entries_;

    uint8_t  pCols_ = 2, pRows_ = 3;   // portrait grid size
    uint8_t  page_  = 0;                // current page index

    std::shared_ptr<Parent> root_;
    Grid*        grid_      = nullptr;
    PageDots*    dots_      = nullptr;
    TextDisplay* titleElem_ = nullptr;

    UI*       ui_  = nullptr;
    DrawCtx*  ctx_ = nullptr;

    // Dirty tracking for adaptToRotation
    size_t  lastEntryCount_ = SIZE_MAX;  // SIZE_MAX forces first rebuild
    bool    builtOnce_      = false;

    // ---- build the root container (once) ------------------------------------
    void buildRoot() {
        auto* ap = new AdaptiveParent([this]{ adaptToRotation(); });
        root_ = std::shared_ptr<Parent>(ap);
        root_->setPadding(8).setSpacing(6);

        // Title row — hidden until setTitle() is called (height=0, disabled)
        titleElem_ = root_->add(new TextDisplay("")).get();
        titleElem_->setHeight(0).addStyle(STYLE_DISABLED);

        // Grid — columns/rows filled in during measure via rebuildGrid()
        auto* g = root_->add(new Grid(pCols_, pRows_)).get();
        grid_ = static_cast<Grid*>(g);
        grid_->setSpacing(8).setPadding(0).flexGrow();

        // Page dots at the bottom
        auto* pd = root_->add(new PageDots(0, 1)).get();
        dots_ = static_cast<PageDots*>(pd);
        dots_->setHeight(10);

        // Gesture handler on the grid itself
        grid_->setOnGesture([](void* ctx2, Element*, uint8_t gid){
            static_cast<IconMenu*>(ctx2)->handleGesture(gid);
        }, this);
        // NOTE: rebuildGrid is NOT called here — entries haven't been added yet.
        // It will be called on every measure() via adaptToRotation().
    }

    // ---- called on every measure to adapt to current rotation ---------------
    void adaptToRotation() {
        // Determine current grid dimensions based on orientation.
        // ctx_ may be null before setUIContext() — fall back to portrait.
        bool landscape = ctx_ && (ctx_->W() >= ctx_->H());
        uint8_t cols = landscape ? pRows_ : pCols_;
        uint8_t rows = landscape ? pCols_ : pRows_;

        // Always sync grid dimensions and rebuild if anything changed:
        // dimensions, orientation, or entry count since last build.
        bool dimChanged    = (cols != grid_->cols() || rows != grid_->rows());
        bool countChanged  = (entries_.size() != lastEntryCount_);
        if (dimChanged || countChanged || !builtOnce_) {
            grid_->setGrid(cols, rows);
            rebuildGrid(cols, rows);
            lastEntryCount_ = entries_.size();
            builtOnce_      = true;
        }
    }

    // ---- rebuild grid children for current page + grid size -----------------
    void rebuildGrid(uint8_t cols, uint8_t rows) {
        grid_->clearChildren();

        uint8_t perPage = uint8_t(cols) * uint8_t(rows);
        if (perPage == 0) perPage = 1;
        uint8_t totalPages = uint8_t((entries_.size() + perPage - 1) / perPage);
        if (totalPages == 0) totalPages = 1;
        if (page_ >= totalPages) page_ = uint8_t(totalPages - 1);

        size_t start = size_t(page_) * perPage;
        size_t end   = std::min(entries_.size(), start + perPage);

        for (size_t i = start; i < end; ++i) {
            auto& e = entries_[i];
            IconCell* cell;
            if (e.customFn) {
                cell = grid_->add(new IconCell(e.customFn, e.label.c_str(), e.action)).get();
            } else if (e.icon == IconType::VG) {
                // For VG icons: the cell's label_ is used as the VG asset name.
                // vgLabel holds "&assetname"; label holds the display text below.
                // We pass vgLabel as the element label (drives drawVGIcon),
                // and store display label separately in displayLabel_.
                const char* vgl = e.vgLabel.empty() ? e.label.c_str() : e.vgLabel.c_str();
                cell = grid_->add(new IconCell(IconType::VG, vgl, e.action)).get();
                // Set the display label (text below icon) — overrides the VG label
                if (!e.vgLabel.empty() && !e.label.empty())
                    cell->setDisplayLabel(e.label.c_str());
            } else {
                cell = grid_->add(new IconCell(e.icon, e.label.c_str(), e.action)).get();
            }
            e.cell = cell;
        }

        if (dots_) dots_->set(page_, totalPages);
    }

    // ---- gesture handler ----------------------------------------------------
    void handleGesture(uint8_t gid) {
        bool landscape = ctx_ && (ctx_->W() >= ctx_->H());
        uint8_t cols = landscape ? pRows_ : pCols_;
        uint8_t rows = landscape ? pCols_ : pRows_;

        uint8_t perPage = cols * rows;
        if (perPage == 0) perPage = 1;
        uint8_t totalPages = uint8_t((entries_.size() + perPage - 1) / perPage);

        bool changed = false;

        switch (gid) {
        case GESTURE_MOVE_UP:
        case GESTURE_MOVE_RIGHT:
            // Previous page
            if (page_ > 0) { --page_; changed = true; }
            break;
        case GESTURE_MOVE_DOWN:
        case GESTURE_MOVE_LEFT:
            // Next page
            if (page_ + 1 < totalPages) { ++page_; changed = true; }
            break;
        case GESTURE_ZOOM_IN:
            // Fewer icons per page (larger cells): shrink grid
            if (pCols_ > 1) --pCols_;
            if (pRows_ > 1) --pRows_;
            page_ = 0;
            changed = true;
            break;
        case GESTURE_ZOOM_OUT:
            // More icons per page (smaller cells): grow grid
            ++pCols_; ++pRows_;
            page_ = 0;
            changed = true;
            break;
        default: break;
        }

        if (changed) {
            bool ls = ctx_ && (ctx_->W() >= ctx_->H());
            uint8_t nc = ls ? pRows_ : pCols_;
            uint8_t nr = ls ? pCols_ : pRows_;
            grid_->setGrid(nc, nr);
            rebuildGrid(nc, nr);
            if (ui_ && ctx_) ui_->draw(*ctx_);
        }
    }
};

} // namespace einkui