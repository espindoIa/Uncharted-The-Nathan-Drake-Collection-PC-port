"""Draws the launcher's logo fonts and writes them as TTF.

The three logos share the word UNCHARTED (an ultra-heavy grotesque, close to the free Base 02) and
differ in the subtitle: U1 hand-lettered caps on tape, U2 and U3 classical serif small caps. So there
is one heavy face for the wordmark and headings ("logo") and one subtitle face per game.

Each glyph is a set of centre-line strokes thickened by a pen that is thicker in x than in y (the
stroke contrast of the lettering), clipped to the cap box; the serif faces get bracketed serifs, the
hand face a round pen. The shapes are drawn from scratch for this project: not a copy of Base 02 or
of any other font.

  sheet   writes <out>/<style>-sheet.svg: every glyph on a 1000 pt grid, for VectorCraft (roughen the
          edges there, export the SVG back as <style>-worn.svg)
  build   writes launcher/assets/fonts/<style>.ttf from the clean glyphs, or from <style>-worn.svg
  preview renders a specimen PNG of the built fonts

Needs Windows Python 3.13 with fonttools, shapely and pillow.
"""
import argparse
import math
import re
import sys
from pathlib import Path

from shapely import affinity
from shapely.geometry import LineString, LinearRing, MultiPolygon, Polygon, box
from shapely.geometry.polygon import orient
from shapely.ops import unary_union

ROOT = Path(__file__).resolve().parent.parent
FONT_DIR = ROOT / "launcher" / "assets" / "fonts"

# kind (sans, serif, hand), cap height, vertical stem (tv), horizontal bar (th), base letter width
# (W), roundness exponent of the superellipse (2 = ellipse, higher = squarer), side bearing, wear
# (speck density) and speck size. Measured on the logos (25 Oct 2026, Wikimedia/Logopedia copies):
# the UNCHARTED stems are about 0.26 of the cap height, the bars 0.22, the letters almost touch.
STYLES = {
    "logo": dict(family="Uncharted Logo", kind="sans", H=700, tv=184, th=152, W=620, n=2.15, sb=16,
                 wear=1.0, speck=1.6),
    # U1 subtitle: monoline marker capitals, slightly slanted.
    "u1": dict(family="Uncharted Fortune", kind="hand", H=700, tv=82, th=82, W=470, n=2.0, sb=34,
               wear=0, speck=1),
    # U2 subtitle: light, wide-spaced Roman capitals.
    "u2": dict(family="Uncharted Thieves", kind="serif", H=700, tv=92, th=34, W=600, n=2.0, sb=64,
               wear=0.15, speck=.6),
    # U3 subtitle: the same Roman capitals, heavier and tighter.
    "u3": dict(family="Uncharted Deception", kind="serif", H=700, tv=126, th=46, W=590, n=2.0, sb=40,
               wear=0.2, speck=.7),
}
SMALL_CAPS = .78  # serif and hand faces: lowercase is drawn as small capitals, as on the logos

# Width of each letter as a multiple of W.
WIDTH = dict(A=1.06, B=1.04, C=1.0, D=1.02, E=.86, F=.82, G=1.06, H=1.0, J=.8, K=1.02, L=.8, M=1.3,
             N=1.02, O=1.1, P=1.02, Q=1.1, R=1.04, S=.96, T=.96, U=1.02, V=1.06, W=1.6, X=1.04, Y=1.04,
             Z=.96)
DIGIT_WIDTH = .98
LETTERS = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
DIGITS = "0123456789"
PUNCT = ".,:;!?'-/+’–•"


