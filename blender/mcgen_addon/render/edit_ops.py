"""Инструменты редактирования мира в 3D-виде: модальные операторы «Поставить», «Сломать», «Пипетка», undo/redo правок.

Блоки — данные (массивы u16 чанков), а не объекты: луч попадает в меш чанка → (блок, грань) → правка массива в `EditSession`
(mesh/edit.py: автосвязи, undo/redo, слой правок) → перестройка только затронутых чанков (включая соседей на границе).

Подключение (делает аддон/W5): `edit_ops.register()`; после `SceneBuilder.build(...)` — `edit_ops.attach(scene_builder)`.
Выбор ставимого блока: `edit_ops.STATE.block = 'minecraft:oak_stairs'` (или оператор с параметром `block`).
Чистые функции инструментов (`tool_place`, `tool_break`, `tool_pick`) не зависят от модальности и тестируются headless.
"""
import json
import time

import bpy
from bpy.app.translations import pgettext_rpt
from bpy.props import BoolProperty, EnumProperty, StringProperty

from ..mesh.edit import PlaceContext
from . import picking

__all__ = ['STATE', 'TEXT_NAME', 'attach', 'detach', 'store_edits', 'discard_edits', 'tool_place', 'tool_break', 'tool_pick', 'tool_undo', 'tool_redo', 'CLASSES', 'register', 'unregister']


class _State:
    def __init__(self):
        self.sb = None                 # SceneBuilder
        self.block = 'minecraft:stone'
        self.exact_state = None        # id состояния-образца (пипетка)
        self.orient = 'VIEW'           # 'VIEW' — ориентация по виду/клику, 'EXACT' — как у образца
        self.last = {}                 # сводка последней операции (время, число блоков)
        self.hover = None              # PickResult под курсором (для подсветки)
        self.handle = None
        self.key = None                # ключ мира (версия, измерение, пресет, сиды): правки накладываются только на тот же мир
        self.restored = None           # {'blocks': n, 'conflicts': m} — сколько сохранённых правок наложено после последней сборки


STATE = _State()
TEXT_NAME = 'MC World edits'         # текстовый блок .blend со слоем правок (JSON): воксели в .blend не сохраняются, правки — да


def attach(scene_builder, key=None):
    """Привязывает инструменты к сцене (SceneBuilder после build). key — ключ мира (список: версия, измерение, пресет, сиды); если в .blend есть сохранённый
    слой правок ТОГО ЖЕ мира, он накладывается на только что построенные блоки (меши затронутых чанков перестраиваются)."""
    STATE.sb = scene_builder
    STATE.hover = None
    STATE.last = {}
    STATE.key = list(key) if key is not None else None
    STATE.restored = None
    ed = scene_builder.get_edit_session()
    _restore_edits(ed)
    return ed


def detach():
    """Отвязывает инструменты от сцены (очистка мира, пересборка): правки без сцены невозможны. Сохранённый слой правок остаётся в .blend."""
    STATE.sb = None
    STATE.hover = None
    STATE.last = {}
    STATE.restored = None


def store_edits():
    """Записывает слой правок текущей сцены в текстовый блок .blend (вызывается после каждой правки/отмены). Ошибки не пробрасываются."""
    if STATE.sb is None:
        return
    try:
        data = {'v': 1, 'key': STATE.key, 'edits': STATE.sb.get_edit_session().to_dict()}
        txt = bpy.data.texts.get(TEXT_NAME) or bpy.data.texts.new(TEXT_NAME)
        txt.use_fake_user = True
        txt.clear()
        txt.write(json.dumps(data, separators=(',', ':')))
    except Exception:      # noqa: BLE001 — запись правок не должна ломать сам инструмент
        pass


def discard_edits():
    """Удаляет сохранённый слой правок из .blend (мир вернётся к сгенерированному после следующей генерации)."""
    txt = bpy.data.texts.get(TEXT_NAME)
    if txt is not None:
        bpy.data.texts.remove(txt)
    STATE.restored = None


