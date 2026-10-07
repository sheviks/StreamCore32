#!/usr/bin/env python3
# svg_folder_to_vg_header.py
#
# Generates a single .h from all .svg in a folder.
# Object name = filename without extension (sanitized).
#
# Output stream (VG_Q1, integer coords):
#   OP_VIEWBOX (u16 w,h)
#   OP_STYLE (flags, strokeW, colorIdx)
#   OP_LINE / OP_RECT / OP_CIRCLE / OP_POLY / OP_SPLINE
#   OP_END
#
# Supported SVG elements:
#   line, rect, circle, ellipse, polyline, polygon, path
#
# Supported path commands:
#   M/m L/l H/h V/v Z/z C/c S/s Q/q T/t A/a
#
# Curve handling strategy (minimises byte count):
#   - Cubic bezier runs (C/c/S/s) → OP_SPLINE (most compact for stroked icons)
#   - Quadratic bezier (Q/q/T/t) → elevated to cubic, then OP_SPLINE
#   - Arc (A/a)                  → approximated as 1–4 cubic segments, OP_SPLINE
#   - Straight-only subpaths     → OP_POLY (no change)
#   - The SPLINE opcode stores raw control points (4 bytes/s16 each) so a
#     spline of n segments costs 5 + (1+3n)*4 bytes vs tessellating to ~8
#     line points per curve which costs 5 + 8*4 bytes per segment.
#
# --box WxH scales SVG to fit into the box (preserve aspect) and sets
#           OP_VIEWBOX to the scaled size.

import argparse
import math
import os
import re
import sys
import xml.etree.ElementTree as ET
from dataclasses import dataclass
from typing import Optional, List, Tuple, Dict

# Opcodes (must match eink_vg.h)
OP_END     = 0
OP_VIEWBOX = 1
OP_STYLE   = 2
OP_LINE    = 3
OP_RECT    = 4
OP_CIRCLE  = 5
OP_POLY    = 6
OP_SPLINE  = 7

STYLE_FILL    = 0x01
STYLE_STROKE  = 0x02
POLY_CLOSED   = 0x01
SPLINE_CLOSED = 0x01

NUM_RE = re.compile(r'[-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?')

PATH_TOK_RE = re.compile(
    r'[AaCcHhLlMmQqSsTtVvZz]|[-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?'
)

# ---------------------------------------------------------------------------
#  Misc helpers
# ---------------------------------------------------------------------------

def strip_ns(tag: str) -> str:
    return tag.split('}', 1)[-1] if '}' in tag else tag

def parse_number(s: Optional[str], default: float = 0.0) -> float:
    if not s:
        return default
    m = NUM_RE.search(s)
    return float(m.group(0)) if m else default

def round_i(x: float) -> int:
    return int(x + 0.5) if x >= 0 else int(x - 0.5)

def pack_u16(v: int) -> bytes:
    v &= 0xFFFF
    return bytes((v & 0xFF, (v >> 8) & 0xFF))

def pack_s16(v: int) -> bytes:
    if v < 0:
        v = (1 << 16) + v
    return pack_u16(v)

def bytes_to_c_init(b: bytes, indent: str = "  ") -> str:
    hexes = [f"0x{v:02X}" for v in b]
    lines = []
    for i in range(0, len(hexes), 16):
        lines.append(indent + ", ".join(hexes[i:i + 16]))
    return ",\n".join(lines)

def safe_c_ident(filename: str) -> str:
    name = os.path.splitext(os.path.basename(filename))[0]
    name = re.sub(r'[^a-zA-Z0-9_]', '_', name)
    if not name:
        name = "svg_asset"
    if name[0].isdigit():
        name = "_" + name
    return name

# ---------------------------------------------------------------------------
#  Style handling
# ---------------------------------------------------------------------------

@dataclass(frozen=True)
class Style:
    fill: Optional[str] = None
    stroke: Optional[str] = None
    stroke_width: Optional[float] = None
    fill_rule: Optional[str] = None

def parse_style_attr(style_str: str) -> Dict[str, str]:
    out: Dict[str, str] = {}
    if not style_str:
        return out
    for part in style_str.split(';'):
        if ':' not in part:
            continue
        k, v = part.split(':', 1)
        out[k.strip().lower()] = v.strip()
    return out

def merged_style(parent: Style, elem: ET.Element) -> Style:
    style_map = parse_style_attr(elem.attrib.get("style", ""))
    def pick(attr: str) -> Optional[str]:
        return elem.attrib.get(attr, style_map.get(attr))
    fill   = pick("fill")
    stroke = pick("stroke")
    sw     = pick("stroke-width")
    fr     = pick("fill-rule")
    return Style(
        fill         = fill   if fill   is not None else parent.fill,
        stroke       = stroke if stroke is not None else parent.stroke,
        stroke_width = float(parse_number(sw, 0.0)) if sw is not None else parent.stroke_width,
        fill_rule    = fr.lower() if fr is not None else parent.fill_rule,
    )

