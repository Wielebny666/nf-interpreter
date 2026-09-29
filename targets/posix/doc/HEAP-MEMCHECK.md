# Checking the managed heap with valgrind

A build of the POSIX host that tells valgrind's memcheck what lives inside the managed heap, plus GC and compaction stress modes, a quarantine for freed objects, a self-test and a stress application that exercises as much of the CLR's native code as possible.

## What it finds

In a normal build the managed heap is one large `malloc` block to valgrind. The CLR places objects inside it itself, so every byte of the heap looks valid and initialised, and memcheck cannot see errors inside it.

With `NANO_POSIX_HEAP_MEMCHECK` the allocator and the GC keep memcheck informed:

| Heap block | What memcheck sees |
|---|---|
| new object | uninitialised until the CLR writes it |
| free block | first heap block (header and free-list links) accessible, the rest **inaccessible** |
| block parked in the event cache (stack frames, lock requests, wait objects, sub-threads...) | like a free block; uninitialised again when the cache hands it out |
| object freed in the last GC, with quarantine on | only the 4-byte header accessible, kept out of the free list until the next GC |
| object moved by compaction | its initialised state moves with the data |

That makes two kinds of bug in the CLR's native code visible:

| Bug | memcheck report |
|---|---|
| native code uses an object after the GC freed it, because nothing rooted it (missing `CLR_RT_ProtectFromGC`, raw pointer kept across an allocation), or an event-cache block after releasing it | `Invalid read` / `Invalid write` |
| a decision taken on object memory nothing has written, for example the GC marking through an uninitialised stack-frame slot or field | `Conditional jump or move depends on uninitialised value(s)`, `Use of uninitialised value` |

Errors in native memory outside the managed heap are reported as with any valgrind run.

## Building

The POSIX dev container, [.devcontainer/POSIX](../../../.devcontainer/POSIX/devcontainer.json), has everything this page needs: the 32-bit toolchain, gdb, valgrind with the 32-bit libc symbols, and the .NET SDK and Mono for the stress application. In VS Code, the tasks and launch configurations in `.vscode` build the preset and the stress application and run it natively, under valgrind, or under valgrind with gdb attached.

```bash
cd targets/posix
cmake --preset posix-x64-memcheck
cmake --build --preset posix-x64-memcheck
```

The preset inherits `posix-x64-debugger` (Debug with the Wire Protocol debugger) and builds into `build/posix64-memcheck`. It sets the CMake option `NANO_POSIX_HEAP_MEMCHECK`, which needs the valgrind headers (`valgrind/memcheck.h`, package `valgrind` on Debian and Ubuntu).

`posix-x86-memcheck` is the same for i386 (`build/posix32-memcheck`). That build has the 12-byte `CLR_RT_HeapBlock` of the embedded targets, so it is the one closest to a device. It needs the 32-bit multilib toolchain like the other `posix-x86` presets, and `libc6-dbg:i386` for valgrind. Everything below works the same with either build.

The binary also runs without valgrind. The annotations are then a few no-op instructions each.

## Environment variables

