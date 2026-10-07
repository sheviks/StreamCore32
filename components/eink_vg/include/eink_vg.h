// eink_vg.h
#pragma once
#include <stdint.h>
#include <math.h>

// ============================================================================
//  VG_Q1 bytecode format
//
//  All coordinates are int16 (little-endian).  The bytecode is designed to be
//  compact: typical icon SVGs convert to 100-400 bytes.
//
//  Stream layout:
//    OP_VIEWBOX  vbW(u16) vbH(u16)
//    { OP_STYLE flags strokeW colorIdx }*
//    { OP_LINE | OP_RECT | OP_CIRCLE | OP_POLY | OP_SPLINE }*
//    OP_END
//
//  OP_SPLINE encodes a chain of cubic Bézier segments sharing endpoints:
//    n(u8)  pflags(u8)  P0.x P0.y  [C1.x C1.y  C2.x C2.y  P1.x P1.y]*n
//  where n is the number of cubic segments.  The curve starts at P0 and each
//  segment adds 3 more points (two control + endpoint).  Total points in
//  bytecode = 1 + 3*n, coordinates = 2*(1+3n) int16 values.
//  pflags bit 0 (SPLINE_CLOSED): close the curve back to P0 after the last seg.
// ============================================================================

enum : uint8_t {
  OP_END     = 0,
  OP_VIEWBOX = 1,   // vbW(u16) vbH(u16)
  OP_STYLE   = 2,   // flags strokeW colorIndex
  OP_LINE    = 3,   // x0 y0 x1 y1 (s16 each)
  OP_RECT    = 4,   // x y w h rx   (s16 each)
  OP_CIRCLE  = 5,   // cx cy r      (s16 each)
  OP_POLY    = 6,   // n(u8) pflags(u8) then n*(x s16, y s16)
  OP_SPLINE  = 7,   // n(u8) pflags(u8) then (1+3n)*(x s16, y s16)
};

// OP_STYLE flags
static constexpr uint8_t STYLE_FILL   = 0x01;
static constexpr uint8_t STYLE_STROKE = 0x02;

// OP_POLY / OP_SPLINE flags
static constexpr uint8_t POLY_CLOSED   = 0x01;
static constexpr uint8_t SPLINE_CLOSED = 0x01;  // same bit, different opcode

// ---------------------------------------------------------------------------
//  Wire-reading helpers
// ---------------------------------------------------------------------------
static inline uint8_t rd_u8(const uint8_t*& p) {
  return *p++;
}
static inline uint16_t rd_u16(const uint8_t*& p) {
  uint16_t v = (uint16_t)(p[0] | (p[1] << 8));
  p += 2;
  return v;
}
static inline int16_t rd_s16(const uint8_t*& p) {
  int16_t v = (int16_t)(p[0] | (p[1] << 8));
  p += 2;
  return v;
}

// ---------------------------------------------------------------------------
//  Drawing helpers
// ---------------------------------------------------------------------------
static inline int16_t iroundf(float v) {
  return (int16_t)(v >= 0.0f ? (v + 0.5f) : (v - 0.5f));
}
static inline int16_t map_i1(int16_t v, float scale, int16_t off) {
  return iroundf(off + (float)v * scale);
}

struct VGPoint { int16_t x, y; };