def _restore_edits(ed):
    txt = bpy.data.texts.get(TEXT_NAME)
    if txt is None:
        return None
    try:
        data = json.loads(txt.as_string())
        if data.get('v') != 1 or data.get('key') != STATE.key or not data.get('edits', {}).get('chunks'):
            return None
        applied, conflicts, affected = ed.load_dict(data['edits'])
    except Exception:      # noqa: BLE001 — повреждённый текст не мешает показу мира
        return None
    if affected:
        STATE.sb.update_chunks(affected)
    STATE.restored = {'blocks': applied, 'conflicts': conflicts}
    return STATE.restored


def _norm_block(name):
    name = (name or '').strip()
    return name if not name or ':' in name else 'minecraft:' + name


def _session():
    if STATE.sb is None:
        raise RuntimeError('edit tools are not attached to a scene (edit_ops.attach)')
    return STATE.sb.get_edit_session()


def _view_dir_mc(origin, hit):
    d = (hit[0] - origin[0], hit[1] - origin[1], hit[2] - origin[2])
    return picking.view_dir_mc(d)


def _finish(label, t0, n_blocks, affected):
    sb = STATE.sb
    t_edit = time.time() - t0
    t1 = time.time()
    sb.update_chunks(affected)
    STATE.last = {'op': label, 'blocks': n_blocks, 'chunks': len(affected), 'edit_ms': t_edit * 1000.0, 'mesh_ms': (time.time() - t1) * 1000.0}
    store_edits()
    return STATE.last


def tool_place(pick, view_dir_mc, block=None, exact_state=None):
    """Ставит блок рядом с гранью `pick` (PickResult). view_dir_mc — направление взгляда (оси Minecraft). Возвращает сводку операции."""
    ed = _session()
    t0 = time.time()
    block = _norm_block(block) or STATE.block
    es = exact_state if exact_state is not None else (STATE.exact_state if STATE.orient == 'EXACT' else None)
    ctx = PlaceContext.from_view(pick.face, pick.hit, pick.place, view_dir_mc)
    ed.begin('Place block')
    placed = ed.place(pick.place[0], pick.place[1], pick.place[2], block, ctx, es)
    n = ed.commit()
    return _finish('place', t0, n, ed.take_affected()) if placed else None


def tool_break(pick):
    ed = _session()
    t0 = time.time()
    ed.begin('Break block')
    ok = ed.break_block(*pick.block)
    n = ed.commit()
    return _finish('break', t0, n, ed.take_affected()) if ok else None


def tool_pick(pick):
    """Пипетка: запоминает блок (и точное состояние) под курсором как ставимый."""
    ed = _session()
    sid = ed.pick(*pick.block)
    if sid < 0:
        return None
    blk, props = ed.info(sid)
    STATE.block = 'minecraft:' + blk
    STATE.exact_state = sid
    STATE.last = {'op': 'pick', 'state': ed.t.names[sid]}
    return STATE.last


def tool_undo():
    ed = _session()
    t0 = time.time()
    aff = ed.undo()
    return _finish('undo', t0, len(aff), aff) if aff else None


def tool_redo():
    ed = _session()
    t0 = time.time()
    aff = ed.redo()
    return _finish('redo', t0, len(aff), aff) if aff else None


# ----------------------------------------------------------------------------------------------------------------------
#                                          подсветка блока под курсором
# ----------------------------------------------------------------------------------------------------------------------
def _draw_hover():
    h = STATE.hover
    if h is None or STATE.sb is None:
        return
    try:
        import gpu
        from gpu_extras.batch import batch_for_shader
    except Exception:
        return
    s = float(STATE.sb.vs.scale)
    cell = h[0]
    x, y, z = cell
    e = 0.004
    pts = [(x - e, y - e, z - e), (x + 1 + e, y - e, z - e), (x + 1 + e, y - e, z + 1 + e), (x - e, y - e, z + 1 + e),
           (x - e, y + 1 + e, z - e), (x + 1 + e, y + 1 + e, z - e), (x + 1 + e, y + 1 + e, z + 1 + e), (x - e, y + 1 + e, z + 1 + e)]
    bl = [picking.mc_to_blender(p, s) for p in pts]
    idx = ((0, 1), (1, 2), (2, 3), (3, 0), (4, 5), (5, 6), (6, 7), (7, 4), (0, 4), (1, 5), (2, 6), (3, 7))
    coords = []
    for a, b in idx:
        coords += [tuple(bl[a]), tuple(bl[b])]
    try:
        shader = gpu.shader.from_builtin('UNIFORM_COLOR')
        batch = batch_for_shader(shader, 'LINES', {'pos': coords})
        gpu.state.line_width_set(2.0)
        gpu.state.depth_test_set('LESS_EQUAL')
        shader.bind()
        shader.uniform_float('color', h[1])
        batch.draw(shader)
        gpu.state.line_width_set(1.0)
    except Exception:
        pass


