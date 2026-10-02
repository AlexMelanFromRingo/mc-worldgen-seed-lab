#!/usr/bin/env python3
"""Читатель/писатель дампа региона MCR1 (формат — libmcgen/README.md; little-endian).

  char[4] "MCR1"; i32 abi; i32 cx0, cz0, nx, nz, min_y, height; u32 stages;                       (36 байт заголовка)
  по чанкам cz-major (cz = cz0..; cx = cx0..):
      u16 blocks[height*256]            [y][z][x]
      u8  biomes[(height/4)*16]         [qy][qz][qx]
      i16 heightmaps[4][256]            WORLD_SURFACE, OCEAN_FLOOR, MOTION_BLOCKING, MOTION_BLOCKING_NO_LEAVES (абсолютная y)
  таблицы: u32 n_states; n_states строк (u16 длина + UTF-8) — имена состояний; u32 n_biomes; строки имён биомов.

Python API:
  m = Mcr('region.mcr')        m.blocks(cx, cz) -> u16[height,16,16] (memmap, только чтение), m.biomes(cx, cz) -> u8[height/4,4,4],
                               m.heightmaps(cx, cz) -> i16[4,256]; m.state_names, m.biome_names; m.has(cx, cz)
  write_mcr(path, cx0, cz0, nx, nz, min_y, height, stages, chunk_fn, state_names, biome_names, abi=1)
      chunk_fn(cx, cz) -> (blocks[h,16,16] u16, biomes[h/4,4,4] u8, heightmaps[4,256] i16)

CLI:
  mcr.py info region.mcr
  mcr.py from-world <мир> --dim overworld --cx0 -10 --cz0 -10 --nx 21 --nz 21 --out x.mcr      эталонный мир -> MCR1 (для самопроверок)
  mcr.py selftest                                                                              запись/чтение синтетического дампа
"""
import argparse, os, struct, sys
import numpy as np

HDR = struct.Struct('<4siiiiiiiI')   # magic, abi, cx0, cz0, nx, nz, min_y, height, stages
HM_NAMES = ['WORLD_SURFACE', 'OCEAN_FLOOR', 'MOTION_BLOCKING', 'MOTION_BLOCKING_NO_LEAVES']