The memcheck build reads `NANOCLR_HEAP_SELFTEST` ([Self-test](#self-test)), `NANOCLR_GC_STRESS` ([GC stress](#gc-stress)), `NANOCLR_COMPACT_STRESS` ([Compaction stress](#compaction-stress)) and `NANOCLR_HEAP_QUARANTINE` ([Quarantine](#quarantine)), on top of the variables every build reads, such as `NANOCLR_HEAP_SIZE_MB`. All of them, with their exact values, are listed in one place: [HANDOFF.md, "Reference"](HANDOFF.md#reference-build-options-environment-variables-harness-options).

## Self-test

The self-test checks that the annotations work: it plants known errors in the heap and expects memcheck to report exactly those. Run it after changing the allocator (`CLR_RT_HeapCluster`), the GC, compaction or the hooks themselves, and after updating valgrind.

At the first managed allocation, `RunSelfTest()` in [nanoCLR/HeapAnnotations.cpp](../nanoCLR/HeapAnnotations.cpp):

1. **Read after free.** Creates a string held only in a native `CLR_RT_HeapBlock` the GC cannot see, runs a GC, which frees the string, and reads one of its characters back. The text lies past the first heap block of the object, so it is inaccessible once freed. Expected: `Invalid read of size 1`.
2. **Read of a freed object's first block, quarantine only.** Reads the last byte of the string's first heap block, which a free-list node would otherwise keep accessible. Expected: `Invalid read of size 1`.
3. **Uninitialised object memory.** Allocates an object without zeroing it, never writes it, and branches on its first byte. Expected: `Conditional jump or move depends on uninitialised value(s)`, with the origin `created by a client request at CLR_RT_HeapCluster::ExtractBlocks`.
4. **Read after release to the event cache.** Takes a node from the event cache, releases it the way a stack frame or a lock request is released, and reads it. Expected: `Invalid read of size 1`.

That is three errors, or four with `NANOCLR_HEAP_QUARANTINE=1`. The objects are ordinary garbage afterwards and the program carries on normally, so the self-test can be added to any run. Any PE v1 application will do; the command below uses the stress application, built as described in [Stress application](#stress-application):

```bash
NANOCLR_HEAP_SELFTEST=1 \
valgrind --track-origins=yes --num-callers=8 --log-file=vg-selftest.log \
    ./build/posix64-memcheck/bin/nanoFramework.nanoCLR.test $(cat build/heapstress/pe-files.txt)
```

The log brackets the errors with markers, the first of which states how many to expect (shortened):

```
**PID** nanoCLR heap self-test: expect 3 errors from HeapAnnotations.cpp
Invalid read of size 1
   at RunSelfTest() (HeapAnnotations.cpp)
   by NanoCLR_HeapStress_BeforeAllocation(unsigned int) (HeapAnnotations.cpp)
   by CLR_RT_ExecutionEngine::ExtractHeapBlocks(...) (Execution.cpp)
 Address 0x... is 138 bytes inside a block of size 10,485,760 alloc'd
   at malloc
   by HeapLocation(unsigned char*&, unsigned int&) (Memory.cpp)
Conditional jump or move depends on uninitialised value(s)
   at RunSelfTest() (HeapAnnotations.cpp)
   ...
 Uninitialised value was created by a client request
   at CLR_RT_HeapCluster::ExtractBlocks(unsigned int, unsigned int, unsigned int) (CLR_RT_HeapCluster.cpp)
   by CLR_RT_ExecutionEngine::ExtractHeapBlocksForObjects(...) (Execution.cpp)
   by RunSelfTest() (HeapAnnotations.cpp)
Invalid read of size 1
   at RunSelfTest() (HeapAnnotations.cpp)
   ...
**PID** nanoCLR heap self-test: done
```

| Result | Meaning |
|---|---|
| the expected number of errors between the markers, all in `RunSelfTest()` | the annotations work |
| no `Invalid read` from the first case | free blocks are not being made inaccessible: check the `FREE` hooks in `RecoverFromGC`, and that the build defines `NANOCLR_HEAP_ANNOTATIONS` |
| no `Invalid read` from the quarantine case | check the `QUARANTINE` hook in `RecoverFromGC` |
| no `Conditional jump` | new objects are not being marked uninitialised: check the `ALLOC` hook in `ExtractBlocks` |
| no `Invalid read` from the event-cache case | check the `FREE` hook in `CLR_RT_EventCache::Append_Node` |
| no markers in the log | the binary is not from `build/posix64-memcheck`, or `NANOCLR_HEAP_SELFTEST` is not set |
| further errors between the markers | false positives from the allocator or the GC itself, i.e. a bug in the hooks |
| errors after `self-test: done` | unrelated to the self-test: findings in the application or the CLR |

A scripted check, for CI:

```bash
section() { sed -n '/self-test: expect/,/self-test: done/p' vg-selftest.log; }
want=$(section | grep -oE 'expect [0-9]+ errors' | grep -oE '[0-9]+')
n=$(section | grep -cE '^==[0-9]+== (Invalid read|Conditional jump)')
at=$(section | grep -A1 -E '^==[0-9]+== (Invalid read|Conditional jump)' | grep -c 'RunSelfTest()')
[ -n "$want" ] && [ "$n" -eq "$want" ] && [ "$at" -eq "$want" ] && echo "heap self-test OK" \
    || { echo "heap self-test FAILED"; exit 1; }
```

## GC stress

`NANOCLR_GC_STRESS=<n>` runs a full GC before every n-th managed allocation. It is the GC an allocation runs anyway when the heap is full, so anything it finds is a real bug; without stress such a bug only shows when the heap happens to be full at the wrong moment, which is rare and not repeatable.

| Value | Use |
|---|---|
| `1` | a GC at every allocation; most thorough, very slow under valgrind |
| `20`–`100` | a reasonable compromise under valgrind |
| unset or `0` | no extra collections |

Stress skips allocations flagged `HB_NoGcOnFailedAllocation` and allocations made during a GC, where a collection must not run, and it does not start a GC from inside one (`PerformGarbageCollection` allocates the finalizer thread).

Stress also works without valgrind. A bug then shows as a crash instead of a report, but much sooner, which is a quick way to see whether a problem is there at all.

## Compaction stress

The CLR never compacts inside an allocation, only at a safe point of the scheduler loop, and on its own it rarely compacts at all. `NANOCLR_COMPACT_STRESS=<n>` schedules a compaction after every n-th stress GC (so it needs `NANOCLR_GC_STRESS`); the CLR runs it at the next safe point, exactly like a compaction it schedules itself. Live objects then move often, and native code that keeps a raw pointer to one across calls, where `Relocate` does not update it, is left holding the old address. That address is a free block after the move, so memcheck reports the next access.

Several collections between two safe points still make one compaction, so `1` means "as often as the CLR can", which is about one compaction per scheduler pass. (The harness options `--forcegc` and `--compactionaftergc` are not forwarded to the CLR on this host, so they do not do this.)

## Quarantine

A freed block usually goes back to the allocator at once, so a stale pointer soon lands in a new, valid object and memcheck has nothing to report. With `NANOCLR_HEAP_QUARANTINE=1` the objects that die in a collection are kept out of the free list until the next one: `RecoverFromGC` gathers them into separate free blocks that are not linked, with everything but their 4-byte header inaccessible (heap walks read nothing else). The next collection releases them normally.

The quarantine needs more heap. If allocations start failing, raise `NANOCLR_HEAP_SIZE_MB`.

## Stress application

Memcheck only finds bugs in code that actually runs. A program that only churns the allocator goes through almost none of the CLR's native code and finds nothing, even at `NANOCLR_GC_STRESS=1`. [tests/HeapStress](../tests/HeapStress) is a managed application written to cover as much native code as it can, and to check every result so that corruption that does not crash still shows up.

| Module | Covers |
|---|---|
| String, Text, Number | `System.String` natives, `StringBuilder`, UTF-8, parsing and formatting, `Convert`, `BitConverter`, `DateTime`/`TimeSpan`, `Guid`, `Random`, `System.Math` |
| Array, Collection | numeric, reference, struct and jagged arrays, `Array.Copy`/`Clear`/`IndexOf`/`CreateInstance`, `ArrayList`, the native `Hashtable` (colliding keys, removal, enumeration, clone), `Queue`, `Stack` |
| ObjectModel, Delegate, Exception | virtual/abstract/interface dispatch, static constructors, boxing, indexers, `ref`/`out`/`params`, enum flags, static/instance/closure/multicast delegates, events, managed and CLR exceptions, nesting deep enough to make the CLR clip its nested exception records |
| Reflection | `GetMethods`, `GetParameters`, `MethodInfo.Invoke`, fields, constructors, attributes, assembly and type lookup, `Type` as a `Hashtable` key |
| Threading, Timer | threads with priorities, `lock`, `Interlocked`, `AutoResetEvent`/`ManualResetEvent`, `WaitAny`/`WaitAll`, `Thread.Abort`, `CancellationTokenSource`, periodic and one-shot timers |
| GC | finalizers, resurrection, `SuppressFinalize`/`ReRegisterForFinalize`, weak references, a long linked list interleaved with garbage that must survive compaction |
| Stream, Serialization, Json | `MemoryStream`, `StreamReader`/`StreamWriter`, CRC32, `BinaryFormatter`, nanoFramework.Json (reflection-heavy) |
| Event, Socket, RuntimeNative | managed events through the native `EventSink`, `WeakDelegate`, TCP echo and UDP over loopback from several threads, `SystemInfo`, `GC.Run`, `ExecutionConstraint` |

It runs every module for five rounds and forces a compacting GC between rounds. The last line it prints is `HEAPSTRESS RESULT: PASS` or `HEAPSTRESS RESULT: FAIL (<n> failed checks)`. Other lines start with `HEAPSTRESS`, except the CLR's own dump of every exception thrown (lines starting with `++++`), which includes the exceptions the application throws on purpose.

| Prefix | Meaning |
|---|---|
| `HEAPSTRESS FAIL` | a check failed; counted |
| `HEAPSTRESS KNOWN` | a check that fails for a reason already known; reported, not counted |
| `HEAPSTRESS KNOWN ISSUE NOW PASSES` | a known issue has gone away; turn that check into a normal one |
| `HEAPSTRESS SKIP` | a feature this host does not implement |

### Building it

The application builds on Linux without Visual Studio, with Roslyn from the .NET SDK and the nanoFramework MetadataProcessor run under Mono:

```bash
sudo apt-get install dotnet-sdk-8.0 mono-complete     # once; already in the dev container
targets/posix/tests/HeapStress/build.sh
```

The script fetches the packages listed in `packages.config` from NuGet, compiles, and converts the result to `build/heapstress/HeapStress.pe`. It also writes `build/heapstress/pe-files.txt`, the full list of `.pe` files in load order. The package versions are pinned: their native checksums must match the native code compiled into this target, and a package with a different checksum fails to load. This build loads PE v1 (`NFMRK1`) only, so the script uses MetadataProcessor 3.x.

`HeapStress.nfproj` builds the same sources in Visual Studio or VS Code with the nanoFramework extension.

### Running it

```bash
# natively, quick check that it passes
./build/posix64-memcheck/bin/nanoFramework.nanoCLR.test $(cat build/heapstress/pe-files.txt)

# natively with GC stress: a CLR bug shows up as a crash
NANOCLR_GC_STRESS=1 ./build/posix64-memcheck/bin/nanoFramework.nanoCLR.test $(cat build/heapstress/pe-files.txt)

# under valgrind: a CLR bug shows up as a report at the faulty access
NANOCLR_HEAP_SELFTEST=1 NANOCLR_GC_STRESS=50 \
valgrind --track-origins=yes --num-callers=30 --log-file=vg.log \
    ./build/posix64-memcheck/bin/nanoFramework.nanoCLR.test $(cat build/heapstress/pe-files.txt)

# everything at once: quarantine, and a compaction after every stress GC
NANOCLR_HEAP_SELFTEST=1 NANOCLR_HEAP_QUARANTINE=1 NANOCLR_GC_STRESS=20 NANOCLR_COMPACT_STRESS=1 \
valgrind --track-origins=yes --num-callers=30 --log-file=vg.log \
    ./build/posix64-memcheck/bin/nanoFramework.nanoCLR.test $(cat build/heapstress/pe-files.txt)
```

To see only the application's own output, filter with `grep ^HEAPSTRESS`.

## Reading a report

The address description always reads `... bytes inside a block of size <heap size> alloc'd ... HeapLocation (Memory.cpp)`, because the whole heap is one `malloc`. It says nothing about the object. What matters is:

- **the call stack of the error**: which native code touched the memory;
- for uninitialised values, **`Uninitialised value was created by a client request`** with the allocation stack: `ExtractBlocks` ← the function that allocated the object, for example `CLR_RT_StackFrame::Push` for stack frames or `NewObject` for class instances.

When the stack is not enough, stop at the error (`--vgdb=yes --vgdb-error=1`, then `gdb` with `target remote | vgdb`) and inspect the state: which stack frame and slot the GC is scanning, or the type and size of the object (`ptr->DataType()`, `ptr->DataSize()`).

### Findings

Everything found with this tooling so far, with how each was found and a verification patch where one exists, is in [heap-memcheck-findings](heap-memcheck-findings/README.md). Those files are also worked examples of reading a report.

## Limitations

- **Quarantine lasts one collection.** A stale pointer used only after the next GC can land in a new object and go unreported.
- **Neighbouring objects.** There are no red zones between objects, so a write past the end of an object into a **live** neighbour is not detected; only writes into free, quarantined or cached blocks are.
- **Relocation.** A pointer the CLR relocates to a wrong but valid address is not detected; only pointers left at the old address are.
- **Inline frame buffers.** `CLR_RT_InlineBuffer` records live in a static array, not in the heap, and are not annotated.

Native memory outside the managed heap needs no annotations: on this host `platform_malloc` is plain `malloc`, so memcheck checks it like any other allocation, leaks included.

## How it is built

The shared code only has hooks. Without `NANOCLR_HEAP_ANNOTATIONS` they are empty macros, so nothing changes on other targets.

| Hook | Where | Meaning |
|---|---|---|
| `NANOCLR_HEAP_ANNOTATE_ALLOC` | `CLR_RT_HeapCluster::ExtractBlocks` | a new object, uninitialised |
| `NANOCLR_HEAP_ANNOTATE_UNFREE` | `ExtractBlocks` (before splitting a free block), `Heap_Compact` (before `memmove`), `CLR_RT_EventCache::Extract_Node_Fast`/`_Slow` | the inside of a free or cached block becomes writable and uninitialised; the header and links stay |
| `NANOCLR_HEAP_ANNOTATE_FREE` | `RecoverFromGC` (sweep), `InsertInOrder`, the remainder in `ExtractBlocks`, `Heap_Compact`, `CLR_RT_EventCache::Append_Node` | a free or cached block: header and links accessible, the rest inaccessible |
| `NANOCLR_HEAP_ANNOTATE_QUARANTINE` | `RecoverFromGC` | a quarantined block: only the header accessible |
| `NANOCLR_HEAP_ANNOTATE_RELINK` | `RecoverFromGC` | the first block of a run becomes writable before it is turned into a free-list node |
| `NANOCLR_HEAP_QUARANTINE_ENABLED` | `RecoverFromGC` | whether dying objects are quarantined; `false` when annotations are off, so the code compiles away |
| `NANOCLR_HEAP_STRESS_BEFORE_ALLOCATION` | `CLR_RT_ExecutionEngine::ExtractHeapBlocks` | GC and compaction stress, and the self-test |

| File | Contents |
|---|---|
| `src/CLR/Include/nanoCLR_HeapAnnotations.h` | the hooks and their empty defaults |
| `targets/posix/Include/nanoCLR_HeapAnnotations_target.h` | the hooks as memcheck client requests (`VALGRIND_MAKE_MEM_*`) |
| `targets/posix/nanoCLR/HeapAnnotations.cpp` | GC and compaction stress, quarantine switch and the self-test |
| `targets/posix/nanoCLR/CMakeLists.txt` | the `NANO_POSIX_HEAP_MEMCHECK` option |
| `targets/posix/CMakePresets.json` | the `posix-x64-memcheck` and `posix-x86-memcheck` presets |
| `targets/posix/tests/HeapStress/` | the stress application and its build script |
| `targets/posix/doc/heap-memcheck-findings/` | the findings, how they were found, and verification patches |
| `.devcontainer/POSIX/` | the dev container with every tool above |
| `.vscode/tasks.json`, `.vscode/launch.json` | build, self-test, valgrind runs and debugging with gdb or vgdb |

## References

Valgrind 3.22 documentation; a local copy comes with the `valgrind` package in `/usr/share/doc/valgrind/html/`. The comments in `/usr/include/valgrind/memcheck.h` and `valgrind.h` are the most precise description of each request.

The client requests this build uses, and those to extend it with:

- [Memcheck client requests](https://valgrind.org/docs/manual/mc-manual.html#mc-manual.clientreqs): `VALGRIND_MAKE_MEM_NOACCESS/UNDEFINED/DEFINED`, `VALGRIND_CHECK_MEM_IS_*`, `VALGRIND_CREATE_BLOCK`
- [Memory pools](https://valgrind.org/docs/manual/mc-manual.html#mc-manual.mempools): `VALGRIND_CREATE_MEMPOOL`, `VALGRIND_MEMPOOL_ALLOC/FREE/CHANGE`, which would give each object its own "alloc'd at / freed at" in reports
- [Core client requests](https://valgrind.org/docs/manual/manual-core-adv.html#manual-core-adv.clientreq): `VALGRIND_PRINTF`, `RUNNING_ON_VALGRIND`, `VALGRIND_DISABLE_ERROR_REPORTING`
- [How memcheck tracks memory](https://valgrind.org/docs/manual/mc-manual.html#mc-manual.machine): the A (addressable) and V (valid) bits behind "defined" and "undefined"
- [Memcheck monitor commands](https://valgrind.org/docs/manual/mc-manual.html#mc-manual.monitor-commands): `get_vbits`, `check_memory`, `who_points_at` from gdb through vgdb

Writing a separate valgrind tool, which instruments the code itself:

- [Writing a New Valgrind Tool](https://valgrind.org/docs/manual/manual-writing-tools.html)
- [Valgrind technical documentation](https://valgrind.org/docs/manual/tech-docs.html)

Without valgrind:

- [AddressSanitizer manual poisoning](https://github.com/google/sanitizers/wiki/AddressSanitizerManualPoisoning): `ASAN_POISON_MEMORY_REGION` / `ASAN_UNPOISON_MEMORY_REGION` from `<sanitizer/asan_interface.h>`. An ASan variant of the hooks would only need another `nanoCLR_HeapAnnotations_target.h` and runs far faster than valgrind, but ASan does not track uninitialised memory, so it would not see bugs like [finding 1](heap-memcheck-findings/01-pushinline-phantom-slot.md).
