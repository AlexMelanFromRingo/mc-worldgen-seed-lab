#!/usr/bin/env python3
"""Читатель Anvil (.mca) Minecraft 26.x -> numpy. Часть инструментов tools/gt.

Что отдаёт для чанка (Chunk):
  status   : 'minecraft:full' | 'minecraft:terrain' | ...
  min_y    : нижняя y (секции Y*16)
  blocks   : uint16 [высота][16 z][16 x]  — id состояний по reports/blocks.json пак-каталога (StateTable)
  biomes   : uint8  [высота/4][4][4]     — индексы в таблице имён биомов (BiomeTable), клетки 4x4x4
  heightmaps: int16 [4][256]             — абсолютные y «первой свободной клетки»: WORLD_SURFACE, OCEAN_FLOOR, MOTION_BLOCKING,
                                           MOTION_BLOCKING_NO_LEAVES (как Heightmap.getFirstAvailable; порядок как в MCR1)

Формат палитры блоков 26.x (проверено на 26.3): запись — либо состояние по умолчанию, записанное как компаунд {"": "minecraft:stone"}
(NBT не смешивает строки и компаунды в одном списке; конструкция Codec.either(Block, BlockState)), либо {id, properties{...}}
(полный набор свойств); для старых миров ещё {Name, Properties}. Палитры биомов — строки.
Упаковка: ceil(log2(n)) бит (блоки не менее 4), значения не пересекают границу long (формат 1.16+); одна запись — без data.

Кэш: <мир>/anvilcache/<dim>/r.X.Z.npz (на область; ключ — размер+mtime .mca и хэш таблицы состояний).

CLI:  anvil.py info <мир> [--dim overworld] [--version 26.3]       сводка по статусам и областям
      anvil.py chunk <мир> CX CZ [--dim ..]                         гистограмма блоков чанка
"""
import argparse, collections, glob, gzip, hashlib, json, os, re, struct, sys, zlib
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from common import ROOT, DIMS, pack_dir

HM_KEYS = ['WORLD_SURFACE', 'OCEAN_FLOOR', 'MOTION_BLOCKING', 'MOTION_BLOCKING_NO_LEAVES']


# ----------------------------------------------------------------------------------------------------------------------------
#  Таблицы состояний и биомов
# ----------------------------------------------------------------------------------------------------------------------------
def canon_name(block, props):
    """'minecraft:oak_stairs[facing=north,half=bottom]' — свойства по алфавиту (как BlockState.toString)."""
    if not props:
        return block
    return block + '[' + ','.join(f'{k}={props[k]}' for k in sorted(props)) + ']'


def parse_state_name(name):
    """'minecraft:x[a=b,c=d]' -> ('minecraft:x', (('a','b'),('c','d'))) с отсортированными свойствами."""
    i = name.find('[')
    if i < 0:
        return name, ()
    kv = [p.split('=', 1) for p in name[i + 1:-1].split(',') if p]
    return name[:i], tuple(sorted((k, v) for k, v in kv))


