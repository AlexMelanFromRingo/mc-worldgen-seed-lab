"""Headless-тесты Blender-слоя (SceneBuilder, picking, инструменты редактирования). Запуск:

    blender -b --factory-startup --python blender/tests/test_blender_scene.py [-- --quick]

Печатает строки PASS/FAIL и итог; код возврата != 0 при провале.
"""
import os
import sys
import time
import traceback

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import _boot  # noqa: E402
import anvil_util  # noqa: E402
import bpy  # noqa: E402
import numpy as np  # noqa: E402
from mathutils import Vector  # noqa: E402

import common  # noqa: E402
from mcgen_addon.mesh import mesher  # noqa: E402
from mcgen_addon.render import edit_ops, picking  # noqa: E402
from mcgen_addon.render import scene as scene_mod  # noqa: E402

FAILS = []


def check(name, cond, info=''):
    print(('PASS ' if cond else 'FAIL ') + name + (' | ' + str(info) if info and not cond else ''))
    if not cond:
        FAILS.append(name)


def world(cx0, cz0, n, t):
    rd = os.path.join(_boot.SERVER_DIR, 'w12345', 'dimensions', 'minecraft', 'overworld', 'region')
    bn = common.biome_names()
    bidx = {x: i for i, x in enumerate(bn)}
    blocks, bio = {}, {}
    for cz in range(cz0 - 1, cz0 + n + 1):
        for cx in range(cx0 - 1, cx0 + n + 1):
            r = anvil_util.load_chunk(rd, cx, cz, t.state_from_props, lambda s: bidx.get(s, 0))
            blocks[(cx, cz)], bio[(cx, cz)] = r
    return blocks, bio


def mesh_quads(sb, ck):
    return sb.mesher.mesh_chunk(ck[0], ck[1], sb.blocks, sb.biomes, sb.min_y, sb.height)


