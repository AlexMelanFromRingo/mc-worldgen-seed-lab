#!/usr/bin/env python3
"""G3 без сервера: libmcgen (BIOMES+TERRAIN+SURFACE) против настоящих классов игры (Java-эталон libmcgen/tests/g3_java: doFill +
buildSurface одного чанка на BiomeSource измерения). Позволяет проверять любые области/биомы и версии, для которых нет серверных эталонов
(26.1, 26.2, 26.4-snapshot-2, редкие биомы). Растекание жидкостей (fluid_flow) выключено — Java-эталон его не выполняет.

Примеры:
  python3 libmcgen/tests/g3_java.py --version 26.3 --dim overworld --seed 12345 --cx0 0 --cz0 0 --nx 4 --nz 4
  python3 libmcgen/tests/g3_java.py --version 26.3 --seed 12345 --find mangrove_swamp,mushroom_fields,ice_spikes --radius 20 --nx 2 --nz 2
  python3 libmcgen/tests/g3_java.py --version 26.1 --dim nether --seed 8675309 --cx0 60 --cz0 -60 --nx 4 --nz 4
Код возврата 0 — все блоки совпали.
"""
import argparse, base64, collections, json, os, subprocess, sys, time
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'gt'))
import mcr  # noqa: E402

DIM = {'overworld': 'minecraft:overworld', 'nether': 'minecraft:the_nether', 'end': 'minecraft:the_end'}


class Java:
    def __init__(self, version, opts):
        env = dict(os.environ)
        if opts:
            env['MCGENREF_JAVA_OPTS'] = opts
        self.p = subprocess.Popen([os.path.join(HERE, 'g3_java', 'run.sh'), version], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.DEVNULL, text=True, env=env)

    def cmd(self, line):
        self.p.stdin.write(line + '\n'); self.p.stdin.flush()
        r = self.p.stdout.readline().strip()
        if not r.startswith('ok '):
            raise RuntimeError(r or 'пустой ответ Java-эталона')
        return r[3:].split(' ')

    def close(self):
        try:
            self.p.stdin.write('quit\n'); self.p.stdin.flush(); self.p.wait(timeout=10)
        except Exception:
            self.p.kill()


def cli_dump(a, dim, preset, seed, cx0, cz0, nx, nz, out, stages='0x7'):
    cmd = [a.cli, '--pack', os.path.join(ROOT, 'run', f'pack-{a.version}'), '--version', a.version, '--dim', DIM[dim], '--preset', preset,
           '--seed', str(seed), '--cx0', str(cx0), '--cz0', str(cz0), '--nx', str(nx), '--nz', str(nz), '--stages', stages,
           '--threads', str(a.threads), '--tweak', 'fluid_flow=0', '--out', out]
    p = subprocess.run(cmd, capture_output=True, text=True)
    if p.returncode:
        raise RuntimeError('mcgen-cli: ' + p.stderr[-400:])


def find_biomes(a, dim, preset, seed, names, radius_chunks, step=32):
    """Первое попадание каждого биома на сетке (шаг step блоков, y = a.y) в квадрате ±radius_chunks чанков; mcgen-cli biome."""
    n = (2 * radius_chunks * 16) // step + 1
    x0 = z0 = -radius_chunks * 16
    cmd = [a.cli, 'biome', '--pack', os.path.join(ROOT, 'run', f'pack-{a.version}'), '--version', a.version, '--dim', DIM[dim], '--preset', preset,
           '--seed', str(seed), '--x0', str(x0), '--z0', str(z0), '--nx', str(n), '--nz', str(n), '--step', str(step), '--y', str(a.y)]
    p = subprocess.run(cmd, capture_output=True, text=True)
    if p.returncode:
        raise RuntimeError('mcgen-cli biome: ' + p.stderr[-300:])
    lines = p.stdout.split('\n')
    found = {}
    cnt = collections.Counter()
    for i, nm in enumerate(lines[:n * n]):
        short = nm.replace('minecraft:', '')
        cnt[short] += 1
        if short in names and short not in found:
            iz, ix = divmod(i, n)
            found[short] = (x0 + ix * step, z0 + iz * step)
    return found, cnt


