# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

PC port of the PS4 *Uncharted: The Nathan Drake Collection* (CUSA02344: U1 Drake's Fortune, U2 Among
Thieves, U3 Drake's Deception). The games' own x86-64 code runs natively; a C runtime supplies the PS4
system libraries (HLE) and a shadPS4-derived Vulkan renderer executes the PS4 GPU work. Windows only
in practice (MSYS2 CLANG64 toolchain). The runtime began as the gow3_pc project, so internal names
stay `gow3` (`gow3-probe.exe`, `GOW3_*` variables, `gow3_*` files) — that is not God of War code
unless it sits behind the game-hooks gate (below).

**Locations.**
- The repo lives in `C:\Users\marce\Downloads\Projetos\Uncharted-PC-Port`. `C:\dev\uncharted` is a junction to it; build through that path, because the CMake cache in `gow3_pc/out/gpu` was generated there.
- The playable port is in `Downloads\Jogos_e_Mods\Jogos\Uncharted-PC-Port`: `Uncharted\` is the extracted package, `Jogo\CUSA02344\` the game.
- Remotes:
  - `origin` is the fork, `espindoIa/Uncharted-The-Nathan-Drake-Collection-PC-port`, and the work is on branch `claude/dlss-uncharted`.
  - `upstream` is BilalReloaded's repo. Its 0.3.0 is merged into the branch; the merge choices are listed under "Fork (espindoIa) notes" in `analysis/STATUS.md`.
- Both sides patch some of the same game bytes. When taking more upstream work, look for duplicate patch tables in `tools/probe_uncharted.py`: a patch applied first keeps a later one's signature from matching.

## Commands

The toolchain is MSYS2 CLANG64. From Git Bash, wrap commands like this:

```bash
/c/msys64/usr/bin/bash.exe -lc "export MSYSTEM=CLANG64 PATH=/clang64/bin:/usr/bin:\$PATH; cd /c/dev/uncharted/gow3_pc && ./build.sh"
```

- **Build the runtime:** `gow3_pc/build.sh` → `gow3_pc/out/gow3-probe.exe` (CMake + Ninja in `out/gpu`). On failure read `gow3_pc/out/gpu-build.log`; the script prints little.
- **C/C++ unit tests:** CMake targets marked `EXCLUDE_FROM_ALL` in `gow3_pc/gpu/CMakeLists.txt` (e.g. `movie-hook-test`, `scene-resolution-test`). Build and run one under the CLANG64 PATH (the exes need its DLLs): `ninja -C out/gpu movie-hook-test && ./out/gpu/movie-hook-test.exe` from `gow3_pc`.
- **Python tests:** `unittest` files in `gow3_pc/tests/` that import `paths.py`; run one from that folder, e.g. `python -m unittest test_prepare`.
- **Launcher syntax check:** `python -c "import ast; ast.parse(open('launcher/uncharted_launcher.pyw', encoding='utf-8').read())"`.
- **Run from source:** `Start-Uncharted.cmd` (launcher), or `python tools/probe_uncharted.py --boot --seconds 0 --executable big3-ps4_Shipping.elf --game <CUSA02344 folder>` (`eboot.bin` = U1 / Collection menu, `big2-…` = U2, `big3-…` = U3). Logs go to `analysis/<executable>/boot.log`.
- **Package:** run from Git Bash, not the CLANG64 shell. In CLANG64 it picks MSYS2's Python, and Python copying fails:
  `DLSS_BRIDGE=/c/dev/uncharted/gow3_pc/out/gow3_dlss.dll DLSS_SDK=/c/dev/DLSS tools/package_windows.sh` → `dist/Uncharted-windows.zip`. The pacman "Import PGP key?" lines in its output are noise. The zip must contain `gow3_dlss.dll` and must **not** contain `nvngx_dlss.dll` (NVIDIA's license does not fit the GPL; the launcher has the player add it). Test it extracted into an empty folder.
- **Release / public sync:** `tools/release.sh <version>` (needs `tools/release-notes/v<version>.md`); `tools/export_github.sh` copies source only (no game files) into a separate repo for GitHub Actions (`.github/workflows/build-windows.yml`).

## Architecture

**Launch chain.**
1. `launcher/uncharted_launcher.pyw` (Tkinter) writes per-game settings to `analysis/uncharted-user/probe-u{1,2,3}.ini`.
2. `tools/probe_uncharted.py` prepares the executable with `gow3_pc/scripts/` (patches, render-resolution rewrite, update 1.02 overlay). It turns the ini into `GOW3_*` environment variables and starts `gow3-probe.exe`. Some choices are decided there at start, which is why they need a restart: an upscaler on or off (camera source, scene size), the render resolution, readback modes.
3. `UNCHARTED_*` variables and `analysis/uncharted-user/test-env.txt` override what it sets.

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
  - Also window and SDL, Videodec2, fibers and movie scaling.
- Upscalers live in `shadps4/video_core/renderer_vulkan/vk_temporal_upscaler.cpp` (FSR 3.1, FSR 4, TAA, DLSS). DLSS goes through the optional MIT bridge `gpu/dlss_bridge` (`gow3_dlss.dll`), loaded at run time next to `nvngx_dlss.dll`.
- A new setting usually touches four places: `gow3_settings.h`/`.cpp` (parse in `Set`, write in `Current`), the menu, the launcher, and possibly `probe_uncharted.py`.

**Analysis material.**
- `analysis/<game>/boot-linked.bin` holds the linked guest images for disassembly. At run time the guest base is `0xb400000`.
- `analysis/STATUS.md` is the detailed engineering log: measurements, causes ruled out, open issues. `analysis/PLAN.md` is the plan and test list.

## Conventions

- Every behaviour change gets a `CHANGELOG.md` entry (newest date first, player-facing wording). `analysis/STATUS.md` gets the technical detail: evidence, measurements, what was not verified.
- Code comments explain the why with dated evidence (e.g. "17 Oct profile: …"); match that density and style.
- Never commit `gow3_pc/stat-multipliers-test.ini` or the dirty submodule `gow3_pc/gpu/third_party/fsr-vulkan`. Stage files by name. `.claude/ship.md` has these ship rules.
- No game files, dumps, saves or logs in commits (`export_github.sh` refuses them too).
