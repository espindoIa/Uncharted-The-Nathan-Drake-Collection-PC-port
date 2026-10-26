"""Builds the launcher's lettering from the games' own art (25 Oct 2026), kept in gow3_pc/assets/ui
(the owner's exception for the games' UI art, 25 Oct 2026).

The font. The three games draw their text with one bitmap font, "Adelon-Serial DB" at 42 px
(BMFont main.fnt + main_00.tga, glyphs packed in the blue and green channels, in u3data/data.psarc
-> data/fonts/ps4; source copy in fonts/src). Steps, with a manual Image Trace in VectorCraft:

    python tools/build_ui_art.py sheet   # fonts/src/main.fnt -> fonts/src/sheet.png (+ sheet.json)
    # VectorCraft: open sheet.png, Object > Image Trace > Make and Expand (black and white,
    # threshold 128, paths 70, corners 60, noise 6, ignore white), export as fonts/src/traced.svg
    python tools/build_ui_art.py font    # -> fonts/drake-serial.ttf ("Drake Serial")

The logo. The Collection's own logo, from Uncharted 1's menu.pak (art/general/menu/bootloader/
ucclogo-eng.tga and its -alpha twin, 1024x256 BC1 / BC4, PS4 1D-thin tiled and stored upside down):

    python tools/build_ui_art.py logo <game folder>   # -> logo/src/*.png (the textures, the trace source)
    # VectorCraft: open logo/src/wordmark-trace.png, Image Trace (black and white, threshold 128,
    # paths 60, corners 60, noise 3, ignore white), export wordmark.svg. The launcher draws that
    # SVG at the exact size each screen scale needs (launcher/art.py svg_mask): no bitmap resize.

The HD textures. Replacements for the games' UI textures that the runtime swaps in (gow3_hdtex.cpp):

    python tools/build_ui_art.py hd [--only <hash prefix>] [--preview] [--rgba] [--src DIR] [--out DIR]

    Reads  gow3_pc/assets/ui/hd/src/<u1|u2|u3>/<hash16hex>_<w>x<h>_<vkformat>.<png|svg>  (the name of the
    texture dump, w x h = the guest size; PNG already at the target size, SVG drawn at it: 4x up to 512 px
    on the long side, 2x up to 1024, 1x above), writes  hd/<game>/<hash16hex>_<w>x<h>.dds  with the full mip
    chain (premultiplied box filter, linear light for *Srgb* guests) and a DX10 header. Format by guest:
    Bc4/R8 -> BC4 (the R channel), Bc5/R8G8 -> BC5, opaque Bc1 -> BC1, everything else -> BC3; a name ending
    in .rgba.png / .rgba.svg (or --rgba) keeps uncompressed RGBA8. --preview writes before/after PNGs to
    analysis/ui-hd-preview/ (before = analysis/ui-dump/ PNG, bilinear to the target size; after = our DDS decoded).
    The SVG rasteriser and block encoders are in tools/hd_art.py (numpy + Pillow).

Needs Pillow; fontTools for `font` (build time only: the launcher only loads the results); numpy for `hd`.
"""
import io
import json
import re
import struct
import sys
from pathlib import Path

from PIL import Image, ImageFilter

ROOT = Path(__file__).resolve().parent.parent
UI = ROOT / "gow3_pc" / "assets" / "ui"
SRC = UI / "fonts" / "src"
TTF = UI / "fonts" / "drake-serial.ttf"
FAMILY = "Drake Serial"
UP, CELL, COLS = 7, 380, 14  # the trace sheet: glyphs 7x up, one 380 px cell each
SMOOTH = 0.3  # blur radius before the trace, in source pixels (0.55 bloated the stems and rounded the serifs)
SIZE = 42  # the atlas font's em in px; the TrueType em is 1000 units


