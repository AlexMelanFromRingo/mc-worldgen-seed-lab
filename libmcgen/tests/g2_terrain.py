#!/usr/bin/env python3
"""Ворота G2 (заполнение шумом): libmcgen против эталонных миров ванильного сервера (поток W6, run/gt/<V>/<вариант>/*).

Эталон W6 — область радиуса r вокруг центра + кольца, которые игра догенерирует сама: r+1, r+2 — статус full (прошли
LevelChunk.postProcessGeneration: растекание жидкостей), r+3, r+4 — статус ниже full («чистое» заполнение шумом).
libmcgen генерирует область (r+4) дважды и сравнивает блок-в-блок БЕЗ масок жидкостей:
  full   — fluid_flow=1 (растекание как при переходе в FULL), чанки колец 0..r+1, статус эталона full;
  pure   — fluid_flow=0 («чистое» заполнение), чанки колец r+3, r+4 (статус эталона ниже full);
  border — fluid_flow=1, кольцо r+2 (full, но соседи в эталоне не full): справочно, расхождения здесь — эффект границы
           эталона (у нас растекание выполнено во ВСЕХ чанках, в эталоне соседние чанки r+3 его не проходили и не
           влили воду через границу), в итог ворот не входят.
Маскируются только столбцы «расширений» бесплодных земель и айсбергов (--mask-ext) — это код системы материалов (стадия
поверхности), выполняемый игрой при любом правиле материала.
  вариант raw   : ore_veins=0 (в 26.3+ жилы — правила материала, в raw их нет; в 26.1/26.2 эталон raw — с выключенными жилами)
  вариант veins : жилы включены
Биомы клеток сравниваются по всей области (ничьи R-дерева в самой игре недетерминированы — см. docs/blender/terrain.md).

    python3 libmcgen/tests/g2_terrain.py --version 26.3 [--variants raw veins] [--margin 4] [--report g2-26.3.json]
"""
import argparse, glob, json, os, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
CLI = os.path.join(ROOT, 'libmcgen', 'build', 'mcgen-cli')
sys.path.insert(0, os.path.join(ROOT, 'tools', 'gt'))
import diff  # noqa: E402  (tools/gt/diff.py, поток W6)

DIM_FULL = {'overworld': 'minecraft:overworld', 'nether': 'minecraft:the_nether', 'end': 'minecraft:the_end', 'the_nether': 'minecraft:the_nether', 'the_end': 'minecraft:the_end'}


