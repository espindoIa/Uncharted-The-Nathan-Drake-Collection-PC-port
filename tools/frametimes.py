"""Frame time statistics from GOW3_FRAME_TIMES (one flip interval in ms per line).

    python tools/frametimes.py <file> [--skip N] [--count N] [--label text]

--skip leaves out the first N frames (e.g. the lines before a measurement started), --count
keeps N frames after that. Prints frames, mean (and FPS), p50, p95, p99, 1% low FPS (mean of
the slowest 1%) and the worst frame.
"""
import argparse
import statistics


def stats(times):
    ordered = sorted(times)
    n = len(ordered)
    pick = lambda q: ordered[min(n - 1, int(q * n))]
    slowest = ordered[-max(1, n // 100):]
    mean = statistics.fmean(ordered)
    return {
        "frames": n, "mean": mean, "fps": 1000 / mean, "p50": pick(0.5), "p95": pick(0.95),
        "p99": pick(0.99), "low1": 1000 / statistics.fmean(slowest), "max": ordered[-1],
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("file")
    parser.add_argument("--skip", type=int, default=0)
    parser.add_argument("--count", type=int, default=0)
    parser.add_argument("--label", default="")
    args = parser.parse_args()
    with open(args.file, encoding="ascii") as f:
        times = [float(line) for line in f if line.strip()]
    times = times[args.skip:]
    if args.count:
        times = times[:args.count]
    if not times:
        print(f"{args.label}: no frames")
        return
    s = stats(times)
    print(f"{args.label}: {s['frames']} frames, mean {s['mean']:.2f} ms ({s['fps']:.1f} FPS), "
          f"p50 {s['p50']:.2f}, p95 {s['p95']:.2f}, p99 {s['p99']:.2f} ms, "
          f"1% low {s['low1']:.1f} FPS, worst {s['max']:.1f} ms")


if __name__ == "__main__":
    main()
