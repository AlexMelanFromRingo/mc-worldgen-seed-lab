# Сборка crack-struct (CUDA, fallback CPU/OpenMP) и crack-lift64 (CPU, OpenMP + engine/).
#   make            — crack-struct (nvcc, sm_89 по умолчанию: make ARCH=sm_86 для другой карты) и crack-lift64 -> crack/bin/
#   make cpu        — crack-struct-cpu: сборка БЕЗ CUDA (g++ + OpenMP), для машин без nvcc/GPU
#   make unit       — юнит-тесты математики (host, без GPU и oracle)
#   make test       — unit + selftest placement против векторов oracle + ядра (lift/full × GPU/CPU) на host-сгенерированных наблюдениях
#   make e2e        — сквозной тест с oracle (реальный код Mojang); ~15-30 мин; параметры: make e2e E2E_ARGS="--seeds 6 --versions 26.3"
#   make bench      — замер скорости ядер полного перебора
#   make clean      — удалить только бинарники этого Makefile
# Другие инструменты crack/ (slime/pillars/mineshaft — Makefile.slp, nether-bedrock, gpu_biomes) собираются своими файлами.
ARCH   ?= sm_89
NVCC   ?= nvcc
CC     ?= gcc
CXX    ?= g++
BIN     = bin
# --fmad=false / -ffp-contract=off: бит-точность float/double с Java (важно для движка engine/; для crack-struct безвредно)
NVFLAGS = -O3 -arch=$(ARCH) -Xcompiler -fopenmp --fmad=false
CXXFLAGS_CPU = -O3 -march=native -std=c++17 -fopenmp -Wall -Wno-unused-function -Wno-unused-variable -Wno-unknown-pragmas -DNO_CUDA -x c++
CFLAGS  = -O2 -std=gnu11 -ffp-contract=off -fopenmp -Wall -Wno-unused-function -Wno-comment -Wno-misleading-indentation -I../engine
STRUCT_HDR = src/lcg48.h src/pred.h src/sets.h src/mini_json.h
ENGINE_HDR = $(wildcard ../engine/*.h) $(wildcard ../engine/gen/*.h)
E2E_ARGS ?= --seeds 4

all: $(BIN)/crack-struct $(BIN)/crack-lift64
cpu: $(BIN)/crack-struct-cpu

$(BIN)/crack-struct: src/crack_struct.cu $(STRUCT_HDR) | $(BIN)
	$(NVCC) $(NVFLAGS) -o $@ $<
$(BIN)/crack-struct-cpu: src/crack_struct.cu $(STRUCT_HDR) | $(BIN)
	$(CXX) $(CXXFLAGS_CPU) -o $@ $<
$(BIN)/crack-lift64: src/crack_lift64.c $(ENGINE_HDR) | $(BIN)
	$(CC) $(CFLAGS) -o $@ $< -lm
$(BIN):
	mkdir -p $(BIN)

unit: tests/unit_pred.cpp tests/unit_lift64.c $(STRUCT_HDR) src/crack_lift64.c
	$(CXX) -O2 -std=c++17 -Isrc -o /tmp/crack_unit_pred tests/unit_pred.cpp && /tmp/crack_unit_pred ../data
	$(CC) -O2 -std=gnu11 -ffp-contract=off -fopenmp -Wno-comment -Wno-misleading-indentation -I../engine -o /tmp/crack_unit_lift64 tests/unit_lift64.c -lm && /tmp/crack_unit_lift64 ..

test: all unit
	for v in 26.1 26.2 26.3; do $(BIN)/crack-struct --version $$v --selftest ../tests/vectors/$$v/structs.tsv || exit 1; done
	python3 tests/test_kernels.py --seeds 2
e2e: all
	python3 tests/e2e.py $(E2E_ARGS)
bench: $(BIN)/crack-struct
	tests/bench_struct.sh

clean:
	rm -f $(BIN)/crack-struct $(BIN)/crack-struct-cpu $(BIN)/crack-lift64
.PHONY: all cpu unit test e2e bench clean
