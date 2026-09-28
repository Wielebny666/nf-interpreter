# 1. Phantom evaluation-stack slot in `PushInline`

## Summary

When the interpreter inlines a method, the inlined method starts with one evaluation-stack slot counted as occupied that nothing ever writes. Every GC during an inlined call scans that slot as a heap reference and follows whatever it contains.

## Where

- [src/CLR/Core/CLR_RT_StackFrame.cpp](../../../src/CLR/Core/CLR_RT_StackFrame.cpp), `CLR_RT_StackFrame::PushInline` (line 272): `evalPos++` (line 321) and `m_evalStackPos = evalPos + 1` (line 330).
- Scanned by `CLR_RT_GarbageCollector::Thread_Mark`, [GarbageCollector.cpp:794](../../../src/CLR/Core/GarbageCollector.cpp): `CheckMultipleBlocks(stack->m_evalStack, stack->TopValuePosition())`.
- Relocated by `CLR_RT_StackFrame::Relocate` during compaction, [CLR_RT_StackFrame.cpp:1261](../../../src/CLR/Core/CLR_RT_StackFrame.cpp): for an inlined frame it relocates one range from the caller's evaluation stack up to `m_evalStackPos`, which includes the slot.

## Cause

`PushInline` moves `evalPos` past the caller's arguments and makes that position the inlined method's `m_evalStack`, but sets `m_evalStackPos = evalPos + 1`. `TopValuePosition()` is therefore 1 while the inlined method's stack is empty, and `m_evalStack[0]` is never written by anything: it holds whatever the stack-frame memory held before. On a device that is stale data from an earlier frame, which is usually harmless (a dead object kept alive) but can be a pointer into a free or moved block; in a freshly carved frame it is garbage.

## How it was found

`NANOCLR_GC_STRESS=n` (memcheck build only) forces a full garbage collection before every n-th heap allocation.

1. A production application on `posix-x64-memcheck` under valgrind with `NANOCLR_GC_STRESS=20`: 65 errors in 10 contexts, all in `ComputeReachabilityGraphForMultipleBlocks` called from `Thread_Mark`: `Conditional jump or move depends on uninitialised value(s)` and `Use of uninitialised value`, then an `Invalid read` of a garbage address and a segfault.
2. `--vgdb-error=1` and gdb on the report: the scanned frame had `c_MethodKind_Inlined` set, `TopValuePosition() == 1`, `numLocals == 0`, and `m_evalStack` below `m_locals`, which only happens for an inlined frame. That led to `PushInline`.
3. The verification patch removed all of them; the application then ran on until an unrelated crash.
4. HeapStress reproduces it natively in about 2 seconds at `NANOCLR_GC_STRESS=1` (segfault in `Thread_Mark`), on x86-64 and i386. Under valgrind at `NANOCLR_GC_STRESS=50` it passes functionally but memcheck reports it: 23 errors in 7 contexts on x86-64, 56 in 15 on i386.

## Evidence

Line numbers are those of `main`; frames that exist only in the memcheck build are marked as such.

```
Conditional jump or move depends on uninitialised value(s)
   at CLR_RT_GarbageCollector::ComputeReachabilityGraphForMultipleBlocks(CLR_RT_HeapBlock*, unsigned int) (GarbageCollector_ComputeReachabilityGraph.cpp:184)
   by CLR_RT_GarbageCollector::Thread_Mark(CLR_RT_Thread*) (GarbageCollector.cpp:794)
   by CLR_RT_GarbageCollector::Thread_Mark(CLR_RT_DblLinkedList&) (GarbageCollector.cpp:832)
   by CLR_RT_GarbageCollector::Mark() (GarbageCollector.cpp:435)
   by CLR_RT_GarbageCollector::ExecuteGarbageCollection() (GarbageCollector.cpp:199)
   by CLR_RT_ExecutionEngine::PerformGarbageCollection() (Execution.cpp:404)
   by NanoCLR_HeapStress_BeforeAllocation(unsigned int) (HeapAnnotations.cpp)
   by CLR_RT_ExecutionEngine::ExtractHeapBlocks(...) (memcheck build only: GC stress hook)
   by CLR_RT_ExecutionEngine::ExtractHeapBlocksForClassOrValueTypes(...) (Execution.cpp:1532)
   by CLR_RT_ExecutionEngine::NewObject(...) (Execution.cpp:2078)
   by CLR_RT_ExecutionEngine::NewObjectFromIndex(...) (Execution.cpp:2022)
   by Library_corlib_native_System_Type::GetMethods(...) (corlib_native_System_Type.cpp:671)
 Uninitialised value was created by a client request
   at CLR_RT_HeapCluster::ExtractBlocks(unsigned int, unsigned int, unsigned int) (memcheck build only: allocation annotation)
   by CLR_RT_ExecutionEngine::ExtractHeapBlocks(...) (Execution.cpp:1649)
   by CLR_RT_ExecutionEngine::ExtractHeapBlocksForEvents(...) (Execution.cpp:1593)
   by CLR_RT_EventCache::Extract_Node_Fast(...) (Cache.cpp:558)
   by CLR_RT_EventCache::Extract_Node(...) (Cache.cpp:585)
```

The origin is the stack-frame allocation (`Extract_Node_Fast` called from `CLR_RT_StackFrame::Push`). `GetMethods` at the bottom of the first stack is only the allocation that triggered the stress GC; the scanned frame was an inlined call.

## Patch

[01-pushinline-phantom-slot.patch](01-pushinline-phantom-slot.patch) writes a null reference into the slot right after `evalPos++`, which covers both the mark and the relocation. It is the smallest change that makes the slot safe to scan. With it every report from this slot disappears and the `Thread_Mark` segfault no longer occurs on either build; a fully clean HeapStress run (`NANOCLR_GC_STRESS=1` natively, `NANOCLR_GC_STRESS=50` under valgrind) also needed a fix for an unrelated reflection bug.

For review: the alternative is to set `m_evalStackPos = evalPos` so the inlined method starts with an empty stack. That changes the convention the interpreter's local `evalPos` relies on after `PushInline` returns (it points at the phantom slot as the top of stack), and the return path of an inlined method would have to be checked with it, so it was not tried.