def color_to_palette_idx(color: Optional[str]) -> Optional[int]:
    if color is None:
        return None
    c = color.strip().lower()
    if c == "none":
        return None
    if c == "black":
        return 0
    if c == "white":
        return 1
    if c in ("currentcolor", "current-color"):
        return 0
    def luma_idx(r: int, g: int, b: int) -> int:
        y = (r * 30 + g * 59 + b * 11) // 100
        return 1 if y >= 128 else 0
    if c.startswith("#"):
        if len(c) == 4:
            return luma_idx(int(c[1],16)*17, int(c[2],16)*17, int(c[3],16)*17)
        if len(c) == 7:
            return luma_idx(int(c[1:3],16), int(c[3:5],16), int(c[5:7],16))
    if c.startswith("rgb(") and c.endswith(")"):
        parts = c[4:-1].split(",")
        if len(parts) >= 3:
            r = max(0, min(255, int(parse_number(parts[0], 0))))
            g = max(0, min(255, int(parse_number(parts[1], 0))))
            b = max(0, min(255, int(parse_number(parts[2], 0))))
            return luma_idx(r, g, b)
    return 0

def style_to_key(style: Style, scale: float = 1.0) -> Tuple[int, int, int]:
    fill_c   = style.fill   if style.fill   is not None else "black"
    stroke_c = style.stroke if style.stroke is not None else "none"
    sw_f     = style.stroke_width if style.stroke_width is not None else 1.0
    fill_idx   = color_to_palette_idx(fill_c)
    stroke_idx = color_to_palette_idx(stroke_c)
    flags = 0
    if fill_idx   is not None: flags |= STYLE_FILL
    if stroke_idx is not None: flags |= STYLE_STROKE
    # Scale stroke-width by the SVG→pixel scale factor so sw is in pixel units
    sw   = max(1, min(255, round_i(sw_f * scale)))
    cidx = int(stroke_idx) if stroke_idx is not None else (int(fill_idx) if fill_idx is not None else 0)
    return (flags, sw, cidx)

def parse_viewbox(root: ET.Element) -> Tuple[float, float, float, float]:
    vb = root.attrib.get("viewBox") or root.attrib.get("viewbox")
    if vb:
        nums = [float(x) for x in NUM_RE.findall(vb)]
        if len(nums) >= 4:
            return nums[0], nums[1], nums[2], nums[3]
    w = parse_number(root.attrib.get("width"), 0.0)
    h = parse_number(root.attrib.get("height"), 0.0)
    return 0.0, 0.0, w, h

# ---------------------------------------------------------------------------
#  Emit helpers
# ---------------------------------------------------------------------------

def emit_line(out: bytearray, x0: int, y0: int, x1: int, y1: int) -> None:
    out.append(OP_LINE)
    out += pack_s16(x0) + pack_s16(y0) + pack_s16(x1) + pack_s16(y1)

def emit_rect(out: bytearray, x: int, y: int, w: int, h: int, rx: int) -> None:
    out.append(OP_RECT)
    out += pack_s16(x) + pack_s16(y) + pack_s16(w) + pack_s16(h) + pack_s16(rx)

def emit_circle(out: bytearray, cx: int, cy: int, r: int) -> None:
    out.append(OP_CIRCLE)
    out += pack_s16(cx) + pack_s16(cy) + pack_s16(r)

def emit_poly(out: bytearray, pts: List[Tuple[int, int]], closed: bool,
              max_points: int) -> None:
    if len(pts) < 2:
        return
    if len(pts) > max_points:
        pts = pts[:max_points]
    if len(pts) > 255:
        pts = pts[:255]
    out.append(OP_POLY)
    out.append(len(pts) & 0xFF)
    out.append(POLY_CLOSED if closed else 0x00)
    for (x, y) in pts:
        out += pack_s16(x) + pack_s16(y)

def emit_spline(out: bytearray,
                segments: List[Tuple[Tuple[int,int], Tuple[int,int], Tuple[int,int]]],
                start: Tuple[int, int],
                closed: bool) -> None:
    """
    Emit OP_SPLINE.
    segments: list of (c1, c2, endpoint) tuples; start is P0.
    Wire: OP_SPLINE n pflags P0 [C1 C2 P1]*n
    """
    n = len(segments)
    if n == 0:
        return
    if n > 255:
        n = 255
        segments = segments[:255]
    out.append(OP_SPLINE)
    out.append(n & 0xFF)
    out.append(SPLINE_CLOSED if closed else 0x00)
    out += pack_s16(start[0]) + pack_s16(start[1])
    for (c1, c2, ep) in segments:
        out += pack_s16(c1[0]) + pack_s16(c1[1])
        out += pack_s16(c2[0]) + pack_s16(c2[1])
        out += pack_s16(ep[0]) + pack_s16(ep[1])

# ---------------------------------------------------------------------------
#  Curve approximation utilities
# ---------------------------------------------------------------------------

def quad_to_cubic(p0: Tuple[float,float],
                  q:  Tuple[float,float],
                  p2: Tuple[float,float]
                  ) -> Tuple[Tuple[float,float], Tuple[float,float]]:
    """Elevate quadratic Bézier to cubic control points."""
    c1 = (p0[0] + (2/3)*(q[0]-p0[0]),  p0[1] + (2/3)*(q[1]-p0[1]))
    c2 = (p2[0] + (2/3)*(q[0]-p2[0]),  p2[1] + (2/3)*(q[1]-p2[1]))
    return c1, c2

