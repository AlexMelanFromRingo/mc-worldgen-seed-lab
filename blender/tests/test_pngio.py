import os
import struct
import unittest
import zlib

import numpy as np

from _boot import ASSETS_DIR, have_resources
from mcgen_addon.assets import pngio


def _rand_samples(h, w, ch, depth, rng):
    return rng.integers(0, 1 << depth, size=(h, w, ch), dtype=np.uint32).astype(np.uint16)


class TestPng(unittest.TestCase):
    def test_all_formats(self):
        rng = np.random.default_rng(1)
        for ctype, depths in pngio.VALID_DEPTHS.items():
            for depth in depths:
                for il in (False, True):
                    for fm in ('none', 'sub', 'up', 'avg', 'paeth', 'mixed'):
                        for (h, w) in ((7, 9), (1, 1), (16, 16), (5, 33)):
                            ch = pngio.CHANNELS[ctype]
                            s = _rand_samples(h, w, ch, depth, rng)
                            pal = None
                            if ctype == 3:
                                n = 1 << depth
                                pal = rng.integers(0, 256, size=n * 3, dtype=np.uint8).tobytes()
                            data = pngio.encode_png(s, ctype, depth, palette=pal, interlace=il, filter_mode=fm)
                            rgba, info = pngio.decode_png(data, check_crc=True)
                            self.assertEqual((info.width, info.height, info.bit_depth, info.color_type), (w, h, depth, ctype))
                            self.assertEqual(rgba.shape, (h, w, 4))
                            exp = self._expected(s, ctype, depth, pal)
                            np.testing.assert_array_equal(rgba, exp, '%d/%d il=%s f=%s' % (ctype, depth, il, fm))

    @staticmethod
    def _expected(s, ctype, depth, pal):
        h, w, ch = s.shape
        out = np.zeros((h, w, 4), dtype=np.uint8)
        out[:, :, 3] = 255

        def sc(a):
            if depth == 16:
                return (a >> 8).astype(np.uint8)
            if depth == 8:
                return a.astype(np.uint8)
            return (a.astype(np.uint32) * 255 // ((1 << depth) - 1)).astype(np.uint8)
        if ctype == 0:
            out[:, :, :3] = sc(s[:, :, :1])
        elif ctype == 2:
            out[:, :, :3] = sc(s)
        elif ctype == 3:
            p = np.frombuffer(pal, dtype=np.uint8).reshape(-1, 3)
            out[:, :, :3] = p[s[:, :, 0]]
        elif ctype == 4:
            out[:, :, :3] = sc(s[:, :, :1])
            out[:, :, 3] = sc(s[:, :, 1])
        else:
            out = sc(s)
        return out

    def test_trns(self):
        rng = np.random.default_rng(2)
        # палитра + tRNS
        s = rng.integers(0, 4, size=(4, 5, 1)).astype(np.uint16)
        pal = bytes(range(12))
        data = pngio.encode_png(s, 3, 2, palette=pal, trns=bytes([0, 128, 255]))
        rgba = pngio.decode_png(data)[0]
        exp_a = np.array([0, 128, 255, 255])[s[:, :, 0]]
        np.testing.assert_array_equal(rgba[:, :, 3], exp_a)
        # серый 8 бит + прозрачный цвет
        g = np.array([[[10], [20]], [[20], [30]]], dtype=np.uint16)
        rgba = pngio.decode_png(pngio.encode_png(g, 0, 8, trns=struct.pack('>H', 20)))[0]
        self.assertEqual(rgba[:, :, 3].tolist(), [[255, 0], [0, 255]])
        # RGB 16 бит + прозрачный цвет
        c = np.array([[[1000, 2000, 3000], [1, 2, 3]]], dtype=np.uint16)
        rgba = pngio.decode_png(pngio.encode_png(c, 2, 16, trns=struct.pack('>HHH', 1, 2, 3)))[0]
        self.assertEqual(rgba[:, :, 3].tolist(), [[255, 0]])

    def test_roundtrip_rgba(self):
        rng = np.random.default_rng(3)
        a = rng.integers(0, 256, size=(13, 17, 4), dtype=np.uint8)
        np.testing.assert_array_equal(pngio.read_png(pngio.encode_rgba(a)), a)

    def test_bad(self):
        with self.assertRaises(pngio.PngError):
            pngio.decode_png(b'not a png at all')
        good = pngio.encode_rgba(np.zeros((2, 2, 4), np.uint8))
        bad = bytearray(good)
        bad[-20] ^= 0xff
        with self.assertRaises(Exception):
            pngio.decode_png(bytes(bad), check_crc=True)

    @unittest.skipUnless(have_resources(), 'нет ресурсов клиента')
    def test_real_textures(self):
        base = os.path.join(ASSETS_DIR, 'assets', 'minecraft', 'textures')
        n = 0
        for root, _, files in os.walk(os.path.join(base, 'block')):
            for f in files:
                if f.endswith('.png'):
                    p = os.path.join(root, f)
                    with open(p, 'rb') as fh:
                        data = fh.read()
                    rgba, info = pngio.decode_png(data, check_crc=True)
                    self.assertEqual(rgba.shape[:2], (info.height, info.width))
                    n += 1
        self.assertGreater(n, 1000)
        g = pngio.read_png(os.path.join(base, 'colormap', 'grass.png'))
        self.assertEqual(g.shape, (256, 256, 4))


if __name__ == '__main__':
    unittest.main()
