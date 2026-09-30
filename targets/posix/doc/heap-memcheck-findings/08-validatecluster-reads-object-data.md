# 8. Heap validation reads object data as list links

## Summary

With heap validation at level 3 or 4, `ValidateCluster` checks every heap block against the blocks its `next` and `prev` links point to. Only list nodes have links there; in every other block that word is object data. When the data happens to be a number below the block's address, the check dereferences it: the CLR crashes during a compaction, or prints false `Overlapping blocks detected` messages when the address is mapped. The `-soak` presets of this repository build at level 3.

## Where

- [src/CLR/Core/GarbageCollector_Info.cpp:119](../../../../src/CLR/Core/GarbageCollector_Info.cpp): `ValidateCluster` reads `ptr->Next()` for every block except `DATATYPE_VALUETYPE` and `DATATYPE_CLASS`, and at line 126 reads `nextPtr->DataSize()` when the value is below `ptr`. Lines 136 to 143 do the same with `ptr->Prev()`.
- [src/CLR/Core/GarbageCollector_Compaction.cpp:305](../../../../src/CLR/Core/GarbageCollector_Compaction.cpp): `Heap_Compact` calls `ValidateCluster` after every move (lines 305 and 306) and `ValidateHeap`, which calls it for every cluster, around every relocation (lines 185, 189, 354 and 358).

## Cause

`Next()` and `Prev()` read the data of a block's first heap block as the links of a `CLR_RT_HeapBlock_Node`. That holds for free blocks, event cache blocks and other list nodes, but not for strings, arrays, boxed values and the other types the check does not exclude. Their data read as a pointer is often a small number, such as an element count or a value, and `nextPtr + nextPtr->DataSize()` then reads unmapped memory.

## How it was found

1. HeapStress on the `posix-x64-soak` and `posix-x86-soak` presets crashed with `SIGSEGV` at its first compacting GC (`GC.Run(true)`), after several `Overlapping blocks detected` messages. GCCompactionSoak crashes the same way on `posix-x86-soak`; on `posix-x64-soak` it printed 1890 such messages in 90 seconds.
2. gdb at the crash: the block being checked was a one-block `DATATYPE_I4` on x86-64 and a `DATATYPE_SZARRAY` on i386, and the word read as `next` was `0x70141` and `0x10002`: object data, not an address.
3. The memcheck build with `NANO_POSIX_VALIDATE_HEAP=3` shows the same reads under valgrind: during HeapStress, valgrind stopped reporting at its limit of 10,000,000 errors, nearly all from `ValidateCluster` using uninitialised object memory as a pointer or reading through such a pointer into a free block. With `NANOCLR_HEAP_QUARANTINE=1` that build crashes as in step 1 even without valgrind, because a quarantined block keeps the dead object's data where a free block has its links.

Seen on x86-64 and i386.

## Evidence

```
Overlapping blocks detected: Previous block of 0x7ffff6f161e8 is overlapping it.
...
Thread 1 "nanoFramework.n" received signal SIGSEGV, Segmentation fault.
#0  CLR_RT_HeapBlock::DataSize (this=0x70141) at src/CLR/Include/nanoCLR_Runtime__HeapBlock.h:779
#1  CLR_RT_GarbageCollector::ValidateCluster (hc=0x7ffff6f16010) at src/CLR/Core/GarbageCollector_Info.cpp:126
#2  CLR_RT_GarbageCollector::Heap_Compact () at src/CLR/Core/GarbageCollector_Compaction.cpp:305
#3  CLR_RT_GarbageCollector::ExecuteCompaction () at src/CLR/Core/GarbageCollector_Compaction.cpp:39
#4  CLR_RT_ExecutionEngine::PerformHeapCompaction () at src/CLR/Core/Execution.cpp:429
#5  Library_nf_rt_native_nanoFramework_Runtime_Native_GC::Run___STATIC__U4__BOOLEAN (stack=...) at src/nanoFramework.Runtime.Native/nf_rt_native_nanoFramework_Runtime_Native_GC.cpp:34
(gdb) frame 1
(gdb) print nextPtr
$1 = (const CLR_RT_HeapBlock_Node *) 0x70141
(gdb) x/4xw ptr
0x7ffff6f1cd28: 0x00010007      0x00000000      0x00070141      0x00000000
```

The id `0x00010007` is a `DATATYPE_I4` of one heap block; on x86-64 its data starts at byte 8, where `Next()` reads the value `0x70141`.

## Impact

Heap validation at level 3 or 4 is unusable with most applications: a compaction crashes the CLR or floods the output with false messages. Device builds leave it at level 0, so released firmware is not affected. CMake refuses to configure the heap memcheck build with it, see [HEAP-MEMCHECK.md, "Building"](../HEAP-MEMCHECK.md#building).

## Patch

None. The overlap check has to be limited to blocks whose first heap block really holds list links, for example free and event cache blocks, or it has to check that a link points into the heap before following it. Even then a quarantined block of the memcheck build, a free block on no list, has no links, so the quarantine stays incompatible with this check.
