#!/usr/bin/env python3
"""Читатель Anvil (.mca) для ВСЕХ поколений формата Java Edition (1.7.10 … 26.x): извлекает из чанка маски глины, алмазной руды и воды.

Форматы секций (определяются по содержимому и DataVersion):
  * <= 1.12.2   Level.Sections[].Blocks (byte[4096], индекс y*256+z*16+x) + Add (nibble) ; id: глина 82, алмазная руда 56, вода 8/9
  * 1.13 – 1.15 Level.Sections[].Palette + BlockStates (long[]; значения МОГУТ пересекать границу long)
  * 1.16 – 1.17 то же, но без пересечения границы long (DataVersion >= 2529)
  * >= 1.18     sections[].block_states.{palette,data}  (палитра — compound {Name} либо строка либо {"": имя})
Использование как библиотека: chunk_masks(nbt) ; как скрипт — извлечение мира в .npz:
  tools/l3_anvil_legacy.py extract <каталог_region> <выход.npz> [--chunks cx0 cz0 cx1 cz1]
"""
import os, sys, struct, zlib, glob, time
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import l3_anvil as A      # parse_nbt, read_chunk_nbt

DIAMOND_NAMES = {'minecraft:diamond_ore', 'minecraft:deepslate_diamond_ore'}
CLAY_NAMES = {'minecraft:clay'}
WATER_NAMES = {'minecraft:water', 'minecraft:kelp', 'minecraft:kelp_plant', 'minecraft:seagrass', 'minecraft:tall_seagrass',
               'minecraft:bubble_column'}
FULL_STATUS = {'full', 'postprocessed', 'fullchunk'}


def pal_name(p):
    if isinstance(p, str): return p
    if isinstance(p, dict): return p.get('Name') or p.get('') or str(p)
    return str(p)


def unpack_cross(longs, bits, count=4096):
    """упаковка 1.13–1.15: значения идут непрерывным битовым потоком, могут пересекать границу long"""
    w = longs.astype('>i8').view('>u8').astype(np.uint64)
    w = np.concatenate([w, np.zeros(1, np.uint64)])
    i = np.arange(count, dtype=np.uint64)
    start = i * np.uint64(bits)
    word = (start >> np.uint64(6)).astype(np.int64)
    off = start & np.uint64(63)
    lo = w[word] >> off
    sh_hi = np.uint64(64) - off
    hi = np.where(off + np.uint64(bits) > np.uint64(64), w[word + 1] << np.where(sh_hi >= np.uint64(64), np.uint64(0), sh_hi), np.uint64(0))
    return ((lo | hi) & np.uint64((1 << bits) - 1)).astype(np.uint16)


def _unpack(longs, bits, cross):
    return unpack_cross(longs, bits) if cross else A.unpack(longs, bits)


def _flags(pal_names):
    return (np.array([n in CLAY_NAMES for n in pal_names]),
            np.array([n in DIAMOND_NAMES for n in pal_names]),
            np.array([n in WATER_NAMES for n in pal_names]))


def chunk_status(nbt):
    """-> (full: bool, подробности). Для <=1.12 'full' = TerrainPopulated."""
    lv = nbt.get('Level', nbt)
    if 'Status' in lv:
        s = str(lv['Status']).replace('minecraft:', '')
        return (s in FULL_STATUS), s
    if 'status' in lv:
        s = str(lv['status']).replace('minecraft:', '')
        return (s in FULL_STATUS), s
    if 'TerrainPopulated' in lv:
        return bool(lv['TerrainPopulated']), 'populated' if lv['TerrainPopulated'] else 'unpopulated'
    return False, 'unknown'


