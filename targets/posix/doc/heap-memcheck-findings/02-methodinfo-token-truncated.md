# 2. `MethodInfo` truncated by `SetReflection()`

## Summary

The native reflection code creates a `MethodInfo` object with two heap blocks (header and the `_token` field), then turns it into a reflection block with `SetReflection()`, which writes a size of **one** block into the header. The `_token` block becomes a separate block nothing refers to; the GC frees it, and `MethodBase.GetParameters()` later reads the method token from freed or reused memory.

## Where

- [src/CLR/CorLib/corlib_native_System_Type.cpp](../../../../src/CLR/CorLib/corlib_native_System_Type.cpp), `GetMethods` and `GetMethod`: `NewObjectFromIndex(..., m_MethodInfo)`, `SetReflection(...)`, then `hbObj[MethodBase::FIELD___token] = ...` (lines 677 and 770).
- [src/CLR/Core/CLR_RT_HeapBlock.cpp](../../../../src/CLR/Core/CLR_RT_HeapBlock.cpp), `CLR_RT_HeapBlock::SetReflection(const CLR_RT_MethodDef_Index &)` (line 276) and its overloads: `m_id.raw = CLR_RT_HEAPBLOCK_RAW_ID(DATATYPE_REFLECTION, 0, 1)`.
- Read in [src/CLR/CorLib/corlib_native_System_Reflection_MethodBase.cpp:214](../../../../src/CLR/CorLib/corlib_native_System_Reflection_MethodBase.cpp), `GetParametersNative`: `idx.m_data = hbMethodInfo[FIELD___token].NumericByRef().u4`.

## Cause

`SetReflection()` rewrites the whole data id, size included, so the object shrinks to its first block. A heap walk then sees the former `_token` field as a one-block object of its own (in gdb: data type 7, size 1, holding the token) that no reference reaches. The next GC frees it. Until the block is reused the token is still there, which is why `GetParameters()` usually works; once it is reused, the "token" is whatever the new object holds, `InitializeFromIndex` fails, and `Initialize_MethodSignature` dereferences a null method (`assm=0x0, md=0x0`, crash at address `0xe`). If the garbage happens to be a valid index, `GetParameters()` silently describes a different method.

The other `MethodBase` natives are not affected: they call `GetMethodDescriptor`, which reads the reflection data of the block itself.

## How it was found

1. A production application, with an unrelated earlier GC crash patched out, on `posix-x64-memcheck` with `NANOCLR_GC_STRESS=20`: a single `Invalid read` at address `0xe` in `Initialize_MethodSignature` from `GetParametersNative`, then a segfault. Nothing reported the earlier read of the freed field, because the block had already been reused. Natively the same crash at stress 1 and 20, never without stress.
2. gdb at the crash: `this` of `GetParametersNative` was `DATATYPE_REFLECTION` with size 1 and the block after it was an unrelated object; `idx` held 5.
3. gdb without stress, breakpoint in `GetParametersNative` with a printf of the reflection data and of the following block: the following block was a `DATATYPE_I4` block whose value equalled the method index in the reflection data. That shows the token lives outside the object.
4. Reading `GetMethods`/`GetMethod` and `SetReflection` confirmed the size overwrite.
5. HeapStress reproduces it natively in about a minute at `NANOCLR_GC_STRESS=20` (crash in round 2 or 3) on x86-64 and i386.
6. With `NANOCLR_HEAP_QUARANTINE=1` the freed `_token` block stays inaccessible, and memcheck reports the read at the exact line; HeapStress then passes, because the block has not been reused.

## Evidence

Crash (natively, `NANOCLR_GC_STRESS=20`):

```
Thread 1 "nanoFramework.n" received signal SIGSEGV, Segmentation fault.
#0  CLR_RT_SignatureParser::Initialize_MethodSignature (this=..., assm=0x0, md=0x0) at src/CLR/Core/TypeSystem.cpp:264
#1  Library_corlib_native_System_Reflection_MethodBase::GetParametersNative___SZARRAY_SystemReflectionParameterInfo (stack=...) at src/CLR/CorLib/corlib_native_System_Reflection_MethodBase.cpp:218
#2  CLR_RT_Thread::Execute_Inner () at src/CLR/Core/Interpreter.cpp:843
```

Under valgrind with quarantine:

```
Invalid read of size 4
   at Library_corlib_native_System_Reflection_MethodBase::GetParametersNative___SZARRAY_SystemReflectionParameterInfo(CLR_RT_StackFrame&) (corlib_native_System_Reflection_MethodBase.cpp:214)
   by CLR_RT_Thread::Execute_Inner() (Interpreter.cpp:843)
   by CLR_RT_Thread::Execute() (Interpreter.cpp:645)
```

## Patch

[02-methodinfo-token-truncated.patch](02-methodinfo-token-truncated.patch) makes `GetParametersNative` take the method from the reflection data through `GetMethodDescriptor`, like the other `MethodBase` natives, instead of reading `_token`. With it (and a fix for an unrelated GC crash in inlined calls) HeapStress passes under every stress setting on both builds.

For review: the patch removes the only reader of the orphaned field, but the object is still truncated and the orphaned block is still allocated and freed for every `MethodInfo`. A complete fix would either stop writing `_token` in `GetMethods`/`GetMethod`, or make `SetReflection()` keep the object's size when it is applied to a multi-block object.
