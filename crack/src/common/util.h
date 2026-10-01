/* util.h — таймер, CUDA-обёртки, разбор аргументов. */
#ifndef CRACK_UTIL_H
#define CRACK_UTIL_H
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <string>
#include <vector>
#include <omp.h>

#ifndef NO_CUDA
#include <cuda_runtime.h>
#define CK(call) do { cudaError_t e_ = (call); if (e_ != cudaSuccess) { fprintf(stderr, "CUDA error %s:%d: %s\n", __FILE__, __LINE__, cudaGetErrorString(e_)); exit(3); } } while (0)
#endif

static inline double now_s() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

/* 1 если есть работающее CUDA-устройство (и сборка с CUDA) */
static inline bool have_gpu(int *sms = nullptr, std::string *name = nullptr) {
#ifdef NO_CUDA
    (void)sms; (void)name; return false;
#else
    int n = 0;
    if (cudaGetDeviceCount(&n) != cudaSuccess || n <= 0) { cudaGetLastError(); return false; }
    cudaDeviceProp pr; if (cudaGetDeviceProperties(&pr, 0) != cudaSuccess) { cudaGetLastError(); return false; }
    if (sms) *sms = pr.multiProcessorCount;
    if (name) *name = pr.name;
    return true;
#endif
}

/* SplitMix64 — детерминированный генератор для синтетических данных */
struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed) {}
    uint64_t next() { uint64_t z = (s += 0x9E3779B97F4A7C15ULL); z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL; z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL; return z ^ (z >> 31); }
    int range(int n) { return (int)(next() % (uint64_t)n); }
};

static inline bool parse_u64(const char *s, unsigned long long &v) {
    char *e; errno = 0;
    if (s[0] == '-') { long long x = strtoll(s, &e, 0); v = (unsigned long long)x; return *e == 0 && errno == 0; }
    v = strtoull(s, &e, 0); return *s && *e == 0 && errno == 0;
}
#endif
