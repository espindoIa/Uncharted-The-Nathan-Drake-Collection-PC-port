"""tools/pipe_stats.py: rows join GPU records on a_seq while flips lag stage A by 1-2 frames;
rows the process did not finish writing (std::_Exit, a crash) are left out and counted."""
import contextlib
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


class PipeStatsCutRowTests(unittest.TestCase):
    """B1/B2 (docs/team/SENTINEL_review_vector-pipe-stats.md): the game ends with std::_Exit, so
    the last line can stop in the middle of a row."""

    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.addCleanup(self.dir.cleanup)
        self.main, self.gpu, self.shows, _ = simulate(frames=50)
        self.complete = len(self.main.splitlines()) - 2

    def write(self, text, gpu=None):
        path = Path(self.dir.name) / "run.csv"
        path.write_text(text)
        if gpu is not None:
            Path(str(path) + ".gpu.csv").write_text(gpu)
        return str(path)

    def test_cut_last_row_is_left_out_and_counted(self):
        # Cut in the middle of the t_flip_ns cell of the last row (no newline), as in the review.
        last = self.main.rstrip("\n").rsplit("\n", 1)[1]
        cut = self.main[: len(self.main) - len(last) - 1 + len(last.split(",")[0]) + 4]
        problems = {}
        _, rows = pipe_stats.load(self.write(cut), problems)
        self.assertEqual(len(rows), self.complete - 1)
        self.assertEqual(len(problems["skipped"]), 1)

    def test_cut_row_with_all_cells_but_no_newline_is_left_out(self):
        problems = {}
        _, rows = pipe_stats.load(self.write(self.main.rstrip("\n")), problems)
        self.assertEqual(len(rows), self.complete - 1)
        self.assertEqual(problems["skipped"], [self.complete + 2])

    def test_short_or_bad_rows_are_left_out(self):
        lines = self.main.splitlines()
        lines[5] = ",".join(lines[5].split(",")[:3])         # too few cells
        lines[6] = lines[6].replace(",", ",x", 1)            # not a number
        lines[7] = ",".join([""] + lines[7].split(",")[1:])  # empty frame_id
        problems = {}
        _, rows = pipe_stats.load(self.write("\n".join(lines) + "\n"), problems)
        self.assertEqual(problems["skipped"], [6, 7, 8])
        self.assertEqual(len(rows), self.complete - 3)

    def test_header_only_files_load_empty(self):
        header = self.main.split("\n", 1)[0] + "\n"
        for text in (header, header + "frame_id,t_fl"):
            _, rows = pipe_stats.load(self.write(text))
            self.assertEqual(rows, [])

    def test_cut_gpu_line_is_left_out(self):
        problems = {}
        path = self.write(self.main, self.gpu[:-3])
        records = pipe_stats.load_gpu(path + ".gpu.csv", problems)
        self.assertEqual(len(records), len(self.gpu.splitlines()) - 2)
        self.assertEqual(len(problems["skipped"]), 1)

    def test_report_warns_and_runs(self):
        path = self.write(self.main[:-7], self.gpu)
        r = subprocess.run([sys.executable, str(REPO / "tools" / "pipe_stats.py"), path,
                            "--skip-s", "0"], capture_output=True, text=True, timeout=30)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("1 truncated or malformed row(s) ignored", r.stderr)
        self.assertIn("GPU (joined on a_seq)", r.stdout)


