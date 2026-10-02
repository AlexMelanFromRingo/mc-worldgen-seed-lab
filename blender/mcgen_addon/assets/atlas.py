"""Атлас текстур блоков: загрузка спрайтов (первый кадр анимации по .mcmeta), упаковка «полками» в собственном размере кадра
(Atlas.tile — максимальная сторона), поля вокруг спрайтов (протяжка краёв — против швов при линейной фильтрации), прозрачность по
подобласти UV (как SpriteContents.computeTransparency). Чистый Python, без bpy.
"""
import json
import math
import os

import numpy as np

from . import pngio

__all__ = ['Atlas', 'build_atlas', 'MISSING', 'TRANSPARENCY_OPAQUE', 'TRANSPARENCY_CUTOUT', 'TRANSPARENCY_TRANSLUCENT']

MISSING = 'minecraft:missingno'
TRANSPARENCY_OPAQUE, TRANSPARENCY_CUTOUT, TRANSPARENCY_TRANSLUCENT = 0, 1, 2   # = слой SOLID/CUTOUT/TRANSLUCENT


def _missing_image(s=16):
    """«missingno»: фиолетово-чёрная шашка 2×2 клетки."""
    a = np.zeros((s, s, 4), dtype=np.uint8)
    a[:, :, 3] = 255
    h = s // 2
    a[:h, :h, :3] = (248, 0, 248)
    a[h:, h:, :3] = (248, 0, 248)
    return a


class _Sprite:
    __slots__ = ('name', 'frame0', 'w', 'h', 'tr_t', 'tr_l', 'animated', 'nframes')


def _read_meta(path):
    p = path + '.mcmeta'
    if not os.path.isfile(p):
        return None
    try:
        with open(p, encoding='utf-8') as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def _frame_geometry(img_w, img_h, anim):
    """Размер кадра и порядок кадров: (fw, fh, [индексы кадров в порядке показа]) — как SpriteContents/AnimationMetadataSection."""
    fw = fh = None
    if isinstance(anim, dict):
        fw = anim.get('width')
        fh = anim.get('height')
    if fw is None and fh is None:
        fw = fh = min(img_w, img_h)
    elif fw is None:
        fw = img_w
    elif fh is None:
        fh = img_h
    fw, fh = int(fw), int(fh)
    if fw <= 0 or fh <= 0 or img_w % fw or img_h % fh:
        fw, fh = img_w, img_h
    cols = img_w // fw
    rows = img_h // fh
    n = cols * rows
    order = list(range(n))
    if isinstance(anim, dict) and 'frames' in anim:
        order = []
        for e in anim['frames']:
            idx = e.get('index', 0) if isinstance(e, dict) else e
            if 0 <= int(idx) < n:
                order.append(int(idx))
        if not order:
            order = list(range(n))
    return fw, fh, cols, order


