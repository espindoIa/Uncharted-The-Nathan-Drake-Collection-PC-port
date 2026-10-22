"""Play an Uncharted game from a script or an agent: start it, press buttons, move the sticks and
see the screen as it is now, in a loop (look, decide, act).

    python tools/play.py start u3 [--mt 1] [--res 3840x2160] [--env GOW3_FPS_LIMIT=0 ...]
    python tools/play.py gameplay              press Cross until the game is in gameplay
    python tools/play.py shot                  the newest frame as a PNG (path, frame number, age)
    python tools/play.py press cross           a short press, then a fresh shot (--no-shot skips it)
    python tools/play.py move f --ms 2000      left stick: f b l r fl fr bl br, or x,y in -1..1
    python tools/play.py look r --ms 400       right stick, same directions
    python tools/play.py do "r2 lx=128 ly=0" --ms 1500    any pad tokens held together
    python tools/play.py seq "left=f@800" "left=f cross@150"   steps held back to back (--burst 300)
    python tools/play.py wait 3                let the game run, then a shot
    python tools/play.py status                process, last frame stats, newest frame
    python tools/play.py mark | save <route> [--max-idle 1500]   start a route here | write the
                                               inputs since the mark (pauses between steps shortened)
    python tools/play.py replay <route> [--times N]        played back by the game itself
                         [--marks 10,30 --prefix P]        shots at those seconds and at the end
    python tools/play.py measure 20 [--route R] [--tag T]  frame times (port and PresentMon), GPU use, CPU threads
    python tools/play.py presentmon file.csv   the same PresentMon summary for any capture
    python tools/play.py compare a.png b.png   SSIM and changed pixels of two shots
    python tools/play.py stop

How it works: input goes through the runtime's scripted pad file (GOW3_PAD_FILE,
analysis/uncharted-user/probe-pad.txt, read every 20 ms); the screen comes from GOW3_LIVE_VIEW,
a scaled copy of the presented image the renderer writes every 200 ms (analysis/play/live), so
the window needs no focus and may be covered. Every input is also written to
analysis/play/trace.txt in the GOW3_PAD_REPLAY format (ms buttons lx ly rx ry l2 r2); "save"
turns the part since "mark" into a route the game replays with its own timing.
"""
import argparse
import json
import os
import shutil
import statistics
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
USER = ROOT / "analysis" / "uncharted-user"
PAD = USER / "probe-pad.txt"
PLAY = ROOT / "analysis" / "play"
LIVE = PLAY / "live"
SHOTS = PLAY / "shots"
TRACE = PLAY / "trace.txt"
STATE = PLAY / "session.json"
ROUTES = PLAY / "routes"
GAMES = {
    "u1": ("eboot.bin", "uncharted"),
    "u2": ("big2-ps4_Shipping.elf", "big2-ps4_Shipping"),
    "u3": ("big3-ps4_Shipping.elf", "big3-ps4_Shipping"),
}
# Pad bits as runtime_pad.c has them (the replay format stores them as they are).
BUTTONS = {
    "l3": 0x2, "r3": 0x4, "options": 0x8, "up": 0x10, "right": 0x20, "down": 0x40, "left": 0x80,
    "l2": 0x100, "r2": 0x200, "l1": 0x400, "r1": 0x800, "triangle": 0x1000, "circle": 0x2000,
    "cross": 0x4000, "square": 0x8000, "touchpad": 0x40000000, "touchpad_left": 0x40000000,
    "touchpad_right": 0x80000000,
}
DIRECTIONS = {
    "f": (0, -1), "b": (0, 1), "l": (-1, 0), "r": (1, 0),
    "fl": (-0.71, -0.71), "fr": (0.71, -0.71), "bl": (-0.71, 0.71), "br": (0.71, 0.71),
}
CREATE_NEW_PROCESS_GROUP = 0x200
DETACHED_PROCESS = 0x8


def load_state():
    # Another play.py may be replacing the file right now: retry before giving up.
    for _ in range(20):
        try:
            return json.loads(STATE.read_text(encoding="utf-8"))
        except FileNotFoundError:
            return {}
        except (OSError, ValueError):
            time.sleep(0.05)
    return {}


def save_state(state):
    PLAY.mkdir(parents=True, exist_ok=True)
    tmp = STATE.with_name(f"session.{os.getpid()}.tmp")
    tmp.write_text(json.dumps(state, indent=1), encoding="utf-8")
    for _ in range(20):
        try:
            os.replace(tmp, STATE)  # atomic: readers see the old or the new file, never half
            return
        except PermissionError:  # Windows: a reader has it open
            time.sleep(0.05)
    tmp.unlink(missing_ok=True)


def game_running():
    out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq Uncharted.exe", "/NH"],
                         capture_output=True, text=True).stdout
    return "Uncharted.exe" in out


