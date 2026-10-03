#!/usr/bin/env python3
"""Сравнение дампа libmcgen (.mcr) с эталонным миром ванильного сервера блок-в-блок.

  diff.py --ref run/gt/26.3/raw/overworld-s12345-c0_0-r10 --mcr ours.mcr [--dim overworld] [--version 26.3]
  diff.py --ref <мир A> --vs-world <мир B> --cx0 -10 --cz0 -10 --nx 21 --nz 21     # эталон против эталона (детерминизм, варианты)

--ref принимает каталог запуска gen_world (с world/ и manifest.json) или сам каталог мира.
Сравниваются только чанки со статусом >= --min-status в эталоне (по умолчанию full). Сопоставление состояний — по ИМЕНАМ
(свойства упорядочиваются), поэтому порядок id у нас и в эталоне не обязан совпадать.

Маски (позиции не участвуют ни в числителе, ни в знаменателе):
  --ignore-state ИМЯ   (повтор.) состояние/блок/шаблон fnmatch: `minecraft:water`, `*_ore`, `minecraft:oak_log[axis=y]`;
                       позиция маскируется, если указанное состояние стоит в эталоне ИЛИ у нас
  --ignore-y A:B       (повтор.) исключить диапазон абсолютных y (включительно)
  --only-y A:B         сравнивать только этот диапазон y
  --mask-ext           исключить столбцы у биомов eroded_badlands / frozen_ocean / deep_frozen_ocean (расширения «столбы бесплодных земель»
                       и «айсберги» выполняются кодом игры при ЛЮБОМ правиле материала, поэтому в варианте raw они «грязь» для G2)
  --margin N           не сравнивать N крайних чанков области дампа
  --stable-with МИР    (повтор.) маскировать клетки, где эталон расходится с этим повторным прогоном: порядок шагов features у ванили зависит от
                       планировщика (измерено: ~0.1-0.25 % блоков у features/full), поэтому 100 % по декорациям недостижимы и у самой ванили
  --no-mask-flow       не маскировать «текущие» жидкости (water/lava[level=1..15]) и их гало: при переходе чанка в full игра один раз растекает жидкость
                       из позиций, помеченных aquifer (PostProcessing), даже при /tick freeze — это не стадия генерации (в raw ~26 блоков на 43 млн)
                       и превращает соседние пустые клетки в источники (правило «бесконечной воды»)
                       Кроме самих клеток маскируются цепочки (26-соседство) расхождений «жидкость <-> не жидкость», связанные с текущей жидкостью
                       эталона: так игра каскадно превращает клетки в источники (правило «бесконечной воды»); число — blocks_flow_induced
  --flow-halo N        дополнительно маскировать куб радиуса N (Чебышёв) вокруг текущей жидкости (по умолчанию 0)
Отчёты: --top N пар «наше -> эталон»; ASCII-карта по чанкам; гистограмма по высоте (полосы по 16); --biomes и --heightmaps — те же метрики для
биомов клеток и карт высот; --json/--md/--png — файлы. Коды возврата: 0 — совпадение >= --min-match (по умолчанию 100), 1 — ниже порога,
2 — структурная ошибка (нет чанков, разные высоты/min_y, неизвестные состояния, нечитаемый файл).
"""
import argparse, collections, fnmatch, json, os, sys, time
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import anvil
from anvil import parse_state_name
from mcr import Mcr, HM_NAMES

EXT_BIOMES = ['minecraft:eroded_badlands', 'minecraft:frozen_ocean', 'minecraft:deep_frozen_ocean']
STATUS_ORDER = ['minecraft:empty', 'minecraft:structure_starts', 'minecraft:structure_references', 'minecraft:biomes', 'minecraft:noise_biomes', 'minecraft:noise',
                'minecraft:surface', 'minecraft:carvers', 'minecraft:terrain', 'minecraft:liquid_carvers', 'minecraft:features',
                'minecraft:initialize_light', 'minecraft:light', 'minecraft:spawn', 'minecraft:full']


def status_rank(s):
    s = s if s and ':' in s else 'minecraft:' + (s or 'empty')
    return STATUS_ORDER.index(s) if s in STATUS_ORDER else -1


def find_world(path):
    return f'{path}/world' if os.path.isdir(f'{path}/world') else path


def parse_range(s):
    a, b = s.split(':')
    return int(a), int(b)


class Opts:
    def __init__(self, **kw):
        self.dim = 'overworld'; self.version = '26.3'; self.min_status = 'minecraft:full'
        self.ignore_state = []; self.ignore_y = []; self.only_y = None; self.mask_ext = False; self.margin = 0
        self.top = 20; self.biomes = False; self.heightmaps = False; self.min_match = 100.0; self.allow_missing = False
        self.region = None; self.no_cache = False; self.mask_flow = True; self.flow_halo = 0; self.list = 0; self.stable_with = []
        self.__dict__.update(kw)


