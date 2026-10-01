#!/usr/bin/env python3
"""Сквозной тест crack-struct + crack-lift64 против эталона (oracle = реальный код Mojang).

Для каждой версии (26.1/26.2/26.3) и случайных 64-битных seed:
  1. oracle `structs`/`structstart` -> позиции СТРУКТУР (структуры, реально сгенерированные игрой, где это возможно;
     иначе «потенциальные» чанки — isStructureChunk реального кода; в отчёте помечено),
  2. файл наблюдений -> crack-struct (lifting / полный перебор) -> список structure seed, проверка: истинный seed среди кандидатов,
  3. crack-lift64: (a) hashed seed из oracle `hashed`; (b) биомы из oracle `blockbiome` (Overworld, блок-уровень) / `biome` (Nether/End, кварты)
     -> восстановлен ли исходный 64-битный seed.
Сценарии: ow-lift (liftable-структуры), ow-mixed (+triangular, pow2, reducers, exclusion zone), ow-full (без liftable: полный перебор, окно), nether, end.
Для nether/end/«без liftable» полный перебор 2^48 в массовом тесте заменён окном 2^W значений вокруг истинного seed (--window-bits,
по умолчанию 34; проверяет то же ядро); реальные полные прогоны 2^48 — опция --full-real N.

Запуск:  crack/tests/e2e.py [--versions 26.3] [--seeds 6] [--rng-seed 1] [--real/--potential] [--full-real 1] [--json out.json]
Требует: собранные crack/bin/crack-struct и crack/bin/crack-lift64 (make -C crack struct), JDK для oracle.
"""
import argparse
import json
import os
import random
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from oracle_client import Oracle, ROOT  # noqa: E402

BIN = os.path.join(ROOT, 'crack', 'bin')
MASK48 = (1 << 48) - 1
# структуры, для которых oracle.structstart надёжно работает (реальная генерация); остальные — потенциальные чанки
REAL_OK = {'desert_pyramids', 'igloos', 'jungle_temples', 'swamp_huts', 'shipwrecks', 'ocean_monuments', 'woodland_mansions', 'villages',
           'ocean_ruins', 'ancient_cities', 'trial_chambers', 'trail_ruins', 'abandoned_camp', 'pillager_outposts', 'nether_complexes', 'end_cities'}
OBS_NAME = {  # имя в файле наблюдений: чередуем id набора и id структуры (формат SeedcrackerX) для проверки обоих
    'desert_pyramids': 'desert_pyramid', 'igloos': 'igloo', 'jungle_temples': 'jungle_pyramid', 'swamp_huts': 'swamp_hut', 'shipwrecks': 'shipwreck',
    'ocean_monuments': 'monument', 'woodland_mansions': 'mansion', 'end_cities': 'end_city', 'villages': 'village', 'nether_complexes': 'nether_complex',
    'pillager_outposts': 'pillager_outpost', 'buried_treasures': 'buried_treasure'}


def game_random_seed(rng):
    """Seed как его генерирует игра при пустом поле seed: WorldOptions.randomSeed() = LegacyRandomSource(uniqueSeed).nextLong()."""
    st = (rng.getrandbits(64) ^ 0x5DEECE66D) & MASK48

    def nxt(st):
        st = (st * 0x5DEECE66D + 0xB) & MASK48
        v = st >> 16
        return st, (v - (1 << 32) if v >> 31 else v)
    st, a = nxt(st)
    st, b = nxt(st)
    w = ((a << 32) + b) & ((1 << 64) - 1)
    return w - (1 << 64) if w >> 63 else w


def sh(cmd, timeout=3600):
    # GPU делят несколько агентов: каждый запуск crack-struct (кроме CPU) — под замком /tmp/gpu.lock (flock), короткими прогонами
    if os.path.basename(cmd[0]) == 'crack-struct' and '--dev' not in cmd and os.environ.get('E2E_NO_LOCK') is None:
        cmd = ['flock', '/tmp/gpu.lock'] + cmd
    t0 = time.time()
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    return r, time.time() - t0