class Face:
    def __init__(self, key):
        p = STYLES[key]
        self.key, self.family, self.kind = key, p["family"], p["kind"]
        self.H, self.tv, self.th, self.W, self.n, self.sb, self.wear, self.speck = (
            p["H"], p["tv"], p["th"], p["W"], p["n"], p["sb"], p["wear"], p["speck"])
        self.hx, self.hy, self.k = self.tv / 2, self.th / 2, self.tv / self.th
        self.xf = self.hx * 1.07  # centre of a diagonal's foot, so its outer edge lands on the box
        self.round = self.kind == "hand"  # round pen: round ends and joins, nothing clipped

    # -- strokes -----------------------------------------------------------------------------

    def stroke(self, pts, ext=0.0, closed=False):
        pts = [tuple(p) for p in pts]
        if ext and not closed:
            (x0, y0), (x1, y1) = pts[0], pts[1]
            d = math.hypot(x1 - x0, y1 - y0)
            pts[0] = (x0 - (x1 - x0) / d * ext, y0 - (y1 - y0) / d * ext)
            (x0, y0), (x1, y1) = pts[-2], pts[-1]
            d = math.hypot(x1 - x0, y1 - y0)
            pts[-1] = (x1 + (x1 - x0) / d * ext, y1 + (y1 - y0) / d * ext)
        scaled = [(x / self.k, y) for x, y in pts]
        line = LinearRing(scaled) if closed else LineString(scaled)
        if self.round:
            shape = line.buffer(self.th / 2, cap_style=1, join_style=1, quad_segs=6)
        else:
            shape = line.buffer(self.th / 2, cap_style=2, join_style=2, mitre_limit=8)
        return affinity.scale(shape, xfact=self.k, yfact=1, origin=(0, 0))

    def arc(self, cx, cy, rx, ry, a0, a1):
        steps = max(10, int(abs(a1 - a0) / 4))
        e = 2 / self.n
        out = []
        for i in range(steps + 1):
            a = math.radians(a0 + (a1 - a0) * i / steps)
            c, s = math.cos(a), math.sin(a)
            out.append((cx + rx * math.copysign(abs(c) ** e, c), cy + ry * math.copysign(abs(s) ** e, s)))
        return out

    # -- glyphs ------------------------------------------------------------------------------

    def advance_width(self, ch):
        if ch == "I":
            return self.tv
        if ch in DIGITS:
            return {"1": .62}.get(ch, DIGIT_WIDTH) * self.W
        return WIDTH[ch] * self.W

    def glyph(self, ch):
        """Returns (geometry, width) for one character, in font units, y up, baseline at 0."""
        H, tv, th, hx, hy, xf = self.H, self.tv, self.th, self.hx, self.hy, self.xf
        w = self.advance_width(ch)
        parts, cut = [], []
        low = 0
        st = lambda pts, ext=0.0: parts.append(self.stroke(pts, ext))
        V = lambda x, y0, y1: st([(x, y0), (x, y1)])
        Hb = lambda y, x0, x1: st([(x0, y), (x1, y)])
        ring = lambda cx, cy, rx, ry: parts.append(self.stroke(self.arc(cx, cy, rx, ry, 0, 360)[:-1], closed=True))
        diag_ext = 0 if self.round else th * 3

        def bowl(y0, y1, xr, x0=hx):
            """Stem-side bowl open to the left: top line, right arc, bottom line (B, D, P, R)."""
            ry = (y1 - y0) / 2
            rx = min(ry * .95, xr - x0)
            cx = xr - rx
            return [(x0, y1), (cx, y1)] + self.arc(cx, y0 + ry, rx, ry, 90, -90)[0:] + [(x0, y0)]

        if ch == "A":
            st([(xf, 0), (w / 2, H), (w - xf, 0)], diag_ext)
            Hb(.27 * H, .2 * w, .8 * w)
        elif ch == "B":
            V(hx, 0, H)
            mid = .53 * H
            st(bowl(mid - hy, H - hy, w * .9))
            st(bowl(hy, mid + hy, w))
        elif ch in "CG":
            ring(w / 2, H / 2, w / 2 - hx, H / 2 - hy)
            if ch == "C":
                cut.append(box(w * .5, .31 * H, w + 5, .69 * H))
            else:
                cut.append(box(w * .5, .5 * H, w + 5, .72 * H))
                Hb(.5 * H - hy, w * .5, w)
        elif ch == "D":
            V(hx, 0, H)
            ry = (H - th) / 2
            rx = w * .56
            cx = w - hx - rx
            st([(hx, H - hy), (cx, H - hy)] + self.arc(cx, H / 2, rx, ry, 90, -90) + [(hx, hy)])
        elif ch == "E":
            V(hx, 0, H)
            Hb(hy, 0, w)
            Hb(H - hy, 0, w)
            Hb(H / 2, 0, w * .9)
        elif ch == "F":
            V(hx, 0, H)
            Hb(H - hy, 0, w)
            Hb(.5 * H, 0, w * .88)
        elif ch == "H":
            V(hx, 0, H)
            V(w - hx, 0, H)
            Hb(H / 2, 0, w)
        elif ch == "I":
            V(hx, 0, H)
        elif ch == "J":
            rr = H * .27
            ry = rr - hy
            rx = (w - tv) / 2
            st([(w - hx, H), (w - hx, rr)] + self.arc(w / 2, rr, w / 2 - hx, ry, 0, -180)[1:] + [(hx, rr + hy)])
        elif ch == "K":
            V(hx, 0, H)
            st([(w, H), (tv * .8, .42 * H)], diag_ext)
            st([(tv * .6, .5 * H), (w - xf * .6, 0)], diag_ext)
        elif ch == "L":
            V(hx, 0, H)
            Hb(hy, 0, w)
        elif ch == "M":
            V(hx, 0, H)
            V(w - hx, 0, H)
            st([(hx, H), (w / 2, .2 * H), (w - hx, H)], diag_ext)
        elif ch == "N":
            V(hx, 0, H)
            V(w - hx, 0, H)
            st([(hx, H), (w - hx, 0)], diag_ext)
        elif ch in "OQ":
            ring(w / 2, H / 2, w / 2 - hx, H / 2 - hy)
            if ch == "Q":
                low = -.09 * H
                st([(w * .56, .3 * H), (w * 1.0, low)], 0)
        elif ch == "P":
            V(hx, 0, H)
            st(bowl(.42 * H, H - hy, w))
        elif ch == "R":
            V(hx, 0, H)
            st(bowl(.44 * H, H - hy, w))
            st([(w * .4, .44 * H), (w - xf * .6, 0)], diag_ext)
        elif ch == "S":
            parts.append(self.stroke(self.s_curve(w), 0))
        elif ch == "T":
            Hb(H - hy, 0, w)
            V(w / 2, 0, H)
        elif ch == "U":
            rr = H * .3
            st([(hx, H), (hx, rr)] + self.arc(w / 2, rr, w / 2 - hx, rr - hy, 180, 360)[1:] + [(w - hx, H)])
        elif ch == "V":
            st([(xf, H), (w / 2, 0), (w - xf, H)], diag_ext)
        elif ch == "W":
            st([(xf, H), (w * .26, 0), (w / 2, .78 * H), (w * .74, 0), (w - xf, H)], diag_ext)
        elif ch == "X":
            st([(xf, H), (w - xf, 0)], diag_ext)
            st([(xf, 0), (w - xf, H)], diag_ext)
        elif ch == "Y":
            st([(xf, H), (w / 2, .45 * H), (w - xf, H)], diag_ext)
            V(w / 2, 0, .5 * H)
        elif ch == "Z":
            Hb(H - hy, 0, w)
            Hb(hy, 0, w)
            st([(w - hx, H), (hx, 0)], diag_ext)
        elif ch in DIGITS:
            self.digit(ch, w, st, V, Hb, ring, parts, diag_ext)
        else:
            low = self.punct(ch, w, parts, cut, st, V, Hb)
        if self.kind == "serif":
            parts += self.serifs(ch, w)
        geom = unary_union(parts)
        if cut:
            geom = geom.difference(unary_union(cut))
        if self.round:
            return affinity.skew(geom, xs=7, origin=(0, 0)), w
        pad = self.tv * 1.3 if self.kind == "serif" else 0  # serifs reach past the letter's box
        return geom.intersection(box(-pad, min(low, 0), w + pad, H)), w

    def serifs(self, ch, w):
        """Bracketed slab serifs at the stem ends ('b' bottom, 't' top) and upright ones at the
        ends of the arms ('d' hanging from a top arm, 'u' standing on a bottom arm)."""
        H, tv, th, hx, hy, xf = self.H, self.tv, self.th, self.hx, self.hy, self.xf
        spec = {
            "A": "b:xf b:w-xf", "B": "b:hx t:hx", "D": "b:hx t:hx", "E": "b:hx t:hx d:w u:w",
            "F": "b:hx t:hx d:w", "H": "b:hx t:hx b:w-hx t:w-hx", "I": "b:hx t:hx", "J": "t:w-hx",
            "K": "b:hx t:hx t:w-xf b:w-xf*.6", "L": "b:hx t:hx u:w", "M": "b:hx b:w-hx t:hx t:w-hx",
            "N": "b:hx t:hx b:w-hx t:w-hx", "P": "b:hx t:hx", "R": "b:hx t:hx b:w-xf*.6",
            "T": "b:w/2 d:0 d:w", "U": "t:hx t:w-hx", "V": "t:xf t:w-xf", "W": "t:xf t:w-xf",
            "X": "t:xf t:w-xf b:xf b:w-xf", "Y": "t:xf t:w-xf b:w/2", "Z": "d:0 u:w", "1": "b:w-hx",
        }.get(ch, "")
        sw, s, br = tv * 2.4, th * 1.05, tv * .45
        out = []
        for item in spec.split():
            side, expr = item.split(":")
            x = eval(expr, {}, dict(w=w, hx=hx, xf=xf))
            if side in "bt":
                # slab, then a concave bracket into the stem on each side
                pts = [(x - sw / 2, 0), (x + sw / 2, 0), (x + sw / 2, s)]
                pts += [(x + hx + br * (1 - math.sin(a)), s + br * (1 - math.cos(a)))
                        for a in [math.pi / 2 * i / 6 for i in range(7)]]
                pts += [(x - hx - br * (1 - math.sin(a)), s + br * (1 - math.cos(a)))
                        for a in [math.pi / 2 * i / 6 for i in range(6, -1, -1)]]
                pts += [(x - sw / 2, s)]
                poly = Polygon(pts).buffer(0)
                out.append(poly if side == "b" else affinity.scale(poly, 1, -1, origin=(0, H / 2)))
            else:
                x0, x1 = (x - th * 1.2, x) if x > 0 else (0, th * 1.2)
                out.append(box(x0, H - th * 2.6, x1, H) if side == "d" else box(x0, 0, x1, th * 2.6))
        return out

    def s_curve(self, w):
        H, hx, hy = self.H, self.hx, self.hy
        ry = (H - 2 * hy) / 4
        rx = w / 2 - hx
        top = self.arc(w / 2, H - hy - ry, rx, ry, 25, 270)
        bot = self.arc(w / 2, hy + ry, rx, ry, 90, -155)
        return top + bot[1:]

    def digit(self, ch, w, st, V, Hb, ring, parts, diag_ext):
        H, tv, th, hx, hy = self.H, self.tv, self.th, self.hx, self.hy
        if ch == "0":
            ring(w / 2, H / 2, w / 2 - hx, H / 2 - hy)
        elif ch == "1":
            V(w - hx, 0, H)
            st([(0, .7 * H), (w - tv, H - hy)])
        elif ch == "2":
            ry = H * .27
            st(self.arc(w / 2, H - hy - ry, w / 2 - hx, ry, 165, -35) + [(hx * 1.3, hy)])
            Hb(hy, 0, w)
        elif ch == "3":
            ry = (H - 2 * hy) / 4
            rx = w / 2 - hx
            st(self.arc(w / 2, H - hy - ry, rx, ry, 150, -90)[0:] + self.arc(w / 2, hy + ry, rx, ry, 90, -150)[1:])
        elif ch == "4":
            xs = w * .7
            V(xs, 0, H)
            st([(xs, H), (hx * .6, .32 * H), (w, .32 * H)], diag_ext * .2)
        elif ch == "5":
            Hb(H - hy, 0, w)
            V(hx, .5 * H, H)
            ry = H * .28
            st([(hx, .52 * H)] + self.arc(w / 2 - hx * .2, hy + ry, w / 2 - hx * .8, ry, 135, -150))
        elif ch == "6":
            ry = H * .29
            ring(w / 2, hy + ry, w / 2 - hx, ry)
            st([(hx, hy + ry), (hx, H * .72)] + self.arc(w / 2, H - hy - H * .2, w / 2 - hx, H * .2, 180, 60)[1:])
        elif ch == "7":
            Hb(H - hy, 0, w)
            st([(w - hx, H), (w * .3, 0)], diag_ext)
        elif ch == "8":
            half = (H - th) / 2  # the two rings share one bar: r1 + r2 = (H - th) / 2
            r1 = half * .45
            r2 = half - r1
            ring(w / 2, H - hy - r1, w / 2 - hx * 1.15, r1)
            ring(w / 2, hy + r2, w / 2 - hx, r2)
        elif ch == "9":
            ry = H * .29
            sub = Face.__new__(Face)
            sub.__dict__.update(self.__dict__)
            parts6 = []
            sub.digit("6", w, lambda pts, ext=0.0: parts6.append(sub.stroke(pts, ext)),
                      lambda x, y0, y1: parts6.append(sub.stroke([(x, y0), (x, y1)])),
                      lambda y, x0, x1: parts6.append(sub.stroke([(x0, y), (x1, y)])),
                      lambda cx, cy, rx, ry_: parts6.append(sub.stroke(sub.arc(cx, cy, rx, ry_, 0, 360)[:-1], closed=True)),
                      parts6, diag_ext)
            parts.append(affinity.rotate(unary_union(parts6), 180, origin=(w / 2, H / 2)))

    def punct(self, ch, w, parts, cut, st, V, Hb):
        H, tv, th, hx, hy = self.H, self.tv, self.th, self.hx, self.hy
        dot = lambda cx, cy: parts.append(box(cx - tv * .5, cy, cx + tv * .5, cy + tv * .95))
        if ch == ".":
            dot(w / 2, 0)
        elif ch == ",":
            dot(w / 2, 0)
            parts.append(Polygon([(w / 2 - tv * .5, tv * .2), (w / 2 + tv * .5, tv * .2), (w / 2 + tv * .05, -tv * .8)]))
            return -tv * .8
        elif ch in ":;":
            dot(w / 2, 0)
            dot(w / 2, H * .5)
            if ch == ";":
                parts.append(Polygon([(w / 2 - tv * .5, tv * .2), (w / 2 + tv * .5, tv * .2), (w / 2 + tv * .05, -tv * .8)]))
                return -tv * .8
        elif ch == "!":
            st([(w / 2 + tv * .0, H), (w / 2, tv * 1.5)])
            dot(w / 2, 0)
        elif ch == "?":
            ry = H * .24
            st(self.arc(w / 2, H - hy - ry, w / 2 - hx, ry, 165, -80) + [(w / 2, H * .3)])
            dot(w / 2, 0)
        elif ch in "'’":
            parts.append(Polygon([(w / 2 - tv * .5, H), (w / 2 + tv * .5, H), (w / 2 + tv * .3, H * .66), (w / 2 - tv * .3, H * .66)]))
        elif ch in "-–":
            Hb(H * .4, 0, w)
        elif ch == "/":
            st([(w - hx, H), (hx, 0)], th * 3)
        elif ch == "+":
            Hb(H * .45, 0, w)
            V(w / 2, H * .45 - w / 2, H * .45 + w / 2)
        elif ch == "•":
            r = tv * .7
            parts.append(Polygon([(w / 2 + r * math.cos(a), H * .42 + r * math.sin(a)) for a in [i * math.pi / 12 for i in range(24)]]))
        return 0

    def punct_width(self, ch):
        return {".": self.tv, ",": self.tv, ":": self.tv, ";": self.tv, "!": self.tv, "?": self.W * .72,
                "'": self.tv, "’": self.tv, "-": self.W * .55, "–": self.W * .7, "/": self.W * .6,
                "+": self.W * .6, "•": self.tv * 1.6}[ch]

    def glyph_set(self):
        out = {}
        for ch in LETTERS + DIGITS:
            out[ch] = self.glyph(ch)
        for ch in PUNCT:
            self_w = self.punct_width(ch)
            saved = self.advance_width
            self.advance_width = lambda c, _w=self_w: _w  # noqa: E731
            try:
                out[ch] = self.glyph(ch)
            finally:
                del self.advance_width
        return out


