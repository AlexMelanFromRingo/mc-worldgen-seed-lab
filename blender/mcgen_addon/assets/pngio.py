"""Собственный PNG-декодер/кодировщик на zlib + numpy (без PIL; чистый Python, без bpy).

Декодер: все цветовые типы (0 серый, 2 RGB, 3 палитра, 4 серый+альфа, 6 RGBA), глубины 1/2/4/8/16, чересстрочность Adam7,
палитры, tRNS (для палитры, серого и RGB), фильтры 0–4. Результат всегда (H, W, 4) uint8 RGBA: 16 бит переводятся
старшим байтом (так делает и игра через stb), серый/палитра — в RGB, отсутствующая альфа = 255.

Кодировщик: `write_png` (RGBA8, фильтр Paeth/Up/None) — для атласа, отладочных картинок и тестов; `encode_png` — тестовый
генератор любых цветовых типов/глубин/Adam7 (в тестах декодера).
"""
import struct
import zlib

import numpy as np

SIGNATURE = b'\x89PNG\r\n\x1a\n'

CHANNELS = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}
VALID_DEPTHS = {0: (1, 2, 4, 8, 16), 2: (8, 16), 3: (1, 2, 4, 8), 4: (8, 16), 6: (8, 16)}

# Adam7: (x0, y0, dx, dy)
ADAM7 = ((0, 0, 8, 8), (4, 0, 8, 8), (0, 4, 4, 8), (2, 0, 4, 4), (0, 2, 2, 4), (1, 0, 2, 2), (0, 1, 1, 2))


class PngError(ValueError):
    pass


class PngInfo:
    """Заголовок PNG (без декодирования пикселей)."""
    __slots__ = ('width', 'height', 'bit_depth', 'color_type', 'interlace')

    def __init__(self, width, height, bit_depth, color_type, interlace):
        self.width, self.height, self.bit_depth = width, height, bit_depth
        self.color_type, self.interlace = color_type, interlace

    def __repr__(self):
        return 'PngInfo(%dx%d, depth=%d, type=%d, interlace=%d)' % (
            self.width, self.height, self.bit_depth, self.color_type, self.interlace)


def _chunks(data):
    if data[:8] != SIGNATURE:
        raise PngError('не PNG: неверная сигнатура')
    p = 8
    n = len(data)
    while p + 8 <= n:
        ln, = struct.unpack_from('>I', data, p)
        typ = bytes(data[p + 4:p + 8])
        body = data[p + 8:p + 8 + ln]
        if len(body) != ln:
            raise PngError('обрезанный чанк %r' % typ)
        yield typ, body
        p += 12 + ln
        if typ == b'IEND':
            return


def read_info(data):
    """PngInfo из байтов (первые 33 байта)."""
    for typ, body in _chunks(data):
        if typ == b'IHDR':
            w, h, bd, ct, comp, flt, il = struct.unpack('>IIBBBBB', body)
            return PngInfo(w, h, bd, ct, il)
        break
    raise PngError('нет IHDR')


def _unfilter(raw, height, stride, bpp):
    """raw: bytes из zlib, по строкам [filter byte + stride байт]. Возвращает uint8[height, stride]."""
    if len(raw) < height * (stride + 1):
        raise PngError('данные IDAT короче ожидаемых (%d < %d)' % (len(raw), height * (stride + 1)))
    arr = np.frombuffer(raw, dtype=np.uint8, count=height * (stride + 1)).reshape(height, stride + 1)
    ftypes = arr[:, 0]
    out = np.zeros((height, stride), dtype=np.uint8)
    zero = np.zeros(stride, dtype=np.uint8)
    for y in range(height):
        f = int(ftypes[y])
        line = arr[y, 1:]
        prev = out[y - 1] if y else zero
        if f == 0:
            out[y] = line
        elif f == 1:  # Sub: префиксная сумма по каждому каналу с шагом bpp
            if bpp == 1:
                out[y] = np.cumsum(line, dtype=np.uint8)
            else:
                m = (stride + bpp - 1) // bpp
                pad = np.zeros(m * bpp, dtype=np.uint8)
                pad[:stride] = line
                out[y] = np.cumsum(pad.reshape(m, bpp), axis=0, dtype=np.uint8).reshape(-1)[:stride]
        elif f == 2:  # Up
            out[y] = line + prev
        elif f == 3:  # Average
            cur = bytearray(stride)
            lb = line.tolist()
            pb = prev.tolist()
            for i in range(stride):
                a = cur[i - bpp] if i >= bpp else 0
                cur[i] = (lb[i] + ((a + pb[i]) >> 1)) & 255
            out[y] = np.frombuffer(bytes(cur), dtype=np.uint8)
        elif f == 4:  # Paeth
            cur = bytearray(stride)
            lb = line.tolist()
            pb = prev.tolist()
            for i in range(stride):
                if i >= bpp:
                    a = cur[i - bpp]
                    c = pb[i - bpp]
                else:
                    a = c = 0
                b = pb[i]
                p = a + b - c
                pa = abs(p - a)
                pb_ = abs(p - b)
                pc = abs(p - c)
                if pa <= pb_ and pa <= pc:
                    pr = a
                elif pb_ <= pc:
                    pr = b
                else:
                    pr = c
                cur[i] = (lb[i] + pr) & 255
            out[y] = np.frombuffer(bytes(cur), dtype=np.uint8)
        else:
            raise PngError('неизвестный фильтр %d в строке %d' % (f, y))
    return out


