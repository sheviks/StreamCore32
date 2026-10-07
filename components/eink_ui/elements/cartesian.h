#pragma once
// ============================================================================
//  einkui/elements/cartesian.h
//
//  CartesianPlot — XY grid with draggable control points.
//
//  addPoint(worldX, worldY, label) takes values in world-space (same units
//  as minX/maxX/minY/maxY).  The plot normalises internally.
//
//  onChange callback receives "worldX,worldY" as a string.
//
//  Example (5-band EQ, -12..+12 dB):
//    auto* plot = page->add(new CartesianPlot("EQ", 20.f,20000.f, -12.f,12.f, 0, 80));
//    plot->addPoint(60.f,   0.f,  "60");
//    plot->addPoint(250.f,  1.5f, "250");
//    plot->addPoint(1000.f,-2.0f, "1k");
// ============================================================================
#include "../include/element.h"
#include "FT6X36.h"
#include <vector>
#include <algorithm>
#include <cstdio>

namespace einkui {

// ============================================================================
//  CartesianPoint
// ============================================================================
class CartesianPoint : public Element {
public:
    // wx/wy are world-space values
    CartesianPoint(const char* label, float wx, float wy)
        : Element(label, 0, 12, 12), wx_(wx), wy_(wy) {}

    float wx() const { return wx_; }
    float wy() const { return wy_; }
    void  setWx(float v){ wx_=v; }
    void  setWy(float v){ wy_=v; }

    void drawAt(DrawCtx& ctx, int16_t cx, int16_t cy, Colors c, bool active,
                int16_t plotTop = -32768) const {
        // white disc with a black ring; filled when being dragged
        ctx.fillCircle(cx, cy, 5, c.fg);
        if (!active) ctx.fillCircle(cx, cy, 3, c.bg);
        if (!label_.empty()) {
            int16_t tw = ctx.textWidth(label_.c_str(), TEXT_CAPTION);
            int16_t cap = ctx.metrics(TEXT_CAPTION).cap;
            // label above the point, or below when it would leave the plot
            int16_t ty = int16_t(cy - 8 - cap);
            if (ty < plotTop) ty = int16_t(cy + 8);
            ctx.fillRect(ctx.d, int16_t(cx - tw/2 - 1), int16_t(ty - 1), int16_t(tw + 2), int16_t(cap + 2), c.bg);
            ctx.text(int16_t(cx - tw/2), int16_t(ty + cap), label_.c_str(), TEXT_CAPTION, c.fg);
        }
    }

    Rect measure(DrawCtx&, Rect, Rect, uint16_t) override { return Rect(x,y,w,h); }
    void draw(DrawCtx&, Colors) override {}
    bool onTouch(DrawCtx&, const TTouchFrame&, std::shared_ptr<Element>&) override { return false; }

private:
    float wx_, wy_;  // world-space
};

// ============================================================================
//  CartesianPlot
// ============================================================================
class CartesianPlot : public Element {
public:
    CartesianPlot(const char* label="",
                  float minX=0.f, float maxX=1.f,
                  float minY=-1.f, float maxY=1.f,
                  uint16_t fixedW=0, uint16_t fixedH=60)
        : Element(label, STYLE_BORDER, fixedW, fixedH),
          minX_(minX), maxX_(maxX), minY_(minY), maxY_(maxY) {}

    // addPoint: world-space coordinates
    CartesianPoint* addPoint(float worldX, float worldY, const char* label="") {
        auto* p = new CartesianPoint(label, worldX, worldY);
        points_.push_back(std::shared_ptr<CartesianPoint>(p));
        return p;
    }

    bool redrawOnMove() const override { return true; }
    void cancelTouch() override { dragging_ = nullptr; }

    void setMinLabel(const char* l){ minLabel_=l; }
    void setMaxLabel(const char* l){ maxLabel_=l; }

    // Show or hide horizontal grid lines (zero line always shown)
    void setGridLines(uint8_t count){ gridLines_=count; }

