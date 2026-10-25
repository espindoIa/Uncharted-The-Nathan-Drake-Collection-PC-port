"""Pure helpers of tools/session_recorder.py (no Windows API, no game). Run from this folder:
python -m unittest test_session_recorder"""
from paths import ROOT
import struct
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

sys.path.insert(0, str(ROOT.parent / 'tools'))
import session_recorder as sr

STATS = ("Frame stats: 59.8 FPS, worst frame {worst} ms (vblank 60 Hz); {compiles} shader/pipeline compiles, "
         "1.0 ms; async: capture 0.0 ms")


class ClassifyTests(unittest.TestCase):
    def test_frame_stats_parse(self):
        self.assertEqual(sr.parse_frame_stats(STATS.format(worst='55.3', compiles=2)),
                         {'fps': 59.8, 'worst_ms': 55.3, 'compiles': 2})
        self.assertIsNone(sr.parse_frame_stats('Warning: something'))

    def test_slow_frame_and_events(self):
        self.assertIsNone(sr.classify_line(STATS.format(worst='16.7', compiles=0)))
        self.assertEqual(sr.classify_line(STATS.format(worst='41.0', compiles=0)), 'slow-frame')
        self.assertEqual(sr.classify_line(STATS.format(worst='10.0', compiles=1)), 'slow-frame')
        self.assertEqual(sr.classify_line('Texture cache: memory is 91% used'), 'event')
        self.assertEqual(sr.classify_line('first unsupported PS4 import: foo'), 'event')
        self.assertIsNone(sr.classify_line('Loading level 3'))

    def test_duplicates_collapse_after_twenty(self):
        log = sr.EventLog()
        verdicts = [log.offer('Warning: x') for _ in range(23)]
        self.assertEqual(verdicts[:20], [True] * 20)
        self.assertEqual(verdicts[20:], ['note', False, False])
        self.assertEqual(log.dropped(), 3)
        self.assertEqual(log.top(), [('Warning: x', 23)])


