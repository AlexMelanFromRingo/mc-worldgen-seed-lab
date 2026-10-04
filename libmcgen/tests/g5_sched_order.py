#!/usr/bin/env python3
"""Порядок шагов FEATURES по умолчанию (feature_sched.c) = Python-модель tools/gt/sched_sim.py, и результат не зависит от числа потоков.

    python3 tests/g5_sched_order.py [--pack ../run/pack-26.3] [--version 26.3]

Область 9×7 чанков: порядок, записанный библиотекой (MCGEN_FEATURES_ORDER_OUT), совпадает с порядком модели, отфильтрованным по окну декорации;
дампы при 1 и 4 потоках побитово равны (параллелизм по графу зависимостей окон 3×3 даёт тот же результат, что последовательный обход).
"""
import argparse, filecmp, os, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__)); ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'gt'))
import sched_sim as S


def main():
    ap = argparse.ArgumentParser(); ap.add_argument('--pack', default=os.path.join(ROOT, 'run', 'pack-26.3')); ap.add_argument('--version', default='26.3')
    a = ap.parse_args()
    cli = os.path.join(ROOT, 'libmcgen', 'build', 'mcgen-cli')
    cx0, cz0, nx, nz = -4, -3, 9, 7
    tmp = tempfile.mkdtemp()
    outs = []
    for th in (1, 4):
        env = dict(os.environ); env['MCGEN_FEATURES_ORDER_OUT'] = f'{tmp}/order{th}.txt'
        env.pop('MCGEN_FEATURES_SCHED', None); env.pop('MCGEN_FEATURES_AREA', None)
        subprocess.run([cli, '--pack', a.pack, '--version', a.version, '--dim', 'minecraft:overworld', '--preset', 'normal', '--seed', '12345', '--cx0', str(cx0), '--cz0', str(cz0),
                        '--nx', str(nx), '--nz', str(nz), '--stages', '0x1f', '--threads', str(th), '--out', f'{tmp}/d{th}.mcr'], env=env, check=True, capture_output=True)
    got = [tuple(map(int, l.split())) for l in open(f'{tmp}/order1.txt')]
    # окно декорации = область + кольцо 1; модель считается для области (радиусы по осям разные — используем общий код с прямоугольником)
    ax0, az0, ax1, az1 = cx0, cz0, cx0 + nx - 1, cz0 + nz - 1
    exp = rect_order(ax0, az0, ax1, az1)
    rect = {(x, z) for x in range(ax0 - 1, ax1 + 2) for z in range(az0 - 1, az1 + 2)}
    exp = [c for c in exp if c in rect]
    ok_order = got == exp
    ok_thr = filecmp.cmp(f'{tmp}/d1.mcr', f'{tmp}/d4.mcr', shallow=False)
    print(f'порядок C == модель Python: {"OK" if ok_order else "НЕТ"} ({len(got)} чанков); 1 поток == 4 потока: {"OK" if ok_thr else "НЕТ"}')
    sys.exit(0 if ok_order and ok_thr else 1)


def rect_order(ax0, az0, ax1, az1):
    """sched_sim.simulate для прямоугольной области: берём ту же модель, подменяя область (simulate рассчитан на квадрат ±radius вокруг нуля — сдвигаем и растягиваем вручную)."""
    return S.simulate_rect(ax0, az0, ax1, az1)


if __name__ == '__main__':
    main()
