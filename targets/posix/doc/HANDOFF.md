# POSIX host - building, running and debugging

How to build and run the POSIX host of the CLR that the tools in this repository use, how to deploy an application to it over the Wire Protocol and debug it, and which launch configuration does what. The memory-bug tooling built on top of it is described in [README.md](README.md) and [HEAP-MEMCHECK.md](HEAP-MEMCHECK.md).

This is the English version of [HANDOFF.pl.md](HANDOFF.pl.md); the two are kept in sync.

## Why the POSIX host

- It runs the same CLR code as the devices. In
  `src/CLR/Include/nanoCLR_PlatformDef.h:96` the POSIX host takes the same branch
  as ARM and ESP32: no profiler, no memory statistics, heap validation only when
  asked for. The Windows virtual device (`VIRTUAL_DEVICE`) enables
  `NANOCLR_PROFILE_NEW*`, which instruments exactly the allocation paths the tools
  look at.
- The i386 build has the 12-byte `CLR_RT_HeapBlock` of the embedded targets; the
  x86-64 build is the control. Different word sizes over the same sources are a
  diagnostic tool.
- It builds and runs in a Linux container, under gdb and valgrind.

## Building

Builds use the presets in `targets/posix/CMakePresets.json`, **from the
`targets/posix` directory**: from the repo root, `cmake --preset ...` hits the board
presets and fails with `is not a directory`:

```bash
cd targets/posix
cmake --preset posix-x86-debugger && cmake --build --preset posix-x86-debugger
```

In VS Code the `cmake: build <preset>` and `prepare: ...` tasks do this, and every
launch configuration calls its task before it starts.

Only the debugger presets have `--networkport`, `--flashimage` and the other network
options; they sit behind `NANOCLR_ENABLE_SOURCELEVELDEBUGGING`. Any other build
rejects them as unknown arguments:

```
error: unexpected argument '--networkport' (expected an option, a .pe file or a directory)
```

If you see this, the binary was built without the debugger stack; it is not a
launch configuration error.

Every preset sets `CMAKE_BUILD_TYPE`; a configure without a preset builds at -O0,
because `targets/posix/CMakeLists.txt` sets no default.

| preset | directory | for |
|---|---|---|
| `posix-x86` | `build/posix32` | measurements, RelWithDebInfo |
| `posix-x86-debug` | `build/posix32-debug` | work under gdb |
| `posix-x86-soak` | `build/posix32-soak` | long runs, heap validation 3 |
| `posix-x86-debugger` | `build/posix32-wp` | Wire Protocol stack, debugger over TCP |
| `posix-x86-memcheck` | `build/posix32-memcheck` | heap described to valgrind, GC stress ([HEAP-MEMCHECK.md](HEAP-MEMCHECK.md)) |
| `posix-x64` | `build/posix64` | LP64 control |
| `posix-x64-debug` | `build/posix64-debug` | control under gdb |
| `posix-x64-soak` | `build/posix64-soak` | soak on LP64, heap validation 3 |
| `posix-x64-debugger` | `build/posix64-wp` | Wire Protocol stack on LP64, debugger over TCP |
| `posix-x64-memcheck` | `build/posix64-memcheck` | heap described to valgrind, GC stress |

Heap validation: `NANO_POSIX_VALIDATE_HEAP` 0-4, 0 for measurements, 3 for a soak.

Profiling: callgrind works unprivileged. `perf` is not in the image (the package is
tied to the container's kernel, not the host's); run it from the host against the
process in the container.

## Running: three modes

### 1. PE files

```bash
./build/posix32/bin/nanoFramework.nanoCLR.test build/heapstress/refs build/heapstress/HeapStress.pe
```

The harness takes `.pe` files and directories; a directory expands to its `.pe`
files, sorted, with `mscorlib.pe` first. The fastest edit-run loop.

`--forcegc` and `--compactionaftergc` **do nothing** on the POSIX host: `main.cpp`
sets them in `CLR_SETTINGS`, but `CLRStartup.cpp` calls
`CLR_RT_ExecutionEngine::CreateInstance()` without parameters, so they never reach
the engine. GC runs as on a device, when the heap runs out; compaction only when
the CLR schedules it itself. To force collections and compactions use a memcheck
build with `NANOCLR_GC_STRESS` and `NANOCLR_COMPACT_STRESS`.

