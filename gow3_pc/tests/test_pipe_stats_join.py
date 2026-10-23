"""tools/pipe_stats.py: rows join GPU records on a_seq while flips lag stage A by 1-2 frames."""
import csv
import io
import random
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from paths import ROOT

REPO = ROOT.parent
if str(REPO / "tools") not in sys.path:
    sys.path.insert(0, str(REPO / "tools"))

import pipe_stats  # noqa: E402

FIXED = ["frame_id", "t_flip_ns", "a_frame_id", "a_seq", "a_seq_issued", "toggles1", "toggles2",
         "stats_paused", "dropped", "a_wait_spin_ns", "a_wait_yield_ns", "a_idle_block_ns",
         "a_idle_poll_ns", "a_drains", "a_space_waits", "a_space_spin_ns", "a_packets",
         "a_wake_notifies", "b_packets", "b_busy_ns", "b_wait_spin_ns", "b_wait_block_ns",
         "b_sleeps", "b_max_packet_ns", "a_occ_0", "a_occ_samples"]


def simulate(frames=200, seed=1):
    """Stage A finishes frame s (a_seq s) and its GPU work is tagged s; the flip that shows it
    comes 1 or 2 frames later. Returns the main CSV text, the GPU CSV text and, per frame_id,
    the a_seq it shows; also the frame_id the flip thread had reached when A started each frame
    (the drifting tag)."""
    rng = random.Random(seed)
    rows, gpu, shows, naive = [], [], {}, {}
    in_flight, issued, frame_id, t = [], 0, 1, 0
    gpu.append("a_seq,label,gpu_ms")
    while len(shows) < frames:
        lag = rng.choice((1, 2))
        while len(in_flight) < lag + 1:
            issued += 1
            naive[issued] = frame_id
            in_flight.append(issued)
            gpu.append(f"{issued},scene,{issued / 100:.2f}")
            gpu.append(f"{issued},post,{issued / 1000:.3f}")
        seq = in_flight.pop(0)
        frame_id += 1
        t += 16_000_000 + rng.randrange(1_000_000)
        shows[frame_id] = seq
        row = dict.fromkeys(FIXED, 0)
        row.update(frame_id=frame_id, t_flip_ns=t, a_seq=seq, a_seq_issued=issued,
                   a_packets=100, b_packets=100, b_busy_ns=8_000_000)
        rows.append(row)
    out = io.StringIO()
    out.write("# gow3_pipe_stats schema=1 build=test toggles=0,0 tsc_hz=1 threads=1 "
              "occ_sample=1/16\n")
    writer = csv.DictWriter(out, FIXED, lineterminator="\n")
    writer.writeheader()
    writer.writerows(rows)
    return out.getvalue(), "\n".join(gpu) + "\n", shows, naive


class PipeStatsJoinTests(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.addCleanup(self.dir.cleanup)
        main, gpu, self.shows, self.naive = simulate()
        self.path = Path(self.dir.name) / "run.csv"
        self.path.write_text(main)
        Path(str(self.path) + ".gpu.csv").write_text(gpu)

    def test_a_seq_join_finds_each_shown_frame(self):
        _, rows = pipe_stats.load(str(self.path))
        records = pipe_stats.load_gpu(str(self.path) + ".gpu.csv")
        joined, unmatched = pipe_stats.join_by_a_seq(rows, records)
        self.assertEqual(len(joined), len(self.shows))
        for frame_id, seq in self.shows.items():
            self.assertAlmostEqual(joined[frame_id]["scene"], seq / 100, places=6)
            self.assertAlmostEqual(joined[frame_id]["post"], seq / 1000, places=6)
        # Frames A built but no flip showed before the file ended.
        self.assertTrue(unmatched)
        self.assertTrue(all(seq > max(self.shows.values()) for seq in unmatched))
        depths = {r["a_seq_issued"] - r["a_seq"] for r in rows}
        self.assertEqual(depths, {1, 2})

    def test_frame_id_tags_drift(self):
        # The frame_id reached when A started a frame is not the frame_id of the row that shows
        # it, and the offset changes, so no constant shift fixes it.
        offsets = {frame_id - self.naive[seq] for frame_id, seq in self.shows.items()}
        self.assertGreater(len(offsets), 1)
        tags = list(self.naive.values())
        self.assertLess(len(set(tags)), len(tags))  # several frames share one tag

    def test_rows_without_a_seq_are_left_out(self):
        _, rows = pipe_stats.load(str(self.path))
        for row in rows[::2]:
            row["a_seq"] = None
        joined, _ = pipe_stats.join_by_a_seq(rows, [])
        self.assertEqual(len(joined), len(rows) - len(rows[::2]))

    def test_report_runs(self):
        r = subprocess.run([sys.executable, str(REPO / "tools" / "pipe_stats.py"), str(self.path),
                            "--skip-s", "0"], capture_output=True, text=True, timeout=30)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("pipeline depth at flip", r.stdout)
        self.assertIn("GPU (joined on a_seq)", r.stdout)
        self.assertIn("gpu scene", r.stdout)


if __name__ == "__main__":
    unittest.main()