def arc_to_cubics(x1: float, y1: float,
                  rx: float, ry: float, phi_deg: float,
                  large_arc: int, sweep: int,
                  x2: float, y2: float
                  ) -> List[Tuple[Tuple[float,float],
                                  Tuple[float,float],
                                  Tuple[float,float],
                                  Tuple[float,float]]]:
    """
    Convert SVG arc to a list of cubic Bézier segments.
    Each element is (P0, C1, C2, P1) in absolute coordinates.
    Returns [] when the arc is degenerate (same start/end or zero radii).
    """
    if abs(x2-x1) < 1e-6 and abs(y2-y1) < 1e-6:
        return []
    if abs(rx) < 1e-6 or abs(ry) < 1e-6:
        # Degenerate: straight line — caller handles as line_to
        return []

    phi = math.radians(phi_deg)
    cos_phi, sin_phi = math.cos(phi), math.sin(phi)
    rx, ry = abs(rx), abs(ry)

    # Step 1: compute (x1', y1')
    dx2 = (x1 - x2) / 2.0
    dy2 = (y1 - y2) / 2.0
    x1p =  cos_phi * dx2 + sin_phi * dy2
    y1p = -sin_phi * dx2 + cos_phi * dy2

    # Step 2: correct radii
    lam = (x1p/rx)**2 + (y1p/ry)**2
    if lam > 1.0:
        lam = math.sqrt(lam)
        rx *= lam
        ry *= lam

    # Step 3: compute center
    num = max(0.0, (rx*ry)**2 - (rx*y1p)**2 - (ry*x1p)**2)
    den = (rx*y1p)**2 + (ry*x1p)**2
    sq  = math.sqrt(num / den) if den > 1e-10 else 0.0
    if large_arc == sweep:
        sq = -sq
    cxp =  sq * rx * y1p / ry
    cyp = -sq * ry * x1p / rx
    cx  = cos_phi*cxp - sin_phi*cyp + (x1+x2)/2.0
    cy  = sin_phi*cxp + cos_phi*cyp + (y1+y2)/2.0

    # Step 4: compute angles
    def angle(ux, uy, vx, vy):
        n = math.sqrt(ux*ux + uy*uy) * math.sqrt(vx*vx + vy*vy)
        if n < 1e-10: return 0.0
        c = max(-1.0, min(1.0, (ux*vx + uy*vy) / n))
        a = math.acos(c)
        return -a if (ux*vy - uy*vx) < 0 else a

    theta1 = angle(1, 0, (x1p-cxp)/rx, (y1p-cyp)/ry)
    dtheta = angle((x1p-cxp)/rx, (y1p-cyp)/ry, (-x1p-cxp)/rx, (-y1p-cyp)/ry)

    if not sweep and dtheta > 0:   dtheta -= 2*math.pi
    if     sweep and dtheta < 0:   dtheta += 2*math.pi

    # Step 5: split into ≤4 segments of ≤π/2
    n_segs = max(1, math.ceil(abs(dtheta) / (math.pi / 2)))
    dt = dtheta / n_segs
    alpha = math.sin(dt) * (math.sqrt(4 + 3*math.tan(dt/2)**2) - 1) / 3.0

    def pt_on_arc(t):
        c, s = math.cos(t), math.sin(t)
        return (cos_phi*(rx*c) - sin_phi*(ry*s) + cx,
                sin_phi*(rx*c) + cos_phi*(ry*s) + cy)

    def dt_on_arc(t):
        c, s = math.cos(t), math.sin(t)
        return (cos_phi*(-rx*s) - sin_phi*(ry*c),
                sin_phi*(-rx*s) + cos_phi*(ry*c))

    result = []
    t = theta1
    p_start = pt_on_arc(t)
    for _ in range(n_segs):
        d1 = dt_on_arc(t)
        t += dt
        d2 = dt_on_arc(t)
        p_end = pt_on_arc(t)
        c1 = (p_start[0] + alpha*d1[0], p_start[1] + alpha*d1[1])
        c2 = (p_end[0]   - alpha*d2[0], p_end[1]   - alpha*d2[1])
        result.append((p_start, c1, c2, p_end))
        p_start = p_end
    return result

# ---------------------------------------------------------------------------
#  Subpath type: a sequence of moves described as a mix of line and cubic segs
# ---------------------------------------------------------------------------

# A segment is either:
#   ('L', (x, y))                      — straight line to
#   ('C', (c1x,c1y), (c2x,c2y), (ex,ey)) — cubic bezier to
PT_FLOAT = Tuple[float, float]

@dataclass
class Subpath:
    start: PT_FLOAT
    segs: list   # list of ('L', pt) or ('C', c1, c2, ep)
    closed: bool
    has_curve: bool

    def endpoints(self) -> List[PT_FLOAT]:
        """All points including start (for circle-fit / bbox)."""
        pts = [self.start]
        for s in self.segs:
            pts.append(s[-1])
        return pts

# ---------------------------------------------------------------------------
#  Circle-fit
# ---------------------------------------------------------------------------

def maybe_circle(pts_f: List[PT_FLOAT]) -> Optional[Tuple[float,float,float]]:
    """Return (cx, cy, r) if points lie on a circle within 8% tolerance."""
    xs = [p[0] for p in pts_f]
    ys = [p[1] for p in pts_f]
    w = max(xs) - min(xs)
    h = max(ys) - min(ys)
    if w <= 0 or h <= 0:
        return None
    if abs(w - h) / max(w, h) > 0.08:
        return None
    cx = (min(xs) + max(xs)) * 0.5
    cy = (min(ys) + max(ys)) * 0.5
    rs = [math.hypot(x-cx, y-cy) for (x,y) in pts_f]
    r  = sum(rs) / len(rs)
    if r <= 0.5:
        return None
    if max(abs(rv-r) for rv in rs) / r > 0.08:
        return None
    return cx, cy, r

