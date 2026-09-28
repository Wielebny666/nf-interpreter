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
// NANOCLR_HEAP_ANNOTATE_UNFREE(ptr, blocks)  the free block at 'ptr' is about to be carved up or overwritten, or a
//                                            block from the event cache handed out again; its interior becomes
//                                            writable and uninitialised, its header and links stay as they are.
//                                            (The CLR relies on the links of a node taken from the cache being null.)
// NANOCLR_HEAP_ANNOTATE_FREE(ptr, blocks)    'ptr' is now a free block with valid header and links; the rest of it
//                                            must not be touched until it is handed out again. Also used for
//                                            blocks parked in the event cache, which have the same shape.
// NANOCLR_HEAP_ANNOTATE_QUARANTINE(ptr, blocks)
//                                            'ptr' is a free block kept out of the free list; only its header may
//                                            be read.
// NANOCLR_HEAP_ANNOTATE_RELINK(ptr)          the first heap block at 'ptr' is about to be rewritten as a free-list
//                                            node.
// NANOCLR_HEAP_QUARANTINE_ENABLED()          true when objects that die in a collection stay out of the free list
//                                            until the next one.
// NANOCLR_HEAP_STRESS_BEFORE_ALLOCATION(flags) called at the start of every managed allocation; may run a GC there.

#if defined(NANOCLR_HEAP_ANNOTATIONS)

#include <nanoCLR_HeapAnnotations_target.h>

#else

#define NANOCLR_HEAP_ANNOTATE_ALLOC(ptr, blocks)
#define NANOCLR_HEAP_ANNOTATE_UNFREE(ptr, blocks)
#define NANOCLR_HEAP_ANNOTATE_FREE(ptr, blocks)
#define NANOCLR_HEAP_ANNOTATE_QUARANTINE(ptr, blocks)
#define NANOCLR_HEAP_ANNOTATE_RELINK(ptr)
#define NANOCLR_HEAP_QUARANTINE_ENABLED() false
#define NANOCLR_HEAP_STRESS_BEFORE_ALLOCATION(flags)

#endif

#endif // NANOCLR_HEAPANNOTATIONS_H