def _unpack_samples(rows, width, channels, depth):
    """uint8[h, stride] -> целые отсчёты uint16[h, width, channels] (в исходной глубине)."""
    h = rows.shape[0]
    if depth == 8:
        return rows[:, :width * channels].reshape(h, width, channels).astype(np.uint16)
    if depth == 16:
        v = rows[:, :width * channels * 2].reshape(h, width * channels, 2).astype(np.uint16)
        return ((v[:, :, 0] << 8) | v[:, :, 1]).reshape(h, width, channels)
    # 1/2/4 бита: каналов только 1; MSB-first
    per = 8 // depth
    n = rows.shape[1]
    out = np.empty((h, n * per), dtype=np.uint16)
    mask = (1 << depth) - 1
    for k in range(per):
        out[:, k::per] = (rows >> (8 - depth * (k + 1))) & mask
    return out[:, :width].reshape(h, width, 1)


def _to_rgba(samples, color_type, depth, palette, trns):
    h, w, _ = samples.shape
    out = np.empty((h, w, 4), dtype=np.uint8)
    maxv = (1 << depth) - 1

    def scale(a):
        if depth == 8:
            return a.astype(np.uint8)
        if depth == 16:
            return (a >> 8).astype(np.uint8)
        # 1/2/4 -> 8 бит растяжением
        return (a.astype(np.uint32) * 255 // maxv).astype(np.uint8)

    if color_type == 3:
        if palette is None:
            raise PngError('нет PLTE для индексированного PNG')
        pal = np.zeros((max(256, len(palette) // 3), 4), dtype=np.uint8)
        pl = np.frombuffer(palette, dtype=np.uint8)
        np_ = len(pl) // 3
        pal[:np_, :3] = pl[:np_ * 3].reshape(np_, 3)
        pal[:, 3] = 255
        if trns:
            tt = np.frombuffer(trns, dtype=np.uint8)
            pal[:len(tt), 3] = tt[:np_] if len(tt) > np_ else tt
        idx = samples[:, :, 0]
        if idx.size and int(idx.max()) >= len(pal):
            raise PngError('индекс палитры вне диапазона')
        return pal[idx]
    if color_type == 0:
        g = scale(samples[:, :, 0])
        out[:, :, 0] = out[:, :, 1] = out[:, :, 2] = g
        out[:, :, 3] = 255
        if trns and len(trns) >= 2:
            tv, = struct.unpack('>H', trns[:2])
            out[:, :, 3] = np.where(samples[:, :, 0] == tv, 0, 255).astype(np.uint8)
        return out
    if color_type == 2:
        out[:, :, :3] = scale(samples)
        out[:, :, 3] = 255
        if trns and len(trns) >= 6:
            tr, tg, tb = struct.unpack('>HHH', trns[:6])
            hit = (samples[:, :, 0] == tr) & (samples[:, :, 1] == tg) & (samples[:, :, 2] == tb)
            out[:, :, 3] = np.where(hit, 0, 255).astype(np.uint8)
        return out
    if color_type == 4:
        g = scale(samples[:, :, 0])
        out[:, :, 0] = out[:, :, 1] = out[:, :, 2] = g
        out[:, :, 3] = scale(samples[:, :, 1])
        return out
    if color_type == 6:
        out[:] = scale(samples)
        return out
    raise PngError('цветовой тип %d' % color_type)


def decode_png(data, check_crc=False):
    """bytes -> (rgba uint8[H, W, 4], PngInfo)."""
    data = bytes(data)
    ihdr = None
    palette = None
    trns = None
    idat = []
    for typ, body in _chunks(data):
        if typ == b'IHDR':
            ihdr = struct.unpack('>IIBBBBB', body)
        elif typ == b'PLTE':
            palette = bytes(body)
        elif typ == b'tRNS':
            trns = bytes(body)
        elif typ == b'IDAT':
            idat.append(bytes(body))
    if ihdr is None:
        raise PngError('нет IHDR')
    if check_crc:
        p = 8
        while p + 12 <= len(data):
            ln, = struct.unpack_from('>I', data, p)
            crc, = struct.unpack_from('>I', data, p + 8 + ln)
            if zlib.crc32(data[p + 4:p + 8 + ln]) & 0xffffffff != crc:
                raise PngError('CRC не совпал в чанке %r' % data[p + 4:p + 8])
            p += 12 + ln
    w, h, depth, ctype, comp, flt, il = ihdr
    if ctype not in CHANNELS or depth not in VALID_DEPTHS[ctype]:
        raise PngError('неподдерживаемое сочетание тип %d / глубина %d' % (ctype, depth))
    if comp != 0 or flt != 0 or il not in (0, 1):
        raise PngError('неподдерживаемые compression/filter/interlace')
    if w == 0 or h == 0:
        raise PngError('нулевой размер')
    raw = zlib.decompress(b''.join(idat))
    ch = CHANNELS[ctype]
    bits_pp = ch * depth
    bpp = max(1, bits_pp // 8)
    info = PngInfo(w, h, depth, ctype, il)
    if il == 0:
        stride = (w * bits_pp + 7) // 8
        rows = _unfilter(raw, h, stride, bpp)
        samples = _unpack_samples(rows, w, ch, depth)
        return _to_rgba(samples, ctype, depth, palette, trns), info
    # Adam7
    samples = np.zeros((h, w, ch), dtype=np.uint16)
    pos = 0
    for x0, y0, dx, dy in ADAM7:
        pw = (w - x0 + dx - 1) // dx
        ph = (h - y0 + dy - 1) // dy
        if pw <= 0 or ph <= 0:
            continue
        stride = (pw * bits_pp + 7) // 8
        size = ph * (stride + 1)
        rows = _unfilter(raw[pos:pos + size], ph, stride, bpp)
        pos += size
        samples[y0::dy, x0::dx, :] = _unpack_samples(rows, pw, ch, depth)
    return _to_rgba(samples, ctype, depth, palette, trns), info


def read_png(path_or_bytes):
    """Путь или bytes -> RGBA uint8[H, W, 4]."""
    if isinstance(path_or_bytes, (bytes, bytearray, memoryview)):
        return decode_png(path_or_bytes)[0]
    with open(path_or_bytes, 'rb') as f:
        return decode_png(f.read())[0]


# ----------------------------------------------------------------------------------------------------------------------
#                                                      кодировщик
# ----------------------------------------------------------------------------------------------------------------------

def _chunk(typ, body):
    return struct.pack('>I', len(body)) + typ + body + struct.pack('>I', zlib.crc32(typ + body) & 0xffffffff)


def _filter_rows(rows, bpp, mode):
    """rows uint8[h, stride] -> bytes с фильтр-байтами. mode: 'none' | 'up' | 'sub' | 'avg' | 'paeth' | 'mixed'."""
    h, stride = rows.shape
    out = bytearray()
    zero = np.zeros(stride, dtype=np.uint8)
    names = ['none', 'sub', 'up', 'avg', 'paeth']
    for y in range(h):
        line = rows[y]
        prev = rows[y - 1] if y else zero
        m = mode
        if mode == 'mixed':
            m = names[y % 5]
        if m == 'none':
            out.append(0)
            out += line.tobytes()
        elif m == 'sub':
            out.append(1)
            left = np.zeros(stride, dtype=np.uint8)
            left[bpp:] = line[:-bpp] if stride > bpp else left[bpp:]
            out += (line - left).astype(np.uint8).tobytes()
        elif m == 'up':
            out.append(2)
            out += (line - prev).astype(np.uint8).tobytes()
        elif m == 'avg':
            out.append(3)
            left = np.zeros(stride, dtype=np.uint8)
            left[bpp:] = line[:-bpp] if stride > bpp else left[bpp:]
            pr = ((left.astype(np.uint16) + prev.astype(np.uint16)) >> 1).astype(np.uint8)
            out += (line - pr).astype(np.uint8).tobytes()
        else:
            out.append(4)
            a = np.zeros(stride, dtype=np.int32)
            c = np.zeros(stride, dtype=np.int32)
            a[bpp:] = line[:-bpp] if stride > bpp else a[bpp:]
            c[bpp:] = prev[:-bpp] if stride > bpp else c[bpp:]
            b = prev.astype(np.int32)
            p = a + b - c
            pa, pb, pc = np.abs(p - a), np.abs(p - b), np.abs(p - c)
            pr = np.where((pa <= pb) & (pa <= pc), a, np.where(pb <= pc, b, c)).astype(np.uint8)
            out += (line - pr).astype(np.uint8).tobytes()
    return bytes(out)


def _pack_samples(samples, depth):
    """uint16[h, w, ch] -> uint8[h, stride] в формате PNG."""
    h, w, ch = samples.shape
    if depth == 8:
        return samples.reshape(h, w * ch).astype(np.uint8)
    if depth == 16:
        s = samples.reshape(h, w * ch).astype(np.uint16)
        return np.stack([(s >> 8) & 255, s & 255], axis=2).reshape(h, w * ch * 2).astype(np.uint8)
    per = 8 // depth
    n = (w + per - 1) // per
    pad = np.zeros((h, n * per), dtype=np.uint16)
    pad[:, :w] = samples[:, :, 0]
    out = np.zeros((h, n), dtype=np.uint16)
    for k in range(per):
        out |= pad[:, k::per] << (8 - depth * (k + 1))
    return out.astype(np.uint8)


def encode_png(samples, color_type, bit_depth, palette=None, trns=None, interlace=False, filter_mode='mixed', level=6):
    """Тестовый кодировщик произвольного формата. samples: uint16[h, w, ch] в исходной глубине бит."""
    h, w, ch = samples.shape
    assert ch == CHANNELS[color_type] and bit_depth in VALID_DEPTHS[color_type]
    bpp = max(1, ch * bit_depth // 8)
    hdr = struct.pack('>IIBBBBB', w, h, bit_depth, color_type, 0, 0, 1 if interlace else 0)
    parts = [SIGNATURE, _chunk(b'IHDR', hdr)]
    if palette is not None:
        parts.append(_chunk(b'PLTE', bytes(palette)))
    if trns is not None:
        parts.append(_chunk(b'tRNS', bytes(trns)))
    if not interlace:
        raw = _filter_rows(_pack_samples(samples, bit_depth), bpp, filter_mode)
    else:
        raw = b''
        for x0, y0, dx, dy in ADAM7:
            sub = samples[y0::dy, x0::dx, :]
            if sub.shape[0] == 0 or sub.shape[1] == 0:
                continue
            raw += _filter_rows(_pack_samples(sub, bit_depth), bpp, filter_mode)
    parts.append(_chunk(b'IDAT', zlib.compress(raw, level)))
    parts.append(_chunk(b'IEND', b''))
    return b''.join(parts)


def encode_rgba(rgba, filter_mode='paeth', level=6):
    """RGBA8 uint8[H, W, 4] -> bytes PNG."""
    rgba = np.ascontiguousarray(rgba, dtype=np.uint8)
    return encode_png(rgba.astype(np.uint16), 6, 8, filter_mode=filter_mode, level=level)


def write_png(path, rgba, filter_mode='paeth', level=6):
    """Записывает RGBA8 (H, W, 4) или RGB8 (H, W, 3) в файл."""
    a = np.asarray(rgba)
    if a.ndim == 3 and a.shape[2] == 3:
        a = np.concatenate([a, np.full(a.shape[:2] + (1,), 255, dtype=np.uint8)], axis=2)
    with open(path, 'wb') as f:
        f.write(encode_rgba(a, filter_mode, level))
