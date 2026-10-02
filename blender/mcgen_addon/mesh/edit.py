"""Слой правок мира: поставить / сломать / пипетка, автосвязи по правилам ванили (заборы, панели, стены, ступени, `snowy`, плиты, двери,
кровати, двойные растения), собственный undo/redo, сохранение правок в JSON. Чистый Python (numpy), без bpy.

Источник истины — массивы состояний чанков (u16, [y][z][x]) — правятся на месте; слой правок хранит только разницу
(чанк → {локальный индекс: (исходное состояние, новое)}) и позволяет «наложить» правки на заново сгенерированный мир.

    ed = EditSession(table, blocks_by_chunk, min_y=-64, height=384, pack_dir=…)
    ed.begin('Поставить блок'); ed.place(x, y, z, 'minecraft:oak_stairs', PlaceContext(face=1, hit=(.5, 1, .5), look=2)); ed.commit()
    changed = ed.take_affected()          # {(cx, cz), …} — какие меши пересобрать (включая соседей по границам)
    ed.undo() / ed.redo()                 # -> множество затронутых чанков

Правила воспроизводят поведение игры (StairBlock, FenceBlock, IronBarsBlock, WallBlock, DoorBlock, BedBlock, DoublePlantBlock,
FenceGateBlock, ChorusPlantBlock, SnowyDirtBlock, SlabBlock, TrapDoorBlock…) без выживания: упавшие растения, «осыпание» и подобное не моделируются.
"""
import json
import math

import numpy as np

from ..assets import blockprops_data as bp
from ..assets.state_table import StateTable

__all__ = ['PlaceContext', 'EditSession', 'DIR_VEC']

# направления: DOWN, UP, NORTH, SOUTH, WEST, EAST (как в игре)
DIR_VEC = ((0, -1, 0), (0, 1, 0), (0, 0, -1), (0, 0, 1), (-1, 0, 0), (1, 0, 0))
OPP = (1, 0, 3, 2, 5, 4)
DIR_NAME = ('down', 'up', 'north', 'south', 'west', 'east')
DIR_BY_NAME = {n: i for i, n in enumerate(DIR_NAME)}
HORIZ = (2, 5, 3, 4)   # по часовой стрелке сверху: N, E, S, W
_CW = {2: 5, 5: 3, 3: 4, 4: 2}
_CCW = {v: k for k, v in _CW.items()}


def cw(d):
    return _CW[d]


def ccw(d):
    return _CCW[d]


def _C(name):
    return bp.CLASS_MEMBERS.get(name, frozenset())


FENCES = _C('FenceBlock')
FENCE_GATES = _C('FenceGateBlock')
PANES = _C('IronBarsBlock')
WALLS = _C('WallBlock')
STAIRS = _C('StairBlock')
SLABS = _C('SlabBlock')
DOORS = _C('DoorBlock')
BEDS = _C('BedBlock')
DOUBLE_PLANTS = _C('DoublePlantBlock')
LEAVES = _C('LeavesBlock')
SHULKERS = _C('ShulkerBoxBlock')
SNOW_BLOCKS = frozenset(('snow', 'snow_block', 'powder_snow'))
CONNECT_EXCEPTIONS = frozenset(('barrier', 'carved_pumpkin', 'jack_o_lantern', 'melon', 'pumpkin'))

# блоки, у которых facing = противоположное взгляду игрока (HorizontalDirectionalBlock: furnace-подобные)
FACING_OPPOSITE = frozenset(('furnace', 'blast_furnace', 'smoker', 'carved_pumpkin', 'jack_o_lantern', 'chest', 'trapped_chest', 'ender_chest',
                             'lectern', 'loom', 'stonecutter', 'repeater', 'comparator', 'end_portal_frame', 'grindstone', 'cartography_table',
                             'copper_chest', 'exposed_copper_chest', 'weathered_copper_chest', 'oxidized_copper_chest',
                             'waxed_copper_chest', 'waxed_exposed_copper_chest', 'waxed_weathered_copper_chest', 'waxed_oxidized_copper_chest'))
# 6 направлений: противоположное «ближайшему направлению взгляда»
FACING6_OPPOSITE_LOOK = frozenset(('piston', 'sticky_piston', 'dispenser', 'dropper', 'barrel', 'observer', 'command_block',
                                   'chain_command_block', 'repeating_command_block', 'crafter'))
FACING_CLICKED_FACE = frozenset(('end_rod', 'lightning_rod', 'amethyst_cluster', 'small_amethyst_bud', 'medium_amethyst_bud', 'large_amethyst_bud'))