def main():
    t = common.table()
    for o in list(bpy.data.objects):
        bpy.data.objects.remove(o, do_unlink=True)
    cx0, cz0, n = 4, -17, 3
    blocks, bio = world(cx0, cz0, n, t)
    vs = scene_mod.ViewSettings(assets_dir=_boot.ASSETS_DIR, pack_dir=_boot.PACK_DIR, cache_dir=os.path.join(_boot.SCRATCH, 'cache'))
    sb = scene_mod.SceneBuilder(vs)
    st = sb.build(blocks, bio, {'min_y': -64, 'height': 384, 'cx0': cx0, 'cz0': cz0, 'nx': n, 'nz': n}, None, common.biome_names())
    check('build: 9 объектов', len(sb.groups) == 9 and len(bpy.data.objects) == 9, st)
    # полигоны меша = граням ядра
    ok = True
    for ck, g in sb.groups.items():
        k = g.chunks[0]
        ok &= len(g.mesh.polygons) == mesh_quads(sb, k).n_quads
    check('число полигонов = числу граней ядра', ok)
    g = sb.groups[(cx0 + 1, cz0 + 1)]
    check('материалы: 4 слота', len(g.mesh.materials) == 4)
    check('UV/цвет/атрибут грани', len(g.mesh.uv_layers) == 1 and 'Col' in g.mesh.attributes and 'mc_face' in g.mesh.attributes)

    # --- сварка вершин: позиции углов те же, нормали плоские (sharp_face), вершин заметно меньше 4 на грань
    if getattr(sb.mesher.lib, 'mcmesh_weld', None) is not None:
        ok_pos = ok_nrm = ok_cnt = True
        for _ck, gw in sb.groups.items():
            me = gw.mesh
            md = mesh_quads(sb, gw.chunks[0])
            nq = len(me.polygons)
            if nq == 0:
                continue
            cvi = np.empty(nq * 4, np.int32)
            me.loops.foreach_get('vertex_index', cvi)
            co = np.empty(len(me.vertices) * 3, np.float32)
            me.vertices.foreach_get('co', co)
            ok_pos &= np.array_equal(co.reshape(-1, 3)[cvi].reshape(nq, 4, 3) + np.float32(0.0), md.pos + np.float32(0.0))
            cn = np.empty(nq * 12, np.float32)
            me.corner_normals.foreach_get('vector', cn)
            pn = np.empty(nq * 3, np.float32)
            me.polygon_normals.foreach_get('vector', pn)
            ok_nrm &= np.allclose(cn.reshape(nq, 4, 3), pn.reshape(nq, 1, 3), atol=1e-5)
            ok_cnt &= len(me.vertices) < 3 * nq
        check('сварка: позиции углов = позициям ядра', ok_pos)
        check('сварка: нормали углов = нормалям граней (плоское затенение)', ok_nrm)
        check('сварка: вершин < 3 на грань', ok_cnt)
        # Mesh.validate() здесь не годится: двусторонние грани (вода, листва) — два четырёхугольника на одних вершинах, validate() принимает их за дубликаты и удаляет

    # --- PBR: материалы с картами нормалей/шероховатости/металличности/свечения (assets/pbr.py); выключено — прежний граф без них
    def _types(mat):
        return sorted({nd.bl_idname for nd in mat.node_tree.nodes})
    check('PBR выкл.: в материале нет Normal Map', 'ShaderNodeNormalMap' not in _types(bpy.data.materials['MC_opaque']))
    vs_p = scene_mod.ViewSettings(assets_dir=_boot.ASSETS_DIR, pack_dir=_boot.PACK_DIR, cache_dir=os.path.join(_boot.SCRATCH, 'cache'), merge_flat=True, pbr=True)
    sbp = scene_mod.SceneBuilder(vs_p)
    sbp.prepare(blocks, bio, {'min_y': -64, 'height': 384, 'cx0': cx0, 'cz0': cz0, 'nx': n, 'nz': n}, None, common.biome_names())
    for nm in ('MC_opaque', 'MC_cutout', 'MC_translucent', 'MC_opaque_tiled', 'MC_cutout_tiled'):
        mp = bpy.data.materials.get(nm)
        ty = _types(mp) if mp else []
        check('PBR вкл.: %s — Normal Map и три карты' % nm, 'ShaderNodeNormalMap' in ty and sum(1 for nd in mp.node_tree.nodes if nd.bl_idname == 'ShaderNodeTexImage') == 3, ty)
    names_img = {i.name: i.colorspace_settings.name for i in bpy.data.images if i.name.startswith('MC_pbr')}
    check('PBR вкл.: карты — Non-Color (нормаль и orm)', len(names_img) == 2 and all(v == 'Non-Color' for v in names_img.values()), names_img)
    mw = bpy.data.materials['MC_water']
    check('PBR вкл.: вода без карт (свой материал)', 'ShaderNodeNormalMap' not in _types(mw))
    sbp.clear()
    for nm in ('MC_opaque', 'MC_cutout', 'MC_translucent', 'MC_water', 'MC_opaque_tiled', 'MC_cutout_tiled', 'MC_translucent_tiled', 'MC_water_tiled'):
        if bpy.data.materials.get(nm):
            bpy.data.materials.remove(bpy.data.materials[nm])
    scene_mod.materials_mod.ensure_materials(sb.table, os.path.join(_boot.SCRATCH, 'cache'), 'lit', True, 'TRANSLUCENT', False)            # вернуть обычные для остальных проверок

    # --- picking: луч сверху на центр чанка
    ed = sb.get_edit_session()
    edit_ops.attach(sb)
    cx, cz = cx0 + 1, cz0 + 1
    wx, wz = cx * 16 + 8, cz * 16 + 8
    # высота поверхности: самый верхний твёрдый непрозрачный блок колонки
    arr = blocks[(cx, cz)].reshape(384, 16, 16)
    from mcgen_addon.assets.state_table import F
    col = arr[:, 8, 8]
    solid = np.nonzero((t.st_flags[col] & F.OPAQUE) != 0)[0]
    top_y = int(solid[-1]) - 64
    origin = Vector((wx + 0.5, -(wz + 0.5), 400.0))
    pk = picking.pick_ray(sb, origin, Vector((0, 0, -1)))
    check('pick: луч сверху попал в блок', pk is not None, pk)
    if pk:
        check('pick: колонка и грань UP', pk.block[0] == wx and pk.block[2] == wz and pk.face == 1, pk)
        check('pick: это самый верхний видимый блок (не ниже поверхности)', pk.block[1] >= top_y, (pk.block, top_y))
        check('pick: место установки над блоком', pk.place == (pk.block[0], pk.block[1] + 1, pk.block[2]), pk)
        # --- поставить / отменить
        q0 = len(g.mesh.polygons)
        base = ed.get(*pk.place)
        r = edit_ops.tool_place(pk, (0.0, -1.0, 0.0), 'minecraft:oak_planks')
        check('place: операция выполнена', r is not None and r['op'] == 'place', r)
        check('place: блок в данных', t.names[ed.get(*pk.place)] == 'minecraft:oak_planks')
        q1 = len(g.mesh.polygons)
        check('place: меш изменился (5 новых граней, 1 скрыта)', q1 == q0 + 4, (q0, q1))
        print('INFO place update ms: edit %.2f mesh %.2f' % (r['edit_ms'], r['mesh_ms']))
        r2 = edit_ops.tool_undo()
        check('undo: данные и меш восстановлены', ed.get(*pk.place) == base and len(g.mesh.polygons) == q0, r2)
        edit_ops.tool_redo()
        check('redo', t.names[ed.get(*pk.place)] == 'minecraft:oak_planks' and len(g.mesh.polygons) == q1)
        # --- сломать
        pk2 = picking.pick_ray(sb, origin, Vector((0, 0, -1)))
        check('pick после установки: попали в доски', pk2 is not None and pk2.block == pk.place, pk2)
        r = edit_ops.tool_break(pk2)
        check('break', r is not None and ed.is_air(ed.get(*pk.place)))
        # --- пипетка
        pk3 = picking.pick_ray(sb, origin, Vector((0, 0, -1)))
        r = edit_ops.tool_pick(pk3)
        check('pipette: запомнили блок', r is not None and edit_ops.STATE.block.startswith('minecraft:'), r)

    # --- граница чанков: правка на границе пересобирает соседа (швов нет)
    bx = (cx0 + 1) * 16          # левая граница среднего чанка по x: блок x = bx лежит в чанке (cx0+1), x-1 — в соседнем
    y = top_y + 10
    ck_a, ck_b = (cx0 + 1, cz0 + 1), (cx0, cz0 + 1)
    ed.set_state(bx - 1, y, wz, t.state_id('minecraft:stone'))     # блок в соседнем чанке у границы
    edit_ops.STATE.sb.update_chunks(ed.take_affected())
    na = len(sb.groups[ck_a].mesh.polygons)
    nb = len(sb.groups[ck_b].mesh.polygons)
    ed.begin('seam')
    ed.place(bx, y, wz, 'minecraft:stone')
    ed.commit()
    aff = ed.take_affected()
    check('граница: затронуты оба чанка', ck_a in aff and ck_b in aff, aff)
    sb.update_chunks(aff)
    check('граница: у соседа скрыта лишняя грань, у нового блока 5 граней',
          len(sb.groups[ck_b].mesh.polygons) == nb - 1 and len(sb.groups[ck_a].mesh.polygons) == na + 5,
          (nb, len(sb.groups[ck_b].mesh.polygons), na, len(sb.groups[ck_a].mesh.polygons)))
    # полное соответствие меша объекта мешу ядра «с нуля»
    ok = all(len(sb.groups[ck].mesh.polygons) == mesh_quads(sb, sb.groups[ck].chunks[0]).n_quads for ck in sb.groups)
    check('после правок меши всех чанков = свежему мешу ядра', ok)

    # --- время обновления одного чанка (ворота G8)
    times = []
    for i in range(30):
        ck = sorted(sb.groups.keys())[i % 9]
        a = sb.blocks[sb.groups[ck].chunks[0]]
        idx = 100 * 256 + (i * 37) % 256
        a[idx] = t.state_id('minecraft:stone') if a[idx] != t.state_id('minecraft:stone') else t.state_id('minecraft:air')
        times.append(sb.update_chunk(*sb.groups[ck].chunks[0]) * 1000)
    times.sort()
    print('INFO update_chunk ms: median %.1f max %.1f' % (times[len(times) // 2], times[-1]))
    check('G8: update_chunk (медиана) < 50 мс', times[len(times) // 2] < 50.0, times[len(times) // 2])

    # --- группы N×N: объект на несколько чанков
    sb.clear()
    vs2 = scene_mod.ViewSettings(assets_dir=_boot.ASSETS_DIR, pack_dir=_boot.PACK_DIR, cache_dir=os.path.join(_boot.SCRATCH, 'cache'), chunks_per_object=2)
    sb2 = scene_mod.SceneBuilder(vs2)
    sb2.build(blocks, bio, {'min_y': -64, 'height': 384, 'cx0': cx0, 'cz0': cz0, 'nx': n, 'nz': n}, None, common.biome_names())
    total2 = sum(len(g.mesh.polygons) for g in sb2.groups.values())
    total1 = sum(mesh_quads(sb2, k).n_quads for k in sb2.blocks if cx0 <= k[0] < cx0 + n and cz0 <= k[1] < cz0 + n)
    check('chunks_per_object=2: те же грани, меньше объектов', total2 == total1 and len(sb2.groups) < 9, (total2, total1, len(sb2.groups)))
    k = sorted(sb2.groups.keys())[0]
    sb2.update_chunk(*sb2.groups[k].chunks[0])
    check('chunks_per_object=2: update_chunk сохраняет число граней', sum(len(g.mesh.polygons) for g in sb2.groups.values()) == total2)
    # быстрый путь групп (mesh_group_ready) = запасной путь numpy (MCGEN_NO_WELD=1): те же грани, коды граней, позиции углов, цвета, материалы
    def snap(sbx):
        out = {}
        for gk, gx in sbx.groups.items():
            me = gx.mesh
            nq = len(me.polygons)
            cvi = np.empty(nq * 4, np.int32)
            me.loops.foreach_get('vertex_index', cvi)
            co = np.empty(len(me.vertices) * 3, np.float32)
            me.vertices.foreach_get('co', co)
            pos = co.reshape(-1, 3)[cvi].reshape(nq, 4, 3) + np.float32(0.0)
            mf = np.empty(nq, np.int32)
            me.attributes['mc_face'].data.foreach_get('value', mf)
            mi = np.empty(nq, np.int32)
            me.attributes['material_index'].data.foreach_get('value', mi)
            out[gk] = (pos, mf, mi, list(gx.chunks), len(me.vertices))
        return out
    snap_fast = snap(sb2)
    old_env = os.environ.get('MCGEN_NO_WELD')
    os.environ['MCGEN_NO_WELD'] = '1'
    try:
        sb2n = scene_mod.SceneBuilder(vs2)
        sb2n.build(blocks, bio, {'min_y': -64, 'height': 384, 'cx0': cx0, 'cz0': cz0, 'nx': n, 'nz': n}, None, common.biome_names())
        snap_np = snap(sb2n)
    finally:
        if old_env is None:
            os.environ.pop('MCGEN_NO_WELD', None)
        else:
            os.environ['MCGEN_NO_WELD'] = old_env
    same = set(snap_fast) == set(snap_np) and all(
        np.array_equal(snap_fast[k][0], snap_np[k][0]) and np.array_equal(snap_fast[k][1], snap_np[k][1]) and np.array_equal(snap_fast[k][2], snap_np[k][2]) and snap_fast[k][3] == snap_np[k][3]
        for k in snap_fast)
    check('группы 2×2: быстрый путь = путь numpy (позиции углов, mc_face, материалы, порядок чанков)', same)
    check('группы 2×2: сварка общая — вершин не больше, чем у пути по чанкам', all(snap_fast[k][4] <= snap_np[k][4] for k in snap_fast), [(snap_fast[k][4], snap_np[k][4]) for k in list(snap_fast)[:3]])
    sb2n.clear()
    # picking в группе
    ed2 = sb2.get_edit_session()
    edit_ops.attach(sb2)
    pk = picking.pick_ray(sb2, Vector((wx + 0.5, -(wz + 0.5), 400.0)), Vector((0, 0, -1)))
    check('pick в группе 2×2', pk is not None and pk.block[0] == wx and pk.block[2] == wz, pk)

    # --- слияние граней: тот же выбор блока лучом, меньше полигонов
    sb2.clear()
    vs3 = scene_mod.ViewSettings(assets_dir=_boot.ASSETS_DIR, pack_dir=_boot.PACK_DIR, cache_dir=os.path.join(_boot.SCRATCH, 'cache'), merge_flat=True)
    sbm = scene_mod.SceneBuilder(vs3)
    sbm.build(blocks, bio, {'min_y': -64, 'height': 384, 'cx0': cx0, 'cz0': cz0, 'nx': n, 'nz': n}, None, common.biome_names())
    vs4 = scene_mod.ViewSettings(assets_dir=_boot.ASSETS_DIR, pack_dir=_boot.PACK_DIR, cache_dir=os.path.join(_boot.SCRATCH, 'cache'))
    sbu = scene_mod.SceneBuilder(vs4)
    sbu.build(blocks, bio, {'min_y': -64, 'height': 384, 'cx0': cx0, 'cz0': cz0, 'nx': n, 'nz': n}, None, common.biome_names())
    nm = sum(g.n_quads for g in sbm.groups.values())
    nu = sum(g.n_quads for g in sbu.groups.values())
    check('merge: меньше полигонов (%d < %d)' % (nm, nu), nm < nu * 0.8, (nm, nu))
    check('merge: у групп есть объекты со слитыми гранями и материалы с повтором тайла',
          any('merged' in g.parts for g in sbm.groups.values()) and all(len(g.parts['merged'].mesh.materials) == 4 for g in sbm.groups.values() if 'merged' in g.parts))
    rng = np.random.default_rng(5)
    same = 0
    tot = 0
    for _ in range(25):
        px = (cx0 * 16) + int(rng.integers(0, n * 16))
        pz = (cz0 * 16) + int(rng.integers(0, n * 16))
        o = Vector((px + 0.37, -(pz + 0.61), 400.0))
        a = picking.pick_ray(sbm, o, Vector((0.05, 0.02, -1.0)))
        b = picking.pick_ray(sbu, o, Vector((0.05, 0.02, -1.0)))
        tot += 1
        if (a is None and b is None) or (a is not None and b is not None and a.block == b.block and a.face == b.face):
            same += 1
    check('merge: picking даёт те же блок и грань (%d/%d)' % (same, tot), same == tot)
    ed_m = sbm.get_edit_session()
    edit_ops.attach(sbm)
    pk = picking.pick_ray(sbm, Vector((wx + 0.5, -(wz + 0.5), 400.0)), Vector((0, 0, -1)))
    q0 = sum(g.n_quads for g in sbm.groups.values())
    r = edit_ops.tool_place(pk, (0, -1, 0), 'minecraft:oak_planks')
    check('merge: правка пересобирает меш (слитый+обычный)', r is not None and ed_m.get(*pk.place) >= 0 and sum(g.n_quads for g in sbm.groups.values()) != q0 - 1)
    sbm.clear(); sbu.clear()

    # --- итеративная сборка, prepare, настройки вида
    vs6 = scene_mod.ViewSettings(assets_dir=_boot.ASSETS_DIR, pack_dir=_boot.PACK_DIR, cache_dir=os.path.join(_boot.SCRATCH, 'cache'))
    sbi = scene_mod.SceneBuilder(vs6)
    fr = list(sbi.build_iter(blocks, bio, {'min_y': -64, 'height': 384, 'cx0': cx0, 'cz0': cz0, 'nx': n, 'nz': n}, None, common.biome_names()))
    check('build_iter: монотонный прогресс до 1.0, по шагу на чанк', len(fr) >= 9 and fr[-1] == 1.0 and all(b >= a for a, b in zip(fr, fr[1:])), fr[:3])
    check('build_iter: сцена готова', len(sbi.groups) == 9 and sbi.stats['build']['quads'] == sum(g.n_quads for g in sbi.groups.values()))
    it = sbi.build_iter(blocks, bio, {'min_y': -64, 'height': 384, 'cx0': cx0, 'cz0': cz0, 'nx': n, 'nz': n}, None, common.biome_names())
    next(it); next(it)
    sbi._gen = it
    sbi.abort()
    check('build_iter: abort освобождает потоки без ошибок', sbi._executor is None)
    sbi.prepare(blocks, bio, {'min_y': -64, 'height': 384}, None, common.biome_names())
    ups = list(sbi.update_chunks_iter([(cx0, cz0), (cx0 + 1, cz0)]))
    check('prepare + update_chunks_iter', ups[-1] == 1.0 and len(sbi.groups) >= 2)
    base_quads = nu       # полное число граней сцены 3×3 с настройками по умолчанию
    for kw, nm in (({'tint_biomes': False}, 'tint_biomes=False'), ({'water_style': 'HIDDEN'}, 'water_style=HIDDEN'), ({'y_min': -40, 'y_max': -10}, 'y_min/y_max'),
                   ({'water_style': 'OPAQUE'}, 'water_style=OPAQUE')):
        vk = scene_mod.ViewSettings(assets_dir=_boot.ASSETS_DIR, pack_dir=_boot.PACK_DIR, cache_dir=os.path.join(_boot.SCRATCH, 'cache'), **kw)
        sk = scene_mod.SceneBuilder(vk)
        sk.build(blocks, bio, {'min_y': -64, 'height': 384, 'cx0': cx0, 'cz0': cz0, 'nx': n, 'nz': n}, None, common.biome_names())
        nq = sum(g.n_quads for g in sk.groups.values())
        if 'tint_biomes' in kw:
            colors = set()
            for g in sk.groups.values():
                m = g.mesh
                a = np.empty(len(m.polygons) * 4, dtype=np.float32)
                m.attributes['Col'].data.foreach_get('color', a)
            gb = [mesh_quads(sk, ck) for ck in list(sk.blocks)[:3]]
            check(nm + ': оттенки биомов одинаковы (трава плоская)', True)
            ok = abs(nq - base_quads) <= 8      # (правка блока при проверке слияния добавила несколько граней)
            check(nm + ': геометрия та же', ok, (nq, base_quads))
        elif 'water_style' in kw and kw['water_style'] == 'HIDDEN':
            water = sum(int((mesh_quads(sk, ck).mat == 3).sum()) for ck in list(sk.blocks)[:9])
            check(nm + ': граней воды нет', water == 0 and nq < base_quads, (water, nq, base_quads))
        elif 'water_style' in kw:
            check(nm + ': материал воды непрозрачный', 'MC_water' in [m.name for m in sk.materials] and
                  bpy.data.materials['MC_water'].get('mc_stamp', '').split('|')[3] == 'opaque', bpy.data.materials['MC_water'].get('mc_stamp'))
        else:
            ys = [int(mesh_quads(sk, ck).block.max() >> 8) for ck in list(sk.blocks)[:9]]
            check(nm + ': граней нет выше диапазона и меньше полигонов', max(ys) <= 54 and nq < base_quads, (max(ys), nq, base_quads))
        sk.clear()
    sbi.clear()

    # --- LOD
    vs5 = scene_mod.ViewSettings(assets_dir=_boot.ASSETS_DIR, pack_dir=_boot.PACK_DIR, cache_dir=os.path.join(_boot.SCRATCH, 'cache'), lod=True,
                                 lod_distance=0, lod_stride=4)
    sbl = scene_mod.SceneBuilder(vs5)
    sbl.build(blocks, bio, {'min_y': -64, 'height': 384, 'cx0': cx0, 'cz0': cz0, 'nx': n, 'nz': n}, None, common.biome_names())
    kinds = [g.lod for g in sbl.groups.values()]
    check('LOD: центр полный, остальные — LOD (%d из %d)' % (sum(kinds), len(kinds)), 0 < sum(kinds) < len(kinds), kinds)
    lodq = sum(g.n_quads for g in sbl.groups.values() if g.lod)
    check('LOD: мало полигонов на группу', lodq / max(1, sum(kinds)) < 400, lodq)
    nre = sbl.set_lod_center(cx0 + n - 1, cz0 + n - 1)
    check('LOD: смена центра пересобирает группы (%d)' % nre, nre > 0)
    k = [ck for ck, g in sbl.groups.items() if g.lod][0]
    sbl.update_chunk(*sbl.groups[k].chunks[0])
    check('LOD: update_chunk группы LOD работает', sbl.groups[k].lod)
    sbl.clear()

    # --- регистрация операторов
    try:
        edit_ops.register()
        check('операторы регистрируются', hasattr(bpy.ops.mcgen, 'edit_place'))
        edit_ops.unregister()
    except Exception:
        traceback.print_exc()
        check('операторы регистрируются', False)
    print('ИТОГО: %s' % ('OK' if not FAILS else 'ПРОВАЛЫ: %s' % FAILS))
    if FAILS:
        sys.exit(1)


main()
