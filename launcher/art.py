"""The launcher's pictures, drawn with Pillow at the screen's own scale (25 Oct 2026 redesign): sharp
at 4K where Tk's canvas polygons were jagged.

- icon(): the icon set (assets/icons/*.png, 192 px white masters from the VectorCraft sources in
  assets/icons/src), tinted and reduced to the size asked.
- rounded(): rounded rectangles drawn 4x larger and reduced (anti-aliased edges).
- game_art(): each game's coin from its trophy pack (sce_sys/trophy/trophy0N.trp in the player's
  game folder; the port ships no game art).
- stage(): the window's backdrop: the Collection's art blurred and darkened, the game's colour as a
  glow, a film grain (assets/grain.png, made in PhotoCraft).

Tk images belong to one Tk interpreter, so the caches hang on the root window (tests create and
destroy several roots in one process).
"""
import io
import re
import struct
from functools import lru_cache
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageTk

ASSETS = Path(__file__).resolve().parent / "assets"
# The games' own lettering (tools/build_ui_art.py, 25 Oct 2026): their text font as a TrueType file
# and the Collection logo's UNCHARTED, under the repository's one folder for the games' UI art.
GAME_UI = Path(__file__).resolve().parent.parent / "gow3_pc" / "assets" / "ui"
GAME_FONT = "Drake Serial"


def load_game_font():
    """Makes the games' font (GAME_FONT) usable by this process only, before Tk lists the families.
    False when it can't (file missing, not Windows): the launcher keeps the system fonts."""
    try:
        import ctypes
        return ctypes.windll.gdi32.AddFontResourceExW(str(GAME_UI / "fonts" / "drake-serial.ttf"), 0x10, 0) > 0
    except (AttributeError, OSError):  # FR_PRIVATE (0x10) above: nothing is installed system-wide
        return False
SUPERSAMPLE = 4  # rounded shapes are drawn this much larger, then reduced


def rgb(color):
    color = color.lstrip("#")
    return tuple(int(color[i:i + 2], 16) for i in (0, 2, 4))


def mix(a, b, amount):
    """`amount` (0..1) of colour b over colour a, as #rrggbb."""
    a, b = rgb(a), rgb(b)
    return "#%02x%02x%02x" % tuple(round(x + (y - x) * amount) for x, y in zip(a, b))


def cache(widget):
    root = widget._root()
    if not hasattr(root, "_art_cache"):
        root._art_cache = {}
    return root._art_cache


def photo(widget, key, make):
    """A cached ImageTk.PhotoImage of make() for `widget`'s interpreter."""
    store = cache(widget)
    if key not in store:
        store[key] = ImageTk.PhotoImage(make(), master=widget)
    return store[key]


_masks = {}


def icon_image(name, size, color):
    if name not in _masks:
        _masks[name] = Image.open(ASSETS / "icons" / f"{name}.png").getchannel("A")
    size = max(1, int(size))
    image = Image.new("RGBA", (size, size), rgb(color) + (0,))
    image.putalpha(_masks[name].resize((size, size), Image.LANCZOS))
    return image


def icon(widget, name, size, color):
    return photo(widget, ("icon", name, int(size), color), lambda: icon_image(name, size, color))


def wordmark(widget, height):
    """The Collection logo's UNCHARTED (tools/build_ui_art.py) as a photo `height` px tall, or None
    when the file is missing (the launcher then draws text). 25 Oct 2026: it is vector art
    (wordmark.svg) drawn at exactly the size asked, for any screen scale: no bitmap is stretched or
    filtered to fit (see svg_mask)."""
    path = GAME_UI / "wordmark.svg"
    if not path.exists():
        return None
    def make():
        mask = svg_mask(path, height)
        image = Image.new("RGBA", mask.size, rgb("#f0e9da") + (0,))
        image.putalpha(mask)
        return image
    return photo(widget, ("wordmark", int(height)), make)


_SVG_TOKENS = re.compile(r"[MLCZmlcz]|-?\d*\.?\d+(?:[eE][-+]?\d+)?")


