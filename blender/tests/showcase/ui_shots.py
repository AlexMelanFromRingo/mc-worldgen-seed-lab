"""Снимки интерфейса аддона в НАСТОЯЩЕМ GUI Blender (под Xvfb): панели, боковая панель N, прогресс генерации, карта биомов.

Запускается драйвером run_ui_shots.py (он готовит профиль, Xvfb нужной геометрии и вызывает этот файл на каждый режим и язык):

    xvfb-run -a -s "-screen 0 700x4600x24" blender --factory-startup --window-geometry 0 0 700 4600 \
        --python blender/tests/showcase/ui_shots.py -- --mode panels --lang en --out <каталог>

Режимы:  panels   — всё Properties-окно «Scene → MC World» (все подпанели раскрыты) одним высоким кадром ui-panels-<lang>.png
         viewport — 3D-вид с боковой панелью N и сгенерированным миром                 -> ui-viewport-<lang>.jpg
         progress — вид во время генерации (полоса прогресса, статус-бар)               -> ui-progress-<lang>.jpg
         biomemap — вид после «Biome Map» (плоскость с картой биомов)                  -> ui-biomemap-<lang>.jpg
Мир — настоящая libmcgen, настоящий SceneBuilder, сид 29 (деревня), область 12×10 чанков. Ресурсы Mojang — из run/pack-26.3, run/assets-26.3 (не в репозитории).
"""
import argparse
import importlib
import json
import os
import sys
import time

import addon_utils
import bpy

HERE = os.path.dirname(os.path.abspath(__file__)) if '__file__' in globals() else os.getcwd()
REPO = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
EXT = os.environ.get('MCGEN_EXT_MODULE', 'bl_ext.user_default.mcgen')


def parse():
    argv = sys.argv[sys.argv.index('--') + 1:] if '--' in sys.argv else []
    ap = argparse.ArgumentParser()
    ap.add_argument('--mode', default='panels', choices=['prepare', 'panels', 'viewport', 'progress', 'biomemap'])
    ap.add_argument('--lang', default='en', choices=['en', 'ru'])
    ap.add_argument('--out', required=True)
    ap.add_argument('--variant', default='default', choices=['default', 'split'], help='split: раздельные сиды (в т. ч. текстовый) и включённые «настройки мира»')
    ap.add_argument('--seed', default='29')
    ap.add_argument('--cx0', type=int, default=40)
    ap.add_argument('--cz0', type=int, default=-68)
    return ap.parse_args(argv)


A = parse()
os.makedirs(A.out, exist_ok=True)


def log(*a):
    print('UI-SHOTS', *a, flush=True)


# --- раскрываем все подпанели (в аддоне часть — DEFAULT_CLOSED) ДО регистрации: панели создаются из списка SUBPANELS ----------------------
# panels — все; прочие режимы — только нужные (боковая панель N невысокая)
OPEN = {'panels': None, 'viewport': {'world', 'seeds', 'edit'}, 'progress': {'world', 'seeds', 'area'}, 'biomemap': {'world', 'biomes'}, 'prepare': set()}[A.mode]
panels_mod = importlib.import_module(EXT + '.ui.panels')
panels_mod.SUBPANELS[:] = [(k, l, f, (False if OPEN is None or k in OPEN else True), h) for (k, l, f, c, h) in panels_mod.SUBPANELS]
addon_utils.enable(EXT, default_set=True, persistent=False, handle_error=lambda e: (_ for _ in ()).throw(e))
props = importlib.import_module(EXT + '.ui.props')
jobs = importlib.import_module(EXT + '.core.jobs')

prefs = props.get_prefs()
ver = '26.3'
# ресурсы берутся из КЭША аддона (MCGEN_CACHE): сначала режим prepare — настоящая «Prepare Resources» по jar из jars/ (в репозиторий не входят);
# поля путей к jar после подготовки очищаются, чтобы в кадрах не было личных путей
prefs.sink = 'AUTO'
view = bpy.context.preferences.view
if A.lang == 'ru':
    view.language = 'ru_RU'
    view.use_translate_interface = True
    view.use_translate_tooltips = True