class StateTable:
    """Состояния блоков из reports/blocks.json: id <-> (блок, свойства)."""

    def __init__(self, blocks_json):
        d = json.load(open(blocks_json))
        self.by_key = {}            # (block, ((k,v)..)) -> id
        self.default = {}           # block -> id
        self.default_props = {}     # block -> dict
        self.names = {}             # id -> канонич. имя
        for b, v in d.items():
            for s in v['states']:
                props = s.get('properties', {})
                key = (b, tuple(sorted(props.items())))
                self.by_key[key] = s['id']
                self.names[s['id']] = canon_name(b, props)
                if s.get('default'):
                    self.default[b] = s['id']
                    self.default_props[b] = props
        self.count = max(self.names) + 1
        self.sha = hashlib.sha1(json.dumps(sorted(self.names.items())).encode()).hexdigest()[:12]
        self.air = self.default['minecraft:air']
        self._cache = {}

    @staticmethod
    def load(version):
        return StateTable(f'{pack_dir(version)}/reports/blocks.json')

    def from_palette(self, e):
        """запись палитры NBT -> id состояния"""
        if not isinstance(e, str):
            if '' in e and 'id' not in e and 'Name' not in e:   # NBT не смешивает строки и компаунды в списке: строка = {"": "minecraft:stone"}
                e = e['']
        k = e if isinstance(e, str) else (e.get('id') or e.get('Name'), tuple(sorted((e.get('properties') or e.get('Properties') or {}).items())))
        r = self._cache.get(k)
        if r is not None:
            return r
        if isinstance(e, str):
            r = self.default.get(e)
            if r is None:       # 26.4+: состояние строкой с свойствами `minecraft:x[a=b]`
                r = self.by_key.get(parse_state_name(e))
                if r is None:
                    raise KeyError(f'состояние не найдено в blocks.json: {e}')
        else:
            name = e.get('id') or e.get('Name')
            props = dict(self.default_props.get(name, {}))
            props.update({a: str(b) for a, b in (e.get('properties') or e.get('Properties') or {}).items()})
            r = self.by_key.get((name, tuple(sorted(props.items()))))
            if r is None:
                raise KeyError(f'состояние не найдено в blocks.json: {name} {props}')
        self._cache[k] = r
        return r

    def id_of(self, name):
        b, props = parse_state_name(name)
        return self.by_key[(b, props)]


class BiomeTable:
    """Имена биомов <-> u8. Базовый порядок — алфавитный по файлам worldgen/biome пак-каталога; неизвестные дописываются."""

    def __init__(self, version=None, names=None):
        if names is None:
            names = sorted('minecraft:' + os.path.relpath(p, f'{pack_dir(version)}/data/minecraft/worldgen/biome')[:-5]
                           for p in glob.glob(f'{pack_dir(version)}/data/minecraft/worldgen/biome/**/*.json', recursive=True))
        self.names = list(names)
        self.idx = {n: i for i, n in enumerate(self.names)}

    def get(self, name):
        i = self.idx.get(name)
        if i is None:
            i = len(self.names); self.names.append(name); self.idx[name] = i
        return i


# ----------------------------------------------------------------------------------------------------------------------------
#  NBT (минимальный быстрый парсер)
# ----------------------------------------------------------------------------------------------------------------------------
_S = struct.Struct


def _read(b, p, t):
    if t == 10:
        d = {}
        while True:
            tt = b[p]; p += 1
            if tt == 0:
                return d, p
            nl = (b[p] << 8) | b[p + 1]; p += 2
            name = b[p:p + nl].decode('utf-8', 'replace'); p += nl
            d[name], p = _read(b, p, tt)
    if t == 1: return b[p] - 256 if b[p] > 127 else b[p], p + 1
    if t == 2: return _S('>h').unpack_from(b, p)[0], p + 2
    if t == 3: return _S('>i').unpack_from(b, p)[0], p + 4
    if t == 4: return _S('>q').unpack_from(b, p)[0], p + 8
    if t == 5: return _S('>f').unpack_from(b, p)[0], p + 4
    if t == 6: return _S('>d').unpack_from(b, p)[0], p + 8
    if t == 7:
        n = _S('>i').unpack_from(b, p)[0]; p += 4
        return bytes(b[p:p + n]), p + n
    if t == 8:
        n = (b[p] << 8) | b[p + 1]; p += 2
        return b[p:p + n].decode('utf-8', 'replace'), p + n
    if t == 9:
        it = b[p]; n = _S('>i').unpack_from(b, p + 1)[0]; p += 5
        out = []
        for _ in range(n):
            v, p = _read(b, p, it); out.append(v)
        return out, p
    if t == 11:
        n = _S('>i').unpack_from(b, p)[0]; p += 4
        return np.frombuffer(b, '>i4', n, p), p + 4 * n
    if t == 12:
        n = _S('>i').unpack_from(b, p)[0]; p += 4
        return np.frombuffer(b, '>i8', n, p), p + 8 * n
    raise ValueError(f'NBT tag {t}')


def parse_nbt(b):
    t = b[0]
    nl = (b[1] << 8) | b[2]
    v, _ = _read(b, 3 + nl, t)
    return v


