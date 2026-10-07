#pragma once
// ============================================================================
//  einkui/elements/grid.h
//
//  Grid — CSS-grid-inspired layout container.
//
//  ---- Fixed vs auto dimensions -----------------------------------------------
//
//  Grid(cols)          — auto rows: grows downward as children are added.
//  Grid(cols, rows)    — fixed grid: exactly cols×rows cells are available.
//
//  When rows_ > 0 (fixed mode):
//   • The grid always reserves exactly rows_ rows of equal height.
//   • Children placed outside [1..cols_] × [1..rows_] are silently skipped
//     (not drawn, not measured).  This lets you leave cells empty.
//   • Auto-placed children that overflow the grid are similarly skipped.
//   • This is how you add an empty row: just don't put anything in it.
//
//  When rows_ == 0 (auto mode):
//   • Rows grow to fit all children, same behaviour as before.
//
//  ---- Height resolution -------------------------------------------------------
//    fixedH  > layoutH_  > area.h  > 32px fallback
//  Inner is always derived from our own x,y,w,h — never from area.
//
//  ---- place() argument order -------------------------------------------------
//    place(element,  col, row,  colSpan, rowSpan)
//                    ^^^  ^^^   ^^^^^^^  ^^^^^^^
//                   1-based     number of cells wide/tall
//
//  ---- Usage ------------------------------------------------------------------
//
//    // Auto rows, 3 columns — rows grow as needed:
//    auto* g = page->add(new Grid(3));
//
//    // Fixed 3×2 grid — empty cells are silent gaps:
//    auto* g = page->add(new Grid(3, 2));
//    g->add(new Button("A")); g->placeLast(1,1);   // row 1 col 1
//    g->add(new Button("B")); g->placeLast(3,1);   // row 1 col 3 — col 2 is empty
//    g->add(new Button("C")); g->placeLast(1,2);   // row 2 col 1
//    // row 2 col 2-3 are empty
//
//    // Change grid dimensions at runtime (e.g. on rotation):
//    g->setGrid(cols, rows);
//    // Then call ui.draw(ctx) to re-layout.
// ============================================================================

#include "../include/element.h"

namespace einkui {

// ============================================================================
//  GridSpan
// ============================================================================
struct GridSpan {
    static constexpr uint8_t AUTO = 0;
    uint8_t col     = AUTO;   // 1-based, 0 = auto
    uint8_t row     = AUTO;   // 1-based, 0 = auto
    uint8_t colSpan = 1;
    uint8_t rowSpan = 1;
    bool isExplicit() const { return col != AUTO && row != AUTO; }
};

// ============================================================================
//  Grid
// ============================================================================
class Grid : public Parent {
public:
    // cols: number of columns.
    // rows: 0 = auto-grow (default), >0 = fixed number of rows.
    explicit Grid(uint8_t cols = 2, uint8_t rows = 0)
        : Parent("", STYLE_DISPLAY_FLEX | STYLE_HIDE_LABEL)
        , cols_(cols > 0 ? cols : 1)
        , rows_(rows)
    {}

    // ---- configuration -------------------------------------------------------

    // Change grid dimensions (e.g. on display rotation).
    // Does not affect stored span entries — call ui.draw() after to re-layout.
    Grid& setGrid    (uint8_t cols, uint8_t rows = 0) {
        cols_ = cols > 0 ? cols : 1;
        rows_ = rows;
        return *this;
    }
    Grid& setCols    (uint8_t c) { cols_ = c > 0 ? c : 1; return *this; }
    Grid& setRows    (uint8_t r) { rows_ = r;              return *this; }
    Grid& setRowHeight(uint16_t h){ rowH_ = h;             return *this; }

    uint8_t cols() const { return cols_; }
    uint8_t rows() const { return rows_; }

