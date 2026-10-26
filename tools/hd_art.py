"""Helpers of `build_ui_art.py hd` (25 Oct 2026): SVG -> RGBA, mip chain, BC1/BC3/BC4/BC5 encoders
and DDS writer, for the HD UI replacement textures the runtime loads (gow3_pc/gpu/shim/gow3_hdtex.cpp).
Needs numpy and Pillow only. Images are (H, W, 4) uint8 RGBA, rows top to bottom as in the dump PNGs."""
import math
import re
import struct
import xml.etree.ElementTree as ET

import numpy as np
from PIL import Image, ImageDraw

# ---------------------------------------------------------------- SVG

NAMED = {"black": (0, 0, 0), "white": (255, 255, 255), "red": (255, 0, 0), "green": (0, 128, 0),
         "blue": (0, 0, 255), "yellow": (255, 255, 0), "gray": (128, 128, 128), "grey": (128, 128, 128)}
TOKENS = re.compile(r"[MmLlHhVvCcSsQqTtZzAa]|[-+]?(?:\d*\.\d+|\d+\.?)(?:[eE][-+]?\d+)?")
CURVE_STEPS = 24


def parse_color(text):
    text = text.strip().lower()
    if text in NAMED:
        return NAMED[text]
    if m := re.fullmatch(r"#([0-9a-f]{3}|[0-9a-f]{6})", text):
        h = m.group(1)
        h = "".join(c * 2 for c in h) if len(h) == 3 else h
        return tuple(int(h[i:i + 2], 16) for i in (0, 2, 4))
    if m := re.fullmatch(r"rgb\(([^)]*)\)", text):
        return tuple(round(float(v[:-1]) * 2.55) if v.endswith("%") else round(float(v))
                     for v in re.split(r"[\s,]+", m.group(1).strip()))
    return None


def parse_transform(text):
    """A transform attribute as a 3x3 matrix (translate, scale, rotate, matrix; skew is not used by the traces)."""
    m = np.eye(3)
    for name, args in re.findall(r"(\w+)\s*\(([^)]*)\)", text or ""):
        a = [float(v) for v in re.split(r"[\s,]+", args.strip()) if v]
        if name == "translate":
            t = np.array([[1, 0, a[0]], [0, 1, a[1] if len(a) > 1 else 0], [0, 0, 1]])
        elif name == "scale":
            t = np.diag([a[0], a[1] if len(a) > 1 else a[0], 1.0])
        elif name == "matrix":
            t = np.array([[a[0], a[2], a[4]], [a[1], a[3], a[5]], [0, 0, 1]])
        elif name == "rotate":
            c, s = math.cos(math.radians(a[0])), math.sin(math.radians(a[0]))
            t = np.array([[c, -s, 0], [s, c, 0], [0, 0, 1]])
            if len(a) == 3:
                t = parse_transform(f"translate({a[1]},{a[2]})") @ t @ parse_transform(f"translate({-a[1]},{-a[2]})")
        else:
            raise ValueError(f"unsupported SVG transform {name}")
        m = m @ t
    return m


