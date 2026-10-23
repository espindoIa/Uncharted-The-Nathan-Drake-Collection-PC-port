"""Per-frame CPU pipeline statistics from GOW3_PIPE_STATS (schema 1, gpu/shim/gow3_pipe_stats.h).

    python tools/pipe_stats.py <file> [--skip-s 10] [--split-toggles] [--latest]

<file> is either one file the game wrote or the GOW3_PIPE_STATS value itself: each process
writes <stem>.<UTC start>-<pid>.csv (a game switch or restart is a new process and a new file),
and the value names all of them; each is reported on its own (frame_id and a_seq restart with
the process). --latest reports only the newest.

Prints frame time percentiles (picked as tools/frametimes.py does, so p50/p95/p99 match it for
the same frames), each stage's time per frame and share of the frame, how full the draw pipe
was when stage A committed, and which stage the medians point at. --split-toggles reports the
frames of each toggles2 value separately (a live A/B inside one run; the first frame after each
switch is left out). The rec_*/sub_* columns are reserved and stay empty for now.

a_seq is the stage A number of the frame a row's flip showed (empty while the flip request does
not carry it); a_seq_issued is how far stage A had numbered at that flip, so the difference is
how many frames deep the pipeline was. Records tagged with a_seq (<file>.gpu.csv: a_seq,label,
gpu_ms, written by the GPU timestamp side when it exists) are joined to rows on a_seq, never on
frame_id: the flip that shows a frame comes a variable number of frames after stage A built it.

A row the process did not finish writing (the game ends with std::_Exit; a crash) is left out,
counted and reported on stderr instead of stopping the report.
"""
import argparse
import glob
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from frametimes import stats  # noqa: E402


# Never empty in a row the writer finished (a_seq and the reserved columns may be).
REQUIRED = ("frame_id", "t_flip_ns", "toggles2", "stats_paused", "dropped")


def _data_lines(text):
    """Lines of `text` and whether the last one is cut (no newline: the process ended while
    the writer was in the middle of it)."""
    lines = text.split("\n")
    cut_last = lines[-1] != ""
    if not cut_last:
        lines.pop()
    return [line.rstrip("\r") for line in lines], cut_last


def _warn_skipped(path, skipped):
    if skipped:
        where = ", ".join(str(n) for n in skipped[:5]) + (" ..." if len(skipped) > 5 else "")
        print(f"warning: {path}: {len(skipped)} truncated or malformed row(s) ignored "
              f"(line {where})", file=sys.stderr)


def load(path, problems=None):
    """Header fields and rows (ints, None for an empty cell). A row with the wrong number of
    cells, a cell that is not a number, or a last line with no newline is a row the process
    did not finish writing (std::_Exit, a crash): it is left out, counted and reported on
    stderr; `problems`, if given, gets {"skipped": [line numbers]}."""
    header = {}
    with open(path, newline="") as f:
        text = f.read()
    lines, cut_last = _data_lines(text)
    if not lines or not lines[0].startswith("# gow3_pipe_stats"):
        sys.exit(f"{path}: not a GOW3_PIPE_STATS file")
    for item in lines[0][1:].split():
        if "=" in item:
            key, value = item.split("=", 1)
            header[key] = value
    if header.get("schema") != "1":
        sys.exit(f"{path}: schema {header.get('schema')} (this script reads 1)")
    skipped, rows = [], []
    if len(lines) < 2 or (len(lines) == 2 and cut_last):
        print(f"warning: {path}: no column line (the process ended right after starting)",
              file=sys.stderr)
        if problems is not None:
            problems["skipped"] = skipped
        return header, rows
    columns = lines[1].split(",")
    for number, line in enumerate(lines[2:], start=3):
        cells = line.split(",")
        if len(cells) != len(columns) or (cut_last and number == len(lines)):
            skipped.append(number)
            continue
        try:
            row = {k: (int(v) if v != "" else None) for k, v in zip(columns, cells)}
        except ValueError:
            skipped.append(number)
            continue
        if any(row.get(k) is None for k in REQUIRED):
            skipped.append(number)
            continue
        rows.append(row)
    _warn_skipped(path, skipped)
    if problems is not None:
        problems["skipped"] = skipped
    return header, rows