def gen(a, m, dim, seed, cx0, cz0, n, variant, ff, out):
    cmd = [CLI, '--pack', os.path.join(ROOT, 'run', f'pack-{a.version}'), '--version', a.version, '--dim', DIM_FULL[dim], '--preset', m.get('preset', 'normal'),
           '--seed', str(seed), '--cx0', str(cx0), '--cz0', str(cz0), '--nx', str(n), '--nz', str(n), '--stages', '0x3', '--threads', str(a.threads),
           '--out', out, '--tweak', f'fluid_flow={ff}']
    if variant == 'raw':
        cmd += ['--tweak', 'ore_veins=0']
    t = time.time()
    p = subprocess.run(cmd, capture_output=True, text=True)
    return p.returncode, time.time() - t, p.stderr[-300:]


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
            cx0, cz0, n = x0 - M, z0 - M, x1 - x0 + 1 + 2 * M
            name = os.path.basename(ref)
            row = {'ref': os.path.relpath(ref, ROOT), 'variant': variant, 'dim': dim, 'seed': seed}
            outs = {}
            for ff in (1, 0):
                out = os.path.join(a.tmp, f'g2-{variant}-{name}-ff{ff}.mcr')
                rc, dt, msg = gen(a, m, dim, seed, cx0, cz0, n, variant, ff, out)
                row[f'gen_s_ff{ff}'] = round(dt, 2)
                if rc:
                    row['error'] = msg
                    break
                outs[ff] = out
            if 'error' in row:
                print(f'{variant} {name}: ошибка генерации {row["error"]}', flush=True)
                rows.append(row)
                continue
            kw = dict(dim=dim, version=a.version, mask_ext=True, mask_flow=False, allow_missing=True, top=5)
            # full: кольца 0..r+1, растекание включено
            Rf, _ = diff.run(ref, outs[1], min_status='minecraft:full', region=(x0 - 1, z0 - 1, x1 - x0 + 3, z1 - z0 + 3), **kw)
            # full + кольцо r+2 (граница) — для справки
            Rb, _ = diff.run(ref, outs[1], min_status='minecraft:full', region=(x0 - 2, z0 - 2, x1 - x0 + 5, z1 - z0 + 5), **kw)
            # pure: чанки эталона со статусом ниже full, растекание выключено; заодно биомы всей области
            Rp, _ = diff.run(ref, outs[0], min_status='minecraft:terrain', biomes=True, **kw)
            row.update({
                'full_chunks': Rf['full']['chunks'], 'full_blocks': Rf['full']['blocks'], 'full_mismatch': Rf['full']['mismatch'],
                'pure_chunks': Rp['pure']['chunks'], 'pure_blocks': Rp['pure']['blocks'], 'pure_mismatch': Rp['pure']['mismatch'],
                'border_chunks': Rb['full']['chunks'] - Rf['full']['chunks'], 'border_blocks': Rb['full']['blocks'] - Rf['full']['blocks'],
                'border_mismatch': Rb['full']['mismatch'] - Rf['full']['mismatch'],
                'masked_ext_columns': Rf.get('ext_columns_masked', 0) + Rp.get('ext_columns_masked', 0),
                'biome_cells': Rp['biomes']['cells_compared'], 'biome_mismatch': Rp['biomes']['cells_mismatch'],
                'top_full': Rf.get('top_pairs', [])[:3], 'top_pure': Rp.get('top_pairs', [])[:3], 'top_border': Rb.get('top_pairs', [])[:3]})
            rows.append(row)
            print(f'{variant:6s} {name:42s} full {row["full_chunks"]:4d} ч. расх. {row["full_mismatch"]:3d} | pure {row["pure_chunks"]:4d} ч. '
                  f'расх. {row["pure_mismatch"]:3d} | граница {row["border_chunks"]:3d} ч. расх. {row["border_mismatch"]:3d} | '
                  f'биомы {row["biome_mismatch"]}/{row["biome_cells"]} | ген. {row["gen_s_ff1"]:.1f}+{row["gen_s_ff0"]:.1f} с'
                  + ('' if not (row['full_mismatch'] or row['pure_mismatch']) else '  ' + json.dumps(row['top_full'] + row['top_pure'], ensure_ascii=False)),
                  flush=True)
            for o in outs.values():
                os.remove(o)
    ok = [r for r in rows if 'error' not in r]
    S = lambda k: sum(r[k] for r in ok)
    gate_chunks, gate_blocks, gate_bad = S('full_chunks') + S('pure_chunks'), S('full_blocks') + S('pure_blocks'), S('full_mismatch') + S('pure_mismatch')
    seeds = sorted({r['seed'] for r in ok}); dims = sorted({r['dim'] for r in ok})
    print(f'\nИТОГО G2 {a.version}: эталонов {len(ok)} (ошибок {len(rows) - len(ok)}); full: чанков {S("full_chunks")}, блоков {S("full_blocks"):,}, '
          f'расх. {S("full_mismatch")}; pure: чанков {S("pure_chunks")}, блоков {S("pure_blocks"):,}, расх. {S("pure_mismatch")}; '
          f'ворота: {gate_chunks} чанков, {gate_blocks:,} блоков, расх. {gate_bad} -> {100.0 * (gate_blocks - gate_bad) / max(1, gate_blocks):.6f} %; '
          f'граница (справочно): чанков {S("border_chunks")}, расх. {S("border_mismatch")}; seed {seeds}; измерения {dims}; '
          f'биомы {S("biome_mismatch")}/{S("biome_cells")} клеток расходятся; {time.time() - t0:.0f} с')
    if a.report:
        json.dump({'version': a.version, 'margin': a.margin, 'rows': rows, 'gate_chunks': gate_chunks, 'gate_blocks': gate_blocks, 'gate_mismatch': gate_bad,
                   'border_chunks': S('border_chunks'), 'border_mismatch': S('border_mismatch')}, open(a.report, 'w'), indent=1, ensure_ascii=False)
    return 0 if gate_bad == 0 and len(ok) == len(rows) else 1


if __name__ == '__main__':
    sys.exit(main())
