# libmcgen — локальная сборка gcc (кросс-сборка zig — отдельный поток, build.py).
#   make                 build/libmcgen.so + build/mcgen-cli (статически слинкованный с библиотекой)
#   make test            быстрые самопроверки (tests/*.c, nbt_check, tweaks_effect)
#   make clean
# Бит-точность с Java: -ffp-contract=off, без -ffast-math.
CC      ?= gcc
OPT     ?= -O2
CFLAGS  ?= $(OPT) -g
CFLAGS  += -std=gnu11 -fPIC -ffp-contract=off -fno-fast-math -fvisibility=hidden -Wall -Wextra -Wno-unused-parameter \
           -Wno-unused-function -Wno-comment -Wno-misleading-indentation -Wno-sign-compare -Wno-missing-field-initializers
CPPFLAGS += -Iinclude -Isrc -Igen -I../engine
LDLIBS  = -lm -lpthread

SRC  := $(wildcard src/*.c)
MESH ?= 1
ifeq ($(MESH),1)
SRC  += $(wildcard src/mesh/*.c)
endif
OBJ  := $(patsubst src/%.c,build/obj/%.o,$(SRC))
HDR  := $(wildcard src/*.h include/*.h gen/*.h ../engine/*.h src/mesh/*.h)

all: build/libmcgen.so build/libmcgen.a build/mcgen-cli

build/obj/%.o: src/%.c $(HDR)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

build/libmcgen.so: $(OBJ)
	$(CC) -shared -o $@ $(OBJ) $(LDLIBS)

build/libmcgen.a: $(OBJ)
	rm -f $@; ar rcs $@ $(OBJ)

build/mcgen-cli: cli/mcgen-cli.c build/libmcgen.a $(HDR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ cli/mcgen-cli.c build/libmcgen.a $(LDLIBS)

TESTS := $(patsubst tests/%.c,build/tests/%,$(wildcard tests/*.c))
build/tests/%: tests/%.c build/libmcgen.a $(HDR)
	@mkdir -p build/tests
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $< build/libmcgen.a $(LDLIBS)
tests: $(TESTS)
# быстрые самопроверки без oracle/сервера (нужен пак-каталог ../run/pack-26.3); G1/G2 — см. docs/blender/terrain.md §5
PACK ?= ../run/pack-26.3
test: all tests
	./build/tests/biome_cells $(PACK) 26.3 minecraft:overworld 12345 -4 -4 8
	./build/tests/biome_cells $(PACK) 26.3 minecraft:the_nether 12345 -4 -4 8
	python3 tests/nbt_check.py --version 26.3
	python3 tests/tweaks_effect.py --version 26.3 --n 4
	python3 tests/g5_order.py --versions 26.3     # FeatureSorter против выгрузки игры (стадия FEATURES)

clean:
	rm -rf build

.PHONY: all clean test tests
