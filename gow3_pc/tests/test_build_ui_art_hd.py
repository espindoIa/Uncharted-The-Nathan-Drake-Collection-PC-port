"""Tests of tools/hd_art.py (the `hd` step of tools/build_ui_art.py): encoders, mips, DDS header, SVG.

Run: python -m unittest test_build_ui_art_hd  (from gow3_pc/tests; needs numpy and Pillow).
"""
import struct
import sys
import unittest

import numpy as np

from paths import ROOT

TOOLS = ROOT.parent / "tools"
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))
import hd_art  # noqa: E402


def test_image(w=64, h=32):
    """A colour gradient with a hard diagonal edge and a bit of alpha ramp."""
    y, x = np.mgrid[0:h, 0:w].astype(np.float32)
    r = x / (w - 1) * 255
    g = y / (h - 1) * 255
    b = np.where(x + 2 * y > w, 220.0, 30.0)
    a = 128 + x / (w - 1) * 127
    return np.stack([r, g, b, a], -1).round().astype(np.uint8)


def rmse(a, b):
    return float(np.sqrt(np.mean((a.astype(np.float32) - b.astype(np.float32)) ** 2)))


class EncoderTests(unittest.TestCase):
    def test_bc4_round_trip(self):
        img = test_image()
        plane = img[..., :1]
        out = hd_art.decode_level(hd_art.encode_level(np.repeat(plane, 4, 2), "bc4"), "bc4", 64, 32)
        err = rmse(out[..., 0], plane[..., 0])
        print(f"\nBC4 RMSE {err:.2f}")
        self.assertLess(err, 1.5)

    def test_bc4_extremes_and_flat(self):
        flat = np.full((4, 4, 4), 255, np.uint8)
        self.assertTrue((hd_art.decode_level(hd_art.encode_level(flat, "bc4"), "bc4", 4, 4)[..., 0] == 255).all())
        edge = np.zeros((4, 4, 4), np.uint8)
        edge[:, 2:] = 255
        self.assertTrue((hd_art.decode_level(hd_art.encode_level(edge, "bc4"), "bc4", 4, 4)[..., 0] == edge[..., 0]).all())

    def test_bc1_round_trip(self):
        img = test_image()
        img[..., 3] = 255
        out = hd_art.decode_level(hd_art.encode_level(img, "bc1"), "bc1", 64, 32)
        err = rmse(out[..., :3], img[..., :3])
        print(f"BC1 RMSE {err:.2f}")
        self.assertLess(err, 6.0)
        self.assertTrue((out[..., 3] == 255).all())

    def test_bc3_round_trip(self):
        img = test_image()
        out = hd_art.decode_level(hd_art.encode_level(img, "bc3"), "bc3", 64, 32)
        err_c, err_a = rmse(out[..., :3], img[..., :3]), rmse(out[..., 3], img[..., 3])
        print(f"BC3 RMSE colour {err_c:.2f} alpha {err_a:.2f}")
        self.assertLess(err_c, 6.0)
        self.assertLess(err_a, 1.5)

    def test_sizes_not_multiple_of_four(self):
        img = test_image(7, 3)
        for kind, size in (("bc1", 16), ("bc3", 32), ("bc4", 16), ("bc5", 32)):  # 2 blocks wide
            self.assertEqual(len(hd_art.encode_level(img, kind)), size)
            self.assertEqual(hd_art.decode_level(hd_art.encode_level(img, kind), kind, 7, 3).shape, (3, 7, 4))


class MipTests(unittest.TestCase):
    def test_chain_sizes(self):
        levels = hd_art.mip_chain(test_image(64, 32), False)
        self.assertEqual([l.shape[:2] for l in levels], [(32, 64), (16, 32), (8, 16), (4, 8), (2, 4), (1, 2), (1, 1)])

    def test_premultiplied_edge_has_no_dark_fringe(self):
        img = np.zeros((2, 2, 4), np.uint8)
        img[0] = [255, 255, 255, 255]  # top row white, bottom row transparent black
        last = hd_art.mip_chain(img, False)[1]
        self.assertEqual(tuple(last[0, 0]), (255, 255, 255, 128))

    def test_srgb_filters_in_linear_light(self):
        img = np.zeros((2, 2, 4), np.uint8)
        img[..., 3] = 255
        img[0] = 255  # half white, half black: linear mean 0.5 is sRGB 188
        self.assertEqual(int(hd_art.mip_chain(img, True)[1][0, 0, 0]), 188)
        self.assertIn(int(hd_art.mip_chain(img, False)[1][0, 0, 0]), (127, 128))


class DdsTests(unittest.TestCase):
    def test_header(self):
        img = test_image(64, 32)
        data = hd_art.build_dds(img, "bc3", True)
        magic, size, flags, h, w, _, _, mips = struct.unpack_from("<4s7I", data, 0)
        self.assertEqual((magic, size, h, w, mips), (b"DDS ", 124, 32, 64, 7))
        self.assertTrue(flags & 0x20000)
        self.assertEqual(struct.unpack_from("<I4s", data, 80), (4, b"DX10"))
        self.assertEqual(struct.unpack_from("<4I", data, 128), (78, 3, 0, 1))  # BC3_UNORM_SRGB, 2D, array 1
        payload = sum(max(1, (max(w >> m, 1) + 3) // 4) * ((max(h >> m, 1) + 3) // 4) * 16 for m in range(7))
        self.assertEqual(len(data), 148 + payload)
        self.assertEqual(struct.unpack_from("<I", hd_art.build_dds(img, "bc4", True), 128)[0], 80)  # BC4 has no sRGB
        self.assertEqual(struct.unpack_from("<I", hd_art.build_dds(img, "rgba", False), 128)[0], 28)


class SvgTests(unittest.TestCase):
    def test_circle_coverage_and_colour(self):
        svg = '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 10 10"><circle cx="5" cy="5" r="4" fill="#ff0000"/></svg>'
        img, warnings = hd_art.render_svg(svg, 100, 100)
        self.assertEqual(warnings, [])
        area = img[..., 3].astype(np.float32).sum() / 255
        self.assertAlmostEqual(area, np.pi * 40 * 40, delta=np.pi * 40 * 40 * 0.01)
        self.assertEqual(tuple(img[50, 50]), (255, 0, 0, 255))
        self.assertEqual(img[0, 0, 3], 0)

    def test_even_odd_hole_transform_and_opacity(self):
        svg = ('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 10 10"><g transform="translate(2 2) scale(0.5)" fill-opacity="0.5">'
               '<path fill-rule="evenodd" fill="#00ff00" d="M0 0H12V12H0Z M4 4H8V8H4Z"/></g></svg>')
        img, _ = hd_art.render_svg(svg, 40, 40)
        self.assertEqual(img[4 * 2 + 1, 4 * 2 + 1, 3], 128)  # inside the frame (x = 2 + 0.5 * 1 -> 10 px at 4 px/unit)
        self.assertEqual(img[4 * 4 + 2, 4 * 4 + 2, 3], 0)  # hole centre (unit 4..6 -> 16..24 px)
        self.assertEqual(img[3, 3, 3], 0)  # outside the group's offset


if __name__ == "__main__":
    unittest.main()
