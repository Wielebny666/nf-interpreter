//
// Copyright (c) .NET Foundation and Contributors
// See LICENSE file in the project root for full license information.
//

#include <nanoCLR_Runtime.h>

#include <valgrind/valgrind.h>

#include <cstdio>
#include <cstdlib>

// Set while this file runs a GC or the self-test itself, both of which allocate.
static bool s_running = false;

// NANOCLR_GC_STRESS=<n> runs a full GC before every n-th managed allocation, the same GC an allocation runs when the
// heap is full. An object that native code reaches without rooting it is then freed long before a real application
// would fill the heap, and memcheck reports the access. Unset or 0 leaves allocation alone.
static unsigned long GcStressInterval()
{
    static long cached = -1;

    if (cached < 0)
    {
        cached = 0;

        if (const char *fromEnv = std::getenv("NANOCLR_GC_STRESS"))
        {
            const long interval = std::strtol(fromEnv, nullptr, 10);

            if (interval > 0)
            {
                cached = interval;
            }
        }
    }

    return (unsigned long)cached;
}

// NANOCLR_HEAP_SELFTEST=1 plants two known heap errors at the first managed allocation, so a run under valgrind shows
// whether the annotations work: a read of an object the GC has freed, and a decision taken on object memory nothing
// has written yet. Both are reported from this file; anything else in the log is a finding of its own.
static void RunSelfTest()
{
    std::fprintf(stderr, "nanoCLR heap self-test: planting a read after free and an uninitialised read\n");
    VALGRIND_PRINTF("nanoCLR heap self-test: expect 'Invalid read' and 'Conditional jump' from HeapAnnotations.cpp\n");

    // Nothing roots 'unrooted', so the GC frees the string and the text past its first heap block becomes
    // inaccessible.
    CLR_RT_HeapBlock unrooted;
    unrooted.SetObjectReference(nullptr);

    if (SUCCEEDED(CLR_RT_HeapBlock_String::CreateInstance(
            unrooted,
            "nanoCLR heap self-test: this string is freed by the GC before it is read back")))
    {
        const char *text = unrooted.RecoverString();

        g_CLR_RT_ExecutionEngine.PerformGarbageCollection();

        volatile char freed = text[50];
        (void)freed;
    }

    // A new object is uninitialised until the CLR writes it; this one is never written.
    CLR_RT_HeapBlock *fresh = g_CLR_RT_ExecutionEngine.ExtractHeapBlocksForObjects(DATATYPE_I4, 0, 2);

    if (fresh != nullptr)
    {
        const volatile CLR_UINT8 *payload = (const CLR_UINT8 *)&fresh[1];

        if (payload[0] == 0x5A)
        {
            std::fprintf(stderr, "nanoCLR heap self-test: uninitialised byte happened to be 0x5A\n");
        }
    }

    VALGRIND_PRINTF("nanoCLR heap self-test: done\n");
}

static bool SelfTestRequested()
{
    const char *fromEnv = std::getenv("NANOCLR_HEAP_SELFTEST");

    return fromEnv != nullptr && std::strtol(fromEnv, nullptr, 10) != 0;
}

void NanoCLR_HeapStress_BeforeAllocation(CLR_UINT32 flags)
{
    static unsigned long allocations = 0;
    static bool selfTestDone = false;

    // PerformGarbageCollection allocates the finalizer thread after the GC itself, so without the guard every
    // allocation it makes would start another GC.
    if (s_running || g_CLR_RT_ExecutionEngine.m_heapState == CLR_RT_ExecutionEngine::c_HeapState_UnderGC ||
        (flags & CLR_RT_HeapBlock::HB_NoGcOnFailedAllocation) != 0)
    {
        return;
    }

    if (!selfTestDone)
    {
        selfTestDone = true;

        if (SelfTestRequested())
        {
            s_running = true;
            RunSelfTest();
            s_running = false;
        }
    }

    const unsigned long interval = GcStressInterval();

    if (interval != 0 && (++allocations % interval) == 0)
    {
        s_running = true;
        g_CLR_RT_ExecutionEngine.PerformGarbageCollection();
        s_running = false;
    }
}
