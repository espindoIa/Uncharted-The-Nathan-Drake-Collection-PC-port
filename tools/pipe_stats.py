"""Per-frame CPU pipeline statistics from GOW3_PIPE_STATS (schema 1, gpu/shim/gow3_pipe_stats.h).

    python tools/pipe_stats.py <file> [--skip-s 10] [--split-toggles]

Prints frame time percentiles (picked as tools/frametimes.py does, so p50/p95/p99 match it for
the same frames), each stage's time per frame and share of the frame, how full the draw pipe
was when stage A committed, and which stage the medians point at. --split-toggles reports the
frames of each toggles2 value separately (a live A/B inside one run; the first frame after each
switch is left out). The rec_*/sub_* columns are reserved and stay empty for now.
"""
import argparse
import csv
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from frametimes import stats  # noqa: E402


def load(path):
    header = {}
    with open(path, newline="") as f:
        lines = f.read().splitlines()
    if not lines or not lines[0].startswith("# gow3_pipe_stats"):
        sys.exit(f"{path}: not a GOW3_PIPE_STATS file")
    for item in lines[0][1:].split():
        if "=" in item:
            key, value = item.split("=", 1)
            header[key] = value
    if header.get("schema") != "1":
        sys.exit(f"{path}: schema {header.get('schema')} (this script reads 1)")
    rows = []
    for row in csv.DictReader(lines[1:]):
        rows.append({k: (int(v) if v not in ("", None) else None) for k, v in row.items()})
    return header, rows


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


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("file")
    parser.add_argument("--skip-s", type=float, default=10.0,
                        help="seconds left out at the start (loading, shader compiles)")
    parser.add_argument("--split-toggles", action="store_true")
    args = parser.parse_args()
    header, rows = load(args.file)
    print("header:", " ".join(f"{k}={v}" for k, v in header.items()))
    if rows:
        start = rows[0]["t_flip_ns"] + int(args.skip_s * 1e9)
        rows = [r for r in rows if r["t_flip_ns"] >= start]
    if rows and rows[-1]["dropped"]:
        print(f"note: {rows[-1]['dropped']} rows dropped (ring full or a second flip thread)")
    if not args.split_toggles:
        report(rows, args.file)
        return
    # A frame counts for a group only if the flip before it had the same toggles (report()
    # needs consecutive frame_ids), so the frame in which the toggle changed is left out.
    for key in sorted({r["toggles2"] for r in rows}):
        report([r for r in rows if r["toggles2"] == key], f"toggles2={key}")

if __name__ == "__main__":
    main()