### 2. Bare CLR, application deployed over Wire Protocol

```bash
./build/posix32-wp/bin/nanoFramework.nanoCLR.test \
    --networkport 26000 --host <address reachable by the debugger> \
    --waitfordebugger --loopafterexit
```

`--waitfordebugger` and `--loopafterexit` are what make this work (see
"Debugger over TCP"). Without them a CLR with nothing to run reports `a2000000`
and exits before anything can be deployed.

### 3. Bare CLR with a persistent flash

`--flashimage <path>` keeps the simulated flash in a file, so a deployment
survives a restart of the process:

```bash
# once, to put the application on the "device"
./build/posix32-wp/bin/nanoFramework.nanoCLR.test \
    --networkport 26000 --host <address> --waitfordebugger --loopafterexit \
    --flashimage ~/nanoclr-flash.img

# from then on, no debugger and no assemblies needed
./build/posix32-wp/bin/nanoFramework.nanoCLR.test --flashimage ~/nanoclr-flash.img
```

The second command prints "Loading Deployment Assemblies." and runs what was
deployed. The file is the whole flash, 2 MB (512 blocks of 4 KB,
`targets/posix/nanoCLR/Target_BlockStorage_Simulated.cpp`); one whose size does not
match is reported and started blank instead of being read at wrong offsets.
Without `--flashimage` the flash is blank at every start.

**Do not keep the image in `build/`.** That directory is deleted when switching
targets, and the image cannot be recreated from the repo. The launch
configurations use `/home/nano/nanoclr-flash.img`, the home directory of the dev
container.

### Blanking the flash and serving an instance

`.devcontainer/POSIX/flash-clean.sh` blanks the image:

```bash
.devcontainer/POSIX/flash-clean.sh            # blank the default image (NANOCLR_FLASH_IMAGE or ~/nanoclr-flash.img)
.devcontainer/POSIX/flash-clean.sh -s         # only say what is in it
.devcontainer/POSIX/flash-clean.sh <image>    # another file
```

It fills the image with 0xFF bytes in place instead of deleting the file: that is
what an erased chip looks like, and the file keeps its owner and permissions,
which matters for an image shared with another container. It refuses while a
running CLR holds the file open; use `-f` if that is really what you want. It only
sees processes of its own container.

Blanking the file alone is **not enough** to get a clean device: the CLR reads the
image once, at startup, and works on its in-memory copy afterwards. A reset has to
restart the instance. `.devcontainer/POSIX/bench-serve.sh [port] [address]` does
that: it keeps a `posix-x86-debugger` instance up for a debugger, possibly in
another container, and restarts it with a blank flash on request:

```bash
.devcontainer/POSIX/bench-serve.sh 26000 <address announced to the debugger>
touch ~/nanoclr-bench/reset-flash.request     # from anywhere that sees the state directory
```

After ~2 s the flash is blanked, the instance restarted and the state written to
`~/nanoclr-bench/serve-status.txt`. It also restarts an instance that died on its
own. `NANOCLR_BENCH_DIR` and `NANOCLR_FLASH_IMAGE` move the state directory and the
image.

### Do not mix mode 1 with mode 2

Passing PE files **and** having a deployment in the flash loads **both
copies**, with no priority and no warning. `CLR_RT_TypeSystem::Link`
(`src/CLR/Core/TypeSystem.cpp:3474`) puts the assembly in the first free slot
without checking the name, and `PostLinkageProcessing` overwrites
`m_assemblyMscorlib` unconditionally.

For heap work it is worse: deployed assemblies go **onto the managed heap as
`HB_Unmovable` blocks** (`targets/posix/nanoCLR/CLRStartup.cpp:340`), while the ones
from the command line live outside the heap, in host memory. A duplicated
deployment pins unmovable blocks in the middle of the heap.

## Launch configurations

`.vscode/launch.json` is divided into groups. The name of each configuration
ends with the preset whose binary it runs, in `[...]`; the comment above it says
what it is for. Every configuration has a preLaunchTask that builds its preset
first, and HeapStress where the configuration runs it.

