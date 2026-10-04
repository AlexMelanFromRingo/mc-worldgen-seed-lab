"""Сборка необязательной CUDA-библиотеки (libmcgen_cuda / mcgen_cuda.dll) на машине пользователя. Без bpy.

Готового бинарника для вашей видеокарты в аддоне нет: он собирается из наших исходников (папка gpu_src/ в пакете или libmcgen/gpu/ в репозитории)
вашим же nvcc под ваш compute capability. Нужны:
  * Windows: CUDA Toolkit (nvcc) и «Visual Studio Build Tools» с рабочей нагрузкой «Разработка классических приложений на C++» (nvcc на Windows требует cl.exe);
  * Linux: CUDA Toolkit и gcc/g++.
Результат кладётся в <кэш аддона>/gpu/ и подключается через mcgen_gpu_set_library_path; побитовое совпадение с CPU проверяет самопроверка «GPU Self-test»
(в режиме Auto GPU без неё не используется). Параметры сборки те же, что в libmcgen/gpu/build.sh / build.bat: --fmad=false, без fast-math, /fp:strict.
"""
import glob
import hashlib
import json
import locale
import os
import re
import shutil
import subprocess
import sys
import threading
import time

from . import paths
from .pack import PackError, check_children_visible
from .tasks import Cancelled

WINDOWS = sys.platform.startswith('win')
LIB_NAME = 'mcgen_cuda.dll' if WINDOWS else 'libmcgen_cuda.so'
SRC_FILES = ('mcgpu_core.cu', 'mcgpu_biome.cu', 'mcgpu_terrain.cu')
DEFAULT_ARCHES = ('75', '80', '86', '89')     # если драйвер не сказал, какая у вас карта: все основные поколения (дольше собирается)
PTX_ARCH = '75'                                # вперёд-совместимость: драйвер доскомпилирует PTX для более новых карт


# ---- исходники и результат -----------------------------------------------------------------------------------------------

def sources_dir():
    """Каталог libmcgen/gpu с исходниками .cu (в пакете аддона — gpu_src/libmcgen/gpu, из репозитория — libmcgen/gpu) или None.
    Заголовки движка лежат рядом по тому же относительному пути: ../../engine."""
    cands = [os.path.join(paths.addon_dir(), 'gpu_src', 'libmcgen', 'gpu')]
    root = paths.repo_root()
    if root:
        cands.append(os.path.join(root, 'libmcgen', 'gpu'))
    for d in cands:
        if os.path.isfile(os.path.join(d, SRC_FILES[0])) and os.path.isfile(os.path.join(d, '..', '..', 'engine', 'mc_rng.h')):
            return d
    return None


def stage_sources(src_dir, out_dir):
    """Копия исходников (libmcgen/gpu + ../../engine) в <кэш>/gpu/src/. Внешние программы (cl.exe, nvcc) не видят файлов расширения в AppData у Blender из Microsoft Store
    (виртуализация: «cannot open source file … mcgpu_core.cu»), а каталог кэша им виден (его проверяет check_children_visible). Возвращает каталог libmcgen/gpu копии."""
    root = os.path.join(out_dir, 'src')
    dst_gpu = os.path.join(root, 'libmcgen', 'gpu')
    dst_eng = os.path.join(root, 'engine')
    shutil.rmtree(root, ignore_errors=True)
    os.makedirs(dst_gpu)
    os.makedirs(dst_eng)
    for fn in sorted(os.listdir(src_dir)):
        if fn.endswith(('.cu', '.cuh', '.h', '.map')):
            shutil.copyfile(os.path.join(src_dir, fn), os.path.join(dst_gpu, fn))
    eng = os.path.normpath(os.path.join(src_dir, '..', '..', 'engine'))
    for fn in ('mc_rng.h', 'mc_common.h'):
        shutil.copyfile(os.path.join(eng, fn), os.path.join(dst_eng, fn))
    return dst_gpu


def gpu_dir(create=False):
    d = os.path.join(paths.cache_dir(create), 'gpu')
    if create:
        os.makedirs(d, exist_ok=True)
    return d


def built_library():
    """Путь к собранной аддоном библиотеке или None."""
    p = os.path.join(gpu_dir(), LIB_NAME)
    return p if os.path.isfile(p) else None


