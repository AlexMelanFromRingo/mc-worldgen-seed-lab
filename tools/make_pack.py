#!/usr/bin/env python3
"""Готовит pack-каталоги для libmcgen и Blender-аддона из jar'ов Mojang (материалы Mojang остаются на вашей машине):

    run/pack-<V>/data/minecraft/**       датапак игры (worldgen/*.json, structure/*.nbt, tags, loot_table …) из game jar
    run/pack-<V>/reports/blocks.json     все состояния блоков (id, свойства) — генератор данных игры `--reports`
    run/pack-<V>/reports/registries.json реестры (порядок биомов, блоков, …)
    run/pack-<V>/reports/block_flags.json свойства состояний блоков (libmcgen/tests/g5_blockflags.py; нужен JDK)
    run/pack-<V>/version.json            world_version и т. п.
    run/assets-<V>/assets/minecraft/{blockstates,models,textures,font?}   ресурсы клиента (client jar)

Использование:
    tools/make_pack.py 26.3 [26.1 26.2 26.4-snapshot-2] [--no-assets] [--force]
Предусловие: jars/game-<V>.jar, jars/client-<V>.jar, src/bundle-<V> (создаёт tools/fetch_game.py).
Генератор данных не требует принятия EULA (запускается офлайн, ничего не скачивает).
"""
import json, os, shutil, subprocess, sys, zipfile

ROOT = os.environ.get('MCGEN_ROOT') or os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def make(v, assets=True, force=False):
    game = f'{ROOT}/jars/game-{v}.jar'
    client = f'{ROOT}/jars/client-{v}.jar'
    out = f'{ROOT}/run/pack-{v}'
    stamp = f'{out}/.complete'
    if os.path.exists(stamp) and not force:
        print(f'[{v}] pack уже готов: {out}')
    else:
        shutil.rmtree(out, ignore_errors=True); os.makedirs(out)
        with zipfile.ZipFile(game) as z:
            n = 0
            for name in z.namelist():
                if (name.startswith('data/') or name == 'version.json') and not name.endswith('/'):
                    dst = f'{out}/{name}'
                    os.makedirs(os.path.dirname(dst), exist_ok=True)
                    open(dst, 'wb').write(z.read(name)); n += 1
        cp = subprocess.check_output([f'{ROOT}/tools/classpath.sh', v], text=True).strip()
        tmp = f'{out}/_gen'
        subprocess.run(['java', '-cp', cp, 'net.minecraft.data.Main', '--reports', '--output', tmp],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        os.makedirs(f'{out}/reports', exist_ok=True)
        for f in ('blocks.json', 'registries.json', 'datapack.json'):
            if os.path.exists(f'{tmp}/reports/{f}'):
                shutil.copy(f'{tmp}/reports/{f}', f'{out}/reports/{f}')
        shutil.rmtree(tmp, ignore_errors=True)
        # свойства состояний блоков из настоящих классов игры (isSolid, replaceable, жидкость, sturdy, класс …) — нужны фичам libmcgen
        bf = f'{ROOT}/libmcgen/tests/g5_blockflags.py'
        if os.path.exists(bf):
            try:
                subprocess.run([sys.executable, bf, v, '--force'], check=True, stdout=subprocess.DEVNULL)
            except Exception as e:                       # нужен JDK (javac); без файла libmcgen работает по эвристикам имён (G5 не гарантируется)
                print(f'[{v}] ВНИМАНИЕ: block_flags.json не создан ({e})')
        blocks = json.load(open(f'{out}/reports/blocks.json'))
        nst = sum(len(b['states']) for b in blocks.values())
        open(stamp, 'w').write(f'{n} files, {len(blocks)} blocks, {nst} states\n')
        print(f'[{v}] pack: {n} файлов датапака, {len(blocks)} блоков, {nst} состояний -> {out}')
    if assets:
        aout = f'{ROOT}/run/assets-{v}'
        astamp = f'{aout}/.complete'
        if os.path.exists(astamp) and not force:
            print(f'[{v}] assets уже готовы: {aout}')
            return
        shutil.rmtree(aout, ignore_errors=True); os.makedirs(aout)
        with zipfile.ZipFile(client) as z:
            n = 0
            for name in z.namelist():
                if name.startswith('assets/minecraft/') and not name.endswith('/'):
                    if name.startswith(('assets/minecraft/blockstates/', 'assets/minecraft/models/', 'assets/minecraft/textures/',
                                        'assets/minecraft/atlases/', 'assets/minecraft/items/', 'assets/minecraft/lang/en_us')):
                        dst = f'{aout}/{name}'
                        os.makedirs(os.path.dirname(dst), exist_ok=True)
                        open(dst, 'wb').write(z.read(name)); n += 1
        open(astamp, 'w').write(f'{n} files\n')
        print(f'[{v}] assets: {n} файлов -> {aout}')


def main():
    args = sys.argv[1:]
    versions = [a for a in args if not a.startswith('--')] or ['26.3']
    for v in versions:
        make(v, assets='--no-assets' not in args, force='--force' in args)


if __name__ == '__main__':
    main()