    // ---- explicit placement --------------------------------------------------
    Grid& place(Element* e,
                uint8_t col, uint8_t row,
                uint8_t colSpan = 1, uint8_t rowSpan = 1)
    {
        GridSpan gs;
        gs.col     = col;
        gs.row     = row;
        gs.colSpan = colSpan > 0 ? colSpan : 1;
        gs.rowSpan = rowSpan > 0 ? rowSpan : 1;
        upsertSpan(e, gs);
        return *this;
    }

    Grid& placeLast(uint8_t col, uint8_t row,
                    uint8_t colSpan = 1, uint8_t rowSpan = 1)
    {
        if (!children_.empty())
            place(children_.back().get(), col, row, colSpan, rowSpan);
        return *this;
    }

    // ---- measure override ---------------------------------------------------
    Rect measure(DrawCtx& ctx, Rect cursor, Rect area, uint16_t /*pStyle*/) override
    {
        if (!(style_ & STYLE_DISPLAY_FIXED)) {
            placeAt(cursor, 0);
            w = resolveW(area);

            if      (height_  > 0) h = int16_t(height_);
            else if (layoutH_ > 0) h = int16_t(layoutH_);
            else if (area.h   > 0) h = int16_t(area.h);
            else                   h = 32;
        }

        // Inner rect from OUR OWN bounds — never from area
        Rect inner;
        inner.x = x + int16_t(padding_);
        inner.y = y + int16_t(padding_);
        inner.w = std::max<int16_t>(0, w - int16_t(padding_) * 2);
        inner.h = std::max<int16_t>(0, h - int16_t(padding_) * 2);

        if (!children_.empty())
            gridLayout(ctx, inner);

        return Rect(x, y, w, h);
    }

private:
    uint8_t  cols_ = 2;
    uint8_t  rows_ = 0;    // 0 = auto-grow
    uint16_t rowH_ = 0;

    // =========================================================================
    //  Span registry
    // =========================================================================
    struct SpanEntry { Element* el; GridSpan span; };
    std::vector<SpanEntry> spans_;

    void upsertSpan(Element* e, GridSpan gs) {
        for (auto& se : spans_) {
            if (se.el == e) { se.span = gs; return; }
        }
        spans_.push_back({e, gs});
    }
    GridSpan spanOf(const Element* e) const {
        for (auto& se : spans_)
            if (se.el == e) return se.span;
        return GridSpan{};
    }

    // =========================================================================
    //  Occupancy table
    // =========================================================================
    struct OccGrid {
        uint8_t           cols;
        std::vector<bool> cells;
        explicit OccGrid(uint8_t c) : cols(c) {}

        void ensureRows(uint8_t upTo) {
            size_t need = size_t(upTo) * cols;
            if (cells.size() < need) cells.resize(need, false);
        }
        bool isFree(uint8_t c0, uint8_t r0, uint8_t cs, uint8_t rs) {
            ensureRows(r0 + rs);
            for (uint8_t r = r0; r < r0+rs; ++r)
                for (uint8_t c = c0; c < c0+cs; ++c) {
                    if (c >= cols) return false;
                    if (cells[size_t(r)*cols+c]) return false;
                }
            return true;
        }
        void occupy(uint8_t c0, uint8_t r0, uint8_t cs, uint8_t rs) {
            ensureRows(r0 + rs);
            for (uint8_t r = r0; r < r0+rs; ++r)
                for (uint8_t c = c0; c < c0+cs; ++c)
                    if (c < cols) cells[size_t(r)*cols+c] = true;
        }
        bool findNext(uint8_t& cc, uint8_t& cr,
                      uint8_t cs, uint8_t rs,
                      uint8_t maxRows,           // 0 = unlimited
                      uint8_t& outC, uint8_t& outR)
        {
            if (cs > cols) cs = cols;
            for (;;) {
                if (cc + cs > cols) { cc = 0; ++cr; }
                // If fixed rows: stop when we'd exceed them
                if (maxRows > 0 && cr + rs > maxRows) return false;
                if (isFree(cc, cr, cs, rs)) { outC = cc; outR = cr; return true; }
                ++cc;
            }
        }
    };