def sheet():
    text = (SRC / "main.fnt").read_text()
    atlas = Image.open(SRC / re.search(r'file="([^"]+)"', text).group(1))
    channel = {1: "B", 2: "G", 4: "R", 8: "A"}
    chars = [{k: int(v) for k, v in re.findall(r"(\w+)=(-?\d+)", line)}
             for line in text.splitlines() if line.startswith("char ")]
    chars = [c for c in chars if c["width"] > 0 and c["height"] > 0]
    image = Image.new("L", (COLS * CELL, (len(chars) + COLS - 1) // COLS * CELL), 255)
    cells = {}
    for i, c in enumerate(chars):
        glyph = atlas.getchannel(channel[c["chnl"]]).crop((c["x"], c["y"], c["x"] + c["width"], c["y"] + c["height"]))
        # Bicubic plus a slight blur: the trace then follows the antialiased edge, not the pixel steps.
        # 25 Oct 2026 (second pass): the 42 px antialiasing left bumps along the stems that showed as
        # wobble at large sizes. A wide blur evens them out, then a steep curve brings the edge back
        # to a crisp transition so the trace follows a smooth contour.
        glyph = glyph.resize((c["width"] * UP, c["height"] * UP), Image.LANCZOS).filter(ImageFilter.GaussianBlur(UP * SMOOTH))
        glyph = glyph.point(lambda v: max(0, min(255, (v - 128) * 4 + 128)))
        cx, cy = (i % COLS) * CELL + 20, (i // COLS) * CELL + 20
        image.paste(Image.eval(glyph, lambda v: 255 - v), (cx, cy))
        cells[c["id"]] = {**c, "cx": cx, "cy": cy, "col": i % COLS, "row": i // COLS}
    image.save(SRC / "sheet.png")
    common = {k: int(v) for k, v in re.findall(r"(\w+)=(-?\d+)", re.search(r"^common .*$", text, re.M).group(0))}
    kerning = [[int(v) for v in m] for m in re.findall(r"kerning first=(\d+)\s+second=(\d+)\s+amount=(-?\d+)", text)]
    (SRC / "sheet.json").write_text(json.dumps({"common": common, "chars": cells, "kerning": kerning}))
    print(f"{len(chars)} glyphs -> {SRC / 'sheet.png'}")


def area(points):
    return sum(x0 * y1 - x1 * y0 for (x0, y0), (x1, y1) in zip(points, points[1:] + points[:1])) / 2


def inside(point, polygon):
    x, y, hit = point[0], point[1], False
    for (x0, y0), (x1, y1) in zip(polygon, polygon[1:] + polygon[:1]):
        if (y0 > y) != (y1 > y) and x < x0 + (y - y0) * (x1 - x0) / (y1 - y0):
            hit = not hit
    return hit


STEP, CORNER = 8.0, 50  # contour smoothing: resample step in sheet px; turning angle that is a corner


def flatten(ops):
    """A traced contour as a closed polyline (sheet px)."""
    points, last = [], None
    for op, args in ops:
        if op == "moveTo":
            last = args[0]
            points.append(last)
        elif op == "lineTo":
            last = args[0]
            points.append(last)
        elif op == "curveTo":
            (x0, y0), (x1, y1), (x2, y2), (x3, y3) = last, *args
            for i in range(1, 13):
                t, u = i / 12, 1 - i / 12
                points.append((u**3 * x0 + 3 * u * u * t * x1 + 3 * u * t * t * x2 + t**3 * x3,
                               u**3 * y0 + 3 * u * u * t * y1 + 3 * u * t * t * y2 + t**3 * y3))
            last = args[-1]
    if len(points) > 1 and points[0] == points[-1]:
        points.pop()
    return points


def smooth(points):
    """Evens the wobble the 42 px source left along the contours: resample evenly, smooth between
    corners (sharp turns stay exactly where the trace put them), refit as Catmull-Rom cubics.
    Returns RecordingPen ops."""
    import math
    n0 = len(points)
    seg = [math.dist(points[i], points[(i + 1) % n0]) for i in range(n0)]
    total = sum(seg)
    count = max(8, round(total / STEP))
    step, out, i, run = total / count, [], 0, 0.0
    for s in range(count):
        want = s * step
        while run + seg[i] < want and i < n0 - 1:
            run += seg[i]
            i += 1
        f = (want - run) / seg[i] if seg[i] else 0
        a, b = points[i], points[(i + 1) % n0]
        out.append((a[0] + (b[0] - a[0]) * f, a[1] + (b[1] - a[1]) * f))
    n = len(out)
    reach = max(2, round(24 / step))  # corners are judged over ~24 px each side
    turn = []
    for s in range(n):
        p, a, b = out[s], out[s - reach], out[(s + reach) % n]
        v1, v2 = (p[0] - a[0], p[1] - a[1]), (b[0] - p[0], b[1] - p[1])
        turn.append(abs(math.degrees(math.atan2(v1[0] * v2[1] - v1[1] * v2[0], v1[0] * v2[0] + v1[1] * v2[1]))))
    corners = [s for s in range(n) if turn[s] > CORNER and turn[s] >= max(turn[(s + d) % n] for d in range(-reach, reach + 1))]
    if not corners:
        corners = [0]  # a closed smooth contour: still split once so every run has two ends
    runs = []
    for c, nxt in zip(corners, corners[1:] + [corners[0] + n]):
        runs.append([out[t % n] for t in range(c, nxt + 1)])
    ops = []
    for run in runs:
        m, half = len(run), 3
        kept = [run[0]]
        for t in range(1, m - 1):
            w = min(half, t, m - 1 - t)
            kept.append((sum(run[t + d][0] for d in range(-w, w + 1)) / (2 * w + 1),
                         sum(run[t + d][1] for d in range(-w, w + 1)) / (2 * w + 1)))
        kept.append(run[-1])
        if not ops:
            ops.append(("moveTo", (kept[0],)))
        for t in range(len(kept) - 1):
            p0, p1, p2 = kept[max(t - 1, 0)], kept[t], kept[t + 1]
            p3 = kept[min(t + 2, len(kept) - 1)]
            ops.append(("curveTo", ((p1[0] + (p2[0] - p0[0]) / 6, p1[1] + (p2[1] - p0[1]) / 6),
                                    (p2[0] - (p3[0] - p1[0]) / 6, p2[1] - (p3[1] - p1[1]) / 6), p2)))
    ops.append(("closePath", ()))
    return ops


def font():
    from fontTools.fontBuilder import FontBuilder
    from fontTools.pens.cu2quPen import Cu2QuPen
    from fontTools.pens.recordingPen import RecordingPen
    from fontTools.pens.reverseContourPen import ReverseContourPen
    from fontTools.pens.transformPen import TransformPen
    from fontTools.pens.ttGlyphPen import TTGlyphPen
    from fontTools.svgLib.path import parse_path
    from fontTools.ttLib import newTable
    from fontTools.ttLib.tables._k_e_r_n import KernTable_format_0, table__k_e_r_n

    meta = json.loads((SRC / "sheet.json").read_text())
    chars = {int(k): v for k, v in meta["chars"].items()}
    base, k = meta["common"]["base"], 1000 / SIZE
    # Every traced contour, filed under the glyph whose cell holds its centre.
    by_cell = {(c["col"], c["row"]): [] for c in chars.values()}
    for d in re.findall(r'<path d="([^"]+)"', (SRC / "traced.svg").read_text()):
        pen = RecordingPen()
        parse_path(d, pen)
        ops = []
        for op, args in pen.value:
            ops.append((op, args))
            if op in ("closePath", "endPath"):
                points = flatten(ops)
                xs, ys = [p[0] for p in points], [p[1] for p in points]
                cell = (int((min(xs) + max(xs)) / 2 // CELL), int((min(ys) + max(ys)) / 2 // CELL))
                if cell in by_cell and len(points) > 2:
                    by_cell[cell].append((smooth(points), points))
                ops = []
    glyphs, advances, cmap, names = {".notdef": TTGlyphPen(None).glyph()}, {".notdef": 500}, {}, {}
    for cid, c in sorted(chars.items()):
        name = names[cid] = cmap[cid] = f"uni{cid:04X}"
        tt = TTGlyphPen(None)
        quadratic = Cu2QuPen(tt, 1.0)
        # Sheet px -> font units, y up: x = (xoffset + gx) k, y = (base - yoffset - gy) k.
        move = TransformPen(quadratic, (k / UP, 0, 0, -k / UP, (c["xoffset"] - c["cx"] / UP) * k,
                                        (base - c["yoffset"] + c["cy"] / UP) * k))
        own = by_cell[(c["col"], c["row"])]
        for ops, points in own:
            # TrueType wants outer contours clockwise, holes the other way (y up). The sheet is y
            # down, so a positive shoelace area there is counter-clockwise in font space.
            outer = sum(inside(points[0], o[1]) for o in own if o[1] is not points) % 2 == 0
            replay = RecordingPen()
            replay.value = ops
            replay.replay(ReverseContourPen(move) if outer == (area(points) > 0) else move)
        glyphs[name] = tt.glyph()
        advances[name] = round(c["xadvance"] * k)
    for space in {32, 160} - set(cmap):  # without pixels they may be missing from the sheet
        name = cmap[space] = names[space] = f"uni{space:04X}"
        glyphs[name], advances[name] = TTGlyphPen(None).glyph(), round(10 * k)
    builder = FontBuilder(1000, isTTF=True)
    builder.setupGlyphOrder(list(glyphs))
    builder.setupCharacterMap(cmap)
    builder.setupGlyf(glyphs)
    builder.setupHorizontalMetrics({n: (w, getattr(glyphs[n], "xMin", 0)) for n, w in advances.items()})
    ascent, descent = round(base * k), round((base - SIZE) * k)
    builder.setupHorizontalHeader(ascent=ascent, descent=descent)
    # Windows' GDI refuses a font without the full, unique and PostScript names.
    builder.setupNameTable({"familyName": FAMILY, "styleName": "Regular", "fullName": FAMILY,
                            "uniqueFontIdentifier": f"{FAMILY};1.0", "psName": FAMILY.replace(" ", "-"),
                            "version": "Version 1.0"})
    builder.setupOS2(sTypoAscender=ascent, sTypoDescender=descent, sTypoLineGap=0, usWinAscent=ascent,
                     usWinDescent=-descent, achVendID="NONE")
    builder.setupPost()
    # No hinting: ask the rasterizer for antialiasing without grid-fitting at every size, so the
    # shapes keep their proportions at 1x, 1.5x, 2x and 4K scales (0x0A: grayscale + symmetric smoothing).
    gasp = newTable("gasp")
    gasp.version, gasp.gaspRange = 1, {0xFFFF: 0x000A}
    builder.font["gasp"] = gasp
    kern = table__k_e_r_n()
    kern.version, pairs = 0, KernTable_format_0()
    pairs.version, pairs.coverage, pairs.format = 0, 1, 0
    pairs.kernTable = {(names[a], names[b]): round(v * k) for a, b, v in meta["kerning"]
                       if v and a in names and b in names}
    kern.kernTables = [pairs]
    builder.font["kern"] = kern
    builder.save(TTF)
    print(f"{len(cmap)} characters, {len(pairs.kernTable)} kerning pairs -> {TTF}")


def morton(x, y):
    return sum(((x >> i) & 1) << (2 * i) | ((y >> i) & 1) << (2 * i + 1) for i in range(3))


def pak_texture(pak, data, name, fourcc):
    """The top mip of a texture in a Bluepoint .pak as an image. The pak is its header, resource
    pages, then the VRAM blob (header dword 8 is its size, so it ends the file). Each texture's
    descriptor sits 0x38 bytes before its d:/dev/... cook path: blob offset (+0), width (+0x20),
    height (+0x24). Its 4x4 blocks are 1D-thin tiled: 8x8 blocks per tile, Morton order inside."""
    path = re.search(rb"d:/dev/[^\0]*/" + re.escape(name.encode()) + rb"/", data).start()
    offset,= struct.unpack_from("<I", data, path - 0x38)
    width, height = struct.unpack_from("<2I", data, path - 0x38 + 0x20)
    vram = len(data) - struct.unpack_from("<I", data, 0x20)[0]
    size = 8  # bytes per block, BC1 and BC4
    blocks_x, blocks_y = width // 4, height // 4
    tiled = data[vram + offset:vram + offset + blocks_x * blocks_y * size]
    linear = bytearray(len(tiled))
    for by in range(blocks_y):
        for bx in range(blocks_x):
            src = ((by // 8) * (blocks_x // 8) + bx // 8) * 64 + morton(bx % 8, by % 8)
            dst = (by * blocks_x + bx) * size
            linear[dst:dst + size] = tiled[src * size:(src + 1) * size]
    dds = b"DDS " + struct.pack("<7I", 124, 0x81007, height, width, len(linear), 0, 1) + bytes(44)
    dds += struct.pack("<2I4s5I", 32, 4, fourcc, 0, 0, 0, 0, 0) + struct.pack("<5I", 0x1000, 0, 0, 0, 0)
    print(f"{name}: {width}x{height} at VRAM +{offset:#x} in {pak.name}")
    return Image.open(io.BytesIO(dds + bytes(linear))).transpose(Image.FLIP_TOP_BOTTOM)


def logo(game):
    pak = Path(game) / "u1data" / "build" / "main" / "pak19" / "ps4" / "menu.pak"
    data = pak.read_bytes()
    out = UI / "logo" / "src"
    out.mkdir(parents=True, exist_ok=True)
    color = pak_texture(pak, data, "ucclogo-eng.tga", b"DXT1").convert("RGB")
    alpha = pak_texture(pak, data, "ucclogo-eng-alpha.tga", b"ATI1").convert("L")
    color.save(out / "ucclogo-eng.png")
    alpha.save(out / "ucclogo-eng-alpha.png")
    # The launcher's title: the logo's UNCHARTED (with its TM), white and worn as on the games'
    # title screens: the alpha's scratches over a parchment white, cropped to the word.
    box = (66, 42, 970, 158)
    # The texture is only 904x116 for the word, which went soft on 4K screens, so the launcher
    # uses a vector trace of it. Source for the trace: scaled 4x with a light blur and a steep curve
    # (smooth edges), black on white; the scratches of the logo survive as holes in the letters.
    ink = alpha.crop(box).resize((alpha.crop(box).width * 4, alpha.crop(box).height * 4), Image.LANCZOS)
    ink = ink.filter(ImageFilter.GaussianBlur(1.2)).point(lambda v: 255 - max(0, min(255, (v - 128) * 4 + 128)))
    ink.save(out / "wordmark-trace.png")
    print(f"-> {out}")


HD_NAME = re.compile(r"([0-9a-f]{16})_(\d+)x(\d+)_(\w+)")


def hd_scale(width, height):
    """HD factor over the guest size: 4x up to 512 px on the long side, 2x up to 1024, else 1x."""
    return 4 if max(width, height) <= 512 else 2 if max(width, height) <= 1024 else 1


def hd_kind(fmt, opaque):
    """DDS format the runtime accepts for a guest format (Fit: channels must match the guest's)."""
    if fmt.startswith(("Bc4", "R8Unorm")):
        return "bc4"
    if fmt.startswith(("Bc5", "R8G8Unorm")):
        return "bc5"
    return "bc1" if fmt.startswith("Bc1") and opaque else "bc3"


def hd(argv=()):
    """Turns hd/src/<game>/<hash>_<w>x<h>_<fmt>.{png,svg} into hd/<game>/<hash>_<w>x<h>.dds (see the docstring)."""
    import argparse
    import numpy as np
    import hd_art
    ap = argparse.ArgumentParser(prog="build_ui_art.py hd")
    ap.add_argument("--only", default="", help="hash prefix")
    ap.add_argument("--preview", action="store_true", help="side-by-side PNGs in analysis/ui-hd-preview/")
    ap.add_argument("--rgba", action="store_true", help="keep uncompressed RGBA8 for every texture")
    ap.add_argument("--src", type=Path, default=UI / "hd" / "src")
    ap.add_argument("--out", type=Path, default=UI / "hd")
    args = ap.parse_args(list(argv))
    done = 0
    for game in sorted(p for p in args.src.glob("*") if p.is_dir()) if args.src.is_dir() else []:
        for file in sorted(game.iterdir()):
            raw = file.name[:-len(file.suffix)]
            keep_rgba = raw.endswith(".rgba")
            m = HD_NAME.fullmatch(raw[:-5] if keep_rgba else raw)
            if file.suffix not in (".png", ".svg") or not m or not m.group(1).startswith(args.only):
                continue
            hashname, gw, gh, fmt = m.group(1), int(m.group(2)), int(m.group(3)), m.group(4)
            k = hd_scale(gw, gh)
            tw, th = gw * k, gh * k
            if file.suffix == ".svg":
                rgba, warnings = hd_art.render_svg(file, tw, th)
            else:
                img = Image.open(file).convert("RGBA")
                warnings = []
                if img.width % gw or img.height % gh or img.width // gw != img.height // gh:
                    warnings.append(f"{img.size} is not a multiple of {gw}x{gh}: resized to {tw}x{th}")
                    img = img.resize((tw, th), Image.LANCZOS)
                rgba = np.asarray(img)
            for w in warnings:
                print(f"{file.name}: {w}")
            kind = "rgba" if keep_rgba or args.rgba else hd_kind(fmt, bool((rgba[..., 3] == 255).all()))
            if kind in ("bc4", "bc5"):  # the mask is what shows through: R (and G) over black
                rgba = rgba.copy()
                rgba[..., :3] = rgba[..., :3].astype(np.uint16) * rgba[..., 3:] // 255
                rgba[..., 3] = 255
            data = hd_art.build_dds(rgba, kind, "Srgb" in fmt)
            target = args.out / game.name / f"{hashname}_{gw}x{gh}.dds"
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
            done += 1
            print(f"{game.name}/{target.name}: {rgba.shape[1]}x{rgba.shape[0]} {kind}, {len(data)} bytes")
            if args.preview:
                dump = ROOT / "analysis" / "ui-dump" / game.name / f"{raw[:-5] if keep_rgba else raw}.png"
                th, tw = rgba.shape[:2]  # a source PNG may be larger than the default scale (e.g. 2x parchment)
                before = (Image.open(dump).convert("RGBA").resize((tw, th), Image.BILINEAR) if dump.exists()
                          else Image.new("RGBA", (tw, th), (255, 0, 255, 255)))
                payload = data[148:148 + len(hd_art.encode_level(rgba, kind))]
                after = Image.fromarray(hd_art.decode_level(payload, kind, tw, th), "RGBA")
                sheet_ = Image.new("RGBA", (tw * 2 + 8, th), (128, 128, 128, 255))
                for i, part in enumerate((before, after)):
                    sheet_.alpha_composite(part, (i * (tw + 8), 0))
                out = ROOT / "analysis" / "ui-hd-preview" / game.name
                out.mkdir(parents=True, exist_ok=True)
                sheet_.convert("RGB").save(out / f"{target.stem}.png")
    print(f"{done} texture(s) -> {args.out}")


if __name__ == "__main__":
    if sys.argv[1] == "hd":
        hd(sys.argv[2:])
    else:
        {"sheet": sheet, "font": font, "logo": logo}[sys.argv[1]](*sys.argv[2:])
