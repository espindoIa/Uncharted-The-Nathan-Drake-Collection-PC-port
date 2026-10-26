# Editor sources, 26 October 2026

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