class OursMcr:
    """Наша сторона: дамп MCR1 с отображением id -> id эталона по именам."""

    def __init__(self, path, ref):
        self.m = Mcr(path)
        m = self.m
        self.cx0, self.cz0, self.nx, self.nz = m.cx0, m.cz0, m.nx, m.nz
        self.min_y, self.height = m.min_y, m.height
        self.lut = np.full(65536, 65535, dtype=np.uint16)
        self.unknown = {}
        for i, n in enumerate(m.state_names):
            k = parse_state_name(n)
            r = ref.states.by_key.get(k)
            if r is None:
                if n:
                    self.unknown[i] = n
            else:
                self.lut[i] = r
        self.names = m.state_names
        self.blut = np.array([ref.biomes.get(n) for n in m.biome_names] + [0] * (256 - len(m.biome_names)), dtype=np.uint8)
        self.biome_names = m.biome_names

    def chunks(self):
        return self.m.chunks()

    def get(self, cx, cz):
        return self.m.blocks(cx, cz), self.m.biomes(cx, cz), self.m.heightmaps(cx, cz), True


class OursWorld:
    """Вторая сторона = другой эталонный мир (детерминизм): та же таблица состояний, имена биомов — через общий BiomeTable."""

    def __init__(self, world, box, ref):
        self.w = world
        self.cx0, self.cz0, self.nx, self.nz = box
        c = world.chunk(self.cx0, self.cz0)
        self.min_y, self.height = c.min_y, c.blocks.shape[0]
        self.lut = np.arange(65536, dtype=np.uint16)
        self.blut = np.arange(256, dtype=np.uint8)
        self.unknown = {}
        self.names = [ref.states.names.get(i, '') for i in range(ref.states.count)]
        self.biome_names = ref.biomes.names

    def chunks(self):
        return [(self.cx0 + i, self.cz0 + j) for j in range(self.nz) for i in range(self.nx)]

    def get(self, cx, cz):
        c = self.w.chunk(cx, cz)
        return (c.blocks, c.biomes, c.heightmaps, True) if c else (None, None, None, False)


import re as _re
_FLOW_RE = _re.compile(r'^minecraft:(water|lava)\[level=([1-9]|1[0-5])\]$')


def _flow_lut(names_by_id, size=65536):
    """булев LUT id -> «текущая» жидкость (level != 0): результат растекания при переходе чанка в full, не чистой генерации"""
    lut = np.zeros(size, dtype=bool)
    for i, n in names_by_id.items():
        if _FLOW_RE.match(n):
            lut[i] = True
    return lut


_FLUID_RE = _re.compile(r'^minecraft:(water|lava)\[level=\d+\]$')


def _fluid_lut(names_by_id, size=65536):
    lut = np.zeros(size, dtype=bool)
    for i, n in names_by_id.items():
        if _FLUID_RE.match(n):
            lut[i] = True
    return lut


def _flow_chains(flow_pos, cand):
    """Клетки-кандидаты (расхождение, где с одной из сторон жидкость), связанные цепочкой (26-соседство) с текущей жидкостью эталона: каскад
    «бесконечной воды» при PostProcessing (источник -> новый источник рядом -> ...). Возвращает множество таких координат."""
    hit, frontier = set(), []
    nb = [(dx, dy, dz) for dx in (-1, 0, 1) for dy in (-1, 0, 1) for dz in (-1, 0, 1) if (dx, dy, dz) != (0, 0, 0)]
    for p in cand:
        if any((p[0] + d[0], p[1] + d[1], p[2] + d[2]) in flow_pos for d in nb):
            hit.add(p); frontier.append(p)
    while frontier:
        q = frontier.pop()
        for d in nb:
            r = (q[0] + d[0], q[1] + d[1], q[2] + d[2])
            if r in cand and r not in hit:
                hit.add(r); frontier.append(r)
    return hit


def _flow_halo(ref, cx, cz, flow_lut, H, cache):
    """bool[h,16,16]: клетки в кубе радиуса H (Чебышёв) вокруг текущей жидкости эталона в 3x3 чанках (растекание при PostProcessing перешагивает границы)."""
    from scipy.ndimage import maximum_filter
    c0 = ref.chunk(cx, cz)
    h = c0.blocks.shape[0]
    big = np.zeros((h, 48, 48), dtype=np.uint8)
    anyf = False
    for dz in (-1, 0, 1):
        for dx in (-1, 0, 1):
            k = (cx + dx, cz + dz)
            if k not in cache:
                n = ref.chunk(*k)
                cache[k] = np.nonzero(flow_lut[n.blocks]) if n is not None and n.blocks.shape[0] == h else None
                if len(cache) > 4096:
                    cache.pop(next(iter(cache)))
            f = cache[k]
            if f is not None and len(f[0]):
                big[f[0], f[1] + (dz + 1) * 16, f[2] + (dx + 1) * 16] = 1
                anyf = True
    if not anyf:
        return np.zeros((h, 16, 16), dtype=bool)
    return maximum_filter(big, size=2 * H + 1, mode='constant')[:, 16:32, 16:32].astype(bool)