def boot_log(state):
    game = state.get("game")
    return ROOT / "analysis" / GAMES[game][1] / "boot.log" if game in GAMES else None


def default_game_dir():
    if os.environ.get("UNCHARTED_GAME_DIR"):
        return os.environ["UNCHARTED_GAME_DIR"]
    config = Path(os.environ.get("APPDATA", "")) / "Uncharted Launcher" / "launcher.json"
    try:
        return json.loads(config.read_text(encoding="utf-8"))["game_dir"]
    except (OSError, ValueError, KeyError):
        sys.exit("No game folder: pass --game or set UNCHARTED_GAME_DIR")


# ---- input ---------------------------------------------------------------------------------

def parse_tokens(tokens):
    """Pad state of a token string: (buttons, [lx, ly, rx, ry], l2, r2)."""
    buttons, axes = 0, [128, 128, 128, 128]
    for token in tokens.split():
        if token in BUTTONS:
            buttons |= BUTTONS[token]
        for i, name in enumerate(("lx=", "ly=", "rx=", "ry=")):
            if token.startswith(name):
                axes[i] = max(0, min(255, int(token[3:])))
    return buttons, axes, 255 if buttons & BUTTONS["l2"] else 0, 255 if buttons & BUTTONS["r2"] else 0


def trace(state, tokens):
    start = state.get("trace_start")
    if start is None:
        return
    buttons, axes, l2, r2 = parse_tokens(tokens)
    ms = int((time.time() - start) * 1000)
    with TRACE.open("a", encoding="ascii") as f:
        f.write(f"{ms} {buttons} {axes[0]} {axes[1]} {axes[2]} {axes[3]} {l2} {r2}\n")


def write_pad(tokens):
    # The runtime notices a change by write time and size; both change on every write.
    PAD.write_text(tokens, encoding="ascii")


def hold(tokens, ms, args=None):
    """Holds the tokens for ms; with --shot-at, the shot is taken that far into the hold."""
    hold_steps([(tokens, ms)], args)


def hold_steps(steps, args=None):
    """Holds each (tokens, ms) in turn with no release between them (seq); --shot-at takes the shot
    that far in, --burst one every that many ms (a shot can delay the next step by ~0.1 s)."""
    state = load_state()
    if not game_running():
        sys.exit("The game is not running (python tools/play.py start <game>)")
    started = time.time()
    shots = []
    shot_at = getattr(args, "shot_at", 0) or 0
    burst = getattr(args, "burst", 0) or 0
    total = sum(ms for _, ms in steps)
    if 0 < shot_at < total:
        shots = [shot_at / 1000]
        args.no_shot = True  # the shot was taken while held
    elif burst:  # to see what a fast scene does
        shots = [i * burst / 1000 for i in range(1, int(total / burst) + 1) if i * burst < total]
    end = 0.0
    for tokens, ms in steps:
        write_pad(tokens)
        trace(state, tokens)
        end += ms / 1000
        while shots and shots[0] < end:
            time.sleep(max(0.0, shots.pop(0) - (time.time() - started)))
            shot(width=args.width, after=time.time() - 0.03)
        time.sleep(max(0.0, end - (time.time() - started)))
    write_pad("")
    trace(state, "")


def stick(direction, prefix):
    if direction in DIRECTIONS:
        x, y = DIRECTIONS[direction]
    else:
        x, y = (float(v) for v in direction.split(","))
    to_axis = lambda v: max(0, min(255, round(128 + v * 127.5)))
    return f"{prefix}x={to_axis(x)} {prefix}y={to_axis(y)}"


# ---- screen --------------------------------------------------------------------------------

def live_info():
    try:
        parts = (LIVE / "live.txt").read_text(encoding="ascii").split()
        return int(parts[1]), int(parts[3]), parts[4]
    except (OSError, ValueError, IndexError):
        return None


def shot(out=None, width=0, after=None, timeout=3.0, quiet=False):
    """Converts the newest live frame (taken after `after`, a time.time() value) to PNG."""
    from PIL import Image
    bmp = LIVE / "live.bmp"
    deadline = time.time() + timeout
    while True:
        try:
            fresh = bmp.stat().st_mtime >= (after or 0)
        except OSError:
            fresh = False
        if fresh or time.time() > deadline:
            break
        time.sleep(0.03)
    if not bmp.exists():
        sys.exit("No live frame yet (the game writes analysis/play/live/live.bmp once it presents)")
    for _ in range(5):
        try:
            image = Image.open(bmp)
            image.load()
            break
        except OSError:
            time.sleep(0.05)
    if width and image.width > width:
        image = image.resize((width, round(image.height * width / image.width)), Image.LANCZOS)
    SHOTS.mkdir(parents=True, exist_ok=True)
    state = load_state()
    index = state.get("shots", 0) + 1
    state["shots"] = index
    save_state(state)
    path = Path(out) if out else SHOTS / f"{index:04d}.png"
    path.parent.mkdir(parents=True, exist_ok=True)
    image.save(path)
    shutil.copyfile(path, PLAY / "shot.png")
    info = live_info()
    age = time.time() - bmp.stat().st_mtime
    if not quiet:
        frame = f"frame {info[0]}" if info else "frame ?"
        print(f"{path} ({frame}, {age:.2f} s old, {image.width}x{image.height})")
    return path


