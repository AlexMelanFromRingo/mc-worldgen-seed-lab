# Сборка crack-nether-bedrock.
#   make        — CUDA-версия (nvcc, sm_89) -> ../../bin/crack-nether-bedrock
#   make cpu    — CPU-only версия (g++ + OpenMP) -> ../../bin/crack-nether-bedrock-cpu
#   make test   — тесты ../../tests/nether_bedrock/run_tests.sh
ARCH    ?= sm_89
NVCC    ?= nvcc
BIN      = ../../bin
NVFLAGS  = -O3 -arch=$(ARCH) -std=c++17 -lineinfo -Xcompiler -fopenmp,-O3,-Wall,-Wno-unused-function,-Wno-unused-variable,-ffp-contract=off
CXXFLAGS = -O3 -march=native -std=c++17 -fopenmp -Wall -Wno-unused-function -Wno-unused-variable -ffp-contract=off -DNO_CUDA -x c++
SRC      = crack_nether_bedrock.cu nb_core.h

all: $(BIN)/crack-nether-bedrock
cpu: $(BIN)/crack-nether-bedrock-cpu

$(BIN)/crack-nether-bedrock: $(SRC) | $(BIN)
	$(NVCC) $(NVFLAGS) -o $@ crack_nether_bedrock.cu
$(BIN)/crack-nether-bedrock-cpu: $(SRC) | $(BIN)
	g++ $(CXXFLAGS) -o $@ crack_nether_bedrock.cu
$(BIN):
	mkdir -p $(BIN)
test: all
	../../tests/nether_bedrock/run_tests.sh
.PHONY: all cpu test
