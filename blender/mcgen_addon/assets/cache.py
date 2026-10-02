"""Кэш таблицы состояний на диске: ключ — SHA-1 содержимого используемых ресурсов (blockstates, модели, текстуры блоков и colormaps,
atlases/blocks.json, reports/blocks.json, теги блоков) + версия формата таблицы. Чтобы не хэшировать ~10 тыс. файлов при каждой загрузке,
отпечаток (путь, размер, mtime) запоминается в файле-манифесте и пересчёт SHA-1 делается только при его изменении. Без bpy."""
import hashlib
import json
import os

FORMAT_VERSION = 12   # поднимать при любом изменении формата/логики сборки таблицы

_ASSET_DIRS = ('blockstates', 'models', 'textures/block', 'textures/colormap', 'atlases')


def _walk_files(root, rels):
    out = []
    for rel in rels:
        base = os.path.join(root, rel.replace('/', os.sep))
        if rel == 'atlases':
            p = os.path.join(base, 'blocks.json')
            if os.path.isfile(p):
                out.append(p)
            continue
        for dp, dn, fn in os.walk(base):
            dn.sort()
            for f in sorted(fn):
                if f.endswith(('.json', '.png', '.mcmeta')):
                    out.append(os.path.join(dp, f))
    return out


def resource_files(assets_root, pack_dir):
    """Список файлов, влияющих на таблицу (отсортированный, абсолютные пути)."""
    files = _walk_files(assets_root, _ASSET_DIRS)
    p = os.path.join(pack_dir, 'reports', 'blocks.json')
    if os.path.isfile(p):
        files.append(p)
    tags = os.path.join(pack_dir, 'data', 'minecraft', 'tags', 'block')
    if os.path.isdir(tags):
        for f in sorted(os.listdir(tags)):
            if f.endswith('.json'):
                files.append(os.path.join(tags, f))
    return files


def _fingerprint(files):
    h = hashlib.sha1()
    for p in files:
        st = os.stat(p)
        h.update(('%s|%d|%d\n' % (p, st.st_size, st.st_mtime_ns)).encode('utf-8', 'surrogateescape'))
    return h.hexdigest()


def _content_hash(files):
    h = hashlib.sha1()
    for p in files:
        h.update('/'.join(p.replace(os.sep, '/').split('/')[-3:]).encode('utf-8', 'surrogateescape') + b'\0')
        with open(p, 'rb') as f:
            while True:
                b = f.read(1 << 20)
                if not b:
                    break
                h.update(b)
    return h.hexdigest()


def resource_hash(assets_root, pack_dir, cache_dir=None):
    """SHA-1 ресурсов (+ FORMAT_VERSION), с мемоизацией по отпечатку файловой системы в cache_dir/fingerprint.json."""
    files = resource_files(assets_root, pack_dir)
    fp = _fingerprint(files)
    memo_path = os.path.join(cache_dir, 'fingerprint.json') if cache_dir else None
    memo = {}
    if memo_path and os.path.isfile(memo_path):
        try:
            with open(memo_path, encoding='utf-8') as f:
                memo = json.load(f)
        except (OSError, ValueError):
            memo = {}
    if memo.get('fp') == fp and memo.get('fmt') == FORMAT_VERSION and memo.get('root') == assets_root + '|' + pack_dir:
        return memo['sha1']
    sha = _content_hash(files)
    sha = hashlib.sha1((sha + '|%d' % FORMAT_VERSION).encode()).hexdigest()
    if memo_path:
        try:
            os.makedirs(cache_dir, exist_ok=True)
            with open(memo_path, 'w', encoding='utf-8') as f:
                json.dump({'fp': fp, 'fmt': FORMAT_VERSION, 'root': assets_root + '|' + pack_dir, 'sha1': sha}, f)
        except OSError:
            pass
    return sha


def cache_path(cache_dir, sha1):
    return os.path.join(cache_dir, 'statetable-%s.npz' % sha1[:16])