class TailerTests(unittest.TestCase):
    def test_partial_line_and_truncation(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'boot.log'
            tailer = sr.LogTailer(path)
            self.assertEqual(tailer.poll(), [])          # no file yet
            path.write_bytes(b'one\r\ntwo\nthr')
            self.assertEqual(tailer.poll(), ['one', 'two'])
            with open(path, 'ab') as f:
                f.write(b'ee\n')
            self.assertEqual(tailer.poll(), ['three'])
            self.assertEqual((tailer.lineno, tailer.offset), (3, 15))
            path.write_bytes(b'new\n')                   # a new launch overwrote the log
            self.assertEqual(tailer.poll(), ['new'])
            self.assertEqual(tailer.resets, 1)
            with open(path, 'ab') as f:
                f.write(b'tail without newline')
            self.assertEqual(tailer.poll(), [])
            self.assertEqual(tailer.poll(flush=True), ['tail without newline'])


class ImageTests(unittest.TestCase):
    def test_png_roundtrip(self):
        rgb = bytes([255, 0, 0, 0, 255, 0, 0, 0, 255, 9, 8, 7])    # 2x2
        png = sr.png_encode(2, 2, rgb)
        self.assertEqual(png[:8], b'\x89PNG\r\n\x1a\n')
        chunks, pos = {}, 8
        while pos < len(png):
            size, tag = struct.unpack('>I4s', png[pos:pos + 8])
            body = png[pos + 8:pos + 8 + size]
            self.assertEqual(struct.unpack('>I', png[pos + 8 + size:pos + 12 + size])[0],
                             zlib.crc32(tag + body) & 0xFFFFFFFF)
            chunks[tag] = body
            pos += 12 + size
        self.assertEqual(struct.unpack('>IIBBBBB', chunks[b'IHDR']), (2, 2, 8, 2, 0, 0, 0))
        self.assertEqual(zlib.decompress(chunks[b'IDAT']), b'\0' + rgb[:6] + b'\0' + rgb[6:])

    def test_bgra_and_black(self):
        self.assertEqual(sr.bgra_to_rgb(bytes([1, 2, 3, 255, 4, 5, 6, 255])), bytes([3, 2, 1, 6, 5, 4]))
        self.assertTrue(sr.is_all_black(bytes(12)))
        self.assertFalse(sr.is_all_black(bytes(11) + b'\1'))


FLIGHT = ("# flight v1 unix_ms=1000 reason=texture frames=3\n"
          "seq,unix_ms,t_ms,dt_ms,marker,events,event_names,d_compiles,d_skipped_draws,cam_jump_deg\n"
          "1,1000,0,16.7,0,0,,0,0,0.1\n"
          "2,1017,17,90.5,0,5,stall|reset,2,1,0.2\n"
          "3,1108,108,16.7,0,0x1,stall,1,0,12.5\n")


class FlightTests(unittest.TestCase):
    def summarize(self, text):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'flight_20261025-120000-123_texture.csv'
            path.write_text(text, encoding='utf-8')
            return sr.summarize_flight_csv(path)

    def test_normal(self):
        s = self.summarize(FLIGHT)
        self.assertEqual((s['rows'], s['span_s'], s['dt_ms_max'], s['dt_ms_max_unix_ms']), (3, 0.108, 90.5, 1017))
        self.assertEqual((s['event_rows'], s['event_names']), (2, {'stall': 2, 'reset': 1}))
        self.assertEqual((s['cam_jump_deg_max'], s['cam_jump_unix_ms']), (12.5, 1108))
        self.assertEqual((s['d_compiles'], s['d_skipped_draws']), (3, 1))
        text = sr.format_flight({'flight_file': 'f.csv', 'flight_status': 'ok', 'flight_summary': s})
        for needle in ('3 rows', 'longest flip 90.5 ms', 'stall x2', 'max camera jump 12.5'):
            self.assertIn(needle, text)

    def test_missing_columns(self):
        s = self.summarize('seq,dt_ms\n1,5\n2,x\n')
        self.assertEqual((s['rows'], s['dt_ms_max'], s['dt_ms_max_unix_ms']), (2, 5, None))
        for key in ('event_rows', 'cam_jump_deg_max', 'd_compiles', 'span_s'):
            self.assertNotIn(key, s)
        self.assertIn('2 rows', sr.format_flight({'flight_file': 'f.csv', 'flight_summary': s}))

    def test_empty_and_header_only(self):
        self.assertEqual(self.summarize(''), {'rows': 0})
        self.assertEqual(self.summarize('# flight v1 unix_ms=1\nseq,dt_ms\n'), {'rows': 0})
        self.assertEqual(sr.summarize_flight_csv(Path('no_such_flight.csv'))['rows'], 0)
        self.assertIn('timed out', sr.format_flight({'flight_file': None, 'flight_status': 'timed out'}))

    def test_find_and_label(self):
        self.assertEqual(sr.flight_label('Texture!'), 'texture')
        self.assertEqual(sr.flight_label('???'), 'manual')
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            (d / 'flight_20261025-120000-123_texture.tmp').write_text('x')
            (d / 'flight_20261025-120000-123_auto-stall.csv').write_text('x')
            self.assertIsNone(sr.find_flight(d, 'texture', 0))
            good = d / 'flight_20261025-120001-456_texture.csv'
            good.write_text('x')
            self.assertEqual(sr.find_flight(d, 'texture', 0), good)
            self.assertIsNone(sr.find_flight(d, 'texture', good.stat().st_mtime + 10))   # older than the trigger
            self.assertIsNone(sr.find_flight(d / 'missing', 'texture', 0))

    def test_summary_section(self):
        marks = [{'label': 'texture', 'time': '10:00:00.000', 'elapsed_s': 1.0, 'folder': 'm', 'flight': 'F-TEXT'}]
        text = sr.build_summary({'name': 's', 'folder': 'f', 'start': 'a', 'end': 'b', 'duration_s': 1.0,
                                 'exit_code': 0, 'marks': marks, 'frames': {}, 'metrics': [], 'top_repeats': [],
                                 'files': [], 'flight_auto': [{'name': 'n.csv', 'reason': 'auto-stall', 'time': 't'}] * 2})
        for needle in ('Flight, texture at 10:00:00.000: F-TEXT', 'auto-stall x2', 'boot.timed.log'):
            self.assertIn(needle, text)


class MiscTests(unittest.TestCase):
    def test_keys(self):
        self.assertEqual(sr.parse_keys(sr.DEFAULT_KEYS),
                         {'F7': 'texture', 'F8': 'geometry', 'F9': 'flash', 'F10': 'other'})
        self.assertEqual(sr.virtual_key('F7'), 0x76)
        self.assertEqual(sr.virtual_key('K'), ord('K'))
        with self.assertRaises(ValueError):
            sr.parse_keys('Escape=x')

    def test_exit_codes(self):
        self.assertIn('clean', sr.describe_exit_code(0))
        self.assertIn('access violation', sr.describe_exit_code(0xC0000005))
        self.assertIn('access violation', sr.describe_exit_code(-1073741819))   # signed form of the same
        self.assertIn('CRASH', sr.describe_exit_code(0xC0000999))
        self.assertIn('taskkill', sr.describe_exit_code(1))
        self.assertIn('unknown', sr.describe_exit_code(None))

    def test_cmdline(self):
        quoted = r'"C:\Games\My Port\gow3_pc\out\Uncharted.exe" C:\Games\My Port\analysis\big3-ps4_Shipping\boot-linked.bin --x'
        plain = r'C:\p\gow3_pc\out\Uncharted.exe C:\p\analysis\uncharted\boot-linked.bin'
        self.assertEqual(sr.parse_game_cmdline(quoted), (r'C:\Games\My Port', 'big3-ps4_Shipping'))
        self.assertEqual(sr.parse_game_cmdline(plain), (r'C:\p', 'uncharted'))
        self.assertIsNone(sr.parse_game_cmdline('python.exe -c pass'))

    def test_summary(self):
        windows = [{'fps': 60.0, 'worst_ms': 20.0, 'compiles': 0}, {'fps': 30.0, 'worst_ms': 120.0, 'compiles': 4},
                   {'fps': 45.0, 'worst_ms': 50.0, 'compiles': 1}]
        frames = sr.summarize_frame_stats(windows)
        self.assertEqual((frames['fps_min'], frames['fps_median'], frames['over_40'], frames['over_100'],
                          frames['compiles'], frames['worst_ms']), (30.0, 45.0, 2, 1, 5, 120.0))
        text = sr.build_summary({
            'name': 's', 'folder': r'C:\x\s', 'start': 'a', 'end': 'b', 'duration_s': 90.0, 'exit_code': 0xC0000005,
            'game_ended': True, 'marks': [{'label': 'texture', 'time': '10:00:00.000', 'elapsed_s': 12.0,
                                           'folder': r'marks\01_texture_10-00-00'}],
            'frames': frames, 'metrics': [{'ws_mb': '100', 'cpu_pct': ''}, {'ws_mb': '300', 'cpu_pct': '5.0'}],
            'top_repeats': [('Warning: x', 7)], 'dropped': 0, 'notes': [], 'files': ['summary.md']})
        for needle in ('0xC0000005 CRASH', 'texture at 10:00:00.000', 'median 45.0', 'Working set MB: 200 / 300',
                       'Process CPU % (of all cores): 5.0 / 5.0', '7x `Warning: x`', r'C:\x\s'):
            self.assertIn(needle, text)
        stopped = sr.build_summary({'name': 's', 'folder': 'f', 'start': 'a', 'end': 'b', 'duration_s': 1.0,
                                    'exit_code': None, 'game_ended': False, 'marks': [], 'frames': {},
                                    'metrics': [], 'top_repeats': [], 'files': []})
        self.assertIn('still running', stopped)


if __name__ == '__main__':
    unittest.main()
