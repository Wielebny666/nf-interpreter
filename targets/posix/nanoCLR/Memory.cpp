//
// Copyright (c) .NET Foundation and Contributors
// See LICENSE file in the project root for full license information.
//

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#if defined(NANOCLR_HEAP_ANNOTATIONS)
#include <valgrind/memcheck.h>
#endif

namespace
{
    constexpr size_t c_DefaultHeapSizeBytes = 10 * 1024 * 1024;

    unsigned char *g_MemoryStart = nullptr;
    unsigned char *g_CustomHeapStart = nullptr;
    std::mutex g_DefaultHeapMutex;
    std::mutex g_CustomHeapMutex;
} // namespace

// Heap size drives how often compaction runs and how much it has to move, so
// being able to sweep it without rebuilding is worth an environment variable.
// NANOCLR_HEAP_SIZE_MB overrides the default; anything unparseable is ignored.
static size_t HeapSizeBytes()
{
    static size_t cached = 0;

    if (cached == 0)
    {
        cached = c_DefaultHeapSizeBytes;

        if (const char *fromEnv = std::getenv("NANOCLR_HEAP_SIZE_MB"))
        {
            const long megabytes = std::strtol(fromEnv, nullptr, 10);

            if (megabytes > 0 && megabytes <= 1024)
            {
                cached = (size_t)megabytes * 1024 * 1024;
            }
        }
    }

    return cached;
}

void HeapLocation(unsigned char *&BaseAddress, unsigned int &SizeInBytes)
{
    std::lock_guard<std::mutex> lock(g_DefaultHeapMutex);

    if (g_MemoryStart == nullptr)
    {
        // TODO: evaluate mmap() if we need stronger parity with win32 VirtualAlloc behavior.
        g_MemoryStart = static_cast<unsigned char *>(std::malloc(HeapSizeBytes()));

        if (g_MemoryStart != nullptr)
        {
            std::memset(g_MemoryStart, 0xEA, HeapSizeBytes());
        }
    }
#if defined(NANOCLR_HEAP_ANNOTATIONS)
    else
    {
        // A soft reboot of the CLR gets the same memory back, and HeapCluster_Initialize reads all of it to salvage
        // the objects that survive a reboot. The annotations of the previous session no longer describe it.
        (void)VALGRIND_MAKE_MEM_DEFINED(g_MemoryStart, HeapSizeBytes());
    }
#endif

    BaseAddress = g_MemoryStart;
    SizeInBytes = (g_MemoryStart != nullptr) ? static_cast<unsigned int>(HeapSizeBytes()) : 0;
}

void CustomHeapLocation(unsigned char *&BaseAddress, unsigned int &SizeInBytes)
{
    std::lock_guard<std::mutex> lock(g_CustomHeapMutex);

    if (g_CustomHeapStart == nullptr)
    {
        // TODO: revisit custom heap ownership model when CLR runtime is wired.
        g_CustomHeapStart = static_cast<unsigned char *>(std::malloc(HeapSizeBytes()));

        if (g_CustomHeapStart != nullptr)
        {
            std::memset(g_CustomHeapStart, 0xEA, HeapSizeBytes());
        }
    }

    BaseAddress = g_CustomHeapStart;
    SizeInBytes = (g_CustomHeapStart != nullptr) ? static_cast<unsigned int>(HeapSizeBytes()) : 0;
}
