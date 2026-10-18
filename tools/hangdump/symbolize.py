"""Symbolizes hangdump output: offsets into the runtime (Uncharted.exe / gow3-probe.exe) become
function names (llvm-symbolizer with gow3_pc/out/gpu/gow3-probe.exe, image base 0x140000000).

    python tools/hangdump/symbolize.py dump.txt [--all]

Without --all, threads whose top frames are only waits in system DLLs are summarized on one line.
"""
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
EXE = ROOT / "gow3_pc" / "out" / "gpu" / "gow3-probe.exe"
RUNTIME = ("Uncharted.exe", "gow3-probe.exe")


def main():
    lines = Path(sys.argv[1]).read_text(encoding="utf-8", errors="replace").splitlines()
    offsets = sorted({m.group(2) for line in lines
                      if (m := re.match(r"\s+(\S+?)\+0x([0-9a-f]+)$", line)) and m.group(1) in RUNTIME})
    names = {}
    if offsets:
        out = subprocess.run(["llvm-symbolizer", f"--obj={EXE}", "--functions=linkage", "--demangle",
                              "--no-inlines"], input="\n".join(hex(0x140000000 + int(o, 16)) for o in offsets),
                             capture_output=True, text=True).stdout
        for offset, block in zip(offsets, out.strip().split("\n\n")):
            names[offset] = re.sub(r"\(.*", "", block.split("\n")[0])[:120]
    for line in lines:
        m = re.match(r"(\s+)(\S+?)\+0x([0-9a-f]+)$", line)
        if m and m.group(2) in RUNTIME:
            print(f"{m.group(1)}{names.get(m.group(3), line.strip())}")
        else:
            print(line)


if __name__ == "__main__":
    main()
