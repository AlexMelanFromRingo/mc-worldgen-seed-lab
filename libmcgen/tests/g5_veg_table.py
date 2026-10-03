#!/usr/bin/env python3
"""Таблица «placed_feature × статус» для группы растительности (W10) — в docs/blender/features-veg.md.

    python3 libmcgen/tests/g5_veg_table.py --dense dense.json --sparse sparse.json [--version 26.3] > таблица.md
dense.json — отчёт g5_features.py (--report), sparse.json — отчёт g5_veg_sparse.py (--report). Выгрузка реализации — mcgen-cli (MCGEN_FEATURES_DUMP_ORDER).
Статусы:
  «100 %»          есть эталон с эффектом (>0 блоков), расхождений 0 (плотный r=5 или разреженный featuresparse);
  «100 % в одиночных окнах» — разреженный эталон: в окнах 3x3 вокруг декорируемых чанков, не пересекающихся с другими декорируемыми, расхождений 0, а остаток — в кластерах
                     (порядок обхода соседних чанков: недетерминизм ванильного сервера);
  «шум порядка»    расхождения только в кластерах, одиночных окон нет (нельзя отделить от недетерминизма);
  «нет эффекта»    реализована, но в имеющихся эталонах фича ничего не меняет (нужна другая область);
  «нет эталона»    реализована, эталона нет."""
import argparse, collections, json, os, subprocess, sys, tempfile
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CLI = os.environ.get('MCGEN_CLI') or f'{ROOT}/libmcgen/build/mcgen-cli'
MINE = {'simple_block', 'block_column', 'bamboo', 'vines', 'vegetation_patch', 'waterlogged_vegetation_patch', 'root_system', 'coral_tree', 'coral_claw', 'coral_mushroom',
        'huge_fungus', 'weighted_random_selector', 'simple_random_selector', 'random_boolean_selector', 'kelp', 'seagrass', 'sea_pickle', 'nether_forest_vegetation',
        'twisting_vines', 'weeping_vines', 'random_selector', 'no_op'}


def leaves(rd, d, f):
    out = []
    if isinstance(f, str):
        f = json.load(open(f'{rd}/{d}/{f.split(":")[-1]}.json'))
    out.append(f['type'].split(':')[1])
    cfg = f.get('config', f)

    def walk(o):
        if isinstance(o, dict):
            for k, v in o.items():
                if k in ('feature', 'default', 'feature_true', 'feature_false', 'vegetation_feature', 'ground_feature', 'data', 'features'):
                    for it in (v if isinstance(v, list) else [v]):
                        if isinstance(it, dict) and 'data' in it: it = it['data']
                        if isinstance(it, dict) and 'feature' in it and 'chance' in it: it = it['feature']
                        if isinstance(it, str) and os.path.exists(f'{rd}/placed_feature/{it.split(":")[-1]}.json'):
                            out.extend(leaves(rd, d, json.load(open(f'{rd}/placed_feature/{it.split(":")[-1]}.json'))['feature']))
                        elif isinstance(it, dict) and 'feature' in it and 'placement' in it: out.extend(leaves(rd, d, it['feature']))
                        else: walk(it)
                else: walk(v)
        elif isinstance(o, list):
            for v in o: walk(v)
    walk(cfg)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--version', default='26.3'); ap.add_argument('--dense', default=''); ap.add_argument('--sparse', default='')
    a = ap.parse_args()
    v = a.version
    rd = f'{ROOT}/run/pack-{v}/data/minecraft/worldgen'
    d = 'feature' if os.path.exists(rd + '/feature') else 'configured_feature'
    impl, dims = {}, collections.defaultdict(set)
    for dn, dim in (('overworld', 'minecraft:overworld'), ('nether', 'minecraft:the_nether')):
        f = tempfile.mktemp(suffix='.json'); mcr = f + '.mcr'
        subprocess.run([CLI, '--pack', f'{ROOT}/run/pack-{v}', '--version', v, '--dim', dim, '--seed', '1', '--cx0', '0', '--cz0', '0', '--nx', '1', '--nz', '1', '--stages', '0x13',
                        '--threads', '1', '--out', mcr], env=dict(os.environ, MCGEN_FEATURES_DUMP_ORDER=f), capture_output=True)
        for p in json.load(open(f))['placed']:
            impl[p['id']] = p['impl']; dims[p['id']].add(dn)
        os.remove(f); os.path.exists(mcr) and os.remove(mcr)
    dense, sparse = collections.defaultdict(list), collections.defaultdict(list)
    if a.dense and os.path.exists(a.dense):
        for r in json.load(open(a.dense))['rows']: dense[r['feature']].append(r)
    if a.sparse and os.path.exists(a.sparse):
        for r in json.load(open(a.sparse))['rows']: sparse[r['feature']].append(r)
    rows, cnt = [], collections.Counter()
    for fid in sorted(impl):
        n = fid.split(':')[1]
        try:
            t = leaves(rd, d, json.load(open(f'{rd}/placed_feature/{n}.json'))['feature'])
        except Exception:
            continue
        if not set(t) <= MINE or (set(t) == {'random_selector'}) or 'tree' in t: continue
        top = t[0]
        ds, ss = dense.get(fid, []), sparse.get(fid, [])
        parts = []
        status = 'нет эталона'
        eff_any = False
        for r in ds:
            if (r.get('effect') or 0) > 0:
                eff_any = True; parts.append(f'плотный r5: {r["mismatch"]} из {r["effect"]} изменённых')
        sp_ok = sp_iso = sp_noise = False
        for r in ss:
            if (r.get('effect') or 0) <= 0: continue
            eff_any = True
            iso = r.get('iso') or [0, 0, 0, 0, 0, 0]
            parts.append(f'{r["variant"].split("@")[1] and "@" + r["variant"].split("@")[1]}: расх. {r["mismatch"]}, одиночных окон {iso[4]}/{iso[5]} (расх. в них {iso[1]})')
            if r['mismatch'] == 0: sp_ok = True
            elif iso[4] > 0 and iso[1] == 0: sp_iso = True
            else: sp_noise = True
        dense_ok = any((r.get('effect') or 0) > 0 and r['mismatch'] == 0 for r in ds)
        if impl[fid] == 0: status = 'не начата'
        elif impl[fid] == 2: status = 'частично'
        elif sp_ok or dense_ok: status = '100 %'
        elif sp_iso: status = '100 % в одиночных окнах'
        elif sp_noise or (ds and eff_any): status = 'шум порядка'
        elif ds or ss: status = 'нет эффекта'
        cnt[status] += 1
        rows.append(f'| `{n}` | `{top}` | {",".join(sorted(dims[fid]))} | {status} | {"; ".join(parts) if parts else "—"} |')
    print(f'Группа растительности ({v}): {len(rows)} placed_feature; ' + ', '.join(f'{k}: {c}' for k, c in cnt.most_common()) + '\n')
    print('| placed_feature | тип | изм. | статус | эталоны |\n|---|---|---|---|---|')
    print('\n'.join(rows))


if __name__ == '__main__':
    main()
