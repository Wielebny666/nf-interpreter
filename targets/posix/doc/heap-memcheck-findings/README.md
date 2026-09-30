# Findings of the heap memcheck tooling

Bugs found with the heap-annotated POSIX build, GC and compaction stress, quarantine and the HeapStress application described in [../HEAP-MEMCHECK.md](../HEAP-MEMCHECK.md). None of them is fixed in this repository. Where a patch exists, it is a **verification patch**: the change used to confirm the diagnosis, which removed the reports and let HeapStress pass under stress. It has not been reviewed as the final fix.

Line numbers in shared code (`src/`) are those of `main` of nanoFramework/nf-interpreter at commit `2ff3e42b2` (2026-09-30), the sources without the memcheck hooks that a fix applies to; stack frames that exist only in the memcheck build are marked as such. Code under `targets/posix` is cited as it is in this repository.

What has not been analysed yet is listed in [TODO.md](TODO.md).

| # | Finding | Where | Severity | Patch |
|---|---|---|---|---|
| 1 | [Phantom evaluation-stack slot in `PushInline`](01-pushinline-phantom-slot.md) | shared CLR, interpreter | crash (GC follows garbage) | [01-pushinline-phantom-slot.patch](01-pushinline-phantom-slot.patch) |
| 2 | [`MethodInfo` truncated by `SetReflection()`](02-methodinfo-token-truncated.md) | shared CLR, CoreLib reflection | crash / wrong method | [02-methodinfo-token-truncated.patch](02-methodinfo-token-truncated.patch) |
| 3 | [`Enum.ToString()` throws `NullReferenceException`](03-enum-tostring-nre.md) | CLR reflection or metadata (root cause not found) | managed exception | none |
| 4 | [Host aborts at exit: `ReadinessMonitor` thread still joinable](04-readiness-monitor-exit-abort.md) | POSIX host sockets | abort at process exit | none |
| 5 | [Static-constructor thread used after release](05-cctor-thread-used-after-release.md) | shared CLR, threads | use after release, harmless today | none |
| 6 | [`SpawnFinalizer` re-entered through a GC](06-spawnfinalizer-reentry.md) | shared CLR, finalizers | use after release | none |
| 7 | [Unwritten result slot of a failing native method](07-pushvalue-unwritten-result-slot.md) | shared CLR natives (pattern), serialization stub | GC scans garbage | none |

Findings 1 and 2 reproduce identically on the x86-64 build and on the i386 build, which has the 12-byte `CLR_RT_HeapBlock` of the embedded targets, so they are not artefacts of a 64-bit host. Findings 5 to 7 were seen on both as well.

## Method

The same procedure produced every finding; each file says which of the steps and settings found it.

### 1. Make the heap visible to memcheck

The `posix-x64-memcheck` and `posix-x86-memcheck` presets build the CLR with `NANO_POSIX_HEAP_MEMCHECK`. The allocator, the sweep, compaction and the event cache then tell valgrind which bytes of the managed heap belong to live objects, which are free, and which have never been written. Before any other run, the self-test (`NANOCLR_HEAP_SELFTEST=1`) confirms that the annotations report exactly the errors it plants.

### 2. Run code that reaches the CLR's native paths

Memcheck only sees code that runs. A test that only churns the allocator ([GCCompactionSoak](../../tests/GCCompactionSoak)) found nothing even under full stress. What found the bugs was code that goes through many native methods: first a production application, then [HeapStress](../../tests/HeapStress), written for this purpose. It covers strings, numbers, collections, delegates, exceptions, reflection, threads, timers, finalizers, weak references, streams, JSON, serialization, events and sockets, and checks every result.

### 3. Make rare GC timings happen every time

A bug that needs a GC at one particular allocation shows up in the field only when the heap happens to be full at that moment. The stress settings force it:

| Setting | Effect |
|---|---|
| `NANOCLR_GC_STRESS=<n>` | full GC before every n-th allocation (`1` natively, `20`–`50` under valgrind) |
| `NANOCLR_COMPACT_STRESS=1` | a compaction after every stress GC, at the next safe point |
| `NANOCLR_HEAP_QUARANTINE=1` | freed objects stay inaccessible until the next GC, so a stale pointer cannot land in a new object |

### 4. Run natively first, then under valgrind

A native run with `NANOCLR_GC_STRESS=1` takes seconds and shows whether something breaks at all (a crash, or a `HEAPSTRESS FAIL`). The run under valgrind then reports the faulty access where it happens, with its call stack and, for uninitialised memory, the stack of the allocation it came from (`--track-origins=yes`).

### 5. Find the cause

- Stop at the error: `valgrind --vgdb=yes --vgdb-error=<n>` and `gdb` with `target remote | vgdb`. The self-test errors come first, so `<n>` skips them. A gdb Python loop that continues until the stack contains a given function (for example `Thread_Mark`) stops at the right report among many.
- Inspect the state at that point: which stack frame the GC is scanning and at which slot (`stack->m_call`, `TopValuePosition()`, `m_flags`), or the type and size of a heap block (`DataType()`, `DataSize()`).
- For a use after release, read the code between the last valid access and the failing one: in findings 5 and 6 the release happens inside a callback or an allocation called from the function that keeps using the object.

### 6. Confirm

- A hypothesis about a fix is confirmed with a temporary patch: the reports disappear and HeapStress passes under stress. Those patches are the `.patch` files here.
- A known bug is re-run with the setting that should turn its crash into a report; for example, with quarantine, finding 2 is reported at the exact line that reads the freed field.
- Every result is checked on both the x86-64 and the i386 build.

## Not bugs

Two behaviours showed up as failed checks in the first HeapStress runs but are properties of this host, not defects. HeapStress accounts for them.

- The native `Hashtable` compares keys with `CLR_RT_HeapBlock::ObjectsEqual`, not with a virtual `Equals` ([nf_system_collections_System_Collections_Hashtable.cpp](../../../../src/nanoFramework.System.Collections/nf_system_collections_System_Collections_Hashtable.cpp)), so a different key object with an overridden `Equals` is not found. HeapStress looks keys up through the stored instance.
- `BinaryFormatter` is not implemented on this host: the build links `BinaryFormatter_stub.cpp`, and `Serialize` throws `NotImplementedException`. HeapStress reports its serialization module as `SKIP`. (That stub is, however, the trigger of finding 7.)
