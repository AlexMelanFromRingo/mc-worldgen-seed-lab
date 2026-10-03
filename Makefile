# Верхнеуровневая сборка. Подробности — README.md и docs/.
#   make setup V="26.1 26.2 26.3"   скачать официальные jar'ы Mojang, распаковать датапак, декомпилировать (tools/fetch_game.py)
#   make mcquery                    CLI биомов/климата на движке engine/ (tools/mcquery)
#   make crack                      все GPU/CPU-инструменты восстановления seed в crack/bin/ (нужен nvcc; ARCH=sm_86 для другой карты)
#   make crack-cpu                  то же без CUDA (g++ + OpenMP)
#   make oracle V=26.3              эталон на реальном коде Mojang (нужен `make setup`, JDK 25)
#   make libmcgen                   библиотека генератора мира (libmcgen/build/libmcgen.so, mcgen-cli); make libmcgen-all — 4 платформы (zig); make addon — расширение Blender
ARCH ?= sm_89
V    ?= 26.1 26.2 26.3

.PHONY: setup mcquery crack crack-cpu oracle libmcgen libmcgen-all addon clean
setup:
	python3 tools/fetch_game.py $(V)

mcquery: tools/mcquery
tools/mcquery: tools/mcquery.c $(wildcard engine/*.h) $(wildcard engine/gen/*.h)
	$(CC) -O2 -std=gnu11 -ffp-contract=off -fopenmp -Wno-comment -Wno-misleading-indentation -Iengine -o $@ $< -lm

crack:
	$(MAKE) -C crack ARCH=$(ARCH)
	$(MAKE) -C crack -f Makefile.slp ARCH=$(ARCH)
	$(MAKE) -C crack/src/nether_bedrock ARCH=$(ARCH)
	CUDA_ARCH=$(ARCH) crack/src/gpu_biomes/build.sh all

crack-cpu:
	$(MAKE) -C crack cpu
	$(MAKE) -C crack -f Makefile.slp cpu
	$(MAKE) -C crack/src/nether_bedrock cpu

oracle:
	oracle/build.sh $(firstword $(V))

libmcgen:
	$(MAKE) -C libmcgen

libmcgen-all:
	python3 libmcgen/build.py --targets all

addon: libmcgen-all
	python3 tools/build_extension.py

clean:
	rm -f tools/mcquery
	$(MAKE) -C crack clean