def flatten_path(d):
    """Subpaths (lists of points) of a path's data: M L H V C S Q T Z, absolute and relative. Arcs are not supported."""
    tokens, i, cmd = TOKENS.findall(d), 0, ""
    subs, pts, here, start, prev_c, prev_q = [], [], (0.0, 0.0), (0.0, 0.0), None, None

    def number():
        nonlocal i
        i += 1
        return float(tokens[i - 1])

    def cubic(p0, p1, p2, p3):
        for s in range(1, CURVE_STEPS + 1):
            t, u = s / CURVE_STEPS, 1 - s / CURVE_STEPS
            pts.append(tuple(u**3 * p0[k] + 3 * u * u * t * p1[k] + 3 * u * t * t * p2[k] + t**3 * p3[k] for k in (0, 1)))

    while i < len(tokens):
        if tokens[i].isalpha():
            cmd = tokens[i]
            i += 1
            if cmd in "Zz":
                if len(pts) > 1:
                    subs.append(pts)
                pts, here, prev_c, prev_q = [], start, None, None
                continue
        if cmd in "Aa":
            raise ValueError("SVG arcs are not supported: convert them to curves")
        rel = cmd.islower()
        ox, oy = here if rel else (0.0, 0.0)
        c = cmd.upper()
        if c == "M":
            if len(pts) > 1:
                subs.append(pts)
            here = start = (number() + ox, number() + oy)
            pts, cmd, prev_c, prev_q = [here], ("l" if rel else "L"), None, None
        elif c == "L":
            here = (number() + ox, number() + oy)
            pts.append(here)
            prev_c = prev_q = None
        elif c == "H":
            here = (number() + ox, here[1])
            pts.append(here)
            prev_c = prev_q = None
        elif c == "V":
            here = (here[0], number() + oy)
            pts.append(here)
            prev_c = prev_q = None
        elif c in "CS":
            if c == "C":
                p1 = (number() + ox, number() + oy)
            else:
                p1 = (2 * here[0] - prev_c[0], 2 * here[1] - prev_c[1]) if prev_c else here
            p2 = (number() + ox, number() + oy)
            p3 = (number() + ox, number() + oy)
            cubic(here, p1, p2, p3)
            here, prev_c, prev_q = p3, p2, None
        else:  # Q, T: quadratic, raised to a cubic
            if c == "Q":
                q = (number() + ox, number() + oy)
            else:
                q = (2 * here[0] - prev_q[0], 2 * here[1] - prev_q[1]) if prev_q else here
            p3 = (number() + ox, number() + oy)
            cubic(here, tuple(here[k] + 2 / 3 * (q[k] - here[k]) for k in (0, 1)),
                  tuple(p3[k] + 2 / 3 * (q[k] - p3[k]) for k in (0, 1)), p3)
            here, prev_q, prev_c = p3, q, None
    if len(pts) > 1:
        subs.append(pts)
    return subs


def ellipse_points(cx, cy, rx, ry, n=256):
    return [(cx + rx * math.cos(2 * math.pi * k / n), cy + ry * math.sin(2 * math.pi * k / n)) for k in range(n)]


def element_subpaths(el, tag):
    f = lambda name, default=0.0: float(re.sub(r"[a-z%]+$", "", el.get(name, str(default))))
    if tag == "path":
        return flatten_path(el.get("d", ""))
    if tag == "rect":
        x, y, w, h = f("x"), f("y"), f("width"), f("height")
        rx, ry = f("rx", f("ry")), f("ry", f("rx"))
        rx, ry = min(rx, w / 2), min(ry, h / 2)
        if rx <= 0 or ry <= 0:
            return [[(x, y), (x + w, y), (x + w, y + h), (x, y + h)]]
        quarter = lambda cx, cy, a0: [(cx + rx * math.cos(math.radians(a0 + 90 * k / 16)),
                                      cy + ry * math.sin(math.radians(a0 + 90 * k / 16))) for k in range(17)]
        return [quarter(x + w - rx, y + ry, -90) + quarter(x + w - rx, y + h - ry, 0) +
                quarter(x + rx, y + h - ry, 90) + quarter(x + rx, y + ry, 180)]
    if tag == "circle":
        return [ellipse_points(f("cx"), f("cy"), f("r"), f("r"))]
    if tag == "ellipse":
        return [ellipse_points(f("cx"), f("cy"), f("rx"), f("ry"))]
    if tag == "polygon":
        v = [float(t) for t in re.findall(r"[-+]?(?:\d*\.\d+|\d+\.?)(?:[eE][-+]?\d+)?", el.get("points", ""))]
        return [list(zip(v[0::2], v[1::2]))]
    return []


