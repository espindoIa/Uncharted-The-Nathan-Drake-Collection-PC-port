# AGENTS.md

This file is the guide for any coding agent (Codex, Claude Code, etc.) working in this repository.

PC port of the PS4 *Uncharted: The Nathan Drake Collection* (US CUSA02320 or Europe CUSA02344: U1 Drake's Fortune, U2 Among
Thieves, U3 Drake's Deception). The games' own x86-64 code runs natively; a C runtime supplies the PS4
system libraries (HLE) and a shadPS4-derived Vulkan renderer executes the PS4 GPU work. Windows only
in practice (MSYS2 CLANG64 toolchain). The runtime began as the gow3_pc project, so internal names
stay `gow3` (`gow3-probe.exe`, `GOW3_*` variables, `gow3_*` files) — that is not God of War code
unless it sits behind the game-hooks gate (below).

## Agent rules

- Never commit `gow3_pc/stat-multipliers-test.ini` or the dirty submodule `gow3_pc/gpu/third_party/fsr-vulkan`. Stage files by name.
- No game files, dumps, saves or logs in commits (`export_github.sh` refuses them too). One exception, set by the owner on 25 Oct 2026: the games' UI art (font glyph outlines, button icons, HUD shapes as SVG, and the source textures, atlases, TGA/PNG extracts and files taken out of pak/psarc archives that they come from) may be committed, only under `gow3_pc/assets/ui/`. Still out, everywhere: executables and ELF/eboot images, PKGs, whole pak/psarc archives, saves, dumps, logs, shader caches. `README.md` states the exception; `CONTRIBUTING.md`, the PR template and `export_github.sh` also exempt `gow3_pc/assets/ui/` from their file checks. What the tree holds now: the launcher's font and wordmark (`tools/build_ui_art.py`), the games' main-font atlases (`tools/ui_assets/`) and HD UI art (`assets/ui/hd/`).
- Only one game instance at a time (the player may be playing too).
- Push to `origin` only when explicitly asked; committing locally is fine.

**Locations.**
- The repo is `C:\dev\uncharted` (a real folder since 19 Oct; the old `Downloads\Projetos\Uncharted-PC-Port` copy and junction are gone). The CMake cache in `gow3_pc/out/gpu` was generated at this path.
- The playable port is in `Downloads\Jogos_e_Mods\Jogos\Uncharted-PC-Port`: `Uncharted\` is the extracted package (the player opens `Uncharted Launcher.exe` there), `Jogo\CUSA02320\` the game (US) with `Jogo\CUSA02320-patch\` its update 1.02 (since 19 Oct; earlier Europe's CUSA02344 1.00). Keep only this one Uncharted folder (the user asked, 19 Oct): no old installs, test copies or game PKGs elsewhere.
- Scripts outside the repo, in `C:\dev`: `update_port.sh` installs `dist/Uncharted-windows.zip` as the player's `Uncharted\` (replaces the old one; `analysis/uncharted-user` with saves and the player's `nvngx_dlss.dll` carried over). `pkgclean.sh` boots U3 from the zip extracted into an empty folder (delete that folder afterwards). `bench/` holds the benchmark and frame-pacing scripts (`bench.sh <u1|u2|u3> <name>`, `pace.sh`, `timeline.py`). Close the player's launcher/game before `update_port.sh`.
- Remotes:
  - `origin` is the fork, `espindoIa/Uncharted-The-Nathan-Drake-Collection-PC-port`, and the work is on `main` (the former `claude/dlss-uncharted` branch was merged into it and removed, 19 Oct).
  - `upstream` is BilalReloaded's repo. Its releases up to 0.6.0 are merged (0.4.1/0.5.0 on 21 Oct, 0.6.0 on 24 Oct); each merge's choices are in its STATUS section ("Fork (espindoIa) notes", "Upstream (BilalReloaded) 0.4.1 / 0.5.0 notes", "Upstream 0.6.0 merged").
- Both sides patch some of the same game bytes. When taking more upstream work, look for duplicate patch tables in `tools/probe_uncharted.py`: a patch applied first keeps a later one's signature from matching.

## Commands

The toolchain is MSYS2 CLANG64. From Git Bash, wrap commands like this:

```bash
/c/msys64/usr/bin/bash.exe -lc "export MSYSTEM=CLANG64 PATH=/clang64/bin:/usr/bin:\$PATH; cd /c/dev/uncharted/gow3_pc && ./build.sh"
```

- **Build the runtime:** `gow3_pc/build.sh` → `gow3_pc/out/gow3-probe.exe` (CMake + Ninja in `out/gpu`). On failure read `gow3_pc/out/gpu-build.log`; the script prints little.
- **C/C++ unit tests:** CMake targets marked `EXCLUDE_FROM_ALL` in `gow3_pc/gpu/CMakeLists.txt` (e.g. `movie-hook-test`, `scene-resolution-test`, `hdr-session-test`). Tests that use the temp directory (`flight-test`, `pipeline-binary-cache-test`, `hd-texture-test`) need `TMP`/`TEMP` set to a Windows path in the CLANG64 shell. Build and run one under the CLANG64 PATH (the exes need its DLLs): `ninja -C out/gpu movie-hook-test && ./out/gpu/movie-hook-test.exe` from `gow3_pc`.
- **Python tests:** `unittest` files in `gow3_pc/tests/` that import `paths.py`; run one from that folder, e.g. `python -m unittest test_prepare`.
- **Launcher syntax check:** `python -c "import ast; ast.parse(open('launcher/uncharted_launcher.pyw', encoding='utf-8').read())"`.
- **Launcher tests:** from `gow3_pc/tests`, `python -m unittest test_uncharted_launcher_ui` (Tk windows open; about a minute).
- **Tools tests (upstream's, 26 Oct):** from `tools/`, `python -m unittest test_steam_library test_uc2mp_extract_spawns test_uc2mp_lightmaps test_uc2mp_local_session test_uc2mp_stage_wrapper`; `test_uc2mp_lan_processes.py` is a script (`ninja -C out/gpu uc2mp-lan-peer` first, put the CLANG64 `bin` on PATH, `--logs <dir>`) and `test_uc2mp_stage_control.py` needs a running offscreen instance. C targets `uc2mp-test`, `uc2mp-menu-test`, `uc2mp-lan-test`, `uc2mp-config-test`, `uc2mp-tick-test`, `uc2mp-spawn-test`; `videodec2-test` must run from `gow3_pc` (relative data path).
- **Run from source:** `Start-Uncharted.cmd` (launcher), or `python tools/probe_uncharted.py --boot --seconds 0 --executable big3-ps4_Shipping.elf --game <CUSA02320 or CUSA02344 folder>` (`eboot.bin` = U1 / Collection menu, `big2-…` = U2, `big3-…` = U3). Logs go to `analysis/<executable>/boot.log`.
- **Package:** run from Git Bash, not the CLANG64 shell. In CLANG64 it picks MSYS2's Python, and Python copying fails. It needs a Windows Python 3.10+ with Pillow, by default the `python` on PATH (3.13 here since Python 3.12 was uninstalled, 25 Oct; `PYTHON_HOME` overrides):
  `DLSS_BRIDGE=/c/dev/uncharted/gow3_pc/out/gow3_dlss.dll DLSS_SDK=/c/dev/DLSS STREAMLINE_SDK=/c/dev/Streamline-bin/sdk tools/package_windows.sh` → `dist/Uncharted-windows.zip`. The pacman "Import PGP key?" lines in its output are noise. The zip must contain `gow3_dlss.dll` and must **not** contain `nvngx_dlss.dll` (NVIDIA's license does not fit the GPL; the launcher has the player add it). With `STREAMLINE_SDK` it also holds Streamline's `sl.*.dll` (MIT) but never its `nvngx_*` or `NvLowLatencyVk.dll`. It also carries `gow3_pc/assets/ui/` (the launcher's lettering and the three font atlases) and the launcher's icons. Test it extracted into an empty folder.
- **Release / public sync:** `tools/release.sh <version>` (needs `tools/release-notes/v<version>.md`); `tools/export_github.sh` copies source only (no game files) into a separate repo for GitHub Actions (`.github/workflows/build-windows.yml`).
- **Other tools:** `gow3_pc/scripts/verify_dump.py <game folder>` checks a dump or PKG extraction for zero-filled or random-byte damage before `prepare` (read-only; exit 0 clean, 1 damage; test `test_verify_dump`, from the qequ fork, 26 Oct); `tools/session_recorder.py` records a play session for bug reports (`Record-Session.cmd` starts it); `tools/hangdump/` captures a hung game (`hangdump.c`) and symbolizes its dump (`symbolize.py`); `tools/ui_assets/` builds the vector font atlases and `tools/build_ui_art.py` the launcher's font and wordmark from the games' files; `tools/windows_starter/` is the launcher `.exe`; `tools/pkgtool/` is LibOrbisPkg's `PkgTool.exe` (used by `extract_metadata.py` and `install_update.py`); `tools/github/` and `tools/github-overlay/` are the public repo's README/CHANGELOG that `export_github.sh` lays over the runtime's.

## Architecture

**Launch chain.**
1. `launcher/uncharted_launcher.pyw` (Tkinter) writes per-game settings to `analysis/uncharted-user/probe-u{1,2,3}.ini`.
   Its lettering is the games' own (`gow3_pc/assets/ui/`, built by `tools/build_ui_art.py` with a manual VectorCraft trace step): the menus' font as `drake-serial.ttf`, loaded privately by `art.load_game_font()` before Tk starts, and the Collection logo's UNCHARTED as `wordmark.svg`, rasterised at the exact size per screen scale by `art.svg_mask()`. Missing files fall back to Segoe UI / Bahnschrift and plain text.
   `launcher/art.py` draws the panels and icons at 4x and reduces them (`rounded_image`, `icon`), `launcher/widgets.py` holds the shared widgets, and `launcher/updater.py` the self-update.
2. `tools/probe_uncharted.py` prepares the executable with `gow3_pc/scripts/` (patches, render-resolution rewrite, update 1.02 overlay). It turns the ini into `GOW3_*` environment variables and starts `gow3-probe.exe`. Some choices are decided there at start, which is why they need a restart: an upscaler on or off (camera source, scene size), the render resolution, readback modes.
3. `UNCHARTED_*` variables and `analysis/uncharted-user/test-env.txt` override what it sets; `test-env-u1.txt` (`-u2`, `-u3`) after it, for one game only (since 26 Oct, from the qequ fork).

**Runtime (`gow3_pc/src/`).**
- `probe.c` is the loader.
  - It maps the prepared guest image and resolves PS4 imports to the `runtime_*.c` HLE libraries (memory, threads, files, saves, audio, pad, video decode, services).
  - Unsupported imports stop with "first unsupported PS4 import".
- Guest code patches are byte signatures, not fixed addresses (`*_hook.h`, e.g. `movie_hook.h`). `probe.c` finds the site, checks the original bytes, and writes a `call`/`jmp` to a stub.
  - Stubs must sit within ±2 GiB of the site: use `allocate_near()`. The image is only loaded low with `--low-image`.
  - Uncharted runs with `--no-game-hooks`. Hooks inside that gate (cheats, orbs, health bar) are God of War 3 leftovers. Uncharted fixes, like cutscene pacing, are installed outside it.

**GPU library (`gow3_pc/gpu/`).**
- `shadps4/` is the shadPS4 video core: command processor, shader recompiler, Vulkan renderer, buffer and texture caches. It is modified in place and committed in this repo (not a submodule).
- `shim/` holds the port's own pieces:
  - `gow3_settings.{h,cpp}`: settings as atomics read every frame. It loads the ini and `GOW3_*` overrides, and `Save()` merges with keys the launcher changed meanwhile.
  - `gow3_overlay.cpp`: the ImGui in-game menu (Insert / R3+L2) and the performance overlay.
  - `gow3_reflex.{h,cpp}`: NVIDIA Reflex (VK_NV_low_latency2; settings `reflex`, `frame_gen`).
  - Also window and SDL, Videodec2, fibers and movie scaling.
- Upscalers live in `shadps4/video_core/renderer_vulkan/vk_temporal_upscaler.cpp` (FSR 3.1, FSR 4, TAA, DLSS). DLSS goes through the optional MIT bridge `gpu/dlss_bridge` (`gow3_dlss.dll`), loaded at run time next to `nvngx_dlss.dll`.
- Font atlases: `ImageInfo::host_scale` (`texture_cache/image_info.h`) lets the texture cache back the three main font atlases with a larger host image (1x at 1080p and below, 2x up to 1920 rows, 4x above). `shim/gow3_atlas.cpp` recognises them by the XXH3 of the guest texels and fills that image from `gow3_pc/assets/ui/fonts/<hash>.png`, the glyphs redrawn as vector at 4x and validated against the guest atlas (missing or mismatched: bicubic + contrast). The games' text shader (default hash `2e403aa4`, `GOW3_TEXT_SHARPEN`) gets an edge remap in `shader_recompiler/ir/passes/text_sharpen_pass.cpp`. The assets are built offline by `tools/ui_assets/make_font_atlas.py` and shipped; `GOW3_ATLAS_LOG=1`, `GOW3_ATLAS_SCALE=1` (off), `GOW3_ATLAS_ASSETS=0` are the A/B switches.
- DLSS Frame Generation: Vulkan side `vk_dlss.*` and `vk_presenter.cpp` (`PresentGenerated`); bridge ABI 3; `nvngx_dlssg.dll` is supplied by the player (never packaged). NGX 310.9.16 has no application-side FG model selector (model / Preset B only through the NVIDIA App), so the port has none.
- NVIDIA Streamline 2.14.16 is the only player-facing path for DLSS, FG and Reflex (no setting; `GOW3_NV_BACKEND=ngx` turns it off for A/B tests). `shim/gow3_streamline.{h,cpp}` loads the signed `sl.interposer.dll` and is the only file that includes the vendored MIT headers (`gpu/third_party/streamline`). It is not loaded without `nvngx_dlss.dll` or an NVIDIA adapter, and it is dropped before vkCreateDevice when the chosen GPU is not NVIDIA's. In all those cases, and when it fails to load, the NGX bridge and the direct VK_NV_low_latency2 Reflex run. The release DLLs are in `C:\dev\Streamline-bin\sdk\bin\x64` (copy `sl.*.dll` into `gow3_pc/out` for source runs).
  - Streamline's Frame Generation only interpolates while the game window has the focus ("DLSS-G disabled: window not focused"). Automated FG sessions need `FOCUS=1` (`C:\dev\bench\focusgame.ps1`).
  - `GOW3_SL_LOG=1` writes Streamline's log, `=2` is verbose. Benign warnings: "CmdBindPipeline is NOT supported", "PCL marker 13 has no VK_NV_low_latency2 equivalent", "VSync interval 1 not supported with FG".
- A new setting usually touches four places: `gow3_settings.h`/`.cpp` (parse in `Set`, write in `Current`), the menu, the launcher, and possibly `probe_uncharted.py`.

**GPU pipeline and flips.** The GPU command thread parses PM4 packets, a draw recording thread (DrawPipe) prepares draws, and a recorder thread writes Vulkan commands. Flips go `VideoOutDriver::SubmitFlipInternal` → `PrepareFrame` → present thread (frame limiter) → swap thread (`vk_swapchain.cpp`); the guest-visible flip waits for its fences separately (`shadps4/core/libraries/videoout/driver.cpp`).

**Runtime toggles and diagnostics.**
- `analysis/uncharted-user/disabled-optimizations.txt` (`GOW3_TOGGLE_FILE`) is read live: "<first> <second>" bit masks, bit set = optimization off. The bits are the enums in `gpu/shim/gow3_toggles.h` (`Disabled()` / `Disabled2()`); new performance work goes behind a new bit so it can be A/B'd in one run. Reflex/FG bits (`Disabled2`): `ReflexSleep`, `FrameGeneration`, `ReflexNoSleep`, `FrameGenStdSleep`, `ReflexEarlySubmit`. `Disabled2` bit 52 `MotionShaderCache` (25 Oct): set = motion-vector vertex shaders and their pipelines stay out of the disk cache (old behaviour); unit test `motion-addresses-test`.
- HD UI textures (25 Oct): `GOW3_TEX_DUMP=<dir>` dumps the UI art textures the games upload (PNG or raw `.bin`, `textures.csv`), named `<hash16hex>_<w>x<h>_<vkformat>` by the hash of mip 0's guest bytes; `<hash16hex>_<w>x<h>.dds` (the dump's name without the format; the guest size is required) in `GOW3_HD_TEXTURES` (else `<exe dir>/hd-textures`, else `gow3_pc/assets/ui/hd`) replaces that texture with a whole-multiple-size image (`shim/gow3_hdtex.cpp`, `Image::EnsureHdBacking`; details in `analysis/STATUS.md` "HD UI textures"). Setting `hd_ui` / `GOW3_HD_UI`; `Disabled2` bit 48 `HdUiTextures`. Test: `hd-texture-test`. `GOW3_HD_LOG=1` lists the textures that have a file for their size but a different hash. The replacement keeps the file's whole mip chain and `Rasterizer::WidenSamplerForHdMips` gives samplers of unmipped images the full LOD range (26 Oct). The art is in `gow3_pc/assets/ui/hd/src/<game>/<dump name>.png|svg` (committed; PNG at 4x the guest size, or 2x/1x for big ones); `python tools/build_ui_art.py hd [--only <hash prefix>] [--preview]` (needs numpy with Pillow) turns it into the generated, git-ignored `gow3_pc/assets/ui/hd/u1|u2|u3/*.dds` (mips, BC1/BC3/BC4 encode; `--preview` writes original-vs-HD sheets to `analysis/ui-hd-preview/`); the package script runs it. Test: `gow3_pc/tests/test_build_ui_art_hd.py`. Prompts are keyed by device: test with `GOW3_PROMPTS=playstation` (a scripted pad counts as keyboard otherwise, and the HD controller art is skipped on purpose).
- Ultrawide HUD (U2; 26 Oct, from feros's fork, upstream PR #191): setting `hud` / `UNCHARTED_HUD` (`16:9` default, `wide`, `stretch`) and `hud_wide_aspect` / `UNCHARTED_HUD_WIDE_ASPECT` (`screen`, `21:9`, `32:9`); `probe_uncharted.py` turns them into `GOW3_HUD_ASPECT`, `GOW3_HUD_MODE`, `GOW3_HUD_WIDE_ASPECT`, `GOW3_HUD_COMPOSITE_PS`, `GOW3_UI_TONEMAP_PS` and `GOW3_HUD_TRACE=1` (HUD draw counts and shader extents in the log). `renderer_vulkan/hud_layout.h` decides per draw by its vertex extent; no game bytes are patched. Test: `hud-layout-test`.
- Batched deferred fences (26 Oct, from BilalReloaded's `meme-igpu` branch; opt-in, unvalidated): `GOW3_COALESCE_FENCES=1` (launcher Performance -> "Batch GPU fences", `coalesce_fences`, `UNCHARTED_COALESCE_FENCES`) records the draw thread's compute-release/end-of-pipe fences in groups of up to 64 (`Scheduler::FlushDeferredSignals`, `DrawPipe::FlushIfCaughtUp`); `Disabled2` bit 53 `CoalescedFences`, set = per-fence path (the branch used first-word bit 56, which is `RingReuse` here). Needs the draw pipeline (multithreaded rendering).
- HDR10 output (26 Oct, moistman7's work, upstream PR #156): setting `hdr` / `GOW3_HDR_OUTPUT` (default off) with `hdr_peak_nits`, `hdr_game_nits`, `hdr_ui_nits` (in-game Image tab, launcher Display). On a display that offers HDR10 (`GOW3_HDR`, default on, only offers it) the swapchain is A2B10G10R10 / Rec. 2020 PQ and the post pass encodes the SDR image (`post_process.frag`, `hdr_common.glsl`); the menu overlay is blended on at the HUD brightness. The scene HDR (`gow3_pc/hdr/`: the games' tone pass `3371ae8d` replaced by a RenoDX-based shader, checked-in SPIR-V that `build.sh` copies to `out/hdr/`; `GOW3_HDR_SHADER_DIR`, `GOW3_HDR_STAGE`, `GOW3_HDR_TRACE`) and the float HUD chain exist only with HDR on at start (`hdr_pipelines`); at the game's own size `probe_uncharted.py` (`native_hdr_composite`, `UNCHARTED_HDR_NATIVE=0` opts out; only with `UNCHARTED_MOTION_SOURCE=velocity`, since with the default flat camera `GOW3_RENDER_RES` would end the upscalers, and scene HDR needs the velocity camera) sends the frame through the upscaling path at 1:1 (`GOW3_COMPOSITE_NATIVE`), and `vk_swapchain.cpp` removes those variables when there is no HDR10. Pure choices are in `renderer_vulkan/hdr_session.h` (`hdr-session-test`, `tools/test_hdr_launcher.py`, `gow3_pc/tests/test_hdr_shader.py`, `test_hdr_composition.py`). Not verified on a real HDR display; no `Disabled2` bit.
- `GOW3_FRAME_LOG=<file>` logs flip events (S/P/Q/C, steady-clock ms); `GOW3_DISPLAY_LOG=<file>` logs on-screen times via VK_KHR_present_wait plus `<file>.gpu` GPU completion; `GOW3_FRAME_TIMES`, `GOW3_FRAME_STATS=1` add more.
- `tools/play.py` drives a game from scripted input (replays, `--burst`, `--marks`) for closed-loop tests. It and the bench scripts need a Python with Pillow first on PATH (3.13, the only one installed since 25 Oct; without PIL: "No module named PIL", "no gameplay").
- CPU experiments (23 Oct, uncommitted work): `Disabled2` bits 28–34 are `GuestFalseSharing`, `GuestReadPrewait`, `ForwardedCopies`, `RingAssociative`, `ComputeSpecScratch`, `ComputeVariantCheck`, `CommandsOnStageB`. Forwarded copies additionally require `GOW3_FORWARD_COPIES=1`; stage-B commands require `GOW3_COMMANDS_STAGE_B=1` (both opt-in). Earlier 23 Oct gate benchmarks were **not** in the meat market: walk backwards from the save to reach it. `C:/dev/bench/retest_cpu.py` logs external CPU load per short comparison window; comparisons under unequal load are inconclusive.
- Structural prototypes (23 Oct): exact pre-structural checkpoint `c48f91f`, matching exe `C:/dev/bench/cpu_before_structural_20261023.exe`. Disabled2 bits 35/36/37 are `StaticMeshSnapshots` / `CoalescedSkinningWrites` / `ForwardCopyDefer`, requiring `GOW3_STATIC_MESH_SNAPSHOTS=1` / `GOW3_SKINNING_WRITE_BATCH=1` / `GOW3_FORWARD_COPY_DEFER=1` (last also needs forwarded copies). All opt-in. At 4K DLAA M, `C:/dev/bench/market_bottleneck1` has updated CPU/GPU/timeline evidence: not CPU saturation across twelve threads; solver-matrix readback dependencies and GPU/DLAA cost remain. Do not claim the older CPU-bound profile is the 4K bottleneck ranking.

- Upstream 0.7.0 additions (26 Oct): `GOW3_FLIP_AFTER_GPU=1` (flips complete after the frame's GPU work; `CompleteFlip`), `GOW3_MAX_QUEUES=1..3`, `GOW3_HEADLESS=1` (offscreen swapchain for diagnostics). Uncharted 2 LAN multiplayer (`runtime_net.c`, `runtime_uc2mp*.c`, `uc2mp_*.h`, `tools/uc2mp_*.py`, `tools/net_overlay.py`, notes in `analysis/MULTIPLAYER-*.md`) is a hidden preview: `UNCHARTED_MULTIPLAYER=1` or `analysis/uncharted-user/multiplayer-preview` adds the launcher's Multiplayer section; `UNCHARTED_NET_MODE`, `UNCHARTED_INSTANCE` as in `probe_uncharted.py` `net_mode` / `instance_suffix`. `launcher/steam_library.py` (Add to Steam) and `launcher/net_lan.py` ship with the launcher (the package copies `launcher/*.py *.pyw *.json`).
- HD art editor sources (26 Oct): `gow3_pc/assets/ui/hd/editable/` keeps native VectorCraft/PhotoCraft files and the finishing recipe. The exported PNG under `hd/src/` is the build input; avoid reapplying destructive finishing filters to an already finished export. `hd/inventory.csv` records each collected reference and HD source, intentional soft originals, native logos, missing references and individual on-screen verification limits; a loaded texture alone is not proof that its icon appeared.

**Measuring.**
- `C:/dev/bench` (outside the repo): `bench.sh <u1|u2|u3> <tag>` (player's settings, saves restored, 20 s standing still), `ab2.sh` (live on/off windows in one session, order flipped each round), `settoggle.sh "<first> <second>"` (writes the toggle file), `abstats.py <tag>` (medians, p99, 1% lows, bootstrap intervals of the difference). Use PresentMon (run by `play.py measure`) for display cadence.
- A/B through the toggle file in one session, alternating the order. Windows of one session are not independent replicates: run an A/A (`"0 0"` as OFF) for the noise level.
- Profilers (all print to the port's stdout): `GOW3_GPU_PROFILE=1` (ms per label, and "(between submissions: GPU idle)" per call stack, every 5 s); `GOW3_SUBMIT_STATS=1` (stack offsets: symbolize with `llvm-symbolizer --obj=gow3_pc/out/gow3-probe.exe` at `0x140000000`+offset, under the CLANG64 PATH); `GOW3_SUBMIT_WAIT_STATS=1`; `GOW3_REFLEX_LOG=<file>`.
- The port's stdout goes to `analysis/<executable>/boot.log` (no timestamps: note the file size before and after each window to split it). `analysis/play/game.log` only holds the prepare output.

**Analysis material.**
- `analysis/<game>/boot-linked.bin` holds the linked guest images for disassembly. At run time the guest base is `0xb400000`.
- `analysis/STATUS.md` is the detailed engineering log: measurements, causes ruled out, open issues. Read its index block at the top before digging. `analysis/PLAN.md` is the old 13-15 Oct plan; the current plan is `ROADMAP.md`.

**Gotchas.**
- The packaged `gow3-probe.exe` is stripped (`llvm-strip` in the packaging script); `gow3_pc/out/gow3-probe.exe` is the ~160 MB unstripped build. If the game exits silently right after boot, run `build.sh` first.
- `update_port.sh` needs the launcher and the game closed.
- A `.claude/worktrees/<name>` checkout has its submodules empty and `build.sh` would clone them: copy `gow3_pc/gpu/third_party/{imgui,magic_enum,miniz,xbyak,fsr-vulkan}` and `gow3_pc/third_party/LibAtrac9` from the main checkout (without `.git`; `fsr-vulkan` there already carries the patches) and build in the worktree (about 10 minutes at 12 threads).
- `tools/export_github.sh` exits 1 before copying anything on this tree (26 Oct): it reads the submodule list from `gow3_pc/.gitmodules`, but the list is the root `.gitmodules` (paths prefixed `gow3_pc/`), so the lookup returns nothing and `set -e` stops it without a message. Its two lookups (lines 28 and 56) need the root file; `tools/release.sh` is unaffected. STATUS 25 Oct "Sharp text" records the same failure.
- When a task prompt says "do not commit" (or to commit only on the user's explicit authorization), do not; the orchestrator or the user commits.

## Conventions

- Commits and pull requests follow `CONTRIBUTING.md`: subject `<area>: <what is now true>` (max 72 chars, English; areas u1 u2 u3 games gpu runtime launcher package tools docs merge release), body with Why / What changed / Tested / Not tested; PRs use `.github/pull_request_template.md`. One change per commit.
- Every behaviour change gets a `CHANGELOG.md` entry under the top, unreleased section (newest date first, player-facing wording). `analysis/STATUS.md` gets the technical detail: evidence, measurements, what was not verified.
- Code comments explain the why with dated evidence (e.g. "17 Oct profile: …"); match that density and style.
- `.claude/ship.md` has the ship rules: the commit rules above, plus the build and test commands.
- Game test runs: short by default (tens of seconds); long runs only for stability soaks or bug hunting. Performance claims need an A/B at the player's settings (4K, 120 FPS cap, DLAA, multithreading on) that beats run-to-run noise.

## Repository upkeep

Keep the documentation true and the tree clean as part of every task, in the same commit as the change that makes it necessary. The map of which document holds what is in README.md "Documentation".

**Update rules (do them without being asked).**
- A behaviour change: `CHANGELOG.md` (player wording, unreleased section) and an `analysis/STATUS.md` section with its line in the STATUS index. A STATUS section with no index line is unfinished.
- A new or changed command, setting, `GOW3_*` variable, toggle bit, path or tool: the matching place in this file ("Commands", "Architecture", "Runtime toggles", "Measuring") and in `.claude/ship.md` when it is a build or test command. A new setting also reaches the launcher and `probe_uncharted.py` (the four places above).
- A change players see in the package: `tools/README-Windows.txt` if it changes how they install or play, and `tools/github/README.md` / `tools/github/CHANGELOG.md` at release time (the public copies).
- A new top-level folder, document or asset folder: README.md "Repository layout" / "Documentation".
- A fact that changed on this PC (installed tools, paths, Python versions, SDK locations): fix every line that states the old one; grep for it.
- Dated facts stay dated ("since 19 Oct"). When one stops being true, change the line instead of adding a contradicting one; history belongs in STATUS and the CHANGELOG, not in the guides.

**Cleanup rules.**
- Scratch work (probes, logs, one-off scripts, captures, extracted game files) goes in the session's scratchpad or `analysis/` (ignored), never in the tracked tree. Before committing, `git status --short` must show only the task's files.
- A file, asset or setting that a change makes unused is deleted in the same commit, with every reference (grep the name: code, package script, docs, `.gitignore`). Superseded plans are marked as history at the top (as `analysis/PLAN.md`) rather than deleted when STATUS or the CHANGELOG link to them.
- Opt-in experiments (toggle bits, `GOW3_*` opt-ins) that a measurement rejected are removed with their code once STATUS records the result; keep only those still under test, listed in this file.
- Generated files are rebuilt, not committed, unless the build needs a manual step (e.g. the VectorCraft traces in `gow3_pc/assets/ui/`); `.gitignore` covers the rest.
- Merged `claude/*` branches and their worktrees are removed after the merge; never delete an unmerged branch, a Codex branch or anything outside this repository without asking.
- Before deleting anything, look at it: report files you did not create instead of removing them.
