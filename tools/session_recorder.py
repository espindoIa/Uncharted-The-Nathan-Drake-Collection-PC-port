#!/usr/bin/env python3
"""Session recorder for the Uncharted PC port: a separate process that watches the running game and
leaves a folder a player can send with a bug report (system, build, settings, timed game log,
resource metrics, exit code, and moments flagged with a hotkey, each with a screenshot and an
optional engine frame dump).

    python tools\\session_recorder.py [--root DIR] [--sessions-dir DIR] [--keys F7=texture,...]
                                     [--max-engine-dumps 20] [--no-engine-dump] [--interval 2]
                                     [--once] [--selftest]

Waits for Uncharted.exe, records one session in
<root>\\analysis\\uncharted-user\\sessions\\<YYYYmmdd-HHMMSS>_<out dir>\\, finalizes it when the game
exits, then waits for the next launch. Python standard library only; Windows API through ctypes.

What it never does (the game must not notice it): open the game's boot.log for writing (it is read
a chunk at a time and closed again), read or write the game's memory, inject anything, or write
outside the sessions folder, except creating <out>\\dumps\\trigger on a hotkey (the engine's own
on-demand frame dump, see vk_presenter.cpp) and moving the files that answer it into the session.
The pure helpers at the top (line classification, Frame stats parsing, PNG, summary) import on any
OS; they are what gow3_pc/tests/test_session_recorder.py tests."""
import argparse
import csv
import ctypes
import datetime
import hashlib
import json
import os
import re
import shutil
import statistics
import struct
import subprocess
import sys
import tempfile
import threading
import time
import traceback
import zlib
from collections import Counter, deque
from pathlib import Path

RECORDER_VERSION = "1"
IS_WIN = os.name == "nt"
LOG_NAME = "boot.log"
SIZE_CAP = 200 * 1024 * 1024        # per growing text file (timed log, events)
MIN_FREE_BYTES = 2 * 1024 ** 3      # below this: no screenshots or engine dumps
RING_LINES = 300
DUPLICATE_LIMIT = 20                # identical event lines written before they are only counted
COLUMNS = ["time", "elapsed_s", "cpu_pct", "cpu_cores", "ws_mb", "private_mb", "threads", "handles",
           "sys_free_ram_mb", "gpu_util", "gpu_mem_mb", "gpu_clock_mhz", "gpu_temp_c", "gpu_power_w",
           "foreground_window"]
DEFAULT_KEYS = "F7=texture,F8=geometry,F9=flash,F10=other"
SLOW_FRAME_MS = 40.0

# ---------------------------------------------------------------------------------------------
# Pure helpers (no Windows API)

EVENT_RE = re.compile(
    r"warning|error|assert|unsupported|fail|Dropped|garbage|skipped|corrupt|mismatch|"
    r"Hang watchdog|Texture cache: memory|first unsupported|exit code", re.IGNORECASE)
WORST_RE = re.compile(r"worst frame ([0-9.]+) ms")
COMPILES_RE = re.compile(r"([0-9]+) shader/pipeline compiles")
FPS_RE = re.compile(r"Frame stats: ([0-9.]+) FPS")


def parse_frame_stats(line):
    """{'fps', 'worst_ms', 'compiles'} of a 'Frame stats: ...' line (one 5 s window), else None."""
    if "Frame stats:" not in line:
        return None
    fps, worst, compiles = FPS_RE.search(line), WORST_RE.search(line), COMPILES_RE.search(line)
    if not (fps and worst):
        return None
    return {"fps": float(fps.group(1)), "worst_ms": float(worst.group(1)),
            "compiles": int(compiles.group(1)) if compiles else 0}


def classify_line(line):
    """'slow-frame' for a Frame stats window with a worst frame over 40 ms or any shader/pipeline
    compile, 'event' for a line the events file keeps, else None. Frame stats lines never count as
    'event': their text has words like 'blocked' but is not a problem report by itself."""
    stats = parse_frame_stats(line)
    if stats:
        return "slow-frame" if stats["worst_ms"] > SLOW_FRAME_MS or stats["compiles"] > 0 else None
    return "event" if EVENT_RE.search(line) else None


class EventLog:
    """Decides which lines go to events.txt: every identical line only DUPLICATE_LIMIT times (the
    rest are counted, so a warning repeated per frame cannot fill the disk)."""

    def __init__(self):
        self.counts = Counter()

    def offer(self, line):
        """True when the line is to be written, 'note' once at the first suppressed repeat."""
        self.counts[line] += 1
        n = self.counts[line]
        return True if n <= DUPLICATE_LIMIT else ("note" if n == DUPLICATE_LIMIT + 1 else False)

    def dropped(self):
        return sum(n - DUPLICATE_LIMIT for n in self.counts.values() if n > DUPLICATE_LIMIT)

    def top(self, limit=10):
        return [(line, n) for line, n in self.counts.most_common() if n > 1][:limit]


class LogTailer:
    """Reads what the game appended to its log since the last poll. The file is opened only for the
    read and closed again (the game writes it through its own file position). A file that shrank
    (a new launch overwrote it) restarts from 0. Only complete lines are returned; the partial
    last line waits for its newline (flush=True returns it too)."""
    CHUNK = 16 * 1024 * 1024

    def __init__(self, path):
        self.path, self.offset, self.lineno, self.partial, self.resets = Path(path), 0, 0, b"", 0

    def poll(self, flush=False):
        try:
            size = self.path.stat().st_size
            if size < self.offset:
                self.offset, self.partial, self.resets = 0, b"", self.resets + 1
            data = b""
            if size > self.offset:
                with open(self.path, "rb") as f:
                    f.seek(self.offset)
                    data = f.read(min(size - self.offset, self.CHUNK))
        except OSError:
            return []
        self.offset += len(data)
        parts = (self.partial + data).split(b"\n")
        self.partial = b"" if flush else parts.pop()
        lines = [p.rstrip(b"\r").decode("utf-8", "replace") for p in parts if p or not flush]
        self.lineno += len(lines)
        return lines


def png_encode(width, height, rgb):
    """8-bit RGB PNG from width*height*3 bytes (top row first). zlib level 1: a 4K frame must not
    keep a core busy for seconds."""
    stride = width * 3
    view = memoryview(rgb)
    raw = b"".join(b"\0" + bytes(view[y * stride:(y + 1) * stride]) for y in range(height))

    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 1)) + chunk(b"IEND", b""))