def render_svg(source, width, height, supersample=8):
    """RGBA uint8 (height, width, 4) of an SVG (path/rect/circle/ellipse/polygon with solid fills, fill-opacity,
    opacity, fill-rule, nested transforms), stretched from its viewBox to width x height. Each shape is
    rasterised at `supersample` times the size, then box-reduced to exact coverage (strokes, gradients and
    clip paths are ignored: skipped shapes are counted in the returned warnings)."""
    text = source.read_text(encoding="utf-8") if hasattr(source, "read_text") else source
    root = ET.fromstring(text)
    vb = [float(v) for v in re.split(r"[\s,]+", root.get("viewBox", f"0 0 {width} {height}").strip())]
    base = np.array([[width / vb[2], 0, -vb[0] * width / vb[2]], [0, height / vb[3], -vb[1] * height / vb[3]], [0, 0, 1]])
    dst = np.zeros((height, width, 4), np.float32)  # premultiplied, 0..1
    warnings = []
    ss = supersample

    def style(el, inherited):
        s = dict(inherited)
        for k in ("fill", "fill-opacity", "fill-rule", "opacity"):
            if k in el.attrib:
                s[k] = el.get(k)
        for k, v in re.findall(r"([\w-]+)\s*:\s*([^;]+)", el.get("style", "")):
            s[k] = v.strip()
        return s

    def draw(el, inherited, matrix, group_opacity):
        tag = el.tag.rsplit("}", 1)[-1]
        if tag in ("defs", "clipPath", "mask", "title", "desc", "metadata", "linearGradient", "radialGradient"):
            return
        s = style(el, inherited)
        m = matrix @ parse_transform(el.get("transform"))
        opacity = group_opacity * float(s.get("opacity", 1)) if tag != "svg" else group_opacity
        if tag in ("g", "svg", "a", "switch"):
            child_style = {k: v for k, v in s.items() if k != "opacity"}
            for child in el:
                draw(child, child_style, m, opacity)
            return
        paint = s.get("fill", "black")
        if paint.strip() == "none":
            return
        color = parse_color(paint)
        subs = element_subpaths(el, tag) if tag in ("path", "rect", "circle", "ellipse", "polygon") else []
        if color is None or not subs:
            warnings.append(f"<{tag}> skipped (fill {paint!r})")
            return
        alpha = opacity * float(s.get("fill-opacity", 1))
        polys = []
        for pts in subs:
            xy = (m @ np.array([[p[0] for p in pts], [p[1] for p in pts], [1] * len(pts)]))[:2].T
            polys.append(xy)
        lo = np.floor(np.min([p.min(0) for p in polys], 0)).astype(int)
        hi = np.ceil(np.max([p.max(0) for p in polys], 0)).astype(int)
        x0, y0, x1, y1 = max(lo[0], 0), max(lo[1], 0), min(hi[0], width), min(hi[1], height)
        if x1 <= x0 or y1 <= y0:
            return
        even_odd = s.get("fill-rule", "nonzero").strip() == "evenodd"
        wind = np.zeros(((y1 - y0) * ss, (x1 - x0) * ss), np.int16)
        for xy in polys:
            if len(xy) < 3:
                continue
            big = (xy - [x0, y0]) * ss - 0.5  # PIL addresses a pixel by its centre coordinate
            area = float(np.sum(big[:, 0] * np.roll(big[:, 1], -1) - np.roll(big[:, 0], -1) * big[:, 1]))
            img = Image.new("1", (wind.shape[1], wind.shape[0]), 0)
            ImageDraw.Draw(img).polygon([tuple(p) for p in big], fill=1)
            mask = np.asarray(img, dtype=np.int16)
            if even_odd:
                wind ^= mask
            else:
                wind += mask if area > 0 else -mask  # opposite windings cancel: holes
        cover = (wind != 0).reshape(y1 - y0, ss, x1 - x0, ss).sum((1, 3), dtype=np.float32) / (ss * ss) * alpha
        rgb = np.array(color, np.float32) / 255
        region = dst[y0:y1, x0:x1]
        region[:] = np.concatenate([rgb * cover[..., None], cover[..., None]], -1) + region * (1 - cover[..., None])

    draw(root, {}, base, 1.0)
    a = dst[..., 3:4]
    rgb = np.where(a > 0, dst[..., :3] / np.maximum(a, 1e-8), 0)
    return np.rint(np.concatenate([rgb, a], -1).clip(0, 1) * 255).astype(np.uint8), warnings


# ---------------------------------------------------------------- mips

def srgb_to_linear(c):
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def linear_to_srgb(c):
    return np.where(c <= 0.0031308, c * 12.92, 1.055 * np.maximum(c, 0) ** (1 / 2.4) - 0.055)