### 1-2 HeapStress - `posix-x64-memcheck`, `posix-x86-memcheck`

Finding memory bugs in the CLR; described in [README.md](README.md).

| configuration | for |
|---|---|
| native | HeapStress under gdb; with GC stress a CLR bug is a crash, and gdb stops there |
| valgrind + gdb | HeapStress under valgrind; gdb, through vgdb, stops at every memcheck report |

### 3 Wire Protocol x86 - `posix-x86-debugger` (`build/posix32-wp`)

| configuration | for |
|---|---|
| bare CLR, wait for a deployment | an empty CLR waits for the debugger (modes 2+3); the deployed application goes into the flash image |
| run what was deployed | runs what is in the flash image (mode 3); a debugger can attach over TCP |
| run HeapStress, debugger port open | HeapStress with the debugger port open (`--loopafterexit`) |

### 4 Wire Protocol x64 - `posix-x64-debugger` (`build/posix64-wp`)

| configuration | for |
|---|---|
| bare CLR, wait for a deployment | as on x86, on LP64 |
| run what was deployed | as on x86, on LP64 |
| run what was deployed, valgrind + gdb | the task starts valgrind with the flash image and waits; gdb attaches through vgdb and stops at every error. No heap annotations: errors on the managed heap are caught by the memcheck configurations |

### 5 GC bench x86 - `posix-x86*`

| configuration | preset | for |
|---|---|---|
| run HeapStress | `posix-x86-debug` | HeapStress under gdb |
| --forcegc --compactionaftergc | `posix-x86-debug` | as above; the flags do not work (see mode 1) |
| break on Heap_Compact | `posix-x86-debug` | stops in `Heap_Compact` when the CLR schedules a compaction itself |
| measure, pick heap size | `posix-x86` | runs on the optimised build, heap size chosen at launch |
| soak, heap validation 3 | `posix-x86-soak` | long runs with heap validation; slow |
| smoke, no assemblies | `posix-x86-debug` | CLR start only: banner and exit `a2000000` |

### Parameters asked at launch

| input | meaning |
|---|---|
| `heapMb`, `vgHeapMb` | `NANOCLR_HEAP_SIZE_MB`; 10 is the host default, 32 matches NXP_MIMXRT1060_EVK |
| `networkPort`, `announceHost` (`vgNetworkPort`, `vgAnnounceHost`) | port and address of the debugger over TCP |
| `flashImage`, `vgFlashImage` | simulated flash file, `/home/nano/nanoclr-flash.img` by default |
| `wpTrace` | `NANOCLR_WP_TRACE`: hex dump of every Wire Protocol packet; 0 for timing runs |
| `gcStress`, `compactStress`, `quarantine` | memcheck stress settings, see [HEAP-MEMCHECK.md](HEAP-MEMCHECK.md) |

## Debugger over TCP

Presets `posix-x86-debugger` and `posix-x64-debugger`. The Wire Protocol carries
32-bit heap references: on i386 they are the pointers themselves, on x86-64 they
are offsets from the heap base (`src/CLR/Debugger/Debugger.cpp:30-33`).

The transport is in `targets/posix/nanoCLR/TcpWireProtocol.cpp` and belongs to
the harness, not to the CLR, as on the devices, where the managed nanoclr tool
plays that role.

