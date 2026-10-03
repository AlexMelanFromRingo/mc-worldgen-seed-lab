#!/usr/bin/env python3
"""G6 (особняки): части стартов woodland_mansion против сохранённых в эталоне полей шаблонных частей (Template, TPX/TPY/TPZ, Rot, Mi, BB).

  g6_woodland_mansion_pieces.py [--version 26.3] [-v]
g6_starts.py сверяет только id/BB/O/GD; здесь — шаблон, позиция, поворот и отражение каждой части в порядке списка.
Сборка — G6_BUILD (как у g6_starts.py).
"""
import argparse, glob, json, os, subprocess, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'tools', 'gt'))
import anvil
ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
BIN = f"{ROOT}/libmcgen/{os.environ.get('G6_BUILD', 'build')}/tests/g6_starts"


def ref_starts(world):
    rd = anvil.region_dir(world + '/world', 'overworld')
    out = {}
    for f in sorted(glob.glob(f'{rd}/r.*.mca')):
        rx, rz = map(int, os.path.basename(f).split('.')[1:3])
        rf = anvil.RegionFile(f)
        for lz in range(32):
            for lx in range(32):
                cx, cz = rx * 32 + lx, rz * 32 + lz
                if not rf.has(cx, cz):
                    continue
                n = anvil.parse_nbt(rf.raw(cx, cz))
                for k, v in n.get('structures', {}).get('starts', {}).items():
                    if v.get('id') != 'INVALID' and k == 'minecraft:mansion':
                        out[(v['ChunkX'], v['ChunkZ'])] = v
        rf.close()
    return out


def main():
    ap = argparse.ArgumentParser(); ap.add_argument('--version', default='26.3'); ap.add_argument('-v', action='store_true')
    a = ap.parse_args()
    tot = ok = 0
    for w in sorted(glob.glob(f'{ROOT}/run/gt/{a.version}/structure_minecraft_woodland_mansions/overworld-s*-c*-r*')):
        base = os.path.basename(w); seed = base.split('-s', 1)[1].rsplit('-c', 1)[0]
        for (cx, cz), st in sorted(ref_starts(w).items()):
            tot += 1
            p = subprocess.run([BIN, f'{ROOT}/run/pack-{a.version}', a.version, 'minecraft:overworld', seed, str(cx), str(cz), '1', '1'], capture_output=True, text=True)
            ours = [json.loads(l) for l in p.stdout.splitlines() if l.strip()]
            ours = [j for j in ours if j['id'] == 'minecraft:mansion' and j['cx'] == cx and j['cz'] == cz]
            rp = [(c['Template'], c['TPX'], c['TPY'], c['TPZ'], c['Rot'], c['Mi'], [int(x) for x in c['BB']]) for c in st['Children']]
            op = [(c['tpl'], *c['tpos'], c['trot'], c['tmir'], c['bb']) for c in ours[0]['pieces']] if ours else []
            bad = [i for i in range(max(len(rp), len(op))) if i >= len(rp) or i >= len(op) or rp[i] != op[i]]
            good = not bad
            ok += good
            print(f'{base:46s} старт {cx},{cz}: частей эталон {len(rp)} наши {len(op)}  {"совпало" if good else f"расхождений {len(bad)}"}')
            if a.v:
                for i in bad[:5]:
                    print('   ', i, 'эталон', rp[i] if i < len(rp) else None, '\n    ', i, 'наше  ', op[i] if i < len(op) else None)
    print(f'ИТОГО: стартов {tot}, совпали полностью {ok}')
    return 0 if ok == tot else 1


if __name__ == '__main__':
    sys.exit(main())
