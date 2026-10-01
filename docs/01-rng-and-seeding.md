# 01. RNG и посев: как из `long seed` получаются все генераторы (26.1 / 26.2 / 26.3)

Ссылки: `V:путь:строка`, где путь — от `src/dec/V/net/minecraft/...` (декомпилят Vineflower), либо `V:data:...` — датапак `src/data-V/data/minecraft/...`.
Если файл побайтно одинаков во всех трёх версиях (проверено `diff -q`), ссылка даётся один раз как `26.x:`.

**Верификация.** Все формулы этого документа реализованы в C (`tools/verify/rng_ref.c`) и сверены с РЕАЛЬНЫМ кодом игры
(`tools/verify/RngDump.java`, запуск `tools/verify/run_rngdump.sh <V>`): 1280 строк дампа (10 seed × Legacy/Xoroshiro/WorldgenRandom(оба)/positional/
decoration/largeFeature/slime/RandomSpread/4 frequency-reducer'а) совпадают **бит-в-бит** между C-портом и игрой, и между 26.1, 26.2, 26.3.
Единственное, что не покрыто (и не нужно генерации мира): `nextGaussian` — Java `Math.log` и glibc `log` расходятся в 1 ulp примерно на 0,05 % значений;
`nextGaussian` в `world/level/levelgen/**` не вызывается (только частицы/блоки: ComposterBlock, EndRodBlock, TrialSpawner).

---

## 1. Итог в одну таблицу: какой RNG где и сколько бит seed он несёт

| Место | RNG | Что от `seed` реально используется | Ссылка |
|---|---|---|---|
| `RandomSpreadStructurePlacement` (все «рассеянные» структуры) | `WorldgenRandom(LegacyRandomSource)` → `setLargeFeatureWithSalt` | **младшие 48 бит** | 26.x:levelgen/structure/placement/RandomSpreadStructurePlacement.java:67-76 (26.1/26.2) |
| Frequency reducers (`legacy_type_1..3`, `default`) | `WorldgenRandom(Legacy)` | **48 бит** | §5.3 |
| Stronghold (concentric rings) | `RandomSource.create()`+`setSeed(seed)` = `LegacyRandomSource` | **48 бит** | 26.3:world/level/chunk/ChunkGeneratorStructureState.java:125-126 |
| `Structure.GenerationContext.random` (позиция/форма внутри старт-чанка) | `WorldgenRandom(Legacy)` + `setLargeFeatureSeed(seed,cx,cz)` | 48 бит | 26.3:levelgen/structure/Structure.java:246-249 |
| Карверы (пещеры/каньоны) | `WorldgenRandom(Legacy)` + `setLargeFeatureSeed(seed+index,cx,cz)` | 48 бит | 26.3:levelgen/NoiseBasedChunkGenerator.java:261 |
| Слайм-чанки | `SingleThreadedRandomSource` (тот же LCG) | 48 бит | 26.x:levelgen/WorldgenRandom.java:71-73 |
| Декорация (features, деревья, руды, жеоды, подземные озёра, dungeon…) | `WorldgenRandom(**Xoroshiro**)`, `setDecorationSeed`, `setFeatureSeed` | **все 64 бита** | 26.3:world/level/chunk/ChunkGenerator.java:359-360 |
| Шумы климата Overworld (`overworld`, `amplified`, `large_biomes`) | `XoroshiroRandomSource(seed).forkPositional().fromHashOf(name)` | **все 64 бита** | §4, 26.3:levelgen/RandomState.java:64-66 |
| Nether/End/caves/floating_islands: все именованные шумы | `LegacyRandomSource(seed).forkPositional().fromHashOf(name)` (`legacy_random_source: true`) | **48 бит** | 26.x:data:worldgen/noise_settings/{nether,end,caves,floating_islands}.json (`legacy_random_source: true`) |
| Nether: `temperature`/`vegetation` (биомы) | `LegacyRandomSource(seed)` / `(seed+1)`, `createForLegacyNetherBiome` | 48 бит (seed и seed+1) | 26.3:levelgen/RandomState.java:79-87 |
| End: острова | `LegacyRandomSource(seed)` + `consumeCount(17292)` | 48 бит | 26.3:levelgen/RandomState.java:94-96 (детали — `docs/03`, §End) |
| `GeodeFeature` crack-noise, `EndSpikeFeature`, `DesertPyramid/NetherFossil/Capped` (`.at(pos)`) | `LegacyRandomSource(level.getSeed())` / `createThreadLocalInstance(seed).forkPositional().at(pos)` | 48 бит | 26.3:levelgen/feature/GeodeFeature.java:82; EndSpikeFeature.java:49; structures/DesertPyramidStructure.java:53; NetherFossilPieces.java:99 |
| Обфускация seed для клиента (`BiomeManager`) | SHA-256 (см. `docs/03`) | 64 бит на входе | 26.3:server/level/ServerPlayer.java:2209 |

Сразу следствие: **всё, что идёт через `LegacyRandomSource`, зависит только от `seed mod 2^48`** (проверено запуском: `seed` и `seed ^ (0xABCD<<48)` дают
идентичные позиции структур для 10 000 ячеек и идентичный поток `setLargeFeatureSeed`, но РАЗНЫЙ `setDecorationSeed`). Старшие 16 бит проявляются
только через Xoroshiro-ветки (Overworld-шумы, декорации).

Автогенерируемые seed'ы: `WorldOptions.randomSeed() = RandomSource.create().nextLong()` (26.x:levelgen/WorldOptions.java:88-90) — это `LegacyRandomSource(generateUniqueSeed()).nextLong()`,
т. е. пара `next(32),next(32)` от одного 48-битного состояния ⇒ **случайный seed принадлежит подпространству из ≤ 2^48 значений** (проверено: 2000/2000 сгенерированных seed'ов
удовлетворяют соотношению LCG «верхняя половина → нижняя половина» при подборе 16 неизвестных бит). Текстовый seed: `Long.parseLong` либо `String.hashCode()` (32-битный, знаково расширенный) — WorldOptions.java:75-86.
`RandomSupport.generateUniqueSeed = SEED_UNIQUIFIER(8682522807148012L) *= 1181783497276652981L ^ System.nanoTime()` (RandomSupport.java:14, 40-42) — источник энтропии автосида.

---

## 2. `LegacyRandomSource` — 48-битный LCG (идентичен `java.util.Random`)

Файлы: 26.x:levelgen/LegacyRandomSource.java (идентичен во всех трёх версиях), 26.x:levelgen/BitRandomSource.java, `SingleThreadedRandomSource`/`ThreadSafeLegacyRandomSource` — тот же алгоритм.
Проверено: `LegacyRandomSource(s)` совпадает с `java.util.Random(s)` на 1000 смешанных вызовов (nextInt/nextInt(b)/nextLong/nextFloat/nextDouble/nextBoolean).

Константы: `MULT = 0x5DEECE66D (25214903917)`, `INC = 0xB`, `MASK = 2^48-1` (LegacyRandomSource.java:10-13).

```c
typedef uint64_t u64; typedef int64_t i64; typedef uint32_t u32; typedef int32_t i32;
void   lcg_setSeed(u64 *st, i64 seed) { *st = ((u64)seed ^ 0x5DEECE66Dull) & ((1ull<<48)-1); }   // LegacyRandomSource.java:32-38
i32    lcg_next(u64 *st, int bits)    { *st = (*st * 0x5DEECE66Dull + 0xBull) & ((1ull<<48)-1);   // :41-49
                                        return (i32)(*st >> (48 - bits)); }                       // st>=0 => >> == >>>
```
`BitRandomSource` (BitRandomSource.java):
```c
i32 nextInt()            { return next(32); }                                  // :12-14
i32 nextInt(i32 bound)   {                                                     // :17-33   (bound>0)
    if ((bound & (bound-1)) == 0) return (i32)(((i64)bound * next(31)) >> 31); // степень двойки: берём СТАРШИЕ биты
    i32 s, m; do { s = next(31); m = s % bound; } while ((i32)((u32)s - (u32)m + (u32)(bound-1)) < 0); // int-переполнение = «отклонение»
    return m; }
i64 nextLong()           { i32 hi = next(32), lo = next(32);                    // :37-42   ВНИМАНИЕ: lo знаково расширяется
                           return (i64)(((u64)(i64)hi << 32) + (u64)(i64)lo); }
bool nextBoolean()       { return next(1) != 0; }                               // :45-47
float nextFloat()        { return next(24) * 5.9604645E-8F; }                  // :50-52   (=2^-24, точно)
double nextDouble()      { i32 a = next(26), b = next(27);                     // :55-60
                           return (double)(((i64)a << 27) + b) * 1.1102230246251565E-16; }  // (double)1.110223E-16F == 2^-53 ровно
int nextInt(int origin, int bound)  = origin + nextInt(bound-origin)           // 26.x:util/RandomSource.java:73-79
void consumeCount(int n) { for n times: nextInt(); }                           // RandomSource.java:67-71  => n шагов LCG
```
`consumeCount(n)` = n шагов LCG ⇒ прыжок за O(log n) (проверено: `consumeCount(17292)` = `skipNextN` из cubiomes):
```c
void skipNextN(u64 *st, u64 n){ u64 m=1,a=0,im=0x5DEECE66Dull,ia=0xB; for(u64 k=n;k;k>>=1){ if(k&1){ m*=im; a=im*a+ia; } ia=(im+1)*ia; im*=im; }
                                *st = (*st*m + a) & ((1ull<<48)-1); }
```
Всё, что берёт `setSeed(x)`, зависит от `x mod 2^48` (маска на :33); XOR-константа `0x5DEECE66D` обратима, значит `setSeed` — биекция на 48 битах.

`fork()` = `new LegacyRandomSource(nextLong())`; `forkPositional()` = `LegacyPositionalRandomFactory(nextLong())` (LegacyRandomSource.java:22-29). Фабрика (:56-79):
```c
RandomSource at(x,y,z)      = LegacyRandomSource( Mth.getSeed(x,y,z) ^ factorySeed );              // :64-68
RandomSource fromHashOf(s)  = LegacyRandomSource( (i64)(i32)java_String_hashCode(s) ^ factorySeed ); // :71-74   hashCode — 32-бит, знаково расширен
RandomSource fromSeed(v)    = LegacyRandomSource( v );                                              // :77-79 (factorySeed игнорируется!)
```
`java_String_hashCode`: `h = 31*h + c` по UTF-16 символам, `int`. Для Identifier это строка `"minecraft:name"` (26.x:levelgen/PositionalRandomFactory.java:398-400 → `name.toString()`).

---

## 3. `XoroshiroRandomSource` — Xoroshiro128++ и апгрейд seed 64→128

Файлы: 26.x:levelgen/{XoroshiroRandomSource,Xoroshiro128PlusPlus,RandomSupport}.java (идентичны во всех версиях).

```c
u64 rotl(u64 x,int k){ return (x<<k)|(x>>(64-k)); }
u64 mixStafford13(u64 z){ z=(z^(z>>30))*0xBF58476D1CE4E5B9ull; z=(z^(z>>27))*0x94D049BB133111EBull; return z^(z>>31); }  // RandomSupport.java:17-21
// константы: SILVER_RATIO_64 = 0x6A09E667F3BCC909 (RandomSupport.java:12), GOLDEN_RATIO_64 = 0x9E3779B97F4A7C15 (:11)
Seed128 upgradeSeedTo128bitUnmixed(i64 s){ u64 lo = (u64)s ^ 0x6A09E667F3BCC909ull; u64 hi = lo + 0x9E3779B97F4A7C15ull; return {lo,hi}; }   // :23-27
Seed128 upgradeSeedTo128bit(i64 s)       { u = unmixed(s); return { mixStafford13(u.lo), mixStafford13(u.hi) }; }                    // :29-31, :53-55
void xr_init(Xr*r,u64 lo,u64 hi){ if((lo|hi)==0){ lo=0x9E3779B97F4A7C15ull; hi=0x6A09E667F3BCC909ull; } r->lo=lo; r->hi=hi; }    // Xoroshiro128PlusPlus.java:17-24
u64 xr_nextLong(Xr*r){ u64 s0=r->lo, s1=r->hi; u64 res = rotl(s0+s1,17)+s0; s1^=s0;                                                 // Xoroshiro128PlusPlus.java:26-34
                       r->lo = rotl(s0,49) ^ s1 ^ (s1<<21); r->hi = rotl(s1,28); return res; }
```
`new XoroshiroRandomSource(long seed)` и `setSeed(seed)` = `xr_init(upgradeSeedTo128bit(seed))` (XoroshiroRandomSource.java:16-18, 43-46).
`new XoroshiroRandomSource(lo,hi)` — без микширования (:24-26).

Методы (XoroshiroRandomSource.java) — **НЕ те же, что у `BitRandomSource`**:
```c
i32   nextInt()        = (i32)nextLong();                                  // :49-51  МЛАДШИЕ 32 бита
i64   nextLong()       = xr_nextLong();                                    // :77
bool  nextBoolean()    = (nextLong() & 1) != 0;                            // :82-84  младший бит
float nextFloat()      = (float)(nextLong() >> 40) * 5.9604645E-8F;        // :87-89  nextBits(24)=nextLong()>>>(64-24)
double nextDouble()    = (double)(nextLong() >> 11) * 1.1102230246251565E-16; // :92-94 nextBits(53); ОДИН вызов, не два
i32 nextInt(i32 bound) {                                                   // :54-75   метод Lemire без деления
    u64 r = (u32)nextInt(); u64 m = r*(u64)bound; u64 f = m & 0xFFFFFFFF;
    if (f < (u64)bound) { u32 thr = (u32)(-(u32)bound) % (u32)bound;       //   remainderUnsigned(~bound+1, bound)
                          while (f < thr) { r=(u32)nextInt(); m=r*(u64)bound; f=m&0xFFFFFFFF; } }
    return (i32)(m >> 32); }                                                //   СТАРШИЕ 32 бита произведения — одинаково для pow2 и не pow2
void  consumeCount(n)  { n раз xr_nextLong(); }                            // :102-106
```
`fork()` = `Xoroshiro(nextLong(), nextLong())` (без микса); `forkPositional()` = `XoroshiroPositionalRandomFactory(nextLong(), nextLong())` — `lo` берётся ПЕРВЫМ (:33-40).

### PositionalRandomFactory (Xoroshiro) — XoroshiroRandomSource.java:112-137
```c
RandomSource at(x,y,z)     = Xoroshiro( lo = Mth.getSeed(x,y,z) ^ F.lo,  hi = F.hi );      // :122-126  — микса нет!
RandomSource fromHashOf(n) = { md = MD5(utf8(n)); h.lo = BE64(md[0..7]); h.hi = BE64(md[8..15]);   // RandomSupport.seedFromHashOf :33-38
                               return Xoroshiro( h.lo ^ F.lo, h.hi ^ F.hi ); }                      // :129-132 — тоже без Stafford-микса
RandomSource fromSeed(v)   = Xoroshiro( v ^ F.lo, v ^ F.hi );                                     // :135-137
```
`BE64` — big-endian чтение (Guava `Longs.fromBytes(b0..b7)`), MD5 — от UTF-8 байтов строки `"namespace:path"` (для `Identifier` — `toString`).
`RandomState.getOrCreateRandomFactory(name) = random.fromHashOf(name).forkPositional()` (26.3:levelgen/RandomState.java:124-126) — т.е. ещё два `nextLong` от производного источника.

### `Mth.getSeed(x,y,z)` — 26.1:util/Mth.java:314-318; 26.2/26.3:util/Mth.java:332-336
```c
i64 mth_getSeed(i32 x,i32 y,i32 z){
    u64 a = (u64)(i64)(i32)((u32)x * 3129871u);   // x*3129871 — ЦЕЛОЧИСЛЕННОЕ умножение int*int (переполнение!), затем знаковое расширение
    u64 b = (u64)(i64)z * 116129781ull;            // z*116129781L — long
    u64 s = a ^ b ^ (u64)(i64)y;
    s = s*s*42317861ull + s*11ull;
    return (i64)s >> 16; }                          // арифметический сдвиг
```

---

## 4. Откуда берутся Xoroshiro/Legacy на верхнем уровне (`RandomState`)

26.3:levelgen/RandomState.java:56-100 (26.1/26.2 — эквивалент в конструкторе, файл 26.2:levelgen/RandomState.java, идентичен 26.1):
```
alg    = noise_settings.legacy_random_source ? LEGACY : XOROSHIRO         // 26.3:RandomState.java:64
this.random = alg.newInstance(seed).forkPositional()                       // :66  (Xoroshiro: 2×nextLong от XoroshiroRandomSource(seed), т.е. upgrade+nextLong; Legacy: nextLong от LCG(seed))
noise(name)         = NormalNoise.create( random.fromHashOf(name /*"minecraft:temperature"*/) , params)     // Noises.java:83 (26.3:  context.fromHashOf(name.identifier()))
random_factory(nm)  = random.fromHashOf(nm).forkPositional()               // getOrCreateRandomFactory :124-126 ('aquifer','ore','clay_bands'…)
BlendedNoise (terrain): useLegacy ? Legacy(seed+0) : random.fromHashOf("minecraft:terrain")   // 26.3:RandomState.java:91 ; 26.2: wrapNew (RandomState.java, ветка BlendedNoise)
Nether temperature/vegetation: NormalNoise.createLegacyNetherBiome(Legacy(seed+0 / seed+1))     // 26.3:RandomState.java:79-87
End islands: LegacyRandomSource(seed)                                      // 26.3:RandomState.java:94-96 ; 26.2: new EndIslandDensityFunction(seed)
```
Подробности (какие именно шумы/параметры/имена) — `docs/03-seed-dependency-map.md`. Здесь важно: для Overworld фабрика имеет 128 бит состояния (lo,hi — функции ВСЕХ 64 бит seed
через Stafford), шумы различаются XOR'ом MD5(имени); для Nether/End фабрика — 64-битное число `nextLong()` от 48-битного LCG, а шумы различаются
XOR'ом `String.hashCode()` (32 бита!) — то есть в Nether/End пространство состояний всех шумов ≤ 2^48 и, вообще говоря, коллизионно по именам.

---

## 5. `WorldgenRandom` (26.x:levelgen/WorldgenRandom.java, идентичен во всех версиях)

`WorldgenRandom extends LegacyRandomSource` и оборачивает произвольный `RandomSource delegate` (:6-13).

### 5.1. Как ведёт себя обёртка
```c
i32 next(int bits) { count++;                                                       // :30-35
    return delegate is LegacyRandomSource ? delegate.next(bits)                     // Legacy: обычный LCG
                                          : (i32)(delegate.nextLong() >>> (64-bits)); }  // Xoroshiro: СТАРШИЕ биты каждого nextLong
void setSeed(v) { if (delegate) delegate.setSeed(v); }                              // :38-42 (delegate сам маскирует/апгрейдит)
```
Все `nextInt/nextInt(bound)/nextLong/nextFloat/nextDouble/nextBoolean` берутся из `BitRandomSource` (§2), т.е. **WorldgenRandom(Xoroshiro) ≠ XoroshiroRandomSource**:
* `nextInt()` = `(i32)(xr.nextLong() >>> 32)` (старшая половина), `nextInt(b)` = `next(31)`=`xr>>>33` + BitRandom-алгоритм (pow2: `b*next(31)>>31`, иначе `%` с отклонением),
* `nextLong()` = **два** вызова `xr.nextLong()` (`hi=xr>>>32`, `lo=xr>>>32` со знаковым расширением и сложением, не OR),
* `nextDouble()` = **два** вызова (`next(26)`=`xr>>>38`, `next(27)`=`xr>>>37`), `nextFloat()` = `xr>>>40`, `nextBoolean()` = `xr>>>63`, `consumeCount(n)` = n вызовов `xr.nextLong()`.
* Нюанс: `WorldgenRandom.setSeed` не вызывает `LegacyRandomSource.setSeed`, поэтому закэшированное второе значение Marsaglia-гауссианы обёртки не сбрасывается (для генерации мира неважно).
* `fork()`/`forkPositional()` делегируются (:20-27).

Что лежит внутри в каждом месте (сверено grep'ом по трём версиям):
| Конструкция | Где (26.3) | В 26.1 / 26.2 |
|---|---|---|
| `new WorldgenRandom(new XoroshiroRandomSource(generateUniqueSeed()))` — только декорация | chunk/ChunkGenerator.java:359 | 26.1:chunk/ChunkGenerator.java:322, 26.2:chunk/ChunkGenerator.java:326 |
| `new WorldgenRandom(new LegacyRandomSource(0L))` (структуры: `Structure.makeRandom`, placement, reducers, `ChunkGenerator.getNearestGeneratedStructure`-ветка) | Structure.java:246; RandomSpread…java:70; Abstract…java:102,108,114,122; ChunkGenerator.java:543 | 26.1:Structure.java:248; StructurePlacement.java:105-125; ChunkGenerator.java:507; 26.2: то же (Structure.java:248; StructurePlacement.java:105-125; ChunkGenerator.java:511) |
| `new WorldgenRandom(new LegacyRandomSource(generateUniqueSeed()))` (карверы, спавн мобов при генерации, OceanMonument) | NoiseBasedChunkGenerator.java:237, 478; OceanMonumentStructure.java:59 | 26.1: NoiseBasedChunkGenerator.java:311, 492; 26.2: :316, 497 |
Начальный seed `generateUniqueSeed()` всегда сразу перезаписывается `setSeed(...)`, на результат не влияет.

### 5.2. Сидеры (WorldgenRandom.java)
```c
// :44-51  setDecorationSeed(seed, chunkX, chunkZ)   — В ChunkGenerator ВЫЗЫВАЕТСЯ С БЛОКОВЫМИ КООРДИНАТАМИ юго-западного угла чанка (origin.getX()=16*cx, getZ()=16*cz), см. ChunkGenerator.java:360 и NoiseBasedChunkGenerator.java:479
i64 setDecorationSeed(i64 seed, i32 cx, i32 cz){ setSeed(seed);
    u64 xs = (u64)nextLong() | 1;  u64 zs = (u64)nextLong() | 1;
    u64 r  = ((u64)(i64)cx * xs + (u64)(i64)cz * zs) ^ (u64)seed;     // (cx*xs + cz*zs) ^ seed : int*long → long, приоритет: + выше ^
    setSeed(r); return r; }                                              // возвращённое значение = «populationSeed»/decorationSeed
// :53-56  setFeatureSeed(seed=decorationSeed, index, step)
void setFeatureSeed(i64 seed, i32 index, i32 step){ setSeed(seed + index + 10000*step); }
// :58-64  setLargeFeatureSeed(seed, cx, cz)  — chunk-координаты
void setLargeFeatureSeed(i64 seed, i32 cx, i32 cz){ setSeed(seed); u64 xs=(u64)nextLong(), zs=(u64)nextLong();   // БЕЗ |1
    setSeed( ((u64)(i64)cx*xs) ^ ((u64)(i64)cz*zs) ^ (u64)seed ); }
// :66-69  setLargeFeatureWithSalt(seed, x, z, blend)
void setLargeFeatureWithSalt(i64 seed, i32 x, i32 z, i32 blend){ setSeed( (i64)x*341873128712LL + (i64)z*132897987541LL + seed + blend ); }   // 341873128712=0x4F9939F508, 132897987541=0x1EF1565BD5
// :71-73  seedSlimeChunk(x, z, seed, salt) -> SingleThreadedRandomSource (тот же LCG); в игре salt=987234911L (26.3:world/entity/monster/cubemob/Slime.java:93)
RandomSource seedSlimeChunk(i32 x,i32 z,i64 seed,i64 salt){
    i32 xx = x*x*4987142;  i32 x1 = x*5947611;                 // ЦЕЛОЧИСЛЕННО (int), с переполнением
    i64 zz = (i64)(z*z) * 4392871LL;  i32 z1 = z*389711;        // (z*z) — int, затем *long; z*389711 — int
    return LCG( ((i64)seed + xx + x1 + zz + z1) ^ salt ); }     // слева направо, потом ^;  slime = (nextInt(10) == 0)
```
`setBaseChunkSeed`, `setBaseStoneSeed` (были в 1.16-) в 26.x **отсутствуют** (grep по всему декомпилату — 0 вхождений).

Использование сидеров (26.3, ссылки): `setDecorationSeed` — ChunkGenerator.java:360 (декорация, `Xoroshiro`) и NoiseBasedChunkGenerator.java:479 (`spawnOriginalMobs`, Legacy);
`setFeatureSeed` — ChunkGenerator.java:381 (для каждой структуры шага, `index` = порядковый номер структуры шага), :423 (для каждой фичи: `globalIndexOfFeature` по `FeatureSorter`);
`setLargeFeatureSeed` — Structure.java:247, ChunkGenerator.java:544, NoiseBasedChunkGenerator.java:261 (`seed + index` для карверов, `index` — порядковый номер карвера в биоме исходного чанка), OceanMonumentStructure.java:60, StrongholdStructure.java:29 (`context.seed() + tries++`);
`setLargeFeatureWithSalt` — RandomSpreadStructurePlacement.java:71, Abstract…java:103,115.

### 5.3. Зависимость от бит seed (важно для восстановления)
* `setLargeFeatureSeed(seed,cx,cz)` на Legacy: `setSeed(seed)` маскирует до 48 бит ⇒ `xs,zs` зависят от `seed mod 2^48`; итог `cx*xs ^ cz*zs ^ seed` затем снова маскируется `& (2^48-1)` при `setSeed` ⇒ зависит только от `seed mod 2^48`. (Проверено запуском.)
* `setLargeFeatureWithSalt` на Legacy: линейно по `seed` ⇒ только `seed mod 2^48`, причём поток позиции структуры линейно связан с seed: `S0 = (x*341873128712 + z*132897987541 + seed + salt)` — при известном (x,z,salt) первое состояние LCG = `(S0 ^ 0x5DEECE66D) mod 2^48` – основа классического кракинга.
* `setDecorationSeed` на **Xoroshiro**: `setSeed(seed)` использует ВСЕ 64 бита (upgrade+Stafford), `xs|1`,`zs|1` — 64-битные нечётные множители, итог `cx*xs+cz*zs ^ seed` — 64 бит, снова апгрейдится Stafford'ом. Для чанка (0,0) `decorationSeed = (0*xs+0*zs) ^ seed = seed` (проверено на всех 10 seed'ах дампа `WX.decoration 0,0`): декорация чанка (0,0) стартует ровно с `setSeed(seed)`, а `setFeatureSeed(seed, index, step)` = `Xoroshiro.setSeed(seed + index + 10000*step)`.
  Следствие: на декорации младшие 48 бит НЕ «изолированы» от старших; но знание 48 бит (из структур) оставляет 2^16 кандидатов для старших бит, каждый из которых даёт совершенно иной поток декорации — это стандартный шаг «48 → 64».
* `setFeatureSeed`: `decorationSeed + index + 10000*step` → `Xoroshiro.setSeed` (Stafford) — соседние index/step дают несвязанные потоки. `index` — глобальный индекс фичи в шаге по `FeatureSorter` (зависит от набора и порядка фич во всех биомах измерения!) — см. `docs/03`, §декорации.
* Frequency reducers (`AbstractSpreadingStructurePlacement.FrequencyReductionMethod`), 26.3:...placement/AbstractSpreadingStructurePlacement.java:101-126 (в 26.1/26.2 то же в `StructurePlacement.java:104-129`, `StructurePlacement.FrequencyReductionMethod`):
  ```c
  // default (probabilityReducer, :101-105). ВНИМАНИЕ на порядок аргументов: setLargeFeatureWithSalt(seed, /*x=*/salt, /*z=*/sourceX, /*blend=*/sourceZ)
  bool default_(seed,salt,sx,sz,p){ WG(Legacy); setLargeFeatureWithSalt(seed, salt, sx, sz); return nextFloat() < p; }
  //   => состояние = salt*341873128712 + sx*132897987541 + seed + sz
  // legacy_type_1 (legacyPillagerOutpostReducer, :119-126): sx,sz — ЧАНКОВЫЕ координаты (`>>4` здесь применяется к чанковым → «блочные/16» баг оригинала)
  bool legacy1(seed,salt,sx,sz,p){ i32 cx=sx>>4, cz=sz>>4; WG(Legacy).setSeed( (i64)(cx ^ (cz<<4)) ^ seed ); nextInt(); return nextInt((i32)(1.0F/p)) == 0; }
  // legacy_type_2 (:113-117):  setLargeFeatureWithSalt(seed, sx, sz, 10387320); return nextFloat() < p;
  // legacy_type_3 (:107-111):  setLargeFeatureSeed(seed, sx, sz);               return nextDouble() < (double)p;   // p float→double
  ```
  Применяется только если `frequency < 1.0F` (Abstract…java:91-93). Вычисления `cx ^ cz << 4 ^ seed` — приоритет `<<` > `^`, `cx`,`cz` — int, XOR с long знаково расширяет.

### 5.4. Что в 26.x изменилось относительно «классики»
Формулы RNG (`LegacyRandomSource`, `Xoroshiro*`, `RandomSupport`, `WorldgenRandom`, `BitRandomSource`, `MarsagliaPolarGaussian`, `Positional…`) совпадают с эталоном cubiomes (`refs/cubiomes/rng.h`: `setSeed`, `next`, `nextInt`, `nextLong`, `nextFloat`, `nextDouble`, `skipNextN`, `xSetSeed`) и **байт-в-байт одинаковы в 26.1/26.2/26.3** (`diff -q`; единственные отличающиеся соседи: `RandomState` (26.3, см. `docs/04`), `WorldOptions` (26.1→26.2, только Codec), `Mth` — 26.3 перевёл `smoothstep(double)`→`smoothstep(float)`, к RNG не относится).
Архитектурные особенности 26.x (сравнение с 1.18–1.21 по исходникам невозможно — их в проекте нет; сверка велась с cubiomes): (1) в `PositionalRandomFactory` есть `fromSeed(long)` (Legacy: игнорирует seed фабрики, Xoroshiro: `v^lo, v^hi`); (2) `nextGaussian` вынесен в `MarsagliaPolarGaussian` (использует `Math.log/sqrt`); (3) в 26.3 `StructurePlacement` — интерфейс, общая логика (`salt`, `frequency`, `exclusion_zone`, reducers) в `AbstractSpreadingStructurePlacement`, добавлен тип `dimension_origin` (см. `docs/02`) — семантика `random_spread`/`concentric_rings` не изменилась (diff 26.2→26.3 — только смена базового класса и `codec()`); (4) `RandomState` в 26.3 перестроен под `DensityFunctionCompiler` (`createRandom`, `createNoiseSampler`, `createEndIslandRandom`) — посев именованных шумов остаётся прежним по формуле (`fromHashOf(Identifier)`).

---

## 6. Примечания по портированию
* Все `int`-умножения в `Mth.getSeed`, `seedSlimeChunk` (x*x*4987142 …) и в `cx ^ cz << 4` — 32-битные с переполнением; в C используйте `uint32_t`, затем знаковое расширение `(int64_t)(int32_t)`.
* `nextLong` у `BitRandomSource` — **сложение** со знаковым расширением нижней половины (а не OR): при `lo<0` верхняя половина эффективно уменьшается на 1.
* `LegacyRandomSource.nextInt(bound)` для pow2 берёт старшие 31 бит (`bound*next(31)>>31`); поэтому `nextInt(16)` и `nextInt(10)` дают разные «зависимости» от битов состояния.
* `Xoroshiro.nextInt(bound)` использует младшие 32 бита `nextLong` и старшие 32 бита произведения, `WorldgenRandom(Xoroshiro).nextInt(bound)` — старшие 31 бит `nextLong` и «Legacy»-алгоритм: **не смешивайте эти два**.
* `Math.round/cos/sin` в кольцах strongholds (ChunkGeneratorStructureState.java:132-134) — `double`; `Math.sin/cos` в HotSpot — интринсики, в редких точках могут отличаться от `StrictMath` на 1 ulp; на итоговое округление `Math.round` влияет лишь при попадании на полуцелое значение (не выяснено численно; для проверки: сравнить 1e6 значений с `StrictMath`).
* Генерация случайных seed'ов клиента (создание мира) в этом декомпилате не представлена (нет клиентского кода); серверная часть: `DedicatedServerProperties.java:135-137` → `WorldOptions.parseSeed(...).orElse(WorldOptions.randomSeed())`.