class PipeStatsSessionTests(unittest.TestCase):
    """B3: each process writes <stem>.<UTC start>-<pid>.csv; the GOW3_PIPE_STATS value names
    them all, each with its own <session>.gpu.csv."""

    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.addCleanup(self.dir.cleanup)
        self.base = Path(self.dir.name) / "run.csv"
        self.names = ["run.20261023T140000Z-4100.csv", "run.20261023T140230Z-977.csv"]
        for seed, name in enumerate(self.names, start=1):
            main, gpu, _, _ = simulate(frames=60 + seed, seed=seed)
            (Path(self.dir.name) / name).write_text(main)
            (Path(self.dir.name) / (name + ".gpu.csv")).write_text(gpu)
        # Not sessions of run.csv.
        (Path(self.dir.name) / "run.csv.info").write_text("1 2\n")
        (Path(self.dir.name) / "rerun.20261023T140000Z-1.csv").write_text(main)
        (Path(self.dir.name) / "run.old.csv").write_text(main)

    def test_sessions_oldest_first(self):
        found = [Path(p).name for p in pipe_stats.sessions(str(self.base))]
        self.assertEqual(found, self.names)
        one = str(Path(self.dir.name) / self.names[1])
        self.assertEqual(pipe_stats.sessions(one), [one])

    def test_report_each_session_with_its_gpu_file(self):
        cmd = [sys.executable, str(REPO / "tools" / "pipe_stats.py"), str(self.base), "--skip-s", "0"]
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("2 session files", r.stdout)
        self.assertEqual(r.stdout.count("GPU (joined on a_seq)"), 2)
        for name in self.names:
            self.assertIn(name, r.stdout)
        r = subprocess.run(cmd + ["--latest"], capture_output=True, text=True, timeout=30)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertNotIn(self.names[0], r.stdout)
        self.assertIn(self.names[1], r.stdout)

    def test_latest_same_second_orders_pid_as_number(self):
        # N1 (SENTINEL r2): same UTC second, pids of different widths; a text sort would put
        # -99999 last and --latest would pick it.
        d = Path(self.dir.name)
        same = ["same.20261023T150000Z-99999.csv", "same.20261023T150000Z-100000.csv",
                "same.20261023T150000Z-7.csv", "same.20261023T145959Z-1000000.csv"]
        for seed, name in enumerate(same, start=1):
            main, _, _, _ = simulate(frames=40 + seed, seed=seed)
            (d / name).write_text(main)
        found = [Path(p).name for p in pipe_stats.sessions(str(d / "same.csv"))]
        self.assertEqual(found, ["same.20261023T145959Z-1000000.csv", "same.20261023T150000Z-7.csv",
                                 "same.20261023T150000Z-99999.csv", "same.20261023T150000Z-100000.csv"])
        r = subprocess.run([sys.executable, str(REPO / "tools" / "pipe_stats.py"), str(d / "same.csv"),
                            "--skip-s", "0", "--latest"], capture_output=True, text=True, timeout=30)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("same.20261023T150000Z-100000.csv", r.stdout)
        for name in same[:1] + same[2:]:
            self.assertNotIn(name, r.stdout)

    def test_legacy_file_next_to_sessions_warns(self):
        # O1 (SENTINEL r3): an old <stem>.csv still wins, but says it hides the session files.
        d = Path(self.dir.name)
        main, _, _, _ = simulate(frames=40, seed=11)
        (d / "legacy.csv").write_text(main)
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            found = pipe_stats.sessions(str(d / "legacy.csv"))
        self.assertEqual(found, [str(d / "legacy.csv")])
        self.assertEqual(err.getvalue(), "")  # alone: no warning
        for seed, name in enumerate(["legacy.20261023T150000Z-12.csv",
                                     "legacy.20261023T150001Z-13.csv"], start=1):
            main, _, _, _ = simulate(frames=40 + seed, seed=seed)
            (d / name).write_text(main)
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            found = pipe_stats.sessions(str(d / "legacy.csv"))
        self.assertEqual(found, [str(d / "legacy.csv")])
        self.assertIn("warning:", err.getvalue())
        self.assertIn("2 session file(s)", err.getvalue())
        self.assertIn("legacy.20261023T150001Z-13.csv", err.getvalue())
        r = subprocess.run([sys.executable, str(REPO / "tools" / "pipe_stats.py"), str(d / "legacy.csv"),
                            "--skip-s", "0"], capture_output=True, text=True, timeout=30)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("2 session file(s)", r.stderr)
        self.assertNotIn("legacy.20261023T150000Z-12.csv", r.stdout)

    def test_no_sessions_is_an_error(self):
        r = subprocess.run([sys.executable, str(REPO / "tools" / "pipe_stats.py"),
                            str(Path(self.dir.name) / "none.csv")], capture_output=True, text=True,
                           timeout=30)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("UTC start", r.stderr)


if __name__ == "__main__":
    unittest.main()
