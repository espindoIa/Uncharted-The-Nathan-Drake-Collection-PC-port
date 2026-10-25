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