def load_gpu(path, problems=None):
    """<file>.gpu.csv: one line per GPU timestamp range, a_seq,label,gpu_ms (None if absent).
    Cut or malformed lines are left out and reported as in load()."""
    if not os.path.exists(path):
        return None
    with open(path, newline="") as f:
        text = f.read()
    lines, cut_last = _data_lines(text)
    numbered = [(n, line) for n, line in enumerate(lines, start=1) if not line.startswith("#")]
    if not numbered or "a_seq" not in numbered[0][1].split(","):
        sys.exit(f"{path}: no a_seq column (schema 1 keys GPU records by a_seq)")
    columns = numbered[0][1].split(",")
    skipped, records = [], []
    for number, line in numbered[1:]:
        cells = line.split(",")
        if len(cells) != len(columns) or (cut_last and number == len(lines)):
            skipped.append(number)
            continue
        row = dict(zip(columns, cells))
        try:
            records.append({"a_seq": int(row["a_seq"]), "label": row["label"],
                            "gpu_ms": float(row["gpu_ms"])})
        except (KeyError, ValueError):
            skipped.append(number)
    _warn_skipped(path, skipped)
    if problems is not None:
        problems["skipped"] = skipped
    return records


SESSION = re.compile(r"\.(\d{8}T\d{6}Z)-(\d+)\.csv$")


def session_key(name):
    """Sort key of a session file name: (UTC start, pid as a number). The start has one-second
    resolution, so two launches in the same second are ordered by pid, numerically (a text sort
    would put -99999 after -100000). Within one second that is a tie-break, not proof of
    which process started later (Windows reuses pids)."""
    m = SESSION.search(name)
    return (m.group(1), int(m.group(2))) if m else ("", -1)


def sessions(path):
    """The files to report for `path`: itself if it exists, else the session files the game
    wrote for GOW3_PIPE_STATS=<path>, oldest first by (UTC start, pid); --latest takes the last."""
    if os.path.exists(path):
        return [path]
    stem = path[:-4] if path.endswith(".csv") else path
    found = sorted((p for p in glob.glob(glob.escape(stem) + ".*.csv")
                    if SESSION.match(p[len(stem):])),
                   key=lambda p: session_key(p[len(stem):]))
    if not found:
        sys.exit(f"{path}: no such file, and no {stem}.<UTC start>-<pid>.csv files")
    return found


def join_by_a_seq(rows, records):
    """{frame_id: {label: gpu_ms}} for rows whose a_seq is known, plus the records no row showed
    (frames still in flight at the end, or shown by a dropped row)."""
    by_seq = {}
    for rec in records:
        labels = by_seq.setdefault(rec["a_seq"], {})
        labels[rec["label"]] = labels.get(rec["label"], 0.0) + rec["gpu_ms"]
    joined, shown = {}, set()
    for row in rows:
        seq = row.get("a_seq")
        if seq is None:
            continue
        joined[row["frame_id"]] = by_seq.get(seq, {})
        shown.add(seq)
    unmatched = sorted(seq for seq in by_seq if seq not in shown)
    return joined, unmatched


def pick(values, q):
    ordered = sorted(values)
    return ordered[min(len(ordered) - 1, int(q * len(ordered)))] if ordered else float("nan")


