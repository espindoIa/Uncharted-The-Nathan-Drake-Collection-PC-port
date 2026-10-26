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
- Curve preparation uses a 0.4 source-pixel Gaussian and a 0.08-pixel initial fit tolerance
  (26 October fidelity revision, previously 0.5 / 0.12). This reduces rounding before the
  coverage fit; corner detection, glyph positions, weights and channel packing stay unchanged.
  The exported atlas uses area coverage, without a finishing blur or global sharpness filter.
- Check: each glyph is rendered at 4x, box-filtered back to 1x and compared with the atlas. The script prints
  that error next to the error of the bicubic + contrast fallback the runtime uses otherwise.

Naming: assets are committed as `gow3_pc/assets/ui/fonts/<xxh3 hex of the guest atlas>.png`, the hash of
the atlas as the game loads it, so the runtime finds the replacement by content.

Rebuild the three main-font masters from the retained original references, from the repository root:

| Game | Atlas / BMFont under `gow3_pc/assets/ui/fonts/` | Hash / output prefix |
|---|---|---|
| U1 | `src/u1/game3_00.tga` / `src/u1/game3.fnt` | `b4337ad64e659f14` |
| U2 | `src/u2/main_00.tga` / `src/u2/main.fnt` | `188453145becdc37` |
| U3 | `src/main_00.tga` / `src/main.fnt` | `35ce544569cac222` |

Give the generator an output prefix under `analysis/`, then install its `_x4.png` as
`fonts/<hash>.png` and its `.svg` as `fonts/<hash>.svg` after it finishes successfully.
U1/U2 masters are 1024x512; U3 is 1024x1024. The game keeps its original BMFont metrics,
kerning, vertex positions and shadows. The launcher font, classic/Russian/CJK fonts and
the original atlases are not rewritten. Further game tests and visual comparisons of this
revision are deferred at the owner's request.
The retained U1/U2 BMFont descriptors have trailing whitespace normalized; their
glyph metrics and kerning values are unchanged.