def sources_hash(src_dir):
    h = hashlib.sha1()
    for fn in sorted(os.listdir(src_dir)):
        if fn.endswith(('.cu', '.cuh', '.h', '.map')):
            with open(os.path.join(src_dir, fn), 'rb') as f:
                h.update(fn.encode() + b'\0' + f.read())
    return h.hexdigest()[:12]


def stamp():
    try:
        with open(os.path.join(gpu_dir(), 'build.json'), encoding='utf-8') as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def activate(dll, libmcgen_path):
    """Подключает собранную библиотеку к libmcgen (если рядом с libmcgen своей нет). Возвращает путь или None.
    Если прошлая сборка не смогла заменить загруженный файл (Windows блокирует DLL) — доводит замену до конца при запуске."""
    try:
        gd = gpu_dir()
        pending = os.path.join(gd, LIB_NAME + '.new')
        if os.path.isfile(pending):
            try:
                os.replace(pending, os.path.join(gd, LIB_NAME))
            except OSError:
                pass
        p = built_library()
        if not p:
            return None
        if libmcgen_path and os.path.isfile(os.path.join(os.path.dirname(libmcgen_path), LIB_NAME)):
            return None
        fn = getattr(dll, 'mcgen_gpu_set_library_path', None)
        if fn is None:
            return None
        import ctypes as C
        fn.restype, fn.argtypes = C.c_int, [C.c_char_p]
        fn(os.fsencode(p))
        return p
    except OSError:
        return None


# ---- поиск инструментов --------------------------------------------------------------------------------------------------

def _version_key(p):
    m = re.findall(r'\d+', p)
    return [int(x) for x in m]


def find_nvcc(hint=''):
    """nvcc: подсказка (файл или каталог CUDA) -> PATH -> CUDA_PATH / CUDA_HOME -> типичные места установки. None, если нет."""
    exe = 'nvcc.exe' if WINDOWS else 'nvcc'
    cands = []
    if hint:
        cands += [hint, os.path.join(hint, 'bin', exe)]
    w = shutil.which('nvcc')
    if w:
        cands.append(w)
    for env in ('CUDA_PATH', 'CUDA_HOME', 'CUDA_ROOT', 'CUDA_TOOLKIT_ROOT_DIR'):
        v = os.environ.get(env)
        if v:
            cands.append(os.path.join(v, 'bin', exe))
    if WINDOWS:
        for pf in {os.environ.get('ProgramFiles', r'C:\Program Files'), os.environ.get('ProgramW6432', r'C:\Program Files')}:
            cands += sorted(glob.glob(os.path.join(pf, 'NVIDIA GPU Computing Toolkit', 'CUDA', 'v*', 'bin', exe)), key=_version_key, reverse=True)
    else:
        cands += ['/usr/local/cuda/bin/nvcc', '/usr/bin/nvcc'] + sorted(glob.glob('/usr/local/cuda-*/bin/nvcc'), key=_version_key, reverse=True)
    for c in cands:
        if c and os.path.isfile(c):
            return os.path.abspath(c)
    return None


def nvcc_version(nvcc):
    """'12.0' из `nvcc --version` или ''."""
    try:
        r = subprocess.run([nvcc, '--version'], capture_output=True, text=True, timeout=60, errors='replace', creationflags=paths.NO_WINDOW)
        m = re.search(r'release (\d+\.\d+)', r.stdout + r.stderr)
        return m.group(1) if m else ''
    except (OSError, subprocess.SubprocessError):
        return ''


def nvcc_supported(nvcc, what='code'):
    """Множество поддерживаемых этим nvcc архитектур ('75', '89', …) по `nvcc --list-gpu-code|--list-gpu-arch`; пусто — не удалось узнать."""
    try:
        r = subprocess.run([nvcc, '--list-gpu-' + what], capture_output=True, text=True, timeout=60, errors='replace', creationflags=paths.NO_WINDOW)
        return {m for m in re.findall(r'(?:sm|compute)_(\d+)', r.stdout)}
    except (OSError, subprocess.SubprocessError):
        return set()


