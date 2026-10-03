/* mcgpu_internal.h — внутренности libmcgen_cuda: ошибки, выбор устройства, загрузка массивов. Только C++/CUDA. */
#pragma once
#include <cuda_runtime.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mutex>
#include <vector>
#include "mcgen_gpu_abi.h"

#define MCGPU_EXPORT extern "C" __attribute__((visibility("default")))

/* последняя ошибка потока (текст для моста/аддона) */
void mcgpu_set_error(const char *fmt, ...);
const char *mcgpu_get_error(void);
/* выбранное устройство инициализировано? (вызывается перед работой) */
bool mcgpu_ensure_init(void);
std::mutex &mcgpu_mutex(void);

#define MCGPU_CHECK(call) do { cudaError_t _e = (call); if (_e != cudaSuccess) { mcgpu_set_error("%s: %s (%s:%d)", #call, cudaGetErrorString(_e), __FILE__, __LINE__); return false; } } while (0)

template <typename T> static bool mcgpu_upload(T **dst, const T *src, size_t n) {
    *dst = nullptr;
    if (n == 0) return true;
    cudaError_t e = cudaMalloc((void **)dst, sizeof(T) * n);
    if (e != cudaSuccess) { mcgpu_set_error("cudaMalloc(%zu байт): %s", sizeof(T) * n, cudaGetErrorString(e)); *dst = nullptr; return false; }
    e = cudaMemcpy(*dst, src, sizeof(T) * n, cudaMemcpyHostToDevice);
    if (e != cudaSuccess) { mcgpu_set_error("cudaMemcpy: %s", cudaGetErrorString(e)); cudaFree(*dst); *dst = nullptr; return false; }
    return true;
}
