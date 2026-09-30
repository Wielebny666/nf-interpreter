# Building the VS Code extension with the patched debugger library

How to build the nanoFramework VS Code extension so that its debug bridge uses the debugger library from the `fix/tcpip-transport-and-build` branch, which is needed to debug the POSIX host over TCP. How to connect the built extension to the host, and every command for debugging and testing over the Wire Protocol, is in [HANDOFF.md, "Debugging and testing over Wire Protocol, step by step"](HANDOFF.md#debugging-and-testing-over-wire-protocol-step-by-step). The Polish version of this page is [VSCODE-EXTENSION.pl.md](VSCODE-EXTENSION.pl.md), kept in sync.

State as of 2026-09-30.

## How the pieces fit together

The extension (TypeScript) starts a separate .NET process, the debug bridge `nanoFramework.Tools.DebugBridge.dll`, and talks to it in JSON over stdin/stdout. The bridge speaks the Wire Protocol to the device through `nanoFramework.Tools.DebugLibrary.Net` from the nf-debugger repository. Upstream the bridge gets that library from NuGet (`nanoFramework.Tools.Debugger.Net`); on the `exception-breakpoints` branch it builds it from a local nf-debugger checkout instead, if one is present:

```
nf-VSCodeExtension (exception-breakpoints)
 └─ src/debugger/bridge/dotnet/nanoFramework.Tools.DebugBridge   (net10.0)
     └─ ProjectReference → ../nf-debugger-local-fixes/nanoFramework.Tools.DebugLibrary.Net   (net8.0)
                           nf-debugger worktree, branch fix/tcpip-transport-and-build
```

**If the local checkout is missing, the build does not fail.** It silently falls back to the latest NuGet package, which has none of the fixes below. Check which one was used after every build (see [Checking what the bridge was built from](#checking-what-the-bridge-was-built-from)).

## Repositories and branches

| Repository | Path | Branch | Where it is |
|---|---|---|---|
| nf-VSCodeExtension | `/workspace/nf-VSCodeExtension` | `exception-breakpoints` | fork: remote `wielebny`, https://github.com/Wielebny666/nf-VSCodeExtension |
| nf-debugger | `/workspace/nf-debugger-local-fixes` (a worktree of `/workspace/nf-debugger`) | `fix/tcpip-transport-and-build` | fork: remote `wielebny`, https://github.com/Wielebny666/nf-debugger |

Both branches are rebased onto `origin/main` of nanoframework: nf-VSCodeExtension onto `e1721d3` (#589), nf-debugger onto `c789b5a` (#405). The branches before that rebase are kept as `backup/exception-breakpoints-pre-rebase` and `backup/local-fixes-pre-rebase`.

### fix/tcpip-transport-and-build (nf-debugger)

| Commit | What it fixes |
|---|---|
| `37278a5` PortTcpIp: fix AddDevice regex | `AddDevice()` used a JavaScript regex literal (quotes and `gm` flags included) as a .NET pattern and could never match, so a TCP device could not be added by its address. The pattern now accepts `tcpip://host:port` and `host:port`. |
| `a1c57d0` TCP transport: set NoDelay and send header+payload in one write | The header and the payload of a packet were two writes; with Nagle on, the second one waited for the peer's delayed ACK (~40 ms). A ~44 KB deployment over TCP: 11.1 s before, 1.6 s after. |
| `4db8afc` Fix Compile include casing for TICC32xx.TargetCapabilities.cs | The projitems said `TiCC32xx`, the file is `TICC32xx`. **Without it the library does not build on Linux.** |

The same commit as `37278a5` also fixed a concurrent start of `DeviceWatcher` (two sockets binding the discovery port, `SocketException` 22). That part was dropped when rebasing: upstream #404 rewrote the watcher's lifecycle and covers it.

### exception-breakpoints (nf-VSCodeExtension)

| Commit | What it does |
|---|---|
| `c359ca2` | builds the bridge against the local nf-debugger checkout (see above); fixes the HintPath rewrite for projects next to their `packages` folder |
| `3ef6992` … `267feca` | debug bridge and adapter: break on thrown and uncaught exceptions, stack frames innermost first, sequence points from the PDB, stepping and breakpoint binding at start, stdout reserved for the JSON protocol, C# `const` fields, draining the breakpoint hit queue, an unanswered state query is not a stop, breakpoint ids returned to VS Code, restarting the app when an attach session restarts |
| `4ded7cf` | assembly names from nanoCLR's shared string table: assemblies such as `System.Threading` store their name as a shared-table token, and the deployment compatibility check rejected them ("Unsupported shared assembly-name token") |
| `ebc5743` | v2 projects are built with `dotnet msbuild` on native Linux and macOS too, not only in WSL; Mono's msbuild has no C# 13 compiler |

## Requirements

| Tool | Version verified | What for |
|---|---|---|
| .NET SDK | 10.0.112 (and 8.0.131) | the bridge targets `net10.0`, the debugger library `net8.0`; SDK 10 builds both |
| .NET runtime 10 | 10.0.12 | on the machine where the extension runs: it starts the bridge as `dotnet nanoFramework.Tools.DebugBridge.dll` |
| Node.js, npm | 24.21.0 | the extension, gulp, `vsce` (all in `node_modules` after `npm install`) |
| PowerShell 7+ | `pwsh` | `scripts/build.ps1`, which downloads the nanoFramework SDKs into `dist/utils` |
| Mono (`mono-complete`, with `msbuild`), `nuget` | from the Mono Project repository | building v1 projects on Linux; the Ubuntu `mono-complete` has no `msbuild` |
| git, network access | | the repositories, NuGet, the VS extension packages |

The [`builder` service](#build-image-the-builder-service) of this repository's dev container has all of it; the versions above were verified in a container built from the same recipe. **The `posix` service ([Dockerfile.POSIX](../../../.devcontainer/POSIX/Dockerfile.POSIX)) does not**: it has .NET SDK 8 and Mono only, no .NET 10, Node.js or PowerShell. Build the extension in `builder`, or install the missing tools.

### Build image: the `builder` service

The second image of [.devcontainer/POSIX](../../../.devcontainer/POSIX), for building nanoFramework managed projects (v1 and v2 `.nfproj`) on Linux, and the extension itself.

| | |
|---|---|
| Definition | [`Dockerfile.NFBUILD`](../../../.devcontainer/POSIX/Dockerfile.NFBUILD) (the only source of the versions) |
| Service | `builder` in [`docker-compose.yml`](../../../.devcontainer/POSIX/docker-compose.yml), in the `builder` profile: `docker compose up` and the dev container neither build nor start it |
| Build and run | `docker compose -f .devcontainer/POSIX/docker-compose.yml run --rm builder` (builds the image the first time); `... build builder` after changing the Dockerfile |
| Prebuilt image | `NF_BUILDER_IMAGE`, in the environment or in `.devcontainer/POSIX/.env`; default `nf-builder:local` |
| Mounts | `NF_WORKSPACE` (default: the parent directory of this repository) as `/workspace`, this repository as `/workspace/nf-interpreter` |

What is in it and why:

| Component | Source | Why |
|---|---|---|
| Ubuntu 24.04 | | base |
| .NET SDK 8 and 10 | Ubuntu archive | `dotnet msbuild` for v2 projects (C# 13+), the bridge (`net10.0`), nanoff |
| `mono-complete` with `msbuild` | Mono Project repository | v1 projects; the Ubuntu package has only `xbuild`, and the extension calls `msbuild` directly |
| PowerShell 7 | Microsoft package repository, added after Mono so it cannot interfere with it | `scripts/build.ps1` |
| Node.js 24 with npm | NodeSource; Ubuntu 24.04 ships Node 18 | the extension, gulp, `vsce` |
| `nuget` | `nuget.exe` from nuget.org behind a shim that runs it under Mono; Ubuntu 24.04 has no `nuget` package | restoring v1 projects |
| nanoff 2.5.162 | .NET global tool in `/opt/dotnet-tools`, copied into `~/.dotnet/tools` on the first start | required by the extension, which runs `dotnet tool update nanoff` on every start |
| git, curl, python3, file, unzip, sudo | Ubuntu archive | |

It has no CMake and no cross toolchains: the native CLR is built in the `posix` service. The third service, `full` ([`Dockerfile.FULL`](../../../.devcontainer/POSIX/Dockerfile.FULL), profile `full`, `NANOCLR_FULL_IMAGE`), has both in one image, with the mounts of `builder`: `docker compose -f .devcontainer/POSIX/docker-compose.yml run --rm full`.

The container runs as user `nano` (UID/GID 1000, passwordless sudo), `HOME=/home/nano`, working directory `/workspace`. The bridge expects the repositories next to each other under `/workspace`, so clone them into the directory that `NF_WORKSPACE` names (see [Setting it up from scratch](#setting-it-up-from-scratch)).

The entrypoint `nf-entrypoint.sh` copies nanoff from `/opt/dotnet-tools` to `~/.dotnet/tools` on the first start. Tools installed in an image layer cannot be moved at runtime on overlayfs (`EXDEV`, "Invalid cross-device link"), and `dotnet tool update` moves them, so the extension's nanoff update would fail every time. When the entrypoint is overridden (e.g. `docker run --entrypoint`), nanoff still works from `/opt/dotnet-tools`, which stays on `PATH`, but it cannot be updated.

## Setting it up from scratch

The bridge looks for the debugger library at `../nf-debugger-local-fixes` relative to the root of nf-VSCodeExtension, so both must be next to each other:

```bash
cd /workspace

# nf-debugger with the fixes, as a worktree next to the extension
git clone https://github.com/nanoframework/nf-debugger.git
git -C nf-debugger remote add wielebny https://github.com/Wielebny666/nf-debugger.git
git -C nf-debugger fetch wielebny
git -C nf-debugger worktree add ../nf-debugger-local-fixes -b fix/tcpip-transport-and-build wielebny/fix/tcpip-transport-and-build

# the extension
git clone https://github.com/nanoframework/nf-VSCodeExtension.git
git -C nf-VSCodeExtension remote add wielebny https://github.com/Wielebny666/nf-VSCodeExtension.git
git -C nf-VSCodeExtension fetch wielebny
git -C nf-VSCodeExtension checkout -b exception-breakpoints wielebny/exception-breakpoints
```

A plain clone of the fork into `/workspace/nf-debugger-local-fixes` works as well; a worktree only saves a second copy of the history. What matters is the directory name and that the `fix/tcpip-transport-and-build` branch is checked out in it.

To use a debugger library somewhere else, set the `NfDebuggerProject` property to the full path of `nanoFramework.Tools.DebugLibrary.Net.csproj`. `npx gulp build-debug-bridge` passes no properties, but MSBuild also reads environment variables as properties, so `NfDebuggerProject=/path/to/...csproj npx gulp build-debug-bridge` works.

## Build

In `/workspace/nf-VSCodeExtension`:

```bash
npm install                                    # also runs 'gulp build', which builds the bridge
pwsh scripts/build.ps1                         # nanoFramework SDKs v1.0 and v2.0, templates, MetadataProcessor task → dist/utils
npx vsce package --out ../vscode-nanoframework-local.vsix --allow-star-activation
```

- `scripts/build.ps1` downloads the v1 and v2 VS2022 extension packages and the MetadataProcessor task; the v1 package and the task are pinned by SHA-256, the v2 package is the latest `2022.14.2.*` from the VSIX Gallery. It only needs to run again when `dist/utils` is missing, or after `-Clean`, or when the script changes the versions.
- `vsce package` runs `vscode:prepublish` first, i.e. `gulp build-debug-bridge` (a `dotnet publish` of the bridge into `bin/nanoDebugBridge`, which is deleted first) and `tsc`. The package then has the extension in `out/`, the bridge in `bin/nanoDebugBridge/` and the SDKs in `dist/utils/`. The whole command takes about 15 s once everything is restored.
- Do not use `npm run package`: it is for CI. It stamps the version from nbgv into `package.json` and publishes CI variables.
- When only the TypeScript changed, `npm run compile` (or `npm run watch`) is enough to run the extension from source with F5 ("Run Extension").

Install the package into VS Code and reload the window:

```bash
code --install-extension ../vscode-nanoframework-local.vsix --force
```

In a dev container this installs it into the VS Code server of the container.

## After changing the debugger branch

Nothing rebuilds the bridge on its own. After a commit or a rebase in `/workspace/nf-debugger-local-fixes`:

```bash
cd /workspace/nf-VSCodeExtension
npx gulp build-debug-bridge      # or build the whole package again with 'npx vsce package ...'
```

`npm run compile` does not rebuild the bridge.

## Checking what the bridge was built from

After every build:

```bash
cd /workspace/nf-VSCodeExtension/bin/nanoDebugBridge

# 'project' = built from the local checkout, 'package' = from NuGet (no fixes)
python3 -c "import json; d=json.load(open('nanoFramework.Tools.DebugBridge.deps.json')); print({k: v['type'] for k, v in d['libraries'].items() if 'Debugger' in k})"

# the version stamped into the library ends with the commit it was built from
strings nanoFramework.Tools.DebugLibrary.Net.dll | grep -m1 -E '^[0-9]+\.[0-9]+\.[0-9]+\+[0-9a-f]+'
git -C /workspace/nf-debugger-local-fixes rev-parse --short=10 HEAD
```

The correct result is `{'nanoFramework.Tools.Debugger.Net/1.0.0': 'project'}` and a version such as `2.5.31+4db8afc7f0` whose suffix is the `HEAD` of the worktree.

## Tests

```bash
cd /workspace/nf-VSCodeExtension
npx tsc --noEmit -p .          # TypeScript
npm run lint                   # 0 errors expected; the warnings (180 today) are upstream's

# bridge tests, on Linux only with these two overrides (see below)
dotnet run --project src/debugger/bridge/dotnet/nanoFramework.Tools.DebugBridge.Tests \
    -p:RuntimeIdentifier=linux-x64 -p:SelfContained=false
```

The bridge tests print `PASS:` or `FAIL:` per case; there are 8 of them.

`nanoFramework.Tools.DebugBridge.Tests.csproj` has `RuntimeIdentifier=win-x64` and `SelfContained=true`. A self-contained executable cannot reference the bridge, which is a framework-dependent executable, and the build stops with `NETSDK1150` (that rule does not depend on the operating system). The two overrides above get around it without changing the file; removing those two lines from the csproj fixes it for good, and all 8 tests then pass with a plain `dotnet run`.

## Updating the branches onto upstream

nf-debugger:

```bash
cd /workspace/nf-debugger-local-fixes
git fetch origin
git branch -f backup/tcpip-pre-rebase HEAD          # keep the old state
git rebase origin/main
dotnet build nanoFramework.Tools.DebugLibrary.Net -c Release
git push --force-with-lease wielebny fix/tcpip-transport-and-build
```

nf-VSCodeExtension: the same with `origin/main` and the `exception-breakpoints` branch (`git push --force-with-lease wielebny exception-breakpoints`), then build the package again.

Upstream is actively working on the parts these branches touch: the last rebase had a conflict in `PortTcpIp/DeviceWatcher.cs` and `PortTcpIpManager.cs` against #404. When upstream solves the same problem differently, take the upstream version and keep only what it does not cover, and say so in the commit message.

## Problems seen

| Symptom | Cause |
|---|---|
| The TCP fixes have no effect, the bridge behaves like upstream | the bridge was built from NuGet: `nf-debugger-local-fixes` is not next to the extension, or has a different name; check `deps.json` |
| `nanoFramework.Tools.DebugLibrary.Net` does not build on Linux, a file `TiCC32xx.TargetCapabilities.cs` is missing | a branch without `4db8afc` |
| `NETSDK1150` when running the bridge tests | the test project's `win-x64` + `SelfContained`, see [Tests](#tests) |
| A v2 project does not build on Linux, a C# language version error | an extension without `ebc5743`: Mono's msbuild was used |
| The deployment compatibility check fails with "Unsupported shared assembly-name token" | an extension without `4ded7cf` |
| The bridge does not start | no .NET 10 runtime where the extension runs |
| `build.ps1` fails on the download | no network access, or the VSIX Gallery returned a version outside `2022.14.2.*` for v2 |
