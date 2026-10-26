#!/usr/bin/env bash
# Builds dist/Uncharted-windows/ and dist/Uncharted-windows.zip: a folder that runs on any Windows
# 10/11 x64 PC with a Vulkan GPU, without Python or MSYS2 installed.
#   Uncharted Launcher.exe   starter (tools/windows_starter) -> python\pythonw.exe launcher\...
#   python\                  Windows Python with Tkinter and Pillow (tools/copy_python.py)
#   launcher\ tools\         the launcher and the start script (tools/probe_uncharted.py)
#   gow3_pc\scripts\         the preparation scripts; gow3_pc\out\ the runtime and its DLLs
# No game files ship: each player selects their own copy of the game in the launcher.
# Run after building the runtime (gow3_pc/build.sh), from Git Bash or an MSYS2 CLANG64 shell.
# PYTHON_HOME is a Windows Python 3.10+ with Pillow and numpy (default: the `python` on PATH).
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
# The Windows Python (looked up before the CLANG64 tools go first on PATH: MSYS2 has its own).
python=${PYTHON:-python}
python_home=${PYTHON_HOME:-$("$python" -c 'import sys, os; print(os.path.dirname(sys.executable))')}
msys=${MSYS2_ROOT:-/c/msys64}
# Inside the MSYS2 shell (GitHub Actions) its root is "/": without the slash, "$msys/usr" stays a
# local path ("//usr/..." is a network share, each try timed out after ~11 s).
msys=${msys%/}
[[ -d $msys/clang64/bin ]] && export PATH="$msys/clang64/bin:$PATH"
[[ -f gow3_pc/out/gow3-probe.exe ]] || { echo 'Build the runtime first: gow3_pc/build.sh' >&2; exit 1; }

stage=dist/stage/Uncharted-windows
rm -rf -- dist/stage
mkdir -p "$stage/gow3_pc/out" "$stage/gow3_pc/scripts" "$stage/tools" "$stage/launcher/assets" \
    "$stage/analysis/uncharted-user"

# The runtime without debug information, and every DLL it loads from the CLANG64 tree.
llvm-strip --strip-debug -o "$stage/gow3_pc/out/gow3-probe.exe" gow3_pc/out/gow3-probe.exe
# The launcher lists the Vulkan GPUs with it (Display page, graphics card); it needs no CLANG64 DLL.
llvm-strip --strip-debug -o "$stage/gow3_pc/out/gow3-gpu-capabilities.exe" gow3_pc/out/gow3-gpu-capabilities.exe
ldd gow3_pc/out/gow3-probe.exe | awk '/clang64/ {print $3}' | sort -u | while read -r dll; do
    cp "$dll" "$stage/gow3_pc/out/"
done

# DLSS (optional): DLSS_BRIDGE=<gow3_dlss.dll, built with MSVC from gow3_pc/gpu/dlss_bridge> and
# DLSS_SDK=<NVIDIA DLSS SDK root, for its license> add the bridge next to the runtime. NVIDIA's
# nvngx_dlss.dll is never packaged (18 Oct: its license does not fit the GPL runtime): players
# download it from NVIDIA's DLSS repository and the launcher installs it after checking NVIDIA's
# signature (Display tab). The bridge links NVIDIA's NGX static library, so NVIDIA's license goes
# with it (DLSS SDK license 2c; supplement 7.1(b): attribution in README.txt).
if [[ -n ${DLSS_BRIDGE:-} || -n ${DLSS_SDK:-} ]]; then
    [[ -f ${DLSS_BRIDGE:-} && -f ${DLSS_SDK:-}/LICENSE.txt ]] || {
        echo "DLSS: need DLSS_BRIDGE (gow3_dlss.dll) and DLSS_SDK (with LICENSE.txt)" >&2; exit 1; }
    cp "$DLSS_BRIDGE" "$stage/gow3_pc/out/"
    mkdir -p "$stage/licenses/nvidia-dlss" "$stage/licenses/gow3-dlss-bridge"
    cp "$DLSS_SDK/LICENSE.txt" "$stage/licenses/nvidia-dlss/LICENSE.txt"
    cp gow3_pc/gpu/dlss_bridge/LICENSE.txt "$stage/licenses/gow3-dlss-bridge/LICENSE.txt"
    echo "DLSS: gow3_dlss.dll included (nvngx_dlss.dll comes from NVIDIA, added in the launcher)"
