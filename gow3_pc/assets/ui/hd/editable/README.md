# Editor sources, 26 October 2026

## Current fidelity revision

The 60 HD sources remain the build inputs. This revision changes ten PNGs,
without another global sharpness filter. `../inventory.csv` records each source,
original dump name, shared hash, review result and on-screen verification limit.
The local review gallery is `analysis/ui-refinement/index.html`: original,
preceding HD and current art at equal size on the same background. Dump paths
in the inventory describe local references, not files required by the build.

### Controller cross

`cross-overlay.vectorcraft` contains two editable diagonal paths, (856,388) to
(936,468) and (936,388) to (856,468), on a 2048-square transparent artboard.
The main stroke is 10 px, round caps, with a linear gradient from #8b9bff at
(896,382) to #6c79dc at (896,474). Its restrained outer glow is a 16 px stroke
#6c78e6 at 16% opacity. Positions and all other atlas cells stay fixed.

In PhotoCraft, remove only the preceding cross with a lasso and Content-Aware
Fill, then place the vector export at (1024,1024), scale 100%. Transfer the
finished merged RGB into each preceding atlas within rectangle (836,373),
116x108, using Apply Image, Normal 100%, Preserve Transparency. This last step
is necessary: Content-Aware Fill otherwise changes a few interior alpha bytes.
`buttons-finish.pcraft` retains the finished U1 document. U2 uses the same RGB;
transfer it into the U3 colour sheet while keeping that sheet's alpha opaque.
The separate U3 alpha sheet is unchanged and is reviewed with colour combined.

### World triangle

`triangle-material.pcraft` removes the previous pointed triangle within the
disc. `triangle-rounded.vectorcraft` embeds that material and draws
`M336 396 L688 396 L512 700 Z`, round joins/caps: dark #111811 outline 34 px,
green stroke 24 px. The vertical green gradient runs from (512,385) to
(512,712), with stops 0 #34da69, 0.5 #2bd25e and 1 #39d96a.
`triangle-rounded-finish.pcraft` is the 1024-square export document. The disc's
orientation, outer field and scale are retained. The hash is shared by all
three games: build one DDS, not one duplicate per game.

### Weapon and grenade materials

The five `*-material.pcraft` documents retain the previous geometry and apply
PhotoCraft Levels per RGB channel, with unchanged black/white points:

| Game / hash | R gamma | G gamma | B gamma |
|---|---:|---:|---:|
| U1 / 59bbd33997fa57a3 | 1.12 | 1.08 | 1.03 |
| U2 / ceaa8da3c8a5b66d | 0.89 | 0.89 | 0.89 |
| U2 / 38e5a4d1f9b184bc | 0.87 | 0.87 | 0.85 |
| U2 / 0f32552961787064 | 0.85 | 0.86 | 0.85 |
| U3 / 96b0df09e814afe9 | 1.15 | 1.15 | 1.15 |

### Foliage material

`ornament-material.pcraft` finishes an image-generation reconstruction guided
by the original and the preceding HD ornament. The request preserves the
framing, grayscale foliage relief, dark background, spacing and soft fade;
it adds no lettering or new objects. The 1254-square generated image is
desaturated and resized once in PhotoCraft to 2048-square with Lanczos. This
restores relief missing from the previous flat painting; generated small
leaf details are an approximation, not an exact vector trace of the original.

Build all DDS with `python tools/build_ui_art.py hd`. Never apply the recipes
again to the finished PNGs. The 23 intentionally soft companion textures and
five native 2048-wide title-logo textures have no new DDS; their originals are
explicitly retained. Thirteen known prompt hashes still lack collected
references and are not counted as completed HD art.

Cross, circle and OPTIONS were visually checked in menus in all three games.
Further comparisons and game tests were deferred by the owner: remaining glyphs,
matched HD weapon/HUD captures and world prompts are not accepted on screen.
The inventory records loaded-but-unseen textures separately from observed art.
Native editor files are marked binary in `.gitattributes`: embedded image blobs
must not undergo Git line-ending conversion or text merging.

## Historical first editor pass

The following recipes describe the preceding revision. Its native projects
remain as history; use the current projects above for further editing.

The triangle prompt is finished in the real VectorCraft and PhotoCraft editors.
`triangle-rim.vectorcraft` embeds the existing HD painting and adds one editable
ellipse stroke over the rim: x 246, y 240.4, 540 x 540, weight 16, opacity 32%.
The stroke gradient runs from (650, 300) to (350, 740), with stops
0 #191d19, 0.48 #353c35, 0.82 #737e73 and 1 #889188. It softens the excessively
wide black rim without moving the outer silhouette or changing the green field.

Export the 1024 x 1024 artboard to PNG, open it in PhotoCraft and apply Unsharp
Mask: amount 35%, radius 0.6 px, threshold 3. `triangle-finish.pcraft` keeps that
finished document. Export to `../src/u1/5db884aa1bae54f9_256x256_Bc1RgbaUnormBlock.png`.

The U1/U2 button sheets and U3 colour sheet receive one PhotoCraft Unsharp Mask
pass: amount 45%, radius 0.65 px, threshold 3. Apply once to the preceding
revision, never repeatedly to the finished PNG. The alpha bytes stay identical;
the U3 alpha sheet is unchanged.

The U1/U2 1024 x 512 guest menu masks (`1e90f519716fb1e7`, `07d768602fb4ff87`)
are resized from the preceding 2048 x 1024 masters to 4096 x 2048 using
PhotoCraft Lanczos, then Unsharp Mask 25%, radius 0.55 px, threshold 4.
This preserves their existing worn silhouettes; it does not invent new detail.

Run `python tools/build_ui_art.py hd` from the repository root to generate DDS
files. These are incremental refinements of the existing HD art, not new
from-scratch paintings. In-game acceptance of this editor pass is pending.
