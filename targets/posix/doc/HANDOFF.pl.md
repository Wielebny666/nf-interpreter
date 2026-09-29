# Host POSIX — budowanie, uruchamianie i debugowanie

Jak zbudować i uruchomić host POSIX CLR, z którego korzystają narzędzia tego repozytorium, jak wgrać na niego aplikację po Wire Protocol i ją debugować, i która konfiguracja uruchomieniowa do czego służy. Zbudowane na nim narzędzia do szukania błędów pamięci opisują [README.md](README.md) i [HEAP-MEMCHECK.md](HEAP-MEMCHECK.md).

Wersja angielska: [HANDOFF.md](HANDOFF.md); obie są utrzymywane w zgodzie.

## Dlaczego host POSIX

- Wykonuje ten sam kod CLR co urządzenia. W
  `src/CLR/Include/nanoCLR_PlatformDef.h:96` host POSIX idzie tą samą gałęzią
  co ARM i ESP32: bez profilera, bez statystyk pamięci, walidacja sterty tylko na
  życzenie. Windowsowe urządzenie wirtualne (`VIRTUAL_DEVICE`) włącza
  `NANOCLR_PROFILE_NEW*`, czyli instrumentuje dokładnie te ścieżki alokacji, na
  które patrzą narzędzia.
- Build i386 ma 12-bajtowy `CLR_RT_HeapBlock` targetów wbudowanych; build x86-64
  jest kontrolą. Różna bitowość tych samych źródeł to narzędzie diagnostyczne.
- Buduje się i działa w kontenerze linuksowym, pod gdb i valgrindem.

## Budowanie

Buduje się presetami z `targets/posix/CMakePresets.json`, **z katalogu
`targets/posix`** — z korzenia repo `cmake --preset ...` trafia w presety płytek
i kończy się `is not a directory`:

```bash
cd targets/posix
cmake --preset posix-x86-debugger && cmake --build --preset posix-x86-debugger
```

W VS Code robią to zadania `cmake: build <preset>` i `prepare: ...`, a każda
konfiguracja uruchomieniowa woła swoje zadanie przed startem.

Tylko presety z debuggerem mają `--networkport`, `--flashimage` i resztę opcji
sieciowych — siedzą za `NANOCLR_ENABLE_SOURCELEVELDEBUGGING`. Każdy inny build
odrzuci je jako nieznane argumenty:

```
error: unexpected argument '--networkport' (expected an option, a .pe file or a directory)
```

Jeśli to widzisz, binarka jest zbudowana bez stosu debuggera — nie jest to błąd
konfiguracji uruchamiania.

Każdy preset ustawia `CMAKE_BUILD_TYPE`; configure bez presetu buduje na -O0,
bo `targets/posix/CMakeLists.txt` nie ustawia domyślnego.

| preset | katalog | do czego |
|---|---|---|
| `posix-x86` | `build/posix32` | pomiary, RelWithDebInfo |
| `posix-x86-debug` | `build/posix32-debug` | praca pod gdb |
| `posix-x86-soak` | `build/posix32-soak` | długie przebiegi, walidacja sterty 3 |
| `posix-x86-debugger` | `build/posix32-wp` | stos Wire Protocol, debugger po TCP |
| `posix-x86-memcheck` | `build/posix32-memcheck` | sterta opisana dla valgrinda, GC stress ([HEAP-MEMCHECK.md](HEAP-MEMCHECK.md)) |
| `posix-x64` | `build/posix64` | kontrola LP64 |
| `posix-x64-debug` | `build/posix64-debug` | kontrola pod gdb |
| `posix-x64-soak` | `build/posix64-soak` | soak na LP64, walidacja sterty 3 |
| `posix-x64-debugger` | `build/posix64-wp` | stos Wire Protocol na LP64, debugger po TCP |
| `posix-x64-memcheck` | `build/posix64-memcheck` | sterta opisana dla valgrinda, GC stress |