def wear(geom, face, seed):
    """Eats chips out of a glyph: small specks, a few scratches and nicks on the edges."""
    import random
    rnd = random.Random(seed)
    minx, miny, maxx, maxy = geom.bounds
    area = (maxx - minx) * (maxy - miny)
    cuts = []
    for _ in range(int(area / 9000 * face.wear * 1.6)):
        x, y = rnd.uniform(minx, maxx), rnd.uniform(miny, maxy)
        r = rnd.choice([5, 7, 9, 12, 16]) * face.speck
        n = rnd.randint(5, 8)
        cuts.append(Polygon([(x + r * rnd.uniform(.6, 1.4) * math.cos(a), y + r * rnd.uniform(.6, 1.4) * math.sin(a))
                             for a in [2 * math.pi * i / n for i in range(n)]]))
    for _ in range(int(3 * face.wear)):
        x, y = rnd.uniform(minx, maxx), rnd.uniform(miny, maxy)
        a = rnd.uniform(-.5, .5)
        ln = rnd.uniform(.2, .5) * (maxx - minx)
        cuts.append(LineString([(x, y), (x + ln * math.cos(a), y + ln * math.sin(a))]).buffer(rnd.uniform(1.5, 3.5), cap_style=2))
    return geom.difference(unary_union(cuts)) if cuts else geom