def find_nvidia_smi():
    w = shutil.which('nvidia-smi')
    if w:
        return w
    if WINDOWS:
        for p in (os.path.join(os.environ.get('SystemRoot', r'C:\Windows'), 'System32', 'nvidia-smi.exe'),
                  os.path.join(os.environ.get('ProgramFiles', r'C:\Program Files'), 'NVIDIA Corporation', 'NVSMI', 'nvidia-smi.exe')):
            if os.path.isfile(p):
                return p
    return None


def parse_compute_caps(text):
    """'8.9\\n8.6' -> ['86', '89']."""
    return sorted({m.group(1) + m.group(2) for m in (re.fullmatch(r'\s*(\d+)\.(\d+)\s*', ln) for ln in text.splitlines()) if m})


def detect_arches():
    """compute capability установленных карт по nvidia-smi: ['89']; пусто, если драйвер не ответил."""
    smi = find_nvidia_smi()
    if not smi:
        return []
    try:
        r = subprocess.run([smi, '--query-gpu=compute_cap', '--format=csv,noheader'], capture_output=True, text=True, timeout=30, errors='replace', creationflags=paths.NO_WINDOW)
        return parse_compute_caps(r.stdout) if r.returncode == 0 else []
    except (OSError, subprocess.SubprocessError):
        return []


def find_vcvars():
    """vcvars64.bat Visual Studio / Build Tools с компонентом «C++ x64» (через vswhere и по типичным путям) или None. Только Windows."""
    if not WINDOWS:
        return None
    pf86 = os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)')
    vsw = os.path.join(pf86, 'Microsoft Visual Studio', 'Installer', 'vswhere.exe')
    if os.path.isfile(vsw):
        for extra, enc in ((['-utf8'], 'utf-8'), ([], 'mbcs')):
            try:
                r = subprocess.run([vsw, '-latest', '-products', '*', '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath'] + extra,
                                   capture_output=True, timeout=60, creationflags=paths.NO_WINDOW)
                if r.returncode == 0 and r.stdout.strip():
                    bat = os.path.join(r.stdout.decode(enc, 'replace').strip().splitlines()[0], 'VC', 'Auxiliary', 'Build', 'vcvars64.bat')
                    if os.path.isfile(bat):
                        return bat
            except (OSError, subprocess.SubprocessError):
                pass
    for pf in (os.environ.get('ProgramFiles', r'C:\Program Files'), pf86):
        hits = sorted(glob.glob(os.path.join(pf, 'Microsoft Visual Studio', '*', '*', 'VC', 'Auxiliary', 'Build', 'vcvars64.bat')), reverse=True)
        if hits:
            return hits[0]
    return None


def toolchain(nvcc_hint=''):
    """Что нашлось для сборки: {'nvcc','nvcc_version','vcvars','cl_in_path','arches','sources','problem': (шаблон, kw)|None}."""
    t = {'nvcc': find_nvcc(nvcc_hint), 'vcvars': None, 'cl_in_path': False, 'arches': detect_arches(), 'sources': sources_dir(), 'problem': None}
    t['nvcc_version'] = nvcc_version(t['nvcc']) if t['nvcc'] else ''
    if WINDOWS:
        t['cl_in_path'] = bool(shutil.which('cl'))
        t['vcvars'] = None if t['cl_in_path'] else find_vcvars()
    if not t['sources']:
        t['problem'] = ('The CUDA sources were not found in this installation (gpu_src folder).', {})
    elif not t['nvcc']:
        t['problem'] = ('nvcc (CUDA Toolkit) was not found. Install the CUDA Toolkit from developer.nvidia.com/cuda-downloads and restart Blender (or set CUDA_PATH).', {})
    elif WINDOWS and not t['cl_in_path'] and not t['vcvars']:
        t['problem'] = ('The Microsoft C++ compiler was not found: nvcc on Windows needs "Visual Studio Build Tools" with the "Desktop development with C++" workload (free, visualstudio.microsoft.com/downloads).', {})
    return t


# ---- команда сборки ------------------------------------------------------------------------------------------------------