def svg_contours(path):
    """The closed contours (lists of points, in the SVG's own units) of every <path> in a
    VectorCraft SVG: M L C Z, absolute and relative; curves flattened finely (16 steps)."""
    text = Path(path).read_text(encoding="utf-8")
    view = [float(v) for v in re.search(r'viewBox="([^"]+)"', text).group(1).split()]
    contours = []
    for d in re.findall(r'<path d="([^"]+)"', text):
        tokens, i, command, here, start, points = _SVG_TOKENS.findall(d), 0, "", (0.0, 0.0), (0.0, 0.0), []
        while i < len(tokens):
            if tokens[i].isalpha():
                command = tokens[i]
                i += 1
                if command in "Zz":
                    if len(points) > 2:
                        contours.append(points)
                    points, here = [], start
                continue
            n = {"M": 2, "m": 2, "L": 2, "l": 2, "C": 6, "c": 6}[command]
            v = [float(t) for t in tokens[i:i + n]]
            i += n
            if command.islower():
                v = [v[k] + here[k % 2] for k in range(n)]
            if command in "Mm":
                if len(points) > 2:
                    contours.append(points)
                here = start = (v[0], v[1])
                points, command = [here], "L" if command == "M" else "l"  # extra pairs are lines
            elif command in "Ll":
                here = (v[0], v[1])
                points.append(here)
            else:
                (x0, y0), (x1, y1), (x2, y2), (x3, y3) = here, (v[0], v[1]), (v[2], v[3]), (v[4], v[5])
                for s in range(1, 17):
                    t, u = s / 16, 1 - s / 16
                    points.append((u**3 * x0 + 3 * u * u * t * x1 + 3 * u * t * t * x2 + t**3 * x3,
                                   u**3 * y0 + 3 * u * u * t * y1 + 3 * u * t * t * y2 + t**3 * y3))
                here = (x3, y3)
    return view, contours


def svg_mask(path, height, supersample=8):
    """Coverage mask of a VectorCraft SVG (even-odd over its contours, which the trace never
    overlaps) at exactly `height` px tall, anything but a bitmap resize: every contour is
    rasterised at `supersample` times the size, and each output pixel takes the exact fraction of its
    area covered (a box average of that coverage, not an interpolation between pixels)."""
    (x0, y0, w, h), contours = svg_contours(path)
    scale = height / h
    big = scale * supersample
    size = (round(w * scale), height)
    big_size = (size[0] * supersample, size[1] * supersample)
    mask = Image.new("1", big_size, 0)
    for points in contours:
        pts = [((x - x0) * big, (y - y0) * big) for x, y in points]
        xs, ys = [p[0] for p in pts], [p[1] for p in pts]
        left, top = max(0, int(min(xs))), max(0, int(min(ys)))
        right, bottom = min(big_size[0], int(max(xs)) + 2), min(big_size[1], int(max(ys)) + 2)
        if right <= left or bottom <= top:
            continue
        part = Image.new("1", (right - left, bottom - top), 0)
        ImageDraw.Draw(part).polygon([(x - left, y - top) for x, y in pts], fill=1)
        box = (left, top, right, bottom)
        mask.paste(ImageChops.logical_xor(mask.crop(box), part), box)  # even-odd: a hole cancels its parent
    return mask.convert("L").reduce(supersample)  # reduce() is the exact box average


@lru_cache(maxsize=96)  # the stage's panels come back on every game switch; callers never change the image
def rounded_image(w, h, r, fill=None, outline=None, width=0, opacity=255):
    """RGBA rounded rectangle; `fill` "#rrggbb" or a vertical gradient (top, bottom)."""
    w, h, k = max(1, int(w)), max(1, int(h)), SUPERSAMPLE
    big = Image.new("RGBA", (w * k, h * k), (0, 0, 0, 0))
    mask = Image.new("L", big.size, 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, w * k - 1, h * k - 1), r * k, fill=opacity)
    if isinstance(fill, tuple):
        top, bottom = rgb(fill[0]), rgb(fill[1])
        ramp = Image.linear_gradient("L").resize(big.size)
        paint = Image.composite(Image.new("RGB", big.size, bottom), Image.new("RGB", big.size, top), ramp)
        big.paste(paint, (0, 0), mask)
    elif fill:
        big.paste(Image.new("RGBA", big.size, rgb(fill) + (255,)), (0, 0), mask)
    if outline and width:
        ring = Image.new("L", big.size, 0)
        line = max(1, int(width * k))
        ImageDraw.Draw(ring).rounded_rectangle((0, 0, w * k - 1, h * k - 1), r * k, outline=255, width=line)
        big.paste(Image.new("RGBA", big.size, rgb(outline) + (255,)), (0, 0), ring)
    return big.resize((w, h), Image.LANCZOS)


def rounded(widget, w, h, r, fill=None, outline=None, width=0, opacity=255):
    key = ("round", int(w), int(h), round(r, 1), fill, outline, width, opacity)
    return photo(widget, key, lambda: rounded_image(w, h, r, fill, outline, width, opacity))


def circle_image(d, fill):
    return rounded_image(d, d, d / 2, fill)


# -- the games' art -------------------------------------------------------------------------------

def trophy_file(trp, name):
    """A file from a PS4 trophy pack (.trp): the header is 0x60 bytes (entry count at 0x10, entry
    size at 0x14), then entries of name[32], offset u64, size u64 (big-endian). Its PNGs are plain."""
    try:
        data = Path(trp).read_bytes()
        if data[:4] != b"\xdc\xa2\x4d\x00":
            return None
        count, size = struct.unpack(">II", data[0x10:0x18])
        for i in range(min(count, 512)):
            entry = data[0x60 + i * size:0x60 + (i + 1) * size]
            if entry[:32].split(b"\0")[0].decode("ascii", "replace") == name:
                offset, length = struct.unpack(">QQ", entry[32:48])
                return data[offset:offset + length]
    except (OSError, struct.error, ValueError):
        pass
    return None


