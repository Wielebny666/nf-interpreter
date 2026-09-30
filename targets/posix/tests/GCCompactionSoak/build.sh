#!/usr/bin/env bash
#
# Copyright (c) .NET Foundation and Contributors
# See LICENSE file in the project root for full license information.
#
# Build GCCompactionSoak into .pe assemblies for the POSIX host CLR, on Linux, without
# Visual Studio. Roslyn (from the .NET SDK) compiles the C# against the
# nanoFramework reference assemblies from NuGet, then the MetadataProcessor CLI
# (a .NET Framework executable, run under Mono) turns the result into a .pe.
#
# Needs: dotnet-sdk-8.0, mono-complete, curl, unzip.
#
# Usage:
#   build.sh [--out DIR]
#
# The default output directory is build/gccompactionsoak at the repository root, which
# git ignores. It ends up holding every .pe the program needs, and the script
# prints the command line that runs them.
#
# Versions: this branch loads PE v1 (NFMRK1) only, so the metadata processor has
# to be a 3.x release, and every package in packages.config is pinned to the
# version whose native checksum matches the native code compiled into
# targets/posix. A newer package with a different checksum fails to load.
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../../../.." && pwd)"
OUT_DIR="${REPO_ROOT}/build/gccompactionsoak"
MDP_VERSION="3.0.104"
CACHE_DIR="${NF_CACHE_DIR:-${HOME}/.cache/nfbuild}"
NUGET_BASE="https://api.nuget.org/v3-flatcontainer"
ASSEMBLY_NAME="GCCompactionSoak"

while [ $# -gt 0 ]; do
    case "$1" in
        --out)     OUT_DIR="$2"; shift 2 ;;
        -h|--help) sed -n '2,24p' "$0"; exit 0 ;;
        *)         echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

mkdir -p "${CACHE_DIR}" "${OUT_DIR}"
REFS_DIR="${OUT_DIR}/refs"
rm -rf "${REFS_DIR}"
mkdir -p "${REFS_DIR}"

# ── Reference assemblies from NuGet ──────────────────────────────────────────
echo "==> restoring reference assemblies"
while read -r id ver; do
    low="$(echo "${id}" | tr '[:upper:]' '[:lower:]')"
    nupkg="${CACHE_DIR}/${low}.${ver}.nupkg"
    if [ ! -f "${nupkg}" ]; then
        curl -fsSL --max-time 120 -o "${nupkg}" "${NUGET_BASE}/${low}/${ver}/${low}.${ver}.nupkg"
    fi
    # lib/ holds both the reference .dll (for Roslyn) and the prebuilt .pe the CLR loads.
    unzip -o -q -j "${nupkg}" 'lib/*.dll' 'lib/*.pe' -d "${REFS_DIR}"
    echo "    ${id} ${ver}"
done < <(grep -oE 'id="[^"]+" version="[^"]+"' "${SCRIPT_DIR}/packages.config" \
         | sed -E 's/id="([^"]+)" version="([^"]+)"/\1 \2/')

# ── Metadata processor ───────────────────────────────────────────────────────
MDP_DIR="${CACHE_DIR}/mdp-${MDP_VERSION}"
if [ ! -d "${MDP_DIR}" ]; then
    echo "==> fetching metadata processor ${MDP_VERSION}"
    nupkg="${CACHE_DIR}/mdp.${MDP_VERSION}.nupkg"
    curl -fsSL --max-time 120 -o "${nupkg}" \
        "${NUGET_BASE}/nanoframework.tools.metadataprocessor.cli/${MDP_VERSION}/nanoframework.tools.metadataprocessor.cli.${MDP_VERSION}.nupkg"
    mkdir -p "${MDP_DIR}"
    unzip -o -q "${nupkg}" -d "${MDP_DIR}"
fi
MDP_EXE="$(find "${MDP_DIR}" -name 'nanoFramework.Tools.MetadataProcessor.exe' | head -1)"
[ -n "${MDP_EXE}" ] || { echo "error: MetadataProcessor.exe not found in ${MDP_DIR}" >&2; exit 1; }

# ── Compile C# with Roslyn ───────────────────────────────────────────────────
CSC="$( { find /usr/lib/dotnet/sdk /usr/share/dotnet/sdk "${HOME}/.dotnet/sdk" -name csc.dll -path '*bincore*' 2>/dev/null || true; } | sort | tail -1)"
[ -n "${CSC}" ] || { echo "error: Roslyn csc.dll not found - is the .NET SDK installed?" >&2; exit 1; }

# DEBUG stays defined: nanoFramework's Debug.WriteLine is [Conditional("DEBUG")].
CSC_ARGS=(/nostdlib /noconfig /nologo /langversion:latest /target:exe /debug:portable /define:DEBUG /nowarn:0168,0219)
for dll in "${REFS_DIR}"/*.dll; do CSC_ARGS+=("/reference:${dll}"); done

mapfile -t SOURCES < <(find "${SCRIPT_DIR}" -name '*.cs' -not -path '*/bin/*' -not -path '*/obj/*' | sort)

echo "==> compiling ${#SOURCES[@]} source files"
dotnet "${CSC}" "${CSC_ARGS[@]}" "/out:${OUT_DIR}/${ASSEMBLY_NAME}.dll" "${SOURCES[@]}"

# ── Convert to .pe ───────────────────────────────────────────────────────────
echo "==> running metadata processor under Mono"
MDP_ARGS=()
for dll in "${REFS_DIR}"/*.dll; do
    MDP_ARGS+=(-loadHints "$(basename "${dll}" .dll)" "${dll}")
done
MDP_ARGS+=(-parse "${OUT_DIR}/${ASSEMBLY_NAME}.dll")
MDP_ARGS+=(-compile "${OUT_DIR}/${ASSEMBLY_NAME}.pe" false)

mono "${MDP_EXE}" "${MDP_ARGS[@]}" >/dev/null

[ -f "${OUT_DIR}/${ASSEMBLY_NAME}.pe" ] || { echo "error: metadata processor produced no .pe" >&2; exit 1; }
[ "$(head -c 6 "${OUT_DIR}/${ASSEMBLY_NAME}.pe")" = "NFMRK1" ] \
    || { echo "error: ${ASSEMBLY_NAME}.pe is not PE v1 (NFMRK1)" >&2; exit 1; }

# ── Report ───────────────────────────────────────────────────────────────────
# mscorlib.pe goes first and the application last; the rest in any order.
PE_FILES=("${REFS_DIR}/mscorlib.pe")
for pe in "${REFS_DIR}"/*.pe; do
    [ "$(basename "${pe}")" = "mscorlib.pe" ] || PE_FILES+=("${pe}")
done
PE_FILES+=("${OUT_DIR}/${ASSEMBLY_NAME}.pe")
printf '%s\n' "${PE_FILES[@]}" > "${OUT_DIR}/pe-files.txt"

echo
echo "built: ${OUT_DIR}/${ASSEMBLY_NAME}.pe (list of all .pe files in ${OUT_DIR}/pe-files.txt)"
echo
echo "run it with:"
echo "  ./build/posix64-memcheck/bin/nanoFramework.nanoCLR.test \$(cat ${OUT_DIR}/pe-files.txt)"
