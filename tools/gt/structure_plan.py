#!/usr/bin/env python3
"""План областей с настоящими постройками для эталонов (G6 и «витрина» блоков для мешера): oracle (код Mojang) -> tools/gt/structures_plan.json.

  structure_plan.py [--seeds 12345,8675309,...] [--version 26.3] [--radius 120]

Для каждого structure_set и seed ищет ближайший к началу координат чанк, где `structstart` вернул generated=true (валидный старт по биому/высоте).
План потребляет gen_queue.py --structures: вариант `structure:<set>` и область вокруг найденного чанка. Oracle запускается как `oracle/run.sh <V> serve`
(не сервер Minecraft; блокировка сервера не нужна).
"""
import argparse, json, os, subprocess, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from common import ROOT

SEEDS = [12345, 8675309, -7048155917072976836]
DIMS = {'nether_complexes': ['nether'], 'nether_fossils': ['nether'], 'end_cities': ['end'], 'ruined_portals': ['overworld', 'nether']}


class Oracle:
    def __init__(self, version):
        env = dict(os.environ, ORACLE_JAVA_OPTS='-Xmx2g')
        self.p = subprocess.Popen([f'{ROOT}/oracle/run.sh', version, 'serve'], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                                  text=True, bufsize=1, env=env)

    def q(self, line):
        self.p.stdin.write(line + '\n'); self.p.stdin.flush()
        out = self.p.stdout.readline()
        return json.loads(out) if out else {'ok': False, 'error': 'eof'}

    def close(self):
        try:
            self.p.stdin.close(); self.p.wait(timeout=20)
        except Exception:
            self.p.kill()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--version', default='26.3'); ap.add_argument('--radius', type=int, default=120)
    ap.add_argument('--seeds', type=lambda s: [int(x) for x in s.split(',')], default=SEEDS)
    ap.add_argument('--out', default=f'{ROOT}/tools/gt/structures_plan.json')
    ap.add_argument('--max-try', type=int, default=80)
    a = ap.parse_args()
    sets = sorted(f[:-5] for f in os.listdir(f'{ROOT}/run/pack-{a.version}/data/minecraft/worldgen/structure_set'))
    o = Oracle(a.version)
    plan = []
    t0 = time.time()
    for seed in a.seeds:
        for st in sets:
            if st == 'strongholds':
                continue            # concentric_rings: позиции — oracle `stronghold <seed>`; отдельный случай
            for dim in DIMS.get(st, ['overworld']):
                found = None
                for R in (a.radius, 3 * a.radius):
                    r = o.q(f'structs {dim} {seed} minecraft:{st} {-R} {-R} {2 * R} {2 * R}')
                    if not r.get('ok'):
                        break
                    cands = sorted(r['sets'].get(f'minecraft:{st}', []), key=lambda c: abs(c[0]) + abs(c[1]))
                    for cx, cz in cands[:a.max_try]:
                        s = o.q(f'structstart {dim} {seed} {cx} {cz} minecraft:{st}')
                        if not s.get('ok'):
                            continue
                        res = s['results'][0] if s.get('results') else {}
                        if res.get('generated'):
                            ok = [x['structure'] for x in res.get('attempts', []) if x.get('valid')]
                            found = {'set': f'minecraft:{st}', 'dim': dim, 'seed': seed, 'cx': cx, 'cz': cz, 'structure': ok[0] if ok else None}
                            break
                    if found:
                        break
                print(f'{time.time() - t0:6.0f}s seed {seed} {st:20s} {dim:9s} ->', found or 'не найдено', flush=True)
                if found:
                    plan.append(found)
    o.close()
    json.dump({'doc': 'tools/gt/structure_plan.py (oracle structstart): ближайшие валидные старты построек', 'version': a.version, 'plan': plan},
              open(a.out, 'w'), indent=1, ensure_ascii=False)


if __name__ == '__main__':
    main()