    // ---- measure ------------------------------------------------------------
    Rect measure(DrawCtx& ctx, Rect cursor, Rect area, uint16_t pStyle) override {
        if (style_ & STYLE_DISPLAY_FIXED) return Rect(x,y,w,h);
        placeAt(cursor, pStyle);
        bool title = !label_.empty() && !(style_ & STYLE_HIDE_LABEL);
        headerH_ = title ? int16_t(ctx.metrics(TEXT_BOLD).cap + 8) : 0;
        footerH_ = (minLabel_ || maxLabel_) ? int16_t(ctx.metrics(TEXT_CAPTION).cap + 6) : 0;
        w = resolveW(area);
        h = resolveH(int16_t(height_ > 0 ? height_ : 60));
        if (w < 1) w = 1;
        if (h < 1) h = 1;
        return Rect(x,y,w,h);
    }

    // ---- draw ---------------------------------------------------------------
    void draw(DrawCtx& ctx, Colors colors) override {
        if (style_ & STYLE_DISABLED) return;
        Colors c = resolveColors(colors);
        Rect body = (style_ & STYLE_BORDER)
                  ? ctx.surface(x, y, w, h, ctx.th().radius, c.fg, c.bg, false)
                  : Rect(x, y, w, h);
        if (!(style_ & STYLE_BORDER)) ctx.fillRect(ctx.d, x, y, w, h, c.bg);
        body_ = body;

        if (headerH_) {
            ctx.textBox(int16_t(body.x + 8), int16_t(body.y + 2), int16_t(body.w - 16), int16_t(headerH_ - 2),
                        label_.c_str(), TEXT_BOLD, c.fg);
            char rng[24]; snprintf(rng, sizeof(rng), "%+.0f / %+.0f", (double)minY_, (double)maxY_);
            ctx.textBox(int16_t(body.x + 8), int16_t(body.y + 2), int16_t(body.w - 16), int16_t(headerH_ - 2),
                        rng, TEXT_CAPTION, c.fg, TEXT_HALIGN_RIGHT);
        }

        Rect pa = plotArea();
        if (pa.w <= 0 || pa.h <= 0) return;

        // Grid: dotted horizontal lines + dotted frame bottom
        if (gridLines_ > 0) {
            for (uint8_t i = 1; i < gridLines_; ++i) {
                float gy = minY_ + (maxY_ - minY_) * (float)i / (float)gridLines_;
                int16_t pgy = wyToY(pa, gy);
                if (pgy >= pa.y && pgy < pa.y2())
                    for (int16_t px = pa.x; px < pa.x2(); px += 4) ctx.drawPixel(ctx.d, px, pgy, c.fg);
            }
        }
        ctx.hairline(pa.x, pa.y, pa.w, c.fg);
        ctx.hairline(pa.x, int16_t(pa.y2() - 1), pa.w, c.fg);
        // Zero line (solid)
        if (minY_ < 0.f && maxY_ > 0.f) {
            int16_t zeroY = wyToY(pa, 0.f);
            if (zeroY >= pa.y && zeroY < pa.y2())
                ctx.drawFastHLine(ctx.d, pa.x, zeroY, pa.w, c.fg);
        }

        // Axis labels
        int16_t capC = ctx.metrics(TEXT_CAPTION).cap;
        int16_t base = int16_t(pa.y2() + 3 + capC);
        if (minLabel_) ctx.text(pa.x, base, minLabel_, TEXT_CAPTION, c.fg);
        if (maxLabel_) ctx.text(int16_t(pa.x2() - ctx.textWidth(maxLabel_, TEXT_CAPTION)), base,
                                maxLabel_, TEXT_CAPTION, c.fg);

        // Curve through sorted points (2px)
        std::vector<CartesianPoint*> sorted;
        for (auto& p : points_) sorted.push_back(p.get());
        std::sort(sorted.begin(), sorted.end(),
                  [](CartesianPoint* a, CartesianPoint* b){ return a->wx()<b->wx(); });
        for (size_t i=1; i<sorted.size(); ++i) {
            int16_t x0=wxToX(pa, sorted[i-1]->wx()), y0=wyToY(pa, sorted[i-1]->wy());
            int16_t x1=wxToX(pa, sorted[i  ]->wx()), y1=wyToY(pa, sorted[i  ]->wy());
            ctx.drawLine(ctx.d, x0,y0, x1,y1, c.fg);
            ctx.drawLine(ctx.d, x0,int16_t(y0+1), x1,int16_t(y1+1), c.fg);
        }

        for (auto& p : points_) {
            int16_t cx=wxToX(pa,p->wx()), cy=wyToY(pa,p->wy());
            p->x = int16_t(cx-5); p->y = int16_t(cy-5); p->w = 11; p->h = 11;
            p->drawAt(ctx, cx, cy, c, dragging_.get()==p.get(), pa.y);
        }
    }

