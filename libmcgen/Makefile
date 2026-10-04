# libmcgen — локальная сборка gcc (кросс-сборка zig — отдельный поток, build.py).
#   make                 $(B)/libmcgen.so + $(B)/mcgen-cli (статически слинкованный с библиотекой)
#   make test            быстрые самопроверки (tests/*.c, nbt_check, tweaks_effect)
#   make clean
# Бит-точность с Java: -ffp-contract=off, без -ffast-math.
# B — каталог сборки (параллельная работа нескольких потоков: make B=build_w9)
B       ?= build
CC      ?= gcc
OPT     ?= -O2
CFLAGS  ?= $(OPT) -g
CFLAGS  += -std=gnu11 -fPIC -ffp-contract=off -fno-fast-math -fvisibility=hidden -Wall -Wextra -Wno-unused-parameter \
           -Wno-unused-function -Wno-comment -Wno-misleading-indentation -Wno-sign-compare -Wno-missing-field-initializers
CPPFLAGS += -Iinclude -Isrc -Igen -I../engine
LDLIBS  = -lm -lpthread -ldl

SRC  := $(wildcard src/*.c)
MESH ?= 1
ifeq ($(MESH),1)
SRC  += $(wildcard src/mesh/*.c)
endif
SRC  += $(wildcard src/structures/*.c)
OBJ  := $(patsubst src/%.c,$(B)/obj/%.o,$(SRC))
HDR  := $(wildcard src/*.h src/*.inc include/*.h gen/*.h ../engine/*.h src/mesh/*.h gpu/*.h)

all: $(B)/libmcgen.so $(B)/libmcgen.a $(B)/mcgen-cli

$(B)/obj/%.o: src/%.c $(HDR)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(B)/libmcgen.so: $(OBJ)
	$(CC) -shared -o $@ $(OBJ) $(LDLIBS)

$(B)/libmcgen.a: $(OBJ)
	rm -f $@; ar rcs $@ $(OBJ)

$(B)/mcgen-cli: cli/mcgen-cli.c $(B)/libmcgen.a $(HDR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ cli/mcgen-cli.c $(B)/libmcgen.a $(LDLIBS)

TESTS := $(patsubst tests/%.c,$(B)/tests/%,$(wildcard tests/*.c))
$(B)/tests/%: tests/%.c $(B)/libmcgen.a $(HDR)
	@mkdir -p $(B)/tests
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $< $(B)/libmcgen.a $(LDLIBS)
tests: $(TESTS)
# быстрые самопроверки без oracle/сервера (нужен пак-каталог ../run/pack-26.3); G1/G2 — см. docs/blender/terrain.md §5
PACK ?= ../run/pack-26.3
test: all tests
	./$(B)/tests/biome_cells $(PACK) 26.3 minecraft:overworld 12345 -4 -4 8
	./$(B)/tests/biome_cells $(PACK) 26.3 minecraft:the_nether 12345 -4 -4 8
	python3 tests/nbt_check.py --version 26.3
	python3 tests/tweaks_effect.py --version 26.3 --n 4
	python3 tests/g5_order.py --versions 26.3     # FeatureSorter против выгрузки игры (стадия FEATURES)

# необязательное ускорение на видеокарте (поток W7, docs/blender/gpu.md): CUDA Toolkit нужен только для сборки
gpu:
	./gpu/build.sh
g9: all tests gpu
	./tests/g9_all.sh $(B)

clean:
	rm -rf $(B)

.PHONY: all clean test tests gpu g9