def nvcc_command(nvcc, src_dir, out, arches, ptx=PTX_ARCH, windows=WINDOWS, extra=()):
    """Аргументы nvcc — те же флаги, что в libmcgen/gpu/build.sh (Linux) и build.bat (Windows): бит-точность с CPU (--fmad=false, без fast-math)."""
    gen = []
    for a in arches:
        gen += ['-gencode', f'arch=compute_{a},code=sm_{a}']
    gen += ['-gencode', f'arch=compute_{ptx},code=compute_{ptx}']
    cmd = [nvcc, '-O3', '-std=c++17', '--shared'] + gen + ['--fmad=false', '-prec-div=true', '-prec-sqrt=true', '-ftz=false', '-Xptxas', '-O3', '-lineinfo']
    if windows:
        cmd += ['-Xcompiler', '/fp:strict,/MT,/EHsc']
    else:
        cmd += ['-Xcompiler', '-fPIC,-ffp-contract=off,-fvisibility=hidden', '-Xlinker', '-z,noexecstack', '-Xcompiler', '-static-libstdc++,-static-libgcc',
                '-Xlinker', '--version-script=' + os.path.join(src_dir, 'mcgpu.map')]
    cmd += ['-I', src_dir, '-I', os.path.normpath(os.path.join(src_dir, '..', '..', 'engine'))] + list(extra) + ['-o', out]
    return cmd + [os.path.join(src_dir, f) for f in SRC_FILES if os.path.isfile(os.path.join(src_dir, f))]


def pick_arches(nvcc, detected):
    """(список sm_XX для cubin, архитектура PTX): только то, что умеет этот nvcc; для более новых карт остаётся PTX."""
    sm = nvcc_supported(nvcc, 'code')
    cp = nvcc_supported(nvcc, 'arch')
    want = list(detected) if detected else list(DEFAULT_ARCHES)
    arches = [a for a in want if not sm or a in sm]
    ptx = PTX_ARCH if (not cp or PTX_ARCH in cp) else (sorted(cp, key=int)[0] if cp else PTX_ARCH)
    if not arches and not detected:
        arches = [a for a in DEFAULT_ARCHES if not sm or a in sm]
    return arches, ptx


def _bat_quote(a):
    a = a.replace('%', '%%')
    return '"' + a.replace('"', '\\"') + '"' if re.search(r'[\s&()^;,=!]', a) else a


def write_build_bat(path, cmd, vcvars):
    lines = ['@echo off', 'chcp 65001 >nul']                    # вывод cmd/cl/nvcc в UTF-8: журнал читается на любой локали
    if vcvars:
        lines += [f'call "{vcvars}" >nul', 'if errorlevel 1 exit /b 101']
    lines += [' '.join(_bat_quote(a) for a in cmd), 'exit /b %errorlevel%']
    with open(path, 'w', encoding='utf-8', newline='') as f:
        f.write('\r\n'.join(lines) + '\r\n')


def _kill(proc):
    try:
        if WINDOWS:
            subprocess.run(['taskkill', '/T', '/F', '/PID', str(proc.pid)], capture_output=True, timeout=30, creationflags=paths.NO_WINDOW)
        else:
            proc.kill()
    except (OSError, subprocess.SubprocessError):
        pass


def oem_codepage():
    """OEM-кодовая страница системной локали (866 для русской) независимо от режима «Unicode UTF-8 для всех языков»; 0 — неизвестно / не Windows."""
    if not WINDOWS:
        return 0
    try:
        import ctypes
        buf = ctypes.create_unicode_buffer(16)
        if ctypes.windll.kernel32.GetLocaleInfoW(0x0800, 0x0B, buf, 16):          # LOCALE_SYSTEM_DEFAULT, LOCALE_IDEFAULTCODEPAGE
            return int(buf.value)
    except (OSError, AttributeError, ValueError):
        pass
    return 0


def decode_output(raw, oem=None):
    """Строка вывода дочерней программы -> текст: UTF-8 (chcp 65001), иначе OEM-кодовая страница (cl.exe, cmd до chcp), иначе с заменой."""
    try:
        return raw.decode('utf-8')
    except UnicodeDecodeError:
        pass
    cp = oem if oem is not None else oem_codepage()
    if cp and cp != 65001:
        try:
            return raw.decode(f'cp{cp}')
        except (UnicodeDecodeError, LookupError):
            pass
    return raw.decode('utf-8', 'replace')