# ----------------------------------------------------------------------------------------------------------------------------
#  Region-файлы
# ----------------------------------------------------------------------------------------------------------------------------
def region_dir(world, dim):
    """<мир>/dimensions/minecraft/<dim>/region (26.x) либо старая раскладка."""
    dim = DIMS.get(dim, dim)
    cands = [f'{world}/dimensions/minecraft/{dim}/region']
    cands += [f'{world}/region'] if dim == 'overworld' else [f'{world}/DIM-1/region'] if dim == 'the_nether' else [f'{world}/DIM1/region']
    for c in cands:
        if os.path.isdir(c):
            return c
    return cands[0]


def _decompress(comp, data):
    c = comp & 127
    if c == 2: return zlib.decompress(data)
    if c == 1: return gzip.decompress(data)
    if c == 3: return data
    if c == 4:
        try:
            import lz4.frame
        except ImportError:
            raise RuntimeError('region-file-compression=lz4: нужен модуль lz4; задайте deflate в server.properties')
        return lz4.frame.decompress(data)
    raise ValueError(f'compression {comp}')


class RegionFile:
    def __init__(self, path):
        self.path = path
        self.f = open(path, 'rb')
        self.hdr = np.frombuffer(self.f.read(4096), '>u4')

    def close(self):
        self.f.close()

    def has(self, cx, cz):
        return int(self.hdr[(cx & 31) + (cz & 31) * 32]) != 0

    def raw(self, cx, cz):
        e = int(self.hdr[(cx & 31) + (cz & 31) * 32])
        if e == 0:
            return None
        off, n = (e >> 8) * 4096, e & 255
        self.f.seek(off)
        ln = struct.unpack('>i', self.f.read(4))[0]
        comp = self.f.read(1)[0]
        if comp & 128:  # данные во внешнем файле c.X.Z.mcc
            mcc = os.path.join(os.path.dirname(self.path), f'c.{cx}.{cz}.mcc')
            data = open(mcc, 'rb').read()
        else:
            data = self.f.read(ln - 1)
        return _decompress(comp, data)


_STATUS_RE = re.compile(rb'\x08\x00\x06[Ss]tatus\x00(.)')   # 26.4: ключ `status` строчными


def chunk_status(path, cx, cz):
    """Быстро: только Status чанка (без разбора NBT); None если чанка нет / файл недочитан."""
    try:
        rf = RegionFile(path)
        try:
            raw = rf.raw(cx, cz)
        finally:
            rf.close()
    except (OSError, zlib.error, struct.error, EOFError, ValueError):
        return None
    if raw is None:
        return None
    m = _STATUS_RE.search(raw)
    return raw[m.end():m.end() + m.group(1)[0]].decode() if m else None


# ----------------------------------------------------------------------------------------------------------------------------
#  Разбор чанка
# ----------------------------------------------------------------------------------------------------------------------------
def unpack_bits(longs, bits, count):
    per = 64 // bits
    arr = np.asarray(longs).astype('>i8').view('>u8').astype(np.uint64)
    out = np.empty(len(arr) * per, dtype=np.uint16)
    mask = np.uint64((1 << bits) - 1)
    for k in range(per):
        out[k::per] = ((arr >> np.uint64(k * bits)) & mask).astype(np.uint16)
    return out[:count]


class Chunk:
    __slots__ = ('cx', 'cz', 'status', 'min_y', 'blocks', 'biomes', 'heightmaps', 'nbt_keys')


def dim_extent(version, dim):
    """(min_y, height) измерения из dimension_type пак-каталога (в чанке есть и «световые» секции вне диапазона — они не блоки)."""
    dim = DIMS.get(dim, dim)
    d = json.load(open(f'{pack_dir(version)}/data/minecraft/dimension_type/{dim}.json'))
    return int(d['min_y']), int(d['height'])


