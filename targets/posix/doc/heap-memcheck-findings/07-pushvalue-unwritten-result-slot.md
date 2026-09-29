# 7. Unwritten result slot of a failing native method

## Summary

A native method that takes its result slot with `stack.PushValue()` and then fails before writing it leaves an uninitialised slot on the evaluation stack. The CLR's reaction to the failure is to allocate an exception, that allocation can run a GC, and the GC scans the slot as a reference. The case seen here is `BinaryFormatter.Serialize` on this host; the pattern is general.

## Where

- [src/System.Runtime.Serialization/nf_system_runtime_serialization_System_Runtime_Serialization_Formatters_Binary_BinaryFormatter.cpp:13](../../../../src/System.Runtime.Serialization/nf_system_runtime_serialization_System_Runtime_Serialization_Formatters_Binary_BinaryFormatter.cpp): `NANOCLR_SET_AND_LEAVE(CLR_RT_BinaryFormatter::Serialize(stack.PushValue(), stack.Arg0()))`.
- [src/CLR/Core/Serialization/BinaryFormatter_stub.cpp:290](../../../../src/CLR/Core/Serialization/BinaryFormatter_stub.cpp): the stub that this host links returns `CLR_E_NOTIMPL` through `NANOCLR_FEATURE_STUB_RETURN()` without touching `refData`.
- `CLR_RT_StackFrame::PushValue()` moves the top of the stack without initialising the slot; `PushValueAndClear()` is the variant that does.
- [src/CLR/Core/Interpreter.cpp:682](../../../../src/CLR/Core/Interpreter.cpp): on a failure code, `CLR_RT_Thread::Execute()` calls `Library_corlib_native_System_Exception::CreateInstance`, which allocates.

## Cause

`PushValue()` makes the slot part of the scanned stack before anything is written to it. Every native method that can return an error between `PushValue()` and the write has the same window; here it is the unconditional error of the stub.

## How it was found

1. HeapStress with every setting on under valgrind, with the verification patches for findings 1 and 2 applied: `Conditional jump ... uninitialised` in `ComputeReachabilityGraphForMultipleBlocks` from `Thread_Mark`, with the origin in `CLR_RT_StackFrame::Push` through `CLR_RT_EventCache::Extract_Node_Fast`. The GC ran inside `Library_corlib_native_System_Exception::CreateInstance`, called from `CLR_RT_Thread::Execute()`.
2. A gdb Python loop over vgdb continued past earlier reports until the stack contained `Thread_Mark`. The scanned frame was the native `BinaryFormatter.Serialize` (`System.Runtime.Serialization`) with `TopValuePosition() == 1` and `numLocals == 0`.
3. Reading the native method and the stub gave the `PushValue()` pattern.

The event cache annotation is what exposes it: the frame came from the cache, whose blocks are now marked uninitialised when handed out. Seen on x86-64 and i386.

## Evidence

```
Conditional jump or move depends on uninitialised value(s)
   at CLR_RT_GarbageCollector::ComputeReachabilityGraphForMultipleBlocks(CLR_RT_HeapBlock*, unsigned int) (GarbageCollector_ComputeReachabilityGraph.cpp:184)
   by CLR_RT_GarbageCollector::Thread_Mark(CLR_RT_Thread*) (GarbageCollector.cpp:794)
   by CLR_RT_GarbageCollector::Mark() (GarbageCollector.cpp:435)
   by CLR_RT_GarbageCollector::ExecuteGarbageCollection() (GarbageCollector.cpp:199)
   by CLR_RT_ExecutionEngine::PerformGarbageCollection() (Execution.cpp:404)
   by NanoCLR_HeapStress_BeforeAllocation(unsigned int) (HeapAnnotations.cpp)
   by CLR_RT_ExecutionEngine::ExtractHeapBlocks(...) (memcheck build only: GC stress hook)
   by CLR_RT_ExecutionEngine::ExtractHeapBlocksForArray(...) (Execution.cpp:1500)
   by CLR_RT_HeapBlock_Array::CreateInstance(...) (CLR_RT_HeapBlock_Array.cpp:80)
   by Library_corlib_native_System_Exception::SetStackTrace(CLR_RT_HeapBlock&, CLR_RT_StackFrame*) (corlib_native_System_Exception.cpp:222)
   by Library_corlib_native_System_Exception::CreateInstance(...) (corlib_native_System_Exception.cpp:142)
   by CLR_RT_Thread::Execute() (Interpreter.cpp:682)
 Uninitialised value was created by a client request
   at CLR_RT_EventCache::Extract_Node_Fast(unsigned int, unsigned int, unsigned int) (memcheck build only: event cache annotation)
   by CLR_RT_EventCache::Extract_Node(unsigned int, unsigned int, unsigned int) (Cache.cpp:585)
   by CLR_RT_StackFrame::Push(CLR_RT_Thread*, CLR_RT_MethodDef_Instance const&, int) (CLR_RT_StackFrame.cpp:80)
```

## Impact

On this host it happens every time `BinaryFormatter.Serialize` is called and the exception allocation runs a GC. On other targets it affects any native method with the same shape that fails after `PushValue()`; which of them do was not surveyed.

## Patch

None. For the case found: the stub, or the native method, can clear the slot (`PushValueAndClear()` or `refData.SetObjectReference(NULL)`) before failing. For the pattern: a check of every native method that calls `PushValue()` and can return an error before writing the value, or making the CLR clear the result slot of a native frame that returns an error.