def _load_sprite(tex_root, sid):
    """Загружает спрайт minecraft:block/xyz -> (_Sprite) или None, если файла нет."""
    ns, path = sid.split(':', 1) if ':' in sid else ('minecraft', sid)
    f = os.path.join(tex_root, path.replace('/', os.sep) + '.png')
    if not os.path.isfile(f):
        return None
    img = pngio.read_png(f)
    meta = _read_meta(f)
    anim = meta.get('animation') if isinstance(meta, dict) else None
    ih, iw = img.shape[:2]
    if anim is None:
        # без анимации игра берёт картинку целиком (как есть, даже если не квадрат)
        fw, fh, cols, order = iw, ih, 1, [0]
    else:
        fw, fh, cols, order = _frame_geometry(iw, ih, anim)
    sp = _Sprite()
    sp.name = sid
    sp.animated = anim is not None and len(set(order)) > 1
    sp.nframes = len(set(order))
    first = order[0]
    fx, fy = (first % cols) * fw, (first // cols) * fh
    sp.frame0 = img[fy:fy + fh, fx:fx + fw]
    sp.w, sp.h = fw, fh
    # объединение по всем уникальным кадрам для прозрачности
    a = None
    for k in sorted(set(order)):
        kx, ky = (k % cols) * fw, (k // cols) * fh
        al = img[ky:ky + fh, kx:kx + fw, 3]
        z = al == 0
        t = (al != 0) & (al != 255)
        a = (z, t) if a is None else (a[0] | z, a[1] | t)
    sp.tr_t = _prefix(a[0])
    sp.tr_l = _prefix(a[1])
    return sp


def _prefix(mask):
    """Интегральное изображение (h+1, w+1) для подсчёта числа True в прямоугольнике."""
    h, w = mask.shape
    p = np.zeros((h + 1, w + 1), dtype=np.int32)
    p[1:, 1:] = np.cumsum(np.cumsum(mask.astype(np.int32), axis=0), axis=1)
    return p


def _rect_count(p, x0, y0, x1, y1):
    return int(p[y1, x1] - p[y0, x1] - p[y1, x0] + p[y0, x0])


class Atlas:
    """Результат сборки. image — uint8[H, W, 4] (строка 0 — верх, v вниз). rect[i] = (u0, v0, u1, v1) спрайта i в долях атласа."""

    def __init__(self):
        self.image = None
        self.tile = 16
        self.pad = 2
        self.names = []          # имена спрайтов
        self.index = {}          # имя -> номер
        self.rect = None         # float32[n, 4]
        self.sprite_size = []    # (w, h) кадра в пикселях
        self.animated = []       # bool
        self._tr_t = []
        self._tr_l = []
        self.cols = 0
        self.rows = 0
        self.missing_names = []  # запрошенные, но не найденные спрайты

    @property
    def size(self):
        return self.image.shape[1], self.image.shape[0]

    def sprite(self, name):
        """Номер спрайта (отсутствующий -> missingno)."""
        return self.index.get(name, self.index[MISSING])

    def transparency(self, idx, u0, v0, u1, v1, force_translucent=False):
        """computeTransparency: 0 непрозрачный, 1 с полностью прозрачными пикселями (cutout), 2 с полупрозрачными (translucent).
        Прямоугольник UV — в долях спрайта; пиксели floor(min*size) .. ceil(max*size) (min/max по u и v)."""
        if force_translucent:
            return TRANSPARENCY_TRANSLUCENT
        w, h = self.sprite_size[idx]
        pt, pl = self._tr_t[idx], self._tr_l[idx]
        lo_u, hi_u = min(u0, u1), max(u0, u1)
        lo_v, hi_v = min(v0, v1), max(v0, v1)
        if lo_u <= 0.0 and lo_v <= 0.0 and hi_u >= 1.0 and hi_v >= 1.0:
            x0, y0, x1, y1 = 0, 0, w, h
        else:
            x0 = min(max(int(math.floor(lo_u * w)), 0), w - 1)
            y0 = min(max(int(math.floor(lo_v * h)), 0), h - 1)
            x1 = max(min(int(math.ceil(hi_u * w)), w), x0 + 1)
            y1 = max(min(int(math.ceil(hi_v * h)), h), y0 + 1)
        if _rect_count(pl, x0, y0, x1, y1) > 0:
            return TRANSPARENCY_TRANSLUCENT
        if _rect_count(pt, x0, y0, x1, y1) > 0:
            return TRANSPARENCY_CUTOUT
        return TRANSPARENCY_OPAQUE

    def to_dict(self):
        return {'tile': self.tile, 'pad': self.pad, 'names': self.names, 'cols': self.cols, 'rows': self.rows,
                'size': list(self.size), 'sprite_size': self.sprite_size, 'animated': self.animated}


def build_atlas(textures_dir, sprite_names, pad=2, max_width=4096, progress=None):
    """Собирает атлас из спрайтов `minecraft:block/…` (каталог `.../assets/minecraft/textures`).

    Спрайты кладутся «полками» в собственном размере (без растягивания; кадр 16×16 остаётся 16×16, а 32×32/64×64 — как есть), вокруг
    каждого поле pad пикселей, заполненное протяжкой краёв (против швов при линейной фильтрации). Atlas.tile — максимальная сторона.
    Отсутствующие спрайты заменяются «missingno» (их имена — в Atlas.missing_names)."""
    atlas = Atlas()
    sprites = []
    names = [MISSING]
    seen = {MISSING}
    for n in sprite_names:
        if n not in seen:
            seen.add(n)
            names.append(n)
    ms = _Sprite()
    ms.name = MISSING
    ms.frame0 = _missing_image(16)
    ms.w = ms.h = 16
    ms.animated = False
    ms.nframes = 1
    z = np.zeros((16, 16), dtype=bool)
    ms.tr_t = _prefix(z)
    ms.tr_l = _prefix(z)
    sprites.append(ms)
    for i, n in enumerate(names[1:]):
        sp = _load_sprite(textures_dir, n)
        if sp is None:
            atlas.missing_names.append(n)
            sp = ms
        sprites.append(sp)
        if progress and i % 200 == 0:
            progress(i / max(1, len(names)), 'текстуры')
    atlas.tile = max(max(s.w, s.h) for s in sprites)
    atlas.pad = pad
    n = len(sprites)
    # одинаковые заменители missingno делят одну ячейку: упаковываем только уникальные объекты
    uniq = []
    slot_of = {}
    for sp in sprites:
        if id(sp) not in slot_of:
            slot_of[id(sp)] = len(uniq)
            uniq.append(sp)
    order = sorted(range(len(uniq)), key=lambda k: (-uniq[k].h, -uniq[k].w, uniq[k].name))
    area = sum((uniq[k].w + 2 * pad) * (uniq[k].h + 2 * pad) for k in order)
    wmin = max(uniq[k].w for k in order) + 2 * pad
    W = int(math.sqrt(area * 1.08)) + 31 & ~31
    W = max(wmin, min(W, max_width))
    pos = {}
    x = y = rowh = 0
    for k in order:
        sp = uniq[k]
        pw, ph = sp.w + 2 * pad, sp.h + 2 * pad
        if x + pw > W:
            x, y, rowh = 0, y + rowh, 0
        pos[k] = (x, y)
        x += pw
        rowh = max(rowh, ph)
    H = y + rowh
    img = np.zeros((H, W, 4), dtype=np.uint8)
    for k in order:
        sp = uniq[k]
        px, py = pos[k]
        t = sp.frame0
        if pad:
            t = np.pad(t, ((pad, pad), (pad, pad), (0, 0)), mode='edge')
        img[py:py + t.shape[0], px:px + t.shape[1]] = t
    rect = np.zeros((n, 4), dtype=np.float32)
    for i, sp in enumerate(sprites):
        px, py = pos[slot_of[id(sp)]]
        rect[i] = ((px + pad) / W, (py + pad) / H, (px + pad + sp.w) / W, (py + pad + sp.h) / H)
        atlas.sprite_size.append((sp.w, sp.h))
        atlas.animated.append(bool(sp.animated))
        atlas._tr_t.append(sp.tr_t)
        atlas._tr_l.append(sp.tr_l)
    atlas.image = img
    atlas.rect = rect
    atlas.names = names
    atlas.index = {nm: i for i, nm in enumerate(names)}
    atlas.cols = W
    atlas.rows = H
    return atlas