// ---------------------------------------------------------------------------
//  Thick circle / filled disc via scanline.
//  Computes exact x-spans per row for outer and inner radii — no Bresenham
//  gaps, never writes outside the ring, background fully preserved.
// ---------------------------------------------------------------------------
template <class DisplayT>
static inline void vg_drawThickCircle(DisplayT& d,
                                      int16_t cx, int16_t cy,
                                      int16_t r,  int16_t sw,
                                      uint16_t col, bool filled) {
  // For filled disc: draw full disc to radius r.
  // Special case r==0 (dot/cap): use sw/2 as disc radius.
  // For stroke ring: Rout = r + upper_half, Rin = r - lower_half
  int16_t half = (sw > 0) ? (int16_t)(sw / 2) : (int16_t)0;
  int16_t Rout = filled ? (r > 0 ? r : half) : (int16_t)(r + (sw - 1 - half));
  int16_t Rin  = filled ? (int16_t)0 : (int16_t)(r - half);
  if (Rout <= 0) return;

  for (int16_t dy = -Rout; dy <= Rout; ++dy) {
    float dy2f = (float)dy * (float)dy;
    float Rout2f = (float)Rout*(float)Rout - dy2f;
    if (Rout2f < 0.0f) continue;

    // xout: rightmost pixel of outer disc on this row
    int16_t xout = (int16_t)sqrtf(Rout2f);  // floor keeps outer edge inside

    // xin: rightmost pixel of inner disc (hole), 0 when filled or hole doesn't reach
    int16_t xin = -1;  // -1 means no hole on this row
    if (Rin > 0) {
      float Rin2f = (float)Rin*(float)Rin - dy2f;
      if (Rin2f > 0.0f) {
        float xin_f = sqrtf(Rin2f);
        xin = (int16_t)xin_f;
        // If sqrt is exact integer, shrink hole by 1 so boundary pixel stays in ring
        if (xin > 0 && (float)xin*(float)xin == Rin2f) xin--;
      }
      // Rin2f==0 means the inner circle just touches this row at one pixel (cx);
      // that pixel is on the ring boundary so treat as full-row (xin stays -1)
    }

    int16_t row = (int16_t)(cy + dy);
    if (xin < 0) {
      // Full disc row: cx-xout .. cx+xout
      d.drawFastHLine((int16_t)(cx - xout), row, (int16_t)(2*xout + 1), col);
    } else {
      // Ring row: two bands either side of the hole
      // Left:  cx-xout .. cx-(xin+1)
      int16_t lw = (int16_t)(xout - xin);
      if (lw > 0) d.drawFastHLine((int16_t)(cx - xout), row, lw, col);
      // Right: cx+(xin+1) .. cx+xout
      if (lw > 0) d.drawFastHLine((int16_t)(cx + xin + 1), row, lw, col);
    }
  }
}



// ---------------------------------------------------------------------------
//  Thick polyline with round joins and round caps.
//  Each segment drawn as a filled quad (drawThickLine), then a filled disc
//  at every join vertex covers the gaps — exactly "stroke-linejoin:round".
//  pts_buf unused but kept for API compatibility.
// ---------------------------------------------------------------------------
template <class DisplayT>
static inline void drawThickPolyline(DisplayT& d,
                                     const VGPoint* pts, int n,
                                     bool closed,
                                     uint16_t col, int16_t sw,
                                     VGPoint* /*pts_buf*/, int /*buf_cap*/) {
  if (n < 2 || sw < 1) return;
  int16_t capR = sw / 2;  // disc radius for joins and caps

  int segs = closed ? n : n - 1;
  for (int i = 0; i < segs; i++) {
    int j = (i + 1) % n;
    drawThickLine(d, pts[i].x, pts[i].y, pts[j].x, pts[j].y, col, sw);
  }
  // Round join disc at interior vertices (covers inter-segment gaps).
  // For open polylines skip the two endpoints — their caps would extend
  // sw/2 outward past the line end, overlapping adjacent elements.
  // For closed polylines every vertex is interior.
  if (capR > 0) {
    int first = closed ? 0 : 1;
    int last  = closed ? n : n - 1;
    for (int i = first; i < last; i++)
      vg_drawThickCircle(d, pts[i].x, pts[i].y, 0, sw, col, true);
  }
}

// ---------------------------------------------------------------------------
//  Polygon fill (scanline)
// ---------------------------------------------------------------------------
template <class DisplayT>
static inline void drawThickLine(DisplayT& d,
                                 int16_t x0, int16_t y0,
                                 int16_t x1, int16_t y1,
                                 uint16_t col, int16_t w) {
  if (w <= 1) { d.drawLine(x0, y0, x1, y1, col); return; }
  // Perpendicular offset rasterisation: for each scan row (or column),
  // compute the exact x (or y) span of the thick line rectangle and
  // draw a hline — no gaps at any angle.
  float dx = (float)(x1 - x0);
  float dy = (float)(y1 - y0);
  float len = sqrtf(dx*dx + dy*dy);
  if (len < 0.5f) {
    // Degenerate point — draw a filled square
    int16_t half = (int16_t)(w / 2);
    d.fillRect((int16_t)(x0-half), (int16_t)(y0-half), w, w, col);
    return;
  }
  // Unit perpendicular vector
  float px = -dy / len;
  float py =  dx / len;
  float half = (w - 1) * 0.5f;
  // Build 4 corners of the thick line rectangle
  float ax = x0 + px*half,  ay = y0 + py*half;
  float bx = x0 - px*half,  by = y0 - py*half;
  float cx2= x1 + px*half,  cy2= y1 + py*half;
  float dx2= x1 - px*half,  dy2= y1 - py*half;
  // Scanline fill the quad (2 triangles)
  // Use fillPolygon helper via a 4-point VGPoint array
  VGPoint pts[4] = {
    {iroundf(ax), iroundf(ay)},
    {iroundf(cx2),iroundf(cy2)},
    {iroundf(dx2),iroundf(dy2)},
    {iroundf(bx), iroundf(by)}
  };
  fillPolygon(d, pts, 4, col);
}