view.show_splash = False
bpy.context.preferences.view.ui_scale = 1.0


def configure_scene():
    for o in list(bpy.data.objects):
        bpy.data.objects.remove(o, do_unlink=True)
    s = bpy.context.scene.mcgen
    s.version = ver
    s.dimension = 'minecraft:overworld'
    s.seed_mode = 'UNIFIED'
    s.seed = A.seed
    s.unit = 'CHUNKS'
    s.origin_x, s.origin_z = A.cx0, A.cz0
    s.size_x, s.size_z = (12, 10)
    s.use_terrain = s.use_surface = s.use_caves = s.use_features = s.use_structures = True
    s.y_min = 40
    return s


def find_area(kind):
    for win in bpy.context.window_manager.windows:
        for ar in win.screen.areas:
            if ar.type == kind:
                return win, ar
    return None, None


def area_override(win, ar, region_type='WINDOW'):
    reg = next((r for r in ar.regions if r.type == region_type), None)
    return bpy.context.temp_override(window=win, area=ar, region=reg)


def maximize(win, ar, hide_panels=True):
    with area_override(win, ar):
        bpy.ops.screen.screen_full_area(use_hide_panels=hide_panels)


def redraw_all():
    for win in bpy.context.window_manager.windows:
        for ar in win.screen.areas:
            ar.tag_redraw()
    try:
        bpy.ops.wm.redraw_timer(type='DRAW_WIN_SWAP', iterations=2)
    except Exception:    # noqa: BLE001
        pass


def _grab(path):
    win = bpy.context.window_manager.windows[0]
    redraw_all()
    with bpy.context.temp_override(window=win, area=win.screen.areas[0]):
        bpy.ops.screen.screenshot(filepath=path, check_existing=False)


def shoot(name, fmt='PNG', stable=True):   # всегда PNG (Blender пишет в формате сцены); JPEG делает драйвер
    """Снимок окна целиком (после макс. размера области — это сама область). Генератор: перед снимком ждёт, пока раскладка устоится — два снимка подряд
    (с паузой) должны совпасть побайтно (панели Blender анимируют высоту, при медленной отрисовке в Xvfb первый кадр может быть «в движении»)."""
    import hashlib
    win = bpy.context.window_manager.windows[0]
    try:                                   # указатель в угол окна, чтобы не всплывали подсказки
        win.cursor_warp(win.width - 2, 2)
    except Exception:    # noqa: BLE001
        pass
    path = os.path.join(A.out, name)
    prev = None
    for attempt in range(12 if stable else 1):
        _grab(path)
        h = hashlib.sha1(open(path, 'rb').read()).hexdigest()
        if h == prev:
            break
        prev = h
        yield 1.0
    log('saved', path, os.path.getsize(path) if os.path.exists(path) else None, 'attempts', attempt + 1)


# --- сценарии -------------------------------------------------------------------------------------------------------------------------
def scenario_prepare():
    prefs.server_jar = os.environ.get('MCGEN_SERVER_JAR', os.path.join(REPO, 'jars', 'server-%s.jar' % ver))
    prefs.client_jar = os.environ.get('MCGEN_CLIENT_JAR', os.path.join(REPO, 'jars', 'client-%s.jar' % ver))
    yield 0.5
    t0 = time.time()
    bpy.ops.mcgen.prepare_resources('EXEC_DEFAULT')
    log('prepare', round(time.time() - t0, 1), 's')
    prefs.server_jar = prefs.client_jar = ''
    yield 0.2


