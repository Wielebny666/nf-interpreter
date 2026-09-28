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

void NanoCLR_HeapStress_BeforeAllocation(CLR_UINT32 flags);

#define NANOCLR_HEAP_STRESS_BEFORE_ALLOCATION(flags) NanoCLR_HeapStress_BeforeAllocation(flags)

#endif // NANOCLR_HEAPANNOTATIONS_TARGET_H