def lines(out):
    return [l.split()[0] for l in out.splitlines() if l.strip() and not l.startswith('#')]


def pick_obs(orc, dim, seed, setname, want, real, rng, max_tries=40):
    """want структур набора setname: (cx, cz, 'real'|'potential'). Ближние к началу координат — как исследует игрок."""
    spacing = {'mansion': 80, 'woodland_mansions': 80}.get(setname, 34)
    rad = max(150, spacing * 5)
    r = orc.call(f'structs {dim} {seed} {setname} {-rad} {-rad} {2 * rad} {2 * rad}')
    pos = r['sets'].get('minecraft:' + setname, [])
    if setname in ('buried_treasures', 'mineshafts'):
        rng.shuffle(pos)
    pos = sorted(pos, key=lambda p: abs(p[0]) + abs(p[1])) if setname not in ('buried_treasures', 'mineshafts') else pos
    out = []
    if real and setname in REAL_OK:
        tries = 0
        for cx, cz in pos:
            if len(out) >= want or tries >= max_tries:
                break
            tries += 1
            try:
                rr = orc.call(f'structstart {dim} {seed} {cx} {cz} {setname}')
            except RuntimeError:
                break
            if any(x.get('generated') for x in rr['results']):
                out.append((cx, cz, 'real'))
    for cx, cz in pos:
        if len(out) >= want:
            break
        if (cx, cz, 'real') not in out:
            out.append((cx, cz, 'potential'))
    return out[:want]


def write_obs(path, obs, rng):
    with open(path, 'w') as f:
        f.write('# e2e: name;chunkX;chunkZ\n')
        for (setname, cx, cz, tag) in obs:
            nm = OBS_NAME.get(setname, setname) if rng.random() < 0.5 else setname
            f.write(f'{nm};{cx};{cz}\n')


def biome_obs_overworld(orc, seed, rng, n=16):
    pts = []
    for _ in range(n):
        x, z = rng.randint(-2500, 2500), rng.randint(-2500, 2500)
        y = rng.choice([50, 64, 70, 80, 100])
        r = orc.call(f'blockbiome overworld {seed} {x} {y} {z}')
        pts.append((x, z, y, r['biome'].replace('minecraft:', '')))
    return pts


def biome_obs_quart(orc, dim, seed, rng, n=24):
    pts = []
    for _ in range(n):
        if dim == 'the_end':
            import math
            a = rng.random() * 6.283; rad = rng.randint(1100, 3500)
            qx, qz = int(math.cos(a) * rad) // 4, int(math.sin(a) * rad) // 4
        else:
            qx, qz = rng.randint(-600, 600), rng.randint(-600, 600)
        qy = rng.choice([8, 16, 24]) if dim == 'the_nether' else 16
        r = orc.call(f'biome {dim} {seed} {qx} {qz} 1 1 {qy} --fmt json')
        pts.append((qx, qz, qy, r['palette'][r['idx'][0]].replace('minecraft:', '')))
    return pts


def opts_random_case(res):
    return res.get('seed_kind') == 'game-random'


