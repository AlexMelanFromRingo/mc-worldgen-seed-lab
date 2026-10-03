"""Объём ОЗУ (без сторонних пакетов) и оценка памяти под воксели: защита от случайной генерации 512×512 чанков (≈ 50 ГБ)."""
import ctypes
import os
import subprocess
import sys

CHUNK_BYTES = {'minecraft:overworld': 384 * 256 * 2, 'minecraft:the_nether': 256 * 256 * 2, 'minecraft:the_end': 256 * 256 * 2}
EXTRA_PER_CHUNK = (384 // 4) * 16 + 4 * 256 * 2            # биомы + 4 карты высот


def estimate_bytes(dimension, nx, nz):
    """Память под блоки/биомы/карты высот региона (u16 блоки + биомы + высоты) — без мешей."""
    return (CHUNK_BYTES.get(dimension, 384 * 256 * 2) + EXTRA_PER_CHUNK) * nx * nz


def total_ram_bytes():
    """Физическая память в байтах или None."""
    try:
        if sys.platform.startswith('linux'):
            with open('/proc/meminfo') as f:
                for ln in f:
                    if ln.startswith('MemTotal:'):
                        return int(ln.split()[1]) * 1024
        elif sys.platform == 'darwin':
            return int(subprocess.check_output(['sysctl', '-n', 'hw.memsize'], text=True).strip())
        elif sys.platform.startswith('win'):
            class MEMSTAT(ctypes.Structure):
                _fields_ = [('dwLength', ctypes.c_ulong), ('dwMemoryLoad', ctypes.c_ulong), ('ullTotalPhys', ctypes.c_ulonglong),
                            ('ullAvailPhys', ctypes.c_ulonglong), ('ullTotalPageFile', ctypes.c_ulonglong), ('ullAvailPageFile', ctypes.c_ulonglong),
                            ('ullTotalVirtual', ctypes.c_ulonglong), ('ullAvailVirtual', ctypes.c_ulonglong), ('sullAvailExtendedVirtual', ctypes.c_ulonglong)]
            m = MEMSTAT()
            m.dwLength = ctypes.sizeof(MEMSTAT)
            if ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(m)):
                return int(m.ullTotalPhys)
    except (OSError, ValueError, subprocess.SubprocessError):
        pass
    return None


def check_fits(dimension, nx, nz, fraction=0.6):
    """(ok, нужно_байт, лимит_байт|None): воксели не должны занимать больше fraction физической памяти."""
    need = estimate_bytes(dimension, nx, nz)
    ram = total_ram_bytes()
    return (ram is None or need <= ram * fraction), need, (int(ram * fraction) if ram else None)


def process_memory():
    """(текущий RSS, пиковый RSS) процесса в байтах; (0, 0), если платформа не поддержана."""
    try:
        if sys.platform.startswith('linux'):
            with open('/proc/self/statm') as f:
                rss = int(f.read().split()[1]) * os.sysconf('SC_PAGE_SIZE')
            peak = rss
            with open('/proc/self/status') as f:
                for ln in f:
                    if ln.startswith('VmHWM:'):
                        peak = int(ln.split()[1]) * 1024
            return rss, peak
        if sys.platform == 'darwin':
            import resource
            peak = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss          # на macOS — байты
            return peak, peak
        if sys.platform.startswith('win'):
            class PMC(ctypes.Structure):
                _fields_ = [('cb', ctypes.c_ulong), ('PageFaultCount', ctypes.c_ulong), ('PeakWorkingSetSize', ctypes.c_size_t),
                            ('WorkingSetSize', ctypes.c_size_t), ('QuotaPeakPagedPoolUsage', ctypes.c_size_t), ('QuotaPagedPoolUsage', ctypes.c_size_t),
                            ('QuotaPeakNonPagedPoolUsage', ctypes.c_size_t), ('QuotaNonPagedPoolUsage', ctypes.c_size_t),
                            ('PagefileUsage', ctypes.c_size_t), ('PeakPagefileUsage', ctypes.c_size_t)]
            pmc = PMC()
            pmc.cb = ctypes.sizeof(PMC)
            h = ctypes.windll.kernel32.GetCurrentProcess()
            if ctypes.windll.psapi.GetProcessMemoryInfo(h, ctypes.byref(pmc), pmc.cb):
                return int(pmc.WorkingSetSize), int(pmc.PeakWorkingSetSize)
    except (OSError, ValueError, AttributeError, ImportError):
        pass
    return 0, 0