def _ensure_handle():
    if STATE.handle is None:
        try:
            STATE.handle = bpy.types.SpaceView3D.draw_handler_add(_draw_hover, (), 'WINDOW', 'POST_VIEW')
        except Exception:
            STATE.handle = None


def _remove_handle():
    if STATE.handle is not None:
        try:
            bpy.types.SpaceView3D.draw_handler_remove(STATE.handle, 'WINDOW')
        except Exception:
            pass
        STATE.handle = None
    STATE.hover = None


# ----------------------------------------------------------------------------------------------------------------------
#                                                  операторы
# ----------------------------------------------------------------------------------------------------------------------
_NAV = {'MIDDLEMOUSE', 'WHEELUPMOUSE', 'WHEELDOWNMOUSE', 'TRACKPADPAN', 'TRACKPADZOOM', 'NDOF_MOTION', 'NUMPAD_1', 'NUMPAD_2', 'NUMPAD_3',
        'NUMPAD_4', 'NUMPAD_5', 'NUMPAD_6', 'NUMPAD_7', 'NUMPAD_8', 'NUMPAD_9', 'NUMPAD_0', 'NUMPAD_PERIOD'}


class _ToolBase(bpy.types.Operator):
    bl_options = {'REGISTER'}
    mode = 'PLACE'
    color = (1.0, 1.0, 1.0, 1.0)
    help_text = ''

    block: StringProperty(name='Block', default='', description='Block to place (empty = the current one from the eyedropper)')

    @classmethod
    def poll(cls, context):
        return STATE.sb is not None and context.area is not None and context.area.type == 'VIEW_3D'

    def _hover_update(self, context, event):
        pk = picking.pick_screen(STATE.sb, context, event)
        self._pick = pk
        if pk is None:
            STATE.hover = None
        else:
            cell = pk.place if self.mode == 'PLACE' else pk.block
            STATE.hover = (cell, self.color)
        if context.area:
            context.area.tag_redraw()

    def invoke(self, context, event):
        if STATE.sb is None:
            self.report({'ERROR'}, pgettext_rpt('No scene: generate the world first'))
            return {'CANCELLED'}
        if self.block.strip():
            STATE.block = _norm_block(self.block)
            STATE.exact_state = None
        self._pick = None
        _ensure_handle()
        context.window_manager.modal_handler_add(self)
        self._status(context)
        self._hover_update(context, event)
        return {'RUNNING_MODAL'}

    def _status(self, context, extra=''):
        txt = pgettext_rpt('{tool}: LMB apply, RMB/Esc leave, Ctrl+Z / Ctrl+Shift+Z undo / redo. Block: {block} {extra}').format(
            tool=pgettext_rpt(self.bl_label), block=STATE.block, extra=extra)
        context.workspace.status_text_set(txt)

    def _end(self, context):
        context.workspace.status_text_set(None)
        _remove_handle()
        if context.area:
            context.area.tag_redraw()

    def apply(self, context, event, pk):
        raise NotImplementedError

    def modal(self, context, event):
        if event.type in _NAV or (event.alt and event.type == 'LEFTMOUSE'):
            return {'PASS_THROUGH'}
        if event.type == 'MOUSEMOVE':
            self._hover_update(context, event)
            return {'RUNNING_MODAL'}
        if event.type in {'ESC', 'RIGHTMOUSE'} and event.value == 'PRESS':
            self._end(context)
            return {'FINISHED'}
        if event.type == 'Z' and event.value == 'PRESS' and event.ctrl:
            r = tool_redo() if event.shift else tool_undo()
            self._status(context, '| %s' % (self._fmt(r),))
            return {'RUNNING_MODAL'}
        if event.type == 'LEFTMOUSE' and event.value == 'PRESS':
            self._hover_update(context, event)
            pk = self._pick
            if pk is not None:
                r = self.apply(context, event, pk)
                self._status(context, '| %s' % (self._fmt(r),))
                self._hover_update(context, event)
            return {'RUNNING_MODAL'}
        return {'RUNNING_MODAL'}

    @staticmethod
    def _fmt(r):
        if not r:
            return pgettext_rpt('no changes')
        if 'edit_ms' in r:
            return pgettext_rpt('{op}: {n} blocks, {c} chunks, mesh {ms:.1f} ms').format(op=r['op'], n=r['blocks'], c=r['chunks'], ms=r['mesh_ms'])
        return str(r.get('state', r))


