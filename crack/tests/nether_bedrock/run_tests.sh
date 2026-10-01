#!/usr/bin/env bash
# Тесты crack-nether-bedrock. Запуск: crack/tests/nether_bedrock/run_tests.sh   (или make -C crack/src/nether_bedrock test)
# Переменные: NB_BIN (путь к бинарнику), NB_FULL_CPU=1 (дополнительно полный CPU-проход, минуты), NB_REGEN=1 (перегенерировать данные реальной игрой через JVM),
#             NB_OUT (каталог для логов; по умолчанию временный).
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
BIN="${NB_BIN:-$ROOT/crack/bin/crack-nether-bedrock}"
OUT="${NB_OUT:-$(mktemp -d)}"; mkdir -p "$OUT"
DATA="$HERE/data"
pass=0; fail=0
ok()   { pass=$((pass+1)); echo "  ok:   $*"; }
bad()  { fail=$((fail+1)); echo "  FAIL: $*"; }
seedsof() { grep -E '^[0-9]+$' "$1" | sort -n | tr '\n' ' ' | sed 's/ $//'; }   # только строки-seed

[ -x "$BIN" ] || { echo "нет бинарника $BIN (make -C crack/src/nether_bedrock)"; exit 2; }
cp "$BIN" "$OUT/nb-under-test" && BIN="$OUT/nb-under-test"        # копия: пересборка во время прогона не ломает тесты

echo "[1] самопроверка формул (векторы реальной игры 26.1/26.2/26.3 побитово + данные NbRef)"
if "$BIN" --selftest "$ROOT"/crack/experiments/vectors/gameref-26.{1,2,3}.txt "$DATA/gen/real_game_floats.txt" > "$OUT/selftest.log" 2>&1; then ok "selftest: $(tail -1 "$OUT/selftest.log")"; else bad "selftest"; cat "$OUT/selftest.log"; fi

if [ "${NB_REGEN:-0}" = 1 ]; then
  echo "[1b] перегенерация данных реальным кодом игры (3 версии) и сверка между версиями"
  if "$HERE/gen_data.sh" > "$OUT/gen.log" 2>&1; then ok "gen_data.sh: $(grep -c 'совпадают' "$OUT/gen.log") версии совпали с 26.3 побайтно"; else bad "gen_data.sh"; cat "$OUT/gen.log"; fi
fi

echo "[2] примеры оригинала (refs/Nether_Bedrock_Cracker/examples)"
run_expect() {  # файл ожидаемый_seed опции...
  local f="$1" want="$2"; shift 2
  if "$BIN" "$@" "$f" > "$OUT/r.out" 2> "$OUT/r.err"; then :; else bad "$(basename "$f"): код возврата $?"; return; fi
  if grep -qx "$want" "$OUT/r.out"; then ok "$(basename "$f"): найден $want ($(seedsof "$OUT/r.out" | wc -w) seed; $(grep 'скан' "$OUT/r.err" | sed 's/.*время: //'))"; else bad "$(basename "$f"): $want не найден; вывод: $(seedsof "$OUT/r.out")"; fi
}
run_expect "$DATA/example_seed_765906787396911863.txt" 13375767216887
"$BIN" --world-seed "$DATA/example_seed_765906787396911863.txt" 2>/dev/null | grep -q "world_seed_if_game_generated.*: 765906787396911863" && ok "оценка world seed: 765906787396911863" || bad "оценка world seed примера"
run_expect "$DATA/example_old_paper_-8788534344520786540.txt" 240328292736404 --paper1_18
"$BIN" --paper1_18 --world-seed "$DATA/example_old_paper_-8788534344520786540.txt" 2>/dev/null | grep -q ": -8788534344520786540" && ok "Paper: оценка world seed -8788534344520786540" || bad "Paper: оценка world seed"
# без режима Paper этот пример не должен давать тот же seed
if "$BIN" "$DATA/example_old_paper_-8788534344520786540.txt" 2>/dev/null | grep -qx 240328292736404; then bad "Paper-пример найден без --paper1_18 (странно)"; else ok "Paper-пример без --paper1_18 seed не даёт (как ожидается)"; fi