def point_in_poly(pt: Tuple[int,int], poly: List[Tuple[int,int]]) -> bool:
    x, y   = pt
    inside = False
    n      = len(poly)
    for i in range(n):
        x0, y0 = poly[i]
        x1, y1 = poly[(i+1) % n]
        if (y0 > y) != (y1 > y):
            xin = x0 + (y - y0) * (x1-x0) / (y1-y0)
            if xin > x:
                inside = not inside
    return inside

# ---------------------------------------------------------------------------
#  SVG → subpath list
# ---------------------------------------------------------------------------

def parse_points_str(s: str) -> List[PT_FLOAT]:
    nums = [float(x) for x in NUM_RE.findall(s)]
    return [(nums[i], nums[i+1]) for i in range(0, len(nums)-1, 2)]

def path_d_to_subpaths(d: str) -> List[Subpath]:
    """Parse SVG path d attribute into Subpath objects."""
    toks = PATH_TOK_RE.findall(d)
    i    = 0

    def read_float() -> Optional[float]:
        nonlocal i
        if i >= len(toks) or (len(toks[i]) == 1 and toks[i].isalpha()):
            return None
        v = float(toks[i]); i += 1
        return v

    subpaths: List[Subpath] = []
    curx = 0.0; cury = 0.0
    startx: Optional[float] = None
    starty: Optional[float] = None
    segs:  list = []
    has_curve = False
    last_c2: Optional[PT_FLOAT] = None   # for S/s reflection
    last_q:  Optional[PT_FLOAT] = None   # for T/t reflection

    def flush(do_close: bool) -> None:
        nonlocal segs, startx, starty, has_curve, last_c2, last_q
        if segs or (startx is not None):
            sp = Subpath(start=(startx or 0.0, starty or 0.0),
                         segs=list(segs),
                         closed=do_close,
                         has_curve=has_curve)
            subpaths.append(sp)
        segs = []
        startx = starty = None
        has_curve = False
        last_c2 = None
        last_q  = None

    def ensure_started() -> None:
        nonlocal startx, starty
        if startx is None:
            startx, starty = curx, cury

    def line_to(nx: float, ny: float) -> None:
        nonlocal curx, cury, last_c2, last_q
        ensure_started()
        segs.append(('L', (nx, ny)))
        curx, cury = nx, ny
        last_c2 = None
        last_q  = None

    def cubic_to(c1: PT_FLOAT, c2: PT_FLOAT, ep: PT_FLOAT) -> None:
        nonlocal curx, cury, has_curve, last_c2, last_q
        ensure_started()
        segs.append(('C', c1, c2, ep))
        curx, cury = ep
        has_curve  = True
        last_c2    = c2
        last_q     = None

    cmd: Optional[str] = None
    while i < len(toks):
        t = toks[i]
        if len(t) == 1 and t.isalpha():
            cmd = t; i += 1
        if cmd is None:
            i += 1; continue

        rel = cmd.islower()
        c   = cmd.upper()

        if c == "Z":
            if startx is not None:
                curx, cury = startx, starty
            flush(True)
            cmd = None
            continue

        if c == "M":
            x = read_float(); y = read_float()
            if x is None or y is None: continue
            if rel: x += curx; y += cury
            if segs or startx is not None:
                flush(False)
            curx, cury = x, y
            startx, starty = curx, cury
            segs = []
            # Subsequent pairs are implicit L
            while True:
                x = read_float(); y = read_float()
                if x is None or y is None: break
                if rel: x += curx; y += cury
                line_to(x, y)

        elif c == "L":
            while True:
                x = read_float(); y = read_float()
                if x is None or y is None: break
                if rel: x += curx; y += cury
                line_to(x, y)

        elif c == "H":
            while True:
                x = read_float()
                if x is None: break
                if rel: x += curx
                line_to(x, cury)

        elif c == "V":
            while True:
                y = read_float()
                if y is None: break
                if rel: y += cury
                line_to(curx, y)

        elif c == "C":
            while True:
                x1 = read_float(); y1 = read_float()
                x2 = read_float(); y2 = read_float()
                x  = read_float(); y  = read_float()
                if None in (x1, y1, x2, y2, x, y): break
                if rel:
                    x1 += curx; y1 += cury
                    x2 += curx; y2 += cury
                    x  += curx; y  += cury
                cubic_to((x1,y1), (x2,y2), (x,y))

        elif c == "S":
            while True:
                x2 = read_float(); y2 = read_float()
                x  = read_float(); y  = read_float()
                if None in (x2, y2, x, y): break
                if rel:
                    x2 += curx; y2 += cury
                    x  += curx; y  += cury
                # Reflect previous C2
                if last_c2:
                    x1 = 2*curx - last_c2[0]
                    y1 = 2*cury - last_c2[1]
                else:
                    x1, y1 = curx, cury
                cubic_to((x1,y1), (x2,y2), (x,y))

        elif c == "Q":
            while True:
                qx = read_float(); qy = read_float()
                x  = read_float(); y  = read_float()
                if None in (qx, qy, x, y): break
                if rel:
                    qx += curx; qy += cury
                    x  += curx; y  += cury
                c1, c2 = quad_to_cubic((curx,cury), (qx,qy), (x,y))
                last_q = (qx, qy)
                cubic_to(c1, c2, (x,y))

        elif c == "T":
            while True:
                x = read_float(); y = read_float()
                if None in (x, y): break
                if rel: x += curx; y += cury
                # Reflect previous Q control point
                if last_q:
                    qx = 2*curx - last_q[0]
                    qy = 2*cury - last_q[1]
                else:
                    qx, qy = curx, cury
                c1, c2 = quad_to_cubic((curx,cury), (qx,qy), (x,y))
                last_q = (qx, qy)
                cubic_to(c1, c2, (x,y))

        elif c == "A":
            while True:
                rx   = read_float(); ry  = read_float()
                rot  = read_float()
                laf  = read_float(); sf  = read_float()
                x    = read_float(); y   = read_float()
                if None in (rx, ry, rot, laf, sf, x, y): break
                if rel: x += curx; y += cury
                cubics = arc_to_cubics(curx, cury,
                                       float(rx), float(ry), float(rot),
                                       int(laf), int(sf), x, y)
                if not cubics:
                    line_to(x, y)
                else:
                    for (p0, c1pt, c2pt, p1) in cubics:
                        cubic_to(c1pt, c2pt, p1)

        else:
            # Unknown: skip until next command
            while i < len(toks) and not (len(toks[i]) == 1 and toks[i].isalpha()):
                i += 1

    if segs or startx is not None:
        flush(False)

    return subpaths

