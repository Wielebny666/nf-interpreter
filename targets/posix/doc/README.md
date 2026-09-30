# Heap memcheck research setup

## What it is

A setup for finding memory bugs in the native code of the nanoFramework CLR by running it as a Linux process. It consists of:

- the POSIX host build of the CLR ([targets/posix](..)), in an x86-64 and an i386 variant; the i386 one has the 12-byte `CLR_RT_HeapBlock` of the embedded targets;
- hooks in the allocator, the GC, compaction and the event cache that describe the managed heap to valgrind memcheck, so a native access to a freed object or to object memory nothing has written is reported where it happens;
- GC stress, compaction stress, a quarantine for freed objects and a self-test of the hooks;
- [HeapStress](../tests/HeapStress), a managed application that goes through as much of the CLR's native code as it can and checks every result;
- a dev container with every tool needed, and VS Code tasks and launch configurations to build, run and debug all of it.

How the tooling works in detail, including how memcheck is adapted to the heap blocks of the CLR, is in [HEAP-MEMCHECK.md](HEAP-MEMCHECK.md) (English) and [HEAP-MEMCHECK.pl.md](HEAP-MEMCHECK.pl.md) (Polish), kept in sync. Every build option, environment variable and harness option is listed in one place, in [HANDOFF.md, "Reference"](HANDOFF.md#reference-build-options-environment-variables-harness-options). The working notes of the GC/compaction bench and the Wire Protocol debugger on the POSIX host, including the run modes and every launch configuration, are in [HANDOFF.md](HANDOFF.md) (English) and [HANDOFF.pl.md](HANDOFF.pl.md) (Polish), kept in sync. How to build the VS Code extension with the debugger library fixes needed for the host is in [VSCODE-EXTENSION.md](VSCODE-EXTENSION.md) (English) and [VSCODE-EXTENSION.pl.md](VSCODE-EXTENSION.pl.md) (Polish), kept in sync.

## Goal

Find bugs in the shared CLR code (`src/`) that on a device show up only rarely: when a GC or a compaction happens at one particular allocation, or when memory that was freed has not been reused yet. On a device such a bug is a crash that cannot be reproduced; here GC stress makes the timing happen every time, and memcheck names the faulty access and where the memory came from.

The results are the [findings](heap-memcheck-findings/README.md): each one described with how it was found and the evidence, so that it can be turned into a fix for nanoFramework. What has not been examined yet is in [heap-memcheck-findings/TODO.md](heap-memcheck-findings/TODO.md).

## Getting started

### Dev container

Everything runs in the POSIX dev container, [.devcontainer/POSIX](../../../.devcontainer/POSIX): GCC with the 32-bit multilib, gdb, valgrind with the 32-bit libc symbols, the .NET SDK and Mono.

- VS Code: open the repository and use "Dev Containers: Reopen in Container", choosing the POSIX one. The image is built locally the first time.
- Without VS Code: `docker compose -f .devcontainer/POSIX/docker-compose.yml run --rm posix`.

The repository is mounted at `/workspace/nf-interpreter`.

### Build

The CMake presets are in [targets/posix/CMakePresets.json](../CMakePresets.json), not in the root one:

```bash
cd targets/posix
cmake --preset posix-x64-memcheck && cmake --build --preset posix-x64-memcheck   # build/posix64-memcheck
cmake --preset posix-x86-memcheck && cmake --build --preset posix-x86-memcheck   # build/posix32-memcheck
cd ../..
targets/posix/tests/HeapStress/build.sh                                          # build/heapstress
```

### Run

In VS Code (Run and Debug, and Terminal > Run Task):

| Name | What it does |
|---|---|
| launch "HeapStress x64 / x86 - native" | runs HeapStress under gdb; with GC stress a CLR bug is a crash and gdb stops there |
| launch "HeapStress x64 / x86 - valgrind + gdb" | runs HeapStress under valgrind with gdb attached through vgdb; gdb stops at every memcheck report |
| task "valgrind: heap self-test (x64)" | checks that the hooks report exactly the errors the self-test plants |
| task "valgrind: HeapStress report (x64 / x86)" | full run under valgrind, report in `build/vg-heapstress-<arch>.log` |

Each of them builds first and asks for the stress settings. `launch.json` also has groups for the Wire Protocol debugger builds (deployment over TCP into a simulated flash, `posix-x86-debugger` and `posix-x64-debugger`) and for the GC bench (`posix-x86` and `posix-x64`, each with `-debug` and `-soak`); they are described in [HANDOFF.md](HANDOFF.md), "Launch configurations". From the command line:

```bash
# natively: a crash means a bug
NANOCLR_GC_STRESS=1 build/posix64-memcheck/bin/nanoFramework.nanoCLR.test build/heapstress/refs build/heapstress/HeapStress.pe

# under valgrind: a report at the faulty access
NANOCLR_HEAP_SELFTEST=1 NANOCLR_GC_STRESS=50 \
valgrind --track-origins=yes --num-callers=30 --log-file=vg.log \
    build/posix64-memcheck/bin/nanoFramework.nanoCLR.test build/heapstress/refs build/heapstress/HeapStress.pe
```

HeapStress prints `HEAPSTRESS RESULT: PASS` or `FAIL` as its last line. The process then aborts with exit code 134; that is a known bug of the host (finding 4), not of the run.

## How to work with it

1. Run the self-test after any change to the allocator, the GC, compaction, the hooks or valgrind.
2. Run natively with `NANOCLR_GC_STRESS=1` first: it takes seconds and shows whether anything breaks.
3. Then run under valgrind with `NANOCLR_GC_STRESS=20`–`50`, and with quarantine and compaction stress for use-after-free bugs. Expect minutes to hours.
4. Find the cause with vgdb and gdb (see "Method" in the [findings README](heap-memcheck-findings/README.md)), confirm it with a temporary patch, and check the result on both x86-64 and i386.

### Writing up a finding

- One file per finding in [heap-memcheck-findings](heap-memcheck-findings), numbered, with Summary, Where, Cause, How it was found, Evidence, Impact (if there is any) and Patch, and a row in the table of its README.
- A `.patch` file is a verification patch: the change that confirmed the diagnosis, not a reviewed fix.
- Line numbers in shared code (`src/`) are those of `main` of nanoFramework/nf-interpreter at commit `2ff3e42b2` (2026-09-30), the sources without the memcheck hooks that a fix applies to; say which commit when adding a finding. Map every frame of a valgrind trace to them, and mark frames that exist only in the memcheck build ("memcheck build only: ...") instead of giving them a line. Code under `targets/posix` is cited as it is in this repository.
- Describe one bug per file. Refer to another only by what it is, not by its number, so that each file can go into a pull request on its own.
- Do not name the applications the tooling was run on, other than HeapStress; call them a production application.

### Fixing a finding

- Findings are not fixed in this repository. A fix is prepared separately against the nanoFramework sources and contains only that change.
- Keep the change minimal and without new code comments; the explanation belongs in the commit message and the pull request description.

## Layout

| Path | Contents |
|---|---|
| `targets/posix/doc/` | this description, [HEAP-MEMCHECK.md](HEAP-MEMCHECK.md) ([HEAP-MEMCHECK.pl.md](HEAP-MEMCHECK.pl.md)), the [findings](heap-memcheck-findings/README.md), the bench notes ([HANDOFF.md](HANDOFF.md), [HANDOFF.pl.md](HANDOFF.pl.md)) and building the VS Code extension ([VSCODE-EXTENSION.md](VSCODE-EXTENSION.md), [VSCODE-EXTENSION.pl.md](VSCODE-EXTENSION.pl.md)) |
| `targets/posix/tests/HeapStress/` | the stress application and its build script |
| `targets/posix/tests/GCCompactionSoak/` | an endless allocator churn with forced compactions, and its build script |
| `targets/posix/nanoCLR/HeapAnnotations.cpp` | GC and compaction stress, quarantine and the self-test |
| `targets/posix/Include/nanoCLR_HeapAnnotations_target.h` | the hooks as memcheck client requests |
| `src/CLR/Include/nanoCLR_HeapAnnotations.h` | the hooks in shared code, empty on every other target |
| `targets/posix/CMakePresets.json` | the `posix-x64-memcheck` and `posix-x86-memcheck` presets |
| `.devcontainer/POSIX/` | the dev container (`posix`) and two optional images, `builder` for managed projects and the VS Code extension and `full` with everything ([VSCODE-EXTENSION.md](VSCODE-EXTENSION.md#build-image-the-builder-service)), `flash-clean.sh` (blank the simulated flash) and `bench-serve.sh` (serve a CLR to a debugger in another container) |
| `.vscode/tasks.json`, `.vscode/launch.json` | build, run and debug |
