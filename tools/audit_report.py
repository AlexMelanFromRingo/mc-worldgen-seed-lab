#!/usr/bin/env python3
"""Собирает docs/06-interactions.md из data/interactions-<V>.json (L1) и data/code-reads-<V>.json (L2)."""
import json, os, collections, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
V = '26.3'
L = {v: json.load(open(f'{ROOT}/data/interactions-{v}.json')) for v in ['26.1', '26.2', '26.3']}
C = json.load(open(f'{ROOT}/data/code-reads-{V}.json'))
NJ = C.pop('__noise_java_users__', {})
d = L[V]
pf = d['placed_features']
STEP = ['raw_generation', 'lakes', 'local_modifications', 'underground_structures', 'surface_structures', 'strongholds',
        'underground_ores', 'underground_decoration', 'fluid_springs', 'vegetal_decoration', 'top_layer_modification']

def cell(x):
    return ', '.join(f'`{i}`' for i in x) if x else '—'

out = []
w = out.append
w(f'# 06. Систематический аудит взаимосвязей генерации (версия {V}; слои L1 + L2)\n')
w('Цель: не полагаться на память о «приколах» генерации, а **вывести все связи из кода и данных автоматически**. '
  'Три слоя: **L1** — граф данных датапака (`tools/audit_interactions.py` → `data/interactions-<V>.json`); '
  '**L2** — статический скан Java-классов фич/карверов/размещения/структур/материалов (`tools/audit_code_reads.py` → `data/code-reads-<V>.json`); '
  '**L3** — эмпирика на настоящем сервере (см. §9: требует принять Mojang EULA пользователем — я этого не делал).\n')
w('Отчёт генерируется скриптом `tools/audit_report.py`; сырые данные — в `data/`. Связи в L1 — **потенциальные** (пересечение «пишет/читает» по блокам и тегам с учётом порядка), реальные зависят от геометрии.\n')

# --- 1. охват ---
n_pf = len(pf); n_in_biome = sum(1 for v in pf.values() if v['biomes'])
w('## 1. Охват\n')
w(f'| | 26.1 | 26.2 | 26.3 |\n|---|---|---|---|')
w(f"| размещаемых фич (`placed_feature`) | {len(L['26.1']['placed_features'])} | {len(L['26.2']['placed_features'])} | {len(L['26.3']['placed_features'])} |")
w(f"| из них подключено хотя бы к одному биому | {sum(1 for v in L['26.1']['placed_features'].values() if v['biomes'])} | {sum(1 for v in L['26.2']['placed_features'].values() if v['biomes'])} | {sum(1 for v in L['26.3']['placed_features'].values() if v['biomes'])} |")
w(f"| потенциальных влияний «ранняя фича → поздняя» (Overworld) | {len(L['26.1']['feature_edges'])} | {len(L['26.2']['feature_edges'])} | {len(L['26.3']['feature_edges'])} |")
w(f"| структур / наборов | {len(L['26.1']['structures'])} / {len(L['26.1']['structure_sets'])} | {len(L['26.2']['structures'])} / {len(L['26.2']['structure_sets'])} | {len(L['26.3']['structures'])} / {len(L['26.3']['structure_sets'])} |")
w(f"| шумов, на которые есть ссылки в данных | {len(L['26.1']['noise_users'])} | {len(L['26.2']['noise_users'])} | {len(L['26.3']['noise_users'])} |")
w(f"| Java-классов в скане L2 (26.3) | | | {len(C)} |\n")

# --- 2. карты высот ---
w('## 2. Карты высот: какие фичи зависят от того, что уже поставили другие\n')
w('Карты высот `*_WG` считаются **только по рельефу** (до фич). Остальные (`MOTION_BLOCKING`, `OCEAN_FLOOR`, `WORLD_SURFACE`, `MOTION_BLOCKING_NO_LEAVES`) — «живые»: обновляются по мере установки блоков, поэтому **фича, использующая живую карту, видит результат работы более ранних фич** (деревья, структуры, озёра). Это порядок «чтение-после-записи» (см. `docs/00` §16).\n')
hm = collections.defaultdict(list)
for k, v in pf.items():
    if not v['biomes']:
        continue
    for h in v['heightmaps']:
        hm[h].append(k)
w('| Карта высот | Тип | Фич | Примеры |\n|---|---|---|---|')
for h, l in sorted(hm.items(), key=lambda x: -len(x[1])):
    kind = 'рельеф' if h.endswith('_WG') else 'живая'
    w(f'| `{h}` | {kind} | {len(l)} | {cell(sorted(l)[:6])} |')
w('')

# --- 3. влияния между фичами ---
w('## 3. Потенциальные влияния между фичами Overworld (L1)\n')
w('Критерий: фича *A* стоит раньше фичи *B* в глобальном порядке (`FeatureSorter`) и **пишет** блок (не вездесущий: исключены воздух, вода, лава, камень, глубинный сланец, земля, трава, бедрок, гравий, песок, туф), который *B* **читает** (предикаты, теги, `target`). Ниже — сводка по классам причин, полный список — `data/interactions-26.3.json → feature_edges`.\n')
E = d['feature_edges']
rd = collections.defaultdict(lambda: collections.defaultdict(set))
for e in E:
    rd[e['reader']][e['writer']] |= set(e['blocks'])
