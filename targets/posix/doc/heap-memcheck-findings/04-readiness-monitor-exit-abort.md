# 4. Host aborts at exit: `ReadinessMonitor` thread still joinable

## Summary

When a managed program that has used sockets ends, the POSIX host process aborts with `terminate called without an active exception` (exit code 134). The socket readiness monitor's `std::thread` is still joinable when its static owner is destroyed.

## Where

- [targets/posix/nanoCLR/Sockets_POSIX.cpp](../../nanoCLR/Sockets_POSIX.cpp): `class ReadinessMonitor` (line 141) owns `std::thread m_thread`; the global instance `ReadinessMonitor s_monitor` (line 293) has no destructor that stops it. `Stop()` joins the thread and is called only from `Network_Uninitialize()` (line 636).
- [targets/posix/nanoCLR/Various.cpp](../../nanoCLR/Various.cpp): `Network_Uninitialize()` is called only from `nanoHAL_Uninitialize()`.
- [targets/posix/nanoCLR/CLRStartup.cpp](../../nanoCLR/CLRStartup.cpp): `nanoHAL_Uninitialize()` runs only on a CLR-only soft reboot (around line 586), not when the program simply ends.

## Cause

On a normal exit `ClrStartup` returns without `nanoHAL_Uninitialize()`, the process exits, and the static destructors run. `std::thread::~thread` on a joinable thread calls `std::terminate`.

## How it was found

1. Every HeapStress run ended with `Aborted` / exit code 134 after `HEAPSTRESS RESULT: PASS`, with and without valgrind; `GCCompactionSoak`, which uses no sockets, never ends on its own so it never showed it.
2. gdb on a native run: the abort comes from `std::thread::~thread` called by `ReadinessMonitor::~ReadinessMonitor` from `__cxa_finalize` during unloading of `nanoFramework.nanoCLR.so`.

## Evidence

```
HEAPSTRESS RESULT: PASS
Thread 1 "nanoFramework.n" received signal SIGABRT, Aborted.
#7  std::terminate() () from /lib/x86_64-linux-gnu/libstdc++.so.6
#9  std::thread::~thread (this=<(anonymous namespace)::s_monitor+96>) at /usr/include/c++/13/bits/std_thread.h:173
#10 (anonymous namespace)::ReadinessMonitor::~ReadinessMonitor (this=<(anonymous namespace)::s_monitor>) at targets/posix/nanoCLR/Sockets_POSIX.cpp
#11 __cxa_finalize (d=...) at ./stdlib/cxa_finalize.c:82
#12 __do_global_dtors_aux () from build/posix64-memcheck/lib/nanoFramework.nanoCLR.so
```

## Impact

The managed program has finished by then, but a script or CI job sees exit code 134 instead of 0, and the abort leaves a core dump.

## Patch

None. Either `ReadinessMonitor` gets a destructor that calls `Stop()`, or the host calls `nanoHAL_Uninitialize()` on a normal exit as well.