fi
[[ ! -e $stage/gow3_pc/out/nvngx_dlss.dll ]] || { echo "nvngx_dlss.dll must not be packaged" >&2; exit 1; }
[[ ! -e $stage/gow3_pc/out/nvngx_dlssg.dll ]] || { echo "nvngx_dlssg.dll must not be packaged" >&2; exit 1; }

# NVIDIA Streamline (optional, 23 Oct 2026): STREAMLINE_SDK=<the Streamline SDK release, e.g.
# streamline-sdk-v2.14.16 unpacked> adds its NVIDIA-signed production plugins next to the runtime
# (nv_backend=streamline). The release's license.txt is MIT for them; the nvngx_* DLLs in the same
# folder are under NVIDIA's RTX SDK license and NvLowLatencyVk.dll under the Reflex SDK license, so
# those stay out (the player adds nvngx_dlss.dll / nvngx_dlssg.dll in the launcher, as for NGX).
# They need the Visual C++ 2015-2022 runtime; without it the runtime keeps the NGX bridge.
STREAMLINE_DLLS=(sl.interposer.dll sl.common.dll sl.dlss.dll sl.dlss_g.dll sl.reflex.dll sl.pcl.dll)
if [[ -n ${STREAMLINE_SDK:-} ]]; then
    for dll in "${STREAMLINE_DLLS[@]}"; do
        [[ -f $STREAMLINE_SDK/bin/x64/$dll ]] || { echo "Streamline: $dll missing in $STREAMLINE_SDK/bin/x64" >&2; exit 1; }
        cp "$STREAMLINE_SDK/bin/x64/$dll" "$stage/gow3_pc/out/"
    done
    mkdir -p "$stage/licenses/nvidia-streamline"
    cp "$STREAMLINE_SDK/license.txt" "$stage/licenses/nvidia-streamline/LICENSE.txt"
    echo "Streamline: ${STREAMLINE_DLLS[*]} included"
else
    # 23 Oct 2026: Streamline is the runtime's only player-facing way to DLSS (no setting).
    echo "warning: STREAMLINE_SDK not set: the package falls back to the NGX bridge" >&2
fi
[[ ! -e $stage/gow3_pc/out/NvLowLatencyVk.dll ]] || { echo "NvLowLatencyVk.dll must not be packaged" >&2; exit 1; }

cp gow3_pc/scripts/*.py "$stage/gow3_pc/scripts/"
cp tools/probe_uncharted.py tools/hang_report.py tools/apply_update.py tools/play.py tools/install_update.py "$stage/tools/"
# Session recorder (a player runs Record-Session.cmd beside the game; it uses python\python.exe).
cp tools/session_recorder.py "$stage/tools/"
cp Record-Session.cmd "$stage/"
# PkgTool (LibOrbisPkg, LGPL-3.0): Advanced -> Install game update unpacks the player's own update PKG.
mkdir -p "$stage/tools/pkgtool"
cp tools/pkgtool/PkgTool.exe tools/pkgtool/LibOrbisPkg.dll "$stage/tools/pkgtool/"
cp launcher/uncharted_launcher.pyw launcher/updater.py launcher/widgets.py launcher/art.py "$stage/launcher/"
# The package's version (tools/VERSION; the launcher's updates compare it with the releases).
cp tools/VERSION "$stage/VERSION"
cp launcher/assets/uncharted.ico launcher/assets/icon.png launcher/assets/grain.png "$stage/launcher/assets/"
# The games' lettering for the launcher (tools/build_ui_art.py): the font and the wordmark only.
mkdir -p "$stage/gow3_pc/assets/ui/fonts"
cp gow3_pc/assets/ui/wordmark.svg "$stage/gow3_pc/assets/ui/"
cp gow3_pc/assets/ui/fonts/drake-serial.ttf "$stage/gow3_pc/assets/ui/fonts/"
# HD UI textures (gow3_pc/gpu/shim/gow3_hdtex.cpp): the <hash16hex>_<w>x<h>.dds files of
# gow3_pc/assets/ui/hd/<set>/ next to the exe, where the runtime looks. The folder ships even empty.
# Flattened, so two sets must not hold the same file name (the second would overwrite the first).
mkdir -p "$stage/gow3_pc/out/hd-textures"
# The .dds files are generated (not committed) from the art in gow3_pc/assets/ui/hd/src: PNG and SVG
# redrawn at 4x, with their mips and compression (needs numpy beside Pillow).
if [[ -d gow3_pc/assets/ui/hd/src ]]; then
    "$python" tools/build_ui_art.py hd || exit 1
fi
if [[ -d gow3_pc/assets/ui/hd ]]; then
    while IFS= read -r -d '' dds; do
        if [[ -e "$stage/gow3_pc/out/hd-textures/${dds##*/}" ]]; then
            echo "HD UI textures: ${dds##*/} is in two sets (second: $dds)" >&2
            exit 1
        fi
        cp -- "$dds" "$stage/gow3_pc/out/hd-textures/"
    done < <(find gow3_pc/assets/ui/hd -type f -name '*.dds' -print0)