# ---------------------------------------------------------------------------
#  Emit a single Subpath → POLY or SPLINE, choosing whichever is smaller
# ---------------------------------------------------------------------------

# Adaptive subdivision tolerance (squared pixel error after mapping).
# 1.0 = sub-pixel; 4.0 = fine for small e-ink icons; increase for very small icons.
BEZIER_TOL2 = 1.5

def _cubic_flat(p0, c1, c2, p3, tol2):
    """True if cubic segment is flat within tol2."""
    def dist2_line(px, py, ax, ay, bx, by):
        dx, dy = bx-ax, by-ay
        l2 = dx*dx + dy*dy
        if l2 < 1e-8: return (px-ax)**2 + (py-ay)**2
        t = max(0., min(1., ((px-ax)*dx + (py-ay)*dy) / l2))
        ex, ey = px-(ax+t*dx), py-(ay+t*dy)
        return ex*ex + ey*ey
    d1 = dist2_line(c1[0],c1[1], p0[0],p0[1], p3[0],p3[1])
    d2 = dist2_line(c2[0],c2[1], p0[0],p0[1], p3[0],p3[1])
    return (d1 + d2) <= tol2


def _conv_pt(pt: PT_FLOAT, scale: float, minx: float, miny: float) -> Tuple[float, float]:
    """Map SVG float coords → pixel float coords (not rounded yet)."""
    return (pt[0] - minx) * scale, (pt[1] - miny) * scale

def _tessellate_cubic_px(p0: Tuple[float,float],
                          c1: Tuple[float,float],
                          c2: Tuple[float,float],
                          p3: Tuple[float,float],
                          tol2: float,
                          depth: int = 0,
                          max_depth: int = 6) -> List[Tuple[float,float]]:
    """
    Tessellate a cubic in **pixel** float space.
    Returns list of endpoint coords (not including p0).
    tol2 is flatness threshold in squared pixels — meaningful at any SVG scale.
    """
    if _cubic_flat(p0, c1, c2, p3, tol2) or depth >= max_depth:
        return [p3]
    q0 = ((p0[0]+c1[0])*.5, (p0[1]+c1[1])*.5)
    q1 = ((c1[0]+c2[0])*.5, (c1[1]+c2[1])*.5)
    q2 = ((c2[0]+p3[0])*.5, (c2[1]+p3[1])*.5)
    r0 = ((q0[0]+q1[0])*.5, (q0[1]+q1[1])*.5)
    r1 = ((q1[0]+q2[0])*.5, (q1[1]+q2[1])*.5)
    s0 = ((r0[0]+r1[0])*.5, (r0[1]+r1[1])*.5)
    return (_tessellate_cubic_px(p0, q0, r0, s0, tol2, depth+1, max_depth) +
            _tessellate_cubic_px(s0, r1, q2, p3, tol2, depth+1, max_depth))

