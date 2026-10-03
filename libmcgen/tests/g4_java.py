#!/usr/bin/env python3
"""G4 без сервера: libmcgen (стадии TERRAIN[+SURFACE]+CARVERS) против настоящих классов игры (Java-эталон libmcgen/tests/g4_java:
doFill + [buildSurface] + applyCarvers одного чанка, тот же NoiseChunk/Aquifer, CarvingMask). Проверяет версии и области, для которых нет серверных
эталонов (26.1, 26.2, 26.4-snapshot-2, любые пресеты). Растекание жидкостей выключено (fluid_flow=0) — эталон его не выполняет.

  python3 libmcgen/tests/g4_java.py --version 26.1 --dim overworld --seed 12345 --cx0 0 --cz0 0 --nx 4 --nz 4 [--surface]
Без --surface — как carve_raw (стадии 0xb, без поверхности); с --surface — стадии 0xf. Код возврата 0 — все блоки совпали.
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
        self.p = subprocess.Popen([os.path.join(HERE, 'g4_java', 'run.sh'), version], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--version', default='26.1')
    ap.add_argument('--dim', default='overworld', choices=list(DIM))
    ap.add_argument('--preset', default='normal')
    ap.add_argument('--seeds', default='12345')
    ap.add_argument('--cx0', type=int, default=0)
    ap.add_argument('--cz0', type=int, default=0)
    ap.add_argument('--nx', type=int, default=4)
    ap.add_argument('--nz', type=int, default=4)
    ap.add_argument('--surface', action='store_true')
    ap.add_argument('--cli', default=os.path.join(ROOT, 'libmcgen', 'build', 'mcgen-cli'))
    ap.add_argument('--threads', type=int, default=0)
    ap.add_argument('--java-opts', default='-Xmx3g')
    ap.add_argument('--tmp', default=os.environ.get('TMPDIR', '/tmp'))
    ap.add_argument('--report')
    a = ap.parse_args()
    java = Java(a.version, a.java_opts)
    rows = []
    stages = '0xf' if a.surface else '0xb'
    try:
        for seed in [int(s) for s in a.seeds.split(',')]:
            out = os.path.join(a.tmp, f'g4j-{a.version}-{a.dim}-{seed}-{a.cx0}_{a.cz0}.mcr')
            cmd = [a.cli, '--pack', os.path.join(ROOT, 'run', f'pack-{a.version}'), '--version', a.version, '--dim', DIM[a.dim], '--preset', a.preset,
                   '--seed', str(seed), '--cx0', str(a.cx0), '--cz0', str(a.cz0), '--nx', str(a.nx), '--nz', str(a.nz), '--stages', stages,
                   '--threads', str(a.threads), '--tweak', 'fluid_flow=0', '--out', out]
            if not a.surface and a.version not in ('26.1', '26.2'):
                cmd += ['--tweak', 'ore_veins=0']      # 26.3+: жилы — часть правила материала; эталон carve_raw — правило из одного default_block
            t0 = time.time(); p = subprocess.run(cmd, capture_output=True, text=True); t_ours = time.time() - t0
            if p.returncode:
                raise RuntimeError('mcgen-cli: ' + p.stderr[-400:])
            M = mcr.Mcr(out)
            bad = tot = badc = 0; pairs = collections.Counter(); carved = 0
            t0 = time.time()
            for cz in range(a.cz0, a.cz0 + a.nz):
                for cx in range(a.cx0, a.cx0 + a.nx):
                    ans = java.cmd(f'carve {a.dim} {a.preset} {seed} {cx} {cz}' + ('' if a.surface else ' nosurface'))
                    jb = np.frombuffer(base64.b64decode(ans[0]), dtype='<u2').reshape(M.height, 16, 16)
                    ob = np.asarray(M.blocks(cx, cz))
                    d = jb != ob; n = int(d.sum()); tot += d.size
                    if n:
                        bad += n; badc += 1
                        ys, zs, xs = np.nonzero(d)
                        for y, z, x in list(zip(ys, zs, xs))[:300]:
                            pairs[(M.state_names[ob[y, z, x]], M.state_names[jb[y, z, x]])] += 1
                        if badc <= 3:
                            print(f'   чанк ({cx},{cz}): {n} расхождений, первые (x,y,z): ' +
                                  ', '.join(f'({cx * 16 + int(x)},{M.min_y + int(y)},{cz * 16 + int(z)})' for y, z, x in list(zip(ys, zs, xs))[:4]), flush=True)
            os.remove(out)
            rows.append({'version': a.version, 'dim': a.dim, 'preset': a.preset, 'seed': seed, 'cx0': a.cx0, 'cz0': a.cz0, 'nx': a.nx, 'nz': a.nz,
                         'surface': a.surface, 'blocks': tot, 'mismatch': bad, 'chunks_bad': badc, 'top': [[k[0], k[1], v] for k, v in pairs.most_common(5)]})
            print(f'{a.version} {a.dim:9s} {a.preset} s={seed} ({a.cx0},{a.cz0}) {a.nx}x{a.nz} {"surface+carve" if a.surface else "carve_raw":13s} блоков {tot:,} расхождений {bad} (чанков {badc})'
                  f' | наш {t_ours:.1f} с, Java {time.time() - t0:.0f} с' + ('' if not bad else '  ' + json.dumps(pairs.most_common(4), ensure_ascii=False)), flush=True)
    finally:
        java.close()
    if a.report:
        json.dump(rows, open(a.report, 'w'), indent=1, ensure_ascii=False)
    return 1 if any(r['mismatch'] for r in rows) else 0


if __name__ == '__main__':
    sys.exit(main())
