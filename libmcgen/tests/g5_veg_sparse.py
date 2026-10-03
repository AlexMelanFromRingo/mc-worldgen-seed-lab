#!/usr/bin/env python3
"""G5-veg «разреженные» эталоны: featuresparse:<id>@K (W6, tools/gt/datapack.py) — перед фичей стоит rarity_filter(K), декорируется ~1/K чанков,
межчанковый недетерминизм порядка почти исчезает, поэтому здесь ожидается 100 % даже у плотных фич.

    python3 libmcgen/tests/g5_veg_sparse.py [--version 26.3] [--only lush_caves_clay,...] [--report файл.json]
Наша сторона: MCGEN_FEATURES_ONLY=<id> + MCGEN_PACK_OVERLAY=<мир>/world/datapacks/gt (override placed_feature с rarity_filter).
"""
import argparse, glob, json, os, sys, tempfile, shutil
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import g5_features as G

def decorated(wd, v, fid, env_extra, stages):
    """Чанки, где фича что-то поставила (отладочный вывод MCGEN_FEATURES_LOGCHUNKS в stderr нашего CLI)."""
    import subprocess
    m = json.load(open(f'{wd}/manifest.json')); x0, z0, x1, z1 = m['area_chunks']
    out = tempfile.mktemp(suffix='.mcr', dir='/tmp')
    env = dict(os.environ, MCGEN_FEATURES_ONLY=fid, MCGEN_FEATURES_LOGCHUNKS='1', **env_extra)
    r = subprocess.run([G.CLI, '--pack', f'{G.ROOT}/run/pack-{v}', '--version', v, '--dim', G.DIMS[m['dim']], '--seed', str(m['seed']), '--cx0', str(x0), '--cz0', str(z0),
                        '--nx', str(x1 - x0 + 1), '--nz', str(z1 - z0 + 1), '--stages', stages, '--threads', '0', '--pp-margin', '1', '--out', out], env=env, capture_output=True, text=True)
    if os.path.exists(out): os.remove(out)
    ch = set()
    for line in r.stderr.splitlines():
        if line.startswith('FEATCHUNK '):
            _, _, cx, cz = line.split(); ch.add((int(cx), int(cz)))
    return ch


def isolation(dec, mism):
    """Декорируемый чанк «одиночный», если рядом (Чебышёв <= 2) нет других декорируемых: окна 3x3 вокруг них не пересекаются, порядок обхода соседей
    не влияет. Возвращает [чанков в окнах одиночных, расхождений в них, чанков в остальных окнах кластеров, расхождений в них]."""
    single = [d for d in dec if all(max(abs(d[0] - e[0]), abs(d[1] - e[1])) > 2 for e in dec if e != d)]
    sw = set()
    for d in single:
        for dx in (-1, 0, 1):
            for dz in (-1, 0, 1): sw.add((d[0] + dx, d[1] + dz))
    rest = set()
    for d in dec:
        if d in single: continue
        for dx in (-1, 0, 1):
            for dz in (-1, 0, 1): rest.add((d[0] + dx, d[1] + dz))
    rest -= sw
    return [len(sw), sum(mism.get(c, 0) for c in sw), len(rest), sum(mism.get(c, 0) for c in rest), len(single), len(dec)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--version', default='26.3'); ap.add_argument('--only', default=''); ap.add_argument('--report', default='')
    ap.add_argument('--stages', default='0x17'); ap.add_argument('--orders', action='store_true', help='при расхождениях перебрать порядки обхода чанков (MCGEN_FEATURES_SEQ) и показать лучший')
    a = ap.parse_args()
    G.CLI = tempfile.mktemp(prefix='mcgen-cli-', dir='/tmp'); shutil.copy(os.environ.get('MCGEN_CLI') or f'{G.ROOT}/libmcgen/build/mcgen-cli', G.CLI)
    only = {x if ':' in x else 'minecraft:' + x for x in a.only.split(',') if x}
    rows, bad = [], 0
    for fdir in sorted(glob.glob(f'{G.ROOT}/run/gt/{a.version}/featuresparse_*')):
        for wd in sorted(glob.glob(fdir + '/*/')):
            wd = wd.rstrip('/')
            mp = f'{wd}/manifest.json'
            if not os.path.exists(mp) or '_rep' in os.path.basename(wd): continue
            m = json.load(open(mp))
            if not m.get('ok'): continue
            var = m['variant']                                   # featuresparse:minecraft:x@K
            fid = var.split(':', 1)[1].split('@')[0]
            if only and fid not in only: continue
            ov = f'{wd}/world/datapacks/gt'
            d = G.run_one(a.version, wd, fid, 1, 0, a.stages, env_extra={'MCGEN_PACK_OVERLAY': ov})
            ctl = G.run_one(a.version, wd, fid, 1, 0, hex(int(a.stages, 16) & ~16), env_extra={'MCGEN_PACK_OVERLAY': ov})
            if 'blocks_compared' not in d:
                print(f'{var:55s} ОШИБКА {d.get("error")}'); bad += 1; continue
            cm = {tuple(int(t) for t in k.split(',')): v[0] for k, v in d.get('_cmap', {}).items()}
            iso = isolation(decorated(wd, a.version, fid, {'MCGEN_PACK_OVERLAY': ov}, a.stages), cm)
            rows.append(dict(iso=iso, variant=var, feature=fid, dim=m['dim'], blocks=d['blocks_compared'], mismatch=d['blocks_mismatch'], match_pct=d['match_pct'], effect=ctl.get('blocks_mismatch')))
            best = None
            if a.orders and d['blocks_mismatch'] > 0:
                best = (d['blocks_mismatch'], 'xz(основной)')
                for o in ('zx', 'ring', 'xz-', 'zx-', 'xzw'):
                    dd = G.run_one(a.version, wd, fid, 1, 0, a.stages, env_extra={'MCGEN_PACK_OVERLAY': ov, 'MCGEN_FEATURES_SEQ': o})
                    if 'blocks_mismatch' in dd and dd['blocks_mismatch'] < best[0]: best = (dd['blocks_mismatch'], o)
                rows[-1]['best_order'] = best
            bad += d['blocks_mismatch'] > 0
            print(f'{var:55s} {m["dim"]:9s} {d["blocks_compared"]:>12,} блоков  расхождений {d["blocks_mismatch"]:>8,}  {d["match_pct"]:.6f} %  эффект {ctl.get("blocks_mismatch", "?")}  | декорируемых {iso[5]}, одиночных {iso[4]}: в их окнах {iso[0]} чанков, расх. {iso[1]}; окна кластеров {iso[2]} чанков, расх. {iso[3]}' + (f'  | лучший порядок {best[1]}: {best[0]}' if best else ''), flush=True)
    if a.report:
        json.dump({'version': a.version, 'rows': rows}, open(a.report, 'w'), indent=1, ensure_ascii=False)
    print(f'итого: {len(rows)} разреженных миров, с расхождениями: {bad}')
    sys.exit(1 if bad else 0)

if __name__ == '__main__':
    main()