    // ---- touch --------------------------------------------------------------
    bool onTouch(DrawCtx& /*ctx*/, const TTouchFrame& tf,
                 std::shared_ptr<Element>& focused) override
    {
        uint8_t  ev=(uint8_t)tf.p[0].event;
        uint16_t tx=tf.p[0].x, ty=tf.p[0].y;

        if (ev==0 && tf.touches>0) {
            for (auto& p : points_) {
                Rect hit(int16_t(p->x-6), int16_t(p->y-6), 23, 23);
                if (hit.contains(tx,ty)) {
                    dragging_ = p;
                    focused   = std::shared_ptr<Element>(this,[](Element*){});
                    return true;
                }
            }
            return false;
        }
        if ((ev==2 || ev==1) && dragging_ && focused.get()==this) {
            Rect pa = plotArea();
            float wx = xToWx(pa, int16_t(tx));
            float wy = yToWy(pa, int16_t(ty));
            // Clamp to world-space bounds
            wx = wx < minX_ ? minX_ : (wx > maxX_ ? maxX_ : wx);
            wy = wy < minY_ ? minY_ : (wy > maxY_ ? maxY_ : wy);
            dragging_->setWx(wx); dragging_->setWy(wy);
            if (dragging_->hasCb() && dragging_->cb().onChange) {
                char buf[48]; snprintf(buf, sizeof(buf), "%f,%f", wx, wy);
                dragging_->cb().onChange(dragging_->cb().ctx, dragging_.get(), buf);
            }
            if (ev==1) { dragging_=nullptr; focused=nullptr; }
            return true;
        }
        return false;
    }

private:
    float minX_, maxX_, minY_, maxY_;
    int16_t headerH_  = 0;
    int16_t footerH_  = 0;
    Rect    body_;
    uint8_t gridLines_ = 0;
    const char* minLabel_ = nullptr;
    const char* maxLabel_ = nullptr;
    std::vector<std::shared_ptr<CartesianPoint>> points_;
    std::shared_ptr<CartesianPoint> dragging_;

    Rect plotArea() const {
        Rect b = body_.empty() ? Rect(x, y, w, h) : body_;
        const int16_t px = 10;   // room for point discs at the edges
        return Rect(int16_t(b.x + px),
                    int16_t(b.y + headerH_ + 6),
                    int16_t(b.w - px*2),
                    int16_t(b.h - headerH_ - footerH_ - 10));
    }

    // World → pixel
    int16_t wxToX(Rect pa, float wx) const {
        float nx = (maxX_ > minX_) ? (wx - minX_) / (maxX_ - minX_) : 0.f;
        return int16_t(pa.x + nx * (pa.w - 1));
    }
    int16_t wyToY(Rect pa, float wy) const {
        float ny = (maxY_ > minY_) ? (wy - minY_) / (maxY_ - minY_) : 0.f;
        // ny=0 → bottom of plot, ny=1 → top
        return int16_t(pa.y2() - 1 - ny * (pa.h - 1));
    }
    // Pixel → world
    float xToWx(Rect pa, int16_t px) const {
        float nx = pa.w > 1 ? float(px - pa.x) / float(pa.w - 1) : 0.f;
        return minX_ + nx * (maxX_ - minX_);
    }
    float yToWy(Rect pa, int16_t py) const {
        float ny = pa.h > 1 ? 1.f - float(py - pa.y) / float(pa.h - 1) : 0.f;
        return minY_ + ny * (maxY_ - minY_);
    }
};

} // namespace einkui