def report(rows, title):
    # Frame time: the flip interval ending at each row (rows after a dropped row are skipped:
    # their interval covers more than one frame).
    frames = []
    for prev, row in zip(rows, rows[1:]):
        if row["frame_id"] == prev["frame_id"] + 1:
            row["_frame_ns"] = row["t_flip_ns"] - prev["t_flip_ns"]
            frames.append(row)
    print(f"== {title}: {len(frames)} frames")
    if not frames:
        return
    s = stats([r["_frame_ns"] / 1e6 for r in frames])
    print(f"frame ms   mean {s['mean']:.3f} ({s['fps']:.2f} FPS)  p50 {s['p50']:.3f}  "
          f"p95 {s['p95']:.3f}  p99 {s['p99']:.3f}  1% low {s['low1']:.2f} FPS  max {s['max']:.3f}")
    paused = sum(1 for r in frames if r["stats_paused"])
    if paused:
        print(f"note: {paused} frames had collection paused (toggle 16777216): their counters are 0")
    for r in frames:
        a_waits = r["a_wait_spin_ns"] + r["a_wait_yield_ns"]
        # Approximate: drains in the idle poll loop are counted in both a_idle_poll_ns and the
        # drain columns, so this can undercount A's work slightly.
        r["a_work_ns"] = max(0, r["_frame_ns"] - r["a_idle_block_ns"] - r["a_idle_poll_ns"] - a_waits)
        r["a_waits_b_ns"] = a_waits
    columns = ["a_work_ns", "a_idle_block_ns", "a_idle_poll_ns", "a_wait_spin_ns",
               "a_wait_yield_ns", "a_space_spin_ns", "b_busy_ns", "b_wait_spin_ns",
               "b_wait_block_ns", "b_max_packet_ns"]
    for name in columns:
        values = [r[name] / 1e6 for r in frames]
        shares = [r[name] / r["_frame_ns"] for r in frames if r["_frame_ns"]]
        print(f"{name:17s} ms p50 {pick(values, .5):7.3f} p95 {pick(values, .95):7.3f} "
              f"p99 {pick(values, .99):7.3f} | share p50 {pick(shares, .5) * 100:5.1f}% "
              f"p95 {pick(shares, .95) * 100:5.1f}%")
    for name in ["a_packets", "b_packets", "a_drains", "a_space_waits", "b_sleeps", "a_wake_notifies"]:
        values = [r[name] for r in frames]
        print(f"{name:17s} /frame p50 {pick(values, .5):8d} p95 {pick(values, .95):8d}")
    buckets = [k for k in frames[0] if k.startswith("a_occ_") and k != "a_occ_samples"]
    total = sum(r[k] for r in frames for k in buckets)
    if total:
        print("pipe bytes in flight at sampled commits: " + "  ".join(
            f"{k[6:]} {sum(r[k] for r in frames) / total * 100:.1f}%" for k in buckets))
    share = lambda name: pick([r[name] / r["_frame_ns"] for r in frames], .5)
    a_work, a_idle = share("a_work_ns"), share("a_idle_block_ns") + share("a_idle_poll_ns")
    a_waits_b, b_busy = share("a_waits_b_ns"), share("b_busy_ns")
    b_starved = share("b_wait_spin_ns") + share("b_wait_block_ns")
    print(f"medians: A works {a_work:.0%}, A waits for B {a_waits_b:.0%}, A idle {a_idle:.0%}, "
          f"B busy {b_busy:.0%}, B waits for A {b_starved:.0%}")
    if a_idle > 0.15:
        verdict = "before stage A (guest submits, GPU or flip pacing): A idles waiting for work"
    elif a_work > 0.85 and b_starved > 0.15:
        verdict = "stage A (B waits while A works)"
    elif b_busy > 0.85 and a_waits_b > 0.15:
        verdict = "stage B (A waits for B)"
    elif a_waits_b > 0.10 and b_starved > 0.10:
        verdict = "A and B take turns (drains): each waits for the other"
    else:
        verdict = "no CPU stage in this file is saturated: look at the recorder, GPU or present"
    print("points at:", verdict)
    seq_rows = [r for r in frames if r.get("a_seq") is not None]
    if seq_rows:
        depth = [r["a_seq_issued"] - r["a_seq"] for r in seq_rows]
        print(f"pipeline depth at flip (a_seq_issued - a_seq) p50 {pick(depth, .5)} "
              f"p95 {pick(depth, .95)} max {max(depth)} ({len(seq_rows)} frames with a_seq)")
    return frames


def report_gpu(frames, records):
    joined, unmatched = join_by_a_seq(frames, records)
    print(f"== GPU (joined on a_seq): {len(joined)} frames, {len(unmatched)} frames not joined "
          "(still in flight at the end, or a dropped or left-out row)")
    labels = sorted({label for per in joined.values() for label in per})
    for label in labels:
        values = [per[label] for per in joined.values() if label in per]
        print(f"gpu {label:20s} ms p50 {pick(values, .5):7.3f} p95 {pick(values, .95):7.3f} "
              f"({len(values)} frames)")


def report_file(path, args):
    header, rows = load(path)
    print("header:", " ".join(f"{k}={v}" for k, v in header.items()))
    if rows:
        start = rows[0]["t_flip_ns"] + int(args.skip_s * 1e9)
        rows = [r for r in rows if r["t_flip_ns"] >= start]
    if rows and rows[-1]["dropped"]:
        print(f"note: {rows[-1]['dropped']} rows dropped (ring full or a second flip thread)")
    if not args.split_toggles:
        frames = report(rows, path)
        records = load_gpu(path + ".gpu.csv")
        if frames and records is not None:
            report_gpu(frames, records)
        return
    # A frame counts for a group only if the flip before it had the same toggles (report()
    # needs consecutive frame_ids), so the frame in which the toggle changed is left out.
    for key in sorted({r["toggles2"] for r in rows}):
        report([r for r in rows if r["toggles2"] == key], f"toggles2={key}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("file")
    parser.add_argument("--skip-s", type=float, default=10.0,
                        help="seconds left out at the start (loading, shader compiles)")
    parser.add_argument("--split-toggles", action="store_true")
    parser.add_argument("--latest", action="store_true", help="only the newest session file")
    args = parser.parse_args()
    paths = sessions(args.file)
    if args.latest:
        paths = paths[-1:]
    elif len(paths) > 1:
        print(f"{len(paths)} session files for {args.file} (one per process)")
    for path in paths:
        report_file(path, args)


if __name__ == "__main__":
    main()
