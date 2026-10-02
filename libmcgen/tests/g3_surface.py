#!/usr/bin/env python3
"""Ворота G3 (поверхность): libmcgen со стадиями BIOMES+TERRAIN+SURFACE против эталонных миров ванильного сервера (поток W6, run/gt/<V>/surface/*).

Эталон `surface` — карверы, декорации и постройки отключены датапаком, правила материала настоящие (бедрок, глубинный сланец, трава,
бэдленды, лёд, жилы руд). Область радиуса r + кольца, которые игра догенерирует сама (r+1 — тикающие, r+2 — FULL, r+3, r+4 — ниже FULL);
растекание жидкостей игра выполняет только в тикающих чанках, поэтому libmcgen генерирует r+M чанков с --pp-margin M-1 (как в G2).
Сравнение блок-в-блок по ВСЕМ чанкам дампа, БЕЗ масок (расширения бэдлендов/айсбергов — часть стадии SURFACE), плюс карты высот и биомы.

    python3 libmcgen/tests/g3_surface.py --version 26.3 [--only overworld] [--seeds 12345] [--report g3-26.3.json] [--cli path/to/mcgen-cli]
"""
import argparse, glob, json, os, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'gt'))
import diff  # noqa: E402  (tools/gt/diff.py, поток W6)

DIM_FULL = {'overworld': 'minecraft:overworld', 'nether': 'minecraft:the_nether', 'end': 'minecraft:the_end', 'the_nether': 'minecraft:the_nether', 'the_end': 'minecraft:the_end'}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--version', default='26.3')
    ap.add_argument('--variant', default='surface')
    ap.add_argument('--margin', type=int, default=4, help='сколько колец вокруг области эталона генерировать (кольца игры r+1..r+4)')
    ap.add_argument('--threads', type=int, default=0)
    ap.add_argument('--only', default='', help='подстрока имени эталона')
    ap.add_argument('--seeds', default='', help='seed через запятую')
    ap.add_argument('--stages', default='0x7')
    ap.add_argument('--cli', default=os.path.join(ROOT, 'libmcgen', 'build', 'mcgen-cli'))
    ap.add_argument('--extra', default='', help='доп. аргументы mcgen-cli (строкой)')
    ap.add_argument('--keep', action='store_true', help='не удалять дампы .mcr')
    ap.add_argument('--report')
    ap.add_argument('--tmp', default=os.environ.get('TMPDIR', '/tmp'))
    ap.add_argument('--top', type=int, default=5)
    a = ap.parse_args()
    seeds = {int(s) for s in a.seeds.split(',') if s}
    rows = []
    t0 = time.time()
    for ref in sorted(glob.glob(os.path.join(ROOT, 'run', 'gt', a.version, a.variant, '*'))):
        if a.only and a.only not in ref:
            continue
        mf = os.path.join(ref, 'manifest.json')
        if not os.path.exists(mf):
            continue
        m = json.load(open(mf))
        if not m.get('ok', True):
            continue
        dim, seed = m['dim'], m['seed']
        if seeds and seed not in seeds:
            continue
        x0, z0, x1, z1 = m['area_chunks']
        M = a.margin
        name = os.path.basename(ref)
        out = os.path.join(a.tmp, f'g3-{a.version}-{name}.mcr')
        cmd = [a.cli, '--pack', os.path.join(ROOT, 'run', f'pack-{a.version}'), '--version', a.version, '--dim', DIM_FULL[dim],
               '--preset', m.get('preset', 'normal'), '--seed', str(seed), '--cx0', str(x0 - M), '--cz0', str(z0 - M),
               '--nx', str(x1 - x0 + 1 + 2 * M), '--nz', str(z1 - z0 + 1 + 2 * M), '--stages', a.stages, '--threads', str(a.threads),
               '--pp-margin', str(max(0, M - 1)), '--out', out] + a.extra.split()
        tg = time.time()
        p = subprocess.run(cmd, capture_output=True, text=True)
        gen_s = time.time() - tg
        row = {'ref': os.path.relpath(ref, ROOT), 'dim': dim, 'seed': seed, 'gen_s': round(gen_s, 2)}
        if p.returncode:
            row['error'] = p.stderr[-300:]
            print(f'{name}: ошибка генерации {row["error"]}', flush=True)
            rows.append(row)
            continue
        R, _ = diff.run(ref, out, dim=dim, version=a.version, mask_ext=False, mask_flow=False, allow_missing=True, top=a.top,
                        min_status='minecraft:terrain', biomes=True, heightmaps=True)
        if not a.keep:
            os.remove(out)
        hm = {k: v['mismatch'] for k, v in R.get('heightmaps', {}).items()}
        row.update({'chunks': R['chunks_compared'], 'blocks': R['blocks_compared'], 'mismatch': R['blocks_mismatch'],
                    'full': R['full'], 'pure': R['pure'], 'heightmaps': hm,
                    'biome_cells': R['biomes']['cells_compared'], 'biome_mismatch': R['biomes']['cells_mismatch'], 'top': R.get('top_pairs', [])[:a.top]})
        rows.append(row)
        print(f'{name:48s} чанков {row["chunks"]:4d} (full {R["full"]["chunks"]}) расх. блоков {row["mismatch"]:6d}, карт высот {sum(hm.values()):4d}, '
              f'биомов {row["biome_mismatch"]}/{row["biome_cells"]} | {gen_s:.1f} с'
              + ('' if not row['mismatch'] else '  ' + json.dumps(row['top'][:3], ensure_ascii=False)), flush=True)
    ok = [r for r in rows if 'error' not in r]
    S = lambda k: sum(r[k] for r in ok)
    seeds = sorted({r['seed'] for r in ok}); dims = sorted({r['dim'] for r in ok})
    hm_bad = sum(sum(r['heightmaps'].values()) for r in ok)
    print(f'\nИТОГО G3 {a.version}: эталонов {len(ok)} (ошибок {len(rows) - len(ok)}), чанков {S("chunks")} (full {sum(r["full"]["chunks"] for r in ok)}, '
          f'ниже full {sum(r["pure"]["chunks"] for r in ok)}), блоков {S("blocks"):,}, расхождений {S("mismatch")} -> '
          f'{100.0 * (S("blocks") - S("mismatch")) / max(1, S("blocks")):.6f} %; карт высот расх. {hm_bad}; seed {seeds}; измерения {dims}; '
          f'биомы {S("biome_mismatch")}/{S("biome_cells")} клеток; {time.time() - t0:.0f} с')
    if a.report:
        json.dump({'version': a.version, 'margin': a.margin, 'rows': rows, 'chunks': S('chunks'), 'blocks': S('blocks'), 'mismatch': S('mismatch'),
                   'heightmap_mismatch': hm_bad, 'biome_cells': S('biome_cells'), 'biome_mismatch': S('biome_mismatch')},
                  open(a.report, 'w'), indent=1, ensure_ascii=False)
    return 0 if S('mismatch') == 0 and len(ok) == len(rows) else 1


if __name__ == '__main__':
    sys.exit(main())
