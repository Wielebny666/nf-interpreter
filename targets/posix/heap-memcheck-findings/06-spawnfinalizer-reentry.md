# 6. `SpawnFinalizer` re-entered through a GC

## Summary

`SpawnFinalizer` allocates a delegate for the first pending finalizer. If that allocation runs a GC, the GC ends by calling `SpawnFinalizer` again, which handles the same finalizer record and releases it to the event cache. The outer call then continues with the released record.

## Where

[src/CLR/Core/Execution.cpp](../../../src/CLR/Core/Execution.cpp):

- `CLR_RT_ExecutionEngine::SpawnFinalizer()` (line 1035): takes `fin = m_finalizersPending.FirstNode()`, calls `CLR_RT_HeapBlock_Delegate::CreateInstance(delegate, fin->m_md, NULL)` (line 1054), then reads `fin->m_object` and releases `fin` with `g_CLR_RT_EventCache.Append_Node(fin)` (line 1062).
- `CLR_RT_ExecutionEngine::PerformGarbageCollection()` calls `SpawnFinalizer()` after every collection (line 417).
- The delegate allocation goes through `ExtractHeapBlocks`, which runs a GC when the heap is full.

## Cause

`SpawnFinalizer` is not re-entrant, and it holds a raw pointer to the pending record across an allocation that can call it again.

## How it was found

1. HeapStress with every setting on (`NANOCLR_HEAP_SELFTEST=1 NANOCLR_HEAP_QUARANTINE=1 NANOCLR_GC_STRESS=20 NANOCLR_COMPACT_STRESS=1`) under valgrind, with the verification patches for findings 1 and 2 applied: `Invalid read` in `CLR_RT_HeapBlock_Delegate::CreateInstance` (`CLR_RT_HeapBlock_Delegate.cpp:53`, reading `fin->m_md` through the `ftn` reference) and in `SpawnFinalizer` (`Execution.cpp:1058`, `fin->m_object`), both from `FinalizerTerminationCallback` inside `Passivate()`.
2. Reading `SpawnFinalizer` against `PerformGarbageCollection` gave the re-entry path. The event cache annotation is what makes the released record inaccessible; GC stress is what puts a GC in that allocation.

Seen on x86-64 and i386.

## Evidence

```
Invalid read of size 4
   at CLR_RT_HeapBlock_Delegate::CreateInstance(CLR_RT_HeapBlock&, CLR_RT_MethodDef_Index const&, CLR_RT_StackFrame*) (CLR_RT_HeapBlock_Delegate.cpp:53)
   by CLR_RT_ExecutionEngine::SpawnFinalizer() (Execution.cpp:1054)
   by CLR_RT_ExecutionEngine::FinalizerTerminationCallback(void*) (Execution.cpp:1032)
   by CLR_RT_Thread::Passivate() (Thread.cpp:496)
   by CLR_RT_ExecutionEngine::PutInProperList(CLR_RT_Thread*) (Execution.cpp:1373)
Invalid read of size 8
   at CLR_RT_ExecutionEngine::SpawnFinalizer() (Execution.cpp:1058)
   by CLR_RT_ExecutionEngine::FinalizerTerminationCallback(void*) (Execution.cpp:1032)
   by CLR_RT_Thread::Passivate() (Thread.cpp:496)
```

## Impact

Needs a GC in exactly that allocation, which in the field means the heap is full when a finalizer is scheduled. The outer call then builds a delegate from the released record and, if the record memory has not been reused, schedules the same finalizer a second time and releases the record again. Whether the finalizer really runs twice was not verified.

## Patch

None. Possible directions: unlink the record from `m_finalizersPending` before the allocation, keep the method and object in locals (the object protected from the GC), or guard `SpawnFinalizer` against re-entry.
