#!/usr/bin/env bash
#
# Copyright (c) .NET Foundation and Contributors
# See LICENSE file in the project root for full license information.
#
# Trzyma instancje CLR dla debuggera (takze z innego kontenera) i na zadanie
# podaje ja z czystym flashem.
#
# Wyczyszczenie samego pliku obrazu nie wystarcza: CLR czyta go raz, przy
# starcie, i dalej pracuje na kopii w pamieci. Zeby urzadzenie bylo naprawde
# czyste, instancje trzeba podniesc od nowa - a tego nie da sie zrobic
# z drugiego kontenera, bo procesy sa widoczne tylko lokalnie.
#
# Stad plik-zadanie w katalogu stanu: kto chce pomiaru od zera, robi
#
#     touch "$NANOCLR_BENCH_DIR/reset-flash.request"
#
# i po chwili ma pusty flash i swieza instancje. Stan jest raportowany w
# $NANOCLR_BENCH_DIR/serve-status.txt.
#
#   bench-serve.sh [port] [adres]
#
# Adres to ten, ktory CLR oglasza debuggerom; debugger z innego kontenera
# potrzebuje adresu tego kontenera. Katalog stanu i obraz flasha ustawiaja
# NANOCLR_BENCH_DIR (domyslnie ~/nanoclr-bench) i NANOCLR_FLASH_IMAGE
# (domyslnie ~/nanoclr-flash.img); oba musza byc widoczne dla drugiej strony,
# jesli ma zamawiac reset. Uruchamia build presetu posix-x86-debugger.

set -uo pipefail

readonly PORT="${1:-26000}"
readonly HOST="${2:-127.0.0.1}"
readonly BENCH_DIR="${NANOCLR_BENCH_DIR:-$HOME/nanoclr-bench}"
readonly IMAGE="${NANOCLR_FLASH_IMAGE:-$HOME/nanoclr-flash.img}"
readonly REQUEST=$BENCH_DIR/reset-flash.request
readonly LOG=$BENCH_DIR/live-$PORT.log
readonly STATUS=$BENCH_DIR/serve-status.txt
readonly HERE="$(cd "$(dirname "$0")" && pwd)"
readonly CLR="$HERE/../../build/posix32-wp/bin/nanoFramework.nanoCLR.test"

mkdir -p "$(dirname "$LOG")"

say() {
    echo "$(date '+%H:%M:%S')  $*" | tee -a "$STATUS"
}

clr_pid=""

start_clr() {
    NANOCLR_WP_TRACE="${NANOCLR_WP_TRACE:-1}" \
        "$CLR" --networkport "$PORT" --host "$HOST" \
               --waitfordebugger --loopafterexit --announceinterval 2 \
               --flashimage "$IMAGE" >> "$LOG" 2>&1 &
    clr_pid=$!
    say "instancja wstala: pid $clr_pid, port $PORT, trace ${NANOCLR_WP_TRACE:-1}"
}

stop_clr() {
    [ -n "$clr_pid" ] || return 0
    kill "$clr_pid" 2>/dev/null
    wait "$clr_pid" 2>/dev/null
    clr_pid=""
}

trap 'say "zatrzymywanie"; stop_clr; exit 0' INT TERM

: > "$STATUS"
say "start: port $PORT, adres $HOST, obraz $IMAGE"
say "pomiar od zera: touch $REQUEST"

"$HERE/flash-clean.sh" -f "$IMAGE" >/dev/null
start_clr

while true; do
    if [ -e "$REQUEST" ]; then
        say "zadanie resetu"
        rm -f "$REQUEST"
        stop_clr
        "$HERE/flash-clean.sh" -f "$IMAGE" >/dev/null
        say "flash wyzerowany"
        start_clr
        say "gotowe do pomiaru od zera"
    fi

    # Instancja moze paść na wlasnych bledach - podnies ja, zamiast zostawiac
    # martwy port, bo to wlasnie kosztowalo druga strone jedna nieudana probe.
    if [ -n "$clr_pid" ] && ! kill -0 "$clr_pid" 2>/dev/null; then
        say "instancja zakonczyla sie sama, podnosze"
        clr_pid=""
        start_clr
    fi

    sleep 2
done
