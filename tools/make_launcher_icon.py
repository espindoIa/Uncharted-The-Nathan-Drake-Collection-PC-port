"""Draws the launcher's icon (launcher/assets/uncharted.ico and icon.png): the fork's own mark, a
compass on a night-blue tile with the north point in signal-flare orange, matching the launcher's
palette (BG, ACCENT in launcher/uncharted_launcher.pyw). Needs Pillow:

    python tools/make_launcher_icon.py

The packaged starter (tools/windows_starter/starter.rc) embeds uncharted.ico when it is built.
"""
import math
from pathlib import Path

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent.parent
ASSETS = ROOT / "launcher" / "assets"

TILE = (11, 17, 22)          # BG
TILE_EDGE = (34, 52, 63)     # LINE
RING = (142, 163, 166)       # MUTED
NEEDLE = (255, 138, 61)      # ACCENT
NEEDLE_DARK = (196, 92, 28)
SOUTH = (230, 238, 236)      # TEXT
SOUTH_DARK = (150, 166, 168)


def draw(size):
    scale = 4  # drawn large, then reduced: smooth edges at every size
    s = size * scale
    image = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(image)
    radius = s * 0.22
    d.rounded_rectangle((0, 0, s - 1, s - 1), radius, fill=TILE, outline=TILE_EDGE, width=max(scale, s // 64))
    c = s / 2
    ring = s * 0.36
    d.ellipse((c - ring, c - ring, c + ring, c + ring), outline=RING, width=max(scale, int(s * 0.035)))
    # Ticks at the four quarters, inside the ring (dropped at small sizes, where they blur).
    if size >= 32:
        for angle in range(0, 360, 90):
            a = math.radians(angle)
            inner, outer = ring * 0.80, ring * 0.96
            d.line((c + inner * math.cos(a), c + inner * math.sin(a), c + outer * math.cos(a),
                    c + outer * math.sin(a)), fill=RING, width=max(scale, int(s * 0.025)))
    # The needle, tilted like a bearing being taken: two halves per point give it a ridge.
    tilt = math.radians(-35)
    length, half = s * 0.40, s * 0.085

    def point(distance, angle):
        return (c + distance * math.sin(angle), c - distance * math.cos(angle))

    tip_n, tip_s = point(length, tilt), point(length, tilt + math.pi)
    left, right = point(half, tilt - math.pi / 2), point(half, tilt + math.pi / 2)
    d.polygon([tip_n, left, (c, c)], fill=NEEDLE)
    d.polygon([tip_n, right, (c, c)], fill=NEEDLE_DARK)
    d.polygon([tip_s, left, (c, c)], fill=SOUTH_DARK)
    d.polygon([tip_s, right, (c, c)], fill=SOUTH)
    hub = s * 0.045
    d.ellipse((c - hub, c - hub, c + hub, c + hub), fill=TILE, outline=SOUTH, width=max(1, scale))
    return image.resize((size, size), Image.LANCZOS)


def main():
    sizes = (16, 24, 32, 48, 64, 128, 256)
    images = [draw(size) for size in sizes]
    images[-1].save(ASSETS / "icon.png")
    images[-1].save(ASSETS / "uncharted.ico", sizes=[(size, size) for size in sizes],
                    append_images=images[:-1])
    print(f"Wrote {ASSETS / 'icon.png'} and {ASSETS / 'uncharted.ico'}")


if __name__ == "__main__":
    main()