def _state_mask_lut(patterns, names_by_id, size=65536):
    """булев LUT id -> попадает под --ignore-state"""
    lut = np.zeros(size, dtype=bool)
    if not patterns:
        return lut
    for i, n in names_by_id.items():
        base = n.split('[', 1)[0]
        for p in patterns:
            if fnmatch.fnmatchcase(n, p) or fnmatch.fnmatchcase(base, p):
                lut[i] = True
                break
    return lut


def _ext_columns(ref, cx, cz, c, ext_ids):
    """bool[16,16] — столбцы, на которые влияют расширения ext-биомов (3x3 клетки вокруг по xz, qy±1 у поверхности)."""
    h4 = c.biomes.shape[0]
    # биомы 3x3 чанка в общую сетку по xz: (12 x 12 клеток) на каждый qy
    big = np.full((h4, 12, 12), 255, dtype=np.uint8)
    for dz in (-1, 0, 1):
        for dx in (-1, 0, 1):
            n = ref.chunk(cx + dx, cz + dz)
            if n is not None and n.biomes.shape[0] == h4:
                big[:, (dz + 1) * 4:(dz + 2) * 4, (dx + 1) * 4:(dx + 2) * 4] = n.biomes
    isx = np.isin(big, list(ext_ids))
    if not isx.any():
        return np.zeros((16, 16), dtype=bool)
    ws = c.heightmaps[0].reshape(16, 16)
    qy = np.clip((ws - c.min_y) // 4, 0, h4 - 1)                       # клетка поверхности для столбца
    out = np.zeros((16, 16), dtype=bool)
    for z in range(16):
        for x in range(16):
            qz, qx = z // 4 + 4, x // 4 + 4
            y0, y1 = max(qy[z, x] - 1, 0), min(qy[z, x] + 2, h4)
            out[z, x] = isx[y0:y1, qz - 1:qz + 2, qx - 1:qx + 2].any()
    return out


def compare(ref, ours, o):
    """-> dict с результатами; ref: anvil.World; ours: OursMcr | OursWorld"""
    t0 = time.monotonic()
    R = {'ref': ref.root, 'dim': ref.dim, 'version': o.version, 'errors': [], 'warnings': []}
    names_ref = ref.states.names
    ign_ref = _state_mask_lut(o.ignore_state, names_ref)
    names_ours = {i: n for i, n in enumerate(ours.names)}
    ign_ours = _state_mask_lut(o.ignore_state, names_ours)
    flow_ref = _flow_lut(names_ref) if o.mask_flow else np.zeros(65536, dtype=bool)
    flow_ours = _flow_lut(names_ours) if o.mask_flow else np.zeros(65536, dtype=bool)
    masked_flow = 0; flow_cache = {}
    samples = []
    fluid_ref = _fluid_lut(names_ref); fluid_ours = _fluid_lut(names_ours)
    flow_pos, cand = set(), {}; cand_overflow = False
    full_rank = status_rank('minecraft:full')
    pure_set = set()
    stab = [anvil.World(find_world(sp), o.dim, o.version, states=ref.states, biomes=ref.biomes, use_cache=not o.no_cache) for sp in o.stable_with]
    unstable_cells = 0
    cls = {'pure': [0, 0, 0], 'full': [0, 0, 0]}        # [чанков, блоков сравнено, расхождений]; pure = статус ниже full (PostProcessing не выполнялся)
    ext_ids = {ref.biomes.get(n) for n in EXT_BIOMES} if o.mask_ext else set()
    minst = status_rank(o.min_status)
    chunks = ours.chunks()
    if o.region:
        rx0, rz0, rnx, rnz = o.region
        chunks = [(x, z) for x, z in chunks if rx0 <= x < rx0 + rnx and rz0 <= z < rz0 + rnz]
    if o.margin:
        chunks = [(x, z) for x, z in chunks if ours.cx0 + o.margin <= x < ours.cx0 + ours.nx - o.margin
                  and ours.cz0 + o.margin <= z < ours.cz0 + ours.nz - o.margin]
    chunks = sorted(chunks, key=lambda c: (c[1] >> 5, c[0] >> 5, c[1], c[0]))   # по region-ам: кэш anvil.World держит несколько region-ов
    H = ours.height
    pair = collections.Counter()
    ychist = np.zeros(H, dtype=np.int64); yden = np.zeros(H, dtype=np.int64)
    cmap = {}            # (cx,cz) -> (mismatch, compared) | 'missing' | 'status' | 'masked'
    tot_cmp = tot_bad = masked_cells = ext_cols = unmapped_bad = 0
    bio_cmp = bio_bad = 0; bpair = collections.Counter(); bsamples = []; bio_skip = False
    hm_bad = np.zeros(4, dtype=np.int64); hm_cmp = 0
    ymask = np.zeros(H, dtype=bool)
    ys = np.arange(H) + ours.min_y
    for a, b in o.ignore_y:
        ymask |= (ys >= a) & (ys <= b)
    if o.only_y:
        ymask |= ~((ys >= o.only_y[0]) & (ys <= o.only_y[1]))
    if o.dim and ours.unknown:
        R['warnings'].append(f'{len(ours.unknown)} имён состояний дампа неизвестны blocks.json эталона (пример: {list(ours.unknown.values())[:3]})')
    for (cx, cz) in chunks:
        rc = ref.chunk(cx, cz)
        if rc is None:
            cmap[(cx, cz)] = 'missing'
            if not o.allow_missing:
                R['errors'].append(f'нет чанка в эталоне: {(cx, cz)}')
            continue
        if status_rank(rc.status) < minst:
            cmap[(cx, cz)] = 'status'
            continue
        ob, obio, ohm, okc = ours.get(cx, cz)
        if not okc:
            cmap[(cx, cz)] = 'missing'
            R['errors'].append(f'нет чанка у нас: {(cx, cz)}')
            continue
        if rc.min_y != ours.min_y or rc.blocks.shape[0] != H:
            R['errors'].append(f'чанк {(cx, cz)}: min_y/высота эталона {rc.min_y}/{rc.blocks.shape[0]} != дампа {ours.min_y}/{H}')
            continue
        mine = ours.lut[ob]                                     # id эталона (65535 = неизвестное имя)
        rb = rc.blocks
        pure = status_rank(rc.status) < full_rank
        use_flow = o.mask_flow       # растекание могут давать обе стороны: в эталоне — только full-чанки, у нас (W1) — любые
        bad = mine != rb
        mask = np.zeros(rb.shape, dtype=bool)
        if o.ignore_state:
            mask |= ign_ref[rb] | ign_ours[ob]
        if use_flow:
            fm = flow_ref[rb] | flow_ours[ob]
            if fm.any() and len(flow_pos) < 3_000_000:
                for yy, zz, xx in np.argwhere(fm).tolist():
                    flow_pos.add((cx * 16 + xx, yy + ours.min_y, cz * 16 + zz))
            if o.flow_halo > 0:
                fm |= _flow_halo(ref, cx, cz, flow_ref, o.flow_halo, flow_cache)
            masked_flow += int((fm & ~mask).sum())
            mask |= fm
        if ymask.any():
            mask |= ymask[:, None, None]
        for sw in stab:       # клетки, где повторные прогоны ВАНИЛЬНОГО сервера расходятся (порядок шагов features недетерминирован), не сравниваем
            sc = sw.chunk(cx, cz)
            if sc is not None and sc.blocks.shape == rb.shape:
                um = (sc.blocks != rb) & ~mask
                unstable_cells += int(um.sum())
                mask |= um
        colmask = None
        if ext_ids:
            colmask = _ext_columns(ref, cx, cz, rc, ext_ids)
            if colmask.any():
                mask |= colmask[None, :, :]
                ext_cols += int(colmask.sum())
        if mask.any():
            masked_cells += int(mask.sum())
            bad &= ~mask
        if use_flow and bad.any():
            cm_ = bad & (fluid_ref[rb] | fluid_ours[ob])
            if cm_.any():
                pts = np.argwhere(cm_)
                if len(cand) + len(pts) > 400_000:
                    cand_overflow = True
                else:
                    for yy, zz, xx in pts.tolist():
                        cand[(cx * 16 + xx, yy + ours.min_y, cz * 16 + zz)] = (int(ob[yy, zz, xx]), int(rb[yy, zz, xx]))
        nbad = int(bad.sum())
        ncmp = rb.size - int(mask.sum())
        tot_cmp += ncmp; tot_bad += nbad
        kcls = 'pure' if pure else 'full'
        if pure:
            pure_set.add((cx, cz))
        cls[kcls][0] += 1; cls[kcls][1] += ncmp; cls[kcls][2] += nbad
        cmap[(cx, cz)] = (nbad, ncmp) if ncmp else 'masked'
        ychist += bad.sum(axis=(1, 2))
        yden += (~mask).sum(axis=(1, 2))
        if nbad and len(samples) < o.list:
            ys_, zs_, xs_ = np.nonzero(bad)
            for k in range(min(len(ys_), o.list - len(samples))):
                a_, b_ = int(ob[ys_[k], zs_[k], xs_[k]]), int(rb[ys_[k], zs_[k], xs_[k]])
                samples.append({'x': cx * 16 + int(xs_[k]), 'y': int(ys_[k]) + ours.min_y, 'z': cz * 16 + int(zs_[k]),
                                'ours': ours.names[a_] if a_ < len(ours.names) else f'#{a_}', 'ref': names_ref.get(b_, f'#{b_}')})
        if nbad:
            ob_bad, rb_bad = ob[bad].astype(np.int64), rb[bad].astype(np.int64)
            key = ob_bad * 65536 + rb_bad
            u, n = np.unique(key, return_counts=True)
            for k, cnt in zip(u.tolist(), n.tolist()):
                pair[(k >> 16, k & 65535)] += cnt
            unmapped_bad += int((mine[bad] == 65535).sum())
        if o.biomes and rc.biomes_block is not None:
            bio_skip = True      # 26.4: биомы эталона поблочные, дамп libmcgen — по клеткам 4x4x4: сравнение по клеткам некорректно (см. anvil.py)
        elif o.biomes:
            mb = ours.blut[obio]
            bm = np.zeros(rc.biomes.shape, dtype=bool)
            if colmask is not None:
                bm = np.repeat(np.repeat(colmask.reshape(4, 4, 4, 4).any(axis=(1, 3)), 1, 0), 1, 1)[None, :, :]
                bm = np.broadcast_to(bm, rc.biomes.shape)
            bb = (mb != rc.biomes) & ~bm
            bio_cmp += int((~bm).sum()); bio_bad += int(bb.sum())
            if bb.any() and len(bsamples) < o.list:
                qy_, qz_, qx_ = np.nonzero(bb)
                for k in range(min(len(qy_), o.list - len(bsamples))):
                    bsamples.append({'x': cx * 16 + int(qx_[k]) * 4, 'y': int(qy_[k]) * 4 + ours.min_y, 'z': cz * 16 + int(qz_[k]) * 4,
                                     'ours': ours.biome_names[int(obio[qy_[k], qz_[k], qx_[k]])], 'ref': ref.biomes.names[int(rc.biomes[qy_[k], qz_[k], qx_[k]])]})
            if bb.any():
                u, n = np.unique(mb[bb].astype(np.int32) * 256 + rc.biomes[bb], return_counts=True)
                for k, cnt in zip(u.tolist(), n.tolist()):
                    bpair[(k >> 8, k & 255)] += cnt
        if o.heightmaps:
            cm = np.ones(256, dtype=bool) if colmask is None else ~colmask.reshape(256)
            hm_cmp += int(cm.sum())
            for k in range(4):
                hm_bad[k] += int(((ohm[k] != rc.heightmaps[k]) & cm).sum())
    flow_induced = 0
    if o.mask_flow and cand and flow_pos and not cand_overflow:
        hit = _flow_chains(flow_pos, cand)
        for (x, y, z) in hit:
            oi, ri = cand[(x, y, z)]
            k = (x >> 4, z >> 4)
            nb_, nc_ = cmap[k]
            cmap[k] = (nb_ - 1, nc_ - 1) if nc_ > 1 else 'masked'
            kc = 'pure' if k in pure_set else 'full'
            cls[kc][1] -= 1; cls[kc][2] -= 1
            ychist[y - ours.min_y] -= 1; yden[y - ours.min_y] -= 1
            tot_bad -= 1; tot_cmp -= 1
            pair[(oi, ri)] -= 1
            if pair[(oi, ri)] <= 0:
                del pair[(oi, ri)]
        flow_induced = len(hit)
        if hit:
            samples = [p_ for p_ in samples if (p_['x'], p_['y'], p_['z']) not in hit]
    elif cand_overflow:
        R['warnings'].append('слишком много расхождений у жидкостей (>400k): цепочки растекания не анализировались')
    n_cmp = sum(1 for v in cmap.values() if isinstance(v, tuple))
    R.update({
        'chunks_in_dump': len(chunks), 'chunks_compared': n_cmp,
        'chunks_skipped_status': sum(1 for v in cmap.values() if v == 'status'),
        'chunks_missing': sum(1 for v in cmap.values() if v == 'missing'),
        'blocks_compared': tot_cmp, 'blocks_mismatch': tot_bad, 'blocks_masked': masked_cells, 'blocks_masked_flow': masked_flow, 'blocks_unstable': unstable_cells, 'blocks_flow_induced': flow_induced, 'ext_columns_masked': ext_cols,
        'match_pct': (100.0 * (tot_cmp - tot_bad) / tot_cmp) if tot_cmp else 0.0,
        'unmapped_state_mismatch': unmapped_bad,
        'pure': {'chunks': cls['pure'][0], 'blocks': cls['pure'][1], 'mismatch': cls['pure'][2]},
        'full': {'chunks': cls['full'][0], 'blocks': cls['full'][1], 'mismatch': cls['full'][2]},
        'min_status': o.min_status, 'min_match': o.min_match, 'seconds': round(time.monotonic() - t0, 1),
        'chunks_mismatching': sum(1 for v in cmap.values() if isinstance(v, tuple) and v[0]),
    })
    R['top_pairs'] = [{'ours': ours.names[a] if a < len(ours.names) else f'#{a}', 'ref': names_ref.get(b, f'#{b}'), 'count': c}
                      for (a, b), c in pair.most_common(o.top)]
    R['pairs_total'] = len(pair)
    R['samples'] = samples
    if o.biomes and bio_skip:
        R['biomes'] = {'cells_compared': 0, 'cells_mismatch': 0, 'samples': [], 'match_pct': float('nan'), 'top_pairs': [],
                       'skipped': 'эталон хранит биомы поблочно (26.4); сверка биомов — через mcgen_biome_at по блокам'}
    elif o.biomes:
        R['biomes'] = {'cells_compared': bio_cmp, 'cells_mismatch': bio_bad, 'samples': bsamples,
                       'match_pct': (100.0 * (bio_cmp - bio_bad) / bio_cmp) if bio_cmp else 0.0,
                       'top_pairs': [{'ours': ours.biome_names[a] if a < len(ours.biome_names) else f'#{a}',
                                      'ref': ref.biomes.names[b], 'count': c} for (a, b), c in bpair.most_common(o.top)]}
    if o.heightmaps:
        R['heightmaps'] = {HM_NAMES[k]: {'columns': hm_cmp, 'mismatch': int(hm_bad[k])} for k in range(4)}
    # полосы по y (16)
    bands = []
    for b0 in range(0, H, 16):
        bad = int(ychist[b0:b0 + 16].sum()); den = int(yden[b0:b0 + 16].sum())
        bands.append({'y0': ours.min_y + b0, 'y1': ours.min_y + b0 + 15, 'mismatch': bad, 'compared': den})
    R['y_bands'] = bands
    R['_cmap'] = {f'{k[0]},{k[1]}': (list(v) if isinstance(v, tuple) else v) for k, v in cmap.items()}
    R['_box'] = [ours.cx0, ours.cz0, ours.nx, ours.nz]
    if R['errors']:
        R['verdict'] = 'error'; R['exit'] = 2
    elif R['match_pct'] + 1e-12 >= o.min_match and (tot_cmp > 0):
        R['verdict'] = 'pass'; R['exit'] = 0
    elif tot_cmp == 0:
        R['verdict'] = 'error'; R['exit'] = 2; R['errors'].append('нечего сравнивать (0 блоков)')
    else:
        R['verdict'] = 'fail'; R['exit'] = 1
    return R


def ascii_map(R, maxw=100):
    cx0, cz0, nx, nz = R['_box']
    cm = R['_cmap']

    def ch(v):
        if v is None: return ' '
        if v == 'missing': return 'X'
        if v == 'status': return '-'
        if v == 'masked': return '~'
        bad = v[0]
        if bad == 0: return '.'
        return 'abcde'[min(len(str(bad)) - 1, 4)]
    step = max(1, (nx + maxw - 1) // maxw)
    lines = [f'карта чанков (cx {cx0}..{cx0 + nx - 1}, cz {cz0}..{cz0 + nz - 1}; шаг {step}):  . = 0 расхождений, a=1-9, b=10-99, c=100-999, d=1000-9999, e>=10000, '
             f'X = нет чанка, - = статус ниже порога, ~ = всё замаскировано']
    for z in range(cz0, cz0 + nz, step):
        row = ''
        for x in range(cx0, cx0 + nx, step):
            vs = [cm.get(f'{x + i},{z + j}') for j in range(step) for i in range(step)]
            vs = [v for v in vs if v is not None]
            tv = [v for v in vs if isinstance(v, list)]
            if tv:
                row += ch([sum(v[0] for v in tv), sum(v[1] for v in tv)])
            else:
                row += ch(vs[0] if vs else None)
        lines.append(f'{z:5d} {row}')
    return '\n'.join(lines)


def text_report(R, o):
    L = []
    L.append(f'эталон: {R["ref"]} [{R["dim"]}] {R["version"]}   статус >= {R["min_status"]}')
    L.append(f'чанков в дампе {R["chunks_in_dump"]}, сравнено {R["chunks_compared"]}, пропущено по статусу {R["chunks_skipped_status"]}, нет {R["chunks_missing"]}')
    L.append(f'блоков сравнено {R["blocks_compared"]:,}, расхождений {R["blocks_mismatch"]:,}  ->  совпадение {R["match_pct"]:.6f} %   '
             f'(замаскировано {R["blocks_masked"]:,}, из них текущих жидкостей {R["blocks_masked_flow"]:,}; ext-столбцов {R["ext_columns_masked"]})   чанков с расхождениями: {R["chunks_mismatching"]}')
    if R['blocks_unstable']:
        L.append(f'  не сравнивались (расходятся повторные прогоны ванильного сервера, --stable-with): {R["blocks_unstable"]:,}')
    if R['pure']['chunks']:
        L.append(f'  чистые чанки (статус ниже full, без PostProcessing): {R["pure"]["chunks"]} чанков, {R["pure"]["blocks"]:,} блоков, расхождений {R["pure"]["mismatch"]:,}; '
                 f'full: {R["full"]["chunks"]} чанков, {R["full"]["blocks"]:,} блоков, расхождений {R["full"]["mismatch"]:,}')
    if R['blocks_flow_induced']:
        L.append(f'  исключено как след растекания (цепочки расхождений жидкость<->не жидкость, связанные с текущей жидкостью эталона): {R["blocks_flow_induced"]:,}')
    if R['unmapped_state_mismatch']:
        L.append(f'  из них из-за неизвестных имён состояний: {R["unmapped_state_mismatch"]:,}')
    for w in R['warnings']:
        L.append('ВНИМАНИЕ: ' + w)
    if R['top_pairs']:
        L.append(f'\nтоп пар «наше -> эталон» (всего различных пар {R["pairs_total"]}):')
        for p in R['top_pairs']:
            L.append(f'  {p["count"]:12,d}  {p["ours"]}  ->  {p["ref"]}')
    if R.get('samples'):
        L.append(f'\nпервые расхождения (блок x y z: наше -> эталон):')
        for p in R['samples']:
            L.append(f'  ({p["x"]}, {p["y"]}, {p["z"]})  {p["ours"]} -> {p["ref"]}')
    L.append('\n' + ascii_map(R))
    bad_bands = [b for b in R['y_bands'] if b['mismatch']]
    if bad_bands:
        L.append('\nрасхождения по высоте (полосы по 16):')
        mx = max(b['mismatch'] for b in bad_bands)
        for b in R['y_bands']:
            if b['compared']:
                bar = '#' * int(40 * b['mismatch'] / mx) if b['mismatch'] else ''
                L.append(f'  y {b["y0"]:5d}..{b["y1"]:5d}  {b["mismatch"]:11,d} / {b["compared"]:12,d}  ({100 * b["mismatch"] / b["compared"]:7.4f} %)  {bar}')
    if 'biomes' in R:
        B = R['biomes']
        if B.get('skipped'):
            L.append('\nбиомы: ' + B['skipped'])
            B = None
    if 'biomes' in R and B is not None:
        L.append(f'\nбиомы (клетки 4x4x4): сравнено {B["cells_compared"]:,}, расхождений {B["cells_mismatch"]:,} -> {B["match_pct"]:.4f} %')
        for p in B['top_pairs'][:10]:
            L.append(f'  {p["count"]:10,d}  {p["ours"]} -> {p["ref"]}')
        for p in B.get('samples', []):
            L.append(f'  клетка ({p["x"]}, {p["y"]}, {p["z"]}): {p["ours"]} -> {p["ref"]}')
    if 'heightmaps' in R:
        L.append('\nкарты высот: ' + ', '.join(f'{k}: {v["mismatch"]}/{v["columns"]}' for k, v in R['heightmaps'].items()))
    L.append(f'\nИТОГ: {R["verdict"].upper()} (порог {R["min_match"]} %)' + ('  ошибки: ' + '; '.join(R['errors'][:5]) if R['errors'] else ''))
    return '\n'.join(L)


def png_report(R, path):
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    cx0, cz0, nx, nz = R['_box']
    grid = np.full((nz, nx), np.nan)
    for k, v in R['_cmap'].items():
        x, z = map(int, k.split(','))
        if isinstance(v, list):
            grid[z - cz0, x - cx0] = v[0] / max(v[1], 1) * 100
    fig, ax = plt.subplots(1, 2, figsize=(14, 6), gridspec_kw={'width_ratios': [1, 1]})
    im = ax[0].imshow(grid, origin='upper', extent=[cx0, cx0 + nx, cz0 + nz, cz0], cmap='magma_r', vmin=0, vmax=max(0.01, np.nanmax(grid) if np.isfinite(grid).any() else 1))
    ax[0].set_title(f'расхождения по чанкам, % блоков   (совпадение {R["match_pct"]:.4f} %)'); ax[0].set_xlabel('cx'); ax[0].set_ylabel('cz')
    fig.colorbar(im, ax=ax[0])
    ys = [b['y0'] + 8 for b in R['y_bands']]
    ax[1].barh(ys, [b['mismatch'] for b in R['y_bands']], height=14)
    ax[1].set_title('расхождения по высоте (полосы 16)'); ax[1].set_xlabel('блоков'); ax[1].set_ylabel('y')
    if max(b['mismatch'] for b in R['y_bands']) > 0:
        ax[1].set_xscale('symlog')
    fig.tight_layout(); fig.savefig(path, dpi=90); plt.close(fig)


def run(ref_path, mcr_path=None, vs_world=None, box=None, **kw):
    """API для run_gate.py: -> dict результата (ключи с «_» — служебные)"""
    o = Opts(**kw)
    ref = anvil.World(find_world(ref_path), o.dim, o.version, use_cache=not o.no_cache)
    if mcr_path:
        ours = OursMcr(mcr_path, ref)
    else:
        w2 = anvil.World(find_world(vs_world), o.dim, o.version, states=ref.states, biomes=ref.biomes, use_cache=not o.no_cache)
        ours = OursWorld(w2, box, ref)
    R = compare(ref, ours, o)
    return R, o


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--ref', required=True)
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument('--mcr')
    g.add_argument('--vs-world')
    ap.add_argument('--cx0', type=int); ap.add_argument('--cz0', type=int); ap.add_argument('--nx', type=int); ap.add_argument('--nz', type=int)
    ap.add_argument('--dim', default='overworld'); ap.add_argument('--version', default='26.3')
    ap.add_argument('--min-status', default='minecraft:full')
    ap.add_argument('--ignore-state', action='append', default=[]); ap.add_argument('--ignore-y', action='append', default=[], type=parse_range)
    ap.add_argument('--only-y', type=parse_range); ap.add_argument('--mask-ext', action='store_true'); ap.add_argument('--margin', type=int, default=0)
    ap.add_argument('--top', type=int, default=20); ap.add_argument('--biomes', action='store_true'); ap.add_argument('--heightmaps', action='store_true')
    ap.add_argument('--min-match', type=float, default=100.0); ap.add_argument('--allow-missing', action='store_true')
    ap.add_argument('--region', nargs=4, type=int, metavar=('CX0', 'CZ0', 'NX', 'NZ'))
    ap.add_argument('--no-cache', action='store_true'); ap.add_argument('--no-mask-flow', action='store_true'); ap.add_argument('--flow-halo', type=int, default=0)
    ap.add_argument('--list', type=int, default=0, help='вывести координаты первых N расхождений блоков и биомов')
    ap.add_argument('--stable-with', action='append', default=[])
    ap.add_argument('--json'); ap.add_argument('--png'); ap.add_argument('--md')
    a = ap.parse_args()
    kw = {k: getattr(a, k) for k in ('dim', 'version', 'min_status', 'ignore_state', 'ignore_y', 'only_y', 'mask_ext', 'margin', 'top', 'biomes',
                                    'heightmaps', 'min_match', 'allow_missing', 'region', 'no_cache', 'list', 'flow_halo', 'stable_with')}
    kw['mask_flow'] = not a.no_mask_flow
    box = None
    if a.vs_world:
        if None in (a.cx0, a.cz0, a.nx, a.nz):
            ap.error('--vs-world требует --cx0 --cz0 --nx --nz')
        box = (a.cx0, a.cz0, a.nx, a.nz)
    try:
        R, o = run(a.ref, a.mcr, a.vs_world, box, **kw)
    except Exception as e:  # структурная ошибка (нечитаемый файл и т.п.)
        print(f'ОШИБКА: {type(e).__name__}: {e}', file=sys.stderr)
        sys.exit(2)
    txt = text_report(R, o)
    print(txt)
    if a.json:
        json.dump({k: v for k, v in R.items()}, open(a.json, 'w'), indent=1, ensure_ascii=False)
    if a.md:
        open(a.md, 'w').write('```\n' + txt + '\n```\n')
    if a.png:
        png_report(R, a.png)
    sys.exit(R['exit'])


if __name__ == '__main__':
    main()