# ---- log -----------------------------------------------------------------------------------

def frame_stats(state, count=1):
    log = boot_log(state)
    if not log or not log.exists():
        return []
    lines = log.read_text(encoding="utf-8", errors="replace").splitlines()
    return [line for line in lines if line.startswith("Frame stats")][-count:]


def draws_per_frame(line):
    import re
    match = re.search(r"(\d+) draws/frame", line)
    return int(match.group(1)) if match else 0


# ---- commands ------------------------------------------------------------------------------

def cmd_start(args):
    if game_running():
        if not args.restart:
            sys.exit("Uncharted.exe is already running (--restart stops it first)")
        subprocess.run(["taskkill", "/IM", "Uncharted.exe", "/F"], capture_output=True)
        time.sleep(2)
    exe, folder = GAMES[args.game]
    (ROOT / "analysis" / folder / "boot.log").unlink(missing_ok=True)  # "gameplay" reads the new one
    shutil.rmtree(LIVE, ignore_errors=True)
    LIVE.mkdir(parents=True, exist_ok=True)
    write_pad("")
    TRACE.write_text("", encoding="ascii")
    env = dict(os.environ)
    env["GOW3_LIVE_VIEW"] = str(LIVE)
    env.setdefault("GOW3_LIVE_VIEW_MS", str(args.live_ms))
    if not args.input:
        # 19 Oct: the borderless window covers the desktop, and someone's mouse and keys there
        # turned the camera and paused U2 in the middle of a route. Only the pad file plays.
        env.setdefault("GOW3_KEYBOARD", "0")
        env.setdefault("GOW3_MOUSE", "0")
    key = args.game.upper()
    if args.mt is not None:
        env[f"UNCHARTED_MULTITHREADED_{key}"] = str(args.mt)
    if args.res:
        env[f"UNCHARTED_RESOLUTION_{key}"] = args.res
    for item in args.env:
        name, _, value = item.partition("=")
        env[name] = value
    log = (PLAY / "game.log").open("wb")
    process = subprocess.Popen(
        [sys.executable, "-u", "tools/probe_uncharted.py", "--boot", "--seconds", "0",
         "--executable", exe, "--game", args.game_dir or default_game_dir()],
        cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT,
        creationflags=CREATE_NEW_PROCESS_GROUP | DETACHED_PROCESS if os.name == "nt" else 0)
    save_state({"game": args.game, "pid": process.pid, "started": time.time(),
                "trace_start": time.time(), "mark": 0, "shots": 0})
    print(f"Started {args.game} (launcher pid {process.pid}); log {PLAY / 'game.log'}")


def cmd_stop(args):
    write_pad("")
    subprocess.run(["taskkill", "/IM", "Uncharted.exe", "/F"], capture_output=True)
    print("Stopped")


def cmd_status(args):
    state = load_state()
    print(f"game {state.get('game', '?')}, running: {'yes' if game_running() else 'no'}")
    for line in frame_stats(state):
        print(line[:220])
    info = live_info()
    if info:
        print(f"newest live frame {info[0]} ({info[2]})")
    log = boot_log(state)
    if log and log.exists():
        bad = [l for l in log.read_text(encoding="utf-8", errors="replace").splitlines()
               if any(k in l for k in ("Assert", "assert", "Exception", "DEVICE_LOST", "Window: closing", "Hang"))]
        for line in bad[-5:]:
            print("! " + line[:200])


def finish(args, started):
    if not getattr(args, "no_shot", False):
        shot(width=args.width, after=started + 0.05)


def cmd_press(args):
    started = time.time()
    hold(" ".join(args.buttons), args.ms, args)
    finish(args, started)


def cmd_do(args):
    started = time.time()
    hold(args.tokens, args.ms, args)
    finish(args, started)


def cmd_seq(args):
    """Steps "tokens@ms" held back to back; left=<dir> / right=<dir> set a stick as move/look do."""
    steps = []
    for step in args.steps:
        tokens, _, ms = step.rpartition("@")
        words = [stick(w[5:], "l") if w.startswith("left=") else
                 stick(w[6:], "r") if w.startswith("right=") else w for w in tokens.split()]
        steps.append((" ".join(words), int(ms)))
    started = time.time()
    hold_steps(steps, args)
    finish(args, started)