// ---------------------------------------------------------------------------
//  Polygon fill (scanline)
// ---------------------------------------------------------------------------
template <class DisplayT>
static inline void fillPolygon(DisplayT& d, const VGPoint* pts, int n,
                               uint16_t col) {
  if (n < 3) return;
  int16_t minY = pts[0].y, maxY = pts[0].y;
  for (int i = 1; i < n; i++) {
    if (pts[i].y < minY) minY = pts[i].y;
    if (pts[i].y > maxY) maxY = pts[i].y;
  }
  for (int16_t y = minY; y <= maxY; ++y) {
    int16_t xints[128]; int xi = 0;
    for (int i = 0; i < n; i++) {
      const VGPoint& a = pts[i];
      const VGPoint& b = pts[(i + 1) % n];
      if (a.y == b.y) continue;
      int16_t y0 = a.y, y1 = b.y, x0 = a.x, x1 = b.x;
      int16_t ymin = (y0 < y1) ? y0 : y1;
      int16_t ymax = (y0 < y1) ? y1 : y0;
      if (!(y >= ymin && y < ymax)) continue;
      int32_t num = (int32_t)(y - y0) * (int32_t)(x1 - x0);
      int32_t den = (int32_t)(y1 - y0);
      int32_t x   = (int32_t)x0 + (den != 0 ? (num / den) : 0);
      if (xi < 128) xints[xi++] = (int16_t)x;
    }
    if (xi < 2) continue;
    for (int i = 1; i < xi; i++) {
      int16_t v = xints[i]; int j = i - 1;
      while (j >= 0 && xints[j] > v) { xints[j+1] = xints[j]; --j; }
      xints[j+1] = v;
    }
    for (int i = 0; i+1 < xi; i += 2) {
      int16_t xa = xints[i], xb = xints[i+1];
      if (xb < xa) { int16_t t = xa; xa = xb; xb = t; }
      d.drawFastHLine(xa, y, (int16_t)(xb - xa + 1), col);
    }
  }
}

// ---------------------------------------------------------------------------
//  Cubic Bézier subdivision + rendering
//
//  Subdivide a single cubic segment (p0..p3) recursively until the chord
//  error is below `tol` pixels, then stroke or collect into a point buffer.
//
//  For stroke-only we draw line segments directly.
//  For fill we accumulate into a caller-supplied VGPoint buffer.
// ---------------------------------------------------------------------------

// Maximum recursion depth — limits stack use on microcontrollers.
// Depth 6 gives ≤ 64 segments per cubic; plenty for e-ink at ≤ 400 px wide.
static constexpr int VG_BEZIER_MAX_DEPTH = 8;

// Flatness tolerance (squared, in pixel units after mapping).
// 1.0 → sub-pixel accuracy; 2.0 is fine for e-ink.
static constexpr float VG_BEZIER_TOL2 = 2.0f;

// Inline float point to avoid heap.
struct VGFPt { float x, y; };

static inline float vg_dist2_line(VGFPt p, VGFPt a, VGFPt b) {
  // Squared distance from p to segment ab (used as flatness test)
  float dx = b.x - a.x, dy = b.y - a.y;
  float len2 = dx*dx + dy*dy;
  if (len2 < 1e-6f) { float ex=p.x-a.x, ey=p.y-a.y; return ex*ex+ey*ey; }
  float t = ((p.x-a.x)*dx + (p.y-a.y)*dy) / len2;
  if (t < 0.f) t = 0.f; else if (t > 1.f) t = 1.f;
  float ex = p.x-(a.x+t*dx), ey = p.y-(a.y+t*dy);
  return ex*ex + ey*ey;
}

