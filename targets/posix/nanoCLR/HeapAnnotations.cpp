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

// Value of a numeric environment variable, or 0 when it is unset, not a number or not positive.
static unsigned long PositiveFromEnv(const char *name)
{
    if (const char *fromEnv = std::getenv(name))
    {
        const long value = std::strtol(fromEnv, nullptr, 10);

        if (value > 0)
        {
            return (unsigned long)value;
        }
    }

    return 0;
}

// NANOCLR_GC_STRESS=<n> runs a full GC before every n-th managed allocation, the same GC an allocation runs when the
// heap is full. An object that native code reaches without rooting it is then freed long before a real application
// would fill the heap, and memcheck reports the access. Unset or 0 leaves allocation alone.
static unsigned long GcStressInterval()
{
    static const unsigned long interval = PositiveFromEnv("NANOCLR_GC_STRESS");

    return interval;
}

// NANOCLR_COMPACT_STRESS=<n> schedules a heap compaction after every n-th of those collections. The CLR runs it at its
// next safe point, exactly like a compaction it schedules itself, so live objects move and native code that kept a raw
// pointer to one across calls is left holding an address memcheck reports.
static unsigned long CompactStressInterval()
{
    static const unsigned long interval = PositiveFromEnv("NANOCLR_COMPACT_STRESS");

    return interval;
}

// NANOCLR_HEAP_QUARANTINE=1 keeps the objects that die in a collection out of the free list until the next one, with
// everything but their header inaccessible. A stale pointer then keeps reaching memory memcheck reports, instead of
// an object the allocator has put there since. It needs more heap; raise NANOCLR_HEAP_SIZE_MB if allocations fail.
bool NanoCLR_HeapQuarantine_Enabled()
{
    static int cached = -1;

    if (cached < 0)
    {
        const char *fromEnv = std::getenv("NANOCLR_HEAP_QUARANTINE");

        cached = (fromEnv != nullptr && std::strtol(fromEnv, nullptr, 10) != 0) ? 1 : 0;
    }

    return cached != 0;
}

// NANOCLR_HEAP_SELFTEST=1 plants known heap errors at the first managed allocation, so a run under valgrind shows
// whether the annotations work. All of them are reported from this file, between two markers that also state how many
// to expect; anything else in the log is a finding of its own.
static void RunSelfTest()
{
    const bool quarantine = NanoCLR_HeapQuarantine_Enabled();
    const int expected = quarantine ? 4 : 3;

    std::fprintf(stderr, "nanoCLR heap self-test: planting %d known heap errors\n", expected);
    VALGRIND_PRINTF("nanoCLR heap self-test: expect %d errors from HeapAnnotations.cpp\n", expected);

    // 1. Read after free. Nothing roots 'unrooted', so the GC frees the string and the text past its first heap block
    // becomes inaccessible.
    CLR_RT_HeapBlock unrooted;
    unrooted.SetObjectReference(nullptr);

    if (SUCCEEDED(CLR_RT_HeapBlock_String::CreateInstance(
            unrooted,
            "nanoCLR heap self-test: this string is freed by the GC before it is read back")))
    {
        const CLR_UINT8 *object = (const CLR_UINT8 *)unrooted.Dereference();
        const char *text = unrooted.RecoverString();

        g_CLR_RT_ExecutionEngine.PerformGarbageCollection();

        volatile char freed = text[50];
        (void)freed;

        // 2. Only with quarantine: the first heap block of a freed object, which a free-list node would otherwise
        // keep accessible.
        if (quarantine)
        {
            volatile CLR_UINT8 field = object[sizeof(CLR_RT_HeapBlock) - 1];
            (void)field;
        }
    }

    // 3. A new object is uninitialised until the CLR writes it; this one is never written.
    CLR_RT_HeapBlock *fresh = g_CLR_RT_ExecutionEngine.ExtractHeapBlocksForObjects(DATATYPE_I4, 0, 2);

    if (fresh != nullptr)
    {
        const volatile CLR_UINT8 *payload = (const CLR_UINT8 *)&fresh[1];

        if (payload[0] == 0x5A)
        {
            std::fprintf(stderr, "nanoCLR heap self-test: uninitialised byte happened to be 0x5A\n");
        }
    }

    // 4. Read after release to the event cache, the way a stack frame or a lock request is released.
    CLR_RT_HeapBlock *node =
        g_CLR_RT_EventCache.Extract_Node(DATATYPE_OBJECT, CLR_RT_HeapBlock::HB_InitializeToZero, 3);

    if (node != nullptr)
    {
        g_CLR_RT_EventCache.Append_Node(node);

        volatile CLR_UINT8 released = ((const CLR_UINT8 *)&node[1])[0];
        (void)released;
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
    static unsigned long collections = 0;
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

        const unsigned long compactInterval = CompactStressInterval();

        if (compactInterval != 0 && (++collections % compactInterval) == 0)
        {
            CLR_EE_SET(Compaction_Pending);
        }
    }
}