def scenario_panels():
    s = configure_scene()
    if A.variant == 'split':
        s.seed_mode = 'SPLIT'
        s.seed_climate, s.seed_terrain, s.seed_structures, s.seed_features = '5', 'Hello World', '2026', '5'
        s.use_tweaks = True
        s.tweaks.cave_density = 1.6
        s.tweaks.terrain_amplitude = 1.25
        s.tweaks.sea_level_offset = 6
    win, ar = find_area('PROPERTIES')
    sp = ar.spaces.active
    sp.context = 'SCENE'
    yield 0.5
    bpy.ops.mcgen.generate('EXEC_DEFAULT')       # статистика и список построек для панели Stats
    log('world', s.seed_mode, s.seed, s.seed_climate, s.seed_terrain, s.seed_structures, s.seed_features, 'faces', s.stats.faces, [(e.name, tuple(e.bb)) for e in s.stats.structure_starts])
    yield 0.5
    maximize(win, ar)
    win, ar = find_area('PROPERTIES')
    sp = ar.spaces.active
    for ctx in ('RENDER', 'SCENE'):         # смена вкладки заставляет Properties заново посчитать высоты подпанелей (иначе после Generate они могут остаться «старыми»)
        sp.context = ctx
        for _ in range(3):
            redraw_all()
            yield 0.6
    yield from shoot(f'panels-{A.variant}-{A.lang}.png')
    yield 0.2


def setup_viewport(ar_win=None):
    win, ar = find_area('VIEW_3D')
    sp = ar.spaces.active
    sp.show_region_ui = True
    sp.shading.type = 'MATERIAL'
    sp.overlay.show_overlays = False
    sp.show_gizmo = False
    return win, ar, sp


def only_mc_tab():
    """Оставляет в боковой панели 3D-вида одну вкладку «MC World»: снимает с регистрации встроенные панели вкладок Item/Tool/View (только в
    этом процессе, для кадра): Region.active_panel_category в Blender 4.5 только для чтения, поэтому выбрать вкладку программно нельзя."""
    n = 0
    for cls in list(bpy.types.Panel.__subclasses__()):
        try:
            if getattr(cls, 'bl_space_type', '') == 'VIEW_3D' and getattr(cls, 'bl_region_type', '') == 'UI' and getattr(cls, 'bl_category', '') in ('Item', 'Tool', 'View'):
                bpy.utils.unregister_class(cls)
                n += 1
        except Exception:    # noqa: BLE001
            pass
    log('unregistered built-in sidebar panels:', n)


def click_mc_tab(win, ar):
    only_mc_tab()          # имя осталось от попыток выбрать вкладку кликом (XTest указатель Blender под Xvfb не принимает)


