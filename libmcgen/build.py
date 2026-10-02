#!/usr/bin/env python3
"""Кросс-сборка libmcgen (zig cc) под платформы расширения Blender: windows-x64, linux-x64, macos-arm64, macos-x64.

    python3 libmcgen/build.py                          # все 4 платформы -> libmcgen/build/<платформа>/ и blender/mcgen_addon/lib/<платформа>/
    python3 libmcgen/build.py --targets linux-x64      # одна платформа
    python3 libmcgen/build.py --stub --out /tmp/x --no-install --targets linux-x64
                                                       # отладочная сборка ЗАГЛУШКИ blender/tests/addon/stub/mcgen_stub.c (не libmcgen/src)
    python3 libmcgen/build.py --cli                    # дополнительно mcgen-cli для каждой платформы
    python3 libmcgen/build.py --check-exports lib.so   # только проверить экспорт готового бинарника

Что собирается: libmcgen/src/**/*.c (включая src/mesh/*.c — мешер потока W4), engine/ подключается заголовками (-I engine).
Флаги: -O2 -ffp-contract=off -fno-fast-math -std=gnu11 -fPIC -fvisibility=hidden -shared (бит-точность с Java: без FMA и fast-math).
Экспорт ограничен префиксами mcgen_ / mcmesh_ (видимость по умолчанию — только у функций с MCGEN_API/MCMESH_API; проверяется
по таблице экспорта собранного файла: ELF .dynsym, PE export directory, Mach-O nlist). Потоки: pthreads на Linux/macOS, Win32 на Windows —
забота исходников (W1); zig подставляет libc/pthread нужной платформы.

Результат каждой платформы: <папка>/<имя библиотеки> + build-info.json (sha256, размер, цель, флаги, версия zig).
Имена: windows — mcgen.dll, linux — libmcgen.so, macos — libmcgen.dylib (как ожидает core/paths.py).
"""
import argparse
import glob
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
ADDON_LIB = os.path.join(ROOT, 'blender', 'mcgen_addon', 'lib')
STUB = os.path.join(ROOT, 'blender', 'tests', 'addon', 'stub', 'mcgen_stub.c')

# платформа расширения -> (цель zig, имя библиотеки, имя cli)
TARGETS = {
    'windows-x64': ('x86_64-windows-gnu', 'mcgen.dll', 'mcgen-cli.exe'),
    'linux-x64': ('x86_64-linux-gnu.2.28', 'libmcgen.so', 'mcgen-cli'),
    'macos-arm64': ('aarch64-macos.11.0', 'libmcgen.dylib', 'mcgen-cli'),
    'macos-x64': ('x86_64-macos.11.0', 'libmcgen.dylib', 'mcgen-cli'),
}
CFLAGS = ['-O2', '-ffp-contract=off', '-fno-fast-math', '-std=gnu11', '-fPIC', '-fvisibility=hidden',
          '-Wall', '-Wextra', '-Wno-unused-parameter', '-Wno-unused-function', '-Wno-comment', '-Wno-misleading-indentation',
          '-Wno-sign-compare', '-Wno-missing-field-initializers']
ALLOWED_PREFIXES = ('mcgen_', 'mcmesh_')
INCLUDES = [os.path.join(HERE, 'include'), os.path.join(HERE, 'src'), os.path.join(HERE, 'gen'), os.path.join(ROOT, 'engine'),
            os.path.join(HERE, 'src', 'mesh')]


class BuildError(RuntimeError):
    pass


def find_zig():
    z = os.environ.get('ZIG') or shutil.which('zig') or os.path.expanduser('~/.local/bin/zig')
    if not z or not os.path.exists(z):
        sys.exit('zig не найден (ожидается в PATH или ~/.local/bin/zig; переменная ZIG)')
    return z


def sources(stub, src_dir=None):
    if stub:
        return [STUB]
    files = sorted(glob.glob(os.path.join(src_dir or os.path.join(HERE, 'src'), '**', '*.c'), recursive=True))
    return [f for f in files if '/tests/' not in f.replace(os.sep, '/')]


