#!/usr/bin/env python3
"""Слой L2 аудита: статический скан Java-кода фич/карверов/размещения/структур/материалов.
Для каждого класса собирает: какие данные окружения он ЧИТАЕТ (блоки, жидкости, карты высот, биом, небо, шумы),
что ПИШЕТ (блоки/сущности/пост-обработка), какие теги и конкретные блоки упоминает, сколько вызовов RNG делает.
Выход: data/code-reads-<V>.json (+ таблица в docs/06). Версия по умолчанию 26.3.
"""
import json, os, re, sys, collections

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
V = sys.argv[1] if len(sys.argv) > 1 else '26.3'
SRC = f'{ROOT}/src/dec/{V}/net/minecraft/world/level'
DIRS = ['levelgen/feature', 'levelgen/placement', 'levelgen/carver', 'levelgen/structure/structures', 'levelgen/material',
        'levelgen/blockpredicates', 'levelgen/feature/treedecorators', 'levelgen/feature/trunkplacers', 'levelgen/feature/foliageplacers']

READ = {
    'getBlockState': r'\.getBlockState\(', 'isStateAtPosition': r'isStateAtPosition\(', 'isFluidAtPosition': r'isFluidAtPosition\(',
    'getFluidState': r'getFluidState\(', 'isEmptyBlock/isAir': r'\.isEmptyBlock\(|\.isAir\(\)', 'canSeeSky': r'canSeeSky\(',
    'getHeight(heightmap)': r'\.getHeight\(|getHeightmapPos\(|getBaseHeight\(|getFirstOccupiedHeight|getFirstFreeHeight',
    'getBiome': r'\.getBiome\(', 'brightness/light': r'getBrightness\(|getRawBrightness\(',
    'sea level / y bounds': r'getSeaLevel\(|getMinY\(|getMaxY\(|getHeight\(\)|isInsideBuildHeight|isOutsideBuildHeight',
    'getChunk/hasChunk': r'\.getChunk\(|hasChunk\(', 'structureManager': r'structureManager|StructureManager|hasAnyStructureAt',
    'noise (Perlin/Simplex/Normal)': r'NormalNoise|PerlinNoise|SimplexNoise|SurfaceNoise|Noise\b|\.getValue\(',
    'world seed': r'getSeed\(\)|levelSeed|\.seed\(\)',
}
WRITE = {
    'setBlock/setBlockState': r'\.setBlock\(|\.setBlockState\(|setBlockIfAir|placeBlock\(',
    'postProcessing/fluid tick': r'markPosForPostProcessing|scheduleTick|scheduleFluidTick|neighborShapeChanged',
    'blockEntity/loot': r'setBlockEntity|getBlockEntity|setLootTable|RandomizableContainer',
    'entities': r'addFreshEntity|createEntity|spawn',
    'heightmap update': r'Heightmap\.primeHeightmaps|update\(',
}
RAND = r'random\.next\w*\(|\.nextInt\(|\.nextFloat\(|\.nextBoolean\(|\.nextDouble\(|\.nextLong\(|\.nextGaussian\('

def scan(path):
    t = open(path, encoding='utf-8', errors='ignore').read()
    t = re.sub(r'/\*.*?\*/', '', t, flags=re.S); t = re.sub(r'//.*', '', t)
    reads = {k: len(re.findall(p, t)) for k, p in READ.items()}
    writes = {k: len(re.findall(p, t)) for k, p in WRITE.items()}
    return {
        'reads': {k: v for k, v in reads.items() if v}, 'writes': {k: v for k, v in writes.items() if v},
        'block_tags': sorted(set(re.findall(r'BlockTags\.([A-Z_0-9]+)', t))), 'fluid_tags': sorted(set(re.findall(r'FluidTags\.([A-Z_0-9]+)', t))),
        'biome_tags': sorted(set(re.findall(r'BiomeTags\.([A-Z_0-9]+)', t))),
        'blocks': sorted(set(re.findall(r'\bBlocks\.([A-Z_0-9]+)', t))), 'heightmaps': sorted(set(re.findall(r'Heightmap\.Types\.([A-Z_]+)', t))),
        'rng_calls': len(re.findall(RAND, t)), 'lines': t.count('\n'),
    }

def main():
    out = {}
    for d in DIRS:
        base = f'{SRC}/{d}'
        if not os.path.isdir(base):
            continue
        for f in sorted(os.listdir(base)):
            if f.endswith('.java') and f != 'package-info.java':
                out[f'{d}/{f[:-5]}'] = scan(f'{base}/{f}')
    # шумы, на которые ссылается Java-код напрямую (Noises.X) — по всему world/level
    nj = collections.defaultdict(set)
    for dp, _, fs in os.walk(SRC):
        for f in fs:
            if f.endswith('.java') and f not in ('Noises.java', 'NoiseData.java', 'NoiseRouterData.java'):
                t = open(os.path.join(dp, f), encoding='utf-8', errors='ignore').read()
                for m in set(re.findall(r'\bNoises\.([A-Z_0-9]+)', t)):
                    nj[m.lower()].add(f[:-5])
    out['__noise_java_users__'] = {k: sorted(v) for k, v in nj.items()}
    json.dump(out, open(f'{ROOT}/data/code-reads-{V}.json', 'w'), indent=1, ensure_ascii=False)
    # краткая сводка: классы, которые читают блоки окружения И пишут блоки
    both = [k for k, v in out.items() if not k.startswith('__') and v['reads'] and v['writes']]
    print('classes', len(out), '| read+write', len(both))

if __name__ == '__main__':
    main()