// Stroke-only cubic subdivision (draws directly).
template <class DisplayT>
static void strokeCubic(DisplayT& d,
                        VGFPt p0, VGFPt p1, VGFPt p2, VGFPt p3,
                        uint16_t col, int16_t sw, int depth)
{
  // Flatness test: max squared distance of control points from chord
  float d1 = vg_dist2_line(p1, p0, p3);
  float d2 = vg_dist2_line(p2, p0, p3);
  if ((d1 + d2) <= VG_BEZIER_TOL2 || depth >= VG_BEZIER_MAX_DEPTH) {
    drawThickLine(d,
                  iroundf(p0.x), iroundf(p0.y),
                  iroundf(p3.x), iroundf(p3.y),
                  col, sw);
    return;
  }
  // De Casteljau split at t=0.5
  VGFPt q0 = {(p0.x+p1.x)*.5f, (p0.y+p1.y)*.5f};
  VGFPt q1 = {(p1.x+p2.x)*.5f, (p1.y+p2.y)*.5f};
  VGFPt q2 = {(p2.x+p3.x)*.5f, (p2.y+p3.y)*.5f};
  VGFPt r0 = {(q0.x+q1.x)*.5f, (q0.y+q1.y)*.5f};
  VGFPt r1 = {(q1.x+q2.x)*.5f, (q1.y+q2.y)*.5f};
  VGFPt s0 = {(r0.x+r1.x)*.5f, (r0.y+r1.y)*.5f};
  strokeCubic(d, p0, q0, r0, s0, col, sw, depth+1);
  strokeCubic(d, s0, r1, q2, p3, col, sw, depth+1);
}

// Fill-mode cubic subdivision — appends tessellated points to a buffer.
// Returns new fill count (caller must ensure buf has enough space).
static int fillCubicPts(VGPoint* buf, int count, int maxCount,
                        VGFPt p0, VGFPt p1, VGFPt p2, VGFPt p3, int depth,
                        float tol2 = VG_BEZIER_TOL2)
{
  float d1 = vg_dist2_line(p1, p0, p3);
  float d2 = vg_dist2_line(p2, p0, p3);
  if ((d1 + d2) <= tol2 || depth >= VG_BEZIER_MAX_DEPTH) {
    if (count < maxCount) { buf[count] = {iroundf(p3.x), iroundf(p3.y)}; count++; }
    return count;
  }
  VGFPt q0 = {(p0.x+p1.x)*.5f, (p0.y+p1.y)*.5f};
  VGFPt q1 = {(p1.x+p2.x)*.5f, (p1.y+p2.y)*.5f};
  VGFPt q2 = {(p2.x+p3.x)*.5f, (p2.y+p3.y)*.5f};
  VGFPt r0 = {(q0.x+q1.x)*.5f, (q0.y+q1.y)*.5f};
  VGFPt r1 = {(q1.x+q2.x)*.5f, (q1.y+q2.y)*.5f};
  VGFPt s0 = {(r0.x+r1.x)*.5f, (r0.y+r1.y)*.5f};
  count = fillCubicPts(buf, count, maxCount, p0, q0, r0, s0, depth+1, tol2);
  count = fillCubicPts(buf, count, maxCount, s0, r1, q2, p3, depth+1, tol2);
  return count;
}