def ensure_tweaks_header():
    """Таблица настроек мира генерируется из tweaks.json (libmcgen/gen/mcgen_tweaks_table.h)."""
    subprocess.run([sys.executable, os.path.join(HERE, 'gen_tweaks.py')], check=True, stdout=subprocess.DEVNULL)


# ---- проверка экспорта ------------------------------------------------------------------------------------------------------

def exported_symbols(path):
    """Имена экспортируемых символов бинарника (ELF/PE/Mach-O, 64-бит little-endian)."""
    data = open(path, 'rb').read()
    if data[:4] == b'\x7fELF':
        return _elf_exports(data)
    if data[:2] == b'MZ':
        return _pe_exports(data)
    magic = struct.unpack('<I', data[:4])[0]
    if magic in (0xFEEDFACF,):
        return _macho_exports(data, 0)
    if data[:4] == b'\xca\xfe\xba\xbe':        # fat (универсальный): объединяем архитектуры
        n = struct.unpack('>I', data[4:8])[0]
        out = set()
        for i in range(n):
            off = struct.unpack('>I', data[16 + 20 * i:20 + 20 * i])[0]
            out |= _macho_exports(data, off)
        return out
    raise ValueError('неизвестный формат бинарника')


def _elf_exports(d):
    shoff, = struct.unpack_from('<Q', d, 0x28)
    shentsize, shnum = struct.unpack_from('<HH', d, 0x3A)
    secs = [struct.unpack_from('<IIQQQQIIQQ', d, shoff + i * shentsize) for i in range(shnum)]
    out = set()
    for s in secs:
        if s[1] == 11:                           # SHT_DYNSYM
            strtab = secs[s[6]]
            nsym = s[5] // 24
            for i in range(nsym):
                name, info, other, shndx, _val, _size = struct.unpack_from('<IBBHQQ', d, s[4] + i * 24)
                if shndx != 0 and (info >> 4) in (1, 2) and (other & 3) == 0:
                    end = d.index(b'\0', strtab[4] + name)
                    nm = d[strtab[4] + name:end].decode()
                    if nm:
                        out.add(nm)
    return out


def _pe_exports(d):
    pe, = struct.unpack_from('<I', d, 0x3C)
    nsec, = struct.unpack_from('<H', d, pe + 6)
    optsize, = struct.unpack_from('<H', d, pe + 20)
    opt = pe + 24
    magic, = struct.unpack_from('<H', d, opt)
    dd = opt + (112 if magic == 0x20B else 96)
    rva, size = struct.unpack_from('<II', d, dd)
    if not rva:
        return set()
    secs = []
    so = opt + optsize
    for i in range(nsec):
        vsize, vaddr, rsize, raddr = struct.unpack_from('<IIII', d, so + 40 * i + 8)
        secs.append((vaddr, max(vsize, rsize), raddr))

    def off(r):
        for va, sz, ra in secs:
            if va <= r < va + sz:
                return r - va + ra
        raise ValueError('RVA вне секций')
    e = off(rva)
    nnames, _addr_fn, addr_names, _ord = struct.unpack_from('<IIII', d, e + 24)
    out = set()
    nm_off = off(addr_names)
    for i in range(nnames):
        nrva, = struct.unpack_from('<I', d, nm_off + 4 * i)
        o = off(nrva)
        out.add(d[o:d.index(b'\0', o)].decode())
    return out


def _macho_exports(d, base):
    ncmds, = struct.unpack_from('<I', d, base + 16)
    off = base + 32
    out = set()
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<II', d, off)
        if cmd == 0x2:                           # LC_SYMTAB
            symoff, nsyms, stroff, _strsize = struct.unpack_from('<IIII', d, off + 8)
            for i in range(nsyms):
                n_strx, n_type, _sect, _desc, _val = struct.unpack_from('<IBBHQ', d, base + symoff + 16 * i)
                if (n_type & 0x01) and (n_type & 0x0E) == 0x0E:          # N_EXT и определён в секции
                    s = base + stroff + n_strx
                    nm = d[s:d.index(b'\0', s)].decode()
                    out.add(nm[1:] if nm.startswith('_') else nm)
        off += size
    return out