def mip_chain(rgba, srgb):
    """All levels down to 1x1 (uint8 RGBA): 2x2 box filter on premultiplied colour, in linear light for sRGB."""
    f = rgba.astype(np.float32) / 255
    if srgb:
        f[..., :3] = srgb_to_linear(f[..., :3])
    f[..., :3] *= f[..., 3:]
    levels = []
    while True:
        a = f[..., 3:]
        rgb = np.where(a > 0, f[..., :3] / np.maximum(a, 1e-8), 0)
        if srgb:
            rgb = linear_to_srgb(rgb)
        levels.append(np.rint(np.concatenate([rgb, a], -1).clip(0, 1) * 255).astype(np.uint8))
        h, w = f.shape[:2]
        if h == 1 and w == 1:
            return levels
        nh, nw = max(h >> 1, 1), max(w >> 1, 1)
        f = f[np.minimum(np.arange(2 * nh), h - 1)][:, np.minimum(np.arange(2 * nw), w - 1)]
        f = f.reshape(nh, 2, nw, 2, 4).mean((1, 3))


# ---------------------------------------------------------------- block codecs

def to_blocks(plane):
    """(H, W, C) -> (N, 16, C) 4x4 blocks in row-major block order, edges replicated to a multiple of 4."""
    h, w, c = plane.shape
    p = np.pad(plane, ((0, -h % 4), (0, -w % 4), (0, 0)), mode="edge")
    return p.reshape(p.shape[0] // 4, 4, p.shape[1] // 4, 4, c).transpose(0, 2, 1, 3, 4).reshape(-1, 16, c)


def from_blocks(blocks, h, w):
    c = blocks.shape[-1]
    bh, bw = (h + 3) // 4, (w + 3) // 4
    p = blocks.reshape(bh, bw, 4, 4, c).transpose(0, 2, 1, 3, 4).reshape(bh * 4, bw * 4, c)
    return p[:h, :w]


def bc4_palette(e0, e1):
    """(N, 8) palettes: 8-value mode when e0 > e1, else 6-value mode with 0 and 255."""
    e0, e1 = e0[:, None].astype(np.float32), e1[:, None].astype(np.float32)
    k8 = np.arange(1, 7, dtype=np.float32)
    p8 = np.concatenate([e0, e1, ((7 - k8) * e0 + k8 * e1) / 7], 1)
    k6 = np.arange(1, 5, dtype=np.float32)
    p6 = np.concatenate([e0, e1, ((5 - k6) * e0 + k6 * e1) / 5, np.zeros_like(e0), np.full_like(e0, 255)], 1)
    return np.where(e0 > e1, p8, p6)


def bc4_encode(v):
    """v: (N, 16) floats 0..255 -> (N, 8) uint8 blocks. Tries the 8-value (max/min) and 6-value modes."""
    v = np.asarray(v, np.float32)
    n = len(v)
    hi8, lo8 = np.rint(v.max(1)), np.rint(v.min(1))
    inner = (v > 0) & (v < 255)
    lo6 = np.rint(np.where(inner, v, 255).min(1))
    hi6 = np.rint(np.where(inner, v, 0).max(1))
    none = lo6 > hi6
    lo6, hi6 = np.where(none, 0, lo6), np.where(none, 255, hi6)
    best = None
    for e0, e1 in ((hi8, lo8), (lo6, hi6)):
        pal = bc4_palette(e0, e1)
        d = (v[:, :, None] - pal[:, None, :]) ** 2
        idx, err = d.argmin(2), d.min(2).sum(1)
        if e0 is hi8:
            err = np.where(hi8 > lo8, err, np.inf)  # equal endpoints would be read as 6-value mode
        cand = (err, e0, e1, idx)
        if best is None:
            best = cand
        else:
            take = err < best[0]
            best = tuple(np.where(take[(...,) + (None,) * (a.ndim - 1)], a, b) for a, b in zip(cand, best))
    _, e0, e1, idx = best
    bits = np.zeros(n, np.uint64)
    for k in range(16):
        bits |= idx[:, k].astype(np.uint64) << np.uint64(3 * k)
    out = np.empty((n, 8), np.uint8)
    out[:, 0], out[:, 1] = e0, e1
    out[:, 2:] = bits.astype("<u8").view(np.uint8).reshape(n, 8)[:, :6]
    return out


def bc4_decode(blocks):
    n = len(blocks)
    bits = np.zeros(n, np.uint64)
    for k in range(6):
        bits |= blocks[:, 2 + k].astype(np.uint64) << np.uint64(8 * k)
    idx = np.stack([(bits >> np.uint64(3 * k)) & np.uint64(7) for k in range(16)], 1).astype(np.intp)
    pal = bc4_palette(blocks[:, 0], blocks[:, 1])
    return np.take_along_axis(pal, idx, 1)


def pack565(c):
    q = np.stack([np.rint(c[..., 0] / 255 * 31), np.rint(c[..., 1] / 255 * 63), np.rint(c[..., 2] / 255 * 31)], -1)
    q = q.clip(0, [31, 63, 31]).astype(np.uint16)
    return (q[..., 0] << 11) | (q[..., 1] << 5) | q[..., 2]


def unpack565(v):
    r, g, b = (v >> 11) & 31, (v >> 5) & 63, v & 31
    return np.stack([(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)], -1).astype(np.float32)


def bc1_palette(c0, c1):
    p0, p1 = unpack565(c0), unpack565(c1)
    return np.stack([p0, p1, (2 * p0 + p1) / 3, (p0 + 2 * p1) / 3], 1)


def bc1_encode(px, iterations=4):
    """px: (N, 16, 3) floats 0..255 -> (N, 8) uint8 blocks, always in 4-colour mode. Endpoints start on the
    principal axis of the block's colours (extreme projections), then least-squares refit to the chosen indices."""
    px = np.asarray(px, np.float32)
    n = len(px)
    mean = px.mean(1, keepdims=True)
    d = px - mean
    axis = np.linalg.eigh(np.einsum("nki,nkj->nij", d, d))[1][:, :, 2]
    t = np.einsum("nki,ni->nk", d, axis)
    ends = (mean[:, 0] + axis * t.min(1)[:, None], mean[:, 0] + axis * t.max(1)[:, None])

    def quantise(a, b):
        ca, cb = pack565(np.clip(a, 0, 255)), pack565(np.clip(b, 0, 255))
        return np.maximum(ca, cb), np.minimum(ca, cb)  # c0 >= c1; equal still decodes to one colour

    def score(c0, c1):
        dist = ((px[:, :, None] - bc1_palette(c0, c1)[:, None]) ** 2).sum(3)
        return dist.argmin(2), dist.min(2).sum(1)

    c0, c1 = quantise(*ends)
    idx, err = score(c0, c1)
    weight = np.array([1, 0, 2 / 3, 1 / 3], np.float32)  # share of endpoint 0 per index
    for _ in range(iterations):
        a = weight[idx]
        b = 1 - a
        aa, ab, bb = (a * a).sum(1), (a * b).sum(1), (b * b).sum(1)
        det = aa * bb - ab * ab
        ok = np.abs(det) > 1e-6
        safe = np.where(ok, det, 1)
        ap, bp = np.einsum("nk,nkc->nc", a, px), np.einsum("nk,nkc->nc", b, px)
        x0 = (bb[:, None] * ap - ab[:, None] * bp) / safe[:, None]
        x1 = (aa[:, None] * bp - ab[:, None] * ap) / safe[:, None]
        n0, n1 = quantise(x0, x1)
        n_idx, n_err = score(n0, n1)
        take = ok & (n_err < err)
        c0, c1 = np.where(take, n0, c0), np.where(take, n1, c1)
        idx, err = np.where(take[:, None], n_idx, idx), np.where(take, n_err, err)
    bits = np.zeros(n, np.uint32)
    for k in range(16):
        bits |= idx[:, k].astype(np.uint32) << np.uint32(2 * k)
    out = np.empty((n, 8), np.uint8)
    out[:, 0:2] = c0.astype("<u2")[:, None].view(np.uint8)
    out[:, 2:4] = c1.astype("<u2")[:, None].view(np.uint8)
    out[:, 4:8] = bits.astype("<u4")[:, None].view(np.uint8)
    return out


def bc1_decode(blocks, four_colour=False):
    """(N, 16, 4) uint8 RGBA. BC1 with c0 <= c1 is 3-colour + transparent unless `four_colour` (BC3's colour half)."""
    c0 = blocks[:, 0].astype(np.uint16) | (blocks[:, 1].astype(np.uint16) << 8)
    c1 = blocks[:, 2].astype(np.uint16) | (blocks[:, 3].astype(np.uint16) << 8)
    pal = bc1_palette(c0, c1)
    alpha = np.full((len(blocks), 4), 255, np.float32)
    three = (c0 <= c1) & (not four_colour)
    pal3 = pal.copy()
    pal3[:, 2] = (unpack565(c0) + unpack565(c1)) / 2
    pal3[:, 3] = 0
    pal = np.where(three[:, None, None], pal3, pal)
    alpha[three, 3] = 0
    bits = sum(blocks[:, 4 + k].astype(np.uint32) << np.uint32(8 * k) for k in range(4))
    idx = np.stack([(bits >> np.uint32(2 * k)) & np.uint32(3) for k in range(16)], 1).astype(np.intp)
    rgb = np.take_along_axis(pal, idx[:, :, None], 1)
    a = np.take_along_axis(alpha, idx, 1)
    return np.rint(np.concatenate([rgb, a[..., None]], -1)).astype(np.uint8)


KINDS = ("bc1", "bc3", "bc4", "bc5", "rgba")


def encode_level(rgba, kind):
    """One mip level (H, W, 4 uint8) as the bytes of `kind` (bc4 uses channel R, bc5 R and G)."""
    h, w = rgba.shape[:2]
    f = rgba.astype(np.float32)
    if kind == "rgba":
        return rgba.tobytes()
    b = to_blocks(f)
    if kind == "bc1":
        return bc1_encode(b[..., :3]).tobytes()
    if kind == "bc3":
        return np.concatenate([bc4_encode(b[..., 3]), bc1_encode(b[..., :3])], 1).tobytes()
    if kind == "bc4":
        return bc4_encode(b[..., 0]).tobytes()
    return np.concatenate([bc4_encode(b[..., 0]), bc4_encode(b[..., 1])], 1).tobytes()  # bc5


def decode_level(data, kind, w, h):
    """Inverse of encode_level, as RGBA uint8 (bc4 -> grey, bc5 -> R,G,0)."""
    if kind == "rgba":
        return np.frombuffer(data, np.uint8).reshape(h, w, 4)
    size = {"bc1": 8, "bc4": 8, "bc3": 16, "bc5": 16}[kind]
    blocks = np.frombuffer(data, np.uint8).reshape(-1, size)
    if kind == "bc1":
        px = bc1_decode(blocks)
    elif kind == "bc3":
        px = bc1_decode(blocks[:, 8:], True)
        px[..., 3] = np.rint(bc4_decode(blocks[:, :8])).astype(np.uint8)
    elif kind == "bc4":
        g = np.rint(bc4_decode(blocks)).astype(np.uint8)
        px = np.stack([g, g, g, np.full_like(g, 255)], -1)
    else:
        r, g = (np.rint(bc4_decode(blocks[:, i:i + 8])).astype(np.uint8) for i in (0, 8))
        px = np.stack([r, g, np.zeros_like(r), np.full_like(r, 255)], -1)
    return from_blocks(px, h, w)


# ---------------------------------------------------------------- DDS

DXGI = {("bc1", False): 71, ("bc1", True): 72, ("bc3", False): 77, ("bc3", True): 78, ("bc4", False): 80,
        ("bc5", False): 83, ("rgba", False): 28, ("rgba", True): 29}


def dds_bytes(kind, srgb, width, height, levels):
    """DX10-header DDS (DXGI *_SRGB for sRGB guests, UNORM otherwise) with the given encoded mip payloads."""
    srgb = srgb and kind in ("bc1", "bc3", "rgba")
    flags = 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000 | 0x80000  # caps height width pixelformat mipmapcount linearsize
    head = b"DDS " + struct.pack("<7I", 124, flags, height, width, len(levels[0]), 0, len(levels)) + bytes(44)
    head += struct.pack("<2I4s5I", 32, 4, b"DX10", 0, 0, 0, 0, 0)
    head += struct.pack("<5I", 0x1000 | 0x8 | 0x400000, 0, 0, 0, 0)
    head += struct.pack("<5I", DXGI[(kind, srgb)], 3, 0, 1, 0)
    return head + b"".join(levels)


def build_dds(rgba, kind, srgb):
    levels = mip_chain(rgba, srgb)
    return dds_bytes(kind, srgb, rgba.shape[1], rgba.shape[0], [encode_level(l, kind) for l in levels])