def frame_scene(sp, ar, win):
    """Камера вида 3D: смотрим на сгенерированную область сверху-сбоку."""
    import mathutils
    xs, ys, zs = [], [], []
    for o in bpy.data.objects:
        if o.name.startswith('mc_') and o.type == 'MESH':
            for c in o.bound_box:
                v = o.matrix_world @ mathutils.Vector(c)
                xs.append(v.x)
                ys.append(v.y)
                zs.append(v.z)
    if not xs:
        return
    cx, cy = (min(xs) + max(xs)) / 2, (min(ys) + max(ys)) / 2
    r3d = sp.region_3d
    r3d.view_perspective = 'PERSP'
    r3d.view_location = (cx, cy, sorted(zs)[len(zs) // 2] - 10)
    r3d.view_rotation = mathutils.Euler((1.0, 0.0, 0.35), 'XYZ').to_quaternion()
    r3d.view_distance = 330
    sp.clip_end = 5000


def build_cottage():
    """Домик, построенный инструментами редактирования (EditSession аддона): стены, окна, дверь, крыша ступенями, забор, факел."""
    import numpy as np
    edit_ops = importlib.import_module(EXT + '.render.edit_ops')
    mesh_edit = importlib.import_module(EXT + '.mesh.edit')
    sess = jobs.session(bpy.context.scene.name)
    reg = sess.region
    sb = edit_ops.STATE.sb
    ed = edit_ops.STATE.sb.get_edit_session()
    cx0, cz0, nx, nz = reg.info.cx0, reg.info.cz0, reg.info.nx, reg.info.nz
    # плоский участок без деревьев (окно 19×19): высоты MOTION_BLOCKING и ..._NO_LEAVES совпадают, перепад <= 1
    from numpy.lib.stride_tricks import sliding_window_view as swv
    H2 = np.zeros((nz * 16, nx * 16), int)
    H3 = np.zeros_like(H2)
    for cz in range(nz):
        for cx in range(nx):
            H2[cz * 16:(cz + 1) * 16, cx * 16:(cx + 1) * 16] = reg.heightmap(cx0 + cx, cz0 + cz, 2)
            H3[cz * 16:(cz + 1) * 16, cx * 16:(cx + 1) * 16] = reg.heightmap(cx0 + cx, cz0 + cz, 3)
    Wn = 19
    bad = swv((H2 != H3).astype(int), (Wn, Wn)).sum(axis=(2, 3))
    hi = swv(H3, (Wn, Wn)).max(axis=(2, 3))
    lo = swv(H3, (Wn, Wn)).min(axis=(2, 3))
    ok = (bad == 0) & (hi - lo <= 1)
    best = None
    if ok.any():
        zz, xx = np.nonzero(ok)
        d = np.abs(xx + Wn / 2 - nx * 8) + np.abs(zz + Wn / 2 - nz * 8)
        k = int(np.argmin(d))
        best = (0, 0, 0, int(lo[zz[k], xx[k]]))
        gx0, gz0 = cx0 * 16 + int(xx[k]) + 5, cz0 * 16 + int(zz[k]) + 5
    if best is None:
        return None
    h = best[3]
    x0, z0, W = gx0, gz0, 9
    Ctx = mesh_edit.PlaceContext
    N, S_, Wd, E = 2, 3, 4, 5
    ed.begin('cottage')
    P = lambda x, y, z, b, look=2, face=1, hit=(0.5, 0.2, 0.5): ed.place(x, y, z, 'minecraft:' + b, Ctx(face=face, hit=hit, look=look))   # noqa: E731
    for x in range(x0 - 2, x0 + W + 2):
        for z in range(z0 - 2, z0 + W + 4):
            for y in range(h, h + 7):
                ed.break_block(x, y, z)                                   # убрать траву и цветы на участке
    for x in range(x0, x0 + W):
        for z in range(z0, z0 + W):
            P(x, h - 1, z, 'cobblestone')                                 # пол
    for y in range(h, h + 3):
        for x in range(x0, x0 + W):
            for z in (z0, z0 + W - 1):
                P(x, y, z, 'oak_log' if x in (x0, x0 + W - 1) else 'oak_planks')
        for z in range(z0 + 1, z0 + W - 1):
            for x in (x0, x0 + W - 1):
                P(x, y, z, 'oak_planks')
    for z in (z0, z0 + W - 1):
        for x in (x0 + 2, x0 + 4, x0 + 6):
            P(x, h + 1, z, 'glass_pane')
    for x in (x0, x0 + W - 1):
        P(x, h + 1, z0 + 4, 'glass_pane')
    ed.break_block(x0 + 4, h, z0 + W - 1)
    ed.break_block(x0 + 4, h + 1, z0 + W - 1)
    P(x0 + 4, h, z0 + W - 1, 'oak_door', look=N)
    for k, (a, b_) in enumerate(((x0 - 1, x0 + W), (x0, x0 + W - 1), (x0 + 1, x0 + W - 2))):
        y = h + 3 + k
        lo, hi = a, b_
        zl, zh = (z0 - 1 + k, z0 + W + 0 - k)
        for x in range(lo, hi + 1):
            P(x, y, zl, 'oak_stairs', look=S_)
            P(x, y, zh, 'oak_stairs', look=N)
        for z in range(zl + 1, zh):
            P(lo, y, z, 'oak_stairs', look=E)
            P(hi, y, z, 'oak_stairs', look=Wd)
    for x in range(x0 + 3, x0 + 6):
        for z in range(z0 + 3, z0 + 6):
            P(x, h + 5, z, 'oak_planks')
    for x in range(x0 - 2, x0 + W + 2):
        if x not in range(x0 + 3, x0 + 6):
            P(x, h, z0 + W + 3, 'oak_fence')
    P(x0 + 4, h, z0 + W + 1, 'torch')
    n = ed.commit()
    sb.update_chunks(ed.take_affected())
    return {'blocks': n, 'at': (x0, h, z0)}


def scenario_viewport():
    s = configure_scene()
    win, ar, sp = setup_viewport()
    maximize(win, ar, hide_panels=False)
    yield 1.0
    win, ar = find_area('VIEW_3D')
    click_mc_tab(win, ar)
    yield 1.0
    sp = ar.spaces.active
    bpy.ops.mcgen.generate('EXEC_DEFAULT')
    bpy.ops.mcgen.structure_markers('EXEC_DEFAULT')
    info = build_cottage()
    log('cottage', info)
    frame_scene(sp, ar, win)
    if info:
        import mathutils
        x0, h, z0 = info['at']
        sp.region_3d.view_location = (x0 + 4, -(z0 + 4), h + 3)
        sp.region_3d.view_distance = 55
    yield 3.0
    yield from shoot(f'viewport-{A.lang}.png')
    yield 0.2


def scenario_progress():
    s = configure_scene()
    s.size_x = s.size_z = 24
    win, ar, sp = setup_viewport()
    maximize(win, ar, hide_panels=False)
    yield 1.0
    win, ar = find_area('VIEW_3D')
    click_mc_tab(win, ar)
    yield 1.0
    with area_override(win, ar):
        bpy.ops.mcgen.generate('INVOKE_DEFAULT')      # модальная генерация: идёт в фоне, UI живой
    t0 = time.time()
    shot = False
    while time.time() - t0 < 120:
        sess = jobs.session(bpy.context.scene.name)
        job = getattr(sess, 'job', None)
        if job is not None and not job.finished and not shot and job.phase == 'generate' and job.fraction > 0.35:
            shot = True
            yield 0.3
            yield from shoot(f'progress-{A.lang}.png', stable=False)       # прогресс меняется каждую долю секунды: один снимок
            bpy.ops.mcgen.cancel('EXEC_DEFAULT')                     # остановить генерацию до выхода (иначе Blender завершается при работающих потоках)
            t1 = time.time()
            while not job.finished and time.time() - t1 < 30:
                yield 0.2
            yield 0.5
            break
        if job is not None and job.finished:
            break
        yield 0.2
    if not shot:
        log('WARNING: прогресс не пойман')
    yield 0.1


def scenario_biomemap():
    s = configure_scene()
    s.size_x, s.size_z = 24, 16
    win, ar, sp = setup_viewport()
    maximize(win, ar, hide_panels=False)
    yield 1.0
    win, ar = find_area('VIEW_3D')
    click_mc_tab(win, ar)
    yield 1.0
    s.use_features = s.use_structures = False
    bpy.ops.mcgen.generate('EXEC_DEFAULT')
    s.bm_step = '4'
    s.bm_plane = True
    bpy.ops.mcgen.biome_map('EXEC_DEFAULT')
    sp = ar.spaces.active
    frame_scene(sp, ar, win)
    yield 3.0
    yield from shoot(f'biomemap-{A.lang}.png')
    yield 0.2


SCENARIOS = {'prepare': scenario_prepare, 'panels': scenario_panels, 'viewport': scenario_viewport, 'progress': scenario_progress, 'biomemap': scenario_biomemap}
GEN = SCENARIOS[A.mode]()


def tick():
    try:
        d = next(GEN)
        return d
    except StopIteration:
        log('done')
        bpy.ops.wm.quit_blender()
        return None
    except Exception as e:     # noqa: BLE001
        import traceback
        traceback.print_exc()
        log('ERROR', e)
        os._exit(2)


bpy.app.timers.register(tick, first_interval=2.0)