def api_functions():
    """Функции публичного ABI из mcgen.h (MCGEN_API ... name()) — они обязаны быть экспортированы."""
    txt = open(os.path.join(HERE, 'include', 'mcgen.h'), encoding='utf-8').read()
    return sorted(set(re.findall(r'MCGEN_API\s+[^;(]*?\b(mcgen_\w+)\s*\(', txt)))


def check_exports(path, require_api=True):
    syms = exported_symbols(path)
    # служебные символы линковщика/рантайма не считаем нарушением
    ignore = re.compile(r'^(_init|_fini|__bss_start|_edata|_end|_DllMainCRTStartup|DllMain|DllMainCRTStartup|__mingw.*|_?_?gnu.*|_mh_dylib_header|__dso_handle|'
                        r'_Z.*|__cxa.*|__tls.*)$')
    bad = sorted(s for s in syms if not s.startswith(ALLOWED_PREFIXES) and not ignore.match(s))
    missing = [f for f in api_functions() if f not in syms] if require_api else []
    return {'exports': sorted(s for s in syms if s.startswith(ALLOWED_PREFIXES)), 'foreign': bad, 'missing_api': missing}


# ---- сборка -----------------------------------------------------------------------------------------------------------------------

def build_target(platform, zig, srcs, out_dir, cli=False, stub=False, extra=None, verbose=False):
    triple, libname, cliname = TARGETS[platform]
    os.makedirs(out_dir, exist_ok=True)
    out = os.path.join(out_dir, libname)
    cmd = [zig, 'cc', '-target', triple] + CFLAGS + (extra or [])
    for inc in INCLUDES:
        cmd += ['-I', inc]
    cmd += ['-shared', '-o', out] + srcs
    if platform.startswith('windows'):
        cmd += ['-lkernel32', '-s']
    elif platform.startswith('macos'):
        cmd += ['-Wl,-install_name,@rpath/' + libname, '-Wl,-x']
    else:
        cmd += ['-lm', '-lpthread', '-s']
    if platform.startswith('macos'):
        cmd += ['-lm']
    t0 = time.time()
    r = subprocess.run(cmd, capture_output=True, text=True)
    dt = time.time() - t0
    if verbose or r.returncode != 0:
        print(r.stderr.strip()[-4000:])
    if r.returncode != 0:
        raise BuildError(f'[{platform}] сборка не удалась (код {r.returncode})')
    for junk in glob.glob(os.path.join(out_dir, '*.lib')) + glob.glob(os.path.join(out_dir, '*.pdb')) + glob.glob(os.path.join(out_dir, '*.dll.a')):
        os.remove(junk)
    info = {
        'platform': platform, 'target': triple, 'library': libname, 'size': os.path.getsize(out),
        'sha256': hashlib.sha256(open(out, 'rb').read()).hexdigest(), 'cflags': CFLAGS, 'zig': subprocess.check_output([zig, 'version'], text=True).strip(),
        'sources': len(srcs), 'stub': stub, 'build_seconds': round(dt, 2), 'warnings': len(re.findall(r'warning:', r.stderr)),
    }
    if cli and not stub:
        cli_src = os.path.join(HERE, 'cli', 'mcgen-cli.c')
        if os.path.exists(cli_src):
            ccmd = [zig, 'cc', '-target', triple] + CFLAGS + [f'-I{i}' for i in INCLUDES] + ['-o', os.path.join(out_dir, cliname), cli_src] + srcs
            ccmd += ['-lm'] + ([] if platform.startswith(('windows', 'macos')) else ['-lpthread'])
            rc = subprocess.run(ccmd, capture_output=True, text=True)
            info['cli'] = cliname if rc.returncode == 0 else f'ошибка: {rc.stderr.strip()[-500:]}'
            for junk in glob.glob(os.path.join(out_dir, '*.pdb')):
                os.remove(junk)
    exp = check_exports(out, require_api=not stub or True)
    info['exports'] = len(exp['exports'])
    info['foreign_exports'] = exp['foreign']
    info['missing_api'] = exp['missing_api']
    with open(os.path.join(out_dir, 'build-info.json'), 'w', encoding='utf-8') as f:
        json.dump(info, f, indent=1, ensure_ascii=False)
    return out, info


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--targets', default='all', help='через запятую: ' + ', '.join(TARGETS) + ' или all')
    ap.add_argument('--stub', action='store_true', help='собрать заглушку тестов (blender/tests/addon/stub), а не libmcgen/src')
    ap.add_argument('--out', help='куда класть <платформа>/ (по умолчанию libmcgen/build)')
    ap.add_argument('--no-install', action='store_true', help='не копировать в blender/mcgen_addon/lib/<платформа>/')
    ap.add_argument('--cli', action='store_true', help='собрать ещё mcgen-cli')
    ap.add_argument('--check-exports', metavar='FILE', help='только проверить экспорт готового бинарника')
    ap.add_argument('--src', help='каталог исходников вместо libmcgen/src (для проверки патчей без правки чужих файлов)')
    ap.add_argument('--flag', action='append', default=[], help='дополнительный флаг компилятора (например -g)')
    ap.add_argument('-v', '--verbose', action='store_true')
    a = ap.parse_args()
    if a.check_exports:
        r = check_exports(a.check_exports)
        print(json.dumps(r, indent=1))
        sys.exit(1 if r['foreign'] or r['missing_api'] else 0)
    targets = list(TARGETS) if a.targets == 'all' else a.targets.split(',')
    for t in targets:
        if t not in TARGETS:
            sys.exit(f'неизвестная платформа {t}; доступны: {", ".join(TARGETS)}')
    zig = find_zig()
    ensure_tweaks_header()
    srcs = sources(a.stub, a.src)
    if a.src:
        INCLUDES.insert(0, os.path.abspath(a.src))
        INCLUDES.insert(1, os.path.join(os.path.abspath(a.src), 'mesh'))
    if not srcs:
        sys.exit('нет исходников: libmcgen/src/**/*.c пуст')
    out_root = a.out or os.path.join(HERE, 'build')
    rc = 0
    print(f'zig {subprocess.check_output([zig, "version"], text=True).strip()}; исходников: {len(srcs)}{" (ЗАГЛУШКА)" if a.stub else ""}')
    failed = []
    for t in targets:
        d = os.path.join(out_root, t)
        try:
            out, info = build_target(t, zig, srcs, d, a.cli, a.stub, a.flag, a.verbose)
        except BuildError as e:           # одна платформа не должна скрывать результат остальных
            print(e)
            failed.append(t)
            rc = 1
            continue
        flag = 'OK' if not info['foreign_exports'] and not info['missing_api'] else 'ЗАМЕЧАНИЯ'
        print(f'[{t}] {info["library"]} {info["size"] / 1024:.0f} KB, {info["build_seconds"]} с, экспорт mcgen_/mcmesh_: {info["exports"]}, {flag}')
        if info['foreign_exports']:
            print('   чужие экспортируемые символы:', ', '.join(info['foreign_exports'][:12]))
            rc = 1
        if info['missing_api']:
            print('   не экспортированы функции mcgen.h:', ', '.join(info['missing_api'][:12]) + (' …' if len(info['missing_api']) > 12 else ''))
            rc = 1 if not a.stub else rc
        if not a.no_install:
            dst = os.path.join(ADDON_LIB, t)
            os.makedirs(dst, exist_ok=True)
            shutil.copy2(out, os.path.join(dst, TARGETS[t][1]))
            shutil.copy2(os.path.join(d, 'build-info.json'), os.path.join(dst, 'build-info.json'))
    if failed:
        print('НЕ СОБРАНЫ:', ', '.join(failed))
    sys.exit(rc)


if __name__ == '__main__':
    main()
