"""Чтение настоящих чанков из .mca (Anvil, 26.x) для тестов W4: блоки (состояния -> id по таблице) и биомы секций.
Без bpy и без внешних зависимостей (numpy + zlib)."""
import os
import struct
import zlib

import numpy as np


class _R:
    __slots__ = ('b', 'p')

    def __init__(self, b):
        self.b, self.p = b, 0


def _payload(r, t):
    b = r.b
    if t == 1:
        v = struct.unpack_from('>b', b, r.p)[0]; r.p += 1; return v
    if t == 2:
        v = struct.unpack_from('>h', b, r.p)[0]; r.p += 2; return v
    if t == 3:
        v = struct.unpack_from('>i', b, r.p)[0]; r.p += 4; return v
    if t == 4:
        v = struct.unpack_from('>q', b, r.p)[0]; r.p += 8; return v
    if t == 5:
        v = struct.unpack_from('>f', b, r.p)[0]; r.p += 4; return v
    if t == 6:
        v = struct.unpack_from('>d', b, r.p)[0]; r.p += 8; return v
    if t == 7:
        n = struct.unpack_from('>i', b, r.p)[0]; r.p += 4
        v = b[r.p:r.p + n]; r.p += n; return v
    if t == 8:
        n = struct.unpack_from('>H', b, r.p)[0]; r.p += 2
        v = b[r.p:r.p + n].decode('utf-8', 'replace'); r.p += n; return v
    if t == 9:
        it = b[r.p]; n = struct.unpack_from('>i', b, r.p + 1)[0]; r.p += 5
        return [_payload(r, it) for _ in range(n)]
    if t == 10:
        d = {}
        while True:
            tt = b[r.p]; r.p += 1
            if tt == 0:
                return d
            nl = struct.unpack_from('>H', b, r.p)[0]; r.p += 2
            name = b[r.p:r.p + nl].decode('utf-8', 'replace'); r.p += nl
            d[name] = _payload(r, tt)
    if t == 11:
        n = struct.unpack_from('>i', b, r.p)[0]; r.p += 4
        v = np.frombuffer(b, '>i4', n, r.p); r.p += 4 * n; return v
    if t == 12:
        n = struct.unpack_from('>i', b, r.p)[0]; r.p += 4
        v = np.frombuffer(b, '>i8', n, r.p); r.p += 8 * n; return v
    raise ValueError('NBT тип %d' % t)


def parse_nbt(b):
    r = _R(b)
    t = b[0]
    nl = struct.unpack_from('>H', b, 1)[0]
    r.p = 3 + nl
    return _payload(r, t)


def read_chunk_nbt(region_dir, cx, cz):
    rx, rz = cx >> 5, cz >> 5
    path = os.path.join(region_dir, 'r.%d.%d.mca' % (rx, rz))
    if not os.path.isfile(path):
        return None
    with open(path, 'rb') as f:
        f.seek(((cx & 31) + (cz & 31) * 32) * 4)
        e = f.read(4)
        if e == b'\0\0\0\0' or len(e) < 4:
            return None
        off = int.from_bytes(e[:3], 'big') * 4096
        f.seek(off)
        ln = struct.unpack('>i', f.read(4))[0]
        comp = f.read(1)[0]
        data = f.read(ln - 1)
    if comp == 2:
        data = zlib.decompress(data)
    elif comp == 1:
        import gzip
        data = gzip.decompress(data)
    elif comp != 3:
        raise ValueError('сжатие %d' % comp)
    return parse_nbt(data)


def _unpack(longs, bits, count):
    per = 64 // bits
    arr = longs.astype('>i8').view('>u8').astype(np.uint64)
    out = np.empty(len(arr) * per, dtype=np.uint16)
    mask = np.uint64((1 << bits) - 1)
    for k in range(per):
        out[k::per] = ((arr >> np.uint64(k * bits)) & mask).astype(np.uint16)
    return out[:count]


def _entry(entry):
    """Запись палитры 26.x: строка | {'': имя} | {'id': имя, 'properties': {...}} | {'Name': имя, 'Properties': {...}} -> (имя, свойства|None)."""
    if isinstance(entry, str):
        return entry, None
    name = entry.get('') or entry.get('id') or entry.get('Name') or ''
    props = entry.get('properties') or entry.get('Properties')
    return name, props


def load_chunk(region_dir, cx, cz, state_of, biome_index, min_y=-64, height=384, missing=None):
    """-> (blocks u16[height*256], biomes u8[(height//4)*16]) либо None, если чанка нет.
    state_of(имя, свойства|None) -> id или -1; biome_index(name) -> id; missing — множество для неизвестных имён."""
    nbt = read_chunk_nbt(region_dir, cx, cz)
    if nbt is None:
        return None
    secs = nbt.get('sections') or []
    air = state_of('minecraft:air', None)
    blocks = np.full((height // 16, 16, 16, 16), air, dtype=np.uint16)
    biomes = np.zeros((height // 16, 4, 4, 4), dtype=np.uint8)
    for s in secs:
        sy = s['Y'] - (min_y >> 4)
        if not (0 <= sy < height // 16):
            continue
        bs = s.get('block_states')
        if bs:
            pal = []
            for p in bs['palette']:
                n, pr = _entry(p)
                i = state_of(n, pr)
                if i < 0:
                    if missing is not None:
                        missing.add(n)
                    i = air
                pal.append(i)
            if len(pal) == 1 or 'data' not in bs:
                blocks[sy] = pal[0]
            else:
                bits = max(4, (len(pal) - 1).bit_length())
                idx = _unpack(bs['data'], bits, 4096)
                blocks[sy] = np.array(pal, dtype=np.uint16)[idx].reshape(16, 16, 16)
        bb = s.get('biomes')
        if bb:
            pal = [biome_index(n if isinstance(n, str) else n.get('Name', '')) for n in bb['palette']]
            if len(pal) == 1 or 'data' not in bb:
                biomes[sy] = pal[0]
            else:
                bits = max(1, (len(pal) - 1).bit_length())
                idx = _unpack(bb['data'], bits, 64)
                biomes[sy] = np.array(pal, dtype=np.uint8)[idx].reshape(4, 4, 4)
    return blocks.reshape(-1), biomes.reshape(-1)


def list_chunks(region_dir):
    """Все (cx, cz) во всех .mca каталога."""
    out = []
    for f in sorted(os.listdir(region_dir)):
        if not f.endswith('.mca'):
            continue
        parts = f.split('.')
        rx, rz = int(parts[1]), int(parts[2])
        with open(os.path.join(region_dir, f), 'rb') as fh:
            hdr = fh.read(4096)
        for i in range(1024):
            if hdr[i * 4:i * 4 + 4] != b'\0\0\0\0':
                out.append((rx * 32 + (i & 31), rz * 32 + (i >> 5)))
    return out