class PlaceContext:
    """Контекст установки блока (как BlockPlaceContext): clicked face, точка клика в долях блока, взгляд игрока."""

    def __init__(self, face=1, hit=(0.5, 0.5, 0.5), look=2, look6=None, yaw=0.0, replacing_clicked=False):
        self.face = face                  # какая грань блока-основания нажата (0..5)
        self.hit = hit                    # точка клика внутри блока, куда ставим (доли 0..1)
        self.look = look                  # горизонтальное направление взгляда (2..5)
        self.look6 = look if look6 is None else look6   # ближайшее направление взгляда из 6 (с учётом наклона)
        self.yaw = yaw                    # градусы, 0 = юг (+Z), как в игре
        self.replacing_clicked = replacing_clicked

    @staticmethod
    def from_view(face, hit_world, block_pos, view_dir):
        """Из луча: face — грань основания, hit_world — точка попадания в мировых координатах Minecraft (x, y, z), block_pos — ставимый
        блок (x, y, z), view_dir — направление взгляда (dx, dy, dz) в осях Minecraft."""
        hx, hy, hz = hit_world[0] - block_pos[0], hit_world[1] - block_pos[1], hit_world[2] - block_pos[2]
        dx, dy, dz = view_dir
        if abs(dx) > abs(dz):
            look = 5 if dx > 0 else 4
        else:
            look = 3 if dz > 0 else 2
        best, bd = 2, -9
        for d in range(6):
            v = DIR_VEC[d]
            p = dx * v[0] + dy * v[1] + dz * v[2]
            if p > bd:
                best, bd = d, p
        yaw = math.degrees(math.atan2(-dx, dz))   # 0 = взгляд на юг
        return PlaceContext(face, (hx, hy, hz), look, best, yaw)


class _Txn:
    __slots__ = ('label', 'changes')

    def __init__(self, label):
        self.label = label
        self.changes = []   # (x, y, z, old, new) в порядке применения


