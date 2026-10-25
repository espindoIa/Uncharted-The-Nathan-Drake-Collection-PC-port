"""Builds the launcher's icon files (launcher/assets/uncharted.ico and icon.png) from the master
launcher/assets/app-icon.png: the fork's own mark, a compass on a night tile with the north point in
the launcher's flare red. The master is 1024 px, drawn in VectorCraft (vector source
launcher/assets/app-icon.svg, 25 Oct 2026), so every size down to 16 px is reduced from it, sharp on
4K screens. Needs Pillow:

    python tools/make_launcher_icon.py

The packaged starter (tools/windows_starter/starter.rc) embeds uncharted.ico when it is built.
"""
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
ASSETS = ROOT / "launcher" / "assets"


def main():
    master = Image.open(ASSETS / "app-icon.png").convert("RGBA")
    sizes = (16, 24, 32, 48, 64, 128, 256)
    images = [master.resize((size, size), Image.LANCZOS) for size in sizes]
    master.resize((512, 512), Image.LANCZOS).save(ASSETS / "icon.png", optimize=True)
    images[-1].save(ASSETS / "uncharted.ico", sizes=[(size, size) for size in sizes], append_images=images[:-1])
    print(f"Wrote {ASSETS / 'icon.png'} and {ASSETS / 'uncharted.ico'}")


if __name__ == "__main__":
    main()