def chunk_masks(nbt):
    """-> (y0, clay[y,z,x] bool, diamond bool, water bool); y0 — мировая y нижней строки массива"""
    lv = nbt.get('Level', nbt)
    secs = lv.get('Sections') if 'Sections' in lv else lv.get('sections')
    if not secs:
        return 0, None, None, None
    dv = nbt.get('DataVersion', 0)
    ys = [int(s['Y']) for s in secs]
    y0, y1 = min(ys), max(ys)
    n = (y1 - y0 + 1) * 16
    clay = np.zeros((n, 16, 16), bool); dia = np.zeros((n, 16, 16), bool); wat = np.zeros((n, 16, 16), bool)
    for s in secs:
        sy = int(s['Y']) - y0
        sl = slice(sy * 16, sy * 16 + 16)
        if 'Blocks' in s:                                  # <= 1.12
            b = np.frombuffer(s['Blocks'], np.uint8).astype(np.uint16)
            if 'Add' in s and len(s['Add']):
                add = np.frombuffer(s['Add'], np.uint8)
                a = np.empty(4096, np.uint16); a[0::2] = add & 0xF; a[1::2] = add >> 4
                b = b | (a << 8)
            clay[sl] = (b == 82).reshape(16, 16, 16)
            dia[sl] = (b == 56).reshape(16, 16, 16)
            wat[sl] = ((b == 8) | (b == 9)).reshape(16, 16, 16)
            continue
        if 'block_states' in s:                            # >= 1.18
            bs = s['block_states']; pal = bs.get('palette'); data = bs.get('data'); cross = False
        elif 'Palette' in s:                               # 1.13 – 1.17
            pal = s['Palette']; data = s.get('BlockStates'); cross = dv < 2529
        else:
            continue
        if not pal: continue
        names = [pal_name(p) for p in pal]
        fc, fd, fw = _flags(names)
        if len(names) == 1 or data is None or len(data) == 0:
            idx = np.zeros(4096, np.uint16)
        else:
            bits = max(4, (len(names) - 1).bit_length())
            idx = _unpack(data, bits, cross)
        clay[sl] = fc[idx].reshape(16, 16, 16); dia[sl] = fd[idx].reshape(16, 16, 16); wat[sl] = fw[idx].reshape(16, 16, 16)
    return y0 * 16, clay, dia, wat


# --- биомы чанка: id для <=1.17 (числовые; 1.13–1.17 те же младшие id), имена для >=1.18 ---
BIO_NAMES = []          # имя -> 1000 + индекс


def bio_code(v):
    if isinstance(v, str) or isinstance(v, dict):
        n = pal_name(v)
        if n not in BIO_NAMES: BIO_NAMES.append(n)
        return 1000 + BIO_NAMES.index(n)
    return int(v)


def chunk_biomes(nbt):
    """-> (центр, угол00): биом в клетке (x=8..11,z=8..11) на высоте ~y=8 (1.15–1.17) / y=64 (1.18+), и в колонке (0,0). Для <=1.14: колонки (8,8) и (0,0).
    Для <=1.12 биом, выбирающий список фич чанка популяции (cx,cz), лежит в углу (0,0) чанка (cx+1,cz+1)."""
    lv = nbt.get('Level', nbt)
    b = lv.get('Biomes')
    if b is not None:
        b = np.asarray(bytearray(b)) if isinstance(b, (bytes, bytearray)) else np.asarray(b)
        if len(b) == 256:
            return bio_code(b[8 * 16 + 8] & 0xFF), bio_code(b[0] & 0xFF)
        if len(b) == 1024:
            return bio_code(b[(2 << 4) | (2 << 2) | 2]), bio_code(b[(2 << 4)])
    secs = lv.get('sections') or []
    for s in secs:
        if int(s['Y']) == 4 and 'biomes' in s:
            bs = s['biomes']; pal = bs.get('palette') or []
            if not pal: break
            if len(pal) == 1 or not len(bs.get('data', [])):
                return bio_code(pal[0]), bio_code(pal[0])
            bits = max(1, (len(pal) - 1).bit_length())
            idx = A.unpack(bs['data'], bits, 64) if bits >= 4 else _unpack_small(bs['data'], bits, 64)
            return bio_code(pal[idx[(0 << 4) | (2 << 2) | 2]]), bio_code(pal[idx[0]])
    return -1, -1


def _unpack_small(longs, bits, count):
    per = 64 // bits
    arr = longs.astype('>i8').view('>u8').astype(np.uint64)
    out = np.empty(len(arr) * per, dtype=np.uint16)
    mask = np.uint64((1 << bits) - 1)
    for k in range(per):
        out[k::per] = ((arr >> np.uint64(k * bits)) & mask).astype(np.uint16)
    return out[:count]


def region_files(reg):
    out = []
    for p in glob.glob(f'{reg}/r.*.*.mca'):
        b = os.path.basename(p).split('.')
        out.append((int(b[1]), int(b[2]), p))
    return sorted(out)