def parse_chunk(nbt, states, biomes_tab, extent=None):
    c = Chunk()
    c.cx, c.cz = nbt.get('xPos'), nbt.get('zPos')
    c.status = nbt.get('Status') or nbt.get('status')
    secs = [s for s in (nbt.get('sections') or []) if 'block_states' in s or 'biomes' in s]   # без «световых» секций (SkyLight/BlockLight)
    c.nbt_keys = list(nbt.keys())
    if extent:
        c.min_y, h = extent
        y0 = c.min_y // 16
    else:
        ys = [s['Y'] for s in secs]
        y0, y1 = min(ys), max(ys)
        c.min_y = y0 * 16
        h = (y1 - y0 + 1) * 16
    blocks = np.full((h, 16, 16), states.air, dtype=np.uint16)
    biomes = np.zeros((h // 4, 4, 4), dtype=np.uint8)
    for s in secs:
        i = s['Y'] - y0
        if not 0 <= i < h // 16:
            continue
        bs = s.get('block_states')
        if bs:
            pal = np.array([states.from_palette(e) for e in bs['palette']], dtype=np.uint16)
            if len(pal) == 1 or 'data' not in bs:
                blocks[i * 16:(i + 1) * 16] = pal[0]
            else:
                bits = max(4, (len(pal) - 1).bit_length())
                blocks[i * 16:(i + 1) * 16] = pal[unpack_bits(bs['data'], bits, 4096)].reshape(16, 16, 16)
        bi = s.get('biomes')
        if bi:
            pal = np.array([biomes_tab.get(e if isinstance(e, str) else e.get('Name', '')) for e in bi['palette']], dtype=np.uint8)
            if len(pal) == 1 or 'data' not in bi:
                biomes[i * 4:(i + 1) * 4] = pal[0]
            else:
                bits = max(1, (len(pal) - 1).bit_length())
                biomes[i * 4:(i + 1) * 4] = pal[unpack_bits(bi['data'], bits, 64)].reshape(4, 4, 4)
    c.blocks, c.biomes = blocks, biomes
    hm = np.zeros((4, 256), dtype=np.int16)
    hbits = max(1, h.bit_length())  # ceil(log2(h+1))
    for k, key in enumerate(HM_KEYS):
        v = (nbt.get('Heightmaps') or {}).get(key)
        if v is not None:
            hm[k] = unpack_bits(v, hbits, 256).astype(np.int16) + c.min_y
    c.heightmaps = hm
    return c


# ----------------------------------------------------------------------------------------------------------------------------
#  Область мира с кэшем
# ----------------------------------------------------------------------------------------------------------------------------
class World:
    """Эталонный мир на диске: чтение чанков с npz-кэшем по region-файлам."""

    def __init__(self, world, dim='overworld', version='26.3', states=None, biomes=None, use_cache=True):
        self.root, self.dim = world, DIMS.get(dim, dim)
        self.version = version
        self.states = states or StateTable.load(version)
        self.biomes = biomes or BiomeTable(version)
        self.use_cache = use_cache
        self.rdir = region_dir(world, dim)
        self.extent = dim_extent(version, dim)
        self.cdir = f'{world}/anvilcache/{self.dim}'
        self._reg = collections.OrderedDict()
        self.max_regions = 6     # кэш region-ов в памяти (область 64x64 чанков = до 9 region-ов по ~200 МБ)

    def region_files(self):
        out = {}
        for p in glob.glob(f'{self.rdir}/r.*.*.mca'):
            m = re.match(r'r\.(-?\d+)\.(-?\d+)\.mca$', os.path.basename(p))
            out[(int(m[1]), int(m[2]))] = p
        return out

    def _cache_path(self, rx, rz):
        return f'{self.cdir}/r.{rx}.{rz}.npz'

    def _key(self, path):
        st = os.stat(path)
        return f'{st.st_size}:{int(st.st_mtime)}:{self.states.sha}'

    def load_region(self, rx, rz):
        """-> dict {(cx,cz): Chunk-подобный кортеж} для всех присутствующих в region-файле чанков (кэшируется)."""
        if (rx, rz) in self._reg:
            self._reg.move_to_end((rx, rz))
            return self._reg[(rx, rz)]
        path = f'{self.rdir}/r.{rx}.{rz}.mca'
        if not os.path.exists(path):
            self._reg[(rx, rz)] = {}
            return {}
        cp = self._cache_path(rx, rz)
        key = self._key(path)
        out = None
        if self.use_cache and os.path.exists(cp):
            try:
                z = np.load(cp, allow_pickle=False)
                if str(z['key']) == key:
                    names = json.loads(str(z['biome_names']))
                    remap = np.array([self.biomes.get(n) for n in names], dtype=np.uint8)
                    out = {}
                    index, status, min_y = z['index'], z['status'], z['min_y']
                    blocks, bio, hm = z['blocks'], z['biomes'], z['hm']   # NpzFile читает массив при каждом обращении: берём один раз
                    for i, idx in enumerate(index):
                        c = Chunk()
                        c.cx, c.cz = rx * 32 + int(idx) % 32, rz * 32 + int(idx) // 32
                        c.status = str(status[i]); c.min_y = int(min_y[i])
                        c.blocks = blocks[i]; c.biomes = remap[bio[i]]; c.heightmaps = hm[i]
                        c.nbt_keys = []
                        out[(c.cx, c.cz)] = c
            except Exception:
                out = None
        if out is None:
            out = {}
            rf = RegionFile(path)
            try:
                for idx in range(1024):
                    cx, cz = rx * 32 + idx % 32, rz * 32 + idx // 32
                    raw = rf.raw(cx, cz)
                    if raw is None:
                        continue
                    out[(cx, cz)] = parse_chunk(parse_nbt(raw), self.states, self.biomes, self.extent)
            finally:
                rf.close()
            if self.use_cache and out:
                os.makedirs(self.cdir, exist_ok=True)
                ks = list(out)
                hs = {c.blocks.shape[0] for c in out.values()}
                if len(hs) == 1:
                    np.savez_compressed(
                        cp, key=key, biome_names=json.dumps(self.biomes.names),
                        index=np.array([(cx & 31) + (cz & 31) * 32 for cx, cz in ks], dtype=np.int32),
                        status=np.array([out[k].status or '' for k in ks]), min_y=np.array([out[k].min_y for k in ks], dtype=np.int32),
                        blocks=np.stack([out[k].blocks for k in ks]), biomes=np.stack([out[k].biomes for k in ks]),
                        hm=np.stack([out[k].heightmaps for k in ks]))
        self._reg[(rx, rz)] = out
        while len(self._reg) > self.max_regions:
            self._reg.popitem(last=False)
        return out

    def chunk(self, cx, cz):
        return self.load_region(cx >> 5, cz >> 5).get((cx, cz))

    def drop_region(self, rx, rz):
        self._reg.pop((rx, rz), None)

    def statuses(self):
        """{(cx,cz): status} по всем region-файлам (быстро, без блоков, но с разбором NBT только Status)."""
        out = {}
        for (rx, rz), p in self.region_files().items():
            rf = RegionFile(p)
            try:
                for idx in range(1024):
                    if int(rf.hdr[idx]) == 0:
                        continue
                    cx, cz = rx * 32 + idx % 32, rz * 32 + idx // 32
                    raw = rf.raw(cx, cz)
                    m = _STATUS_RE.search(raw) if raw else None
                    out[(cx, cz)] = raw[m.end():m.end() + m.group(1)[0]].decode() if m else None
            finally:
                rf.close()
        return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('cmd', choices=['info', 'chunk'])
    ap.add_argument('world')
    ap.add_argument('cx', nargs='?', type=int)
    ap.add_argument('cz', nargs='?', type=int)
    ap.add_argument('--dim', default='overworld')
    ap.add_argument('--version', default='26.3')
    a = ap.parse_args()
    w = World(a.world, a.dim, a.version)
    if a.cmd == 'info':
        st = w.statuses()
        cnt = collections.Counter(st.values())
        print(f'{a.world} [{w.dim}]: {len(st)} чанков; статусы: {dict(cnt)}')
        if st:
            xs = [k[0] for k in st]; zs = [k[1] for k in st]
            print(f'  cx {min(xs)}..{max(xs)}  cz {min(zs)}..{max(zs)}')
    else:
        c = w.chunk(a.cx, a.cz)
        cnt = collections.Counter(c.blocks.ravel().tolist())
        print(c.status, c.min_y, c.blocks.shape)
        for i, n in cnt.most_common(15):
            print(f'{n:8d} {w.states.names[i]}')


if __name__ == '__main__':
    main()
