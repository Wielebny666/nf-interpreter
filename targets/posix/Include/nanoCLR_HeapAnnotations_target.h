//
// Copyright (c) .NET Foundation and Contributors
// See LICENSE file in the project root for full license information.
//

#ifndef NANOCLR_HEAPANNOTATIONS_TARGET_H
#define NANOCLR_HEAPANNOTATIONS_TARGET_H

// Valgrind memcheck implementation of the managed heap hooks. Outside valgrind every client request is a handful of
// no-op instructions, so the same binary also runs natively.

#include <valgrind/memcheck.h>

#define NANOCLR_HEAP_ANNOTATE_ALLOC(ptr, blocks)                                                                       \
    (void)VALGRIND_MAKE_MEM_UNDEFINED((ptr), (size_t)(blocks) * sizeof(CLR_RT_HeapBlock))

#define NANOCLR_HEAP_ANNOTATE_UNFREE(ptr, blocks)                                                                      \
    do                                                                                                                 \
    {                                                                                                                  \
        if ((blocks) > 1)                                                                                              \
        {                                                                                                              \
            (void)VALGRIND_MAKE_MEM_UNDEFINED(                                                                         \
                (CLR_RT_HeapBlock *)(ptr) + 1,                                                                         \
                (size_t)((blocks) - 1) * sizeof(CLR_RT_HeapBlock));                                                    \
        }                                                                                                              \
    } while (0)

#define NANOCLR_HEAP_ANNOTATE_FREE(ptr, blocks)                                                                        \
    do                                                                                                                 \
    {                                                                                                                  \
        if ((blocks) > 1)                                                                                              \
        {                                                                                                              \
            (void)VALGRIND_MAKE_MEM_NOACCESS(                                                                          \
                (CLR_RT_HeapBlock *)(ptr) + 1,                                                                         \
                (size_t)((blocks) - 1) * sizeof(CLR_RT_HeapBlock));                                                    \
        }                                                                                                              \
    } while (0)

// The header is the 32-bit data id at the start of the block; heap walks read nothing else of a free block.
#define NANOCLR_HEAP_ANNOTATE_QUARANTINE(ptr, blocks)                                                                  \
    (void)VALGRIND_MAKE_MEM_NOACCESS(                                                                                  \
        (CLR_UINT8 *)(ptr) + sizeof(CLR_UINT32),                                                                       \
        (size_t)(blocks) * sizeof(CLR_RT_HeapBlock) - sizeof(CLR_UINT32))

#define NANOCLR_HEAP_ANNOTATE_RELINK(ptr) (void)VALGRIND_MAKE_MEM_UNDEFINED((ptr), sizeof(CLR_RT_HeapBlock))

bool NanoCLR_HeapQuarantine_Enabled();

#define NANOCLR_HEAP_QUARANTINE_ENABLED() NanoCLR_HeapQuarantine_Enabled()

void NanoCLR_HeapStress_BeforeAllocation(CLR_UINT32 flags);

#define NANOCLR_HEAP_STRESS_BEFORE_ALLOCATION(flags) NanoCLR_HeapStress_BeforeAllocation(flags)

#endif // NANOCLR_HEAPANNOTATIONS_TARGET_H
