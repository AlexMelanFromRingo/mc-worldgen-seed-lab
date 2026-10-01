#!/usr/bin/env python3
"""Скачивает серверный jar версии из манифеста Mojang в run/oldver/<версия>/server.jar.
Использование: tools/l3_fetch_server.py [--accept-eula] 1.12.2 1.16.5 ...
Файл eula.txt создаётся ТОЛЬКО с флагом --accept-eula — это ваше явное согласие с Minecraft EULA (https://aka.ms/MinecraftEULA);
без флага сервер не запустится, пока вы сами не запишете `eula=true` в run/oldver/<версия>/eula.txt."""
import json, os, sys, urllib.request, hashlib
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MANIFEST = 'https://piston-meta.mojang.com/mc/game/version_manifest_v2.json'
man = json.load(urllib.request.urlopen(MANIFEST))
urls = {v['id']: v['url'] for v in man['versions']}
ACCEPT = '--accept-eula' in sys.argv
for ver in [a for a in sys.argv[1:] if not a.startswith('--')]:
    d = f'{ROOT}/run/oldver/{ver}'; os.makedirs(d, exist_ok=True)
    meta = json.load(urllib.request.urlopen(urls[ver]))
    srv = meta['downloads']['server']
    p = f'{d}/server.jar'
    if not (os.path.exists(p) and hashlib.sha1(open(p, 'rb').read()).hexdigest() == srv['sha1']):
        with urllib.request.urlopen(srv['url']) as r, open(p, 'wb') as f: f.write(r.read())
    if ACCEPT:
        open(f'{d}/eula.txt', 'w').write('eula=true\n')
    print(ver, srv['size'], 'java', meta.get('javaVersion', {}).get('majorVersion'), 'worldVersion', meta.get('id'))