def cmd_move(args):
    started = time.time()
    extra = " " + " ".join(args.with_buttons) if args.with_buttons else ""
    hold(stick(args.direction, "l") + extra, args.ms, args)
    finish(args, started)


def cmd_look(args):
    started = time.time()
    hold(stick(args.direction, "r"), args.ms, args)
    finish(args, started)


def cmd_wait(args):
    time.sleep(args.seconds)
    finish(args, time.time() - 0.05)


def cmd_shot(args):
    shot(out=args.out, width=args.width)


def cmd_gameplay(args):
    state = load_state()
    deadline = time.time() + args.timeout
    while time.time() < deadline and not (frame_stats(state) and game_running()):
        if not game_running() and time.time() - state.get("started", 0) > 20:
            sys.exit("The game is not running")
        time.sleep(2)
    while time.time() < deadline:
        recent = frame_stats(state, 2)
        if len(recent) == 2 and all(draws_per_frame(l) >= args.draws for l in recent):
            print(f"Gameplay ({draws_per_frame(recent[-1])} draws/frame)")
            if (LIVE / "live.bmp").exists():  # builds before GOW3_LIVE_VIEW have none
                shot(width=args.width)
            return
        hold("cross", 200)
        time.sleep(3.5)
    sys.exit("No gameplay before the timeout")


def cmd_mark(args):
    state = load_state()
    lines = TRACE.read_text(encoding="ascii").splitlines() if TRACE.exists() else []
    state["mark"] = len(lines)
    state["mark_time"] = time.time()
    save_state(state)
    print(f"Route starts here (trace line {len(lines)})")


def cmd_save(args):
    state = load_state()
    lines = TRACE.read_text(encoding="ascii").splitlines()[state.get("mark", 0):]
    start_ms = int((state.get("mark_time", state["trace_start"]) - state["trace_start"]) * 1000)
    ROUTES.mkdir(parents=True, exist_ok=True)
    path = Path(args.route) if os.sep in args.route or "/" in args.route else ROUTES / f"{args.route}.txt"
    out = ["0 0 128 128 128 128 0 0"]
    shift, last_ms, last_rest = 0, 0, "0 128 128 128 128 0 0"
    for line in lines:
        ms, rest = line.split(" ", 1)
        ms = max(0, int(ms) - start_ms)
        if args.max_idle and last_rest == "0 128 128 128 128 0 0":
            # the pad was released (an agent thinking between steps): keep at most max_idle of it
            shift += max(0, ms - last_ms - args.max_idle)
        last_ms, last_rest = ms, rest
        out.append(f"{ms - shift} {rest}")
    path.write_text("\n".join(out) + "\n", encoding="ascii")
    print(f"{path}: {len(lines)} input changes, {int(out[-1].split()[0]) / 1000:.1f} s")


def route_path(name):
    path = Path(name)
    if path.exists():
        return path.resolve()
    for candidate in (ROUTES / f"{name}.txt", ROUTES / name):
        if candidate.exists():
            return candidate.resolve()
    sys.exit(f"No route {name}")


def route_seconds(path):
    lines = [l for l in path.read_text(encoding="ascii").splitlines() if l.strip()]
    return int(lines[-1].split()[0]) / 1000 if lines else 0


def arm_replay(path):
    """Starts the game's replay of a route; returns when the token was written. The runtime reads
    pad file tokens into 64 characters (runtime_pad.c), which an absolute path overflows (0 samples
    loaded, 19 Oct): the route goes to a short path relative to the game's folder (the repo)."""
    shutil.copyfile(path, PLAY / "replay.txt")
    started = time.time()
    write_pad("replay=analysis/play/replay.txt")
    time.sleep(0.4)
    write_pad("")
    return started


def cmd_replay(args):
    path = route_path(args.route)
    length = route_seconds(path)
    marks = sorted(float(m) for m in args.marks.split(",")) if args.marks else []
    for i in range(args.times):
        prefix = args.prefix and (f"{args.prefix}-{i + 1}" if args.times > 1 else args.prefix)
        started = arm_replay(path)
        for mark in marks:  # within ~40 ms of the replay's clock, plus the live view's age
            time.sleep(max(0, started + mark - time.time()))
            shot(out=prefix and f"{prefix}_{mark:g}s.png", width=args.width, after=time.time() - 0.03)
        time.sleep(max(0, length + 0.6 - (time.time() - started)))
        if not args.no_shot:
            shot(out=prefix and f"{prefix}_end.png", width=args.width, after=time.time() - 0.05)


def read_frametimes():
    path = USER / "frametimes.txt"
    try:
        return path.read_text(encoding="ascii").split()
    except OSError:
        return []