# -- output ------------------------------------------------------------------------------------

CELL = 1000


def polygons(geom):
    if geom.is_empty:
        return []
    return list(geom.geoms) if isinstance(geom, MultiPolygon) or geom.geom_type == "GeometryCollection" else [geom]


def svg_path(geom, dx, dy):
    """Path data of a geometry placed at (dx, dy) of the sheet (SVG y runs down)."""
    d = []
    for poly in polygons(geom):
        if poly.geom_type != "Polygon":
            continue
        for ring in [poly.exterior, *poly.interiors]:
            pts = [(dx + x, dy - y) for x, y in ring.coords[:-1]]
            d.append("M" + " L".join(f"{x:.1f} {y:.1f}" for x, y in pts) + " Z")
    return " ".join(d)


def glyph_order():
    return list(LETTERS + DIGITS + PUNCT)


def write_sheet(key, out, worn):
    face = Face(key)
    gs = face.glyph_set()
    order = glyph_order()
    cols = 8
    rows = math.ceil(len(order) / cols)
    paths = []
    for i, ch in enumerate(order):
        geom, w = gs[ch]
        if worn:
            geom = wear(geom, face, f"{key}{ch}")
        x, y = (i % cols) * CELL, (i // cols) * CELL
        paths.append(f'<path id="{key}_{i}" d="{svg_path(geom, x + (CELL - w) / 2, y + CELL * .8)}" fill="#000" fill-rule="evenodd"/>')
    svg = (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {cols * CELL} {rows * CELL}" '
           f'width="{cols * CELL}" height="{rows * CELL}">\n' + "\n".join(paths) + "\n</svg>\n")
    path = Path(out) / f"{key}-sheet.svg"
    path.write_text(svg, encoding="utf-8")
    return path


def parse_worn(svg_file, n_glyphs, cols=8):
    """Reads the roughened sheet back: a glyph is whatever path sits in its grid cell."""
    from fontTools.pens.recordingPen import DecomposingRecordingPen
    from fontTools.svgLib.path import parse_path

    text = Path(svg_file).read_text(encoding="utf-8")
    cells = {}
    for m in re.finditer(r'<path\b[^>]*?\bd="([^"]+)"', text):
        pen = DecomposingRecordingPen({})
        parse_path(m.group(1), pen)
        rings, cur = [], []
        for op, args in pen.value:
            if op == "moveTo":
                cur = [args[0]]
            elif op == "lineTo":
                cur.append(args[0])
            elif op == "curveTo":
                p0 = cur[-1]
                c1, c2, p1 = args
                cur += [tuple((1 - t) ** 3 * a + 3 * (1 - t) ** 2 * t * b + 3 * (1 - t) * t ** 2 * c + t ** 3 * d
                              for a, b, c, d in zip(p0, c1, c2, p1)) for t in (.25, .5, .75, 1)]
            elif op == "qCurveTo":
                p0, c, p1 = cur[-1], args[0], args[-1]
                cur += [tuple((1 - t) ** 2 * a + 2 * (1 - t) * t * b + t ** 2 * d for a, b, d in zip(p0, c, p1))
                        for t in (.33, .66, 1)]
            elif op in ("closePath", "endPath"):
                if len(cur) > 2:
                    rings.append(cur)
                cur = []
        for ring in rings:
            xs = [p[0] for p in ring]
            ys = [p[1] for p in ring]
            cell = int(((min(ys) + max(ys)) / 2) // CELL) * cols + int(((min(xs) + max(xs)) / 2) // CELL)
            cells.setdefault(cell, []).append(ring)
    out = {}
    for cell, rings in cells.items():
        if cell < n_glyphs:
            polys = [Polygon(r).buffer(0) for r in rings]
            # even-odd: a ring inside another is a hole
            geom = polys[0]
            for p in polys[1:]:
                geom = geom.symmetric_difference(p)
            out[cell] = geom
    return out


def build_ttf(key, worn_svg=None):
    from fontTools.fontBuilder import FontBuilder
    from fontTools.pens.ttGlyphPen import TTGlyphPen

    face = Face(key)
    gs = face.glyph_set()
    order = glyph_order()
    worn = parse_worn(worn_svg, len(order)) if worn_svg else {}
    glyphs, adv, cmap = {".notdef": TTGlyphPen(None).glyph()}, {".notdef": 500}, {}
    names = [".notdef"]
    def add(name, geom, w):
        # the outline starts face.sb from the origin (serifs and the slant reach past the box)
        if not geom.is_empty:
            minx, _, maxx, _ = geom.bounds
            geom = affinity.translate(geom, face.sb - minx, 0)
            w = maxx - minx
        pen = TTGlyphPen(None)
        for poly in polygons(geom):
            if poly.geom_type != "Polygon" or poly.area < 30:
                continue
            poly = orient(poly, sign=-1.0)  # TrueType: outer contours clockwise
            for ring in [poly.exterior, *poly.interiors]:
                pts = [(round(x), round(y)) for x, y in ring.coords[:-1]]
                pen.moveTo(pts[0])
                for p in pts[1:]:
                    pen.lineTo(p)
                pen.closePath()
        glyphs[name] = pen.glyph()
        adv[name] = round(w + 2 * face.sb)
        names.append(name)

    small = face.kind != "sans"
    for i, ch in enumerate(order):
        geom, w = gs[ch]
        if i in worn:  # sheet coordinates back to glyph coordinates
            x0, y0 = (i % 8) * CELL + (CELL - w) / 2, (i // 8) * CELL + CELL * .8
            geom = affinity.scale(affinity.translate(worn[i], -x0, -y0), 1, -1, origin=(0, 0))
        geom = geom.simplify(.6)
        name = "uni%04X" % ord(ch)
        add(name, geom, w)
        cmap[ord(ch)] = name
        if ch.isalpha():
            low = "uni%04X" % ord(ch.lower())
            if small:
                add(low, affinity.scale(geom, SMALL_CAPS, SMALL_CAPS, origin=(0, 0)), w * SMALL_CAPS)
                cmap[ord(ch.lower())] = low
            else:
                cmap[ord(ch.lower())] = name
    glyphs["space"] = TTGlyphPen(None).glyph()
    adv["space"] = round(face.W * .38)
    cmap[32] = "space"
    names.append("space")
    fb = FontBuilder(1000, isTTF=True)
    fb.setupGlyphOrder(names)
    fb.setupCharacterMap(cmap)
    fb.setupGlyf(glyphs)
    lsb = {}
    for name, g in glyphs.items():
        g.recalcBounds(fb.font["glyf"])
        lsb[name] = g.xMin if g.numberOfContours else 0
    fb.setupHorizontalMetrics({n: (adv[n], lsb[n]) for n in names})
    fb.setupHorizontalHeader(ascent=800, descent=-200)
    ps = face.family.replace(" ", "") + "-Regular"  # GDI refuses a font without these names
    fb.setupNameTable(dict(familyName=face.family, styleName="Regular", fullName=face.family + " Regular",
                           psName=ps, uniqueFontIdentifier=ps + ";1.0", version="Version 1.000"))
    fb.setupOS2(sTypoAscender=800, sTypoDescender=-200, usWinAscent=900, usWinDescent=260, sxHeight=face.H, sCapHeight=face.H)
    fb.setupPost()
    FONT_DIR.mkdir(parents=True, exist_ok=True)
    path = FONT_DIR / f"{key}.ttf"
    fb.save(str(path))
    return path


def lockups(out):
    """One transparent PNG per game laid out as its logo: UNCHARTED, the number, the subtitle."""
    from PIL import Image, ImageDraw, ImageFont
    games = (("u1", "", "Drake's Fortune", None), ("u2", "2", "Among Thieves", "#c8461c"),
             ("u3", "3", "Drake's Deception", "#e7a224"))
    logo = ImageFont.truetype(str(FONT_DIR / "logo.ttf"), 200)
    sheet = Image.new("RGB", (1440, 3 * 390), "#1a1f24")
    for row, (key, number, subtitle, color) in enumerate(games):
        img = Image.new("RGBA", (1440, 390), (0, 0, 0, 0))
        d = ImageDraw.Draw(img)
        d.text((20, 10), "UNCHARTED", font=logo, fill="#eef1f3")
        right = d.textbbox((20, 10), "UNCHARTED", font=logo)[2]
        if number:
            d.text((right + 14, 0), number, font=ImageFont.truetype(str(FONT_DIR / "logo.ttf"), 330), fill=color)
        sub = ImageFont.truetype(str(FONT_DIR / f"{key}.ttf"), 92)
        x0, y0, x1, y1 = d.textbbox((0, 0), subtitle, font=sub)
        x = right - (x1 - x0)
        if key == "u1":  # U1 writes its subtitle on a strip of yellow tape
            d.rectangle((x - 30, 222 + y0 - 18, right + 24, 222 + y1 + 16), fill="#e8c64a")
        d.text((x - x0, 222), subtitle, font=sub, fill="#2a2418" if key == "u1" else "#e4e8ea")
        sheet.paste(img, (0, row * 390), img)
    path = Path(out) / "lockups.png"
    sheet.save(path)
    return [path]


def preview(out, styles, big=False):
    """big: one style, every glyph large. Otherwise a specimen of all styles."""
    from PIL import Image, ImageDraw, ImageFont
    if big:
        lines, size, step = ["ABCDEFGHI", "JKLMNOPQR", "STUVWXYZ", "0123456789", ".,:;!?'-/+"], 150, 190
    else:
        lines, size, step = ["UNCHARTED", "Drake's Fortune", "Among Thieves", "Drake's Deception", "Display 1 2 3"], 72, 92
    img = Image.new("RGB", (1500, len(styles) * (len(lines) * step + 20)), "#10171d")
    d = ImageDraw.Draw(img)
    y = 10
    for key in styles:
        f = ImageFont.truetype(str(FONT_DIR / f"{key}.ttf"), size)
        for line in lines:
            d.text((20, y), line, font=f, fill="#e9eef2")
            y += step
        y += 20
    path = Path(out) / ("big.png" if big else "specimen.png")
    img.save(path)
    return path


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", choices=["sheet", "build", "preview", "lockups"])
    ap.add_argument("--out", default=".")
    ap.add_argument("--worn", action="store_true", help="sheet: with specks; build: read <style>-worn.svg from --out")
    ap.add_argument("--big", action="store_true", help="preview: every glyph large")
    ap.add_argument("styles", nargs="*", default=list(STYLES))
    a = ap.parse_args()
    for k in a.styles:
        if a.cmd == "sheet":
            print(write_sheet(k, a.out, a.worn))
        elif a.cmd == "build":
            print(build_ttf(k, Path(a.out) / f"{k}-worn.svg" if a.worn else None))
    if a.cmd == "lockups":
        print(*lockups(a.out), sep="\n")
    if a.cmd == "preview":
        print(preview(a.out, a.styles, a.big))
