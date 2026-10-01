#!/usr/bin/env python3
"""Минимальный читатель Anvil (.mca) для 26.x: возвращает блоки чанка как numpy-массив [y, z, x] с глобальной таблицей имён.
Только то, что нужно для аудита: block_states секций (палитра + упакованные индексы), без биомов/сущностей."""
import os, struct, zlib
import numpy as np

NAMES = {}          # имя блока -> id
NAME_LIST = []


def bid(name):
    i = NAMES.get(name)
    if i is None:
        i = len(NAME_LIST); NAMES[name] = i; NAME_LIST.append(name)
    return i


class R:
    def __init__(self, b):
        self.b = b; self.p = 0

    def u(self, fmt, n):
        v = struct.unpack_from(fmt, self.b, self.p); self.p += n; return v[0]


def read_payload(r, t):
    if t == 1: return r.u('>b', 1)
    if t == 2: return r.u('>h', 2)
    if t == 3: return r.u('>i', 4)
    if t == 4: return r.u('>q', 8)
    if t == 5: return r.u('>f', 4)
    if t == 6: return r.u('>d', 8)
    if t == 7:
        n = r.u('>i', 4); v = r.b[r.p:r.p + n]; r.p += n; return v
    if t == 8:
        n = r.u('>H', 2); v = r.b[r.p:r.p + n].decode('utf-8', 'replace'); r.p += n; return v
    if t == 9:
        it = r.u('>b', 1); n = r.u('>i', 4)
        return [read_payload(r, it) for _ in range(n)]
    if t == 10:
        d = {}
        while True:
            tt = r.u('>b', 1)
            if tt == 0: return d
            nl = r.u('>H', 2); name = r.b[r.p:r.p + nl].decode('utf-8', 'replace'); r.p += nl
            d[name] = read_payload(r, tt)
    if t == 11:
        n = r.u('>i', 4); v = np.frombuffer(r.b, '>i4', n, r.p); r.p += 4 * n; return v
    if t == 12:
        n = r.u('>i', 4); v = np.frombuffer(r.b, '>i8', n, r.p); r.p += 8 * n; return v
    raise ValueError(t)


def parse_nbt(b):
    r = R(b); t = r.u('>b', 1); nl = r.u('>H', 2); r.p += nl
    return read_payload(r, t)


def read_chunk_nbt(path, cx, cz):
    with open(path, 'rb') as f:
        idx = (cx & 31) + (cz & 31) * 32
        f.seek(idx * 4); e = f.read(4)
        if e == b'\0\0\0\0': return None
        off = int.from_bytes(e[:3], 'big') * 4096
        f.seek(off); ln = struct.unpack('>i', f.read(4))[0]; comp = f.read(1)[0]
        data = f.read(ln - 1)
    if comp == 2: data = zlib.decompress(data)
    elif comp == 1: import gzip; data = gzip.decompress(data)
    elif comp == 3: pass
    else: raise ValueError('compression %d' % comp)
    return parse_nbt(data)


def unpack(longs, bits, count=4096):
    """упаковка без перехода значений между long (формат 1.16+)"""
    per = 64 // bits
    arr = longs.astype('>i8').view('>u8').astype(np.uint64)
    out = np.empty(len(arr) * per, dtype=np.uint16)
    mask = np.uint64((1 << bits) - 1)
    for k in range(per):
        out[k::per] = ((arr >> np.uint64(k * bits)) & mask).astype(np.uint16)
    return out[:count]


def chunk_blocks(nbt):
    """-> (min_y, array[y,z,x] uint16 id) ; секции без данных (одна запись палитры) заполняются целиком."""
    secs = nbt.get('sections') or nbt.get('Level', {}).get('Sections')
    ys = [s['Y'] for s in secs]
    y0 = min(ys); y1 = max(ys)
    arr = np.zeros(((y1 - y0 + 1) * 16, 16, 16), dtype=np.uint16)
    air = bid('minecraft:air')
    arr[:] = air
    for s in secs:
        bs = s.get('block_states')
        if not bs: continue
        pal = [bid(p if isinstance(p, str) else (p.get('Name') or p.get('') or str(p))) for p in bs['palette']]
        if len(pal) == 1 or 'data' not in bs:
            blk = np.full(4096, pal[0], dtype=np.uint16)
        else:
            bits = max(4, (len(pal) - 1).bit_length())
            idx = unpack(bs['data'], bits)
            blk = np.array(pal, dtype=np.uint16)[idx]
        arr[(s['Y'] - y0) * 16:(s['Y'] - y0 + 1) * 16] = blk.reshape(16, 16, 16)
    return y0 * 16, arr


if __name__ == '__main__':
    import sys
    p = sys.argv[1]; cx, cz = int(sys.argv[2]), int(sys.argv[3])
    n = read_chunk_nbt(p, cx, cz)
    print(list(n.keys())[:20], 'status', n.get('Status') or n.get('status'))
    y0, a = chunk_blocks(n)
    import collections
    c = collections.Counter(NAME_LIST[i] for i in a.ravel().tolist())
    print(y0, a.shape, c.most_common(10))