def _run(task, argv, cwd, label):
    """Запускает argv, читает вывод в журнал, передаёт прогресс по времени, по отмене убивает дерево процессов. -> (код, текст вывода)."""
    proc = subprocess.Popen(argv, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, creationflags=paths.NO_WINDOW)      # байты: кодировку строки определяет decode_output
    lines = []
    oem = oem_codepage()

    def reader():
        for ln in proc.stdout:
            lines.append(decode_output(ln.rstrip(b'\r\n'), oem))

    th = threading.Thread(target=reader, daemon=True)
    th.start()
    t0 = time.time()
    try:
        while proc.poll() is None:
            try:
                task.check()
            except Cancelled:
                _kill(proc)
                raise
            el = time.time() - t0
            task.report(min(0.95, 0.05 + 0.9 * (1 - 1 / (1 + el / 120.0))), f'{label} {int(el)} s')
            time.sleep(0.25)
    finally:
        th.join(timeout=5)
        proc.stdout.close()
    return proc.returncode, '\n'.join(lines)


# ---- сборка --------------------------------------------------------------------------------------------------------------

def _problem(tc):
    p = tc['problem']
    if p:
        raise PackError(p[0], **p[1])


def build(task, nvcc_hint='', arches=None, keep_log=True):
    """Собирает библиотеку в <кэш>/gpu/. Возвращает словарь {'path','nvcc','arches','seconds','pending'}; ошибки — PackError с текстом для пользователя."""
    tc = toolchain(nvcc_hint)
    _problem(tc)
    check_children_visible()                          # nvcc и cmd должны видеть файлы кэша (Blender из Microsoft Store их прячет в AppData)
    out_dir = gpu_dir(create=True)
    src_dir = stage_sources(tc['sources'], out_dir)      # копия в кэше: cl.exe/nvcc не видят AppData расширения у Store-Blender
    work = os.path.join(out_dir, 'work')
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    sel, ptx = pick_arches(tc['nvcc'], arches if arches is not None else tc['arches'])
    tmp_out = os.path.join(work, LIB_NAME)
    t0 = time.time()
    log = ''
    attempts = [(), ('-allow-unsupported-compiler',)]      # вторая попытка — если nvcc отказался от слишком нового Visual Studio / GCC
    rc = 1
    for i, extra in enumerate(attempts):
        task.report(0.02, 'nvcc …')
        cmd = nvcc_command(tc['nvcc'], src_dir, tmp_out, sel, ptx, extra=extra)
        if WINDOWS:
            bat = os.path.join(work, 'build_gpu.bat')
            write_build_bat(bat, cmd, tc['vcvars'])
            argv = [os.environ.get('COMSPEC', 'cmd.exe'), '/d', '/c', 'build_gpu.bat']
        else:
            argv = cmd
        rc, log = _run(task, argv, work, 'nvcc')
        if rc == 0 and os.path.isfile(tmp_out):
            break
        if i == 0 and re.search(r'unsupported (Microsoft Visual Studio|GNU|clang) version', log, re.I):
            task.report(None, 'retry with -allow-unsupported-compiler')
            continue
        break
    if keep_log:
        try:
            with open(os.path.join(out_dir, 'build.log'), 'w', encoding='utf-8') as f:
                f.write(' '.join(_bat_quote(a) for a in cmd) + '\n\n' + log + '\n')
        except OSError:
            pass
    if rc != 0 or not os.path.isfile(tmp_out):
        tail = '\n'.join(log.strip().splitlines()[-12:])
        raise PackError('nvcc failed (code {code}). The full log is in {log}\n{tail}', code=rc, log=os.path.join(out_dir, 'build.log'), tail=tail)
    final = os.path.join(out_dir, LIB_NAME)
    pending = False
    try:
        os.replace(tmp_out, final)
    except OSError:                                    # Windows держит загруженный DLL: положим рядом, заменится при следующем запуске
        os.replace(tmp_out, final + '.new')
        pending = True
    shutil.rmtree(work, ignore_errors=True)
    secs = time.time() - t0
    info = {'path': final, 'nvcc': tc['nvcc_version'], 'arches': sel, 'ptx': ptx, 'sources': sources_hash(src_dir), 'seconds': round(secs, 1),
            'built': time.strftime('%Y-%m-%d %H:%M:%S'), 'pending': pending}
    with open(os.path.join(out_dir, 'build.json'), 'w', encoding='utf-8') as f:
        json.dump(info, f, indent=1)
    task.report(1.0, 'done')
    return info


def build_worker(task, nvcc_hint=''):
    return build(task, nvcc_hint)
