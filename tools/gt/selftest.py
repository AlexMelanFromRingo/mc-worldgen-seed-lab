#!/usr/bin/env python3
"""Самопроверка инструментов tools/gt без libmcgen: mcr, anvil, diff, run_gate.

  python3 tools/gt/selftest.py [--ref run/gt/26.3/raw/overworld-s12345-c0_0-r10] [--other <мир с другим seed>]

Проверяет: круговой обмен MCR1; согласованность карты высот WORLD_SURFACE с блоками; сравнение мира с самим собой = 100 %;
искажение ровно N блоков находится ровно N раз и попадает в таблицу пар; другой seed даёт расхождение (код 1); маски (ignore-state, ignore-y,
only-y, margin); обнаружение недостающего чанка / несовпадения высоты (код 2); детерминизм «эталон против эталона»; run_gate с заглушкой CLI.
"""
import argparse, os, subprocess, sys, tempfile, json
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import anvil, mcr, diff, common
ROOT = common.ROOT
OK = []


def check(name, cond, extra=''):
    OK.append(bool(cond))
    print(('PASS ' if cond else 'FAIL ') + name + (f'  [{extra}]' if extra else ''), flush=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--ref', default=f'{ROOT}/run/gt/26.3/raw/overworld-s12345-c0_0-r10')
    ap.add_argument('--other', default=f'{ROOT}/run/gt/26.3/raw/overworld-s8675309-c0_0-r10')
    a = ap.parse_args()
    tmp = tempfile.mkdtemp(prefix='gtself')
    # 1. MCR
    subprocess.run([sys.executable, f'{ROOT}/tools/gt/mcr.py', 'selftest'], check=True)
    check('mcr: круговой обмен', True)
    ref = anvil.World(diff.find_world(a.ref), 'overworld', '26.3')
    c = ref.chunk(0, 0)
    # 2. карта высот
    air = ref.states.air
    ws = np.array([[(np.nonzero(c.blocks[:, z, x] != air)[0].max() + 1 + c.min_y) for x in range(16)] for z in range(16)]).ravel()
    check('anvil: WORLD_SURFACE == первая свободная y по блокам', (ws == c.heightmaps[0]).all())
    check('anvil: чанк 384x16x16, min_y -64, статус full', c.blocks.shape == (384, 16, 16) and c.min_y == -64 and c.status == 'minecraft:full')
    # 3. самосравнение
    p0 = f'{tmp}/self.mcr'
    subprocess.run([sys.executable, f'{ROOT}/tools/gt/mcr.py', 'from-world', diff.find_world(a.ref), '--cx0', '-3', '--cz0', '-3', '--nx', '7', '--nz', '7', '--out', p0], check=True, capture_output=True)
    R, o = diff.run(a.ref, p0, biomes=True, heightmaps=True)
    check('diff: мир против себя = 100 %, код 0', R['match_pct'] == 100.0 and R['exit'] == 0 and R['blocks_compared'] == 49 * 384 * 256, f'{R["blocks_compared"]}')
    check('diff: биомы и карты высот 100 %', R['biomes']['cells_mismatch'] == 0 and all(v['mismatch'] == 0 for v in R['heightmaps'].values()))
    # 4. искажение N блоков
    names = [ref.states.names.get(i, '') for i in range(ref.states.count)]
    rng = np.random.default_rng(3)
    mut = {}
    for cx in range(-3, 4):
        for cz in range(-3, 4):
            cc = ref.chunk(cx, cz); b = cc.blocks.copy()
            idx = rng.choice(b.size, 10, replace=False)
            flat = b.reshape(-1); old = flat[idx].copy()
            flat[idx] = np.where(old == ref.states.id_of('minecraft:stone'), ref.states.id_of('minecraft:dirt'), ref.states.id_of('minecraft:stone'))
            mut[(cx, cz)] = (b, cc.biomes, cc.heightmaps)
    p1 = f'{tmp}/mut.mcr'
    first = ref.chunk(-3, -3)
    mcr.write_mcr(p1, -3, -3, 7, 7, -64, 384, 0x3, lambda cx, cz: mut[(cx, cz)], names, ref.biomes.names)
    R, o = diff.run(a.ref, p1, top=5)
    check('diff: искажение ровно 49*10 блоков находится ровно столько раз, код 1', R['blocks_mismatch'] == 490 and R['exit'] == 1, f'{R["blocks_mismatch"]}')
    check('diff: таблица пар содержит stone->dirt или dirt->stone', any('stone' in p['ours'] for p in R['top_pairs']))
    R2, _ = diff.run(a.ref, p1, min_match=99.0)
    check('diff: порог 99 % -> PASS (код 0)', R2['exit'] == 0)
    R3, _ = diff.run(a.ref, p1, ignore_state=['minecraft:dirt', 'minecraft:stone'])
    check('diff: --ignore-state маскирует (0 расхождений)', R3['blocks_mismatch'] == 0 and R3['blocks_masked'] > 0)
    R4, _ = diff.run(a.ref, p1, ignore_y=[(-64, 319)])
    check('diff: --ignore-y на весь диапазон -> нечего сравнивать, код 2', R4['exit'] == 2)
    R5, _ = diff.run(a.ref, p1, margin=1)
    check('diff: --margin 1 -> 25 чанков', R5['chunks_compared'] == 25, f'{R5["chunks_compared"]}')
    # 5. другой seed
    if os.path.isdir(diff.find_world(a.other)):
        R6, _ = diff.run(a.ref, None, vs_world=a.other, box=(-3, -3, 7, 7))
        check('diff: другой seed -> сильное расхождение, код 1', R6['exit'] == 1 and R6['match_pct'] < 99.0, f'{R6["match_pct"]:.2f} %')
    # 5b. маски ext-биомов (айсберги frozen_ocean) и растекания
    ice = f'{ROOT}/run/gt/26.3/raw/overworld-s-7048155917072976836-c-61_-208-r10'
    if os.path.isdir(ice):
        wi = anvil.World(diff.find_world(ice), 'overworld', '26.3')
        pi_, st_ = wi.states.id_of('minecraft:packed_ice'), wi.states.id_of('minecraft:stone')
        ch = {}
        n_ice = 0
        for cx in range(-71, -50):
            for cz in range(-218, -197):
                cc = wi.chunk(cx, cz)
                b = cc.blocks.copy(); n_ice += int((b == pi_).sum()); b[b == pi_] = st_
                ch[(cx, cz)] = (b, cc.biomes, cc.heightmaps)
        pm = f'{tmp}/noice.mcr'
        mcr.write_mcr(pm, -71, -218, 21, 21, -64, 384, 0x3, lambda cx, cz: ch[(cx, cz)], names, wi.biomes.names)
        Ra, _ = diff.run(ice, pm, mask_ext=False)
        Rb, _ = diff.run(ice, pm, mask_ext=True)
        check('diff: убранный лёд frozen_ocean без --mask-ext даёт расхождения', Ra['blocks_mismatch'] >= n_ice > 0, f'{Ra["blocks_mismatch"]} (лёд {n_ice})')
        check('diff: --mask-ext скрывает расхождения айсбергов', Rb['blocks_mismatch'] == 0 and Rb['ext_columns_masked'] > 0, f'ext-столбцов {Rb["ext_columns_masked"]}')
    # растекание: в ref water[level=1] стоит рядом; заменим клетки рядом с ним на воздух
    fl = {i for i, n in ref.states.names.items() if diff._FLOW_RE.match(n)}
    done = False
    for cx in range(-10, 11):
        for cz in range(-10, 11):
            cc = ref.chunk(cx, cz)
            m = np.isin(cc.blocks, list(fl))
            if m.any() and not done:
                y, z, x = np.argwhere(m)[0]
                b = cc.blocks.copy()
                b[max(y - 1, 0):y + 2, max(z - 1, 0):z + 2, max(x - 1, 0):x + 2] = ref.states.air
                pf = f'{tmp}/flow.mcr'
                mcr.write_mcr(pf, cx, cz, 1, 1, -64, 384, 0x3, lambda a_, b_: (b, cc.biomes, cc.heightmaps), names, ref.biomes.names)
                Rf0, _ = diff.run(a.ref, pf, mask_flow=False)
                Rf1, _ = diff.run(a.ref, pf, flow_halo=1)
                check('diff: клетки у текущей жидкости: без маски расхождения, с гало 1 — 0', Rf0['blocks_mismatch'] > 0 and Rf1['blocks_mismatch'] == 0,
                      f'{Rf0["blocks_mismatch"]} -> {Rf1["blocks_mismatch"]}')
                done = True
    # 6. обрезанный дамп / чанк вне эталона
    p2 = f'{tmp}/far.mcr'
    mcr.write_mcr(p2, 500, 500, 1, 1, -64, 384, 0, lambda cx, cz: (first.blocks, first.biomes, first.heightmaps), names, ref.biomes.names)
    R7, _ = diff.run(a.ref, p2)
    check('diff: чанка нет в эталоне -> ошибка, код 2', R7['exit'] == 2)
    p3 = f'{tmp}/h.mcr'
    mcr.write_mcr(p3, 0, 0, 1, 1, 0, 256, 0, lambda cx, cz: (first.blocks[:256], first.biomes[:64], first.heightmaps), names, ref.biomes.names)
    R8, _ = diff.run(a.ref, p3)
    check('diff: другая высота -> ошибка, код 2', R8['exit'] == 2)
    # 7. заглушка CLI + run_gate
    fake = f'{ROOT}/tools/gt/fake_mcgen_cli.py'
    env = dict(os.environ, GT_FAKE_SRC=a.ref)
    acc = f'{tmp}/accuracy.md'
    r = subprocess.run([sys.executable, f'{ROOT}/tools/gt/run_gate.py', '--gate', 'G2', '--cli', fake, '--profile', 'core', '--seeds', '12345', '--dims', 'overworld',
                        '--accuracy', acc], capture_output=True, text=True, env=env)
    check('run_gate: заглушка -> PASS (код 0), секция записана', r.returncode == 0 and 'gate:G2 begin' in open(acc).read(), r.stdout.strip().splitlines()[-3:][0] if r.stdout else r.stderr[-200:])
    env2 = dict(env, GT_FAKE_PERTURB='5')
    r = subprocess.run([sys.executable, f'{ROOT}/tools/gt/run_gate.py', '--gate', 'G2', '--cli', fake, '--profile', 'core', '--seeds', '12345', '--dims', 'overworld',
                        '--accuracy', acc], capture_output=True, text=True, env=env2)
    check('run_gate: искажение -> FAIL (код 1)', r.returncode == 1, str(r.returncode))
    r = subprocess.run([sys.executable, f'{ROOT}/tools/gt/run_gate.py', '--gate', 'G2', '--cli', '/nonexistent/mcgen-cli', '--no-doc'], capture_output=True, text=True)
    check('run_gate: нет библиотеки -> код 3', r.returncode == 3, str(r.returncode))
    print(f'\n{sum(OK)}/{len(OK)} проверок прошло')
    sys.exit(0 if all(OK) else 1)


if __name__ == '__main__':
    main()