fi
# The launcher's icons: the 192 px masters only (their VectorCraft sources in icons/src stay in the repo).
mkdir -p "$stage/launcher/assets/icons"
cp launcher/assets/icons/*.png "$stage/launcher/assets/icons/"
# The runtime's optimizations known to break Uncharted (GOW3_TOGGLE_FILE).
cp tools/defaults/disabled-optimizations.txt "$stage/analysis/uncharted-user/"
cp tools/README-Windows.txt "$stage/README.txt"
cp gow3_pc/LICENSE "$stage/LICENSE.txt"
cp tools/github/CHANGELOG.md "$stage/CHANGELOG.md"

# Licenses of the sources built into the runtime, and of every MSYS2 package whose DLL ships: its
# license files and, in THIRD-PARTY.txt, its version, license and source (some packages, FFmpeg
# among them, carry no license file).
licenses=$stage/licenses
mkdir -p "$licenses"
for dir in gow3_pc/gpu/third_party/* gow3_pc/third_party/*; do
    for file in "$dir"/LICENSE* "$dir"/COPYING* "$dir"/COPYRIGHT*; do
        [[ -f $file ]] || continue
        mkdir -p "$licenses/$(basename -- "$dir")"
        cp "$file" "$licenses/$(basename -- "$dir")/"
    done
done
head -n 24 gow3_pc/gpu/third_party/gcn/include/gcn/si_ci_vi_merged_offset.h > "$licenses/gcn-headers.txt"
mkdir -p "$licenses/liborbispkg"
cp tools/pkgtool/LICENSE.txt "$licenses/liborbispkg/LICENSE.txt"
echo "PkgTool and LibOrbisPkg 0.2.231 by Maxton, unchanged; source: https://github.com/maxton/LibOrbisPkg" \
    > "$licenses/liborbispkg/SOURCE.txt"
pacman=$(command -v pacman || echo "$msys/usr/bin/pacman")
# Three pacman calls in all (one per DLL took minutes on the CI runner).
# 20 Oct 2026 audit: a DLL no package owns used to be skipped silently (no license in THIRD-PARTY.txt)
# and an empty list made -Ql/-Qi cover every installed package. -Qo prints one "<file> is owned by
# <pkg> <ver>" line per owned file on stdout, and the unowned ones as errors on stderr.
dlls=()
for dll in "$stage"/gow3_pc/out/*.dll; do
    dlls+=("$(cygpath -m "$msys/clang64/bin/$(basename -- "$dll")")")
done
owned=$("$pacman" -Qo "${dlls[@]}" 2>/dev/null || true)
for dll in "${dlls[@]}"; do
    # gow3_dlss.dll is built by this project (MIT bridge, licensed under licenses/gow3-dlss-bridge);
    # Streamline's sl.*.dll come from NVIDIA (licenses/nvidia-streamline).
    [[ $(basename -- "$dll") == gow3_dlss.dll || $(basename -- "$dll") == sl.*.dll ]] && continue
    grep -qF -- "/$(basename -- "$dll") is owned by " <<< "$owned" || { echo "no MSYS2 package owns $(basename -- "$dll"): no license to ship" >&2; exit 1; }
done
mapfile -t packages < <(awk '/ is owned by /{print $(NF-1)}' <<< "$owned" | sort -u)
[[ ${#packages[@]} -gt 0 ]] || { echo "no MSYS2 package owns any shipped DLL" >&2; exit 1; }
{ "$pacman" -Qlq "${packages[@]}" | grep '/share/licenses/.*[^/]$' || true; } | while read -r file; do
    name=${file#*/share/licenses/}
    mkdir -p "$licenses/msys2/$(dirname -- "$name")"
    cp "$msys$file" "$licenses/msys2/$name"
