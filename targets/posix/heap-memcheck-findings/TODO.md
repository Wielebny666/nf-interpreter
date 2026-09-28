# Still to analyse

What the findings so far do not cover. Ordered by how likely each item is to turn up something new, most likely first.

## Runs not done yet

- [ ] **A large real application with every mode on.** The production application went only through the first version of the tooling (no event cache annotation, quarantine or compaction stress), and its last run stopped at [finding 2](02-methodinfo-token-truncated.md). It has never been run to the end with the verification patches applied and `NANOCLR_HEAP_QUARANTINE=1 NANOCLR_GC_STRESS=20 NANOCLR_COMPACT_STRESS=1`. It goes through far more native code than HeapStress: networking, file system, interop.
- [ ] **A really full heap.** Stress forces collections but never exhausts the heap, so the out-of-memory paths have not run: the compact-and-restart path (`c_CompactAndRestartOnOutOfMemory` in `Interpreter.cpp`), the retry of an array allocation after a compaction, and the compaction scheduled after a failed allocation in `ExtractHeapBlocks`. Run HeapStress with a small `NANOCLR_HEAP_SIZE_MB`, or add a module that fills the heap on purpose.
- [ ] **The Wire Protocol debugger attached.** The memcheck presets build the debugger but it has never been connected. `Debugger.cpp` works with pointers into the heap, including the 64-bit handle table, and none of that code has run under the annotations. Attach a debugger, set breakpoints, inspect locals and objects, and step through HeapStress.
- [ ] **`NANOCLR_GC_STRESS=1` under valgrind.** Under valgrind only stress 20 and 50 were used; stress 1 ran only natively. Expect well over an hour per architecture.
- [ ] **Native leaks.** No run used `--leak-check=full`. Native memory needs no annotations (`platform_malloc` is `malloc` on this host), but nobody has looked at what leaks.
- [ ] **Races in the host's threads.** The POSIX host has worker threads (socket readiness monitor, DNS resolver, timers), and [finding 4](04-readiness-monitor-exit-abort.md) shows their lifetime handling has problems. Run helgrind or DRD.

## Code review

- [ ] **The `PushValue()` pattern of [finding 7](07-pushvalue-unwritten-result-slot.md).** `stack.PushValue()` without clearing appears 58 times in 29 files under `src/`. Check which of those native methods can return an error before they write the value.

## Open questions in existing findings

- [ ] **[Finding 3](03-enum-tostring-nre.md), `Enum.ToString()`:** root cause unknown. Check whether `GetType()` on a boxed enum returns the enum type or its underlying type, and whether the metadata processor keeps the `value__` field of enum types.
- [ ] **[Finding 6](06-spawnfinalizer-reentry.md), `SpawnFinalizer`:** not verified whether the finalizer really runs twice.

## Not covered by HeapStress

- [ ] `BinaryFormatter` (a stub on this host) and `ResourceManager` (needs resources embedded in the assembly).
- [ ] `Thread.Suspend` / `Thread.Resume`.
- [ ] An `ExecutionConstraint` that expires (`ConstraintException`).
- [ ] AppDomains (not compiled in).
- [ ] Persistent weak references (`WR_SurviveBoot`) across a soft reboot.
