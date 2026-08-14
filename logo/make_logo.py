#!/usr/bin/env python3
"""
aim1k logo generator -- two overlapping rounded squares, as pure B/W vector art
for PCB silkscreen.

Emits DXF (R12, lines+arcs), SVG, and drop-in KiCad 10 footprints.

Geometry is defined in y-up math coordinates centred on the origin, then
flipped for SVG and KiCad (both y-down). DXF is y-up, so it is used as-is.

    A = back square,  offset up-left
    B = front square, offset down-right
    intersection = 7x7 lens, sharp on the BL/TR diagonal, rounded on TL/BR

Usage:  python make_logo.py [--size 13] [--stroke 0.15] [--layer F.SilkS]
"""
import argparse
import math
import uuid
from pathlib import Path

HERE = Path(__file__).parent


# --------------------------------------------------------------------------
# geometry primitives (y-up, centred on origin)
# --------------------------------------------------------------------------
def arc_point(cx, cy, r, deg):
    a = math.radians(deg)
    return (cx + r * math.cos(a), cy + r * math.sin(a))


def rrect(x0, y0, x1, y1, r):
    """Rounded rectangle as a CCW list of ('line', p0, p1) / ('arc', c, r, a0, a1)."""
    return [
        ("line", (x0 + r, y0), (x1 - r, y0)),
        ("arc", (x1 - r, y0 + r), r, 270, 360),
        ("line", (x1, y0 + r), (x1, y1 - r)),
        ("arc", (x1 - r, y1 - r), r, 0, 90),
        ("line", (x1 - r, y1), (x0 + r, y1)),
        ("arc", (x0 + r, y1 - r), r, 90, 180),
        ("line", (x0, y1 - r), (x0, y0 + r)),
        ("arc", (x0 + r, y0 + r), r, 180, 270),
    ]


def lens(h, r):
    """Intersection of the two squares: half-extent h, rounded at TL and BR only."""
    return [
        ("line", (-h, -h), (h - r, -h)),
        ("arc", (h - r, -h + r), r, 270, 360),          # A's bottom-right corner
        ("line", (h, -h + r), (h, h)),
        ("line", (h, h), (-h + r, h)),
        ("arc", (-h + r, h - r), r, 90, 180),           # B's top-left corner
        ("line", (-h, h - r), (-h, -h)),
    ]


def tessellate(segs, steps=24):
    """Flatten to a closed point list (for filled polygons)."""
    pts = []
    for s in segs:
        if s[0] == "line":
            pts.append(s[1])
        else:
            _, c, r, a0, a1 = s
            for i in range(steps):
                pts.append(arc_point(c[0], c[1], r, a0 + (a1 - a0) * i / steps))
    return pts


# --------------------------------------------------------------------------
# solid / XOR region, split into two hole-free L-shaped contours
#
# XOR = (A u B) \ (A n B).  That region has a hole, which neither a DXF fill
# nor a KiCad fp_poly can express directly.  But A\B and B\A are each a plain
# L with no hole, they only touch at two points, and together they are exactly
# the XOR region -- so two simple contours cover it with no fill-rule tricks.
# --------------------------------------------------------------------------
B90 = math.tan(math.pi / 8)          # bulge for a 90 deg arc = tan(theta/4)


def l_shape(h, S, d, r):
    """A minus B, as (point, bulge) pairs. Bulge sits on the vertex that starts it."""
    x0a, y0a, x1a, y1a = -h, -h + d, -h + S, -h + d + S       # back square
    x0b, y1b = -h + d, -h + S                                  # front square
    return [
        ((x0a + r, y0a), 0.0),
        ((x0b, y0a), 0.0),
        ((x0b, y1b - r), -B90),      # concave: front square's top-left fillet
        ((x0b + r, y1b), 0.0),
        ((x1a, y1b), 0.0),
        ((x1a, y1a - r), B90),
        ((x1a - r, y1a), 0.0),
        ((x0a + r, y1a), B90),
        ((x0a, y1a - r), 0.0),
        ((x0a, y0a + r), B90),
    ]