def run_case(orc, V, dim, scen, seed, sets, K, real, rng, tmp, opts):
    """Один сквозной случай. Возвращает dict с результатами."""
    truth48 = seed & MASK48
    res = {'version': V, 'scenario': scen, 'dim': dim, 'seed': seed, 'seed_kind': 'game-random' if opts.game_random_seed(seed) else 'arbitrary'}
    obs = []
    per = max(1, (K + len(sets) - 1) // len(sets))
    for s in sets:
        for (cx, cz, tag) in pick_obs(orc, dim, seed, s, per, real, rng):
            obs.append((s, cx, cz, tag))
    rng.shuffle(obs)
    obs = obs[:K]
    sh_obs = []
    if scen == 'ow-sh':      # + 3 страхолда первого кольца (реальные позиции из oracle.stronghold) как фильтр
        chunks = orc.call(f'stronghold {seed}')['chunks']
        sh_obs = [('strongholds', cx, cz, 'real') for cx, cz in chunks[:3]]
        res['cands_without_rings'] = None
    res['K'] = len(obs)
    res['real'] = sum(1 for o in obs if o[3] == 'real')
    res['sets'] = sorted(set(o[0] for o in obs))
    path = os.path.join(tmp, f'obs_{V}_{scen}_{seed & 0xFFFFFFFF:08x}.txt')
    write_obs(path, obs, rng)
    if sh_obs:
        r0, _ = sh([os.path.join(BIN, 'crack-struct'), '--version', V, '--max-out', '1000000', path])
        res['cands_without_rings'] = len(lines(r0.stdout))
        with open(path, 'a') as f:
            for (nm, cx, cz, _t) in sh_obs:
                f.write(f'{nm};{cx};{cz}\n')
    # --- crack-struct ---
    cmd = [os.path.join(BIN, 'crack-struct'), '--version', V, '--max-out', '1000000', path]
    if scen in ('nether', 'end', 'ow-full'):
        cmd += ['--mode', 'full', '--assume-seed', str(truth48), '--window-bits', str(opts.window_bits)]
    r, dt = sh(cmd)
    cands = [int(x) for x in lines(r.stdout)]
    res['struct_time'] = dt
    res['cands'] = len(cands)
    res['struct_ok'] = truth48 in cands
    res['struct_err'] = r.stderr.strip().splitlines()[-1] if r.returncode not in (0, 1) else ''
    info = [l for l in r.stderr.splitlines() if 'информация' in l]
    res['info'] = info[0].split('информация ~')[1].split('бит')[0].strip() if info else ''
    mline = [l for l in r.stderr.splitlines() if l.startswith('режим')]
    res['mode'] = mline[0].split(':')[0] if mline else ''
    res['struct_stderr_tail'] = r.stderr.strip().splitlines()[-3:]
    if not res['struct_ok']:
        return res
    cfile = os.path.join(tmp, f'cands_{V}_{scen}.txt')
    with open(cfile, 'w') as f:
        f.write('\n'.join(str(c) for c in cands) + '\n')
    # --- lift64: hashed ---
    H = orc.call(f'hashed {seed}')['hashed_seed']
    r, dt = sh([os.path.join(BIN, 'crack-lift64'), '--version', V, '--seeds', cfile, '--hashed', str(H)])
    got = [int(x) for x in lines(r.stdout)]
    res['hashed_ok'] = got == [seed]
    res['hashed_time'] = dt
    if opts_random_case(res):
        r, dt = sh([os.path.join(BIN, 'crack-lift64'), '--version', V, '--seeds', cfile])
        got = [int(x) for x in lines(r.stdout)]
        res['random_ok'] = seed in got and len(got) <= 3 * len(cands)
        res['random_n'] = len(got)
    # --- lift64: биомы ---
    bfile = os.path.join(tmp, f'biomes_{V}_{scen}.txt')
    if dim == 'overworld':
        pts = biome_obs_overworld(orc, seed, rng)
        with open(bfile, 'w') as f:
            f.write('@dim overworld\n' + ''.join('%d %d %d %s\n' % p for p in pts))
        # биомный проход ~1 с на кандидата: при длинном списке проверяем истинный + до 150 других (в реальном применении список сначала сужают hashed/--random)
        bf = os.path.join(tmp, f'cands_bio_{V}_{scen}.txt')
        sub = [truth48] + [c for c in cands if c != truth48][:150]
        with open(bf, 'w') as f:
            f.write('\n'.join(str(c) for c in sub) + '\n')
        ccmd = [os.path.join(BIN, 'crack-lift64'), '--version', V, '--seeds', bf, '--biomes', bfile]
        r, dt = sh(ccmd)
        got = [int(x) for x in lines(r.stdout) if not x.startswith('UNDET')]
        res['biome_ok'] = got == [seed]
        res['biome_time'] = dt
        res['biome_n'] = len(pts)
    else:
        pts = biome_obs_quart(orc, dim, seed, rng)
        with open(bfile, 'w') as f:
            f.write(f'@dim {dim.replace("the_", "")}\n@quart\n' + ''.join('%d %d %d %s\n' % p for p in pts))
        r, dt = sh([os.path.join(BIN, 'crack-lift64'), '--version', V, '--seeds', cfile, '--biomes', bfile, '--hashed', str(H)])
        got = [int(x) for x in lines(r.stdout)]
        res['biome_ok'] = got == [seed]          # quart-биомы фильтруют structure seed; hashed даёт верхние 16 бит
        res['biome_time'] = dt
        res['biome_n'] = len(pts)
        # только биомы (без hashed): structure seed подтверждён, верхние биты не определены
        r2, _ = sh([os.path.join(BIN, 'crack-lift64'), '--version', V, '--seeds', cfile, '--biomes', bfile])
        res['biome_only_undetermined'] = ('UNDETERMINED struct_seed=%d' % truth48) in r2.stdout
    return res


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--versions', nargs='+', default=['26.1', '26.2', '26.3'])
    ap.add_argument('--seeds', type=int, default=6, help='случайных seed на версию и сценарий')
    ap.add_argument('--rng-seed', type=int, default=20260930)
    ap.add_argument('--potential', action='store_true', help='не использовать structstart (быстрее): только потенциальные чанки')
    ap.add_argument('--window-bits', type=int, default=34)
    ap.add_argument('--scenarios', nargs='+', default=['ow-lift', 'ow-mixed', 'ow-sh', 'ow-full', 'nether', 'end'])
    ap.add_argument('--full-real', type=int, default=0, help='число РЕАЛЬНЫХ полных прогонов 2^48 (nether) на версию')
    ap.add_argument('--json', help='сохранить результаты в JSON')
    ap.add_argument('--tmp', default='/tmp/crack_e2e')
    opts = ap.parse_args()
    os.makedirs(opts.tmp, exist_ok=True)
    rng = random.Random(opts.rng_seed)
    allres = []
    for V in opts.versions:
        # 0) host-реализация placement против векторов oracle
        r, _ = sh([os.path.join(BIN, 'crack-struct'), '--version', V, '--selftest', os.path.join(ROOT, 'tests', 'vectors', V, 'structs.tsv')])
        print(f'[{V}] selftest placement: {r.stdout.strip()}', flush=True)
        orc = Oracle(V)
        t0 = time.time()
        ow_lift = ['desert_pyramids', 'igloos', 'swamp_huts', 'jungle_temples', 'shipwrecks', 'villages', 'trial_chambers', 'trail_ruins', 'ocean_ruins'] + (['abandoned_camp'] if V == '26.3' else [])
        ow_mixed = ['ancient_cities', 'ruined_portals', 'ocean_monuments', 'woodland_mansions', 'pillager_outposts', 'buried_treasures', 'desert_pyramids', 'igloos', 'shipwrecks'] + (['abandoned_camp'] if V == '26.3' else [])
        plan = {'ow-lift': ('overworld', ow_lift, 8), 'ow-mixed': ('overworld', ow_mixed, 9),
                'ow-sh': ('overworld', ['desert_pyramids', 'igloos', 'swamp_huts', 'shipwrecks'], 4),
                'ow-full': ('overworld', ['ancient_cities', 'ruined_portals', 'ocean_monuments', 'woodland_mansions', 'buried_treasures', 'mineshafts'] + (['abandoned_camp'] if V == '26.3' else []), 12),
                'nether': ('the_nether', ['nether_complexes', 'ruined_portals'], 10), 'end': ('the_end', ['end_cities'], 10)}
        for scen in opts.scenarios:
            dim, sets, K = plan[scen]
            for i in range(opts.seeds):
                seed = game_random_seed(rng) if (i % 2 == 0) else rng.randint(-2**63, 2**63 - 1)
                opts.game_random_seed = lambda sd, _g=(i % 2 == 0): _g
                try:
                    res = run_case(orc, V, dim, scen, seed, sets, K, not opts.potential, rng, opts.tmp, opts)
                except Exception as e:  # noqa
                    res = {'version': V, 'scenario': scen, 'seed': seed, 'error': repr(e)}
                allres.append(res)
                print(f"[{V}] {scen:9s} seed={seed:21d} K={res.get('K','?'):>2} real={res.get('real','?'):>2} info={res.get('info','?'):>5} "
                      f"cands={res.get('cands','?'):>5} struct_ok={res.get('struct_ok')} t={res.get('struct_time', 0):.2f}s "
                      f"hashed_ok={res.get('hashed_ok')} biome_ok={res.get('biome_ok')} random_ok={res.get('random_ok')} "
                      f"{('без колец=%s ' % res['cands_without_rings']) if res.get('cands_without_rings') is not None else ''}{res.get('error','')}", flush=True)
        for j in range(opts.full_real):
            seed = rng.randint(-2**63, 2**63 - 1)
            # реальный полный прогон: nether, 10 структур
            obs = []
            for s in ['nether_complexes', 'ruined_portals']:
                obs += [(s, cx, cz, t) for cx, cz, t in pick_obs(orc, 'the_nether', seed, s, 6, not opts.potential, rng)]
            path = os.path.join(opts.tmp, f'full_{V}_{j}.txt')
            write_obs(path, obs[:12], rng)
            r, dt = sh([os.path.join(BIN, 'crack-struct'), '--version', V, '--mode', 'full', path], timeout=36000)
            cands = [int(x) for x in lines(r.stdout)]
            ok = (seed & MASK48) in cands
            print(f'[{V}] FULL 2^48 nether seed={seed} cands={len(cands)} ok={ok} {dt:.1f}s :: {r.stderr.strip().splitlines()[-1]}', flush=True)
            allres.append({'version': V, 'scenario': 'full-real', 'seed': seed, 'cands': len(cands), 'struct_ok': ok, 'struct_time': dt})
        orc.close()
        print(f'[{V}] oracle-сессия {time.time() - t0:.0f} с', flush=True)
    # сводка
    print('\n=== СВОДКА ===')
    bad = 0
    for V in opts.versions:
        for scen in opts.scenarios + (['full-real'] if opts.full_real else []):
            rs = [r for r in allres if r['version'] == V and r['scenario'] == scen]
            if not rs:
                continue
            n = len(rs)
            sok = sum(1 for r in rs if r.get('struct_ok'))
            hok = sum(1 for r in rs if r.get('hashed_ok'))
            bok = sum(1 for r in rs if r.get('biome_ok'))
            rok = [r for r in rs if 'random_ok' in r]
            cands = [r.get('cands', 0) for r in rs if 'cands' in r]
            tm = [r.get('struct_time', 0) for r in rs]
            print(f"{V} {scen:9s}: structure seed найден {sok}/{n}; hashed->world seed {hok}/{n}; биомы->world seed {bok}/{n}; "
                  f"кандидатов: мин {min(cands) if cands else '-'} / медиана {sorted(cands)[len(cands)//2] if cands else '-'} / макс {max(cands) if cands else '-'}; "
                  f"время crack-struct: среднее {sum(tm)/max(len(tm),1):.2f} с"
                  + (f"; seed игры (пустое поле) -> верхние 16 бит без hashed/биомов: {sum(1 for r in rok if r['random_ok'])}/{len(rok)}" if rok else '')
                  + (f"; страхолды: кандидатов без колец (медиана) {sorted(r['cands_without_rings'] for r in rs if r.get('cands_without_rings') is not None)[len(rs)//2]}" if scen == 'ow-sh' else ''))
            if sok != n or (scen != 'full-real' and (hok != n or bok != n)) or any(r.get('random_ok') is False for r in rs):
                bad += 1
    if opts.json:
        with open(opts.json, 'w') as f:
            json.dump(allres, f, ensure_ascii=False, indent=1)
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