// ---------------------------------------------------------------------------
//  Main renderer
// ---------------------------------------------------------------------------
template <class DisplayT>
bool drawVG_Q1(DisplayT& d, const uint8_t* prog,
               int16_t dx, int16_t dy, int16_t dw, int16_t dh,
               uint16_t palette0, uint16_t palette1)
{
  const uint8_t* p = prog;

  if (rd_u8(p) != OP_VIEWBOX) return false;
  uint16_t vbW = rd_u16(p);
  uint16_t vbH = rd_u16(p);
  if (vbW == 0 || vbH == 0) return false;

  // Scale uniformly so the icon fills the draw box (xMidYMid slice):
  // use max(sx,sy) so the larger axis fills exactly, and the smaller
  // axis overflows by at most half a cell each side (centered).
  // This prevents letterboxing / empty space around non-square icons.
  float sx = (float)dw / (float)vbW;
  float sy = (float)dh / (float)vbH;
  float s  = (sx > sy) ? sx : sy;  // max → fill/slice

  // Center the scaled viewbox over the draw box
  float usedW = (float)vbW * s;
  float usedH = (float)vbH * s;
  int16_t ox = (int16_t)(dx + (dw - usedW) * 0.5f);
  int16_t oy = (int16_t)(dy + (dh - usedH) * 0.5f);

  bool    fillOn   = true;
  bool    strokeOn = false;
  uint8_t strokeW  = 1;
  uint16_t col     = palette0;
  // Tessellation tolerance in VG-space: target 0.5px deviation at display resolution
  float tess_tol2  = 0.25f / (s * s);  // 0.25 = 0.5^2

  // Shared point buffer (stack) — used by POLY and SPLINE fill
  static constexpr int VG_MAX_POINTS = 256;
  VGPoint pts[VG_MAX_POINTS];

  for (;;) {
    uint8_t op = rd_u8(p);
    if (op == OP_END) break;

    // ------------------------------------------------------------------
    if (op == OP_STYLE) {
      uint8_t flags  = rd_u8(p);
      strokeW        = rd_u8(p);
      uint8_t cidx   = rd_u8(p);
      fillOn   = (flags & STYLE_FILL)   != 0;
      strokeOn = (flags & STYLE_STROKE) != 0;
      col      = (cidx == 0) ? palette0 : palette1;
      // Scale stroke width from viewbox units to display pixels
      strokeW = (uint8_t)(iroundf((float)strokeW * s));
      if (strokeW < 1) strokeW = 1;

    // ------------------------------------------------------------------
    } else if (op == OP_LINE) {
      int16_t x0=rd_s16(p), y0=rd_s16(p), x1=rd_s16(p), y1=rd_s16(p);
      if (!strokeOn && !fillOn) continue;
      int16_t X0=map_i1(x0,s,ox), Y0=map_i1(y0,s,oy);
      int16_t X1=map_i1(x1,s,ox), Y1=map_i1(y1,s,oy);
      { VGPoint lp[2] = {{X0,Y0},{X1,Y1}};
        VGPoint lb[8];
        drawThickPolyline(d, lp, 2, false, col,
                          strokeOn?(int16_t)strokeW:1, lb, 8); }

    // ------------------------------------------------------------------
    } else if (op == OP_RECT) {
      int16_t x=rd_s16(p),y=rd_s16(p),w=rd_s16(p),h=rd_s16(p),rx=rd_s16(p);
      int16_t X=map_i1(x,s,ox), Y=map_i1(y,s,oy);
      int16_t W=iroundf((float)w*s), H=iroundf((float)h*s), R=iroundf((float)rx*s);
      // Clamp R so tiny scaled rects don't get a radius that distorts them.
      // Rule: r <= min(W,H)/4  keeps corners visually subtle at all sizes;
      // when W or H < 6px the radius is zeroed and a plain rect is drawn.
      { int16_t minWH = W < H ? W : H;
        if (minWH < 6) R = 0;
        else { int16_t maxR = minWH / 4; if (R > maxR) R = maxR; } }
      if (fillOn)   { if (R>0) d.fillRoundRect(X,Y,W,H,R,col); else d.fillRect(X,Y,W,H,col); }
      if (strokeOn) {
        // Thick rect outline: filled outer shape minus filled inner shape
        int16_t sw2 = (int16_t)strokeW;
        int16_t h2  = (int16_t)(sw2 / 2);
        int16_t Xo=X-h2, Yo=Y-h2, Wo=(int16_t)(W+sw2-1), Ho=(int16_t)(H+sw2-1);
        int16_t Xi=(int16_t)(X+h2+1), Yi=(int16_t)(Y+h2+1);
        int16_t Wi=(int16_t)(W-sw2-1), Hi=(int16_t)(H-sw2-1);
        int16_t Ro=(int16_t)(R>0?R+h2:0), Ri=(int16_t)(R>0&&Wi>0&&Hi>0?R-h2:0);
        if (Ro > 0) { d.fillRoundRect(Xo,Yo,Wo,Ho,Ro,col);
          if (Wi>0&&Hi>0) { uint16_t bg=(col==palette0)?palette1:palette0;
            if(Ri>0) d.fillRoundRect(Xi,Yi,Wi,Hi,Ri,bg); else d.fillRect(Xi,Yi,Wi,Hi,bg); }
        } else {
          d.fillRect(Xo,Yo,Wo,(int16_t)sw2,col); d.fillRect(Xo,(int16_t)(Yo+Ho-sw2),Wo,(int16_t)sw2,col);
          d.fillRect(Xo,(int16_t)(Yo+sw2),(int16_t)sw2,(int16_t)(Ho-sw2*2),col);
          d.fillRect((int16_t)(Xo+Wo-sw2),(int16_t)(Yo+sw2),(int16_t)sw2,(int16_t)(Ho-sw2*2),col);
        }
      }

    // ------------------------------------------------------------------
    } else if (op == OP_CIRCLE) {
      int16_t cx=rd_s16(p),cy=rd_s16(p),r=rd_s16(p);
      int16_t X=map_i1(cx,s,ox), Y=map_i1(cy,s,oy);
      int16_t R=iroundf((float)r*s);
      // Scanline-based thick circle/ring — gap-free, background-safe
      if (fillOn || strokeOn)
        vg_drawThickCircle(d, X, Y, R,
                           strokeOn ? (int16_t)strokeW : (int16_t)0,
                           col, fillOn);

    // ------------------------------------------------------------------
    } else if (op == OP_POLY) {
      uint8_t n   = rd_u8(p);
      uint8_t pf  = rd_u8(p);
      bool closed = (pf & POLY_CLOSED) != 0;
      int nread = (n > VG_MAX_POINTS) ? VG_MAX_POINTS : (int)n;
      for (int i = 0; i < (int)n; i++) {
        int16_t x=rd_s16(p), y=rd_s16(p);
        if (i < nread) { pts[i].x=map_i1(x,s,ox); pts[i].y=map_i1(y,s,oy); }
      }
      if (nread >= 3 && closed && fillOn)
        fillPolygon(d, pts, nread, col);
      if (strokeOn && nread >= 2) {
        VGPoint poly_buf[VG_MAX_POINTS * 2 + 4];
        drawThickPolyline(d, pts, nread, closed, col, strokeW,
                          poly_buf, VG_MAX_POINTS*2+4);
      }

    // ------------------------------------------------------------------
    //  OP_SPLINE: chain of n cubic Bézier segments.
    //  Wire format:  n(u8) pflags(u8)  P0.x P0.y  [C1 C2 P1]*n   (all s16)
    //
    //  Each segment shares its start point with the previous end point,
    //  so total control points = 1 + 3*n.
    //
    //  For stroke: subdivide each segment individually (no buffering).
    //  For fill:   tessellate all segments into pts[], then scanline fill.
    // ------------------------------------------------------------------
    } else if (op == OP_SPLINE) {
      uint8_t n   = rd_u8(p);
      uint8_t pf  = rd_u8(p);
      bool closed = (pf & SPLINE_CLOSED) != 0;
      if (n == 0) continue;

      // Map the anchor point (P0)
      float ax = map_i1(rd_s16(p), s, ox);
      float ay = map_i1(rd_s16(p), s, oy);

      if (fillOn) {
        // Tessellate into pts[] for scanline fill
        int npts = 0;
        pts[npts++] = { iroundf(ax), iroundf(ay) };

        float px = ax, py = ay;  // running current point
        for (int seg = 0; seg < (int)n; seg++) {
          float c1x = map_i1(rd_s16(p), s, ox);
          float c1y = map_i1(rd_s16(p), s, oy);
          float c2x = map_i1(rd_s16(p), s, ox);
          float c2y = map_i1(rd_s16(p), s, oy);
          float ex  = map_i1(rd_s16(p), s, ox);
          float ey  = map_i1(rd_s16(p), s, oy);
          npts = fillCubicPts(pts, npts, VG_MAX_POINTS,
                              {px,py}, {c1x,c1y}, {c2x,c2y}, {ex,ey}, 0);
          px = ex; py = ey;
        }
        if (npts >= 3)
          fillPolygon(d, pts, npts, col);

      } else if (strokeOn) {
        // Tessellate all spline segments into pts[] then drawThickPolyline
        int npts = 0;
        pts[npts++] = { iroundf(ax), iroundf(ay) };
        float px = ax, py = ay;
        for (int seg = 0; seg < (int)n; seg++) {
          float c1x = map_i1(rd_s16(p), s, ox);
          float c1y = map_i1(rd_s16(p), s, oy);
          float c2x = map_i1(rd_s16(p), s, ox);
          float c2y = map_i1(rd_s16(p), s, oy);
          float ex  = map_i1(rd_s16(p), s, ox);
          float ey  = map_i1(rd_s16(p), s, oy);
          npts = fillCubicPts(pts, npts, VG_MAX_POINTS,
                              {px,py},{c1x,c1y},{c2x,c2y},{ex,ey}, 0, tess_tol2);
          px = ex; py = ey;
        }
        VGPoint spl_buf[VG_MAX_POINTS * 2 + 4];
        drawThickPolyline(d, pts, npts, closed, col, strokeW,
                          spl_buf, VG_MAX_POINTS*2+4);
      } else {
        // neither fill nor stroke — just consume the bytes
        for (int seg = 0; seg < (int)n; seg++) {
          rd_s16(p); rd_s16(p);  // c1
          rd_s16(p); rd_s16(p);  // c2
          rd_s16(p); rd_s16(p);  // endpoint
        }
      }

    // ------------------------------------------------------------------
    } else {
      return false;  // unknown opcode
    }
  }
  return true;
}