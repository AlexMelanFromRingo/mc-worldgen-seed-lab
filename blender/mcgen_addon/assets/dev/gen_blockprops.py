#!/usr/bin/env python3
"""Генератор assets/blockprops_data.py — блок-уровневые факты, которые в игре закодированы в Java (не в данных/ресурсах):
canOcclude, isSolid, render shape INVISIBLE, смещение (offsetType), классы skipRendering, жидкость «всегда вода», классы блоков.

Источник — настоящий код игры через BlockFacts.java (нужны jars/game-<V>.jar и src/bundle-<V>; запуск в корне репозитория):

    CP=$(tools/classpath.sh 26.3); mkdir -p /tmp/bf && javac -nowarn -d /tmp/bf -cp "$CP" blender/mcgen_addon/assets/dev/BlockFacts.java
    java -Xss8m --sun-misc-unsafe-memory-access=allow -cp "/tmp/bf:$CP" BlockFacts /tmp/facts-26.3.json
    python3 blender/mcgen_addon/assets/dev/gen_blockprops.py /tmp/facts-26.1.json /tmp/facts-26.2.json /tmp/facts-26.3.json /tmp/facts-26.4-snapshot-2.json

Несколько версий объединяются по имени блока (конфликты печатаются). Сами дампы (содержат оклюзионные маски состояний) в репозиторий не кладутся.
"""
import collections
import json
import os
import sys

CLASSES = [
    'FenceBlock', 'FenceGateBlock', 'IronBarsBlock', 'WallBlock', 'StairBlock', 'SlabBlock', 'DoorBlock', 'TrapDoorBlock',
    'BedBlock', 'DoublePlantBlock', 'SnowLayerBlock', 'GrassBlock', 'ChorusPlantBlock', 'VineBlock', 'StemBlock',
    'AttachedStemBlock', 'LeavesBlock', 'PistonBaseBlock', 'MultifaceBlock', 'CarpetBlock', 'FlowerBedBlock', 'ChainBlock',
    'LayeredCauldronBlock', 'AbstractCauldronBlock', 'HalfTransparentBlock', 'TransparentBlock', 'SignBlock', 'ShelfBlock',
    # блоки с особыми (entity) моделями — геометрии в JSON нет
    'AbstractChestBlock', 'ShulkerBoxBlock', 'AbstractBannerBlock', 'AbstractSkullBlock', 'BellBlock', 'ConduitBlock',
    'DecoratedPotBlock', 'EnchantingTableBlock', 'EndPortalBlock', 'EndGatewayBlock', 'CopperGolemStatueBlock',
]


def short(n):
    return n[10:] if n.startswith('minecraft:') else n


def main(paths, out):
    by = {}
    conflicts = collections.Counter()
    for p in paths:
        with open(p) as f:
            data = json.load(f)
        agg = collections.defaultdict(list)
        for x in data:
            agg[x['b']].append(x)
        for b, st in agg.items():
            x = st[0]
            rec = {
                'co': x['co'], 'solid': x['solid'], 'rs': x['rs'], 'off': (x['off'], x['mh'], x['mv']), 'skip': x['skip'],
                'water': int(all(s['fl'] == 'minecraft:water' for s in st)), 'sup': x['sup'].split('>'),
                'lum': max(s['le'] for s in st),
            }
            if b in by:
                for k in ('co', 'solid', 'rs', 'off', 'skip'):
                    if by[b][k] != rec[k]:
                        conflicts[(b, k)] += 1
                        print('конфликт', b, k, by[b][k], rec[k], file=sys.stderr)
                by[b]['sup'] = rec['sup']
            else:
                by[b] = rec
    names = sorted(by)
    lines = ['"""Блок-уровневые факты игры 26.x, закодированные в Java (не в ресурсах). СГЕНЕРИРОВАНО dev/gen_blockprops.py — не править руками.',
             '', 'Имена без префикса `minecraft:`."""', '']

    def fs(name, items, doc):
        lines.append('# ' + doc)
        lines.append('%s = frozenset((' % name)
        row = ''
        for it in sorted(items):
            tok = repr(it) + ', '
            if len(row) + len(tok) > 118:
                lines.append('    ' + row.rstrip())
                row = ''
            row += tok
        if row:
            lines.append('    ' + row.rstrip())
        lines.append('))')
        lines.append('')

    fs('NO_OCCLUDE', [short(b) for b in names if by[b]['co'] == 0], 'BlockBehaviour.Properties.noOcclusion(): блок не скрывает соседние грани')
    fs('NOT_SOLID', [short(b) for b in names if by[b]['solid'] == 0], 'BlockState.isSolid() == false (учитывается жидкостью при расчёте высоты у берега)')
    fs('INVISIBLE', [short(b) for b in names if by[b]['rs'] == 'INVISIBLE'], 'RenderShape.INVISIBLE (воздух, жидкости, барьер, свет, structure_void, …)')
    fs('ALWAYS_WATER', [short(b) for b in names if by[b]['water']], 'блоки, у которых все состояния содержат воду-источник (морская трава, ламинария, столб пузырей)')
    lines.append('# смещение модели (OffsetType): имя -> (1 = XZ | 2 = XYZ, макс. по горизонтали, макс. по вертикали) в блоках')
    lines.append('OFFSET = {')
    for b in names:
        o = by[b]['off']
        if o[0]:
            lines.append('    %r: (%d, %r, %r),' % (short(b), o[0], round(o[1], 6), round(o[2], 6)))
    lines.append('}')
    lines.append('')
    lines.append('# Block.skipRendering: H = HalfTransparentBlock (сосед того же блока), L = листва, B = IronBars/панели, P = порошковый снег,')
    lines.append('# M = корни мангров, W = жидкость')
    lines.append('SKIP = {')
    for b in names:
        if by[b]['skip']:
            lines.append('    %r: %r,' % (short(b), by[b]['skip']))
    lines.append('}')
    lines.append('')
    lines.append('# имена блоков по классам Java (включая наследников)')
    lines.append('CLASS_MEMBERS = {')
    for c in CLASSES:
        mem = [short(b) for b in names if c in by[b]['sup']]
        lines.append('    %r: frozenset((' % c)
        row = ''
        for it in sorted(mem):
            tok = repr(it) + ', '
            if len(row) + len(tok) > 112:
                lines.append('        ' + row.rstrip())
                row = ''
            row += tok
        if row:
            lines.append('        ' + row.rstrip())
        lines.append('    )),')
    lines.append('}')
    lines.append('')
    with open(out, 'w') as f:
        f.write('\n'.join(lines))
    print('записано', out, 'блоков:', len(names), 'конфликтов:', sum(conflicts.values()))


if __name__ == '__main__':
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)
    main(sys.argv[1:], os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'blockprops_data.py'))
