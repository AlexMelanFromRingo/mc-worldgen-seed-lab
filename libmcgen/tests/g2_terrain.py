#!/usr/bin/env python3
"""Ворота G2 (заполнение шумом): libmcgen против эталонных миров ванильного сервера (поток W6, run/gt/<V>/<вариант>/*).

Эталон W6 — область радиуса r вокруг центра (forceload, уровень ENTITY_TICKING) + кольца, которые игра догенерирует сама:
r+1 — BLOCK_TICKING, r+2 — FULL (не тикает), r+3, r+4 — статусы ниже full. Растекание жидкостей
(LevelChunk.postProcessGeneration) игра выполняет в ChunkMap.prepareTickingChunk, т. е. ТОЛЬКО в тикающих чанках
(кольца 0..r+1). libmcgen генерирует область r+M и повторяет это: --pp-margin M-1 (растекание только в чанках не ближе
M-1 к краю дампа). Сравнение блок-в-блок по ВСЕМ чанкам дампа, БЕЗ масок жидкостей, плюс карты высот и биомы.
Маскируются только столбцы «расширений» бесплодных земель и айсбергов (--mask-ext) — это код системы материалов (стадия
поверхности), выполняемый игрой при любом правиле материала.
  вариант raw   : ore_veins=0 (в 26.3+ жилы — правила материала, в raw их нет; в 26.1/26.2 эталон raw — с выключенными жилами)
  вариант veins : жилы включены
Биомы клеток сравниваются тоже (ничьи R-дерева — см. docs/blender/terrain.md).

    python3 libmcgen/tests/g2_terrain.py --version 26.3 [--variants raw veins] [--margin 4] [--report g2-26.3.json]
"""
import argparse, glob, json, os, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
CLI = os.path.join(ROOT, 'libmcgen', 'build', 'mcgen-cli')
sys.path.insert(0, os.path.join(ROOT, 'tools', 'gt'))
import diff  # noqa: E402  (tools/gt/diff.py, поток W6)

DIM_FULL = {'overworld': 'minecraft:overworld', 'nether': 'minecraft:the_nether', 'end': 'minecraft:the_end', 'the_nether': 'minecraft:the_nether', 'the_end': 'minecraft:the_end'}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--version', default='26.3')
    ap.add_argument('--variants', nargs='+', default=['raw', 'veins'])
    ap.add_argument('--margin', type=int, default=4, help='сколько колец вокруг области эталона генерировать (кольца игры r+1..r+4)')
    ap.add_argument('--threads', type=int, default=0)
    ap.add_argument('--only', default='', help='подстрока имени эталона')
    ap.add_argument('--report')
    ap.add_argument('--tmp', default=os.environ.get('TMPDIR', '/tmp'))
    a = ap.parse_args()
    rows = []
    t0 = time.time()
    for variant in a.variants:
        for ref in sorted(glob.glob(os.path.join(ROOT, 'run', 'gt', a.version, variant, '*'))):
            if a.only and a.only not in ref:
                continue
            mf = os.path.join(ref, 'manifest.json')
            if not os.path.exists(mf):
                continue
            m = json.load(open(mf))
            if not m.get('ok', True):
                continue
            dim, seed = m['dim'], m['seed']
            x0, z0, x1, z1 = m['area_chunks']
            M = a.margin
            name = os.path.basename(ref)
            out = os.path.join(a.tmp, f'g2-{variant}-{name}.mcr')
            cmd = [CLI, '--pack', os.path.join(ROOT, 'run', f'pack-{a.version}'), '--version', a.version, '--dim', DIM_FULL[dim],
                   '--preset', m.get('preset', 'normal'), '--seed', str(seed), '--cx0', str(x0 - M), '--cz0', str(z0 - M),
                   '--nx', str(x1 - x0 + 1 + 2 * M), '--nz', str(z1 - z0 + 1 + 2 * M), '--stages', '0x3', '--threads', str(a.threads),
                   '--pp-margin', str(max(0, M - 1)), '--out', out]
            if variant == 'raw':
                cmd += ['--tweak', 'ore_veins=0']
            tg = time.time()
            p = subprocess.run(cmd, capture_output=True, text=True)
            gen_s = time.time() - tg
            row = {'ref': os.path.relpath(ref, ROOT), 'variant': variant, 'dim': dim, 'seed': seed, 'gen_s': round(gen_s, 2)}
            if p.returncode:
                row['error'] = p.stderr[-300:]
                print(f'{variant} {name}: ошибка генерации {row["error"]}', flush=True)
                rows.append(row)
                continue
            R, _ = diff.run(ref, out, dim=dim, version=a.version, mask_ext=True, mask_flow=False, allow_missing=True, top=5,
                            min_status='minecraft:terrain', biomes=True, heightmaps=True)
            os.remove(out)
            hm = {k: v['mismatch'] for k, v in R.get('heightmaps', {}).items()}
            row.update({'chunks': R['chunks_compared'], 'blocks': R['blocks_compared'], 'mismatch': R['blocks_mismatch'],
                        'full': R['full'], 'pure': R['pure'], 'ext_columns_masked': R.get('ext_columns_masked', 0), 'heightmaps': hm,
                        'biome_cells': R['biomes']['cells_compared'], 'biome_mismatch': R['biomes']['cells_mismatch'], 'top': R.get('top_pairs', [])[:3]})
            rows.append(row)
            print(f'{variant:6s} {name:46s} чанков {row["chunks"]:4d} (full {R["full"]["chunks"]}, ниже full {R["pure"]["chunks"]}) '
                  f'расх. блоков {row["mismatch"]:3d}, карт высот {sum(hm.values())}, биомов {row["biome_mismatch"]}/{row["biome_cells"]} | ген. {gen_s:.1f} с'
                  + ('' if not row['mismatch'] else '  ' + json.dumps(row['top'], ensure_ascii=False)), flush=True)
    ok = [r for r in rows if 'error' not in r]
    S = lambda k: sum(r[k] for r in ok)
    seeds = sorted({r['seed'] for r in ok}); dims = sorted({r['dim'] for r in ok})
    hm_bad = sum(sum(r['heightmaps'].values()) for r in ok)
    print(f'\nИТОГО G2 {a.version}: эталонов {len(ok)} (ошибок {len(rows) - len(ok)}), чанков {S("chunks")} (full {sum(r["full"]["chunks"] for r in ok)}, '
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