Walidacja sterty: `NANO_POSIX_VALIDATE_HEAP` 0–4, 0 do pomiarów, 3 do soaku.

Profilowanie: callgrind działa bez uprawnień. `perf` nie ma w obrazie (pakiet jest
związany z jądrem kontenera, nie hosta) — uruchamiaj go z hosta przeciwko
procesowi w kontenerze.

## Uruchamianie: trzy tryby

### 1. Pliki PE

```bash
./build/posix32/bin/nanoFramework.nanoCLR.test build/heapstress/refs build/heapstress/HeapStress.pe
```

Harness przyjmuje pliki `.pe` i katalogi; katalog rozwija się do swoich plików
`.pe`, posortowanych, z `mscorlib.pe` na początku. Najszybsza pętla
edycja–uruchomienie.

`--forcegc` i `--compactionaftergc` **nic nie robią** na hoście POSIX: `main.cpp`
ustawia je w `CLR_SETTINGS`, ale `CLRStartup.cpp` woła
`CLR_RT_ExecutionEngine::CreateInstance()` bez parametrów, więc do silnika nie
docierają. GC działa jak na urządzeniu, gdy brakuje sterty; kompakcja tylko
wtedy, gdy CLR sam ją zaplanuje. Wymuszone kolekcje i kompakcje daje build
memcheck z `NANOCLR_GC_STRESS` i `NANOCLR_COMPACT_STRESS`.

### 2. Goły CLR, aplikacja wgrana po Wire Protocol

```bash
./build/posix32-wp/bin/nanoFramework.nanoCLR.test \
    --networkport 26000 --host <adres widoczny dla debuggera> \
    --waitfordebugger --loopafterexit
```

