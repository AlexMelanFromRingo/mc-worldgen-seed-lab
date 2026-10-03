#!/usr/bin/env python3
"""Выгрузка свойств состояний блоков из настоящих классов игры -> run/pack-<V>/reports/block_flags.json (нужно стадии FEATURES libmcgen).

    python3 libmcgen/tests/g5_blockflags.py 26.3 [26.1 26.2 26.4-snapshot-2] [--force]

Java-источник — libmcgen/tests/g5_blockflags/BlockFlags.java (формат — в его заголовке). Данные Mojang не копируются в репозиторий:
файл создаётся в pack-каталоге пользователя (run/pack-<V>), как reports/blocks.json. Без этого файла libmcgen использует приближённые
эвристики по именам блоков (точность ниже, G5 не гарантируется).
"""
import os, subprocess, sys

ROOT = os.environ.get('MCGEN_ROOT') or os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def run(v, force=False):
    out = f'{ROOT}/run/pack-{v}/reports/block_flags.json'
    if os.path.exists(out) and not force:
        print(f'[{v}] уже есть: {out}'); return
    cp = subprocess.check_output([f'{ROOT}/tools/classpath.sh', v], text=True).strip()
    cls = f'{ROOT}/libmcgen/build/java/g5flags/{v}'
    os.makedirs(cls, exist_ok=True)
    src = f'{ROOT}/libmcgen/tests/g5_blockflags/BlockFlags.java'
    subprocess.run(['javac', '-nowarn', '-Xlint:none', '-encoding', 'UTF-8', '-d', cls, '-cp', cp, src], check=True)
    tmp = out + '.tmp'
    subprocess.run(['java', '-Xss8m', f'-Dlog4j2.configurationFile={ROOT}/oracle/log4j2.xml', '--sun-misc-unsafe-memory-access=allow',
                    '-cp', f'{cls}:{cp}', 'mcgenflags.BlockFlags', tmp], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    os.replace(tmp, out)
    print(f'[{v}] {out}')


if __name__ == '__main__':
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    for v in args or ['26.3']:
        run(v, '--force' in sys.argv)