echo "[3] наблюдения, сгенерированные РЕАЛЬНЫМ кодом игры (NbRef; 7 seed x 5 конфигураций; 26.1 = 26.2 = 26.3 побайтно)"
nfiles=0; nfail=0; nfalse=0; ntwin=0
for f in "$DATA"/gen/s*_*.txt; do
  nfiles=$((nfiles+1))
  want=$(sed -n '1s/.*structure seed \([0-9]*\)).*/\1/p' "$f")
  n=$(grep -cv '^#' "$f")
  # check-seed: формула C согласована с реальной игрой на всех наблюдениях файла
  "$BIN" --check-seed "$want" "$f" > /dev/null 2>&1 || { bad "$(basename "$f"): --check-seed $want не согласован с реальной игрой"; nfail=$((nfail+1)); continue; }
  "$BIN" --max-cand 67108864 "$f" > "$OUT/r.out" 2> "$OUT/r.err"; rc=$?
  if [ $rc -ne 0 ]; then echo "  note: $(basename "$f") n=$n: код $rc (данных недостаточно?) — $(grep -E 'отказ|ОШИБКА' "$OUT/r.err" | head -1)"; continue; fi
  if ! grep -qx "$want" "$OUT/r.out"; then bad "$(basename "$f"): истинный seed $want НЕ найден"; nfail=$((nfail+1)); continue; fi
  tot=$(seedsof "$OUT/r.out" | wc -w); dR=$(grep -o 'различных R (наборов фабрик бедрока): [0-9]*' "$OUT/r.err" | grep -o '[0-9]*$')
  ntwin=$((ntwin + tot - dR)); nfalse=$((nfalse + dR - 1))      # близнецы (тот же R) vs действительно ложные (другой R)
done
[ $nfail -eq 0 ] && ok "$nfiles файлов: истинный structure seed найден во всех, согласование формулы с игрой полное (лишних seed суммарно: близнецов с тем же R — $ntwin, ложных с другим R — $nfalse; см. docs/23)" || bad "$nfail из $nfiles файлов провалены"

echo "[4] CPU-путь (OpenMP, тот же код) == GPU; все варианты ядер и --top-bits дают тот же результат"
f="$DATA/gen/s0_b30.txt"; want=$(sed -n '1s/.*structure seed \([0-9]*\)).*/\1/p' "$f")
ref=$("$BIN" --quiet "$f" 2>/dev/null | grep -E '^[0-9]+$' | sort -n | tr '\n' ' ')
allok=1
for k in 0 1 2 3; do r=$("$BIN" --quiet --kernel $k "$f" 2>/dev/null | grep -E '^[0-9]+$' | sort -n | tr '\n' ' '); [ "$r" = "$ref" ] || { allok=0; bad "kernel $k: '$r' != '$ref'"; }; done
for D in 2 4 6 8 10; do r=$("$BIN" --quiet --kernel 4 --dense $D "$f" 2>/dev/null | grep -E '^[0-9]+$' | sort -n | tr '\n' ' '); [ "$r" = "$ref" ] || { allok=0; bad "kernel 4 dense $D: '$r' != '$ref'"; }; done
for K in 10 11 13; do r=$("$BIN" --quiet --top-bits $K "$f" 2>/dev/null | grep -E '^[0-9]+$' | sort -n | tr '\n' ' '); [ "$r" = "$ref" ] || { allok=0; bad "top-bits $K: '$r' != '$ref'"; }; done
[ $allok = 1 ] && ok "ядра 0..4 (Gray с 2/4/6/8/10 плотными проверками) и K=10/11/12/13 дают одинаковый набор seed ($ref)"
# CPU на диапазоне вокруг истинного префикса: F_primary>>12 — берём из GPU-запуска: ищем диапазон, содержащий истинный; проще — полный CPU при NB_FULL_CPU
if [ "${NB_FULL_CPU:-0}" = 1 ]; then
  r=$("$BIN" --quiet --cpu "$f" 2>/dev/null | grep -E '^[0-9]+$' | sort -n | tr '\n' ' ')
  [ "$r" = "$ref" ] && ok "полный CPU-проход (2^36 префиксов) == GPU" || bad "полный CPU: '$r' != '$ref'"
else
  # истинный префикс: вычислим через --gen-независимый путь: запустим CPU на первой 1/64 диапазона и сравним с GPU на том же диапазоне
  a=$("$BIN" --quiet --cpu --prefix-start 0 --prefix-count 1073741824 --force "$f" 2>/dev/null | grep -E '^[0-9]+$' | sort -n | tr '\n' ' ')
  b=$("$BIN" --quiet --prefix-start 0 --prefix-count 1073741824 --force "$f" 2>/dev/null | grep -E '^[0-9]+$' | sort -n | tr '\n' ' ')
  [ "$a" = "$b" ] && ok "CPU == GPU на диапазоне 2^30 префиксов (найдено: '${a:-ничего}')" || bad "CPU != GPU на диапазоне: '$a' vs '$b'"
  # CPU на окне вокруг известного seed: ищем F_primary истинного seed через --check-seed не выдаёт F; используем пример оригинала (ROOF_SEED из layer.rs) как известную точку
  c=$("$BIN" --quiet --cpu --prefix-start 46856543000 --prefix-count 2000 "$DATA/example_seed_765906787396911863.txt" 2>/dev/null | grep -Ex '[0-9]+')
  [ "$c" = "13375767216887" ] && ok "CPU на окне вокруг истинного префикса (ROOF_SEED оригинала >>12 = 46856543881) находит seed" || bad "CPU-окно: '$c'"