done
{
    echo "Third-party packages whose DLLs ship in gow3_pc\\out (MSYS2 CLANG64 builds)."
    echo "Their sources: https://repo.msys2.org/mingw/sources/ and https://packages.msys2.org/"
    "$pacman" -Qi "${packages[@]}" | awk '/^Name /{print ""} /^(Name|Version|Licenses|URL) /{print}'
} > "$licenses/THIRD-PARTY.txt"

# DLSS and FSR 4 (tools/fetch_aa_files.sh), when fetched: the DLSS bridge under its own license
# (not NVIDIA's nvngx_dlss.dll: see above; the launcher adds it) and the FSR 4 native model.
# Without them the menus leave the two out.
if [[ -f gow3_pc/out/gow3_dlss.dll && ! -f $stage/gow3_pc/out/gow3_dlss.dll ]] &&
        compgen -G "downloads/dlss-sdk-*/LICENSE.txt" > /dev/null; then
    cp gow3_pc/out/gow3_dlss.dll "$stage/gow3_pc/out/"
    mkdir -p "$licenses/nvidia-dlss" "$licenses/gow3-dlss-bridge"
    cp downloads/dlss-sdk-*/LICENSE.txt "$licenses/nvidia-dlss/LICENSE.txt"
    cp gow3_pc/gpu/dlss_bridge/LICENSE.txt "$licenses/gow3-dlss-bridge/LICENSE.txt"
fi
[[ ! -e $stage/gow3_pc/out/nvngx_dlss.dll ]] || { echo "nvngx_dlss.dll must not be packaged" >&2; exit 1; }
[[ ! -e $stage/gow3_pc/out/nvngx_dlssg.dll ]] || { echo "nvngx_dlssg.dll must not be packaged" >&2; exit 1; }
if [[ -f gow3_pc/fsr4_shaders/fsr4_model_v07_i8_native_1080_pre.spv ]]; then
    mkdir -p "$stage/gow3_pc/fsr4_shaders" "$licenses/fsr4"
    cp gow3_pc/fsr4_shaders/LICENSE-FSR4-v07.txt gow3_pc/fsr4_shaders/*.spv gow3_pc/fsr4_shaders/*.bin \
        gow3_pc/fsr4_shaders/*.json "$stage/gow3_pc/fsr4_shaders/"
    cp gow3_pc/fsr4_shaders/LICENSE-FSR4-v07.txt "$licenses/fsr4/"
fi

"$python_home/python.exe" tools/copy_python.py "$python_home" "$stage/python"
# Python's own license is python\LICENSE.txt; Pillow's goes with the others.
for file in "$python_home"/Lib/site-packages/pillow-*.dist-info/licenses/* \
    "$python_home"/Lib/site-packages/pillow-*.dist-info/LICENSE*; do
    [[ -f $file ]] && { mkdir -p "$licenses/pillow"; cp "$file" "$licenses/pillow/"; }
done

windres tools/windows_starter/starter.rc -O coff -o dist/stage/starter.res.o
clang -O2 -municode -mwindows tools/windows_starter/starter.c dist/stage/starter.res.o \
    -o "$stage/Uncharted Launcher.exe"

find "$stage" -name __pycache__ -prune -exec rm -rf {} +
# Nothing from the game or a player (game executables, saves, shader caches, logs) may ship.
forbidden=$(find "$stage" \( -iname eboot.bin -o -iname '*.elf' -o -iname sce_sys -o -iname savedata \
    -o -iname cache -o -iname '*.log' -o -iname 'probe*.ini' -o -iname '*.pkg' -o -iname '*.psarc' \
    -o -iname '*.pak' -o -iname 'boot*.bin' \) -print)
if [[ -n $forbidden ]]; then
    echo "Package holds files that must not ship:" >&2
    echo "$forbidden" >&2
    exit 1
fi

rm -f dist/Uncharted-windows.zip
(cd dist/stage && powershell -NoProfile -Command \
    "Compress-Archive -Path Uncharted-windows -DestinationPath ../Uncharted-windows.zip")
rm -rf dist/Uncharted-windows
mv "$stage" dist/Uncharted-windows
du -sh dist/Uncharted-windows dist/Uncharted-windows.zip