    // =========================================================================
    //  Layout
    // =========================================================================
    void gridLayout(DrawCtx& ctx, Rect inner) {
        const int16_t sp = int16_t(spacing_);

        // Cell width
        int16_t cellW = std::max<int16_t>(1,
            (inner.w - sp * int16_t(cols_ - 1)) / int16_t(cols_));

        // Phase 1: mark explicit placements
        OccGrid occ(cols_);
        for (auto& ch : children_) {
            GridSpan gs = spanOf(ch.get());
            if (!gs.isExplicit()) continue;
            // In fixed mode: skip placements outside the declared grid
            if (rows_ > 0 && uint8_t(gs.row - 1 + gs.rowSpan) > rows_) continue;
            if (uint8_t(gs.col - 1 + gs.colSpan) > cols_) continue;
            occ.occupy(uint8_t(gs.col-1), uint8_t(gs.row-1),
                       gs.colSpan, gs.rowSpan);
        }

        // Phase 2: assign placements
        struct P { Element* el; uint8_t col, row, cs, rs; };
        std::vector<P> pl;
        pl.reserve(children_.size());
        uint8_t cc = 0, cr = 0;

        for (auto& ch : children_) {
            GridSpan gs = spanOf(ch.get());
            if (gs.isExplicit()) {
                // Fixed mode: skip out-of-bounds
                if (rows_ > 0 && uint8_t(gs.row - 1 + gs.rowSpan) > rows_) continue;
                if (uint8_t(gs.col - 1 + gs.colSpan) > cols_) continue;
                pl.push_back({ch.get(),
                              uint8_t(gs.col-1), uint8_t(gs.row-1),
                              gs.colSpan, gs.rowSpan});
            } else {
                uint8_t oc = 0, or_ = 0;
                if (!occ.findNext(cc, cr, gs.colSpan, gs.rowSpan,
                                  rows_, oc, or_))
                    continue;   // no room (fixed grid full)
                occ.occupy(oc, or_, gs.colSpan, gs.rowSpan);
                pl.push_back({ch.get(), oc, or_, gs.colSpan, gs.rowSpan});
                cc = uint8_t(oc + gs.colSpan);
                cr = or_;
            }
        }

        // Effective row count
        uint8_t rowCount = rows_;   // fixed: use declared rows
        if (rowCount == 0) {
            for (auto& p : pl)
                rowCount = std::max<uint8_t>(rowCount, uint8_t(p.row + p.rs));
            if (rowCount == 0) rowCount = 1;
        }

        // Cell height
        int16_t cellH;
        if (rowH_ > 0) {
            cellH = int16_t(rowH_);
        } else if (inner.h > 0) {
            cellH = std::max<int16_t>(1,
                (inner.h - sp * int16_t(rowCount-1)) / int16_t(rowCount));
        } else {
            cellH = 32;
        }

        // Position and measure
        for (auto& p : pl) {
            int16_t slotX = int16_t(inner.x + int16_t(p.col) * (cellW + sp));
            int16_t slotY = int16_t(inner.y + int16_t(p.row) * (cellH + sp));
            int16_t slotW = int16_t(int16_t(p.cs) * cellW + int16_t(p.cs-1) * sp);
            int16_t slotH = int16_t(int16_t(p.rs) * cellH + int16_t(p.rs-1) * sp);
            int16_t m     = int16_t(p.el->margin());

            p.el->setLayoutW(uint16_t(std::max<int16_t>(1, slotW - m*2)));
            p.el->setLayoutH(uint16_t(std::max<int16_t>(1, slotH - m*2)));

            Rect slotCursor(slotX, slotY, 0, 0);
            p.el->measure(ctx, slotCursor, inner, style_);
        }
    }
};

} // namespace einkui