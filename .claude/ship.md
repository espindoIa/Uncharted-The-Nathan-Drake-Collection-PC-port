# Ship rules for this repo (read by the /ship skill)

- Branch: commit directly on the current branch; no merge or push unless asked.
- Commit messages: `CONTRIBUTING.md` (subject `<area>: <what is now true>`, body Why / What changed / Tested / Not tested).
- Never commit: `gow3_pc/stat-multipliers-test.ini`, the dirty submodule `gow3_pc/gpu/third_party/fsr-vulkan`.
- Build (MSYS2 CLANG64): `/c/msys64/usr/bin/bash.exe -lc "export MSYSTEM=CLANG64 PATH=/clang64/bin:/usr/bin:\$PATH; cd /c/dev/uncharted/gow3_pc && ./build.sh"`; on failure read `gow3_pc/out/gpu-build.log`.
- Unit tests: built in `gow3_pc/out/gpu` (e.g. `movie-hook-test.exe`), run under the same CLANG64 PATH.
- Launcher: `python -c "import ast; ast.parse(open('launcher/uncharted_launcher.pyw', encoding='utf-8').read())"`.
- `tools/play.py` and the `C:/dev/bench` scripts need Python 3.13 first on PATH (3.12 has no PIL).
- Update `CHANGELOG.md` (newest date first, unreleased section) and `analysis/STATUS.md` when behaviour changes.
- Package: `DLSS_BRIDGE=/c/dev/uncharted/gow3_pc/out/gow3_dlss.dll DLSS_SDK=/c/dev/DLSS tools/package_windows.sh` -> `dist/Uncharted-windows.zip`. It must not contain `nvngx_dlss.dll` and must contain `gow3_dlss.dll`; test it extracted in a clean folder (the pacman PGP-key prompts in its output are noise).