w('### 3.1. Самые «зависимые» фичи (читают результаты наибольшего числа ранних фич)\n')
w('| Читающая фича | Шаг | Сколько ранних фич влияет | Типичные блоки-связки |\n|---|---|---|---|')
for r, ws in sorted(rd.items(), key=lambda x: -len(x[1]))[:15]:
    blocks = collections.Counter(b for s in ws.values() for b in s)
    steps = pf[r]['steps']
    w(f"| `{r}` | {','.join(STEP[s] for s in steps[:1]) if steps else '—'} | {len(ws)} | {cell([b for b, _ in blocks.most_common(5)])} |")
w('')
w('### 3.2. Классы связей\n')
cls = collections.Counter()
ex = {}
def klass(e):
    b = set(e['blocks'])
    if b <= {'granite', 'diorite', 'andesite', 'calcite', 'dripstone_block', 'tuff'}:
        return 'жилы камня/руд перезаписывают друг друга (общий тег `stone_ore_replaceables`)'
    if b & {'moss_block', 'pale_moss_block', 'clay', 'azalea', 'rooted_dirt', 'moss_carpet'}:
        return 'цепочка лаш-пещер: глина/мох → растительность → азалия (`azalea_grows_on`)'
    if b & {'sulfur'}:
        return 'сульфурные пещеры (26.2+): источник → бассейн'
    if b & {'packed_ice', 'blue_ice', 'ice'}:
        return 'лёд/айсберги ↔ последующие фичи'
    if b & {'sugar_cane', 'tall_grass', 'grass', 'fern', 'large_fern'}:
        return 'растительность на поверхности: растения ↔ растения'
    if any(x.endswith('_leaves') or x.endswith('_log') for x in b):
        return 'деревья: листья/стволы мешают позднейшим кустам/цветам'
    return 'прочее'
for e in E:
    k = klass(e); cls[k] += 1
    ex.setdefault(k, []).append(f"`{e['writer']}` → `{e['reader']}`")
w('| Класс связи | Рёбер | Примеры |\n|---|---|---|')
for k, n in cls.most_common():
    w(f"| {k} | {n} | {'; '.join(ex[k][:3])} |")
w('')
w('Шаги (writer → reader): ' + ', '.join(f'{STEP[a]}→{STEP[b]}: {n}' for (a, b), n in sorted(collections.Counter((e['writer_step'], e['reader_step']) for e in E).items(), key=lambda x: -x[1])[:8]) + '.\n')

# --- 4. структуры ---
w('## 4. Структуры: шаг, адаптация рельефа, биомы\n')
S = d['structures']
grp = collections.defaultdict(list)
for k, s in S.items():
    grp[(s['step'], s['terrain_adaptation'] or 'none')].append(k)
w('| Шаг генерации | `terrain_adaptation` | Структур | Примеры |\n|---|---|---|---|')
for (st, ta), l in sorted(grp.items(), key=lambda x: (str(x[0][0]), str(x[0][1]))):
    w(f'| `{st}` | `{ta}` | {len(l)} | {cell(sorted(l)[:5])} |')
w('\nСмысл: `terrain_adaptation` ≠ `none` ⇒ структура **меняет плотность рельефа** вокруг себя (beardifier) ещё *до* этапа рельефа; шаг (`step`) задаёт, **когда** блоки структуры ставятся относительно фич (структуры шага ставятся перед фичами того же шага). Структуры с `bury`/`encapsulate`/`beard_box` «врастают» в землю — их видимость зависит от рельефа.\n')
w('Наборы с зависимостями между собой: ' + ', '.join(f"`{k}` (exclusion → `{v['exclusion']['other_set']}` r={v['exclusion']['chunk_count']})" for k, v in d['structure_sets'].items() if v.get('exclusion')) + '.\n')

# --- 5. общие шумы ---
w('## 5. Общие шумы и density-функции (одна функция — несколько потребителей)\n')
w('Каждый шум (`worldgen/noise/*.json`) может читаться density-функциями (данные) и/или напрямую Java-кодом (`Noises.X`). Таблица — шумы с **двумя и более** потребителями (это и есть места, где подсистемы делят один и тот же шум):\n')
w('| Шум | Density-функции/конфиги (данные) | Java-классы (`Noises.X`) |\n|---|---|---|')
nu = d['noise_users']
allnames = sorted(set(nu) | set(NJ))
shown = 0
for n in allnames:
    dat = sorted({x for v in nu.get(n, {}).values() for x in v}); jv = NJ.get(n, [])
    if len(dat) + len(jv) >= 2:
        w(f"| `{n}` | {', '.join(dat[:5]) or '—'}{'…' if len(dat) > 5 else ''} | {', '.join(jv[:5]) or '—'} |"); shown += 1