def reflect(verts):
    """B minus A is A minus B rotated 180 deg about the origin; bulges keep sign."""
    return [((-p[0], -p[1]), b) for p, b in verts]


def bulge_pts(p1, p2, b, steps=24):
    """Expand one bulged segment into points (excluding the end point)."""
    if abs(b) < 1e-12:
        return [p1]
    th = 4 * math.atan(b)
    dx, dy = p2[0] - p1[0], p2[1] - p1[1]
    c = math.hypot(dx, dy)
    half = c / 2
    ux, uy = dx / c, dy / c
    cx = (p1[0] + p2[0]) / 2 + (-uy) * (half / math.tan(th / 2))
    cy = (p1[1] + p2[1]) / 2 + (ux) * (half / math.tan(th / 2))
    rad = abs(half / math.sin(th / 2))
    a0 = math.atan2(p1[1] - cy, p1[0] - cx)
    return [(cx + rad * math.cos(a0 + th * i / steps),
             cy + rad * math.sin(a0 + th * i / steps)) for i in range(steps)]


def poly_pts(verts, steps=24):
    out = []
    for i, (p, b) in enumerate(verts):
        out += bulge_pts(p, verts[(i + 1) % len(verts)][0], b, steps)
    return out


def area(pts):
    a = 0.0
    for i, (x, y) in enumerate(pts):
        x2, y2 = pts[(i + 1) % len(pts)]
        a += x * y2 - x2 * y
    return a / 2


def dxf_polys(polys, layer="SILKSCREEN", color=7):
    """Closed R12 POLYLINEs with bulges -- fillable, and readable by everything."""
    o = ["0", "SECTION", "2", "HEADER",
         "9", "$ACADVER", "1", "AC1009",
         "9", "$INSUNITS", "70", "4",
         "0", "ENDSEC",
         "0", "SECTION", "2", "ENTITIES"]
    for verts in polys:
        o += ["0", "POLYLINE", "8", layer, "62", str(color),
              "66", "1", "70", "1",
              "10", "0.0", "20", "0.0", "30", "0.0"]
        for (x, y), b in verts:
            o += ["0", "VERTEX", "8", layer,
                  "10", f"{x:.6f}", "20", f"{y:.6f}", "30", "0.0"]
            if abs(b) > 1e-12:
                o += ["42", f"{b:.8f}"]
        o += ["0", "SEQEND", "8", layer]
    o += ["0", "ENDSEC", "0", "EOF"]
    return "\n".join(o) + "\n"


# --------------------------------------------------------------------------
# DXF  (R12 ASCII, LINE + ARC only -- maximum importer compatibility)
# --------------------------------------------------------------------------
def dxf(shapes, layer="SILKSCREEN"):
    o = ["0", "SECTION", "2", "HEADER",
         "9", "$ACADVER", "1", "AC1009",
         "9", "$INSUNITS", "70", "4",          # 4 = millimetres
         "0", "ENDSEC",
         "0", "SECTION", "2", "ENTITIES"]
    for segs in shapes:
        for s in segs:
            if s[0] == "line":
                (x1, y1), (x2, y2) = s[1], s[2]
                o += ["0", "LINE", "8", layer,
                      "10", f"{x1:.6f}", "20", f"{y1:.6f}", "30", "0.0",
                      "11", f"{x2:.6f}", "21", f"{y2:.6f}", "31", "0.0"]
            else:
                _, (cx, cy), r, a0, a1 = s
                o += ["0", "ARC", "8", layer,
                      "10", f"{cx:.6f}", "20", f"{cy:.6f}", "30", "0.0",
                      "40", f"{r:.6f}",
                      "50", f"{a0:.6f}", "51", f"{a1:.6f}"]
    o += ["0", "ENDSEC", "0", "EOF"]
    return "\n".join(o) + "\n"