def chunk_stride(height):
    return height * 256 * 2 + (height // 4) * 16 + 4 * 256 * 2


def _read_strings(buf, p, n):
    out = []
    for _ in range(n):
        ln = struct.unpack_from('<H', buf, p)[0]; p += 2
        out.append(bytes(buf[p:p + ln]).decode('utf-8')); p += ln
    return out, p


class Mcr:
    def __init__(self, path):
        self.path = path
        self.mm = np.memmap(path, dtype=np.uint8, mode='r')
        magic, self.abi, self.cx0, self.cz0, self.nx, self.nz, self.min_y, self.height, self.stages = HDR.unpack_from(self.mm, 0)
        if magic != b'MCR1':
            raise ValueError(f'{path}: не MCR1 (magic={magic!r})')
        if self.height % 16:
            raise ValueError(f'{path}: height={self.height} не кратно 16')
        self.stride = chunk_stride(self.height)
        t = HDR.size + self.nx * self.nz * self.stride
        if t + 8 > len(self.mm):
            raise ValueError(f'{path}: файл обрезан (ожидалось >= {t + 8} байт, есть {len(self.mm)})')
        n_states = struct.unpack_from('<I', self.mm, t)[0]
        self.state_names, p = _read_strings(self.mm, t + 4, n_states)
        n_biomes = struct.unpack_from('<I', self.mm, p)[0]
        self.biome_names, p = _read_strings(self.mm, p + 4, n_biomes)
        self.size_ok = (p == len(self.mm))

    def has(self, cx, cz):
        return self.cx0 <= cx < self.cx0 + self.nx and self.cz0 <= cz < self.cz0 + self.nz

    def _off(self, cx, cz):
        if not self.has(cx, cz):
            raise KeyError((cx, cz))
        return HDR.size + ((cz - self.cz0) * self.nx + (cx - self.cx0)) * self.stride

    def blocks(self, cx, cz):
        o = self._off(cx, cz)
        return self.mm[o:o + self.height * 512].view('<u2').reshape(self.height, 16, 16)

    def biomes(self, cx, cz):
        o = self._off(cx, cz) + self.height * 512
        return self.mm[o:o + (self.height // 4) * 16].reshape(self.height // 4, 4, 4)

    def heightmaps(self, cx, cz):
        o = self._off(cx, cz) + self.height * 512 + (self.height // 4) * 16
        return self.mm[o:o + 2048].view('<i2').reshape(4, 256)

    def chunks(self):
        return [(self.cx0 + i, self.cz0 + j) for j in range(self.nz) for i in range(self.nx)]


def write_mcr(path, cx0, cz0, nx, nz, min_y, height, stages, chunk_fn, state_names, biome_names, abi=1):
    with open(path, 'wb') as f:
        f.write(HDR.pack(b'MCR1', abi, cx0, cz0, nx, nz, min_y, height, stages))
        for cz in range(cz0, cz0 + nz):
            for cx in range(cx0, cx0 + nx):
                b, bi, hm = chunk_fn(cx, cz)
                assert b.shape == (height, 16, 16) and bi.shape == (height // 4, 4, 4) and hm.shape == (4, 256)
                f.write(np.ascontiguousarray(b, dtype='<u2').tobytes())
                f.write(np.ascontiguousarray(bi, dtype=np.uint8).tobytes())
                f.write(np.ascontiguousarray(hm, dtype='<i2').tobytes())
        for names in (state_names, biome_names):
            f.write(struct.pack('<I', len(names)))
            for s in names:
                e = s.encode('utf-8')
                f.write(struct.pack('<H', len(e)) + e)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('cmd', choices=['info', 'from-world', 'selftest'])
    ap.add_argument('arg', nargs='?')
    ap.add_argument('--dim', default='overworld')
    ap.add_argument('--version', default='26.3')
    ap.add_argument('--cx0', type=int, default=0)
    ap.add_argument('--cz0', type=int, default=0)
    ap.add_argument('--nx', type=int, default=1)
    ap.add_argument('--nz', type=int, default=1)
    ap.add_argument('--stages', type=lambda s: int(s, 0), default=0)
    ap.add_argument('--out')
    a = ap.parse_args()
    if a.cmd == 'info':
        m = Mcr(a.arg)
        print(f'{a.arg}: abi {m.abi}, область cx {m.cx0}..{m.cx0 + m.nx - 1} cz {m.cz0}..{m.cz0 + m.nz - 1} ({m.nx}x{m.nz}), '
              f'min_y {m.min_y}, height {m.height}, stages 0x{m.stages:x}, состояний {len(m.state_names)}, биомов {len(m.biome_names)}, '
              f'размер ок: {m.size_ok}')
    elif a.cmd == 'from-world':
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import anvil
        w = anvil.World(a.arg, a.dim, a.version)
        names = [w.states.names.get(i, '') for i in range(w.states.count)]
        first = w.chunk(a.cx0, a.cz0)
        write_mcr(a.out, a.cx0, a.cz0, a.nx, a.nz, first.min_y, first.blocks.shape[0], a.stages,
                  lambda cx, cz: (lambda c: (c.blocks, c.biomes, c.heightmaps))(w.chunk(cx, cz)), names, w.biomes.names)
        print('записан', a.out)
    else:
        import tempfile
        rng = np.random.default_rng(1)
        H = 64
        data = {(cx, cz): (rng.integers(0, 50, (H, 16, 16), dtype=np.uint16), rng.integers(0, 5, (H // 4, 4, 4), dtype=np.uint8),
                           rng.integers(-64, 60, (4, 256), dtype=np.int16)) for cx in range(3) for cz in range(2)}
        p = tempfile.mktemp(suffix='.mcr')
        write_mcr(p, 0, 0, 3, 2, -64, H, 0x3f, lambda cx, cz: data[(cx, cz)], ['minecraft:air'] + [f'b{i}' for i in range(49)], ['a', 'b', 'ы', 'd', 'e'])
        m = Mcr(p)
        assert m.size_ok and m.biome_names[2] == 'ы' and len(m.state_names) == 50
        for (cx, cz), (b, bi, hm) in data.items():
            assert (m.blocks(cx, cz) == b).all() and (m.biomes(cx, cz) == bi).all() and (m.heightmaps(cx, cz) == hm).all()
        os.remove(p)
        print('selftest OK')


if __name__ == '__main__':
    main()