if not shown:
    w('| — | — | — |')
w('')
w('Density-функции, на которые ссылаются **разные виды** конфигов (общий «источник правды»):\n')
w('| Функция | Кто использует |\n|---|---|')
fi = d['density_fan_in']
for n, u in sorted(fi.items(), key=lambda x: -sum(len(v) for v in x[1].values())):
    kinds = {k: v for k, v in u.items() if not (k == 'density_function' and len(u) == 1)}
    if len(u) >= 2:
        w(f"| `{n}` | {'; '.join(f'{k}: ' + ', '.join(v[:3]) + ('…' if len(v) > 3 else '') for k, v in u.items())} |")
w('')

# --- 6. карверы ---
w('## 6. Карверы\n')
w('| Карвер | Тип | Вероятность | y | lava_level | В биомах |\n|---|---|---|---|---|---|')
for k, c in d['carvers'].items():
    w(f"| `{k}` | {c['type']} | {c['probability']} | `{json.dumps(c['y'], ensure_ascii=False)[:60]}` | `{c['lava_level']}` | {len(d['carver_biomes'].get(k, []))} |")
w('')

# --- 7. код (L2) ---
w('## 7. Скан Java-кода (L2): кто что читает и пишет\n')
feat = {k: v for k, v in C.items() if k.startswith('levelgen/feature/') and k.count('/') == 2}
w('### 7.1. Классы фич, читающие окружение и пишущие блоки (топ-20 по числу чтений)\n')
w('| Класс | Чтения (по API) | Записи | Теги блоков | Карты высот | RNG-вызовов |\n|---|---|---|---|---|---|')
for k, v in sorted(feat.items(), key=lambda x: -sum(x[1]['reads'].values()))[:20]:
    w(f"| `{k.split('/')[-1]}` | {', '.join(f'{a}×{b}' for a, b in v['reads'].items())} | {', '.join(f'{a}×{b}' for a, b in v['writes'].items())} | {cell(v['block_tags'][:4])} | {cell(v['heightmaps'])} | {v['rng_calls']} |")
w('')
seedc = [k for k, v in C.items() if 'world seed' in v['reads']]
w('### 7.2. Классы, использующие мировой seed напрямую (а не только через переданный `RandomSource`)\n')
w(', '.join(f"`{k.split('/')[-1]}`" for k in seedc) or '—')
w('\nЭто «прямые» потребители seed: именно они дают дополнительные независимые потоки (см. `docs/03`, таблицу бит).\n')
bm = [k for k, v in C.items() if 'getBiome' in v['reads']]
w('### 7.3. Классы, меняющие поведение по биому в точке\n')
w(', '.join(f"`{k.split('/')[-1]}`" for k in bm) or '—')
sm = [k for k, v in C.items() if 'structureManager' in v['reads']]
w('\n### 7.4. Классы, которые смотрят на структуры рядом\n')
w(', '.join(f"`{k.split('/')[-1]}`" for k in sm) or '—')
w('')

# --- 8. версии ---
w('## 8. Что изменилось между версиями (L1)\n')
def edge_set(v):
    return {(e['writer'], e['reader']) for e in L[v]['feature_edges']}
for a, b in [('26.1', '26.2'), ('26.2', '26.3')]:
    ea, eb = edge_set(a), edge_set(b)
    w(f"* **{a} → {b}:** рёбер {len(ea)} → {len(eb)}; новых {len(eb - ea)}, исчезнувших {len(ea - eb)}. Примеры новых: " + ', '.join(f'`{x}`→`{y}`' for x, y in sorted(eb - ea)[:5]) + '.')
w('')

# --- 9. что дальше ---
w('## 9. Ограничения и что не сделано\n')
w('* **L1 — потенциальные связи.** Пересечение по блокам не значит, что в реальной геометрии фичи пересекаются; зато *отсутствие* ребра означает, что данные-связи нет (кроме связей через Java-код, их ловит L2, и через общую геометрию — не ловит никто, кроме L3).')
w('* **L3 (эмпирика) не выполнена.** Нужно сгенерировать настоящие чанки и посчитать статистику (например, «глина у реки ↔ алмазы»). Для этого годится только настоящий сервер Mojang, который не запустится без принятия **Mojang EULA** — решать вам. Альтернатива без EULA — in-process генерация через `oracle/` (требует создания `WorldGenRegion`; не реализована).')
w('* L1 не разбирает NBT-шаблоны структур (1511 файлов) — только их параметры и пулы.')
w('* Java-код вне перечисленных каталогов (например `NoiseChunk`, `Aquifer`, `SurfaceSystem`) в L2 не сканируется — их связи описаны вручную в `docs/00` §10–13.')
open(f'{ROOT}/docs/06-interactions.md', 'w').write('\n'.join(out) + '\n')
print('docs/06-interactions.md', len('\n'.join(out)), 'chars')
