#pragma once
#include <cstddef>
#include <cstdint>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <sys/mman.h>
#  include <sys/stat.h>
#  include <fcntl.h>
#  include <unistd.h>
#  ifdef __linux__
#    include <linux/memfd.h>
#    include <sys/syscall.h>
#  endif
#endif

namespace platform {

// Allocate RWX memory for the code cache slab.
inline void* alloc_exec(size_t size) {
#ifdef _WIN32
    return VirtualAlloc(nullptr, size,
                        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
#else
    void* p = mmap(nullptr, size,
                   PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return (p == MAP_FAILED) ? nullptr : p;
#endif
}

inline void free_exec(void* p, size_t size) {
#ifdef _WIN32
    (void)size;
    VirtualFree(p, 0, MEM_RELEASE);
#else
    munmap(p, size);
#endif
}

// Allocate plain RW memory for guest RAM.
inline void* alloc_ram(size_t size) {
#ifdef _WIN32
    return VirtualAlloc(nullptr, size,
                        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#else
    void* p = mmap(nullptr, size,
                   PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return (p == MAP_FAILED) ? nullptr : p;
#endif
}

inline void free_ram(void* p, size_t size) {
#ifdef _WIN32
    (void)size;
    VirtualFree(p, 0, MEM_RELEASE);
#else
    munmap(p, size);
#endif
}

// ---------------------------------------------------------------------------
// Aliased RAM window for fastmem: allocates a contiguous virtual address
// range of `window_size` bytes, backed by a shared memory object of
// `backing_size` bytes.  The backing memory can then be mapped at multiple
// offsets within the window using `map_alias`.
//
// This is used to make the Xbox RAM mirror (0x0C000000) a zero-cost alias
// of main RAM (0x00000000) within the fastmem window, so both ranges
// resolve via the fast `OP [R12+R14]` path without MMIO dispatch.
//
// Layout of the Xbox fastmem window (320 MB = 0x14000000):
//   [0x00000000, 0x08000000)  128 MB  main RAM (backing section)
//   [0x08000000, 0x0C000000)   64 MB  gap (committed, zero-fill)
//   [0x0C000000, 0x10000000)   64 MB  mirror alias → backing[0, 64 MB)
//   [0x10000000, 0x14000000)   64 MB  mirror alias → backing[0, 64 MB)
// ---------------------------------------------------------------------------

#ifdef _WIN32
// Runtime-resolved Windows 10 1803+ APIs for placeholder-based aliasing.
// We load these via GetProcAddress so we link against plain kernel32.lib
// and gracefully fall back on older Windows versions.
#ifndef MEM_RESERVE_PLACEHOLDER
#  define MEM_RESERVE_PLACEHOLDER  0x00040000
#endif
#ifndef MEM_REPLACE_PLACEHOLDER
#  define MEM_REPLACE_PLACEHOLDER  0x00004000
#endif
#ifndef MEM_PRESERVE_PLACEHOLDER
#  define MEM_PRESERVE_PLACEHOLDER 0x00000002
#endif

using PFN_VirtualAlloc2 = PVOID (WINAPI*)(
    HANDLE, PVOID, SIZE_T, ULONG, ULONG,
    void*, ULONG);
using PFN_MapViewOfFile3 = PVOID (WINAPI*)(
    HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG,
    void*, ULONG);

struct Win10MemApis {
    PFN_VirtualAlloc2  pVirtualAlloc2  = nullptr;
    PFN_MapViewOfFile3 pMapViewOfFile3 = nullptr;
    bool resolved = false;
    bool available = false;

    void resolve() {
        if (resolved) return;
        resolved = true;
        HMODULE k = GetModuleHandleW(L"kernelbase.dll");
        if (!k) k = GetModuleHandleW(L"kernel32.dll");
        if (!k) return;
        pVirtualAlloc2  = (PFN_VirtualAlloc2) GetProcAddress(k, "VirtualAlloc2");
        pMapViewOfFile3 = (PFN_MapViewOfFile3)GetProcAddress(k, "MapViewOfFile3");
        available = pVirtualAlloc2 && pMapViewOfFile3;
    }
};

inline Win10MemApis& win10_mem_apis() {
    static Win10MemApis apis;
    apis.resolve();
    return apis;
}
#endif // _WIN32

struct AliasedWindow {
    void*    base         = nullptr;  // start of the virtual address range
    size_t   window_size  = 0;
    size_t   backing_size = 0;
#ifdef _WIN32
    HANDLE   section      = nullptr;
#else
    int      memfd        = -1;
#endif
};

// Allocate the Xbox fastmem window with RAM + mirror aliases.
// `backing_size` = GUEST_RAM_SIZE (128 MB), `window_size` = FASTMEM_WINDOW_SIZE (320 MB).
// Returns AliasedWindow with base=nullptr on failure.
inline AliasedWindow alloc_fastmem_window(size_t window_size, size_t backing_size) {
    AliasedWindow w;
    w.window_size  = window_size;
    w.backing_size = backing_size;

    // Mirror constants (Xbox-specific).
    constexpr size_t MIRROR_BASE = 0x0C000000u;
    constexpr size_t MIRROR_WRAP = 0x04000000u;  // 64 MB
    const     size_t GAP_BASE    = backing_size;  // 0x08000000
    const     size_t GAP_SIZE    = MIRROR_BASE - backing_size;  // 64 MB

#ifdef _WIN32
    // --- Windows: VirtualAlloc2 placeholder + MapViewOfFile3 aliasing ---
    auto& apis = win10_mem_apis();
    if (!apis.available) return w;  // graceful fallback on older Windows

    // 1) Create a pagefile-backed section of `backing_size`.
    LARGE_INTEGER section_sz;
    section_sz.QuadPart = (LONGLONG)backing_size;
    w.section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr,
                                   PAGE_READWRITE, section_sz.HighPart,
                                   section_sz.LowPart, nullptr);
    if (!w.section) return w;

    // 2) Reserve a placeholder of `window_size`.
    w.base = apis.pVirtualAlloc2(nullptr, nullptr, window_size,
                                 MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
                                 PAGE_NOACCESS, nullptr, 0);
    if (!w.base) {
        CloseHandle(w.section);
        w.section = nullptr;
        return w;
    }

    auto fail = [&]() {
        // Best-effort cleanup.
        VirtualFree(w.base, 0, MEM_RELEASE);
        CloseHandle(w.section);
        w.base = nullptr;
        w.section = nullptr;
    };

    auto base8 = static_cast<uint8_t*>(w.base);

    // 3) Split placeholder into 4 chunks:
    //    [0, backing_size) [backing_size, MIRROR_BASE) [MIRROR_BASE, MIRROR_BASE+MIRROR_WRAP) [MIRROR_BASE+MIRROR_WRAP, window_size)

    // Split: [0, backing_size) | rest
    if (!VirtualFree(w.base, backing_size,
                     MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) { fail(); return w; }

    // Split: [backing_size, MIRROR_BASE) | [MIRROR_BASE, window_size)
    if (!VirtualFree(base8 + GAP_BASE, GAP_SIZE,
                     MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) { fail(); return w; }

    // Split: [MIRROR_BASE, MIRROR_BASE+64M) | [MIRROR_BASE+64M, window_size)
    if (!VirtualFree(base8 + MIRROR_BASE, MIRROR_WRAP,
                     MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) { fail(); return w; }

    // 4) Map main RAM at [0, backing_size).
    void* v0 = apis.pMapViewOfFile3(w.section, nullptr,
                                    base8, 0, backing_size,
                                    MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE,
                                    nullptr, 0);
    if (!v0) { fail(); return w; }

    // 5) Commit the gap [backing_size, MIRROR_BASE) as private zero-fill.
    void* vg = apis.pVirtualAlloc2(nullptr, base8 + GAP_BASE, GAP_SIZE,
                                   MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE,
                                   nullptr, 0);
    if (!vg) { fail(); return w; }

    // 6) Map mirror #1 at [MIRROR_BASE, MIRROR_BASE+64M) → backing[0, 64M).
    void* v1 = apis.pMapViewOfFile3(w.section, nullptr,
                                    base8 + MIRROR_BASE, 0, MIRROR_WRAP,
                                    MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE,
                                    nullptr, 0);
    if (!v1) { fail(); return w; }

    // 7) Map mirror #2 at [MIRROR_BASE+64M, window_size) → backing[0, 64M).
    void* v2 = apis.pMapViewOfFile3(w.section, nullptr,
                                    base8 + MIRROR_BASE + MIRROR_WRAP,
                                    0, MIRROR_WRAP,
                                    MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE,
                                    nullptr, 0);
    if (!v2) { fail(); return w; }

#else
    // --- Unix: memfd/shm_open + mmap MAP_FIXED aliasing ---

#  ifdef __linux__
    w.memfd = (int)syscall(SYS_memfd_create, "fastmem", 0);
#  else
    // macOS / BSD: use shm_open with a unique name, then unlink.
    char name[64];
    snprintf(name, sizeof(name), "/fastmem_%d", (int)getpid());
    w.memfd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (w.memfd >= 0) shm_unlink(name);
#  endif
    if (w.memfd < 0) return w;
    if (ftruncate(w.memfd, (off_t)backing_size) < 0) {
        close(w.memfd);
        w.memfd = -1;
        return w;
    }

    // 1) Reserve the full window as private anonymous.
    w.base = mmap(nullptr, window_size, PROT_NONE,
                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (w.base == MAP_FAILED) {
        close(w.memfd);
        w.base = nullptr;
        w.memfd = -1;
        return w;
    }

    auto base8 = static_cast<uint8_t*>(w.base);
    auto fail = [&]() {
        munmap(w.base, window_size);
        close(w.memfd);
        w.base = nullptr;
        w.memfd = -1;
    };

    // 2) Map main RAM at offset 0.
    if (mmap(base8, backing_size, PROT_READ | PROT_WRITE,
             MAP_SHARED | MAP_FIXED, w.memfd, 0) == MAP_FAILED) {
        fail(); return w;
    }

    // 3) Gap [backing_size, MIRROR_BASE) — make accessible private zero-fill.
    if (mmap(base8 + GAP_BASE, GAP_SIZE, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == MAP_FAILED) {
        fail(); return w;
    }

    // 4) Mirror #1 at [MIRROR_BASE, MIRROR_BASE+64M) → backing[0, 64M).
    if (mmap(base8 + MIRROR_BASE, MIRROR_WRAP, PROT_READ | PROT_WRITE,
             MAP_SHARED | MAP_FIXED, w.memfd, 0) == MAP_FAILED) {
        fail(); return w;
    }

    // 5) Mirror #2 at [MIRROR_BASE+64M, window_size) → backing[0, 64M).
    if (mmap(base8 + MIRROR_BASE + MIRROR_WRAP, MIRROR_WRAP, PROT_READ | PROT_WRITE,
             MAP_SHARED | MAP_FIXED, w.memfd, 0) == MAP_FAILED) {
        fail(); return w;
    }
#endif
    return w;
}

inline void free_fastmem_window(AliasedWindow& w) {
    if (!w.base) return;
#ifdef _WIN32
    auto base8 = static_cast<uint8_t*>(w.base);
    constexpr size_t MIRROR_BASE = 0x0C000000u;
    constexpr size_t MIRROR_WRAP = 0x04000000u;
    // Unmap all views (order doesn't matter).
    UnmapViewOfFile(base8);                                // main RAM
    UnmapViewOfFile(base8 + MIRROR_BASE);                  // mirror 1
    UnmapViewOfFile(base8 + MIRROR_BASE + MIRROR_WRAP);    // mirror 2
    // Free the gap (committed private pages).
    VirtualFree(base8 + w.backing_size, 0, MEM_RELEASE);
    // The placeholder regions are now released by the unmap/free calls.
    if (w.section) CloseHandle(w.section);
    w.section = nullptr;
#else
    munmap(w.base, w.window_size);
    if (w.memfd >= 0) close(w.memfd);
    w.memfd = -1;
#endif
    w.base = nullptr;
}

} // namespace platform
