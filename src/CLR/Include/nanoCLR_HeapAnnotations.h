//
// Copyright (c) .NET Foundation and Contributors
// See LICENSE file in the project root for full license information.
//

#ifndef NANOCLR_HEAPANNOTATIONS_H
#define NANOCLR_HEAPANNOTATIONS_H

// Hooks that describe the managed heap to an external memory checker, so that it can tell live objects from free
// blocks inside the single region the heap lives in. They expand to nothing unless the target defines
// NANOCLR_HEAP_ANNOTATIONS and supplies nanoCLR_HeapAnnotations_target.h.
//
// NANOCLR_HEAP_ANNOTATE_ALLOC(ptr, blocks)   'blocks' heap blocks at 'ptr' become a new, uninitialised object.
// NANOCLR_HEAP_ANNOTATE_UNFREE(ptr, blocks)  the free block at 'ptr' is about to be carved up or overwritten; its
//                                            interior becomes writable, its header and links stay as they are.
// NANOCLR_HEAP_ANNOTATE_FREE(ptr, blocks)    'ptr' is now a free block with valid header and links; the rest of it
//                                            must not be touched until it is handed out again.
// NANOCLR_HEAP_STRESS_BEFORE_ALLOCATION(flags) called at the start of every managed allocation; may run a GC there.

#if defined(NANOCLR_HEAP_ANNOTATIONS)

#include <nanoCLR_HeapAnnotations_target.h>

#else

#define NANOCLR_HEAP_ANNOTATE_ALLOC(ptr, blocks)
#define NANOCLR_HEAP_ANNOTATE_UNFREE(ptr, blocks)
#define NANOCLR_HEAP_ANNOTATE_FREE(ptr, blocks)
#define NANOCLR_HEAP_STRESS_BEFORE_ALLOCATION(flags)

#endif

#endif // NANOCLR_HEAPANNOTATIONS_H
