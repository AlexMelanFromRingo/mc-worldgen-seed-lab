#!/usr/bin/env python3
"""Проверка гипотезы «недетерминизм игры = порядок шагов планировщика»: мир, записанный с JFR, воспроизводится libmcgen с ТЕМ ЖЕ порядком.

    replay_check.py <каталог мира gen_world> <запись.jfr> [--stages 0x1f] [--ring 3] [--margin 2] [--default-too]

Из JFR берутся порядок шагов FEATURES (MCGEN_FEATURES_ORDER) и порядок постобработки жидкостей (MCGEN_FLUID_ORDER); дамп libmcgen сравнивается с миром diff.py.
Печатает: совпадение при порядке по умолчанию (--default-too) и при записанном порядке.
"""
import argparse, json, os, re, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
STAGES = {'raw': 0x3, 'surface': 0x7, 'carvers': 0xf, 'features': 0x1f, 'full': 0x3f}


def run(world, jfr, stages, ring, margin, replay, dimname, out_mcr):
    man = json.load(open(f'{world}/manifest.json'))
    x0, z0, x1, z1 = man['area_chunks']
    cx0, cz0, nx, nz = x0 - margin, z0 - margin, x1 - x0 + 1 + 2 * margin, z1 - z0 + 1 + 2 * margin
    env = dict(os.environ)
    if replay:
        base = os.path.splitext(jfr)[0]
        subprocess.run([sys.executable, f'{ROOT}/tools/gt/jfr_order.py', jfr, '--txt', base + '_order.txt', '--fluid-txt', base + '_fluid.txt'], check=True, stdout=subprocess.DEVNULL)
        env.update(MCGEN_FEATURES_SEQ='file', MCGEN_FEATURES_ORDER=base + '_order.txt', MCGEN_FEATURES_RING=str(ring), MCGEN_FLUID_ORDER=base + '_fluid.txt')
    dim = {'overworld': 'minecraft:overworld', 'nether': 'minecraft:the_nether', 'end': 'minecraft:the_end'}[man['dim']]
    cmd = [f'{ROOT}/libmcgen/build/mcgen-cli', '--pack', f'{ROOT}/run/pack-{man["version"]}', '--version', man['version'], '--dim', dim, '--preset', 'normal',
           '--seed', str(man['seed']), '--cx0', str(cx0), '--cz0', str(cz0), '--nx', str(nx), '--nz', str(nz), '--stages', hex(stages), '--threads', '12', '--pp-margin', str(margin - 1), '--out', out_mcr]
    subprocess.run(cmd, env=env, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    r = subprocess.run([sys.executable, f'{ROOT}/tools/gt/diff.py', '--ref', world, '--mcr', out_mcr, '--dim', man['dim'], '--version', man['version'], '--margin', str(margin), '--no-mask-flow', '--top', '5'],
                       capture_output=True, text=True)
    m = re.search(r'блоков сравнено ([\d,]+), расхождений ([\d,]+)\s+->\s+совпадение ([\d.]+) %', r.stdout)
    tops = re.findall(r'^\s+([\d,]+)\s+(minecraft:\S+)\s+->\s+(minecraft:\S+)', r.stdout, re.M)[:4]
    return (m.group(1), m.group(2), m.group(3)) if m else None, tops


def main():
    ap = argparse.ArgumentParser(); ap.add_argument('world'); ap.add_argument('jfr'); ap.add_argument('--stages', default=None)
    ap.add_argument('--ring', type=int, default=3); ap.add_argument('--margin', type=int, default=2); ap.add_argument('--default-too', action='store_true')
    a = ap.parse_args()
    man = json.load(open(f'{a.world}/manifest.json'))
    stages = int(a.stages, 0) if a.stages else STAGES[man['variant'].split(':')[0] if man['variant'].split(':')[0] in STAGES else 'features']
    tmp = tempfile.mkdtemp()
    if a.default_too:
        res, tops = run(a.world, a.jfr, stages, a.ring, a.margin, False, man['dim'], f'{tmp}/d.mcr')
        print(f'{os.path.basename(a.world)}: порядок по умолчанию: {res[1]} расхождений из {res[0]} ({res[2]} %)' if res else 'нет результата')
    res, tops = run(a.world, a.jfr, stages, a.ring, a.margin, True, man['dim'], f'{tmp}/r.mcr')
    print(f'{os.path.basename(a.world)}: ЗАПИСАННЫЙ порядок:  {res[1]} расхождений из {res[0]} ({res[2]} %)' if res else 'нет результата')
    for n, x, y in tops: print(f'     {n:>8s}  {x} -> {y}')


if __name__ == '__main__':
    main()
