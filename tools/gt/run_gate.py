#!/usr/bin/env python3
"""Прогон ворот G2…G6: libmcgen (mcgen-cli или ctypes) против эталонных миров ванильного сервера, таблица -> docs/blender/accuracy.md.

  tools/gt/run_gate.py --gate G2                          # вся матрица профиля land (ocean/mountains/desert/jungle + nether + end)
  tools/gt/run_gate.py --gate G2 --profile core --seeds 12345 --dims overworld --no-doc
  tools/gt/run_gate.py --gate G3 --gen-missing            # недостающие эталоны сгенерировать (под flock)
  tools/gt/run_gate.py --list

Область дампа = запрошенная область эталона + margin чанков (G2–G4: 4, G5–G6: 2). Игра сама генерирует вокруг forceload-области кольца чанков:
r+1, r+2 — full; r+3 — initialize_light; r+4 — terrain. Чанки со статусом ниже full («чистые») не проходили PostProcessing (растекание жидкости),
поэтому для G2–G4 сравниваются и они (строго, без масок жидкости), а для full-чанков растекание маскируется (diff.py --help). Для G5/G6 берутся только full.

Ворота (gates): какой вариант эталона, какие стадии просить у библиотеки, какие маски и порог:
  G2   raw      stages BIOMES|TERRAIN (0x3)   --tweak ore_veins=0 (в 26.3 жилы — в material_rule, в raw их нет); маска ext-биомы
                                              (--veins-mask: вместо твика маскировать состояния жил: filler/ore/raw из material_rule)
  G2v  veins    stages 0x3                    + жилы руды; маска только ext-биомы
  G3   surface  stages 0x7                    настоящие правила материала (бедрок, сланец, трава, бэдленды, лёд)
  G4   carvers  stages 0xf                    + карверы (к surface)
  G4c  carve_raw stages 0xb (БЕЗ SURFACE)     карверы без поверхности (как raw: --tweak ore_veins=0, маска ext-биомов) — для проверки карверов отдельно
  G5   features stages 0x1f                   + все декорации; порог 99.9 % (изолированные фичи — feature:<id>, --gate G5i --feature ID)
  G6   full     stages 0x3f                   + постройки; порог 99.9 %
Библиотека: --cli libmcgen/build/mcgen-cli (по умолчанию), либо --lib libmcgen/build/libmcgen.so (ctypes). Если нет ни того, ни другого —
код возврата 3. Коды: 0 — все строки PASS, 1 — есть FAIL, 2 — ошибки (нет эталона/дампа), 3 — библиотеки нет.
Секция «## <ворота>» в docs/blender/accuracy.md пишется между маркерами <!-- gate:G2 begin --> / <!-- gate:G2 end --> (остальное не трогается).
"""
import argparse, ctypes, json, os, subprocess, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from common import ROOT, GT, VERSIONS, DIMS, DIM_SHORT, pack_dir
import gen_world, gen_queue as gtq, diff, datapack

DEFAULT_CLI = f'{ROOT}/libmcgen/build/mcgen-cli'
DEFAULT_LIB = f'{ROOT}/libmcgen/build/libmcgen.so'
ACCURACY = f'{ROOT}/docs/blender/accuracy.md'
DIM_FULL = {'overworld': 'minecraft:overworld', 'nether': 'minecraft:the_nether', 'end': 'minecraft:the_end'}

GATES = {
    'G2': dict(title='G2: заполнение шумом (raw)', variant='raw', stages=0x3, mask_ext=True, mask_veins=False, tweaks={'ore_veins': 0}, min_match=100.0,
               margin=4, min_status='minecraft:terrain'),
    'G2v': dict(title='G2v: заполнение шумом + жилы руды', variant='veins', stages=0x3, mask_ext=True, mask_veins=False, min_match=100.0,
                margin=4, min_status='minecraft:terrain'),
    'G3': dict(title='G3: + правила материала/поверхности', variant='surface', stages=0x7, mask_ext=False, mask_veins=False, min_match=100.0,
               margin=4, min_status='minecraft:terrain'),
    'G4': dict(title='G4: + карверы', variant='carvers', stages=0xf, mask_ext=False, mask_veins=False, min_match=100.0,
               margin=4, min_status='minecraft:terrain'),
    'G4c': dict(title='G4c: карверы без поверхности (carve_raw)', variant='carve_raw', stages=0xb, mask_ext=True, mask_veins=False, tweaks={'ore_veins': 0},
                min_match=100.0, margin=4, min_status='minecraft:terrain'),
    'G5': dict(title='G5: + декорации (все вместе)', variant='features', stages=0x1f, mask_ext=False, mask_veins=False, min_match=99.9,
               margin=2, min_status='minecraft:full'),
    'G6': dict(title='G6: + постройки (полная ваниль)', variant='full', stages=0x3f, mask_ext=False, mask_veins=False, min_match=99.9,
               margin=2, min_status='minecraft:full'),
}


