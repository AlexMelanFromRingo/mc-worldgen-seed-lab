"""Минимальный декодер PNG (stdlib + numpy) для colormap'ов (grass/foliage/dry_foliage) и тестов: 8 бит, типы 0/2/3/4/6, без interlace."""
import struct
import zlib

import numpy as np


def read_png(path_or_bytes):
    """-> (массив uint8 (h, w, каналы 3 или 4), (w, h)). Палитровые и серые приводятся к RGB(A)."""
    data = path_or_bytes if isinstance(path_or_bytes, (bytes, bytearray)) else open(path_or_bytes, 'rb').read()
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError('не PNG')
    pos, idat, plte, trns, ihdr = 8, [], None, None, None
    while pos < len(data):
        n, typ = struct.unpack('>I4s', data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        pos += 12 + n
        if typ == b'IHDR':
            ihdr = struct.unpack('>IIBBBBB', body)
        elif typ == b'PLTE':
            plte = np.frombuffer(body, np.uint8).reshape(-1, 3)
        elif typ == b'tRNS':
            trns = np.frombuffer(body, np.uint8)
        elif typ == b'IDAT':
            idat.append(body)
        elif typ == b'IEND':
            break
    w, h, bd, ct, _cm, _fm, il = ihdr
    if il:
        raise ValueError('PNG с interlace не поддерживается')
    chans = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ct]
    bits = bd * chans
    stride = (w * bits + 7) // 8
    bpp = max(1, bits // 8)
    raw = np.frombuffer(zlib.decompress(b''.join(idat)), np.uint8).reshape(h, stride + 1)
    out = np.zeros((h, stride), np.uint8)
    prev = np.zeros(stride, np.int32)
    for y in range(h):
        f, line = raw[y, 0], raw[y, 1:].astype(np.int32)
        if f == 0:
            cur = line
        elif f == 1:
            cur = line.copy()
            for i in range(bpp, stride):
                cur[i] = (cur[i] + cur[i - bpp]) & 255
        elif f == 2:
            cur = (line + prev) & 255
        elif f == 3:
            cur = line.copy()
            for i in range(stride):
                left = cur[i - bpp] if i >= bpp else 0
                cur[i] = (cur[i] + ((left + prev[i]) >> 1)) & 255
        elif f == 4:
            cur = line.copy()
            for i in range(stride):
                a = cur[i - bpp] if i >= bpp else 0
                b = prev[i]
                c = prev[i - bpp] if i >= bpp else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                cur[i] = (cur[i] + pr) & 255
        else:
            raise ValueError('неизвестный фильтр PNG')
        out[y] = cur
        prev = cur
    if bd == 8:
        px = out[:, :w * chans].reshape(h, w, chans)
    else:                                   # 1/2/4 бита: распаковка
        per = 8 // bd
        mask = (1 << bd) - 1
        idx = np.zeros((h, stride * per), np.uint8)
        for k in range(per):
            idx[:, k::per] = (out >> (8 - bd * (k + 1))) & mask
        px = idx[:, :w].reshape(h, w, 1)
    if ct == 3:
        rgb = plte[px[..., 0]]
        if trns is not None:
            alpha = np.full(256, 255, np.uint8)
            alpha[:len(trns)] = trns
            return np.concatenate([rgb, alpha[px[..., 0]][..., None]], axis=2), (w, h)
        return rgb, (w, h)
    if ct == 0:
        return np.repeat(px, 3, axis=2), (w, h)
    if ct == 4:
        return np.concatenate([np.repeat(px[..., :1], 3, axis=2), px[..., 1:]], axis=2), (w, h)
    return px, (w, h)