def emit_subpath(out: bytearray,
                 sp: Subpath,
                 conv_xy,
                 conv_len,
                 scale: float,
                 minx: float,
                 miny: float,
                 max_points: int,
                 tol2: float) -> None:
    """
    Emit a Subpath as OP_POLY or OP_SPLINE, choosing whichever is smaller.
    Pure-line subpaths always emit as POLY.
    Tessellation happens in pixel-float space so tol2 (squared pixels) is
    meaningful regardless of the SVG viewBox size.
    """
    if not sp.segs:
        return

    start_i = conv_xy(*sp.start)

    if not sp.has_curve:
        # Pure straight-line subpath → POLY
        ipts = [start_i] + [conv_xy(*s[-1]) for s in sp.segs]
        emit_poly(out, ipts, sp.closed, max_points)
        return

    # ------------------------------------------------------------------
    # Build POLY candidate: tessellate every cubic in pixel-float space
    # ------------------------------------------------------------------
    poly_pts: List[Tuple[int,int]] = [start_i]
    cur_svg = sp.start
    cur_px  = _conv_pt(cur_svg, scale, minx, miny)

    for seg in sp.segs:
        if seg[0] == 'L':
            ep_svg = seg[1]
            poly_pts.append(conv_xy(*ep_svg))
            cur_svg = ep_svg
            cur_px  = _conv_pt(cur_svg, scale, minx, miny)
        else:  # 'C'
            _, c1_svg, c2_svg, ep_svg = seg
            c1_px = _conv_pt(c1_svg, scale, minx, miny)
            c2_px = _conv_pt(c2_svg, scale, minx, miny)
            ep_px = _conv_pt(ep_svg, scale, minx, miny)
            sub = _tessellate_cubic_px(cur_px, c1_px, c2_px, ep_px, tol2)
            for pt in sub:
                poly_pts.append((round_i(pt[0]), round_i(pt[1])))
            cur_svg = ep_svg
            cur_px  = ep_px

    # ------------------------------------------------------------------
    # Build SPLINE candidate: one segment per original cubic (or
    # degenerate-cubic for straight segments).
    # A degenerate straight line as a cubic: C1 = lerp(P0,P1, 1/3),
    # C2 = lerp(P0,P1, 2/3) — collinear, renders identically to a line.
    # ------------------------------------------------------------------
    spline_segs: List[Tuple[Tuple[int,int], Tuple[int,int], Tuple[int,int]]] = []
    cur_svg = sp.start

    for seg in sp.segs:
        if seg[0] == 'C':
            _, c1_svg, c2_svg, ep_svg = seg
            spline_segs.append((conv_xy(*c1_svg), conv_xy(*c2_svg), conv_xy(*ep_svg)))
            cur_svg = ep_svg
        else:
            # Straight segment → collinear degenerate cubic
            ep_svg = seg[1]
            p0i = conv_xy(*cur_svg)
            p1i = conv_xy(*ep_svg)
            c1i = (round_i(p0i[0] + (p1i[0]-p0i[0])/3.0),
                   round_i(p0i[1] + (p1i[1]-p0i[1])/3.0))
            c2i = (round_i(p0i[0] + 2*(p1i[0]-p0i[0])/3.0),
                   round_i(p0i[1] + 2*(p1i[1]-p0i[1])/3.0))
            spline_segs.append((c1i, c2i, p1i))
            cur_svg = ep_svg

    # ------------------------------------------------------------------
    # Pick the smaller encoding
    # ------------------------------------------------------------------
    n_poly  = min(len(poly_pts), max_points)
    n_splin = len(spline_segs)

    # OP_POLY:   opcode(1) + n(1) + flags(1) + n*4
    poly_bytes   = 3 + n_poly * 4
    # OP_SPLINE: opcode(1) + n(1) + flags(1) + P0(4) + n*(C1+C2+P1)(12)
    spline_bytes = 3 + 4 + n_splin * 12

    if n_poly < 2:
        return  # degenerate

    if n_splin > 0 and n_splin <= 255 and spline_bytes < poly_bytes:
        emit_spline(out, spline_segs, start_i, sp.closed)
    else:
        emit_poly(out, poly_pts, sp.closed, max_points)

# ---------------------------------------------------------------------------
#  Main compile_svg
# ---------------------------------------------------------------------------