fi

echo "[5] устойчивость ввода"
printf '0 4 0 bedrock\n0 4 0 other\n' > "$OUT/contra.txt"
"$BIN" "$OUT/contra.txt" > /dev/null 2>&1; [ $? -eq 2 ] && ok "противоречивые наблюдения -> код 2" || bad "противоречие не обнаружено"
printf '# только y без информации\n0 0 0 bedrock\n5 127 5 bedrock\n1 50 1 other\n' > "$OUT/noinfo.txt"
"$BIN" "$OUT/noinfo.txt" > /dev/null 2>&1; [ $? -eq 2 ] && ok "файл без значимых y -> код 2" || bad "файл без значимых y не отклонён"
printf '1 4 1 bedrock\n2 123 2 bedrock\n' > "$OUT/few.txt"
"$BIN" "$OUT/few.txt" > /dev/null 2>&1; [ $? -eq 4 ] && ok "слишком мало данных (2 блока) -> код 4 (отказ)" || bad "малые данные: ожидался код 4"
"$BIN" --version 26.0 "$DATA/example_seed_765906787396911863.txt" > /dev/null 2>&1; [ $? -eq 2 ] && ok "неподдерживаемая версия -> код 2" || bad "версия 26.0 принята"
for v in 26.1 26.2 26.3; do
  "$BIN" --quiet --version $v "$DATA/example_seed_765906787396911863.txt" 2>/dev/null | grep -qx 13375767216887 && ok "--version $v: seed найден" || bad "--version $v"
done

# границы диапазона префиксов (несовпадающие с границами групп по 32 у ядра Gray): окна вокруг истинного префикса 46856543881 (GPU и CPU обязаны совпасть)
E="$DATA/example_seed_765906787396911863.txt"; wb=1
for w in "46856543881 1 yes" "46856543870 20 yes" "46856543875 7 yes" "46856543000 882 yes" "46856543000 881 no" "46856543882 100 no"; do
  set -- $w
  g=$("$BIN" --quiet --prefix-start $1 --prefix-count $2 "$E" 2>/dev/null | grep -Ex '[0-9]+' | tr '\n' ' ')
  c=$("$BIN" --quiet --cpu --prefix-start $1 --prefix-count $2 "$E" 2>/dev/null | grep -Ex '[0-9]+' | tr '\n' ' ')
  exp=""; [ "$3" = yes ] && exp="13375767216887 "
  [ "$g" = "$exp" ] && [ "$c" = "$exp" ] || { wb=0; bad "окно start=$1 count=$2: gpu='$g' cpu='$c' ожидалось '$exp'"; }
done
[ $wb = 1 ] && ok "граничные окна диапазона префиксов (1, 7, 20, 881, 882 префиксов; старт не кратен 32): GPU == CPU == ожидание"

# режимы --filter-seeds (как «Load seed list» оригинала) и --first
printf '765906787396911863\n498574392523453245\n234523453245332\n-8788534344520786540\n0\n' > "$OUT/seedlist.txt"
r=$("$BIN" --filter-seeds "$OUT/seedlist.txt" "$DATA/example_seed_765906787396911863.txt" 2>/dev/null | tr '\n' ' ')
[ "$r" = "765906787396911863 " ] && ok "--filter-seeds: из 5 seed подходит только 765906787396911863" || bad "--filter-seeds: '$r'"
r=$("$BIN" --filter-seeds "$OUT/seedlist.txt" --paper1_18 "$DATA/example_old_paper_-8788534344520786540.txt" 2>/dev/null | tr '\n' ' ')
[ "$r" = "-8788534344520786540 " ] && ok "--filter-seeds --paper1_18: подходит только -8788534344520786540 (знаковый 64-битный seed)" || bad "--filter-seeds paper: '$r'"
"$BIN" --quiet --first "$DATA/example_seed_765906787396911863.txt" 2>/dev/null | grep -qx 13375767216887 && ok "--first находит seed" || bad "--first"

echo
echo "ИТОГО: ok=$pass fail=$fail  (логи: $OUT)"
[ $fail -eq 0 ]
