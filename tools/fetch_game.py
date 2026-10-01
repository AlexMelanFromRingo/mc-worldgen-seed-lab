#!/usr/bin/env python3
"""Скачивает официальный серверный jar Mojang и готовит рабочие каталоги версии (всё, что не входит в репозиторий):

    jars/server-<V>.jar               официальный bundler-jar (проверяется sha1 из манифеста Mojang)
    jars/game-<V>.jar                 внутренний jar игры (классы + датапак) из META-INF/versions/
    src/bundle-<V>/META-INF/libraries библиотеки версии (classpath: tools/classpath.sh <V>)
    src/data-<V>/data, version.json   распакованный датапак (worldgen/*.json)
    src/dec/<V>/                      декомпилированные исходники (Vineflower; с 26.1 код не обфусцирован)
    tools/vineflower.jar              декомпилятор (Maven Central), если ещё не скачан

Использование:
    tools/fetch_game.py 26.1 26.2 26.3            # скачать + распаковать + декомпилировать
    tools/fetch_game.py 26.3 --no-decompile       # без декомпиляции (быстро)
    tools/fetch_game.py --list                    # какие версии есть в манифесте

Сам репозиторий НЕ содержит файлов Mojang (jar'ы, декомпилят, датапак): их нужно получить у Mojang этим скриптом.
Скачивая серверный jar, вы принимаете Minecraft EULA (https://aka.ms/MinecraftEULA); скрипт её за вас не принимает
(`eula.txt` для локальных серверов создаёт только `tools/l3_fetch_server.py --accept-eula` — по вашему явному флагу).
"""
import hashlib, json, os, shutil, subprocess, sys, urllib.request, zipfile

ROOT = os.environ.get('MCGEN_ROOT') or os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MANIFEST = 'https://piston-meta.mojang.com/mc/game/version_manifest_v2.json'
VINEFLOWER = 'https://repo1.maven.org/maven2/org/vineflower/vineflower/1.12.0/vineflower-1.12.0.jar'


def get(url):
    with urllib.request.urlopen(url, timeout=120) as r:
        return r.read()


def sha1(path):
    h = hashlib.sha1()
    with open(path, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''):
            h.update(b)
    return h.hexdigest()


def fetch(version, manifest, decompile):
    entry = next((v for v in manifest['versions'] if v['id'] == version), None)
    if entry is None:
        sys.exit(f'версия {version} не найдена в манифесте (см. --list)')
    meta = json.loads(get(entry['url']))
    srv = meta['downloads']['server']
    os.makedirs(f'{ROOT}/jars', exist_ok=True)
    jar = f'{ROOT}/jars/server-{version}.jar'
    if not (os.path.exists(jar) and sha1(jar) == srv['sha1']):
        print(f'[{version}] скачиваю server.jar ({srv["size"] / 1e6:.0f} МБ)')
        open(jar, 'wb').write(get(srv['url']))
        if sha1(jar) != srv['sha1']:
            sys.exit(f'[{version}] sha1 не совпал')
    with zipfile.ZipFile(jar) as z:
        vl = z.read('META-INF/versions.list').decode().split()
        inner = vl[-1] if len(vl) % 3 == 0 else None          # формат: <sha1>\t<id>\t<path>
        inner = f'META-INF/versions/{inner}' if inner else next(n for n in z.namelist() if n.startswith('META-INF/versions/') and n.endswith('.jar'))
        game = f'{ROOT}/jars/game-{version}.jar'
        open(game, 'wb').write(z.read(inner))
        bundle = f'{ROOT}/src/bundle-{version}'
        for n in z.namelist():
            if n.startswith('META-INF/') and not n.startswith('META-INF/versions/') and not n.endswith('/'):
                dst = f'{bundle}/{n}'
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                open(dst, 'wb').write(z.read(n))
    with zipfile.ZipFile(game) as z:
        data = f'{ROOT}/src/data-{version}'
        for n in z.namelist():
            if (n.startswith('data/') or n == 'version.json') and not n.endswith('/'):
                dst = f'{data}/{n}'
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                open(dst, 'wb').write(z.read(n))
    print(f'[{version}] game jar, библиотеки, датапак готовы')
    if decompile:
        vf = f'{ROOT}/tools/vineflower.jar'
        if not os.path.exists(vf):
            print('скачиваю Vineflower 1.12.0'); open(vf, 'wb').write(get(VINEFLOWER))
        out = f'{ROOT}/src/dec/{version}'
        shutil.rmtree(out, ignore_errors=True); os.makedirs(out)
        print(f'[{version}] декомпиляция (1–3 мин, ~2 ГБ ОЗУ)')
        subprocess.run(['java', '-Xmx4g', '-jar', vf, '--silent', game, out], check=True)
        print(f'[{version}] исходники: {out}')


def main():
    args = sys.argv[1:]
    manifest = json.loads(get(MANIFEST))
    if '--list' in args:
        print(' '.join(v['id'] for v in manifest['versions'] if v['id'].startswith('26.')))
        return
    versions = [a for a in args if not a.startswith('--')]
    if not versions:
        sys.exit(__doc__)
    for v in versions:
        fetch(v, manifest, '--no-decompile' not in args)


if __name__ == '__main__':
    main()