def compile_svg(svg_path: str,
                box_wh: Optional[Tuple[int, int]],
                max_points: int,
                circle_fit: bool,
                tol2: float) -> bytes:
    tree = ET.parse(svg_path)
    root = tree.getroot()
    if strip_ns(root.tag) != "svg":
        raise ValueError("Root is not <svg>")

    minx, miny, vbw, vbh = parse_viewbox(root)
    if vbw <= 0 or vbh <= 0:
        raise ValueError("No usable viewBox/width/height")

    if box_wh:
        box_w, box_h = box_wh
        s = min(box_w / vbw, box_h / vbh)
    else:
        s = 1.0

    out_vbw = max(1, round_i(vbw * s))
    out_vbh = max(1, round_i(vbh * s))

    out = bytearray()
    out.append(OP_VIEWBOX)
    out += pack_u16(out_vbw) + pack_u16(out_vbh)

    current_style_key: Optional[Tuple[int,int,int]] = None
    # SVG default paint: fill=black, no stroke.
    # Then merge the root <svg> element's own presentation attributes
    # (e.g. fill="none" stroke="currentcolor") so icon sets that put their
    # style on the root element work correctly.
    _svg_default = Style(fill="black", stroke="none", stroke_width=1.0, fill_rule="nonzero")
    root_style = merged_style(_svg_default, root)

    def emit_style(flags: int, sw: int, cidx: int) -> None:
        nonlocal current_style_key
        key = (flags & 0xFF, sw & 0xFF, cidx & 0xFF)
        if key != current_style_key:
            out.extend(bytes((OP_STYLE, key[0], key[1], key[2])))
            current_style_key = key

    def ensure_style(style: Style) -> Tuple[int,int,int]:
        key = style_to_key(style, s)  # s = SVG→pixel scale, scales stroke-width
        emit_style(*key)
        return key

    def conv_xy(x: float, y: float) -> Tuple[int, int]:
        return round_i((x - minx) * s), round_i((y - miny) * s)

    def conv_len(v: float) -> int:
        return round_i(v * s)

    def walk(elem: ET.Element, inherited: Style) -> None:
        style = merged_style(inherited, elem)
        tag   = strip_ns(elem.tag)

        if tag in ("g", "svg", "defs", "symbol"):
            for ch in list(elem):
                walk(ch, style)
            return

        if tag == "line":
            ensure_style(style)
            X1, Y1 = conv_xy(parse_number(elem.attrib.get("x1"), 0.0),
                             parse_number(elem.attrib.get("y1"), 0.0))
            X2, Y2 = conv_xy(parse_number(elem.attrib.get("x2"), 0.0),
                             parse_number(elem.attrib.get("y2"), 0.0))
            emit_line(out, X1, Y1, X2, Y2)

        elif tag == "rect":
            ensure_style(style)
            x  = parse_number(elem.attrib.get("x"), 0.0)
            y  = parse_number(elem.attrib.get("y"), 0.0)
            w  = parse_number(elem.attrib.get("width"), 0.0)
            h  = parse_number(elem.attrib.get("height"), 0.0)
            rx = parse_number(elem.attrib.get("rx"), 0.0)
            ry = parse_number(elem.attrib.get("ry"), 0.0)
            if rx <= 0 and ry > 0: rx = ry
            if ry <= 0 and rx > 0: ry = rx
            r  = min(rx, ry) if (rx > 0 and ry > 0) else rx
            X, Y = conv_xy(x, y)
            W_px = max(0, conv_len(w))
            H_px = max(0, conv_len(h))
            R_px = max(0, conv_len(r))
            # Clamp radius to 30% of the shorter side so it scales gracefully
            R_px = min(R_px, (min(W_px, H_px) * 3) // 10)
            emit_rect(out, X, Y, W_px, H_px, R_px)

        elif tag == "circle":
            ensure_style(style)
            CX, CY = conv_xy(parse_number(elem.attrib.get("cx"), 0.0),
                             parse_number(elem.attrib.get("cy"), 0.0))
            R = max(0, conv_len(parse_number(elem.attrib.get("r"), 0.0)))
            emit_circle(out, CX, CY, R)

        elif tag == "ellipse":
            ensure_style(style)
            cx = parse_number(elem.attrib.get("cx"), 0.0)
            cy = parse_number(elem.attrib.get("cy"), 0.0)
            rx = parse_number(elem.attrib.get("rx"), 0.0)
            ry = parse_number(elem.attrib.get("ry"), 0.0)
            if rx <= 0 or ry <= 0:
                return
            if abs(rx - ry) < 1e-6:
                CX, CY = conv_xy(cx, cy)
                emit_circle(out, CX, CY, max(0, conv_len(rx)))
            else:
                # Ellipse → 4 cubic segments (matches arc_to_cubics quality)
                N   = 4
                dt  = math.pi / 2
                alpha = math.sin(dt) * (math.sqrt(4 + 3*math.tan(dt/2)**2) - 1) / 3.0
                segs = []
                for k in range(N):
                    t1 = k * dt
                    t2 = t1 + dt
                    p0 = (cx + rx*math.cos(t1), cy + ry*math.sin(t1))
                    p1 = (cx + rx*math.cos(t2), cy + ry*math.sin(t2))
                    d0 = (-rx*math.sin(t1),  ry*math.cos(t1))
                    d1 = (-rx*math.sin(t2),  ry*math.cos(t2))
                    c1f = (p0[0]+alpha*d0[0], p0[1]+alpha*d0[1])
                    c2f = (p1[0]-alpha*d1[0], p1[1]-alpha*d1[1])
                    segs.append((conv_xy(*c1f), conv_xy(*c2f), conv_xy(*p1)))
                start0 = (cx + rx, cy)
                emit_spline(out, segs, conv_xy(*start0), True)

        elif tag in ("polyline", "polygon"):
            ensure_style(style)
            pts_f = parse_points_str(elem.attrib.get("points", ""))
            ipts  = [conv_xy(x, y) for (x, y) in pts_f]
            emit_poly(out, ipts, tag == "polygon", max_points)

        elif tag == "path":
            base_flags, base_sw, base_cidx = ensure_style(style)

            subpaths = path_d_to_subpaths(elem.attrib.get("d", "") or "")

            # Circle-fit: only for single closed curved single-subpath
            if circle_fit and len(subpaths) == 1 and subpaths[0].closed and subpaths[0].has_curve:
                pts_f = subpaths[0].endpoints()
                fit   = maybe_circle(pts_f)
                if fit:
                    CX, CY = conv_xy(fit[0], fit[1])
                    R = max(0, conv_len(fit[2]))
                    emit_circle(out, CX, CY, R)
                    return

            # Hole detection for even-odd fill-rule compound paths
            fill_rule = (style.fill_rule or "nonzero").strip().lower()
            hole = [False] * len(subpaths)
            closed_idx = [k for k, sp in enumerate(subpaths) if sp.closed and len(sp.segs) >= 2]
            if fill_rule == "evenodd" and (base_flags & STYLE_FILL) and len(closed_idx) >= 2:
                # Build integer-polygon approximations for point-in-poly tests
                def sp_ipts(sp: Subpath) -> List[Tuple[int,int]]:
                    pts = [conv_xy(*sp.start)]
                    for seg in sp.segs:
                        pts.append(conv_xy(*seg[-1]))
                    return pts
                ipolys = {k: sp_ipts(subpaths[k]) for k in closed_idx}
                for ii in closed_idx:
                    test_pt = ipolys[ii][0]
                    cnt = sum(1 for jj in closed_idx if jj != ii
                              and point_in_poly(test_pt, ipolys[jj]))
                    hole[ii] = (cnt % 2) == 1

            for idx, sp in enumerate(subpaths):
                if sp.closed and len(sp.segs) >= 2 and hole[idx]:
                    flipped = 1 if base_cidx == 0 else 0
                    emit_style(STYLE_FILL, 1, flipped)
                    emit_subpath(out, sp, conv_xy, conv_len, s, minx, miny, max_points, tol2)
                else:
                    emit_style(base_flags, base_sw, base_cidx)
                    emit_subpath(out, sp, conv_xy, conv_len, s, minx, miny, max_points, tol2)

            emit_style(base_flags, base_sw, base_cidx)

        # ignore text, image, etc.

    for ch in list(root):
        walk(ch, root_style)

    out.append(OP_END)
    return bytes(out)

# ---------------------------------------------------------------------------
#  CLI
# ---------------------------------------------------------------------------

def parse_box(s: str) -> Tuple[int, int]:
    m = re.match(r'^\s*(\d+)\s*[xX]\s*(\d+)\s*$', s)
    if not m:
        raise ValueError("box must be like 100x100")
    return int(m.group(1)), int(m.group(2))

def main() -> int:
    ap = argparse.ArgumentParser(
        description="Convert SVG folder to a VG_Q1 C header for eink_vg.h")
    ap.add_argument("input_dir",   help="Folder containing .svg files")
    ap.add_argument("output_h",    help="Output header path")
    ap.add_argument("--prefix",    default="", help="Optional C identifier prefix")
    ap.add_argument("--box",       default="", help="Scale-to-fit box WxH, e.g. 256x256")
    ap.add_argument("--max-points", type=int, default=128,
                    help="Max points per polygon (default 128)")
    ap.add_argument("--no-circle-fit", action="store_true",
                    help="Disable circle-fit for path loops")
    ap.add_argument("--tol", type=float, default=1.5,
                    help="Bezier flatness tolerance² in px (default 1.5; "
                         "increase for smaller icons, decrease for large crisp ones)")
    args = ap.parse_args()

    box_wh     = parse_box(args.box) if args.box else None
    circle_fit = not args.no_circle_fit

    svg_files = sorted(
        os.path.join(args.input_dir, f)
        for f in os.listdir(args.input_dir)
        if f.lower().endswith(".svg")
    )
    if not svg_files:
        print(f"No .svg files in {args.input_dir}", file=sys.stderr)
        return 2

    entries = []
    for path in svg_files:
        name = args.prefix + safe_c_ident(path)
        try:
            blob = compile_svg(path, box_wh=box_wh,
                               max_points=args.max_points,
                               circle_fit=circle_fit,
                               tol2=args.tol)
        except Exception as e:
            print(f"[ERROR] {os.path.basename(path)}: {e}", file=sys.stderr)
            return 3
        entries.append((name, path, blob))

    out_lines = [
        "#pragma once",
        "",
        "#include <stdint.h>",
        "#include <stddef.h>",
        "",
        "// Generated VG_Q1 assets  (eink_vg.h format)",
        "//enum {",
        f"//  OP_END={OP_END}, OP_VIEWBOX={OP_VIEWBOX}, OP_STYLE={OP_STYLE},",
        f"//  OP_LINE={OP_LINE}, OP_RECT={OP_RECT}, OP_CIRCLE={OP_CIRCLE},",
        f"//  OP_POLY={OP_POLY}, OP_SPLINE={OP_SPLINE},",
        "//};",
        "",
    ]

    total = 0
    for name, path, blob in entries:
        total += len(blob)
        out_lines.append(f"// Source: {os.path.basename(path)}  ({len(blob)} bytes)")
        out_lines.append(f"static const uint8_t {name}_vg[] = {{")
        out_lines.append(bytes_to_c_init(blob))
        out_lines.append("};")
        out_lines.append(f"static const size_t {name}_len = sizeof({name}_vg);")
        out_lines.append("")

    out_lines += [
        "typedef struct { const char* name; const uint8_t* data; size_t len; } vg_asset_t;",
        "static const vg_asset_t vg_assets[] = {",
    ]
    for name, _, _ in entries:
        out_lines.append(f'  {{"{name}", {name}_vg, {name}_len}},')
    out_lines += [
        "};",
        f"static const size_t vg_assets_count = sizeof(vg_assets) / sizeof(vg_assets[0]);",
        "",
        f"// Total VG data: {total} bytes across {len(entries)} asset(s).",
        "",
    ]

    os.makedirs(os.path.dirname(os.path.abspath(args.output_h)), exist_ok=True)
    with open(args.output_h, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(out_lines))

    print(f"Wrote {args.output_h} with {len(entries)} assets ({total} bytes total).")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())