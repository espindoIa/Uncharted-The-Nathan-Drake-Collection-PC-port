#!/usr/bin/env bash
# Fast update for the player's port folder after a change (replaces package_windows.sh +
# update_port.sh for day-to-day work: those rebuild Python, licenses and a zip, ~minutes).
# Copies only files that differ, straight into the installed Uncharted\ folder; never touches
# analysis\uncharted-user (saves, settings), python\, licenses\, VERSION or NVIDIA's nvngx_*.dll.
#   tools/dev_sync.sh            copy what changed
#   tools/dev_sync.sh -n         list what would change, copy nothing
#   tools/dev_sync.sh --build    run gow3_pc/build.sh first (CLANG64), then copy
# Env: PORT (installed Uncharted\ folder), OUT (runtime build folder, default gow3_pc/out).
# Run from Git Bash. Use the full package + update_port.sh for releases, and when the DLL set,
# Python or the license files change: this script copies none of those.
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
root=$PWD
PORT=${PORT:-/c/Users/marce/Downloads/Jogos_e_Mods/Jogos/Uncharted-PC-Port/Uncharted}
OUT=${OUT:-gow3_pc/out}
dry=0; build=0
for arg in "$@"; do
    case $arg in -n) dry=1 ;; --build) build=1 ;; *) echo "usage: $0 [-n] [--build]" >&2; exit 2 ;; esac
done
[[ -d $PORT ]] || { echo "no installed port at $PORT" >&2; exit 1; }
export PATH="${MSYS2_ROOT:-/c/msys64}/clang64/bin:$PATH"

if (( build )); then
    "${MSYS2_ROOT:-/c/msys64}/usr/bin/bash.exe" -lc \
        "export MSYSTEM=CLANG64 PATH=/clang64/bin:/usr/bin:\$PATH; cd '$(cygpath -u "$root")/gow3_pc' && ./build.sh"
fi

# Windows will not overwrite a running exe; stop early with a clear message.
if tasklist 2>/dev/null | grep -qi 'gow3-probe.exe'; then
    echo "gow3-probe.exe is running: close the game first" >&2; exit 1
fi

changed=0
# put <src> <dst relative to $PORT>: copy when the content differs.
put() {
    local src=$1 dst=$PORT/$2
    [[ -f $src ]] || return 0
    cmp -s -- "$src" "$dst" && return 0
    changed=$((changed + 1))
    echo "  $2"
    (( dry )) || { mkdir -p -- "$(dirname -- "$dst")"; cp -- "$src" "$dst"; }
}

# Stripped runtime exes (the package strips debug info; the build output is ~160 MB). The cache
# keeps the strip from re-running when the build did not change.
cache=dist/dev-sync
mkdir -p "$cache"
for exe in gow3-probe.exe gow3-gpu-capabilities.exe; do
    [[ -f $OUT/$exe ]] || { echo "missing $OUT/$exe: build first (--build)" >&2; exit 1; }
    [[ $cache/$exe -nt $OUT/$exe ]] || llvm-strip --strip-debug -o "$cache/$exe" "$OUT/$exe"
    put "$cache/$exe" "gow3_pc/out/$exe"
done
put "$OUT/gow3_dlss.dll" gow3_pc/out/gow3_dlss.dll

# Keep these lists in step with tools/package_windows.sh.
for f in gow3_pc/scripts/*.py; do put "$f" "$f"; done
for f in probe_uncharted hang_report apply_update play install_update session_recorder; do
    put "tools/$f.py" "tools/$f.py"
done
for f in launcher/uncharted_launcher.pyw launcher/updater.py launcher/widgets.py launcher/art.py \
        launcher/assets/{uncharted.ico,icon.png,grain.png} launcher/assets/icons/*.png; do
    put "$f" "$f"
done
put gow3_pc/assets/ui/wordmark.svg gow3_pc/assets/ui/wordmark.svg
put gow3_pc/assets/ui/fonts/drake-serial.ttf gow3_pc/assets/ui/fonts/drake-serial.ttf

if (( changed )); then
    echo "$changed file(s) $( (( dry )) && echo 'would change' || echo 'updated' )"
else
    echo "up to date"
fi