`--waitfordebugger` i `--loopafterexit` są tu kluczowe (patrz „Debugger po TCP").
Bez nich CLR, który nie ma czego uruchomić, raportuje `a2000000` i kończy się,
zanim cokolwiek zdąży się wgrać.

### 3. Goły CLR z trwałym flashem

`--flashimage <ścieżka>` trzyma symulowany flash w pliku, więc deployment
przeżywa restart procesu:

```bash
# raz, żeby wgrać aplikację na „urządzenie"
./build/posix32-wp/bin/nanoFramework.nanoCLR.test \
    --networkport 26000 --host <adres> --waitfordebugger --loopafterexit \
    --flashimage ~/nanoclr-flash.img

# od tej pory, bez debuggera i bez assembly
./build/posix32-wp/bin/nanoFramework.nanoCLR.test --flashimage ~/nanoclr-flash.img
```

Drugie polecenie wypisuje „Loading Deployment Assemblies." i uruchamia to, co
zostało wgrane. Plik to cały flash, 2 MB (512 bloków po 4 KB,
`targets/posix/nanoCLR/Target_BlockStorage_Simulated.cpp`); taki, którego rozmiar
się nie zgadza, jest raportowany i startuje pusty, zamiast być czytany pod złymi
offsetami. Bez `--flashimage` flash jest pusty przy każdym starcie.

**Nie trzymaj obrazu w `build/`.** Ten katalog kasuje się przy przełączaniu
targetu, a obrazu nie da się odtworzyć z repo. Konfiguracje uruchomieniowe
używają `/home/nano/nanoclr-flash.img`, katalogu domowego w dev containerze.

### Zerowanie flasha i serwowanie instancji

`.devcontainer/POSIX/flash-clean.sh` zeruje obraz:

```bash
.devcontainer/POSIX/flash-clean.sh            # wyzeruj domyślny obraz (NANOCLR_FLASH_IMAGE albo ~/nanoclr-flash.img)
.devcontainer/POSIX/flash-clean.sh -s         # tylko powiedz, co w nim jest
.devcontainer/POSIX/flash-clean.sh <obraz>    # inny plik
```

Wypełnia obraz bajtami 0xFF w miejscu, a nie kasuje pliku: tak wygląda skasowana
kość, a plik zachowuje właściciela i uprawnienia, co ma znaczenie przy obrazie
dzielonym z innym kontenerem. Odmawia pracy, gdy plik trzyma otwarty działający
CLR — wtedy `-f`, jeśli naprawdę o to chodzi. Widzi tylko procesy własnego
kontenera.

Wyzerowanie samego pliku **nie wystarcza**, żeby mieć czyste urządzenie: CLR
czyta obraz raz, przy starcie, i dalej pracuje na kopii w pamięci. Reset musi
podnieść instancję od nowa. Robi to `.devcontainer/POSIX/bench-serve.sh [port]
[adres]`: trzyma instancję `posix-x86-debugger` dla debuggera, także z innego
kontenera, i na żądanie podnosi ją z czystym flashem:

```bash
.devcontainer/POSIX/bench-serve.sh 26000 <adres ogłaszany debuggerowi>
touch ~/nanoclr-bench/reset-flash.request     # z każdego miejsca, które widzi katalog stanu
```

Po ~2 s flash jest wyzerowany, instancja podniesiona od nowa, a stan zapisany
w `~/nanoclr-bench/serve-status.txt`. Podnosi też instancję, która padła sama.
`NANOCLR_BENCH_DIR` i `NANOCLR_FLASH_IMAGE` przenoszą katalog stanu i obraz.

### Nie mieszaj trybu 1 z 2

Podanie plików PE **i** posiadanie deploymentu na flashu ładuje **oba
egzemplarze**, bez priorytetu i bez ostrzeżenia. `CLR_RT_TypeSystem::Link`
(`src/CLR/Core/TypeSystem.cpp:3474`) wkłada assembly w pierwszy wolny slot i nie
sprawdza nazwy, a `PostLinkageProcessing` nadpisuje `m_assemblyMscorlib`
bezwarunkowo.

Dla pracy ze stertą jest gorzej: assembly z deploymentu idą **na zarządzaną
stertę jako bloki `HB_Unmovable`** (`targets/posix/nanoCLR/CLRStartup.cpp:340`),
a te z linii poleceń siedzą poza stertą, w pamięci hosta. Zdublowany deployment
przypina nieruszalne bloki w środku sterty.

## Konfiguracje uruchomieniowe

`.vscode/launch.json` jest podzielony na grupy. W nazwie każdej konfiguracji
w nawiasie `[...]` stoi preset, którego binarkę uruchamia; komentarz nad nią mówi,
do czego służy. Każda konfiguracja poza tą od core dumpa ma preLaunchTask, który
najpierw buduje swój preset, a HeapStress tam, gdzie konfiguracja go uruchamia.

### 1–2 HeapStress — `posix-x64-memcheck`, `posix-x86-memcheck`

Szukanie błędów pamięci w CLR; opisane w [README.md](README.md).

| konfiguracja | do czego |
|---|---|
| native | HeapStress pod gdb; z GC stress błąd CLR to crash, na którym gdb staje |
| valgrind + gdb | HeapStress pod valgrindem; gdb przez vgdb staje na każdym raporcie memchecka |

### 3 Wire Protocol x86 — `posix-x86-debugger` (`build/posix32-wp`)

| konfiguracja | do czego |
|---|---|
| bare CLR, wait for a deployment | pusty CLR czeka na debugger (tryb 2+3); wgrana aplikacja trafia do obrazu flasha |
| run what was deployed | uruchamia to, co jest w obrazie flasha (tryb 3); debugger może się podpiąć po TCP |
| run HeapStress, debugger port open | HeapStress z otwartym portem debuggera (`--loopafterexit`) |

### 4 Wire Protocol x64 — `posix-x64-debugger` (`build/posix64-wp`)

| konfiguracja | do czego |
|---|---|
| bare CLR, wait for a deployment | jak w x86, na LP64 |
| run what was deployed | jak w x86, na LP64 |
| run what was deployed, valgrind + gdb | task startuje valgrind z obrazem flasha i czeka; gdb podpina się przez vgdb i staje na każdym błędzie. Bez adnotacji sterty — błędy na stercie zarządzanej łapią konfiguracje memcheck |

### 5 GC bench x86 — `posix-x86*`

Build ILP32 ma 12-bajtowy `CLR_RT_HeapBlock` jak wbudowane targety ARM,
więc układ sterty i zachowanie GC odpowiadają urządzeniu.

| konfiguracja | preset | do czego |
|---|---|---|
| run HeapStress | `posix-x86-debug` | HeapStress pod gdb |
| --forcegc --compactionaftergc | `posix-x86-debug` | jak wyżej; flagi nie działają (patrz tryb 1) |
| break on Heap_Compact | `posix-x86-debug` | staje w `Heap_Compact`, gdy CLR sam zaplanuje kompakcję |
| measure, pick heap size | `posix-x86` | przebiegi na buildzie zoptymalizowanym, rozmiar sterty wybierany przy starcie |
| soak, heap validation 3 | `posix-x86-soak` | długie przebiegi z walidacją sterty; wolne |
| smoke, no assemblies | `posix-x86-debug` | sam start CLR: baner i wyjście `a2000000` |

### 6 GC bench x64 — `posix-x64*`

Te same sześć konfiguracji na buildzie LP64, czyli konfiguracji docelowej,
z własnym rozmiarem bloku i wyrównaniem: `posix-x64-debug`, `posix-x64`
i `posix-x64-soak` w miejsce presetów x86.

### 7 Core dump

| konfiguracja | do czego |
|---|---|
| open in gdb (newest or chosen core) | pyta o plik core (pusty = najnowszy `core` albo `core.<pid>` w katalogu głównym repozytorium), odczytuje z niego, który program go zrzucił, i otwiera oba w gdb; nic nie jest przebudowywane |

Proces, który pada, zapisuje `core.<pid>` w swoim katalogu roboczym, czyli dla
konfiguracji uruchomieniowych i poleceń z tego dokumentu w katalogu głównym
repozytorium. Zadanie `core: select core dump` dowiązuje core i jego program
w `build/core-debug/`. Ostrzega, gdy program jest nowszy niż core: program
przebudowany po awarii już do niego nie pasuje, więc otwórz core przed ponownym
budowaniem.

### Parametry pytane przy starcie

| input | znaczenie |
|---|---|
| `heapMb`, `vgHeapMb` | `NANOCLR_HEAP_SIZE_MB`; 10 to domyślna hosta, 32 odpowiada NXP_MIMXRT1060_EVK |
| `networkPort`, `announceHost` (`vgNetworkPort`, `vgAnnounceHost`) | port i adres debuggera po TCP |
| `flashImage`, `vgFlashImage` | plik symulowanego flasha, domyślnie `/home/nano/nanoclr-flash.img` |
| `wpTrace` | `NANOCLR_WP_TRACE`: zrzut każdego pakietu Wire Protocol; 0 do pomiarów czasu |
| `gcStress`, `compactStress`, `quarantine` | ustawienia stresu memcheck, patrz [HEAP-MEMCHECK.md](HEAP-MEMCHECK.md) |
| `coreFile` | plik core dla konfiguracji „Core dump"; pusty to najnowszy w katalogu głównym repozytorium |

## Debugger po TCP

Presety `posix-x86-debugger` i `posix-x64-debugger`. Wire Protocol niesie
32-bitowe referencje do sterty: na i386 to same wskaźniki, na x86-64 przesunięcia
od początku sterty (`src/CLR/Debugger/Debugger.cpp:30-33`).

Transport jest w `targets/posix/nanoCLR/TcpWireProtocol.cpp` i należy do
harnessu, nie do CLR — tak samo jak na urządzeniach, gdzie tę rolę gra zarządzane
narzędzie nanoclr.

Flagi (`--networkport`, `--host`, `--broadcastport`, `--broadcastaddress`,
`--announceinterval`, `--waitfordebugger`, `--loopafterexit`, `--flashimage`) są
opisane z wartościami domyślnymi w [„Zestawieniu"](#zestawienie-opcje-builda-zmienne-środowiskowe-opcje-harnessu).

Odkrywanie to jeden datagram UDP `+:host:port`. Jeden, nie dwa — druga kopia
powoduje, że watcher widzi dwa przyjścia tego samego urządzenia i puszcza dwie
walidacje przeciwko sobie.

### --loopafterexit jest obowiązkowy

**Bez niego goły CLR ginie, zanim debugger zdąży cokolwiek wgrać.** To warunek
działania przepływu „launch" z VS Code:

1. `Debugger_Discovery` (`src/CLR/Debugger/Debugger.cpp:130`) czeka 5 sekund na
   debugger. Jeśli wtyczka nie zdąży, CLR pisze `No debugger found...`.
2. Debugger podłącza się chwilę później i **wznawia wykonanie** — przepływ launch
   robi to inaczej niż klient deployujący, który trzyma urządzenie zatrzymane.
3. CLR rusza, nie ma czego uruchomić, zgłasza `Error: a2000000` i **kończy
   proces**, zanim wtyczka dojdzie do wgrywania.

Objaw wygląda na błąd deploymentu, a jest wyścigiem o start. Z `--loopafterexit`
CLR po tym samym `Error: a2000000` zostaje w `Waiting for debug commands...`,
przyjmuje deployment i po restarcie ładuje wszystkie assembly.

Dotyczy też miękkiego restartu: `targets/posix/nanoCLR/CLRStartup.cpp:582`
podmienia `WaitForDebugger` na to, o co poprosił reboot, więc bez flagi urządzenie
znika debuggerowi w połowie sesji.

Wszystkie konfiguracje z grup Wire Protocol w `.vscode/launch.json` mają tę flagę.

### Logi transportu

Każda sesja kończy się jedną linią podsumowania, **zawsze**, niezależnie od
trace'u (`TcpWireProtocol.cpp:107`):

```
[ 11621 ms] WP: connection #1 closed after 11621 ms | rx 52612 bytes in 411 packets
            | tx 10506 bytes in 208 packets | 14755516 polls | 4527 bytes/s in
```

`NANOCLR_WP_TRACE=1` dokłada hex dump każdego pakietu ze znacznikiem czasu; `0`
i puste znaczą wyłączone. Prefiks `[N ms]` przy linii `debugger connected` leci
zawsze i **nie** oznacza, że trace jest włączony; trace'em są linie `WP rx:`
i `WP tx:`.

### Rozszerzenie VS Code

Rozszerzenie nanoFramework potrafi łączyć się po TCP. Nie ma Device Explorera,
więc adres podaje się wprost w konfiguracji uruchomieniowej rozszerzenia jako
`"device": "<adres>:26000"`: mostek `nanoFramework.Tools.DebugBridge.dll` używa
TCP, gdy string zawiera `:` i nie zaczyna się od `COM`. Sesja attach i breakpointy
działają. Do wgrania aplikacji trzeba wpisu `launch` z `deployAssemblies: true` —
`attach` niczego nie wgrywa.

Biblioteka debuggera pomija zapis regionu, który ma już tę samą zawartość
(porównuje CRC przez `Monitor_CheckMemory`), ale nadal restartuje urządzenie
i raportuje sukces. Drugi deploy tej samej aplikacji trwa więc ułamek pierwszego
i nic nie wysyła. Do mierzenia deploymentu zaczynaj od pustego flasha.

## Interpreter i wątek debuggera

Handlery komend debuggera chodzą po liście wątków, ramkach stosu i tablicy
breakpointów oraz alokują odpowiedzi na zarządzanej stercie, na wątku Wire
Protocol, który biegnie obok interpretera. Na urządzeniu komenda wykonuje się
między kwantami interpretera; na hoście tę samą gwarancję daje blokada:

- interpreter trzyma ją przez jeden wsad `ScheduleThreads` (`src/CLR/Core/Execution.cpp:20-55`,
  strażnik RAII, bo pętla wychodzi przez `NANOCLR_SET_AND_LEAVE`, czyli goto);
- debugger trzyma ją przez jedną komendę, w `Messaging_ProcessPayload`
  (`src/CLR/Messaging/Messaging.cpp:453`), przez który przechodzą wszystkie komendy;
- sama blokada to `targets/posix/nanoCLR/HostLock.cpp`, z licznikiem oczekujących,
  żeby interpreter nie zabierał jej z powrotem, zanim dostanie ją wątek Wire
  Protocol.

Zakleszczenia nie ma, bo każde `WaitForDebugger` i `DebuggerLoop` leży poza
`ScheduleThreads`. Blokada istnieje tylko w presetach z debuggerem
(`PLATFORM_POSIX_HOST && NANOCLR_ENABLE_SOURCELEVELDEBUGGING`; `HostLock.cpp`
buduje się tylko z debuggerem); pozostałe buildy nie mają po niej śladu.

## Zestawienie: opcje builda, zmienne środowiskowe, opcje harnessu

Wszystko, czym da się skonfigurować host, w jednym miejscu.

### Opcje CMake (`targets/posix/CMakeLists.txt`, `nanoCLR/CMakeLists.txt`)

| opcja | domyślnie | działanie |
|---|---|---|
| `NANO_POSIX_ENABLE_SMOKE` | `ON` | buduje harness `nanoFramework.nanoCLR.test`; bez niej powstaje tylko biblioteka |
| `NANO_POSIX_ENABLE_NETWORK` | `ON` | `System.Net` na gniazdach BSD hosta |
| `NANO_POSIX_ENABLE_DEBUGGER` | `OFF` (`ON` w presetach `-debugger` i `-memcheck`) | stos debuggera Wire Protocol, transport TCP, symulowany flash, `HostLock` |
| `NANO_POSIX_VALIDATE_HEAP` | `0` (`3` w presetach `-soak`) | poziom walidacji sterty 0–4 |
| `NANO_POSIX_HEAP_MEMCHECK` | `OFF` (`ON` w presetach `-memcheck`) | adnotacje sterty dla valgrinda, stres GC i kompakcji, kwarantanna, self-test; wymaga `valgrind/memcheck.h` |
| `NANO_POSIX_ARCH` | `arm64` | tylko macOS: `arm64` albo `x86_64` |

### Zmienne środowiskowe

| zmienna | buildy | działanie |
|---|---|---|
| `NANOCLR_HEAP_SIZE_MB` | wszystkie | rozmiar sterty zarządzanej w MB, 1–1024; inna wartość jest ignorowana i zostaje domyślne 10 (`nanoCLR/Memory.cpp`) |
| `NANOCLR_NETIF` | z siecią | interfejs hosta używany jako interfejs sieciowy 0; domyślnie pierwszy włączony z adresem IPv4, loopback tylko gdy nic innego się nie nadaje (`nanoCLR/NetworkConfiguration_POSIX.cpp`) |
| `NANOCLR_FLASH_IMAGE` | z debuggerem | plik z symulowanym flashem; ustawia ją `--flashimage` (`nanoCLR/Target_BlockStorage_Simulated.cpp`) |
| `NANOCLR_WP_TRACE` | z debuggerem | zrzut każdego pakietu Wire Protocol, gdy ustawiona, niepusta i różna od `0` (`nanoCLR/TcpWireProtocol.cpp`) |
| `NANOCLR_GC_STRESS` | memcheck | pełny GC przed co n-tą alokacją zarządzaną; 0 albo brak to wyłączone |
| `NANOCLR_COMPACT_STRESS` | memcheck | kompakcja sterty po co n-tym GC ze stresu, w najbliższym bezpiecznym punkcie; wymaga `NANOCLR_GC_STRESS` |
| `NANOCLR_HEAP_QUARANTINE` | memcheck | liczba różna od zera trzyma obiekty zwolnione przez GC niedostępne do następnego |
| `NANOCLR_HEAP_SELFTEST` | memcheck | liczba różna od zera podkłada znane błędy sterty przy pierwszej alokacji zarządzanej |

Ostatnie cztery są w `nanoCLR/HeapAnnotations.cpp` i opisuje je [HEAP-MEMCHECK.md](HEAP-MEMCHECK.md). Skrypty w `.devcontainer/POSIX` czytają też `NANOCLR_FLASH_IMAGE` (domyślnie `~/nanoclr-flash.img`), a `bench-serve.sh` czyta `NANOCLR_BENCH_DIR` (domyślnie `~/nanoclr-bench`).

### Opcje harnessu (`nanoFramework.nanoCLR.test`, `nanoCLR/main.cpp`)

`nanoFramework.nanoCLR.test [opcje] [--assemblies] <plik.pe|katalog> ...`

| opcja | buildy | działanie |
|---|---|---|
| `<plik.pe>`, `<katalog>` | wszystkie | assembly do załadowania, w podanej kolejności; katalog rozwija się do swoich plików `.pe`, posortowanych, `mscorlib.pe` na początku |
| `--assemblies` | wszystkie | przyjmowana i ignorowana, dla zgodności z narzędziem nanoclr; dalej nadal idą pliki i katalogi |
| `--maxcontextswitches <n>` | wszystkie | kwant schedulera, domyślnie 50 |
| `--forcegc` | wszystkie | parsowana, ale na tym hoście nic nie robi (patrz tryb 1) |
| `--compactionaftergc` | wszystkie | parsowana, ale na tym hoście nic nie robi (patrz tryb 1) |
| `--waitfordebugger` | wszystkie, sens ma z debuggerem | czekaj na debugger przed uruchomieniem |
| `--loopafterexit` | wszystkie, sens ma z debuggerem | zostań w pętli debuggera po zakończeniu programu |
| `--networkport <n>` | z debuggerem | debugger na porcie TCP n |
| `--host <adres>` | z debuggerem | adres ogłaszany debuggerom, domyślnie 127.0.0.1 |
| `--broadcastport <n>` | z debuggerem | port odkrywania, 0 wyłącza, domyślnie 23657 |
| `--broadcastaddress <a>` | z debuggerem | gdzie ogłaszać, domyślnie 255.255.255.255 |
| `--announceinterval <s>` | z debuggerem | powtarzaj ogłoszenie co s sekund, 0 to raz, domyślnie 5 |
| `--flashimage <ścieżka>` | z debuggerem | trzymaj symulowany flash w tym pliku (ustawia `NANOCLR_FLASH_IMAGE`) |
| `-h`, `--help` | wszystkie | pomoc |

## Znane problemy hosta

1. **`--forcegc` i `--compactionaftergc` nie działają**, patrz tryb 1.
2. **Standard C++.** Host POSIX buduje się jako C++20
   (`targets/posix/nanoCLR/CMakeLists.txt:351` i `:459`), targety ARM jako C++23
   (`CMake/toolchain.arm-none-eabi.cmake:54`).
3. **Brak ostrzeżenia o zdublowanych assembly**, patrz „Nie mieszaj trybu 1 z 2".
4. **Kod wyjścia 134 po programie, który używał gniazd**: znalezisko
   [4](heap-memcheck-findings/04-readiness-monitor-exit-abort.md).
