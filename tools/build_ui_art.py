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

    python tools/build_ui_art.py logo <game folder>   # -> logo/src/*.png, wordmark.png

Needs Pillow; fontTools for `font` (build time only: the launcher only loads the results).
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
UP, CELL, COLS = 6, 300, 14  # the trace sheet: glyphs 6x up, one 300 px cell each
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
        glyph = glyph.resize((c["width"] * UP, c["height"] * UP), Image.BICUBIC).filter(ImageFilter.GaussianBlur(UP * 0.35))
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


def font():
    from fontTools.fontBuilder import FontBuilder
    from fontTools.pens.cu2quPen import Cu2QuPen
    from fontTools.pens.recordingPen import RecordingPen
    from fontTools.pens.reverseContourPen import ReverseContourPen
    from fontTools.pens.transformPen import TransformPen
    from fontTools.pens.ttGlyphPen import TTGlyphPen
    from fontTools.svgLib.path import parse_path
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
                points = [p for _, a in ops for p in a]
                xs, ys = [p[0] for p in points], [p[1] for p in points]
                cell = (int((min(xs) + max(xs)) / 2 // CELL), int((min(ys) + max(ys)) / 2 // CELL))
                if cell in by_cell:
                    by_cell[cell].append((ops, points))
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
    mark = Image.new("RGBA", alpha.size, (240, 233, 218, 0))
    mark.putalpha(alpha)
    mark.crop((66, 42, 970, 158)).save(UI / "wordmark.png")
    print(f"-> {out}, {UI / 'wordmark.png'}")


if __name__ == "__main__":
    {"sheet": sheet, "font": font, "logo": logo}[sys.argv[1]](*sys.argv[2:])