def region_chunks(path):
    """список (cx_local, cz_local), присутствующих в region-файле (по заголовку)"""
    with open(path, 'rb') as f:
        h = f.read(4096)
    if len(h) < 4096: return []
    t = np.frombuffer(h, '>u4')
    return [(i & 31, i >> 5) for i in range(1024) if t[i] != 0]


def extract_world(reg, box=None, min_y_clay=30, verbose=True):
    """-> dict: clay (N,4: x,y,z,wet), diamond (M,3), chunks (K,3: cx,cz,full), status — счётчик статусов"""
    clay_l, dia_l, chunks = [], [], []
    st = {}
    t0 = time.time()
    for rx, rz, p in region_files(reg):
        for lx, lz in region_chunks(p):
            cx, cz = rx * 32 + lx, rz * 32 + lz
            if box and not (box[0] <= cx <= box[2] and box[1] <= cz <= box[3]):
                continue
            try:
                nbt = A.read_chunk_nbt(p, cx, cz)
            except Exception as e:
                st['err'] = st.get('err', 0) + 1; continue
            if nbt is None: continue
            ok, s = chunk_status(nbt)
            st[s] = st.get(s, 0) + 1
            bc, b0 = chunk_biomes(nbt)
            chunks.append((cx, cz, int(ok), bc, b0))
            if not ok: continue
            y0, clay, dia, wat = chunk_masks(nbt)
            if clay is None: continue
            ci = np.argwhere(clay)
            if len(ci):
                ci = ci[(ci[:, 0] + y0) > min_y_clay]
                if len(ci):
                    up = np.minimum(ci[:, 0] + 1, clay.shape[0] - 1)
                    wet = wat[up, ci[:, 1], ci[:, 2]]
                    g = np.stack([ci[:, 2] + cx * 16, ci[:, 0] + y0, ci[:, 1] + cz * 16, wet.astype(np.int64)], axis=1)
                    clay_l.append(g)
            di = np.argwhere(dia)
            if len(di):
                dia_l.append(np.stack([di[:, 2] + cx * 16, di[:, 0] + y0, di[:, 1] + cz * 16], axis=1))
        if verbose:
            print(f'  region r.{rx}.{rz}: всего чанков {len(chunks)}, {time.time() - t0:.0f}s', flush=True)
    return dict(clay=np.concatenate(clay_l) if clay_l else np.zeros((0, 4), np.int64),
                diamond=np.concatenate(dia_l) if dia_l else np.zeros((0, 3), np.int64),
                chunks=np.array(chunks, dtype=np.int64).reshape(-1, 5), bio_names=np.array(BIO_NAMES, dtype=object).astype(str) if BIO_NAMES else np.zeros(0, str)), st


if __name__ == '__main__':
    if len(sys.argv) >= 4 and sys.argv[1] == 'extract':
        box = None
        if '--chunks' in sys.argv:
            i = sys.argv.index('--chunks'); box = tuple(int(v) for v in sys.argv[i + 1:i + 5])
        d, st = extract_world(sys.argv[2], box)
        np.savez_compressed(sys.argv[3], **d)
        print('статусы:', st)
        print('clay', len(d['clay']), 'diamond', len(d['diamond']), 'chunks', len(d['chunks']), 'full', int(d['chunks'][:, 2].sum()))
    elif len(sys.argv) >= 3 and sys.argv[1] == 'extract-ver':
        # извлечь все готовые миры версии (есть GEN_DONE) в data/l3v/<версия>-w<seed>.npz
        ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        ver = sys.argv[2]
        os.makedirs(f'{ROOT}/data/l3v', exist_ok=True)
        for wd in sorted(glob.glob(f'{ROOT}/run/oldver/{ver}/w*/GEN_DONE')):
            w = os.path.dirname(wd); seed = os.path.basename(w)[1:]
            outp = f'{ROOT}/data/l3v/{ver}-w{seed}.npz'
            if os.path.exists(outp): continue
            reg = f'{w}/dimensions/minecraft/overworld/region' if ver.startswith('26.') else f'{w}/region'
            d, st = extract_world(reg, None, verbose=False)
            np.savez_compressed(outp, **d)
            print(ver, seed, st, 'clay', len(d['clay']), 'diamond', len(d['diamond']), flush=True)
    else:
        print(__doc__)