def compare(a, java, dim, preset, seed, cx0, cz0, nx, nz, rows, label=''):
    out = os.path.join(a.tmp, f'g3j-{a.version}-{dim}-{seed}-{cx0}_{cz0}.mcr')
    t0 = time.time()
    cli_dump(a, dim, preset, seed, cx0, cz0, nx, nz, out)
    t_ours = time.time() - t0
    M = mcr.Mcr(out)
    bad = 0; tot = 0; pairs = collections.Counter(); badc = 0; hm_bad = 0
    t0 = time.time()
    for cz in range(cz0, cz0 + nz):
        for cx in range(cx0, cx0 + nx):
            ans = java.cmd(f'surface {dim} {preset} {seed} {cx} {cz}')
            jb = np.frombuffer(base64.b64decode(ans[0]), dtype='<u2').reshape(M.height, 16, 16)
            jh = np.frombuffer(base64.b64decode(ans[1]), dtype='<i2').reshape(4, 256).astype(np.int32) if len(ans) > 1 else None
            if jh is not None:
                oh = np.asarray(M.heightmaps(cx, cz)).astype(np.int32)
                hm_bad += int((jh != oh).sum())
            ob = np.asarray(M.blocks(cx, cz))
            d = jb != ob
            n = int(d.sum())
            tot += d.size
            if n:
                bad += n; badc += 1
                ys, zs, xs = np.nonzero(d)
                for y, z, x in list(zip(ys, zs, xs))[:200]:
                    pairs[(M.state_names[ob[y, z, x]], M.state_names[jb[y, z, x]])] += 1
                if badc <= 3:
                    print(f'   чанк ({cx},{cz}): {n} расхождений, первые (x,y,z): ' +
                          ', '.join(f'({cx * 16 + int(x)},{M.min_y + int(y)},{cz * 16 + int(z)})' for y, z, x in list(zip(ys, zs, xs))[:4]), flush=True)
    t_java = time.time() - t0
    stats = {}
    if a.stats:
        want = [w for w in a.stats.split(',') if w]
        ids = {i: nm for i, nm in enumerate(M.state_names) if any(nm.startswith('minecraft:' + w) for w in want)}
        if ids:
            lut = np.zeros(len(M.state_names), dtype=bool)
            lut[list(ids)] = True
            cnt = collections.Counter()
            for cz in range(cz0, cz0 + nz):
                for cx in range(cx0, cx0 + nx):
                    b = np.asarray(M.blocks(cx, cz))
                    u, c = np.unique(b[lut[b]], return_counts=True)
                    for k, v in zip(u, c):
                        cnt[ids[int(k)].split('[')[0].replace('minecraft:', '')] += int(v)
            stats = dict(cnt)
    if not a.keep:
        os.remove(out)
    rows.append({'label': label, 'dim': dim, 'seed': seed, 'cx0': cx0, 'cz0': cz0, 'nx': nx, 'nz': nz, 'blocks': tot, 'mismatch': bad,
                 'chunks_bad': badc, 'hm_mismatch': hm_bad, 'stats': stats, 'top': [[k[0], k[1], v] for k, v in pairs.most_common(5)], 'ours_s': round(t_ours, 2), 'java_s': round(t_java, 1)})
    print(f'{a.version} {dim:9s} s={seed} ({cx0},{cz0}) {nx}x{nz} {label:22s} блоков {tot:,} расхождений {bad} (чанков {badc}), карт высот {hm_bad} | наш {t_ours:.1f} с, Java {t_java:.1f} с'
          + ('' if not bad else '  ' + json.dumps(pairs.most_common(4), ensure_ascii=False)), flush=True)
    if stats:
        print('      блоки: ' + ', '.join(f'{k}:{v}' for k, v in sorted(stats.items())), flush=True)
    return bad + hm_bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--version', default='26.3')
    ap.add_argument('--dim', default='overworld', choices=list(DIM))
    ap.add_argument('--preset', default='normal')
    ap.add_argument('--seed', type=int, default=12345)
    ap.add_argument('--seeds', default='', help='несколько seed через запятую (для --find)')
    ap.add_argument('--cx0', type=int, default=0)
    ap.add_argument('--cz0', type=int, default=0)
    ap.add_argument('--nx', type=int, default=2)
    ap.add_argument('--nz', type=int, default=2)
    ap.add_argument('--find', default='', help='биомы через запятую: найти области с ними (первое попадание на сетке) и сравнить')
    ap.add_argument('--radius', type=int, default=24, help='радиус поиска --find в чанках')
    ap.add_argument('--y', type=int, default=70, help='y поиска биомов')
    ap.add_argument('--cli', default=os.path.join(ROOT, 'libmcgen', 'build', 'mcgen-cli'))
    ap.add_argument('--threads', type=int, default=0)
    ap.add_argument('--java-opts', default='-Xmx3g')
    ap.add_argument('--tmp', default=os.environ.get('TMPDIR', '/tmp'))
    ap.add_argument('--keep', action='store_true')
    ap.add_argument('--stats', default='', help='имена блоков (префикс после minecraft:) через запятую: напечатать, сколько их в сравниваемой области')
    ap.add_argument('--report')
    a = ap.parse_args()
    rows = []
    java = Java(a.version, a.java_opts)
    bad = 0
    try:
        if a.find:
            names = [s.strip() for s in a.find.split(',') if s.strip()]
            seeds = [int(s) for s in a.seeds.split(',')] if a.seeds else [a.seed]
            for seed in seeds:
                found, cnt = find_biomes(a, a.dim, a.preset, seed, set(names), a.radius)
                for nm in names:
                    if nm not in found:
                        print(f'{a.version} {a.dim} s={seed}: биом {nm} не найден в ±{a.radius} чанков', flush=True)
                        continue
                    bx, bz = found[nm]
                    cx0 = (bx >> 4) - a.nx // 2; cz0 = (bz >> 4) - a.nz // 2
                    bad += compare(a, java, a.dim, a.preset, seed, cx0, cz0, a.nx, a.nz, rows, nm)
        else:
            bad += compare(a, java, a.dim, a.preset, a.seed, a.cx0, a.cz0, a.nx, a.nz, rows)
    finally:
        java.close()
    tot = sum(r['blocks'] for r in rows)
    print(f'ИТОГО: областей {len(rows)}, блоков {tot:,}, расхождений {bad}')
    if a.report:
        json.dump({'version': a.version, 'rows': rows, 'blocks': tot, 'mismatch': bad}, open(a.report, 'w'), indent=1, ensure_ascii=False)
    return 0 if bad == 0 else 1


if __name__ == '__main__':
    sys.exit(main())
