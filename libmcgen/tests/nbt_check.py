#!/usr/bin/env python3
"""Сверка распаковки gzip/zlib и разбора NBT libmcgen (src/inflate.c, src/nbt.c) с независимым разбором на Python
(gzip из стандартной библиотеки + свой разбор NBT) на всех шаблонах построек пак-каталога.

    python3 libmcgen/tests/nbt_check.py [--version 26.3]
"""
import argparse, glob, gzip, os, struct, subprocess, sys, time, zlib

HERE = os.path.dirname(os.path.abspath(__file__)); ROOT = os.path.dirname(os.path.dirname(HERE))
DUMP = os.path.join(ROOT, 'libmcgen', 'build', 'tests', 'nbt_dump')


def mutf8(b):
    b = b.replace(b'\xc0\x80', b'\x00')
    return b.decode('utf-8', 'surrogatepass').encode('utf-16', 'surrogatepass').decode('utf-16').encode('utf-8')


class R:
    def __init__(self, b): self.b, self.i = b, 0
    def take(self, n):
        v = self.b[self.i:self.i + n]
        if len(v) != n: raise ValueError('eof')
        self.i += n; return v
    def u(self, fmt): return struct.unpack('>' + fmt, self.take(struct.calcsize('>' + fmt)))[0]
    def s(self): return mutf8(self.take(self.u('H')))


def payload(r, t, out):
    if t == 1: out.append(b'b%d' % r.u('b'))
    elif t == 2: out.append(b's%d' % r.u('h'))
    elif t == 3: out.append(b'i%d' % r.u('i'))
    elif t == 4: out.append(b'l%d' % r.u('q'))
    elif t == 5: out.append(b'f%08x' % r.u('I'))
    elif t == 6: out.append(b'd%016x' % r.u('Q'))
    elif t == 8: out.append(b"'" + r.s() + b"'")
    elif t in (7, 11, 12):
        n = r.u('i'); f = {7: 'b', 11: 'i', 12: 'q'}[t]
        vals = struct.unpack('>%d%s' % (n, f), r.take(n * struct.calcsize(f)))
        out.append({7: b'B', 11: b'I', 12: b'L'}[t] + b'%d:' % n + b','.join(b'%d' % v for v in vals))
    elif t == 9:
        et = r.u('b'); n = r.u('i')
        out.append(b'[%d|' % et)
        for k in range(n):
            if k: out.append(b',')
            payload(r, et, out)
        out.append(b']')
    elif t == 10:
        out.append(b'{')
        while True:
            ct = r.u('b')
            if ct == 0: break
            out.append(r.s() + b':'); payload(r, ct, out); out.append(b';')
        out.append(b'}')
    else: raise ValueError(f'tag {t}')


def canon(raw):
    if raw[:2] == b'\x1f\x8b': raw = gzip.decompress(raw)
    elif raw[:1] == b'\x78': raw = zlib.decompress(raw)
    r = R(raw); t = r.u('b'); r.s(); out = []
    payload(r, t, out)
    return b''.join(out)


def main():
    ap = argparse.ArgumentParser(); ap.add_argument('--version', default='26.3'); a = ap.parse_args()
    files = sorted(glob.glob(os.path.join(ROOT, 'run', f'pack-{a.version}', 'data', '*', 'structure', '**', '*.nbt'), recursive=True))
    t0 = time.time(); ours = {}
    for i in range(0, len(files), 200):
        p = subprocess.run([DUMP] + files[i:i + 200], capture_output=True)
        for line in p.stdout.split(b'\n'):
            if line:
                path, st, body = line.split(b'\t', 2)
                ours[path.decode()] = (st, body)
    tc = time.time() - t0
    bad = 0; nbytes = 0
    for f in files:
        ref = canon(open(f, 'rb').read()); nbytes += len(ref)
        st, body = ours.get(f, (b'ERR', 'нет вывода'.encode()))
        if st == b'ERR' or body != ref:
            bad += 1
            if bad <= 5: print('РАСХОЖДЕНИЕ', os.path.relpath(f, ROOT), body[:120])
    print(f'NBT {a.version}: файлов {len(files)}, расхождений {bad}; libmcgen {tc:.2f} с (с печатью), канонической записи {nbytes:,} байт')
    return bad != 0


if __name__ == '__main__':
    sys.exit(main())