# --------------------------------------------------------------------------
# SVG  (y-down: flip y)
# --------------------------------------------------------------------------
def svg_path(segs):
    d = []
    start = segs[0][1]
    d.append(f"M {start[0]:.4f} {-start[1]:.4f}")
    for s in segs:
        if s[0] == "line":
            d.append(f"L {s[2][0]:.4f} {-s[2][1]:.4f}")
        else:
            _, c, r, a0, a1 = s
            end = arc_point(c[0], c[1], r, a1)
            # CCW in y-up renders as CW after the y-flip -> sweep flag 0
            d.append(f"A {r:.4f} {r:.4f} 0 0 0 {end[0]:.4f} {-end[1]:.4f}")
    d.append("Z")
    return " ".join(d)


def svg(shapes, w, stroke, fill_rule=None, filled=None, pad=1.0):
    span = w + 2 * pad
    half = span / 2
    body = []
    if fill_rule:                                   # single evenodd path -> XOR
        d = " ".join(svg_path(s) for s in shapes)
        body.append(f'<path d="{d}" fill="#000" fill-rule="evenodd"/>')
    else:
        for s in shapes:
            body.append(f'<path d="{svg_path(s)}" fill="none" stroke="#000" '
                        f'stroke-width="{stroke}" stroke-linejoin="round"/>')
        for s in (filled or []):
            body.append(f'<path d="{svg_path(s)}" fill="#000"/>')
    inner = "\n    ".join(body)
    return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{span}mm" height="{span}mm" '
            f'viewBox="{-half} {-half} {span} {span}">\n'
            f'  <g>\n    {inner}\n  </g>\n</svg>\n')


# --------------------------------------------------------------------------
# KiCad footprint  (y-down: flip y)
# --------------------------------------------------------------------------
def kicad_mod(name, shapes, stroke, layer, filled=None, polys=None):
    def uid():
        return str(uuid.uuid4())

    o = [f'(footprint "{name}"',
         '\t(version 20260206)',
         '\t(generator "pcbnew")',
         '\t(generator_version "10.0")',
         '\t(layer "F.Cu")',
         '\t(descr "aim1k logo, silkscreen")',
         '\t(tags "logo aim1k")',
         '\t(attr board_only exclude_from_pos_files exclude_from_bom)']
    for prop, val, lay, at in (("Reference", "REF**", "F.SilkS", "0 0 0"),
                               ("Value", name, "F.Fab", "0 0 0")):
        o += [f'\t(property "{prop}" "{val}"',
              f'\t\t(at {at})', f'\t\t(layer "{lay}")', '\t\t(hide yes)',
              f'\t\t(uuid "{uid()}")',
              '\t\t(effects\n\t\t\t(font\n\t\t\t\t(size 1 1)\n\t\t\t\t(thickness 0.15)\n\t\t\t)\n\t\t)',
              '\t)']
    for segs in shapes:
        for s in segs:
            if s[0] == "line":
                (x1, y1), (x2, y2) = s[1], s[2]
                o += ['\t(fp_line',
                      f'\t\t(start {x1:.6f} {-y1:.6f})',
                      f'\t\t(end {x2:.6f} {-y2:.6f})',
                      f'\t\t(stroke\n\t\t\t(width {stroke})\n\t\t\t(type solid)\n\t\t)',
                      f'\t\t(layer "{layer}")', f'\t\t(uuid "{uid()}")', '\t)']
            else:
                _, c, r, a0, a1 = s
                p0 = arc_point(c[0], c[1], r, a0)
                pm = arc_point(c[0], c[1], r, (a0 + a1) / 2)
                p1 = arc_point(c[0], c[1], r, a1)
                o += ['\t(fp_arc',
                      f'\t\t(start {p0[0]:.6f} {-p0[1]:.6f})',
                      f'\t\t(mid {pm[0]:.6f} {-pm[1]:.6f})',
                      f'\t\t(end {p1[0]:.6f} {-p1[1]:.6f})',
                      f'\t\t(stroke\n\t\t\t(width {stroke})\n\t\t\t(type solid)\n\t\t)',
                      f'\t\t(layer "{layer}")', f'\t\t(uuid "{uid()}")', '\t)']
    for pts in [tessellate(s) for s in (filled or [])] + list(polys or []):
        rows = []
        for i in range(0, len(pts), 5):
            rows.append("\t\t\t" + " ".join(f"(xy {x:.6f} {-y:.6f})" for x, y in pts[i:i + 5]))
        o += ['\t(fp_poly', '\t\t(pts', "\n".join(rows), '\t\t)',
              '\t\t(stroke\n\t\t\t(width 0.01)\n\t\t\t(type solid)\n\t\t)',
              '\t\t(fill yes)',
              f'\t\t(layer "{layer}")', f'\t\t(uuid "{uid()}")', '\t)']
    o.append(')')
    return "\n".join(o) + "\n"