def vein_states(version):
    """Состояния жил руды из material_rule пак-каталога (filler/ore/raw_ore + deepslate-вариант руды)."""
    out = set()
    d = f'{pack_dir(version)}/data/minecraft/worldgen/material_rule/overworld'
    if os.path.isdir(d):
        for fn in os.listdir(d):
            if fn.endswith('_ore_vein.json'):
                r = json.load(open(f'{d}/{fn}'))
                for k in ('filler_block', 'ore_block', 'raw_ore_block'):
                    if k in r:
                        out.add(r[k])
                        n = r[k].split(':')[-1]
                        if n.endswith('_ore') and not n.startswith('deepslate_'):
                            out.add('minecraft:deepslate_' + n)
    else:  # 26.1/26.2: OreVeinifier зашит в коде
        out |= {'minecraft:granite', 'minecraft:tuff', 'minecraft:copper_ore', 'minecraft:deepslate_copper_ore', 'minecraft:raw_copper_block',
                'minecraft:iron_ore', 'minecraft:deepslate_iron_ore', 'minecraft:raw_iron_block'}
    return sorted(out)


class Lib:
    """ctypes-обёртка libmcgen (альтернатива CLI)."""

    def __init__(self, so, pack, version):
        self.L = ctypes.CDLL(so)
        L = self.L
        L.mcgen_open.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.POINTER(ctypes.c_void_p), ctypes.c_char_p, ctypes.c_size_t]
        L.mcgen_world_new.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int,
                                      ctypes.POINTER(ctypes.c_void_p), ctypes.c_char_p, ctypes.c_size_t]
        L.mcgen_generate_region.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_uint32, ctypes.c_int,
                                            ctypes.c_void_p, ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p), ctypes.c_char_p, ctypes.c_size_t]
        L.mcgen_region_write_mcr.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_size_t]
        err = ctypes.create_string_buffer(512)
        self.g = ctypes.c_void_p()
        if L.mcgen_open(pack.encode(), version.encode(), ctypes.byref(self.g), err, 512):
            raise RuntimeError(err.value.decode())

    class TweakValue(ctypes.Structure):
        _fields_ = [('id', ctypes.c_char_p), ('value', ctypes.c_double)]

    def dump(self, dim, seed, cx0, cz0, nx, nz, stages, out, tweaks=None):
        err = ctypes.create_string_buffer(512)
        seeds = (ctypes.c_int64 * 4)(seed, seed, seed, seed)
        w, r = ctypes.c_void_p(), ctypes.c_void_p()
        tw = (self.TweakValue * max(1, len(tweaks or {})))(*[self.TweakValue(k.encode(), float(v)) for k, v in (tweaks or {}).items()])
        if self.L.mcgen_world_new(self.g, dim.encode(), b'normal', seeds, tw if tweaks else None, len(tweaks or {}), ctypes.byref(w), err, 512):
            raise RuntimeError(err.value.decode())
        if self.L.mcgen_generate_region(w, cx0, cz0, nx, nz, stages, 0, None, None, ctypes.byref(r), err, 512):
            raise RuntimeError(err.value.decode())
        if self.L.mcgen_region_write_mcr(r, self.g, out.encode(), err, 512):
            raise RuntimeError(err.value.decode())
        self.L.mcgen_region_free(r); self.L.mcgen_world_free(w)


def run_cli(cli, version, dim, seed, cx0, cz0, nx, nz, stages, out, threads=0, extra=()):
    cmd = [cli, '--pack', pack_dir(version), '--version', version, '--dim', DIM_FULL[dim], '--preset', 'normal', '--seed', str(seed),
           '--cx0', str(cx0), '--cz0', str(cz0), '--nx', str(nx), '--nz', str(nz), '--stages', hex(stages), '--threads', str(threads), '--out', out, *extra]
    t = time.monotonic()
    p = subprocess.run(cmd, capture_output=True, text=True)
    return p.returncode, time.monotonic() - t, (p.stderr or p.stdout)[-600:]