def bgra_to_rgb(bgra):
    """GDI's 32-bit BGRA to packed RGB with strided slice assignment (C speed, no per-pixel loop)."""
    rgb = bytearray(len(bgra) // 4 * 3)
    rgb[0::3], rgb[1::3], rgb[2::3] = bgra[2::4], bgra[1::4], bgra[0::4]
    return bytes(rgb)


def is_all_black(rgb):
    return rgb.count(0) == len(rgb)


def parse_keys(spec):
    """'F7=texture,F8=geometry' -> {'F7': 'texture', ...}; F1-F24 or a single letter/digit."""
    keys = {}
    for item in spec.split(","):
        key, _, label = item.partition("=")
        key, label = key.strip().upper(), re.sub(r"[^A-Za-z0-9_-]", "", label.strip()) or "mark"
        if not (re.fullmatch(r"F([1-9]|1[0-9]|2[0-4])", key) or re.fullmatch(r"[A-Z0-9]", key)):
            raise ValueError(f"unsupported key '{key}' (use F1-F24, a letter or a digit)")
        keys[key] = label
    return keys


def virtual_key(key):
    return 0x70 + int(key[1:]) - 1 if key[0] == "F" and len(key) > 1 else ord(key)


def parse_game_cmdline(cmdline):
    """(install root, out dir name) from '<exe> <root>\\analysis\\<name>\\boot-linked.bin ...', else
    None. The root starts at the LAST drive letter before 'analysis' (the exe path comes first)."""
    m = re.search(r'\\analysis\\([^\\"]+)\\boot-linked\.bin', cmdline)
    drives = list(re.finditer(r"[A-Za-z]:\\", cmdline[:m.start()])) if m else []
    return (cmdline[drives[-1].start():m.start()], m.group(1)) if drives else None


def describe_exit_code(code):
    if code is None:
        return "unknown (no exit code could be read)"
    code &= 0xFFFFFFFF
    names = {0xC0000005: "access violation", 0xC0000409: "fast fail / stack buffer overrun",
             0xC000013A: "closed by Ctrl+C / console close", 0xC0000374: "heap corruption",
             0xC00000FD: "stack overflow", 0xC0000094: "integer divide by zero",
             0xC000001D: "illegal instruction", 0xC0000142: "DLL initialization failed"}
    if code == 0:
        return "0 (clean exit)"
    if code in names:
        return f"0x{code:08X} CRASH: {names[code]}"
    if code >= 0xC0000000:
        return f"0x{code:08X} CRASH: Windows error status"
    if code == 1:
        return "1 (non-zero; also what the launcher's STOP, taskkill /F, leaves)"
    return f"{code} (0x{code:X}, non-zero)"


def summarize_frame_stats(windows):
    """windows: parse_frame_stats() dicts, one per 5 s of the game. Counts are of 5 s windows whose
    worst frame was over the limit (the log keeps only each window's worst frame)."""
    if not windows:
        return {}
    fps = [w["fps"] for w in windows]
    return {"windows": len(windows), "fps_min": min(fps), "fps_median": statistics.median(fps),
            "fps_avg": sum(fps) / len(fps), "worst_ms": max(w["worst_ms"] for w in windows),
            "over_40": sum(w["worst_ms"] > 40 for w in windows),
            "over_100": sum(w["worst_ms"] > 100 for w in windows),
            "compiles": sum(w["compiles"] for w in windows)}


def column_stats(rows, column):
    values = [float(r[column]) for r in rows if r.get(column) not in (None, "")]
    return (sum(values) / len(values), max(values)) if values else None


METRIC_COLUMNS = [("cpu_pct", "Process CPU % (of all cores)", "{:.1f}"),
                  ("cpu_cores", "Process CPU (cores busy)", "{:.2f}"),
                  ("ws_mb", "Working set MB", "{:.0f}"), ("private_mb", "Private bytes MB", "{:.0f}"),
                  ("threads", "Threads", "{:.0f}"), ("handles", "Handles", "{:.0f}"),
                  ("gpu_util", "GPU util %", "{:.0f}"), ("gpu_mem_mb", "GPU memory MB", "{:.0f}"),
                  ("gpu_clock_mhz", "GPU clock MHz", "{:.0f}"), ("gpu_temp_c", "GPU temp C", "{:.0f}"),
                  ("gpu_power_w", "GPU power W", "{:.0f}")]


def build_summary(info):
    """summary.md text from a dict: name, folder, start, end, duration_s, exit_code, game_ended,
    marks [{label, time, elapsed_s, folder}], frames (summarize_frame_stats), metrics (rows),
    top_repeats [(line, n)], dropped, notes [str], files [str]."""
    out = [f"# Session {info['name']}", "",
           f"- Started: {info['start']}", f"- Ended: {info['end']}",
           f"- Duration: {info['duration_s'] / 60:.1f} min ({info['duration_s']:.0f} s)"]
    if info.get("game_ended", True):
        out.append(f"- Exit code: {describe_exit_code(info['exit_code'])}")
    else:
        out.append("- The recorder was stopped while the game was still running (no exit code).")
    out += ["", f"## Marks ({len(info['marks'])})", ""]
    out += [f"- {m['label']} at {m['time']} (+{m['elapsed_s']:.0f} s): `{m['folder']}`" for m in info["marks"]] \
        or ["- none"]
    f = info.get("frames") or {}
    out += ["", "## Frame times (from the log's 5 s Frame stats lines)", ""]
    if f:
        out += [f"- FPS: min {f['fps_min']:.1f}, median {f['fps_median']:.1f}, average {f['fps_avg']:.1f} "
                f"over {f['windows']} windows",
                f"- Worst single frame: {f['worst_ms']:.1f} ms",
                f"- 5 s windows with a worst frame over 40 ms: {f['over_40']}, over 100 ms: {f['over_100']}",
                f"- Shader/pipeline compiles: {f['compiles']}"]
    else:
        out.append("- no Frame stats lines in the log")
    out += ["", "## Resources (average / peak)", ""]
    shown = False
    for column, title, fmt in METRIC_COLUMNS:
        stats = column_stats(info["metrics"], column)
        if stats:
            out.append(f"- {title}: {fmt.format(stats[0])} / {fmt.format(stats[1])}")
            shown = True
    if not shown:
        out.append("- no samples")
    out += ["", "## Most repeated warning/error lines", ""]
    out += [f"- {n}x `{line[:200]}`" for line, n in info["top_repeats"]] or ["- none"]
    if info.get("dropped"):
        out.append(f"- ({info['dropped']} further repeats were counted but not written to events.txt)")
    if info.get("notes"):
        out += ["", "## Notes", ""] + [f"- {n}" for n in info["notes"]]
    out += ["", "## What to send / check", "",
            f"Send the whole folder `{info['folder']}` (zip it; the `marks` folders hold the screenshots and "
            "engine dumps and are the biggest part). Start with this file, then `events.txt`; `boot.timed.log` "
            "is the full log with times; `metrics.csv` has CPU/memory/GPU every few seconds.",
            "Log times are when the recorder saw a line (the game's log has no times), accurate to about 0.5 s.",
            "", "Files: " + ", ".join(info["files"])]
    return "\n".join(out) + "\n"


# ---------------------------------------------------------------------------------------------
# Windows API (guarded: the helpers above import everywhere)

if IS_WIN:
    from ctypes import wintypes
    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    u32 = ctypes.WinDLL("user32", use_last_error=True)
    g32 = ctypes.WinDLL("gdi32", use_last_error=True)

    def _api(dll, name, res, *args):
        fn = getattr(dll, name)
        fn.restype, fn.argtypes = res, list(args)
        return fn
    H, DW, VP = wintypes.HANDLE, wintypes.DWORD, ctypes.c_void_p

    class FILETIME(ctypes.Structure):
        _fields_ = [("lo", DW), ("hi", DW)]

    class PMC(ctypes.Structure):
        _fields_ = [("cb", DW), ("PageFaultCount", DW)] + [(n, ctypes.c_size_t) for n in (
            "PeakWorkingSetSize", "WorkingSetSize", "QuotaPeakPagedPoolUsage", "QuotaPagedPoolUsage",
            "QuotaPeakNonPagedPoolUsage", "QuotaNonPagedPoolUsage", "PagefileUsage",
            "PeakPagefileUsage", "PrivateUsage")]

    class MEMSTAT(ctypes.Structure):
        _fields_ = [("dwLength", DW), ("dwMemoryLoad", DW)] + [(n, ctypes.c_ulonglong) for n in (
            "total", "avail", "totalpage", "availpage", "totalvirt", "availvirt", "availext")]

    class PROCENTRY(ctypes.Structure):
        _fields_ = [("dwSize", DW), ("cntUsage", DW), ("pid", DW), ("heap", ctypes.c_size_t),
                    ("module", DW), ("threads", DW), ("parent", DW), ("pri", ctypes.c_long),
                    ("flags", DW), ("exe", ctypes.c_wchar * 260)]

    class BMI(ctypes.Structure):  # BITMAPINFOHEADER + room for the colour table GetDIBits may touch
        _fields_ = [("biSize", DW), ("biWidth", ctypes.c_long), ("biHeight", ctypes.c_long),
                    ("biPlanes", wintypes.WORD), ("biBitCount", wintypes.WORD), ("biCompression", DW),
                    ("biSizeImage", DW), ("biXPels", ctypes.c_long), ("biYPels", ctypes.c_long),
                    ("biClrUsed", DW), ("biClrImportant", DW), ("colors", DW * 3)]

    OpenProcess = _api(k32, "OpenProcess", H, DW, wintypes.BOOL, DW)
    CloseHandle = _api(k32, "CloseHandle", wintypes.BOOL, H)
    WaitForSingleObject = _api(k32, "WaitForSingleObject", DW, H, DW)
    GetExitCodeProcess = _api(k32, "GetExitCodeProcess", wintypes.BOOL, H, ctypes.POINTER(DW))
    GetProcessTimes = _api(k32, "GetProcessTimes", wintypes.BOOL, H, *[ctypes.POINTER(FILETIME)] * 4)
    GetProcessHandleCount = _api(k32, "GetProcessHandleCount", wintypes.BOOL, H, ctypes.POINTER(DW))
    QueryImageName = _api(k32, "QueryFullProcessImageNameW", wintypes.BOOL, H, DW, wintypes.LPWSTR,
                          ctypes.POINTER(DW))
    GetProcessMemoryInfo = _api(k32, "K32GetProcessMemoryInfo", wintypes.BOOL, H, ctypes.POINTER(PMC), DW)
    GlobalMemoryStatusEx = _api(k32, "GlobalMemoryStatusEx", wintypes.BOOL, ctypes.POINTER(MEMSTAT))
    Snapshot = _api(k32, "CreateToolhelp32Snapshot", H, DW, DW)
    Process32First = _api(k32, "Process32FirstW", wintypes.BOOL, H, ctypes.POINTER(PROCENTRY))
    Process32Next = _api(k32, "Process32NextW", wintypes.BOOL, H, ctypes.POINTER(PROCENTRY))
    SetPriorityClass = _api(k32, "SetPriorityClass", wintypes.BOOL, H, DW)
    GetCurrentProcess = _api(k32, "GetCurrentProcess", H)
    GetStdHandle = _api(k32, "GetStdHandle", H, DW)
    GetConsoleMode = _api(k32, "GetConsoleMode", wintypes.BOOL, H, ctypes.POINTER(DW))
    SetConsoleMode = _api(k32, "SetConsoleMode", wintypes.BOOL, H, DW)
    WNDENUMPROC = ctypes.WINFUNCTYPE(wintypes.BOOL, H, wintypes.LPARAM)
    EnumWindows = _api(u32, "EnumWindows", wintypes.BOOL, WNDENUMPROC, wintypes.LPARAM)
    GetWindowThreadProcessId = _api(u32, "GetWindowThreadProcessId", DW, H, ctypes.POINTER(DW))
    IsWindowVisible = _api(u32, "IsWindowVisible", wintypes.BOOL, H)
    GetWindowRect = _api(u32, "GetWindowRect", wintypes.BOOL, H, ctypes.POINTER(wintypes.RECT))
    GetWindowTextW = _api(u32, "GetWindowTextW", ctypes.c_int, H, wintypes.LPWSTR, ctypes.c_int)
    GetForegroundWindow = _api(u32, "GetForegroundWindow", H)
    GetSystemMetrics = _api(u32, "GetSystemMetrics", ctypes.c_int, ctypes.c_int)
    GetDC = _api(u32, "GetDC", H, H)
    ReleaseDC = _api(u32, "ReleaseDC", ctypes.c_int, H, H)
    RegisterHotKey = _api(u32, "RegisterHotKey", wintypes.BOOL, H, ctypes.c_int, wintypes.UINT, wintypes.UINT)
    UnregisterHotKey = _api(u32, "UnregisterHotKey", wintypes.BOOL, H, ctypes.c_int)
    GetMessageW = _api(u32, "GetMessageW", ctypes.c_int, ctypes.POINTER(wintypes.MSG), H, wintypes.UINT, wintypes.UINT)
    PostThreadMessageW = _api(u32, "PostThreadMessageW", wintypes.BOOL, DW, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM)
    CreateCompatibleDC = _api(g32, "CreateCompatibleDC", H, H)
    CreateCompatibleBitmap = _api(g32, "CreateCompatibleBitmap", H, H, ctypes.c_int, ctypes.c_int)
    SelectObject = _api(g32, "SelectObject", H, H, H)
    BitBlt = _api(g32, "BitBlt", wintypes.BOOL, H, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                  H, ctypes.c_int, ctypes.c_int, DW)
    GetDIBits = _api(g32, "GetDIBits", ctypes.c_int, H, H, wintypes.UINT, wintypes.UINT, VP,
                     ctypes.POINTER(BMI), wintypes.UINT)
    DeleteObject = _api(g32, "DeleteObject", wintypes.BOOL, H)
    DeleteDC = _api(g32, "DeleteDC", wintypes.BOOL, H)
    INVALID = ctypes.c_void_p(-1).value
    EPOCH_DIFF_100NS = 116444736000000000


def _ft(ft):
    return (ft.hi << 32) | ft.lo


def snapshot_processes():
    """[(pid, exe name, thread count)] of every running process (Toolhelp: no per-process rights)."""
    result = []
    snap = Snapshot(2, 0)
    if not snap or snap == INVALID:
        return result
    entry = PROCENTRY()
    entry.dwSize = ctypes.sizeof(entry)
    ok = Process32First(snap, ctypes.byref(entry))
    while ok:
        result.append((entry.pid, entry.exe, entry.threads))
        ok = Process32Next(snap, ctypes.byref(entry))
    CloseHandle(snap)
    return result


def find_pid(name):
    for pid, exe, _ in snapshot_processes():
        if exe.lower() == name.lower():
            return pid
    return None


def window_title(hwnd):
    buf = ctypes.create_unicode_buffer(256)
    GetWindowTextW(hwnd, buf, 256)
    return buf.value


def foreground_title():
    return window_title(GetForegroundWindow())


def find_game_window(pid):
    """(hwnd, (left, top, right, bottom), title) of the largest visible top-level window of the pid."""
    best = []

    def visit(hwnd, _):
        owner = DW()
        GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        rect = wintypes.RECT()
        if owner.value == pid and IsWindowVisible(hwnd) and GetWindowRect(hwnd, ctypes.byref(rect)):
            best.append(((rect.right - rect.left) * (rect.bottom - rect.top), hwnd,
                         (rect.left, rect.top, rect.right, rect.bottom)))
        return True
    EnumWindows(WNDENUMPROC(visit), 0)
    if not best:
        return None
    _, hwnd, rect = max(best)
    return hwnd, rect, window_title(hwnd)


def grab_screen(left, top, width, height):
    """Raw BGRA of a screen rectangle (BitBlt from the screen DC), top row first."""
    screen = GetDC(None)
    mem = CreateCompatibleDC(screen)
    bitmap = CreateCompatibleBitmap(screen, width, height)
    old = SelectObject(mem, bitmap)
    try:
        # CAPTUREBLT (0x40000000) also copies layered windows (overlays) that plain SRCCOPY skips.
        if not BitBlt(mem, 0, 0, width, height, screen, left, top, 0x00CC0020 | 0x40000000):
            raise OSError("BitBlt failed")
        bmi = BMI()
        bmi.biSize, bmi.biWidth, bmi.biHeight = 40, width, -height  # negative: top-down
        bmi.biPlanes, bmi.biBitCount = 1, 32
        buf = ctypes.create_string_buffer(width * height * 4)
        if not GetDIBits(mem, bitmap, 0, height, buf, ctypes.byref(bmi), 0):
            raise OSError("GetDIBits failed")
        return bytes(buf)
    finally:
        SelectObject(mem, old)
        DeleteObject(bitmap)
        DeleteDC(mem)
        ReleaseDC(None, screen)


def take_screenshot(pid):
    """(png bytes or None, info). The game window's rectangle from the screen; an all-black result
    (exclusive fullscreen / protected surface) is retried once from the whole primary screen."""
    info = {"source": "window", "black_retry": False}
    found = find_game_window(pid)
    full = (0, 0, GetSystemMetrics(0), GetSystemMetrics(1))
    rect = found[1] if found else None
    if not found or rect[2] - rect[0] < 16 or rect[3] - rect[1] < 16 or rect[0] <= -30000:
        rect = full
        info.update(source="fullscreen", note="no usable game window for the pid (minimized or not created)")
    while True:
        left, top, right, bottom = rect
        w, h = right - left, bottom - top
        rgb = bgra_to_rgb(grab_screen(left, top, w, h))
        info.update(width=w, height=h, rect=list(rect))
        black = is_all_black(rgb)
        if not black or info["source"] == "fullscreen":
            info["all_black"] = black
            return png_encode(w, h, rgb), info
        rect, info["source"], info["black_retry"] = full, "fullscreen", True


# ---------------------------------------------------------------------------------------------
# Small shared helpers

def run_text(cmd, timeout=15):
    """Stdout of a console tool (OEM code page, as Windows tools print), '' when it fails."""
    try:
        r = subprocess.run(cmd, capture_output=True, timeout=timeout,
                           creationflags=0x08000000 if IS_WIN else 0)
        return r.stdout.decode("oem" if IS_WIN else "utf-8", "replace")
    except (OSError, subprocess.SubprocessError):
        return ""


def powershell(command, timeout=20):
    return run_text(["powershell", "-NoProfile", "-NonInteractive", "-Command", command], timeout).strip()


def find_nvidia_smi():
    for path in (shutil.which("nvidia-smi"), r"C:\Windows\System32\nvidia-smi.exe",
                 r"C:\Program Files\NVIDIA Corporation\NVSMI\nvidia-smi.exe"):
        if path and Path(path).exists():
            return path
    return None


def beep(pattern):
    """Audible feedback (the game is fullscreen): ok = one short, done = two, fail = low and long."""
    def play():
        try:
            import winsound
            if pattern == "fail":
                winsound.Beep(300, 600)
            else:
                winsound.Beep(1000, 110)
                if pattern == "done":
                    time.sleep(0.08)
                    winsound.Beep(1400, 110)
        except Exception:
            pass
    threading.Thread(target=play, daemon=True).start()


class TextSink:
    """An append-only text file flushed after every write, with a size cap."""

    def __init__(self, path):
        self.f, self.size, self.capped = open(path, "w", encoding="utf-8", newline="\n"), 0, False

    def write(self, text):
        if self.capped:
            return
        if self.size + len(text) > SIZE_CAP:
            self.capped = True
            text = f"[size cap {SIZE_CAP // 2 ** 20} MB reached; later lines are not written]\n"
        self.f.write(text)
        self.f.flush()
        self.size += len(text)

    def close(self):
        self.f.close()


def stamp(t=None):
    t = time.time() if t is None else t
    return time.strftime("%H:%M:%S", time.localtime(t)) + f".{int(t * 1000) % 1000:03d}"


def local_iso(t):
    return datetime.datetime.fromtimestamp(t).astimezone().isoformat(timespec="seconds")


# ---------------------------------------------------------------------------------------------
# One recorded game run

class Session:
    def __init__(self, rec, pid):
        self.rec, self.args, self.pid = rec, rec.args, pid
        self.handle, self.stop = None, threading.Event()
        self.lock = threading.Lock()                      # ring, counters, latest values
        self.ring = deque(maxlen=RING_LINES)
        self.event_log, self.windows, self.notes = EventLog(), [], []
        self.metrics, self.marks, self.mark_count, self.dumps_taken = [], [], 0, 0
        self.last_stats_line, self.last_row, self.gpu = "", {}, {}
        self.threads, self.exit_code, self.game_ended, self.tailer = [], None, False, None
        self.dir = None

    # -- infrastructure
    def log(self, text):
        try:
            if self.dir:
                with open(self.dir / "recorder.log", "a", encoding="utf-8") as f:
                    f.write(f"{stamp()} {text}\n")
        except OSError:
            pass

    def spawn(self, name, fn, *a):
        def guarded():
            try:
                fn(*a)
            except Exception:
                self.log(f"{name} failed:\n{traceback.format_exc()}")
        t = threading.Thread(target=guarded, name=name, daemon=True)
        t.start()
        return t

    def step(self, name, fn):
        try:
            fn()
        except Exception:
            self.log(f"finalize step {name} failed:\n{traceback.format_exc()}")
            self.notes.append(f"finalize step '{name}' failed (see recorder.log)")

    def alive(self):
        if self.handle:
            return WaitForSingleObject(self.handle, 0) == 0x102
        return any(p == self.pid for p, _, _ in snapshot_processes())

    def low_disk(self):
        return shutil.disk_usage(self.dir).free < MIN_FREE_BYTES

    # -- start
    def run(self):
        self.handle = OpenProcess(0x1000 | 0x00100000, False, self.pid) or None  # QUERY_LIMITED | SYNCHRONIZE
        creation, exit_t, kern, user = FILETIME(), FILETIME(), FILETIME(), FILETIME()
        self.start_epoch = time.time()
        if self.handle and GetProcessTimes(self.handle, *[ctypes.byref(x) for x in (creation, exit_t, kern, user)]):
            self.start_epoch = (_ft(creation) - EPOCH_DIFF_100NS) / 1e7
        image = ctypes.create_unicode_buffer(1024)
        size = DW(1024)
        self.image = Path(image.value) if self.handle and QueryImageName(self.handle, 0, image, ctypes.byref(size)) else None
        self.cmdline = powershell(f"(Get-CimInstance Win32_Process -Filter 'ProcessId={self.pid}').CommandLine")
        found = parse_game_cmdline(self.cmdline)
        self.out = Path(self.args.out_dir) if self.args.out_dir else (
            Path(found[0]) / "analysis" / found[1] if found else None)
        if self.args.root:
            self.root = Path(self.args.root)
        elif found:
            self.root = Path(found[0])
        elif self.image and len(self.image.parents) > 3:   # <root>\gow3_pc\out\Uncharted.exe
            self.root = self.image.parents[2]
        else:
            self.root = Path(__file__).resolve().parent.parent
        self.user = self.root / "analysis" / "uncharted-user"
        if self.out is None:
            self.out = self.root / "analysis" / "uncharted"
            self.notes.append(f"game command line has no boot-linked.bin path; assumed out dir {self.out}")
        sessions = Path(self.args.sessions_dir) if self.args.sessions_dir else self.user / "sessions"
        base = time.strftime("%Y%m%d-%H%M%S", time.localtime(self.start_epoch)) + "_" + self.out.name
        self.dir = sessions / base
        n = 1
        while self.dir.exists():
            n += 1
            self.dir = sessions / f"{base}-{n}"
        self.dir.mkdir(parents=True)
        self.name = self.dir.name
        self.rec.say(f"Recording {self.name}  (game pid {self.pid}); marks: {self.rec.keys_text}")
        self.timed = TextSink(self.dir / "boot.timed.log")
        self.events = TextSink(self.dir / "events.txt")
        self.csv_file = open(self.dir / "metrics.csv", "w", encoding="utf-8", newline="")
        self.csv = csv.writer(self.csv_file)
        self.csv.writerow(COLUMNS)
        self.csv_file.flush()
        self.tailer = LogTailer(self.out / LOG_NAME)
        self.nvidia = find_nvidia_smi()
        self.threads = [self.spawn("tail", self.tail_loop), self.spawn("metrics", self.metrics_loop),
                        self.spawn("session.json", self.write_session_json)]
        if self.nvidia:
            self.threads.append(self.spawn("gpu", self.gpu_loop))
        self.rec.current = self
        try:
            while self.alive() and not self.stop.wait(0.5):
                pass
            self.game_ended = not self.alive()
        except KeyboardInterrupt:
            self.game_ended = False
            self.stop.set()
            raise
        finally:
            self.finalize()

    # -- workers
    def tail_loop(self):
        late = time.time() - self.start_epoch > 5   # attached to a game that was already running

        def drain(flush=False):
            for line in self.tailer.poll(flush):
                t = "--:--:--.---" if late else stamp()   # lines already there: their time is unknown
                with self.lock:
                    self.ring.append(f"{t} | {line}")
                    stats = parse_frame_stats(line)
                    if stats:
                        self.windows.append(stats)
                        self.last_stats_line = line
                self.timed.write(f"{t} | {line}\n")
                kind = classify_line(line)
                if kind:
                    verdict = self.event_log.offer(line) if kind == "event" else True
                    if verdict is True:
                        self.events.write(f"{t} | {line}\n")
                    elif verdict == "note":
                        self.events.write(f"{t} | (the line above repeats; further repeats are only counted)\n")
        drain()
        if late:
            late = False
            self.notes.append("the recorder attached after the game started: log lines stamped --:--:--.--- "
                              "were already in the log (their time is unknown)")
        while not self.stop.wait(0.5):
            drain()
        time.sleep(0.7)  # the game's last writes land just after the process ends
        drain(flush=True)

    def gpu_loop(self):
        fields = "index,utilization.gpu,memory.used,clocks.gr,temperature.gpu,power.draw"
        self.smi = subprocess.Popen([self.nvidia, f"--query-gpu={fields}", "--format=csv,noheader,nounits", "-lms", "2000"],
                                    stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True,
                                    creationflags=0x08000000 if IS_WIN else 0)
        for line in self.smi.stdout:
            cells = [c.strip() for c in line.split(",")]
            if len(cells) == 6:
                with self.lock:
                    self.gpu[cells[0]] = cells[1:]

    def gpu_cells(self):
        with self.lock:   # several GPUs: the one with the most memory in use is the game's
            rows = sorted(self.gpu.values(), key=lambda r: -float(r[1]) if r[1].replace(".", "").isdigit() else 0)
        return [("" if c.startswith("[") or c.lower() == "n/a" else c) for c in rows[0]] if rows else [""] * 5

    def sample(self, prev):
        now = time.time()
        row = {"time": stamp(now), "elapsed_s": f"{now - self.start_epoch:.1f}"}
        if self.handle:
            creation, exit_t, kern, user = FILETIME(), FILETIME(), FILETIME(), FILETIME()
            if GetProcessTimes(self.handle, *[ctypes.byref(x) for x in (creation, exit_t, kern, user)]):
                cpu = (_ft(kern) + _ft(user)) / 1e7
                if prev:
                    cores = (cpu - prev[0]) / max(now - prev[1], 1e-3)
                    row["cpu_cores"] = f"{cores:.2f}"
                    row["cpu_pct"] = f"{100 * cores / (os.cpu_count() or 1):.1f}"
                prev[:] = [cpu, now]
            pmc = PMC()
            pmc.cb = ctypes.sizeof(pmc)
            if GetProcessMemoryInfo(self.handle, ctypes.byref(pmc), pmc.cb):
                row["ws_mb"], row["private_mb"] = f"{pmc.WorkingSetSize / 2**20:.0f}", f"{pmc.PrivateUsage / 2**20:.0f}"
            count = DW()
            if GetProcessHandleCount(self.handle, ctypes.byref(count)):
                row["handles"] = count.value
        row["threads"] = next((t for p, _, t in snapshot_processes() if p == self.pid), "")
        mem = MEMSTAT()
        mem.dwLength = ctypes.sizeof(mem)
        if GlobalMemoryStatusEx(ctypes.byref(mem)):
            row["sys_free_ram_mb"] = f"{mem.avail / 2**20:.0f}"
        row.update(zip(("gpu_util", "gpu_mem_mb", "gpu_clock_mhz", "gpu_temp_c", "gpu_power_w"), self.gpu_cells()))
        row["foreground_window"] = foreground_title()
        return row

    def metrics_loop(self):
        prev = []
        while self.alive():   # after the exit the process counters read 0: no sample
            row = self.sample(prev)
            with self.lock:
                self.last_row = row
                self.metrics.append(row)
            self.csv.writerow([row.get(c, "") for c in COLUMNS])
            self.csv_file.flush()
            if self.stop.wait(self.args.interval):
                return

    # -- session.json
    def write_session_json(self):
        info = {"recorder_version": RECORDER_VERSION, "start_local": local_iso(self.start_epoch),
                "start_utc": datetime.datetime.fromtimestamp(self.start_epoch, datetime.timezone.utc).isoformat(timespec="seconds"),
                "recorder_attached_local": local_iso(time.time()), "pid": self.pid,
                "install_root": str(self.root), "out_dir": str(self.out),
                "process_command_line": self.cmdline, "game_exe": {}, "windows": {}, "cpu": {}, "gpu": {}}
        write = lambda: (self.dir / "session.json").write_text(json.dumps(info, indent=2, default=str), encoding="utf-8")
        write()   # the sections below are slow (PowerShell, hashing); the file is usable after each
        mem = MEMSTAT()
        mem.dwLength = ctypes.sizeof(mem)
        GlobalMemoryStatusEx(ctypes.byref(mem))
        import platform
        info["windows"] = {"platform": platform.platform(), "version": platform.version(),
                           "power_plan": run_text(["powercfg", "/getactivescheme"]).strip()}
        cpu_name = ""
        try:
            import winreg
            with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r"HARDWARE\DESCRIPTION\System\CentralProcessor\0") as k:
                cpu_name = winreg.QueryValueEx(k, "ProcessorNameString")[0].strip()
        except OSError:
            pass
        info["cpu"] = {"name": cpu_name, "logical_cores": os.cpu_count(), "ram_total_mb": mem.total // 2**20}
        info["display"] = {"primary_resolution": f"{GetSystemMetrics(0)}x{GetSystemMetrics(1)}"}
        if self.nvidia:
            q = "index,name,driver_version,memory.total,clocks.max.gr,clocks.max.mem,power.limit,pcie.link.gen.current"
            text = run_text([self.nvidia, f"--query-gpu={q}", "--format=csv"])
            info["gpu"] = {"source": "nvidia-smi", "csv": text.strip().splitlines()}
        else:
            info["gpu"] = {"source": "Win32_VideoController", "json": powershell(
                "Get-CimInstance Win32_VideoController | Select-Object Name,DriverVersion,AdapterRAM,"
                "CurrentHorizontalResolution,CurrentVerticalResolution,CurrentRefreshRate | ConvertTo-Json -Compress")}
        write()
        exe = self.image if self.image and self.image.exists() else None
        if exe:
            st = exe.stat()
            info["game_exe"] = {"path": str(exe), "size": st.st_size, "mtime": local_iso(st.st_mtime)}
            write()
            digest = hashlib.sha256()
            with open(exe, "rb") as f:
                for block in iter(lambda: f.read(1 << 20), b""):
                    digest.update(block)
            info["game_exe"]["sha256"] = digest.hexdigest()
        version = self.root / "VERSION"
        info["package_version"] = version.read_text(encoding="utf-8", errors="replace").strip() if version.exists() else None
        info["git_commit"] = self.git_head()
        launcher = Path(os.environ.get("APPDATA", "")) / "Uncharted Launcher" / "launcher.json"
        try:
            info["launcher_json"] = json.loads(launcher.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            info["launcher_json"] = None
        info["recorder_visible_env"] = {k: v for k, v in os.environ.items() if k.startswith(("GOW3_", "UNCHARTED_"))}
        info["recorder_visible_env_note"] = ("These are the RECORDER's environment variables. The game's own "
                                             "environment cannot be read from outside; probe_uncharted.py builds it "
                                             "from the ini files copied into config\\ plus test-env.txt.")
        config = self.dir / "config"
        config.mkdir(exist_ok=True)
        for name in ("probe-u1.ini", "probe-u2.ini", "probe-u3.ini", "test-env.txt", "disabled-optimizations.txt"):
            if (self.user / name).exists():
                shutil.copy2(self.user / name, config / name)
        write()

    def git_head(self):
        head = self.root / ".git" / "HEAD"
        try:
            text = head.read_text().strip()
            if text.startswith("ref:"):
                ref = text.split(None, 1)[1]
                path = self.root / ".git" / ref
                if path.exists():
                    return path.read_text().strip()
                for line in (self.root / ".git" / "packed-refs").read_text().splitlines():
                    if line.endswith(" " + ref):
                        return line.split()[0]
                return None
            return text
        except OSError:
            return None

    # -- hotkey marks
    def mark(self, label, engine_dump=True):
        with self.lock:
            self.mark_count += 1
            number = self.mark_count
        now = time.time()
        folder = self.dir / "marks" / f"{number:02d}_{label}_{time.strftime('%H-%M-%S', time.localtime(now))}"
        folder.mkdir(parents=True)
        with self.lock:
            tail = list(self.ring)
            data = {"label": label, "number": number, "local_time": stamp(now),
                    "elapsed_s": round(now - self.start_epoch, 1), "log_byte_offset": self.tailer.offset,
                    "log_line_number": self.tailer.lineno, "last_frame_stats": self.last_stats_line,
                    "latest_metrics": dict(self.last_row)}
        data["foreground_window"] = foreground_title()
        (folder / "log_tail.txt").write_text("\n".join(tail) + "\n", encoding="utf-8")
        failed = False
        found = find_game_window(self.pid)
        data["game_window"] = {"rect": list(found[1]), "title": found[2]} if found else None
        if self.low_disk():
            data["screenshot"] = {"skipped": "less than 2 GB free on the session drive"}
        else:
            try:
                png, info = take_screenshot(self.pid)
                (folder / "screen.png").write_bytes(png)
                data["screenshot"] = dict(info, file="screen.png")
            except Exception as e:
                failed = True
                data["screenshot"] = {"error": repr(e)}
                self.log(f"mark {number} screenshot failed:\n{traceback.format_exc()}")
        (folder / "mark.json").write_text(json.dumps(data, indent=2), encoding="utf-8")
        with open(self.dir / "markers.log", "a", encoding="utf-8") as f:
            f.write(f"{data['local_time']} {label} elapsed={data['elapsed_s']}s log_line={data['log_line_number']}\n")
        self.marks.append({"label": label, "time": data["local_time"], "elapsed_s": data["elapsed_s"],
                           "folder": str(folder.relative_to(self.dir))})
        beep("fail" if failed else "ok")
        dumped = False
        try:
            data["engine_dump"], dumped = self.engine_dump(folder, engine_dump)
        except Exception as e:
            data["engine_dump"] = {"status": f"error: {e!r}"}
            self.log(f"mark {number} engine dump failed:\n{traceback.format_exc()}")
        (folder / "mark.json").write_text(json.dumps(data, indent=2), encoding="utf-8")
        if dumped:
            beep("done")
        self.rec.say(f"mark {number} ({label}) saved: {folder}")

    def engine_dump(self, folder, wanted):
        """(info, True when files were captured). Creates <out>\\dumps\\trigger: the presenter removes it
        and writes the next frame's guest display buffer and presented image there."""
        dumps = self.out / "dumps"
        if not wanted:
            return {"status": "skipped", "reason": "--no-engine-dump"}, False
        if self.dumps_taken >= self.args.max_engine_dumps:
            return {"status": "skipped", "reason": f"per-session limit {self.args.max_engine_dumps} reached"}, False
        if self.low_disk():
            return {"status": "skipped", "reason": "less than 2 GB free on the session drive"}, False
        if not dumps.is_dir():
            return {"status": "skipped", "reason": f"{dumps} missing (the game was started without diagnostics)"}, False
        before = {p.name: (p.stat().st_size, p.stat().st_mtime) for p in dumps.iterdir() if p.is_file()}
        trigger = dumps / "trigger"
        trigger.touch()
        deadline, sizes, stable = time.time() + 4.0, None, 0
        while time.time() < deadline + (4.0 if sizes else 0):   # once files appear, give them 4 s more to finish
            time.sleep(0.2)
            new = {p.name: p.stat().st_size for p in dumps.iterdir()
                   if p.is_file() and p.name != "trigger" and not p.name.startswith("hang")
                   and (p.name not in before or before[p.name] != (p.stat().st_size, p.stat().st_mtime))}
            if new:
                stable = stable + 1 if new == sizes else 0
                sizes = new
                if stable >= 2:     # same sizes on two polls: the deferred GPU readback has finished
                    break
        trigger.unlink(missing_ok=True)  # never leave a request the game did not take
        if not sizes:
            return {"status": "no files appeared within 4 s",
                    "reason": "the game did not answer the trigger (not presenting frames, or no diagnostics)"}, False
        target = folder / "engine_dump"
        target.mkdir()
        for name in sizes:
            shutil.move(str(dumps / name), str(target / name))
        self.dumps_taken += 1
        return {"status": "ok", "files": sorted(sizes), "note": "raw 32-bit pixels, name_index_WxH.raw"}, True

    # -- finalize
    def finalize(self):
        self.stop.set()
        smi = getattr(self, "smi", None)
        if smi:   # first: its reader thread blocks until the child ends
            self.step("nvidia-smi stop", smi.terminate)
        for t in self.threads:
            t.join(timeout=30)
        end = time.time()
        for sink in (self.timed, self.events):
            self.step("close", sink.close)
        self.step("close", self.csv_file.close)
        if self.game_ended:
            self.exit_code = self.read_exit_code()
        self.step("copy files", lambda: self.copy_files(end))
        self.step("windows events", lambda: self.copy_app_events(end))
        top = self.event_log.top()
        files = sorted(str(p.relative_to(self.dir)).replace("\\", "/") for p in self.dir.rglob("*") if p.is_file())
        if self.tailer.resets:
            self.notes.append(f"the log shrank {self.tailer.resets} time(s) while recording (restarted from its start)")
        info = {"name": self.name, "folder": str(self.dir), "start": local_iso(self.start_epoch), "end": local_iso(end),
                "duration_s": end - self.start_epoch, "exit_code": self.exit_code, "game_ended": self.game_ended,
                "marks": self.marks, "frames": summarize_frame_stats(self.windows), "metrics": self.metrics,
                "top_repeats": top, "dropped": self.event_log.dropped(), "notes": self.notes, "files": files}
        self.step("summary.md", lambda: (self.dir / "summary.md").write_text(build_summary(info), encoding="utf-8"))
        if self.handle:
            CloseHandle(self.handle)
        self.rec.current = None
        self.rec.say(f"Session finished: {self.dir}  (exit code {describe_exit_code(self.exit_code)})")

    def read_exit_code(self):
        code = DW()
        from_handle = code.value if self.handle and GetExitCodeProcess(self.handle, ctypes.byref(code)) and code.value != 259 else None
        end = time.time() + 6   # probe_uncharted.py writes boot.json when the game ends
        boot = self.out / "boot.json"
        from_json = None
        while time.time() < end:
            try:
                if boot.stat().st_mtime >= self.start_epoch - 5:
                    from_json = json.loads(boot.read_text(encoding="utf-8")).get("exit_code")
                    break
            except (OSError, ValueError):
                pass
            if from_handle is not None:
                break
            time.sleep(0.3)
        if from_handle is not None and from_json is not None and (from_json & 0xFFFFFFFF) != from_handle:
            self.notes.append(f"exit code differs: process handle {from_handle}, boot.json {from_json}")
        return from_handle if from_handle is not None else from_json

    def copy_files(self, end):
        names = [(self.out, "boot.log"), (self.out, "boot.json"), (self.user, "frametimes.txt"),
                 (self.user, "framelog.txt"), (self.user, "launcher-last-run.log")]
        for folder, name in names:
            src = folder / name
            if not src.exists():
                continue
            if src.stat().st_mtime < self.start_epoch - 5:
                self.notes.append(f"{name} not copied: older than this session (left over from an earlier run)")
                continue
            shutil.copy2(src, self.dir / name)
        hang = self.dir / "hang"
        for folder in (self.out, self.out / "dumps"):
            for src in sorted(folder.glob("hang*")) if folder.is_dir() else []:
                if src.stat().st_mtime < self.start_epoch:
                    continue
                if src.stat().st_size <= 5 * 2**20:
                    hang.mkdir(exist_ok=True)
                    shutil.copy2(src, hang / src.name)
                else:
                    self.notes.append(f"hang file {src} ({src.stat().st_size // 2**20} MB) not copied; it is on disk")

    def copy_app_events(self, end):
        ms = int((end - self.start_epoch + 120) * 1000)
        text = run_text(["wevtutil", "qe", "Application", "/c:200", "/rd:true", "/f:text",
                         f"/q:*[System[TimeCreated[timediff(@SystemTime) <= {ms}]]]"], 30)
        blocks = [b for b in re.split(r"\r?\n(?=Event\[)", text) if "uncharted" in b.lower()][:5]
        (self.dir / "windows_events.txt").write_text(
            "\n\n".join(blocks) if blocks else "no Application-log event mentioning Uncharted in the session window\n",
            encoding="utf-8")


# ---------------------------------------------------------------------------------------------
# Recorder: process watcher + hotkeys

class Recorder:
    def __init__(self, args):
        self.args, self.current, self.last_press = args, None, {}
        self.keys = parse_keys(args.keys)
        self.keys_text = ", ".join(f"{k}={v}" for k, v in self.keys.items())
        self.say_lock = threading.Lock()

    def say(self, text):
        with self.say_lock:   # a console in QuickEdit selection mode blocks print; only short lines here
            print(f"[{stamp()}] {text}", flush=True)

    def on_hotkey(self, key):
        now = time.monotonic()
        if now - self.last_press.get(key, -99) < 3:
            return
        self.last_press[key] = now
        session = self.current
        if session is None:
            beep("fail")
            return self.say(f"{key} pressed but no game session is being recorded")
        try:
            session.mark(self.keys[key], engine_dump=not self.args.no_engine_dump)
        except Exception:
            beep("fail")
            session.log(f"mark {key} failed:\n{traceback.format_exc()}")

    def start_hotkeys(self):
        ready = threading.Event()

        def loop():
            self.hk_thread = k32.GetCurrentThreadId()
            registered = {}
            for i, (key, label) in enumerate(self.keys.items(), 1):
                if RegisterHotKey(None, i, 0x4000, virtual_key(key)):   # MOD_NOREPEAT
                    registered[i] = key
                else:
                    self.say(f"hotkey {key} ({label}) is taken by another application (error "
                             f"{ctypes.get_last_error()}); the other keys still work")
            ready.set()
            msg = wintypes.MSG()
            while GetMessageW(ctypes.byref(msg), None, 0, 0) > 0:
                if msg.message == 0x0312 and msg.wParam in registered:   # WM_HOTKEY
                    # not in the message loop: a mark takes seconds (PNG, engine dump)
                    threading.Thread(target=self.on_hotkey, args=(registered[msg.wParam],), daemon=True).start()
            for i in registered:
                UnregisterHotKey(None, i)
        threading.Thread(target=loop, daemon=True, name="hotkeys").start()
        ready.wait(5)

    def stop_hotkeys(self):
        if getattr(self, "hk_thread", None):
            PostThreadMessageW(self.hk_thread, 0x0012, 0, 0)   # WM_QUIT

    def run(self):
        self.say(f"Session recorder {RECORDER_VERSION}. Hotkeys while the game runs: {self.keys_text}"
                 " (one beep = captured, two = engine dump done, low beep = failed).")
        self.say("Close this window or press Ctrl+C to stop. Waiting for Uncharted.exe ...")
        self.start_hotkeys()
        try:
            while True:
                pid = self.args.attach_pid or find_pid(self.args.process_name)
                if pid:
                    try:
                        Session(self, pid).run()
                    except Exception:
                        traceback.print_exc()
                        self.say("the session could not be recorded (see above)")
                    if self.args.once or self.args.attach_pid:
                        return
                    while find_pid(self.args.process_name) == pid:   # the same process cannot start twice
                        time.sleep(1)
                    self.say("Waiting for the next launch ...")
                else:
                    time.sleep(1)
        except KeyboardInterrupt:
            self.say("Stopped.")
        finally:
            self.stop_hotkeys()


# ---------------------------------------------------------------------------------------------
# Self-test: fake root, dummy child process standing in for the game, no hotkeys, no real game

def selftest():
    with tempfile.TemporaryDirectory(prefix="session_recorder_test_", ignore_cleanup_errors=True) as tmp:
        root = Path(tmp) / "root"
        out = root / "analysis" / "fakegame"
        (root / "analysis" / "uncharted-user").mkdir(parents=True)
        (out / "dumps").mkdir(parents=True)
        (root / "analysis" / "uncharted-user" / "probe-u1.ini").write_text("[x]\n", encoding="utf-8")
        (root / "VERSION").write_text("selftest\n", encoding="utf-8")
        log = out / LOG_NAME
        log.write_text("Hello boot\n", encoding="utf-8")
        child = subprocess.Popen([sys.executable, "-c", "import sys,time; time.sleep(7); sys.exit(3)"])

        def write_log():
            for text in ["Frame stats: 61.2 FPS, worst frame 55.3 ms (vblank 60 Hz); 3 shader/pipeline compiles, 12.0 ms;",
                         "Warning: fake texture cache low"] + ["Warning: repeated line"] * 25:
                time.sleep(0.15)
                with open(log, "a", encoding="utf-8") as f:
                    f.write(text + "\n")

        def fake_engine():   # stands in for the presenter: takes the trigger, writes a "frame"
            for _ in range(150):
                time.sleep(0.1)
                if (out / "dumps" / "trigger").exists():
                    (out / "dumps" / "trigger").unlink()
                    (out / "dumps" / "display_000_2x2.raw").write_bytes(b"\x01" * 16)
                    return
        for fn in (write_log, fake_engine):
            threading.Thread(target=fn, daemon=True).start()
        args = argparse.Namespace(root=str(root), out_dir=str(out), sessions_dir=None, keys=DEFAULT_KEYS,
                                  max_engine_dumps=20, no_engine_dump=False, interval=1.0, once=True,
                                  attach_pid=child.pid, process_name="python.exe")
        rec = Recorder(args)
        def record():
            try:
                Session(rec, child.pid).run()
            except Exception:
                traceback.print_exc()
        runner = threading.Thread(target=record)
        runner.start()
        for _ in range(100):
            if rec.current and rec.current.dir and rec.current.dir.exists():
                break
            time.sleep(0.1)
        time.sleep(2.5)
        rec.current.mark("texture")   # what a hotkey press runs
        runner.join(timeout=60)
        child.wait(timeout=30)
        sessions = list((root / "analysis" / "uncharted-user" / "sessions").iterdir())
        assert len(sessions) == 1, sessions
        s = sessions[0]
        problems = []

        def check(cond, what):
            if not cond:
                problems.append(what)
        check(json.loads((s / "session.json").read_text(encoding="utf-8")).get("recorder_version") == RECORDER_VERSION, "session.json")
        check("Frame stats" in (s / "boot.timed.log").read_text(encoding="utf-8"), "boot.timed.log has the stats line")
        events = (s / "events.txt").read_text(encoding="utf-8")
        check("Warning: fake texture cache low" in events and "55.3 ms" in events, "events.txt has warning and slow frame")
        check(events.count("Warning: repeated line") == DUPLICATE_LIMIT, "events.txt collapses duplicates")
        rows = list(csv.DictReader(open(s / "metrics.csv", encoding="utf-8", newline="")))
        check(len(rows) >= 2, f"metrics.csv rows: {len(rows)}")
        marks = list((s / "marks").glob("*/mark.json"))
        check(len(marks) == 1 and json.loads(marks[0].read_text(encoding="utf-8"))["label"] == "texture", "mark.json")
        check(marks and (marks[0].parent / "screen.png").read_bytes()[:8] == b"\x89PNG\r\n\x1a\n", "screen.png signature")
        check(marks and (marks[0].parent / "engine_dump" / "display_000_2x2.raw").exists(), "engine dump moved into the mark")
        check(not (out / "dumps" / "trigger").exists(), "trigger left behind")
        summary = (s / "summary.md").read_text(encoding="utf-8")
        check("Exit code: 3 (" in summary, "summary has the exit code")
        if os.environ.get("SESSION_RECORDER_KEEP"):   # to look at a session by hand
            shutil.copytree(s, os.environ["SESSION_RECORDER_KEEP"])
        print("selftest:", "FAILED: " + "; ".join(problems) if problems else f"ok ({s})")
        return 1 if problems else 0


def main(argv=None):
    p = argparse.ArgumentParser(description="Session recorder for the Uncharted PC port (see the module docstring).")
    p.add_argument("--root", help="install root (default: derived from the running game's path)")
    p.add_argument("--out-dir", help="the game's analysis\\<name> folder holding boot.log (default: from its command line)")
    p.add_argument("--sessions-dir", help="where session folders go (default <root>\\analysis\\uncharted-user\\sessions)")
    p.add_argument("--keys", default=DEFAULT_KEYS, help="hotkeys, e.g. F7=texture,F8=geometry")
    p.add_argument("--max-engine-dumps", type=int, default=20)
    p.add_argument("--no-engine-dump", action="store_true")
    p.add_argument("--interval", type=float, default=2.0, help="seconds between metrics samples")
    p.add_argument("--once", action="store_true", help="record one session, then exit")
    p.add_argument("--selftest", action="store_true")
    p.add_argument("--attach-pid", type=int, help="(test) record this pid instead of looking for the game")
    p.add_argument("--process-name", default="Uncharted.exe")
    args = p.parse_args(argv)
    if not IS_WIN:
        sys.exit("The session recorder needs Windows.")
    u32.SetProcessDPIAware()           # real pixels for window rectangles and screenshots
    SetPriorityClass(GetCurrentProcess(), 0x4000)   # BELOW_NORMAL: never compete with the game
    mode = DW()
    handle = GetStdHandle(-10)
    if GetConsoleMode(handle, ctypes.byref(mode)):  # QuickEdit: a click in this window would freeze printing
        SetConsoleMode(handle, (mode.value & ~0x40) | 0x80)
    if args.selftest:
        return selftest()
    Recorder(args).run()
    return 0


if __name__ == "__main__":
    sys.exit(main())