# --------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--size", type=float, default=13.0, help="overall bbox, mm")
    ap.add_argument("--stroke", type=float, default=0.15, help="silk line width, mm")
    ap.add_argument("--layer", default="F.SilkS")
    a = ap.parse_args()

    W = a.size
    S = W / 1.3           # square side
    d = 0.30 * S          # diagonal offset
    r = 0.095 * S         # corner radius
    h = W / 2

    assert d > r and (S - d) > 2 * r, "offset/radius ratios out of range"

    sq_a = rrect(-h, -h + d, -h + S, -h + d + S, r)      # back, up-left
    sq_b = rrect(-h + d, -h, -h + d + S, -h + S, r)      # front, down-right
    ln = lens((S - d) / 2, r)
    both = [sq_a, sq_b]

    la = l_shape(h, S, d, r)                 # back square minus overlap
    lb = reflect(la)                         # front square minus overlap
    pa, pb = poly_pts(la), poly_pts(lb)

    # the two L contours must together equal (A u B) minus (A n B)
    sq_area = S * S - (4 - math.pi) * r * r
    lens_area = (S - d) ** 2 - 2 * (1 - math.pi / 4) * r * r
    want = sq_area - lens_area
    for nm, p in (("A\\B", pa), ("B\\A", pb)):
        got = area(p)
        assert got > 0, f"{nm} wound clockwise"
        assert abs(got - want) < 0.02, f"{nm} area {got:.4f} != {want:.4f}"
    print(f"solid/xor: 2 contours, {want:.3f} mm^2 each, {2 * want:.3f} mm^2 of ink")

    out = {
        "aim1k-logo-outline.dxf":      dxf(both),
        "aim1k-logo-lens.dxf":         dxf([ln]),
        "aim1k-logo-solid.dxf":        dxf_polys([la, lb]),
        "aim1k-logo-outline.svg":      svg(both, W, a.stroke),
        "aim1k-logo-lens.svg":         svg(both, W, a.stroke, filled=[ln]),
        "aim1k-logo-solid.svg":        svg(both, W, a.stroke, fill_rule="evenodd"),
    }
    lib = HERE / "aim1k.pretty"
    lib.mkdir(exist_ok=True)
    (lib / "aim1k_logo_outline.kicad_mod").write_text(
        kicad_mod("aim1k_logo_outline", both, a.stroke, a.layer), encoding="utf-8")
    (lib / "aim1k_logo_lens.kicad_mod").write_text(
        kicad_mod("aim1k_logo_lens", both, a.stroke, a.layer, filled=[ln]), encoding="utf-8")
    (lib / "aim1k_logo_solid.kicad_mod").write_text(
        kicad_mod("aim1k_logo_solid", [], a.stroke, a.layer, polys=[pa, pb]), encoding="utf-8")

    for name, text in out.items():
        (HERE / name).write_text(text, encoding="utf-8")

    print(f"size {W}mm  square {S:.3f}  offset {d:.3f}  radius {r:.3f}  stroke {a.stroke}")
    for n in list(out) + ["aim1k.pretty/aim1k_logo_outline.kicad_mod",
                          "aim1k.pretty/aim1k_logo_lens.kicad_mod"]:
        print("  wrote", n)


if __name__ == "__main__":
    main()