class EditSession:
    def __init__(self, table, blocks_by_chunk, min_y=-64, height=384, pack_dir=None, max_undo=1000):
        self.t = table
        self.blocks = blocks_by_chunk
        self.min_y = min_y
        self.height = height
        self.max_undo = max_undo
        self.undo_stack = []
        self.redo_stack = []
        self._txn = None
        self._affected = set()
        self.layer = {}        # (cx, cz) -> {idx: [base, new]}
        self._info = {}
        self.air = table.state_id('minecraft:air')
        self.water = table.state_id('minecraft:water[level=0]')
        self._tag_wall_post_override = set()
        if pack_dir:
            try:
                from ..assets.state_table import load_block_tag
                self._tag_wall_post_override = load_block_tag(pack_dir, 'minecraft:wall_post_override')
            except Exception:
                pass

    # ------------------------------------------------------------------------------------------------------------------
    #                                              доступ к миру
    # ------------------------------------------------------------------------------------------------------------------
    def _arr(self, x, z):
        return self.blocks.get((x >> 4, z >> 4))

    def get(self, x, y, z):
        """id состояния либо -1 (за пределами высоты / чанк не загружен)."""
        yy = y - self.min_y
        if yy < 0 or yy >= self.height:
            return -1
        a = self._arr(x, z)
        if a is None:
            return -1
        return int(a[(yy * 16 + (z & 15)) * 16 + (x & 15)])

    def _set_raw(self, x, y, z, sid):
        yy = y - self.min_y
        if yy < 0 or yy >= self.height:
            return False
        key = (x >> 4, z >> 4)
        a = self.blocks.get(key)
        if a is None:
            return False
        idx = (yy * 16 + (z & 15)) * 16 + (x & 15)
        old = int(a[idx])
        if old == sid:
            return False
        a[idx] = sid
        if self._txn is not None:
            self._txn.changes.append((x, y, z, old, sid))
        self._record_layer(key, idx, old, sid)
        self._mark(x, y, z)
        return True

    def _record_layer(self, key, idx, old, new):
        d = self.layer.setdefault(key, {})
        e = d.get(idx)
        if e is None:
            d[idx] = [old, new]
        else:
            e[1] = new
        if d[idx][0] == d[idx][1]:
            del d[idx]
            if not d:
                del self.layer[key]

    def _mark(self, x, y, z):
        cx, cz = x >> 4, z >> 4
        self._affected.add((cx, cz))
        lx, lz = x & 15, z & 15
        xs = [cx - 1] if lx == 0 else ([cx + 1] if lx == 15 else [])
        zs = [cz - 1] if lz == 0 else ([cz + 1] if lz == 15 else [])
        for a in xs:
            self._affected.add((a, cz))
        for b in zs:
            self._affected.add((cx, b))
        for a in xs:      # диагональ: углы поверхности жидкости зависят от соседей по диагонали
            for b in zs:
                self._affected.add((a, b))

    def take_affected(self):
        """Возвращает и очищает множество чанков, меши которых надо пересобрать."""
        out = {k for k in self._affected if k in self.blocks}
        self._affected = set()
        return out

    # ------------------------------------------------------------------------------------------------------------------
    #                                              свойства состояний
    # ------------------------------------------------------------------------------------------------------------------
    def info(self, sid):
        """(короткое имя блока, словарь свойств) состояния; для -1 — ('air', {})."""
        if sid < 0:
            return 'air', {}
        r = self._info.get(sid)
        if r is None:
            base, props = self.t.state_props(sid)
            r = (base[10:] if base.startswith('minecraft:') else base, props)
            self._info[sid] = r
        return r

    def block(self, sid):
        return self.info(sid)[0]

    def prop(self, sid, name, default=None):
        return self.info(sid)[1].get(name, default)

    def with_props(self, sid, **kw):
        """Состояние того же блока с изменёнными свойствами (значения — строки/bool/int); неизвестные свойства игнорируются."""
        blk, props = self.info(sid)
        new = dict(props)
        for k, v in kw.items():
            if k in new:
                new[k] = ('true' if v else 'false') if isinstance(v, bool) else str(v)
        if new == props:
            return sid
        r = self.t.state_from_props('minecraft:' + blk, new)
        return sid if r < 0 else r

    def is_air(self, sid):
        return sid < 0 or self.block(sid) in ('air', 'cave_air', 'void_air')

    def sturdy(self, sid, face):
        """Грань face блока целиком закрыта (isFaceSturdy)."""
        return sid >= 0 and bool((int(self.t.st_sturdy[sid]) >> face) & 1)

    # ------------------------------------------------------------------------------------------------------------------
    #                                           правила автосвязей
    # ------------------------------------------------------------------------------------------------------------------
    def _is_exception(self, sid):
        b = self.block(sid)
        return b in LEAVES or b in CONNECT_EXCEPTIONS or b in SHULKERS

    def _gate_connects(self, sid, direction):
        """FenceGateBlock.connectsToDirection: ось facing перпендикулярна направлению (направление — от ворот к соединяющемуся)."""
        f = DIR_BY_NAME.get(self.prop(sid, 'facing', 'north'), 2)
        return (f in (2, 3)) == (cw(direction) in (2, 3)) if direction in HORIZ else False

    def _fence_connects(self, own_block, nsid, face_solid, direction):
        nb = self.block(nsid)
        same = nb in FENCES and ((nb != 'nether_brick_fence') == (own_block != 'nether_brick_fence'))
        gate = nb in FENCE_GATES and self._gate_connects(nsid, direction)
        return (not self._is_exception(nsid) and face_solid) or same or gate

    def _pane_attaches(self, nsid, face_solid):
        nb = self.block(nsid)
        return (not self._is_exception(nsid) and face_solid) or nb in PANES or nb in WALLS

    def _wall_connects(self, nsid, face_solid, direction):
        nb = self.block(nsid)
        gate = nb in FENCE_GATES and self._gate_connects(nsid, direction)
        return nb in WALLS or (not self._is_exception(nsid) and face_solid) or nb in PANES or gate

    def _above_covers(self, asid, test):
        """isCovered(aboveShape, TEST_SHAPE): нижняя грань блока выше закрывает проверочную полосу. test: 'post' | направление 2..5."""
        if asid < 0:
            return False
        m = np.unpackbits(self.t.occ_masks[int(self.t.st_occ[asid][0])]).reshape(16, 16).astype(bool)   # [u=x][v=z]
        if not m.any() and self.sturdy(asid, 0):
            return True
        if test == 'post':
            return bool(m[7:9, 7:9].all())
        if test == 2:      # север: x 7..9, z 0..9
            return bool(m[7:9, 0:9].all())
        if test == 3:
            return bool(m[7:9, 7:16].all())
        if test == 4:
            return bool(m[0:9, 7:9].all())
        return bool(m[7:16, 7:9].all())

    def _wall_state(self, sid, pos, n, e, s, w, asid):
        """WallBlock.updateShape(level, state, topPos, topState, north, east, south, west): -> новое состояние."""
        def side(conn, d):
            if not conn:
                return 'none'
            return 'tall' if self._above_covers(asid, d) else 'low'
        sides = {'north': side(n, 2), 'east': side(e, 5), 'south': side(s, 3), 'west': side(w, 4)}
        # shouldRaisePost
        up = None
        ab = self.block(asid)
        if ab in WALLS and self.prop(asid, 'up') == 'true':
            up = True
        else:
            nn, ss, ee, ww = (sides['north'] == 'none', sides['south'] == 'none', sides['east'] == 'none', sides['west'] == 'none')
            corner = (nn and ss and ww and ee) or (nn != ss) or (ww != ee)
            if corner:
                up = True
            else:
                high = (sides['north'] == 'tall' and sides['south'] == 'tall') or (sides['east'] == 'tall' and sides['west'] == 'tall')
                up = False if high else (ab in self._tag_wall_post_override or self._above_covers(asid, 'post'))
        return self.with_props(sid, up=up, **sides)

    def _stairs_shape(self, sid, x, y, z):
        facing = DIR_BY_NAME[self.prop(sid, 'facing')]
        half = self.prop(sid, 'half')

        def is_stairs(s):
            return s >= 0 and self.block(s) in STAIRS

        def can_take(nd):
            v = DIR_VEC[nd]
            ns = self.get(x + v[0], y + v[1], z + v[2])
            return (not is_stairs(ns)) or self.prop(ns, 'facing') != self.prop(sid, 'facing') or self.prop(ns, 'half') != half
        v = DIR_VEC[facing]
        behind = self.get(x + v[0], y + v[1], z + v[2])
        if is_stairs(behind) and self.prop(behind, 'half') == half:
            bf = DIR_BY_NAME[self.prop(behind, 'facing')]
            if (bf in (2, 3)) != (facing in (2, 3)) and can_take(OPP[bf]):
                return 'outer_left' if bf == ccw(facing) else 'outer_right'
        o = OPP[facing]
        v = DIR_VEC[o]
        front = self.get(x + v[0], y + v[1], z + v[2])
        if is_stairs(front) and self.prop(front, 'half') == half:
            ff = DIR_BY_NAME[self.prop(front, 'facing')]
            if (ff in (2, 3)) != (facing in (2, 3)) and can_take(ff):
                return 'inner_left' if ff == ccw(facing) else 'inner_right'
        return 'straight'

    def update_shape(self, x, y, z, sid, d):
        """BlockBehaviour.updateShape для состояния sid в (x, y, z), сосед в направлении d изменился. Возвращает новое состояние."""
        if sid < 0:
            return sid
        blk, props = self.info(sid)
        v = DIR_VEC[d]
        nx, ny, nz = x + v[0], y + v[1], z + v[2]
        ns = self.get(nx, ny, nz)
        if blk in FENCES and d in HORIZ:
            c = self._fence_connects(blk, ns, self.sturdy(ns, OPP[d]), OPP[d])
            return self.with_props(sid, **{DIR_NAME[d]: c})
        if blk in PANES and d in HORIZ:
            return self.with_props(sid, **{DIR_NAME[d]: self._pane_attaches(ns, self.sturdy(ns, OPP[d]))})
        if blk in WALLS and d != 0:
            conn = {2: props['north'] != 'none', 5: props['east'] != 'none', 3: props['south'] != 'none', 4: props['west'] != 'none'}
            if d in HORIZ:
                conn[d] = self._wall_connects(ns, self.sturdy(ns, OPP[d]), OPP[d])
            asid = self.get(x, y + 1, z) if d != 1 else ns
            return self._wall_state(sid, (x, y, z), conn[2], conn[5], conn[3], conn[4], asid)
        if blk in STAIRS and d in HORIZ:
            return self.with_props(sid, shape=self._stairs_shape(sid, x, y, z))
        if blk in FENCE_GATES and d in HORIZ:
            facing = DIR_BY_NAME[props['facing']]
            if (cw(facing) in (2, 3)) == (d in (2, 3)):
                other = self.get(x - v[0], y, z - v[2])
                inw = self.block(ns) in WALLS or self.block(other) in WALLS
                return self.with_props(sid, in_wall=inw)
            return sid
        if 'snowy' in props and d == 1:
            return self.with_props(sid, snowy=self.block(ns) in SNOW_BLOCKS)
        if blk in DOORS or blk in DOUBLE_PLANTS:
            half = props.get('half')
            if d in (0, 1) and (half == 'lower') == (d == 1):
                # соседняя половина должна быть тем же блоком с противоположной половиной
                ok = self.block(ns) == blk and self.prop(ns, 'half') != half
                if not ok:
                    return self.air
            return sid
        if blk in BEDS:
            part = props.get('part')
            facing = DIR_BY_NAME[props['facing']]
            nd = facing if part == 'foot' else OPP[facing]
            if d == nd:
                if self.block(ns) == blk and self.prop(ns, 'part') != part:
                    return sid
                return self.air
            return sid
        if blk == 'chorus_plant' and d in range(6):
            conn = self.block(ns) in ('chorus_plant', 'chorus_flower') or (d == 0 and self.block(ns) == 'end_stone')
            return self.with_props(sid, **{DIR_NAME[d]: conn})
        return sid

    def _propagate(self, positions, limit=20000):
        """Каскад updateShape от изменённых позиций (как updateNeighbourShapes с рекурсией)."""
        queue = list(positions)
        steps = 0
        while queue and steps < limit:
            x, y, z = queue.pop()
            steps += 1
            for d in range(6):
                v = DIR_VEC[d]
                px, py, pz = x + v[0], y + v[1], z + v[2]
                s = self.get(px, py, pz)
                if s < 0:
                    continue
                new = self.update_shape(px, py, pz, s, OPP[d])
                if new != s and new >= 0:
                    if self._set_raw(px, py, pz, new):
                        queue.append((px, py, pz))

    # ------------------------------------------------------------------------------------------------------------------
    #                                              транзакции / undo / redo
    # ------------------------------------------------------------------------------------------------------------------
    def begin(self, label='Правка'):
        if self._txn is not None:
            return
        self._txn = _Txn(label)

    def commit(self):
        """Завершает транзакцию (одна запись в стеке undo). Возвращает число изменённых блоков."""
        tx, self._txn = self._txn, None
        if tx is None or not tx.changes:
            return 0
        self.undo_stack.append(tx)
        if len(self.undo_stack) > self.max_undo:
            self.undo_stack.pop(0)
        self.redo_stack.clear()
        return len(tx.changes)

    def rollback(self):
        tx, self._txn = self._txn, None
        if tx is not None:
            for x, y, z, old, new in reversed(tx.changes):
                self._set_raw(x, y, z, old)

    def undo(self):
        if not self.undo_stack:
            return set()
        tx = self.undo_stack.pop()
        for x, y, z, old, new in reversed(tx.changes):
            self._set_raw(x, y, z, old)
        self.redo_stack.append(tx)
        return self.take_affected()

    def redo(self):
        if not self.redo_stack:
            return set()
        tx = self.redo_stack.pop()
        for x, y, z, old, new in tx.changes:
            self._set_raw(x, y, z, new)
        self.undo_stack.append(tx)
        return self.take_affected()

    def can_undo(self):
        return bool(self.undo_stack)

    def can_redo(self):
        return bool(self.redo_stack)

    # ------------------------------------------------------------------------------------------------------------------
    #                                              операции
    # ------------------------------------------------------------------------------------------------------------------
    def set_state(self, x, y, z, sid, update=True):
        """Ставит состояние как есть (+ каскад автосвязей у соседей). Вне транзакции — отдельный undo-шаг."""
        auto = self._txn is None
        if auto:
            self.begin('Правка блока')
        ok = self._set_raw(x, y, z, sid)
        if ok and update:
            self._propagate([(x, y, z)])
        if auto:
            self.commit()
        return ok

    def break_block(self, x, y, z):
        """Ломает блок: воздух (вода-источник, если блок был в воде); двойные блоки (двери, высокие растения, кровати) — обе половины."""
        auto = self._txn is None
        if auto:
            self.begin('Сломать блок')
        s = self.get(x, y, z)
        done = False
        if s >= 0 and not self.is_air(s):
            blk, props = self.info(s)
            repl = self.water if (props.get('waterlogged') == 'true' or blk in bp.ALWAYS_WATER) else self.air
            others = []
            if blk in DOORS or blk in DOUBLE_PLANTS:
                others.append((x, y + (1 if props.get('half') == 'lower' else -1), z))
            elif blk in BEDS:
                f = DIR_BY_NAME[props['facing']]
                nd = f if props.get('part') == 'foot' else OPP[f]
                v = DIR_VEC[nd]
                others.append((x + v[0], y, z + v[2]))
            self._set_raw(x, y, z, repl)
            done = True
            moved = [(x, y, z)]
            for ox, oy, oz in others:
                os_ = self.get(ox, oy, oz)
                if os_ >= 0 and self.block(os_) == blk:
                    p2 = self.info(os_)[1]
                    r2 = self.water if p2.get('waterlogged') == 'true' else self.air
                    self._set_raw(ox, oy, oz, r2)
                    moved.append((ox, oy, oz))
            self._propagate(moved)
        if auto:
            self.commit()
        return done

    def pick(self, x, y, z):
        """Пипетка: id состояния в позиции (либо -1)."""
        s = self.get(x, y, z)
        return s if not self.is_air(s) else -1

    # --- установка с ориентацией по виду/месту клика ---
    def state_for_placement(self, block, x, y, z, ctx, exact_state=None):
        """Состояние `block` (имя `minecraft:…`) при установке в (x, y, z) по правилам getStateForPlacement. Возвращает id либо -1.
        exact_state — id образца (пипетка): берётся как есть."""
        t = self.t
        if exact_state is not None and exact_state >= 0:
            return exact_state
        name = block[10:] if block.startswith('minecraft:') else block
        cur = self.get(x, y, z)
        # знаки/баннеры/головы: напольный или настенный вариант по грани
        wall_variants = {'_sign': '_wall_sign', '_banner': '_wall_banner', '_head': '_wall_head', '_skull': '_wall_skull'}
        horizontal_face = ctx.face in HORIZ
        for suf, wsuf in wall_variants.items():
            if name.endswith(suf) and 'wall' not in name and horizontal_face and t.default_state('minecraft:' + name[:-len(suf)] + wsuf) >= 0:
                name = name[:-len(suf)] + wsuf
                break
        if name in ('torch', 'soul_torch', 'redstone_torch', 'copper_torch') and horizontal_face:
            wn = {'torch': 'wall_torch', 'soul_torch': 'soul_wall_torch', 'redstone_torch': 'redstone_wall_torch', 'copper_torch': 'copper_wall_torch'}[name]
            if t.default_state('minecraft:' + wn) >= 0:
                name = wn
        sid = t.default_state('minecraft:' + name)
        if sid < 0:
            return -1
        blk, props = self.info(sid)
        new = {}
        hit_y = ctx.hit[1]
        top_half = (ctx.face == 0) or (ctx.face != 1 and hit_y > 0.5)
        # --- оси ---
        if 'axis' in props:
            if blk in ('chain', 'iron_chain') or 'axis' in props:
                new['axis'] = ('y', 'y', 'z', 'z', 'x', 'x')[ctx.face]
        # --- facing ---
        if 'facing' in props:
            possible = self._prop_values(blk, 'facing')
            six = 'up' in possible
            if blk in STAIRS or blk in DOORS or blk in FENCE_GATES or blk in BEDS or blk in ('anvil', 'chipped_anvil', 'damaged_anvil'):
                f = ctx.look
                if 'anvil' in blk:
                    f = cw(ctx.look)
                new['facing'] = DIR_NAME[f]
            elif blk in ('trapdoor',) or blk.endswith('_trapdoor'):
                if not ctx.replacing_clicked and horizontal_face:
                    new['facing'] = DIR_NAME[ctx.face]
                else:
                    new['facing'] = DIR_NAME[OPP[ctx.look]]
            elif blk in FACING_OPPOSITE or blk.endswith('glazed_terracotta') or blk.endswith('_chest'):
                new['facing'] = DIR_NAME[OPP[ctx.look]]
            elif blk in FACING6_OPPOSITE_LOOK:
                new['facing'] = DIR_NAME[OPP[ctx.look6]]
            elif blk in FACING_CLICKED_FACE or blk.endswith('shulker_box') or blk in ('hopper',):
                if blk == 'hopper':
                    f = OPP[ctx.face]
                    new['facing'] = 'down' if f == 1 else DIR_NAME[f]
                else:
                    new['facing'] = DIR_NAME[ctx.face]
            elif blk in ('ladder', 'wall_torch', 'soul_wall_torch', 'redstone_wall_torch', 'copper_wall_torch') or 'wall' in blk and 'facing' in props:
                new['facing'] = DIR_NAME[ctx.face] if horizontal_face else DIR_NAME[OPP[ctx.look]]
            elif 'face' in props:     # кнопка/рычаг/точило
                pass
            elif six:
                new['facing'] = DIR_NAME[OPP[ctx.look6]]
            else:
                new['facing'] = DIR_NAME[OPP[ctx.look]]
        if 'face' in props and 'facing' in props:   # AttachFace (кнопки, рычаги)
            if ctx.face == 1:
                new['face'], new['facing'] = 'floor', DIR_NAME[ctx.look]
            elif ctx.face == 0:
                new['face'], new['facing'] = 'ceiling', DIR_NAME[ctx.look]
            else:
                new['face'], new['facing'] = 'wall', DIR_NAME[ctx.face]
        if 'half' in props and (blk in STAIRS or blk.endswith('_trapdoor') or blk == 'trapdoor'):
            if blk in STAIRS:
                new['half'] = 'top' if top_half else 'bottom'
            elif not ctx.replacing_clicked and horizontal_face:
                new['half'] = 'top' if hit_y > 0.5 else 'bottom'
            else:
                new['half'] = 'bottom' if ctx.face == 1 else 'top'
        if blk in SLABS:
            if cur >= 0 and self.block(cur) == blk and self.prop(cur, 'type') != 'double':
                return self.with_props(cur, type='double', waterlogged=False)
            new['type'] = 'top' if top_half else 'bottom'
        if blk in ('snow',) and cur >= 0 and self.block(cur) == 'snow':
            n = int(self.prop(cur, 'layers', '1'))
            if n < 8:
                return self.with_props(cur, layers=n + 1)
        if 'rotation' in props:
            new['rotation'] = str(int(math.floor((180.0 + ctx.yaw) * 16.0 / 360.0 + 0.5)) & 15)
        if 'waterlogged' in props:
            new['waterlogged'] = 'true' if (cur >= 0 and (self.block(cur) == 'water' and self.prop(cur, 'level') == '0')) else 'false'
        if 'persistent' in props:
            new['persistent'] = 'true'
        if blk in DOORS:
            new['half'] = 'lower'
        if blk in DOUBLE_PLANTS:
            new['half'] = 'lower'
        if blk in BEDS:
            new['part'] = 'foot'
        s2 = t.state_from_props('minecraft:' + blk, {**props, **new})
        return s2 if s2 >= 0 else sid

    def _prop_values(self, blk, prop):
        """Допустимые значения свойства блока (по состояниям таблицы)."""
        key = ('vals', blk, prop)
        r = self._info.get(key)
        if r is None:
            r = set()
            bi = self.t.block_names.index('minecraft:' + blk) if 'minecraft:' + blk in self.t.block_names else -1
            if bi >= 0:
                idx = np.nonzero(self.t.st_block == bi)[0]
                for sid in idx[:64]:
                    v = self.info(int(sid))[1].get(prop)
                    if v is not None:
                        r.add(v)
            self._info[key] = r
        return r

    def place(self, x, y, z, block, ctx=None, exact_state=None, replace=True):
        """Ставит блок `block` (имя) в (x, y, z). Возвращает список поставленных позиций (двери, кровати — несколько) либо []."""
        ctx = ctx or PlaceContext()
        if not replace and not self.is_air(self.get(x, y, z)) and self.block(self.get(x, y, z)) not in ('water', 'lava', 'snow', 'short_grass', 'tall_grass', 'fern'):
            return []
        sid = self.state_for_placement(block, x, y, z, ctx, exact_state)
        if sid < 0 or self.get(x, y, z) < 0:
            return []
        blk, props = self.info(sid)
        auto = self._txn is None
        if auto:
            self.begin('Поставить блок')
        placed = []
        extra = []
        if blk in DOORS:
            upper = self.with_props(sid, half='upper')
            if exact_state is None:
                sid = self.with_props(sid, hinge=self._door_hinge(x, y, z, ctx))
                upper = self.with_props(sid, half='upper')
            if self.get(x, y + 1, z) < 0:
                if auto:
                    self.rollback()
                return []
            extra.append((x, y + 1, z, upper))
        elif blk in DOUBLE_PLANTS:
            if self.get(x, y + 1, z) < 0:
                if auto:
                    self.rollback()
                return []
            extra.append((x, y + 1, z, self.with_props(sid, half='upper')))
        elif blk in BEDS:
            f = DIR_BY_NAME[props['facing']]
            v = DIR_VEC[f]
            if self.get(x + v[0], y, z + v[2]) < 0:
                if auto:
                    self.rollback()
                return []
            extra.append((x + v[0], y, z + v[2], self.with_props(sid, part='head')))
        self._set_raw(x, y, z, sid)
        placed.append((x, y, z))
        for ex, ey, ez, es in extra:
            self._set_raw(ex, ey, ez, es)
            placed.append((ex, ey, ez))
        # собственные автосвязи нового блока: он считает форму по соседям, как getStateForPlacement
        self._self_update(placed)
        self._propagate(placed)
        if auto:
            self.commit()
        return placed

    def _door_hinge(self, x, y, z, ctx):
        d = ctx.look
        left, right = ccw(d), cw(d)
        lv, rv = DIR_VEC[left], DIR_VEC[right]
        ls = self.get(x + lv[0], y, z + lv[2])
        las = self.get(x + lv[0], y + 1, z + lv[2])
        rs = self.get(x + rv[0], y, z + rv[2])
        ras = self.get(x + rv[0], y + 1, z + rv[2])
        full = lambda s: s >= 0 and bool(self.t.st_flags[s] & 4)    # F.OPAQUE как «полная коллизия»
        balance = (-1 if full(ls) else 0) + (-1 if full(las) else 0) + (1 if full(rs) else 0) + (1 if full(ras) else 0)
        door_l = ls >= 0 and self.block(ls) in DOORS and self.prop(ls, 'half') == 'lower'
        door_r = rs >= 0 and self.block(rs) in DOORS and self.prop(rs, 'half') == 'lower'
        if (not door_l or door_r) and balance <= 0:
            if (not door_r or door_l) and balance >= 0:
                sx, sz = DIR_VEC[d][0], DIR_VEC[d][2]
                cxh, czh = ctx.hit[0], ctx.hit[2]
                ok = (sx >= 0 or not czh < 0.5) and (sx <= 0 or not czh > 0.5) and (sz >= 0 or not cxh > 0.5) and (sz <= 0 or not cxh < 0.5)
                return 'left' if ok else 'right'
            return 'left'
        return 'right'

    def _self_update(self, positions):
        """Состояние только что поставленного блока пересчитывается по соседям (аналог getStateForPlacement у заборов, панелей, стен и т. д.)."""
        for (x, y, z) in positions:
            s = self.get(x, y, z)
            for _ in range(2):
                for d in range(6):
                    v = DIR_VEC[d]
                    new = self.update_shape(x, y, z, self.get(x, y, z), d)
                    if new != self.get(x, y, z) and new >= 0 and self.block(new) == self.block(self.get(x, y, z)):
                        self._set_raw(x, y, z, new)
            _ = s

    # ------------------------------------------------------------------------------------------------------------------
    #                                              сохранение / наложение
    # ------------------------------------------------------------------------------------------------------------------
    def to_dict(self):
        """Слой правок в компактном виде: имена состояний вместо чисел (переносимо между версиями ресурсов)."""
        names = []
        idx = {}

        def nid(sid):
            n = self.t.names[sid]
            i = idx.get(n)
            if i is None:
                i = idx[n] = len(names)
                names.append(n)
            return i
        chunks = {}
        for (cx, cz), d in self.layer.items():
            chunks['%d,%d' % (cx, cz)] = [[i, nid(b), nid(n)] for i, (b, n) in sorted(d.items())]
        return {'format': 1, 'min_y': self.min_y, 'height': self.height, 'states': names, 'chunks': chunks}

    def to_json(self):
        return json.dumps(self.to_dict(), separators=(',', ':'))

    def load_dict(self, data, apply=True):
        """Загружает слой правок и (apply=True) накладывает на текущие массивы. Возвращает (применено, конфликтов, затронутые чанки).
        Конфликт — исходное состояние блока в мире не совпало с записанным (рельеф изменился)."""
        names = data['states']
        ids = [self.t.state_id(n) for n in names]
        applied = conflicts = 0
        for k, lst in data['chunks'].items():
            cx, cz = (int(v) for v in k.split(','))
            a = self.blocks.get((cx, cz))
            if a is None:
                continue
            for i, b, n in lst:
                nb, nn = ids[b], ids[n]
                if nn < 0:
                    continue
                if apply:
                    if int(a[i]) != nb and nb >= 0:
                        conflicts += 1
                    a[i] = nn
                    applied += 1
                d = self.layer.setdefault((cx, cz), {})
                d[i] = [nb if nb >= 0 else int(a[i]), nn]
                self._affected.add((cx, cz))
        return applied, conflicts, self.take_affected()

    def apply_layer(self):
        """Накладывает слой правок на текущие массивы (после перегенерации). -> число применённых блоков."""
        n = 0
        for key, d in self.layer.items():
            a = self.blocks.get(key)
            if a is None:
                continue
            for i, (b, nw) in d.items():
                a[i] = nw
                n += 1
            self._affected.add(key)
        return n

    def clear_history(self):
        self.undo_stack.clear()
        self.redo_stack.clear()
