#!/usr/bin/env bash
#
# Copyright (c) .NET Foundation and Contributors
# See LICENSE file in the project root for full license information.
#
# Zeruje symulowany flash, ktory host POSIX trzyma pod --flashimage, zeby
# nastepny start CLR wstal jako urzadzenie bez wgranej aplikacji.
#
# Obraz jest wypelniany bajtami 0xFF w miejscu, a nie kasowany: tak wyglada
# skasowana kosc, a plik zachowuje wlasciciela i uprawnienia, co ma znaczenie
# przy obrazie czytanym z innego kontenera.
#
# Domyslny obraz: NANOCLR_FLASH_IMAGE, a bez niego ~/nanoclr-flash.img.
#
#   flash-clean.sh                 wyzeruj domyslny obraz
#   flash-clean.sh <obraz>         wyzeruj wskazany
#   flash-clean.sh -f [<obraz>]    wyzeruj, nawet gdy trzyma go dzialajacy CLR
#   flash-clean.sh -s [<obraz>]    tylko powiedz, co w nim jest

set -euo pipefail

# 512 blocks of 4 KB, as Target_BlockStorage_Simulated.cpp describes the device.
# An image of any other size is reported by the CLR and started blank, so this
# figure has to follow that file.
readonly FLASH_BYTES=$((512 * 4096))
readonly DEFAULT_IMAGE="$HOME/nanoclr-flash.img"

force=0
status_only=0

while [ $# -gt 0 ]; do
    case "$1" in
        -f|--force)  force=1; shift ;;
        -s|--status) status_only=1; shift ;;
        -h|--help)
            sed -n '6,18p' "$0" | sed 's/^# \?//'
            exit 0
            ;;
        -*)
            echo "flash-clean: nieznana opcja '$1'" >&2
            exit 2
            ;;
        *) break ;;
    esac
done

image="${1:-${NANOCLR_FLASH_IMAGE:-$DEFAULT_IMAGE}}"

# What is in there now. Worth saying out loud before overwriting it, and it is
# the whole point of -s.
describe() {
    local path="$1"

    if [ ! -e "$path" ]; then
        echo "  nie istnieje"
        return
    fi

    local size
    size=$(stat -c %s "$path")
    echo "  rozmiar    $size B$([ "$size" -ne "$FLASH_BYTES" ] && echo "  (oczekiwane $FLASH_BYTES - CLR odrzuci ten obraz)")"

    local used
    used=$(od -An -v -tu1 "$path" | tr -s ' ' '\n' | grep -cv '^\(255\)\?$' || true)
    echo "  zapisane   $used B"

    local magic
    magic=$(head -c 6 "$path" | tr -d '\0')
    case "$magic" in
        NFMRK*) echo "  zawartosc  deployment, naglowek $magic" ;;
        *)      [ "$used" -eq 0 ] && echo "  zawartosc  pusty" || echo "  zawartosc  zapisany, bez naglowka assembly" ;;
    esac
}

# A CLR that has the image open keeps writing to it. Blanking it underneath
# would be undone by the next write, or silently detached if the file were
# replaced, so say so rather than appear to have worked.
#
# /proc only shows this container's processes, so a copy of this script run from
# another container sees nothing here and will blank the file happily. That is
# also why a real reset needs the instance restarted: the CLR reads the image
# once, at startup, and works from memory afterwards.
holders() {
    local path="$1" resolved pid
    resolved=$(readlink -f "$path" 2>/dev/null) || return 0

    for fd in /proc/[0-9]*/fd/*; do
        [ -e "$fd" ] || continue
        if [ "$(readlink -f "$fd" 2>/dev/null)" = "$resolved" ]; then
            pid=${fd#/proc/}
            pid=${pid%%/*}
            echo "$pid"
        fi
    done | sort -u
}

echo "obraz: $image"
describe "$image"

if [ "$status_only" -eq 1 ]; then
    exit 0
fi

open_by=$(holders "$image" | tr '\n' ' ')

if [ -n "${open_by// /}" ] && [ "$force" -eq 0 ]; then
    echo
    echo "trzymany przez proces:${open_by%% }" >&2
    echo "zatrzymaj go albo uzyj -f, jesli wiesz, co robisz" >&2
    exit 1
fi

mkdir -p "$(dirname "$image")"

# Write beside the target and move it into place, so an interrupted run leaves
# either the old image or a blank one, never half of each.
tmp=$(mktemp "$image.XXXXXX")
trap 'rm -f "$tmp"' EXIT

if [ -e "$image" ]; then
    chmod --reference="$image" "$tmp"
fi

# head first, tr second: the other way round head closes the pipe when it has
# its bytes and tr dies of SIGPIPE, which pipefail turns into a failed run.
head -c "$FLASH_BYTES" /dev/zero | tr '\0' '\377' > "$tmp"
mv -f "$tmp" "$image"
trap - EXIT

echo
echo "wyczyszczony:"
describe "$image"