def upscale(image, size):
    """`image` to `size` (w, h); enlarged pictures get a light unsharp mask, so a 320 px trophy coin
    stays crisp on a 4K card."""
    factor = max(size[0] / image.width, size[1] / image.height)
    image = image.resize(size, Image.LANCZOS)
    if factor > 1.2:
        image = image.filter(ImageFilter.UnsharpMask(radius=min(3.0, factor), percent=60, threshold=2))
    return image


_coins = {}


def game_coin(game_dir, number):
    """The game's coin (GR001.PNG of trophy0N.trp, cropped to it), or None without a game folder."""
    key = (str(game_dir), number)
    if key not in _coins:
        coin = None
        for name in ("GR001.PNG", "ICON0.PNG"):
            data = trophy_file(Path(game_dir) / "sce_sys" / "trophy" / f"trophy0{number}.trp", name)
            if data:
                try:
                    coin = Image.open(io.BytesIO(data)).convert("RGBA")
                except OSError:
                    continue
                box = coin.getchannel("A").point(lambda a: 255 if a > 24 else 0).getbbox()
                coin = coin.crop(box) if box else coin
                break
        _coins[key] = coin
    return _coins[key]


def coin_image(game_dir, number, size, accent):
    """The coin at `size` px, or a ringed medallion with the game's number without the game."""
    coin = game_coin(game_dir, number)
    if coin is not None:
        side = max(coin.size)
        square = Image.new("RGBA", (side, side), (0, 0, 0, 0))
        square.paste(coin, ((side - coin.width) // 2, (side - coin.height) // 2))
        return upscale(square, (size, size))
    k = SUPERSAMPLE
    big = Image.new("RGBA", (size * k, size * k), (0, 0, 0, 0))
    draw = ImageDraw.Draw(big)
    draw.ellipse((k, k, size * k - k, size * k - k), fill=rgb(mix("#151c23", accent, 0.18)),
                 outline=rgb(accent), width=max(k, size * k // 28))
    return big.resize((size, size), Image.LANCZOS)


# -- the stage ------------------------------------------------------------------------------------

_grain = None


def grain(size):
    global _grain
    if _grain is None:
        try:
            _grain = Image.open(ASSETS / "grain.png").convert("L")
        except OSError:
            _grain = Image.new("L", (64, 64), 128)
    tile = Image.new("L", size)
    for x in range(0, size[0], _grain.width):
        for y in range(0, size[1], _grain.height):
            tile.paste(_grain, (x, y))
    return tile


def cover(image, size, anchor=(1.0, 0.0)):
    """`image` scaled to cover `size`, cropped around `anchor` (fractions: right, top by default)."""
    scale = max(size[0] / image.width, size[1] / image.height)
    image = image.resize((max(size[0], round(image.width * scale)), max(size[1], round(image.height * scale))),
                         Image.BILINEAR)
    x = round((image.width - size[0]) * anchor[0])
    y = round((image.height - size[1]) * anchor[1])
    return image.crop((x, y, x + size[0], y + size[1]))


def stage(size, art, base, accent, glow_at):
    """The backdrop (RGBA, `size`): `art` (the Collection's picture, or None) blurred and darkened
    towards the left, where the settings sit, `accent` glowing around `glow_at` (x, y), then grain."""
    w, h = size
    image = Image.new("RGB", size, base)
    if art is not None:
        # Blurred at a quarter of the size: the same look for a fraction of the time.
        small = cover(art, (max(1, w // 4), max(1, h // 4))).filter(ImageFilter.GaussianBlur(3))
        picture = small.resize(size, Image.BICUBIC)
        shade = Image.linear_gradient("L").rotate(90, expand=True).resize(size)  # 0 on the left
        shade = shade.point(lambda v: 250 - v * 105 // 255)  # 98% dark on the left, 57% on the right
        image = Image.composite(image, picture, shade)
    # radial_gradient is 0 in the middle and 128 half way to the edges: the glow fades out by then.
    glow = Image.radial_gradient("L").resize((int(w * 1.6), int(h * 2.0)))
    glow = glow.point(lambda v: max(0, 80 - v * 80 // 128))
    layer = Image.new("L", size, 0)
    layer.paste(glow, (int(glow_at[0] - glow.width / 2), int(glow_at[1] - glow.height / 2)))
    image = Image.composite(Image.new("RGB", size, rgb(accent)), image, layer)
    noise = grain(size).convert("RGB")
    image = Image.blend(image, ImageChops.overlay(image, noise), 0.5)
    return image.convert("RGBA")


def paste(target, image, x, y):
    """Alpha-composites `image` (RGBA) onto `target` (RGBA) at (x, y), in place."""
    target.alpha_composite(image, (int(x), int(y)))