class MCGEN_OT_edit_place(_ToolBase):
    bl_idname = 'mcgen.edit_place'
    bl_label = 'Place Block'
    bl_description = 'Place a block next to the face under the cursor (orientation from the view and the click point, automatic connections)'
    mode = 'PLACE'
    color = (0.2, 1.0, 0.3, 1.0)

    def apply(self, context, event, pk):
        from bpy_extras import view3d_utils
        d = view3d_utils.region_2d_to_vector_3d(context.region, context.region_data, (event.mouse_region_x, event.mouse_region_y))
        return tool_place(pk, picking.view_dir_mc(d))


class MCGEN_OT_edit_break(_ToolBase):
    bl_idname = 'mcgen.edit_break'
    bl_label = 'Break Block'
    bl_description = 'Break the block under the cursor (doors, beds and tall plants: both halves)'
    mode = 'BREAK'
    color = (1.0, 0.2, 0.2, 1.0)

    def apply(self, context, event, pk):
        return tool_break(pk)


class MCGEN_OT_edit_pick(_ToolBase):
    bl_idname = 'mcgen.edit_pick'
    bl_label = 'Pick Block'
    bl_description = 'Take the block under the cursor as the block to place (with its state)'
    mode = 'PICK'
    color = (0.3, 0.6, 1.0, 1.0)

    def apply(self, context, event, pk):
        r = tool_pick(pk)
        if r:
            try:
                context.scene.mcgen.edit_block = STATE.block       # поле «Block» панели Edit Blocks показывает взятый блок
            except AttributeError:
                pass
        return r


class MCGEN_OT_edit_undo(bpy.types.Operator):
    bl_idname = 'mcgen.edit_undo'
    bl_label = 'Undo Block Edit'
    bl_description = 'Undo the last block edit (Ctrl+Z inside a tool does the same)'
    bl_options = {'REGISTER'}

    @classmethod
    def poll(cls, context):
        return STATE.sb is not None

    def execute(self, context):
        r = tool_undo()
        if not r:
            self.report({'INFO'}, pgettext_rpt('Nothing to undo'))
            return {'CANCELLED'}
        return {'FINISHED'}


class MCGEN_OT_edit_redo(bpy.types.Operator):
    bl_idname = 'mcgen.edit_redo'
    bl_label = 'Redo Block Edit'
    bl_description = 'Redo the undone block edit (Ctrl+Shift+Z inside a tool)'
    bl_options = {'REGISTER'}

    @classmethod
    def poll(cls, context):
        return STATE.sb is not None

    def execute(self, context):
        r = tool_redo()
        if not r:
            self.report({'INFO'}, pgettext_rpt('Nothing to redo'))
            return {'CANCELLED'}
        return {'FINISHED'}


CLASSES = (MCGEN_OT_edit_place, MCGEN_OT_edit_break, MCGEN_OT_edit_pick, MCGEN_OT_edit_undo, MCGEN_OT_edit_redo)


def register():
    for c in CLASSES:
        bpy.utils.register_class(c)


def unregister():
    _remove_handle()
    for c in reversed(CLASSES):
        try:
            bpy.utils.unregister_class(c)
        except RuntimeError:
            pass