def replace_section(path, gate, body):
    b, e = f'<!-- gate:{gate} begin -->', f'<!-- gate:{gate} end -->'
    txt = open(path).read() if os.path.exists(path) else '# Точность libmcgen относительно настоящего сервера\n\nСекции ворот пишет `tools/gt/run_gate.py` (между маркерами).\n'
    block = f'{b}\n{body.rstrip()}\n{e}'
    if b in txt and e in txt:
        i, j = txt.index(b), txt.index(e) + len(e)
        txt = txt[:i] + block + txt[j:]
    else:
        txt = txt.rstrip() + '\n\n' + block + '\n'
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + '.tmp'
    open(tmp, 'w').write(txt)
    os.replace(tmp, path)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--gate', choices=sorted(GATES) + ['all'])
    ap.add_argument('--list', action='store_true')
    ap.add_argument('--version', default='26.3', choices=VERSIONS)
    ap.add_argument('--cli', default=DEFAULT_CLI)
    ap.add_argument('--lib', default=None, help='ctypes вместо CLI: путь к libmcgen.so')
    ap.add_argument('--profile', default='land', choices=['core', 'land', 'all'])
    ap.add_argument('--seeds', type=lambda s: [int(x) for x in s.split(',')], default=None)
    ap.add_argument('--dims', default=None)
    ap.add_argument('--gen-missing', action='store_true', help='недостающие эталоны сгенерировать (flock)')
    ap.add_argument('--no-doc', action='store_true', help='не писать accuracy.md')
    ap.add_argument('--accuracy', default=ACCURACY)
    ap.add_argument('--stages', type=lambda s: int(s, 0), default=None, help='переопределить маску стадий')
    ap.add_argument('--variant', default=None, help='переопределить вариант эталона')
    ap.add_argument('--mask-flow', action='store_true', help='маскировать растекание жидкостей (для библиотек без --pp-margin); по умолчанию — строго, с --pp-margin margin-1')
    ap.add_argument('--veins-mask', action='store_true', help='G2: вместо --tweak ore_veins=0 маскировать состояния жил руды')
    ap.add_argument('--png-dir', default=None, help='сохранять PNG-карты расхождений для FAIL-строк')
    ap.add_argument('--margin', type=int, default=None, help='переопределить margin (чанков вокруг области эталона)')
    ap.add_argument('--area-only', action='store_true', help='G5/G6: сравнивать только запрошенную область, без внешних колец дампа (r+1…r+2); по умолчанию сравниваются и кольца (строже)')
    ap.add_argument('--threads', type=int, default=0)
    ap.add_argument('--top', type=int, default=5)
    a = ap.parse_args()
    if a.list:
        for k, g in GATES.items():
            print(f'{k:4s} {g["title"]:46s} вариант {g["variant"]:9s} стадии 0x{g["stages"]:x} порог {g["min_match"]} %')
        return
    if not a.gate:
        ap.error('--gate обязателен')
    if a.gate == 'all':           # все ворота подряд (каждый — отдельным процессом, свой код возврата)
        rc = {}
        for g in GATES:
            if g in ('G5', 'G6') and not a.profile in ('core', 'land', 'all'):
                continue
            r = subprocess.run([sys.executable, os.path.abspath(__file__)] + [x for x in sys.argv[1:] if x not in ('--gate', 'all')] + ['--gate', g])
            rc[g] = r.returncode
        print('\nКоды возврата ворот:', rc)
        sys.exit(max(rc.values()) if rc else 0)
    G = dict(GATES[a.gate])
    if a.stages is not None: G['stages'] = a.stages
    if a.variant: G['variant'] = a.variant
    lib = None
    if a.lib:
        lib = Lib(a.lib, pack_dir(a.version), a.version)
    elif not os.path.exists(a.cli):
        if a.cli == DEFAULT_CLI and os.path.exists(DEFAULT_LIB):
            lib = Lib(DEFAULT_LIB, pack_dir(a.version), a.version)
        else:
            print(f'libmcgen не найдена: нет {a.cli}' + (f' и {DEFAULT_LIB}' if a.cli == DEFAULT_CLI else '') + '; соберите `make -C libmcgen` (поток W1)', file=sys.stderr)
            sys.exit(3)
    cfgs = gtq.configs(a.profile, a.seeds, a.dims.split(',') if a.dims else None)
    if G['variant'] == 'veins':       # в Nether/End жил руды нет (veins == raw)
        cfgs = [c for c in cfgs if c['dim'] == 'overworld']
    rows, errors, any_fail = [], 0, False
    dumpdir = f'{GT}/dumps/{a.version}/{a.gate}'
    os.makedirs(dumpdir, exist_ok=True)
    if a.veins_mask:
        G['mask_veins'], G['tweaks'] = True, {}
    ign = vein_states(a.version) if G['mask_veins'] else []
    tweaks = G.get('tweaks') or {}
    extra = [x for k, v in tweaks.items() for x in ('--tweak', f'{k}={v}')]
    for c in cfgs:
        wd = gen_world.world_dir(a.version, G['variant'], c['dim'], c['seed'], c['cx'], c['cz'], c['radius'])
        name = os.path.basename(wd)
        row = {'seed': c['seed'], 'dim': c['dim'], 'label': c['label'], 'center': (c['cx'], c['cz']), 'name': name}
        mp = f'{wd}/manifest.json'
        if not (os.path.exists(mp) and json.load(open(mp)).get('ok')):
            if a.gen_missing:
                gen_world.generate(a.version, G['variant'], c['dim'], c['seed'], c['cx'], c['cz'], c['radius'])
            else:
                row['status'] = 'нет эталона'; errors += 1; rows.append(row)
                print(f'{name}: нет эталона ({wd}); --gen-missing', flush=True)
                continue
        mg = a.margin if a.margin is not None else G.get('margin', 0)
        rr = c['radius'] + mg
        # растекание жидкостей игра делает только в чанках, у которых все 8 соседей FULL (кольца эталона 0..r+1): CLI --pp-margin K (K = margin-1)
        extra_c = extra + ([] if (a.mask_flow or lib) else ['--pp-margin', str(max(mg - 1, 0))])
        strict = not (a.mask_flow or lib)
        cx0, cz0, n = c['cx'] - rr, c['cz'] - rr, 2 * rr + 1
        dump = f'{dumpdir}/{name}.mcr'
        # область тикета forceload эталона = центр ± радиус (регион дампа шире на margin колец): libmcgen декорирует чанки дальше 2 колец от неё ПОСЛЕ остальных
        # (так ведёт себя игра: полные чанки — до r+2 в порядке x, z, внешнее кольцо r+3 — позже; измерено: Overworld s12345 99,891 → 99,910 %), см. docs/blender/features-misc.md §3.6
        os.environ['MCGEN_FEATURES_AREA'] = f"{c['cx'] - c['radius']},{c['cz'] - c['radius']},{c['cx'] + c['radius']},{c['cz'] + c['radius']}"
        t = time.monotonic()
        try:
            if lib:
                lib.dump(DIM_FULL[c['dim']], c['seed'], cx0, cz0, n, n, G['stages'], dump, tweaks); rc, msg = 0, ''
            else:
                rc, _, msg = run_cli(a.cli, a.version, c['dim'], c['seed'], cx0, cz0, n, n, G['stages'], dump, a.threads,
                                     list(extra) + ['--pp-margin', str(max(0, mg - 1))])   # растекание только в тикающих чанках игры (G2–G4: margin−1)
        except Exception as e:
            rc, msg = 1, f'{type(e).__name__}: {e}'
        row['gen_s'] = round(time.monotonic() - t, 1)
        if rc or not os.path.exists(dump):
            row['status'] = f'ОШИБКА генерации (код {rc}): {msg.strip()[-200:]}'; errors += 1; rows.append(row)
            print(f'{name}: {row["status"]}', flush=True)
            continue
        try:
            # G5/G6 по умолчанию сравнивают весь дамп (область + кольца r+1…r+2); --area-only — только запрошенную область
            cmp_margin = mg if (G['variant'] in ('features', 'full') and a.area_only) else 0
            R, o = diff.run(wd, dump, dim=c['dim'], version=a.version, ignore_state=ign, mask_ext=G['mask_ext'], min_match=G['min_match'], margin=cmp_margin,
                            min_status=G.get('min_status', 'minecraft:full'), biomes=True, heightmaps=True, top=a.top, list=5,
                            mask_flow=not strict, stable_with=[w for w in (f'{wd}_rep1', f'{wd}_rep2') if os.path.isdir(w)])
        except Exception as e:
            row['status'] = f'ОШИБКА сравнения: {type(e).__name__}: {e}'; errors += 1; rows.append(row)
            print(f'{name}: {row["status"]}', flush=True)
            continue
        row.update({k: R[k] for k in ('chunks_compared', 'blocks_compared', 'blocks_mismatch', 'match_pct', 'verdict', 'ext_columns_masked', 'chunks_mismatching',
                                      'pure', 'full', 'blocks_flow_induced', 'blocks_masked_flow')})
        row['samples'] = R.get('samples', [])
        row['biomes_pct'] = R['biomes']['match_pct']
        row['hm_bad'] = sum(v['mismatch'] for v in R['heightmaps'].values())
        row['top'] = R['top_pairs'][:3]
        row['status'] = R['verdict'].upper()
        if R['verdict'] != 'pass':
            any_fail = True
            if a.png_dir:
                os.makedirs(a.png_dir, exist_ok=True)
                diff.png_report(R, f'{a.png_dir}/{a.gate}-{name}.png')
        rows.append(row)
        print(f'{name}: {row["status"]} {R["match_pct"]:.5f} % ({R["blocks_mismatch"]} из {R["blocks_compared"]}), биомы {row["biomes_pct"]:.3f} %, '
              f'{row["gen_s"]} с', flush=True)
    # --- таблица ---
    ok_rows = [r for r in rows if r.get('verdict') == 'pass']
    tot_chunks = sum(r.get('chunks_compared', 0) for r in rows if 'chunks_compared' in r)
    dims_ok = {r['dim'] for r in ok_rows}
    seeds_ok = {r['seed'] for r in ok_rows}
    L = [f'### {G["title"]}', '',
         f'Версия {a.version}; эталон — вариант `{G["variant"]}` (tools/gt, ванильный сервер); стадии 0x{G["stages"]:x}; порог {G["min_match"]} %; '
         f'маски: ext-биомы {"да" if G["mask_ext"] else "нет"}, жилы руд {"да" if G["mask_veins"] else "нет"}; твики {tweaks or "нет"}. '
         f'Запуск {time.strftime("%Y-%m-%d %H:%M")}, {"ctypes" if lib else "mcgen-cli"}; жидкости: {"маска растекания" if (a.mask_flow or lib) else "строго, CLI --pp-margin " + str(max((a.margin if a.margin is not None else G.get("margin", 0)) - 1, 0))}.', '',
         '| seed | измерение | область (центр, r=10) | чанков (чистых) | блоков | расхождений (в чистых) | совпадение | расх. в full | биомы | карты высот (расх.) | растекание (маска) | время, с | итог |',
         '|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|']
    for r in rows:
        if 'blocks_compared' in r:
            L.append(f'| {r["seed"]} | {r["dim"]} | {r["label"]} ({r["center"][0]},{r["center"][1]}) | {r["chunks_compared"]} ({r["pure"]["chunks"]}) | {r["blocks_compared"]:,} | '
                     f'{r["blocks_mismatch"]:,} ({r["pure"]["mismatch"]:,}) | {r["match_pct"]:.5f} % | {r["full"]["mismatch"]:,} | {("–" if r["biomes_pct"] != r["biomes_pct"] else format(r["biomes_pct"], ".3f") + " %")} | {r["hm_bad"]} | '
                     f'{r["blocks_masked_flow"] + r["blocks_flow_induced"]:,} | {r["gen_s"]} | {r["status"]} |')
        else:
            L.append(f'| {r["seed"]} | {r["dim"]} | {r["label"]} ({r["center"][0]},{r["center"][1]}) | – | – | – | – | – | – | – | – | {r.get("gen_s", "–")} | {r["status"]} |')
    L.append('')
    L.append(f'Итог: {len(ok_rows)}/{len(rows)} строк PASS; чанков сравнено {tot_chunks}; измерений с PASS: {len(dims_ok)}; seed с PASS: {len(seeds_ok)}.')
    fails = [r for r in rows if r.get('verdict') == 'fail']
    if fails:
        L.append('')
        L.append('Главные расхождения (наше -> эталон):')
        for r in fails[:12]:
            L.append(f'* `{r["name"]}` ({r["match_pct"]:.5f} %): ' + '; '.join(f'{p["count"]:,}x {p["ours"]} -> {p["ref"]}' for p in r['top'])
                     + ('; первые: ' + ', '.join(f'({q["x"]},{q["y"]},{q["z"]})' for q in r['samples'][:3]) if r.get('samples') else ''))
    body = '\n'.join(L)
    print('\n' + body)
    if not a.no_doc:
        replace_section(a.accuracy, a.gate if a.version == '26.3' else f'{a.gate}@{a.version}', body)
        print(f'\nзаписано: {a.accuracy} (секция {a.gate if a.version == "26.3" else a.gate + "@" + a.version})')
    sys.exit(2 if errors else (1 if any_fail else 0))


if __name__ == '__main__':
    main()
