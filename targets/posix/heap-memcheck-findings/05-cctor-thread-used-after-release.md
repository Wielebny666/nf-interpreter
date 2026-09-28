# 5. Static-constructor thread used after release

## Summary

When the last static constructor has run, the static-constructor thread is destroyed from inside its own `Passivate()`. `Passivate()` then keeps reading and writing the thread object, which is already back in the event cache, and calls `ReleaseWhenDeadEx()` on it again.

## Where

- [src/CLR/Core/Thread.cpp](../../../src/CLR/Core/Thread.cpp), `CLR_RT_Thread::Passivate()` (line 455): calls the thread's termination callback (line 496), then reads `m_status` (line 499), writes `m_dlg` (line 502) and calls `ReleaseWhenDeadEx()` (line 505).
- [src/CLR/Core/Execution.cpp](../../../src/CLR/Core/Execution.cpp): the callback `StaticConstructorTerminationCallback` (line 805) calls `SpawnStaticConstructor(m_cctorThread)`, which, when no static constructor is left, ends with `pCctorThread->DestroyInstance()` (lines 926 and 1023, one per `NANOCLR_APPDOMAINS` variant).
- `CLR_RT_Thread::DestroyInstance()` → `Passivate()` (nested) → `ReleaseWhenDeadEx()` → `ReleaseWhenDead()` → `g_CLR_RT_EventCache.Append_Node(this)`.

## Cause

The termination callback destroys the thread that is being passivated. After it returns, the outer `Passivate()` has no way to know `this` has been released.

## How it was found

1. First HeapStress run after the event cache was annotated (`posix-x64-memcheck`, `NANOCLR_HEAP_SELFTEST=1`, no stress): six reports after the self-test, all on one object: two `Invalid read` at `Thread.cpp:499`, `Invalid write` at `Thread.cpp:502`, then reads in `ReleaseWhenDeadEx()` (`Thread.cpp:314`, `317`, `329`) and in `CLR_RT_DblLinkedList::IsEmpty()`.
2. The accesses before the callback (`Thread.cpp:458`–`486`) were not reported, so the release happened inside the callback.
3. `--vgdb-error=4` (past the self-test) and gdb at the first report: `g_CLR_RT_ExecutionEngine.m_cctorThread` was already null, pointing at the static-constructor thread; `SpawnStaticConstructor` ends in `DestroyInstance()`.

It is reported in every HeapStress run, on x86-64 and i386.

## Evidence

```
Invalid read of size 4
   at CLR_RT_Thread::Passivate() (Thread.cpp:499)
   by CLR_RT_ExecutionEngine::PutInProperList(CLR_RT_Thread*) (Execution.cpp:1373)
   by CLR_RT_ExecutionEngine::ScheduleThreads(int) (Execution.cpp:1281)
Invalid write of size 8
   at CLR_RT_Thread::Passivate() (Thread.cpp:502)
   by CLR_RT_ExecutionEngine::PutInProperList(CLR_RT_Thread*) (Execution.cpp:1373)
Invalid read of size 4
   at CLR_RT_Thread::ReleaseWhenDeadEx() (Thread.cpp:314)
   by CLR_RT_Thread::Passivate() (Thread.cpp:505)
```

## Impact

Harmless today: nothing is allocated between the release and the last access, so the cached block has not been handed out again. The second `ReleaseWhenDeadEx()` decides from the released memory whether to release the thread again; if it does, `Append_Node` unlinks the node before relinking it, so the cache list stays consistent. It becomes a real corruption as soon as anything between those points allocates from the event cache.

## Patch

None. Possible directions: let the callback only schedule the destruction and do it after `Passivate()` returns, or have `Passivate()` stop touching `this` once the callback has run for a thread that may be destroyed.
