# Uncharted Collection HDR tone replacement

Experimental HDR scene path for the Vulkan port. All three games use tone hash
`3371ae8d` and post-tone hash `484cdbf5`. The HDR output setting must be enabled.
DLSS/DLAA, FSR 3/4, TAA and the stretch with upscaling off receive the HDR scene.
At native resolution the launcher enables the same composition path at 1:1, and the
renderer drops it again when the display offers no HDR10. `UNCHARTED_HDR_NATIVE=0` opts out.
This needs the velocity camera (`UNCHARTED_MOTION_SOURCE=velocity`): with the fork's default flat
camera the launcher does not set `GOW3_RENDER_RES` (it would end the upscalers), the scene HDR
needs `VelocityMode()`, and only the HDR10 encoding of the SDR image exists.

Nothing of this is built with HDR off: the float-target pipelines, the tone pass
shadow and binding 12 exist only when HDR is on at start on an HDR10 display
(`hdr_pipelines`), so turning HDR on in the game applies to the scene after a restart.
The replacement matches the tone pass as translated for NVIDIA's driver (manual
interpolation through fragment shader barycentrics); on other GPUs, and on NVIDIA
GPUs without barycentrics (GTX 10xx), the scene stays SDR inside HDR10 output.

No game code is distributed. The games' tone pass pixel shader is Naughty Dog / Sony
Interactive Entertainment work, so neither its decompiled HLSL nor SPIR-V derived from it is in the
repository, the package or the public export (owner's decision, 26 Oct 2026; they were here until
then as `fs_3371ae8d_hdr.hlsl` / `.spv`, and `tools/package_windows.sh`, `tools/export_github.sh` and
`.gitignore` now refuse any `fs_*_hdr.*`). What is here is the port's own part:

- `hdr_tone.hlsl`: the epilogue, `HdrTone(scene, neutral, color, filmic, c0, c1, c4, c5, c6)`.
  RenoDX tone mapping of the pre-tonemap scene value, the port's mid-grey anchoring of the filmic
  curve's gain, and the gamma 2.2 encoding the next pass expects. It reads the live settings from
  binding 12.
- `hdr_tone.spv`: that as a SPIR-V library (Linkage export `HdrTone`), built by `build.ps1`. It
  carries no capability beyond Shader and Int16. `gow3_pc/build.sh` copies it next to the Windows
  executable under `out/hdr/`; the Windows package carries it and the RenoDX license under
  `licenses/renodx/`. `GOW3_HDR_SHADER_DIR` overrides the directory. It is read once per process.

The game's side is made at run time from the player's own files. The shader recompiler translates
the tone pass (guest hash `3371ae8d`, the same in all three games) from the game, as for any
shader. `gpu/shim/gow3_hdr_tone.cpp` remembers that SPIR-V when the module is created
(`CompileSPV`) and, when the pipeline needs its HDR shadow, patches it in memory and links
`hdr_tone.spv` into it (SPIRV-Tools `Link`):

1. It finds the filmic branch: the three phis that merge the curve's clamped result with the scene
   value, the branch condition, and the curve's constants (c0, c1, c4, c5, c6) and input.
2. Phis at that merge give the epilogue the curve's input (or the scene value when the branch is not
   taken) and the constants; a call to the imported `HdrTone` replaces the three color stores'
   values. Everything else of the translated shader is untouched, so the guest bindings 0-11, push
   constants and stage I/O are those of the normal pipeline.
3. After the link the entry point's interface lists the settings buffer (SPIR-V 1.4+).

Nothing is written to disk. If the player's game is not what was expected (no match: another
recompiler version, the non-barycentric interpolation path, a different shader) the log says
`HDR: no scene HDR (<why>); HDR10 output of the SDR scene` and the scene stays SDR inside HDR10
output, exactly as on GPUs the replacement does not support.

The pipeline then has 12 native descriptors (0-11, the translated shader uses 0-9) plus binding 12,
a read-only byte-address buffer containing two floats: peak nits at byte 0 and game-white nits at
byte 4. The port uploads current slider values for each tone draw, so no restart or pipeline rebuild
is required for brightness.

Rebuild the epilogue from PowerShell with an existing DirectX Shader Compiler (the Vulkan SDK's
`Bin` has it and the SPIRV-Tools the script needs):

```powershell
./gow3_pc/hdr/build.ps1 -Dxc C:/VulkanSDK/<version>/Bin/dxc.exe
```

The `renodx/` directory is the local quoted-include dependency closure of
`draw.hlsl`, unchanged, from the commit recorded in `renodx/SOURCE.txt`.
It carries the original MIT license. No external checkout or download is needed
to rebuild the epilogue.

Tests: `hdr-tone-test` (CMake target; patch and link on a stand-in shaped like the translation,
validated with the Vulkan 1.3 rules; `GOW3_HDR_GUEST_SPV=<a translated 0x3371ae8d, e.g.
`0x000000003371ae8d_0.spv` from the local shader cache>` runs the real one too,
`GOW3_HDR_SHADOW_OUT=<file>` writes the result) and `gow3_pc/tests/test_hdr_shader.py` (the
epilogue's ABI and that no game shader is in this folder).

Diagnostic staging: `GOW3_HDR_STAGE=1` draws the tone shadow; `2` adds the scene
shadow; `3` (default) also sends it through the selected upscaler or stretch.
`GOW3_HDR_TRACE=1` logs composition formats. U1/U3 gameplay, movies and display
changes still need validation; title-screen captures alone do not cover them.