The flags (`--networkport`, `--host`, `--broadcastport`, `--broadcastaddress`,
`--announceinterval`, `--waitfordebugger`, `--loopafterexit`, `--flashimage`) are
listed with their defaults in ["Reference"](#reference-build-options-environment-variables-harness-options).

Discovery is one UDP datagram `+:host:port`. One, not two: a second copy makes a
watcher see the same device arrive twice and run two validations against each
other.

### --loopafterexit is mandatory

**Without it the bare CLR dies before the debugger can deploy anything.** It is a
condition for the VS Code "launch" flow to work:

1. `Debugger_Discovery` (`src/CLR/Debugger/Debugger.cpp:130`) waits 5 seconds for a
   debugger. If the extension is not fast enough, the CLR prints
   `No debugger found...`.
2. The debugger connects a moment later and **resumes execution**; the launch flow
   does that, unlike a deploying client, which keeps the device stopped.
3. The CLR starts, has nothing to run, reports `Error: a2000000` and **ends the
   process** before the extension gets to the deployment.

The symptom looks like a deployment error but is a race at startup. With
`--loopafterexit` the CLR, after the same `Error: a2000000`, stays in
`Waiting for debug commands...`, accepts the deployment and loads all
assemblies after the reboot.

It also applies to a soft reboot: `targets/posix/nanoCLR/CLRStartup.cpp:582`
replaces `WaitForDebugger` with what the reboot asked for, so without the flag the
device disappears from the debugger in the middle of a session.

All configurations of the Wire Protocol groups in `.vscode/launch.json` have this flag.

### Transport logs

Every session ends with one summary line, **always**, regardless of the trace
(`TcpWireProtocol.cpp:107`):

```
[ 11621 ms] WP: connection #1 closed after 11621 ms | rx 52612 bytes in 411 packets
            | tx 10506 bytes in 208 packets | 14755516 polls | 4527 bytes/s in
```

`NANOCLR_WP_TRACE=1` adds a hex dump of every packet with a timestamp; `0` and
empty mean off. The `[N ms]` prefix on the `debugger connected` line is always
printed and does **not** mean the trace is on; the trace is the `WP rx:` and
`WP tx:` lines.

### VS Code extension

The nanoFramework extension can connect over TCP. It has no Device Explorer, so the
address goes directly into the extension's launch configuration as
`"device": "<address>:26000"`: the bridge `nanoFramework.Tools.DebugBridge.dll`
uses TCP when the string contains `:` and does not start with `COM`. Attach
sessions and breakpoints work. Deploying the application needs a `launch` entry
with `deployAssemblies: true`; `attach` deploys nothing.

The debugger library skips writing a region that already has the same content (it
compares the CRC through `Monitor_CheckMemory`), but still reboots the device and
reports success. A second deploy of the same application therefore takes a fraction
of the first and sends nothing. For timing a deployment, start from a blank flash.

## Interpreter and debugger thread

The debugger command handlers walk the thread list, stack frames and the
breakpoint table and allocate replies on the managed heap, on the Wire Protocol
thread, which runs beside the interpreter. On a device a command runs between
interpreter quanta; on the host a lock gives the same guarantee:

- the interpreter holds it for one `ScheduleThreads` batch (`src/CLR/Core/Execution.cpp:20-55`,
  an RAII guard, because the loop leaves through `NANOCLR_SET_AND_LEAVE`, i.e. a goto);
- the debugger holds it for one command, in `Messaging_ProcessPayload`
  (`src/CLR/Messaging/Messaging.cpp:453`), the funnel all commands go through;
- the lock itself is `targets/posix/nanoCLR/HostLock.cpp`, with a waiter count so
  that the interpreter does not take the lock back before the Wire Protocol thread
  gets it.

There is no deadlock, because every `WaitForDebugger` and `DebuggerLoop` lies
outside `ScheduleThreads`. The lock exists only in the debugger presets
(`PLATFORM_POSIX_HOST && NANOCLR_ENABLE_SOURCELEVELDEBUGGING`; `HostLock.cpp` is
built only with the debugger); the other builds contain no trace of it.

## Reference: build options, environment variables, harness options

Everything the host can be configured with, in one place.

### CMake options (`targets/posix/CMakeLists.txt`, `nanoCLR/CMakeLists.txt`)

| option | default | effect |
|---|---|---|
| `NANO_POSIX_ENABLE_SMOKE` | `ON` | builds the harness `nanoFramework.nanoCLR.test`; without it only the library is built |
| `NANO_POSIX_ENABLE_NETWORK` | `ON` | `System.Net` over the host's BSD sockets |
| `NANO_POSIX_ENABLE_DEBUGGER` | `OFF` (`ON` in the `-debugger` and `-memcheck` presets) | Wire Protocol debugger stack, TCP transport, simulated flash, `HostLock` |
| `NANO_POSIX_VALIDATE_HEAP` | `0` (`3` in the `-soak` presets) | heap validation level 0-4 |
| `NANO_POSIX_HEAP_MEMCHECK` | `OFF` (`ON` in the `-memcheck` presets) | heap annotations for valgrind, GC and compaction stress, quarantine, self-test; needs `valgrind/memcheck.h` |
| `NANO_POSIX_ARCH` | `arm64` | macOS only: `arm64` or `x86_64` |

### Environment variables

| variable | builds | effect |
|---|---|---|
| `NANOCLR_HEAP_SIZE_MB` | all | managed heap size in MB, 1-1024; anything else is ignored and the default of 10 is used (`nanoCLR/Memory.cpp`) |
| `NANOCLR_NETIF` | with network | host interface used as network interface 0; by default the first interface that is up and has an IPv4 address, loopback only if nothing else qualifies (`nanoCLR/NetworkConfiguration_POSIX.cpp`) |
| `NANOCLR_FLASH_IMAGE` | debugger | file holding the simulated flash; `--flashimage` sets it (`nanoCLR/Target_BlockStorage_Simulated.cpp`) |
| `NANOCLR_WP_TRACE` | debugger | hex dump of every Wire Protocol packet when set, non-empty and not `0` (`nanoCLR/TcpWireProtocol.cpp`) |
| `NANOCLR_GC_STRESS` | memcheck | full GC before every n-th managed allocation; 0 or unset is off |
| `NANOCLR_COMPACT_STRESS` | memcheck | heap compaction after every n-th stress GC, at the next safe point; needs `NANOCLR_GC_STRESS` |
| `NANOCLR_HEAP_QUARANTINE` | memcheck | a non-zero number keeps objects freed by a GC inaccessible until the next one |
| `NANOCLR_HEAP_SELFTEST` | memcheck | a non-zero number plants known heap errors at the first managed allocation |

The last four are in `nanoCLR/HeapAnnotations.cpp` and described in [HEAP-MEMCHECK.md](HEAP-MEMCHECK.md). The scripts in `.devcontainer/POSIX` also read `NANOCLR_FLASH_IMAGE` (default `~/nanoclr-flash.img`) and `bench-serve.sh` reads `NANOCLR_BENCH_DIR` (default `~/nanoclr-bench`).

### Harness options (`nanoFramework.nanoCLR.test`, `nanoCLR/main.cpp`)

`nanoFramework.nanoCLR.test [options] [--assemblies] <file.pe|directory> ...`

| option | builds | effect |
|---|---|---|
| `<file.pe>`, `<directory>` | all | assemblies to load, in the order given; a directory expands to its `.pe` files, sorted, `mscorlib.pe` first |
| `--assemblies` | all | accepted and ignored, for compatibility with the nanoclr tool; what follows is still files and directories |
| `--maxcontextswitches <n>` | all | scheduler quantum, 50 by default |
| `--forcegc` | all | parsed, but has no effect on this host (see mode 1) |
| `--compactionaftergc` | all | parsed, but has no effect on this host (see mode 1) |
| `--waitfordebugger` | all, useful with the debugger | wait for a debugger before running |
| `--loopafterexit` | all, useful with the debugger | stay in the debugger loop after the program ends |
| `--networkport <n>` | debugger | expose the debugger on TCP port n |
| `--host <address>` | debugger | address announced to debuggers, 127.0.0.1 by default |
| `--broadcastport <n>` | debugger | discovery port, 0 disables, 23657 by default |
| `--broadcastaddress <a>` | debugger | where to announce, 255.255.255.255 by default |
| `--announceinterval <s>` | debugger | repeat the announcement every s seconds, 0 for once, 5 by default |
| `--flashimage <path>` | debugger | keep the simulated flash in this file (sets `NANOCLR_FLASH_IMAGE`) |
| `-h`, `--help` | all | usage |

## Known issues of the host

1. **`--forcegc` and `--compactionaftergc` have no effect**, see mode 1.
2. **C++ standard.** The POSIX host is built as C++20
   (`targets/posix/nanoCLR/CMakeLists.txt:351` and `:459`), the ARM targets as
   C++23 (`CMake/toolchain.arm-none-eabi.cmake:54`).
3. **No warning about duplicated assemblies**, see "Do not mix mode 1 with mode 2".
4. **Exit code 134 after a program that used sockets**: finding
   [4](heap-memcheck-findings/04-readiness-monitor-exit-abort.md).
