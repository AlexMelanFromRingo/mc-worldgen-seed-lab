/* mcgpu_core.cu — libmcgen_cuda: устройства, инициализация, ошибки. C-ABI для моста (src/gpu_bridge.c). */
#include "mcgpu_internal.h"
#include <stdarg.h>
#include <string>

static thread_local char g_err[512];
void mcgpu_set_error(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vsnprintf(g_err, sizeof g_err, fmt, ap); va_end(ap);
}
const char *mcgpu_get_error(void) { return g_err; }

static std::mutex g_mu;
std::mutex &mcgpu_mutex(void) { return g_mu; }

static int g_dev = -1;          /* выбранное устройство */
static bool g_inited = false;

bool mcgpu_ensure_init(void) {
    if (g_inited) return cudaSetDevice(g_dev) == cudaSuccess;
    int n = 0;
    cudaError_t e = cudaGetDeviceCount(&n);
    if (e != cudaSuccess || n <= 0) { mcgpu_set_error("нет устройств CUDA: %s", e == cudaSuccess ? "0 устройств" : cudaGetErrorString(e)); return false; }
    if (g_dev < 0) g_dev = 0;
    if (g_dev >= n) { mcgpu_set_error("устройство %d не существует (всего %d)", g_dev, n); return false; }
    cudaSetDeviceFlags(cudaDeviceScheduleBlockingSync);   /* ожидание GPU без «вращения» ядра CPU (рабочие потоки CPU заняты) */
    e = cudaSetDevice(g_dev);
    if (e != cudaSuccess) { mcgpu_set_error("cudaSetDevice(%d): %s", g_dev, cudaGetErrorString(e)); return false; }
    e = cudaFree(0);   /* создаёт контекст */
    if (e != cudaSuccess) { mcgpu_set_error("инициализация контекста CUDA: %s", cudaGetErrorString(e)); return false; }
    g_inited = true;
    return true;
}

MCGPU_EXPORT int mcgpu_abi(void) { return MCGPU_ABI; }

MCGPU_EXPORT int mcgpu_device_count(void) {
    int n = 0;
    if (cudaGetDeviceCount(&n) != cudaSuccess) { cudaGetLastError(); return 0; }
    return n;
}
/* имя, вычислительная способность, память (МБ); 0 — ок */
MCGPU_EXPORT int mcgpu_device_info(int i, char *name, size_t namelen, int *cc_major, int *cc_minor, size_t *mem_mb) {
    cudaDeviceProp p;
    if (cudaGetDeviceProperties(&p, i) != cudaSuccess) { cudaGetLastError(); return 1; }
    if (name && namelen) { strncpy(name, p.name, namelen - 1); name[namelen - 1] = 0; }
    if (cc_major) *cc_major = p.major;
    if (cc_minor) *cc_minor = p.minor;
    if (mem_mb) *mem_mb = p.totalGlobalMem >> 20;
    return 0;
}
/* выбрать устройство; 0 — ок. Повторный вызов с другим номером допустим до первой работы. */
MCGPU_EXPORT int mcgpu_select(int device) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_inited && device == g_dev) return 0;
    if (g_inited) { mcgpu_set_error("устройство уже выбрано (%d)", g_dev); return 1; }
    g_dev = device;
    return mcgpu_ensure_init() ? 0 : 1;
}
MCGPU_EXPORT int mcgpu_selected(void) { return g_inited ? g_dev : -1; }
MCGPU_EXPORT const char *mcgpu_last_error(void) { return mcgpu_get_error(); }
/* свободная/общая память текущего устройства (МБ) */
MCGPU_EXPORT int mcgpu_mem_info(size_t *free_mb, size_t *total_mb) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!mcgpu_ensure_init()) return 1;
    size_t f = 0, t = 0;
    if (cudaMemGetInfo(&f, &t) != cudaSuccess) { mcgpu_set_error("cudaMemGetInfo"); return 1; }
    if (free_mb) *free_mb = f >> 20;
    if (total_mb) *total_mb = t >> 20;
    return 0;
}
