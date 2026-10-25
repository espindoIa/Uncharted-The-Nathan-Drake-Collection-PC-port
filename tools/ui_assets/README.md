# UI asset generator

Offline tool that redraws a game's packed font atlas (one glyph per BMFont channel) as vector outlines and
renders them at 4x. The runtime uses the result instead of the bicubic upscale at 1440p and 4K.

    python tools/ui_assets/make_font_atlas.py <atlas.tga> <fnt> <out prefix>

Needs Python 3.13 with numpy and Pillow. Writes `<prefix>_x4.png` (the atlas at 4x, same channels),
`<prefix>.svg` (the outlines, one `<path>` per glyph, grouped by channel) and `<prefix>_err.json`
(per glyph round-trip error). The channel map is fixed (BMFont bits 1/2/4/8 -> B/G/R/A).

Method (`glyphfit4.py`, helpers in `glyphfit.py`):

- 0.5 iso-lines of each glyph's coverage.
- Corners are kept only where the blurred bitmap still turns sharply; hinting steps on curves are not corners.
- Smooth G1 cubic Beziers (point, tangent angle, handle lengths per join), optimised with
  Levenberg-Marquardt against the exact area coverage of the glyph.
- Check: each glyph is rendered at 4x, box-filtered back to 1x and compared with the atlas. The script prints
  that error next to the error of the bicubic + contrast fallback the runtime uses otherwise.

Naming: assets are committed as `gow3_pc/assets/ui/fonts/<xxh3 hex of the guest atlas>.png`, the hash of
the atlas as the game loads it, so the runtime finds the replacement by content.