def presentmon_exe():
    found = shutil.which("presentmon") or shutil.which("PresentMon")
    if found:
        return found
    base = Path(os.environ.get("LOCALAPPDATA", "")) / "Microsoft" / "WinGet" / "Packages"
    for exe in base.glob("Intel.PresentMon*/*resentmon*.exe"):
        return str(exe)
    return None


def stats_line(label, times):
    ordered = sorted(times)
    n = len(ordered)
    pick = lambda q: ordered[min(n - 1, int(q * n))]
    slow = ordered[-max(1, n // 100):]
    mean = statistics.fmean(ordered)
    return (f"{label}: {n} frames, {1000 / mean:.1f} FPS, p50 {pick(0.5):.2f} ms, p99 {pick(0.99):.2f} ms, "
            f"1% low {1000 / statistics.fmean(slow):.1f} FPS, worst {ordered[-1]:.1f} ms")


def presentmon_lines(tag, csv_path):
    """Summary of a PresentMon 2.x CSV (--v2_metrics; 1.x MsBetweenPresents files work too).

    FrameTime is the present-to-present interval and DisplayedTime how long each shown frame stayed
    on screen. GPUBusy is the time in that interval when the GPU ran this process's work (all
    queues, DLSS included) and GPUWait the gaps between. GPU busy close to the frame time means
    the GPU is the limit; a large GPU wait means the GPU waited for the CPU side (the port's
    command, recording or present threads). CPUBusy is not the game's CPU time here: the port
    presents from its own present thread."""
    import csv
    with Path(csv_path).open(newline="") as f:
        rows = list(csv.DictReader(f))
    if not rows:
        return [f"{tag} PresentMon: no frames"]

    def column(name):
        return [float(r[name]) for r in rows if r.get(name) not in (None, "", "NA")]

    key = "MsBetweenPresents" if "MsBetweenPresents" in rows[0] else "FrameTime"
    frame = column(key)
    lines = [stats_line(f"{tag} PresentMon", frame)]
    shown = column("DisplayedTime") or column("MsBetweenDisplayChange")
    if shown:
        lines.append(stats_line(f"{tag} PresentMon on screen", shown))
    busy, wait = column("GPUBusy") or column("MsGPUActive"), column("GPUWait")
    if busy and frame:
        mean_frame, mean_busy = statistics.fmean(frame), statistics.fmean(busy)
        text = f"{tag} PresentMon GPU busy {mean_busy:.2f} ms/frame ({100 * mean_busy / mean_frame:.0f}% of the frame)"
        if wait:
            text += f", GPU wait {statistics.fmean(wait):.2f} ms/frame"
        latency = column("GPULatency")
        if latency:
            text += f", CPU start to GPU start {statistics.fmean(latency):.2f} ms"
        lines.append(text)
    return lines


def cmd_presentmon(args):
    print("\n".join(presentmon_lines(args.tag or Path(args.csv).stem, args.csv)))


def game_pid():
    out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq Uncharted.exe", "/NH", "/FO", "CSV"],
                         capture_output=True, text=True, errors="replace").stdout
    rows = [l.split('","') for l in out.splitlines() if l.startswith('"')]  # "no tasks" text is localized
    if not rows:
        raise OSError("Uncharted.exe is not running")
    return int(rows[0][1])


def cpu_snapshot(pid):
    """22 Oct 2026 (roadmap: measure thread usage in every test). Idle and busy ticks (100 ns) of each
    logical CPU, and name, kernel and user ticks of each thread of pid. Win32 calls through ctypes,
    not PDH/typeperf: their counter names are localized. Threads that ended before the snapshot are lost."""
    import ctypes
    from ctypes import wintypes as w
    k32, nt = ctypes.WinDLL("kernel32"), ctypes.WinDLL("ntdll")
    vp = ctypes.c_void_p
    k32.CreateToolhelp32Snapshot.restype, k32.CreateToolhelp32Snapshot.argtypes = vp, [w.DWORD, w.DWORD]
    for fn in (k32.Thread32First, k32.Thread32Next):
        fn.argtypes = [vp, ctypes.POINTER(w.DWORD)]
    k32.OpenThread.restype, k32.OpenThread.argtypes = vp, [w.DWORD, w.BOOL, w.DWORD]
    k32.GetThreadTimes.argtypes = [vp] * 5
    k32.GetThreadDescription.argtypes = [vp, vp]
    k32.LocalFree.argtypes = k32.CloseHandle.argtypes = [vp]

    class CpuPerf(ctypes.Structure):  # SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION; Kernel includes Idle
        _fields_ = [(n, ctypes.c_longlong) for n in ("idle", "kernel", "user", "dpc", "intr")] + [("n", ctypes.c_ulong)]

    perf, size = (CpuPerf * 256)(), ctypes.c_ulong()
    status = nt.NtQuerySystemInformation(8, perf, ctypes.sizeof(perf), ctypes.byref(size))
    if status:
        raise OSError(f"NtQuerySystemInformation 0x{status & 0xffffffff:x}")
    cpus = [(c.idle, c.kernel + c.user) for c in perf[:size.value // ctypes.sizeof(CpuPerf)]]
    now, tids = time.perf_counter(), []
    snap = k32.CreateToolhelp32Snapshot(4, 0)  # TH32CS_SNAPTHREAD
    if snap in (None, vp(-1).value):
        raise OSError("CreateToolhelp32Snapshot failed")
    entry = (w.DWORD * 7)(28)  # THREADENTRY32: size, usage, tid, owner pid, 3 more
    ok = k32.Thread32First(snap, entry)
    while ok:
        if entry[3] == pid:
            tids.append(entry[2])
        ok = k32.Thread32Next(snap, entry)
    k32.CloseHandle(snap)
    threads = {}
    for tid in tids:
        h = k32.OpenThread(0x800, False, tid)  # THREAD_QUERY_LIMITED_INFORMATION
        if not h:  # ended already, or access denied
            continue
        try:
            t, text = (ctypes.c_ulonglong * 4)(), vp()  # FILETIMEs: creation, exit, kernel, user
            if not k32.GetThreadTimes(h, *(ctypes.byref(t, 8 * i) for i in range(4))):
                continue
            name = ""
            if k32.GetThreadDescription(h, ctypes.byref(text)) >= 0 and text.value:
                name = ctypes.wstring_at(text.value)
                k32.LocalFree(text)
            threads[tid] = (name or f"tid {tid}", t[2], t[3])
        finally:
            k32.CloseHandle(h)
    if not threads:
        raise OSError(f"no readable threads for pid {pid} (not running, or access denied)")
    return {"t": now, "pid": pid, "cpus": cpus, "threads": threads}


def cpu_snap(prev=None):
    """cpu_snapshot of the game, or the error instead of raising: measure must not fail on it."""
    try:
        return cpu_snapshot(prev["pid"] if isinstance(prev, dict) else game_pid())
    except Exception as e:
        return e


def cpu_use_lines(tag, out_dir, a, b):
    """Per-CPU busy% and per-thread CPU time between two cpu_snap()s (100% = one full core); also
    saved as <tag>.cpu.json."""
    try:
        for s in (a, b):
            if isinstance(s, Exception):
                raise s
        secs = b["t"] - a["t"]
        cpus = [100 * (1 - (i1 - i0) / max(1, t1 - t0)) for (i0, t0), (i1, t1) in zip(a["cpus"], b["cpus"])]
        rows, groups = [], {}
        for tid, (name, kern, user) in b["threads"].items():
            _, kern0, user0 = a["threads"].get(tid, (name, 0, 0))  # a thread born in the window starts at 0
            row = {"tid": tid, "name": name, "busy_pct": round((kern - kern0 + user - user0) / 1e5 / secs, 2),
                   "kernel_pct": round((kern - kern0) / 1e5 / secs, 2)}
            rows.append(row)
            group = groups.setdefault(name, [0, 0.0])
            group[0] += 1
            group[1] += row["busy_pct"]
        rows.sort(key=lambda r: -r["busy_pct"])
        cores = sum(r["busy_pct"] for r in rows) / 100
        pct_of_machine = 100 * cores / len(cpus)
        (out_dir / f"{tag}.cpu.json").write_text(json.dumps({
            "window_s": round(secs, 3), "pid": a["pid"], "logical_cpus": len(cpus),
            "cpu_busy_pct": [round(c, 1) for c in cpus], "process_cores": round(cores, 3),
            "process_machine_pct": round(pct_of_machine, 1), "threads": rows}, indent=1), encoding="utf-8")
        top_groups = sorted(groups.items(), key=lambda kv: -kv[1][1])[:12]
        return [
            f"{tag} CPU threads: process {cores:.2f} cores busy of {len(cpus)} logical ({pct_of_machine:.0f}% of the machine); "
            "logical CPUs busy%: " + ", ".join(f"c{i} {c:.0f}" for i, c in enumerate(cpus)),
            f"{tag} CPU top threads: " + ", ".join(f"{r['name']} {r['busy_pct']:.0f}%" for r in rows[:12]),
            f"{tag} CPU thread groups: " + ", ".join(
                f"{n} x{c} {p:.0f}%" if c > 1 else f"{n} {p:.0f}%" for n, (c, p) in top_groups)]
    except Exception as e:
        return [f"{tag} CPU threads: unavailable ({str(e) or type(e).__name__})"]


def cmd_measure(args):
    state = load_state()
    tag = args.tag or "measure"
    out_dir = PLAY / "measure"
    out_dir.mkdir(parents=True, exist_ok=True)
    pm = presentmon_exe() if not args.no_presentmon else None
    pm_csv = out_dir / f"{tag}.csv"
    pm_log = out_dir / f"{tag}.presentmon.txt"
    pm_process = None
    if pm:
        pm_csv.unlink(missing_ok=True)
        # 19 Oct: a session left by an interrupted run made every later capture fail silently.
        with pm_log.open("w", encoding="utf-8") as log:
            pm_process = subprocess.Popen(
                [pm, "--process_name", "Uncharted.exe", "--output_file", str(pm_csv), "--timed",
                 str(args.seconds), "--terminate_after_timed", "--no_console_stats", "--v2_metrics",
                 "--session_name", "UnchartedPlay", "--stop_existing_session"],
                stdout=log, stderr=subprocess.STDOUT)
    smi = subprocess.Popen(["nvidia-smi", "--query-gpu=utilization.gpu,power.draw", "--format=csv,noheader,nounits",
                            "-lms", "500"], stdout=subprocess.PIPE, text=True) if shutil.which("nvidia-smi") else None
    before = len(read_frametimes())
    log_before = len(frame_stats(state, 100000))
    cpu_a = cpu_snap()
    started = time.time()
    if args.route:
        path = route_path(args.route)
        length = route_seconds(path)
        while time.time() - started < args.seconds:
            arm_replay(path)
            time.sleep(min(length + 0.6, max(0, args.seconds - (time.time() - started))))
    else:
        time.sleep(args.seconds)
    cpu_b = cpu_snap(cpu_a)
    if smi:
        smi.terminate()
        samples = [l.split(",") for l in smi.communicate()[0].splitlines() if "," in l]
    time.sleep(5.5)  # GOW3_FRAME_TIMES flushes every 5 s
    times = [float(v) for v in read_frametimes()[before:]]
    total, kept = 0.0, []
    for value in times:  # the frames of the measured window
        if total >= args.seconds * 1000:
            break
        kept.append(value)
        total += value
    lines = []
    if kept:
        lines.append(stats_line(f"{tag} port", kept))
        # The window's frame times as they are, for A/B statistics over many windows.
        (out_dir / f"{tag}.port.txt").write_text("\n".join(f"{v:.3f}" for v in kept) + "\n",
                                                 encoding="ascii")
    if pm_process:
        pm_process.wait(timeout=30)
        try:
            lines.extend(presentmon_lines(tag, pm_csv))
        except OSError:
            detail = pm_log.read_text(encoding="utf-8", errors="replace").strip().splitlines()
            lines.append(f"{tag} PresentMon: no capture ({detail[-1] if detail else 'no output'}; "
                         "needs the Performance Log Users group or admin)")
    if smi and samples:
        util = statistics.fmean(float(s[0]) for s in samples)
        watts = statistics.fmean(float(s[1]) for s in samples)
        lines.append(f"{tag} GPU {util:.0f}% busy, {watts:.0f} W (nvidia-smi)")
    for line in frame_stats(state, 100000)[log_before:][-1:]:
        lines.append(f"{tag} last {line[:400]}")
    lines.extend(cpu_use_lines(tag, out_dir, cpu_a, cpu_b))
    with (out_dir / "results.txt").open("a", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    print("\n".join(lines))


def ssim(a, b):
    """Mean SSIM of two grey images (Wang et al. 2004: 11x11 Gaussian window, sigma 1.5)."""
    import numpy as np
    x = np.arange(11) - 5
    g = np.exp(-(x ** 2) / (2 * 1.5 ** 2))
    g /= g.sum()

    def blur(img):
        img = np.apply_along_axis(lambda r: np.convolve(r, g, mode="valid"), 1, img)
        return np.apply_along_axis(lambda c: np.convolve(c, g, mode="valid"), 0, img)
    c1, c2 = (0.01 * 255) ** 2, (0.03 * 255) ** 2
    mu_a, mu_b = blur(a), blur(b)
    va = blur(a * a) - mu_a ** 2
    vb = blur(b * b) - mu_b ** 2
    cov = blur(a * b) - mu_a * mu_b
    s = ((2 * mu_a * mu_b + c1) * (2 * cov + c2)) / ((mu_a ** 2 + mu_b ** 2 + c1) * (va + vb + c2))
    return float(s.mean()), s


def cmd_compare(args):
    import numpy as np
    from PIL import Image
    a = Image.open(args.a).convert("RGB")
    b = Image.open(args.b).convert("RGB")
    if a.size != b.size:
        b = b.resize(a.size, Image.LANCZOS)
    if args.width and a.width > args.width:
        size = (args.width, round(a.height * args.width / a.width))
        a, b = a.resize(size, Image.LANCZOS), b.resize(size, Image.LANCZOS)
    pa = np.asarray(a, dtype=np.float64)
    pb = np.asarray(b, dtype=np.float64)
    diff = np.abs(pa - pb).max(axis=2)
    score, ssim_map = ssim(pa.mean(axis=2), pb.mean(axis=2))
    changed = float((diff > args.threshold).mean() * 100)
    print(f"SSIM {score:.4f}, {changed:.2f}% of pixels differ by more than {args.threshold}, "
          f"max difference {int(diff.max())}")
    if args.heatmap:
        heat = np.clip(diff * 4, 0, 255).astype(np.uint8)
        Image.fromarray(heat).save(args.heatmap)
        print(f"heat map {args.heatmap}")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)

    def with_shot(p):
        p.add_argument("--no-shot", action="store_true", help="no screenshot afterwards")
        p.add_argument("--shot-at", type=int, default=0, help="take the shot this many ms into the hold")
        p.add_argument("--burst", type=int, default=0, help="also a shot every this many ms while held")
        p.add_argument("--width", type=int, default=960)
        return p

    p = sub.add_parser("start")
    p.add_argument("game", choices=GAMES)
    p.add_argument("--mt", type=int, choices=(0, 1), help="multithreaded rendering (default: the game's)")
    p.add_argument("--res", help="render size, e.g. 3840x2160")
    p.add_argument("--game-dir", help="the extracted game (default: the launcher's)")
    p.add_argument("--env", action="append", default=[], help="NAME=value for the game")
    p.add_argument("--live-ms", type=int, default=200, help="live frame interval")
    p.add_argument("--input", action="store_true", help="keep the keyboard and mouse (off by default)")
    p.add_argument("--restart", action="store_true")
    p.set_defaults(func=cmd_start)
    sub.add_parser("stop").set_defaults(func=cmd_stop)
    sub.add_parser("status").set_defaults(func=cmd_status)
    p = sub.add_parser("shot")
    p.add_argument("--out")
    p.add_argument("--width", type=int, default=960)
    p.set_defaults(func=cmd_shot)
    p = with_shot(sub.add_parser("press"))
    p.add_argument("buttons", nargs="+", choices=BUTTONS)
    p.add_argument("--ms", type=int, default=150)
    p.set_defaults(func=cmd_press)
    p = with_shot(sub.add_parser("do"))
    p.add_argument("tokens")
    p.add_argument("--ms", type=int, default=500)
    p.set_defaults(func=cmd_do)
    p = with_shot(sub.add_parser("seq"))
    p.add_argument("steps", nargs="+", help='"tokens@ms" each, e.g. "left=f@800" "left=f cross@150"')
    p.set_defaults(func=cmd_seq)
    p = with_shot(sub.add_parser("move"))
    p.add_argument("direction")
    p.add_argument("--ms", type=int, default=1000)
    p.add_argument("--with", dest="with_buttons", nargs="*", choices=BUTTONS, help="buttons held while moving")
    p.set_defaults(func=cmd_move)
    p = with_shot(sub.add_parser("look"))
    p.add_argument("direction")
    p.add_argument("--ms", type=int, default=300)
    p.set_defaults(func=cmd_look)
    p = with_shot(sub.add_parser("wait"))
    p.add_argument("seconds", type=float)
    p.set_defaults(func=cmd_wait)
    p = sub.add_parser("gameplay")
    p.add_argument("--draws", type=int, default=400, help="draws per frame that mean gameplay")
    p.add_argument("--timeout", type=float, default=300)
    p.add_argument("--width", type=int, default=960)
    p.set_defaults(func=cmd_gameplay)
    sub.add_parser("mark").set_defaults(func=cmd_mark)
    p = sub.add_parser("save")
    p.add_argument("route")
    p.add_argument("--max-idle", type=int, default=0, help="shorten pad-released stretches to this many ms")
    p.set_defaults(func=cmd_save)
    p = with_shot(sub.add_parser("replay"))
    p.add_argument("route")
    p.add_argument("--times", type=int, default=1)
    p.add_argument("--marks", help="also a shot this many seconds into the route, e.g. 10,30,60")
    p.add_argument("--prefix", help="shots go to <prefix>_<mark>s.png and <prefix>_end.png")
    p.set_defaults(func=cmd_replay)
    p = sub.add_parser("measure")
    p.add_argument("seconds", type=float)
    p.add_argument("--route")
    p.add_argument("--tag")
    p.add_argument("--no-presentmon", action="store_true")
    p.set_defaults(func=cmd_measure)
    p = sub.add_parser("presentmon")
    p.add_argument("csv")
    p.add_argument("--tag")
    p.set_defaults(func=cmd_presentmon)
    p = sub.add_parser("compare")
    p.add_argument("a")
    p.add_argument("b")
    p.add_argument("--threshold", type=int, default=24)
    p.add_argument("--width", type=int, default=960)
    p.add_argument("--heatmap")
    p.set_defaults(func=cmd_compare)
    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
