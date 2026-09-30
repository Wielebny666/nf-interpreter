# Sprawdzanie sterty zarządzanej valgrindem

Build hosta POSIX, który mówi memcheckowi valgrinda, co jest w stercie zarządzanej, do tego tryby GC i compaction stress, kwarantanna zwolnionych obiektów, self-test i aplikacja stresowa, która przechodzi przez jak najwięcej natywnego kodu CLR.

Wersja angielska: [HEAP-MEMCHECK.md](HEAP-MEMCHECK.md); obie są utrzymywane w zgodzie.

## Co wykrywa

W zwykłym buildzie sterta zarządzana jest dla valgrinda jednym dużym blokiem z `malloc`. CLR sam rozmieszcza w nim obiekty, więc każdy bajt sterty wygląda na dostępny i zainicjalizowany, a memcheck nie widzi w niej błędów.

Z `NANO_POSIX_HEAP_MEMCHECK` alokator i GC informują memcheck na bieżąco:

| Heap block | Co widzi memcheck |
|---|---|
| nowy obiekt | niezainicjalizowany, dopóki CLR go nie zapisze |
| wolny blok | pierwszy heap block (nagłówek i dowiązania free listy) dostępny, reszta **niedostępna** |
| blok odłożony do event cache (ramki stosu, lock requesty, obiekty oczekiwania, sub-thready...) | jak wolny blok; po wydaniu z cache znów niezainicjalizowany |
| obiekt zwolniony w ostatnim GC, z włączoną kwarantanną | dostępny tylko 4-bajtowy nagłówek, poza free listą do następnego GC |
| obiekt przesunięty przez kompaktowanie | jego stan inicjalizacji przesuwa się razem z danymi |

Dzięki temu widać dwa rodzaje błędów w natywnym kodzie CLR:

| Błąd | Raport memchecka |
|---|---|
| kod natywny używa obiektu po tym, jak GC go zwolnił, bo nic go nie ukorzeniało (brak `CLR_RT_ProtectFromGC`, surowy wskaźnik trzymany przez alokację), albo bloku event cache po jego zwolnieniu | `Invalid read` / `Invalid write` |
| decyzja podjęta na pamięci obiektu, której nic nie zapisało, np. GC znakujący przez niezainicjalizowany slot ramki stosu albo pole | `Conditional jump or move depends on uninitialised value(s)`, `Use of uninitialised value` |

Błędy w pamięci natywnej poza stertą zarządzaną są zgłaszane jak w każdym przebiegu valgrinda.

## Budowanie

Dev container POSIX, [.devcontainer/POSIX](../../../.devcontainer/POSIX/devcontainer.json), ma wszystko, czego potrzebuje ta strona: 32-bitowy toolchain, gdb, valgrinda z symbolami 32-bitowej libc oraz .NET SDK i Mono dla aplikacji stresowej. W VS Code taski i konfiguracje uruchomieniowe w `.vscode` budują preset i aplikację stresową i uruchamiają ją natywnie, pod valgrindem albo pod valgrindem z podpiętym gdb.

```bash
cd targets/posix
cmake --preset posix-x64-memcheck
cmake --build --preset posix-x64-memcheck
```

Preset dziedziczy po `posix-x64-debugger` (Debug z debuggerem Wire Protocol) i buduje do `build/posix64-memcheck`. Ustawia opcję CMake `NANO_POSIX_HEAP_MEMCHECK`, która wymaga nagłówków valgrinda (`valgrind/memcheck.h`, pakiet `valgrind` w Debianie i Ubuntu).

`posix-x86-memcheck` to samo dla i386 (`build/posix32-memcheck`). Ten build ma 12-bajtowy `CLR_RT_HeapBlock` targetów wbudowanych, więc jest najbliższy urządzeniu. Potrzebuje 32-bitowego toolchainu multilib jak pozostałe presety `posix-x86` oraz `libc6-dbg:i386` dla valgrinda. Wszystko poniżej działa tak samo z oboma buildami.

Walidacja sterty działa z tym buildem do poziomu 2; oba presety zostawiają ją na 0. CMake odmawia konfiguracji `NANO_POSIX_HEAP_MEMCHECK` razem z `NANO_POSIX_VALIDATE_HEAP` 3 albo 4, bo od poziomu 3 `ValidateCluster` czyta pierwsze słowo danych każdego heap blocka jako dowiązanie free listy, choć większość bloków to obiekty. Pod valgrindem daje to miliony raportów na pamięci obiektów, a z [kwarantanną](#kwarantanna) CLR pada nawet bez valgrinda, bo blok w kwarantannie trzyma w tym miejscu dane martwego obiektu zamiast dowiązań. Patrz [znalezisko 8](heap-memcheck-findings/08-validatecluster-reads-object-data.md).

Binarka działa też bez valgrinda. Adnotacje są wtedy kilkoma instrukcjami, które nic nie robią.

## Zmienne środowiskowe

Build memcheck czyta `NANOCLR_HEAP_SELFTEST` ([Self-test](#self-test)), `NANOCLR_GC_STRESS` ([GC stress](#gc-stress)), `NANOCLR_COMPACT_STRESS` ([Compaction stress](#compaction-stress)) i `NANOCLR_HEAP_QUARANTINE` ([Kwarantanna](#kwarantanna)), oprócz zmiennych, które czyta każdy build, jak `NANOCLR_HEAP_SIZE_MB`. Wszystkie, z dokładnymi wartościami, są zebrane w jednym miejscu: [HANDOFF.pl.md, „Zestawienie”](HANDOFF.pl.md#zestawienie-opcje-builda-zmienne-środowiskowe-opcje-harnessu).

## Self-test

Self-test sprawdza, czy adnotacje działają: podkłada w stercie znane błędy i oczekuje, że memcheck zgłosi dokładnie je. Uruchamiaj go po zmianie alokatora (`CLR_RT_HeapCluster`), GC, kompaktowania albo samych hooków, i po aktualizacji valgrinda.

Przy pierwszej alokacji zarządzanej `RunSelfTest()` w [nanoCLR/HeapAnnotations.cpp](../nanoCLR/HeapAnnotations.cpp):

1. **Odczyt po zwolnieniu.** Tworzy string trzymany tylko w natywnym `CLR_RT_HeapBlock`, którego GC nie widzi, uruchamia GC, który zwalnia string, i czyta z powrotem jeden z jego znaków. Tekst leży za pierwszym heap blockiem obiektu, więc po zwolnieniu jest niedostępny. Oczekiwane: `Invalid read of size 1`.
2. **Odczyt pierwszego bloku zwolnionego obiektu, tylko z kwarantanną.** Czyta ostatni bajt pierwszego heap blocka stringa, który węzeł free listy zostawiłby dostępny. Oczekiwane: `Invalid read of size 1`.
3. **Niezainicjalizowana pamięć obiektu.** Alokuje obiekt bez zerowania, nigdy go nie zapisuje i rozgałęzia się na jego pierwszym bajcie. Oczekiwane: `Conditional jump or move depends on uninitialised value(s)` ze źródłem `created by a client request at CLR_RT_HeapCluster::ExtractBlocks`.
4. **Odczyt po zwolnieniu do event cache.** Bierze węzeł z event cache, zwalnia go tak, jak zwalniana jest ramka stosu albo lock request, i go czyta. Oczekiwane: `Invalid read of size 1`.

To trzy błędy, albo cztery z `NANOCLR_HEAP_QUARANTINE=1`. Obiekty są potem zwykłymi śmieciami i program działa dalej normalnie, więc self-test można dołączyć do każdego przebiegu. Nada się dowolna aplikacja PE v1; polecenie poniżej używa aplikacji stresowej, zbudowanej jak w [Aplikacja stresowa](#aplikacja-stresowa):

```bash
NANOCLR_HEAP_SELFTEST=1 \
valgrind --track-origins=yes --num-callers=8 --log-file=vg-selftest.log \
    ./build/posix64-memcheck/bin/nanoFramework.nanoCLR.test $(cat build/heapstress/pe-files.txt)
```

Log otacza błędy znacznikami; pierwszy podaje, ilu się spodziewać (skrócone):

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

| Wynik | Znaczenie |
|---|---|
| oczekiwana liczba błędów między znacznikami, wszystkie w `RunSelfTest()` | adnotacje działają |
| brak `Invalid read` z pierwszego przypadku | wolne bloki nie są oznaczane jako niedostępne: sprawdź hooki `FREE` w `RecoverFromGC` i czy build definiuje `NANOCLR_HEAP_ANNOTATIONS` |
| brak `Invalid read` z przypadku kwarantanny | sprawdź hook `QUARANTINE` w `RecoverFromGC` |
| brak `Conditional jump` | nowe obiekty nie są oznaczane jako niezainicjalizowane: sprawdź hook `ALLOC` w `ExtractBlocks` |
| brak `Invalid read` z przypadku event cache | sprawdź hook `FREE` w `CLR_RT_EventCache::Append_Node` |
| brak znaczników w logu | binarka nie pochodzi z `build/posix64-memcheck` albo `NANOCLR_HEAP_SELFTEST` nie jest ustawione |
| dodatkowe błędy między znacznikami | fałszywe alarmy z samego alokatora albo GC, czyli błąd w hookach |
| błędy po `self-test: done` | niezwiązane z self-testem: znaleziska w aplikacji albo w CLR |

Self-test nie podkłada błędów w kompaktowaniu ani w scalaniu w `InsertInOrder`. Te hooki są sprawdzane tylko pośrednio: HeapStress pod [compaction stress](#compaction-stress) musi przejść bez raportów z `Heap_Compact` i `InsertInOrder`.

Sprawdzenie skryptem, dla CI:

```bash
section() { sed -n '/self-test: expect/,/self-test: done/p' vg-selftest.log; }
want=$(section | grep -oE 'expect [0-9]+ errors' | grep -oE '[0-9]+')
n=$(section | grep -cE '^==[0-9]+== (Invalid read|Conditional jump)')
at=$(section | grep -A1 -E '^==[0-9]+== (Invalid read|Conditional jump)' | grep -c 'RunSelfTest()')
[ -n "$want" ] && [ "$n" -eq "$want" ] && [ "$at" -eq "$want" ] && echo "heap self-test OK" \
    || { echo "heap self-test FAILED"; exit 1; }
```

## GC stress

`NANOCLR_GC_STRESS=<n>` uruchamia pełny GC przed co n-tą alokacją zarządzaną. To ten sam GC, który alokacja i tak uruchamia przy pełnej stercie, więc wszystko, co znajdzie, jest prawdziwym błędem; bez stresu taki błąd wychodzi tylko wtedy, gdy sterta akurat zapełni się w złym momencie, a to jest rzadkie i niepowtarzalne.

| Wartość | Zastosowanie |
|---|---|
| `1` | GC przy każdej alokacji; najdokładniej, pod valgrindem bardzo wolno |
| `20`–`100` | rozsądny kompromis pod valgrindem |
| nieustawione albo `0` | bez dodatkowych kolekcji |

Stres pomija alokacje z flagą `HB_NoGcOnFailedAllocation` i alokacje w trakcie GC, gdzie kolekcja nie może się uruchomić, i nie startuje GC z wnętrza innego GC (`PerformGarbageCollection` alokuje wątek finalizatora).

Stres działa też bez valgrinda. Błąd pokazuje się wtedy jako crash zamiast raportu, ale dużo szybciej; to szybki sposób, żeby sprawdzić, czy problem w ogóle jest.

## Compaction stress

CLR nigdy nie kompaktuje w trakcie alokacji, tylko w bezpiecznym punkcie pętli schedulera, a sam z siebie kompaktuje rzadko. `NANOCLR_COMPACT_STRESS=<n>` planuje kompaktowanie po co n-tym GC ze stresu (więc wymaga `NANOCLR_GC_STRESS`); CLR wykonuje je w najbliższym bezpiecznym punkcie, dokładnie jak kompaktowanie, które zaplanuje sam. Żywe obiekty często się wtedy przesuwają, a kod natywny, który trzyma do jednego z nich surowy wskaźnik przez wywołania, których `Relocate` nie aktualizuje, zostaje ze starym adresem. Memcheck zgłasza dostęp przez taki wskaźnik tylko wtedy, gdy ten adres jest akurat wolny, a po kompaktowaniu zwykle nie jest. Kompaktowanie przesuwa każdy ciąg ruchomych obiektów w dół o rozmiar wolnego bloku przed nim, więc pod większością starych adresów ciągu leżą teraz inne obiekty tego samego ciągu, a miejsce, które ciąg zwalnia na swoim końcu, zajmuje następny przesuwany ciąg. Zgłaszane są tylko stare adresy, które wypadną w wolnym miejscu pozostałym po zakończeniu kompaktowania. W przebiegu HeapStress każde przeniesienie było takim przesunięciem, a zaraz po przeniesieniu wolny stary adres miało tylko około 38% przeniesionych heap blocków. Compaction stress znajduje więc taki wskaźnik tylko czasem, nie za każdym razem.

Kilka kolekcji między dwoma bezpiecznymi punktami daje jedno kompaktowanie, więc `1` znaczy „tak często, jak CLR potrafi”, czyli mniej więcej raz na przebieg schedulera. (Opcje harnessu `--forcegc` i `--compactionaftergc` nie są na tym hoście przekazywane do CLR, więc tego nie robią.)

## Kwarantanna

Kwarantanna służy do wykrywania use-after-free, które memcheck inaczej przeoczy: kod natywny trzyma wskaźnik do obiektu, który GC już zwolnił, i używa go później. Bez kwarantanny taki dostęp ukrywają dwie rzeczy:

- **Ponowne użycie.** Zwolniony blok zwykle od razu wraca do alokatora, więc nieaktualny wskaźnik szybko trafia w nowy, poprawny obiekt i memcheck nie ma czego zgłosić. Kwarantanna wydłuża okno, w którym dostęp jest zgłaszany, z „dopóki alokator czegoś tam nie postawi” do „do następnej kolekcji”.
- **Pierwszy heap block.** Sweep zamienia każdy ciąg martwych bloków w jeden węzeł free listy, którego pierwszy heap block (id i dowiązania) pozostaje dostępny. Obiekt, który zaczyna taki ciąg, bo blok przed nim jest żywy, ma więc po zwolnieniu dostępny nagłówek i pierwsze pola, a nieaktualny dostęp do nich nigdy nie jest zgłaszany, nawet zaraz po GC. To 8 bajtów danych na i386 i 16 na x86-64. W kwarantannie dostępne zostaje tylko 4-bajtowe id. Dokładnie to sprawdza przypadek 2 self-testu.

Z `NANOCLR_HEAP_QUARANTINE=1` obiekty, które umierają w kolekcji, zostają poza free listą do następnej: `RecoverFromGC` zbiera je w osobne wolne bloki, które nie są podpięte do listy i mają niedostępne wszystko poza 4-bajtowym nagłówkiem. Następna kolekcja zwalnia je normalnie.

- **Nagłówek to data id.** Dostępny zostaje 32-bitowy `CLR_RT_HeapBlock_Id` na początku bloku: typ danych, flagi i rozmiar w heap blockach. Tylko to czyta przejście po stercie, żeby przeskoczyć blok. Dowiązania free listy nie są potrzebne, bo blok nie jest na żadnej liście, więc na x86-64 niedostępne jest nawet 4-bajtowe wyrównanie za id.
- **Kompaktowanie ich nie przesuwa.** Blok w kwarantannie ma `DATATYPE_FREEBLOCK` z `HB_Pinned`, jak każdy wolny blok, a kompaktowanie przeskakuje bloki przypięte. Miejsca docelowe bierze tylko z free listy, na której ich nie ma, więc nic też do nich nie przesuwa. Przy `NANOCLR_COMPACT_STRESS` nieaktualny wskaźnik do obiektu w kwarantannie po kompaktowaniu nadal trafia w niedostępną pamięć.
- **Nie łączą się z wolnymi sąsiadami.** Sweep w `RecoverFromGC` łączy sąsiednie martwe bloki w jeden ciąg, ale z kwarantanną kończy ciąg tam, gdzie obiekty zmarłe w tej kolekcji stykają się z blokami, które już były wolne. Stare wolne bloki, także te z kwarantanny poprzedniej kolekcji, trafiają na free listę jak dotąd; nowe martwe obiekty stają się obok nich osobnym blokiem w kwarantannie. W następnej kolekcji ten blok nie jest już obiektem, tylko wolnym blokiem, więc łączy się z sąsiadami i trafia na free listę (`NANOCLR_HEAP_ANNOTATE_RELINK` znów pozwala zapisać jego pierwszy heap block, żeby wpisać dowiązania).

Kwarantanna potrzebuje większej sterty: wszystko, co umarło w ostatniej kolekcji, pozostaje nieużywalne i do następnej nie może się połączyć z otaczającym wolnym miejscem. Jeśli alokacje zaczną się nie udawać, podnieś `NANOCLR_HEAP_SIZE_MB`.

## Aplikacja stresowa

Memcheck znajduje błędy tylko w kodzie, który faktycznie się wykonuje. Program, który tylko mieli alokatorem, przechodzi przez prawie żaden natywny kod CLR i nic nie znajduje, nawet przy `NANOCLR_GC_STRESS=1`. [tests/HeapStress](../tests/HeapStress) to aplikacja zarządzana napisana tak, żeby pokryć jak najwięcej natywnego kodu i sprawdzić każdy wynik, tak by uszkodzenie, które nie powoduje crasha, też wyszło.

| Moduł | Pokrywa |
|---|---|
| String, Text, Number | natywne metody `System.String`, `StringBuilder`, UTF-8, parsowanie i formatowanie, `Convert`, `BitConverter`, `DateTime`/`TimeSpan`, `Guid`, `Random`, `System.Math` |
| Array, Collection | tablice liczbowe, referencyjne, struktur i tablice tablic, `Array.Copy`/`Clear`/`IndexOf`/`CreateInstance`, `ArrayList`, natywny `Hashtable` (kolidujące klucze, usuwanie, enumeracja, klonowanie), `Queue`, `Stack` |
| ObjectModel, Delegate, Exception | dispatch wirtualny, abstrakcyjny i przez interfejs, konstruktory statyczne, boxing, indeksery, `ref`/`out`/`params`, flagi enumów, delegaty statyczne, instancyjne, domknięcia i multicast, eventy, wyjątki zarządzane i CLR, zagnieżdżenie na tyle głębokie, że CLR przycina swoje rekordy zagnieżdżonych wyjątków |
| Reflection | `GetMethods`, `GetParameters`, `MethodInfo.Invoke`, pola, konstruktory, atrybuty, wyszukiwanie assembly i typów, `Type` jako klucz `Hashtable` |
| Threading, Timer | wątki z priorytetami, `lock`, `Interlocked`, `AutoResetEvent`/`ManualResetEvent`, `WaitAny`/`WaitAll`, `Thread.Abort`, `CancellationTokenSource`, timery okresowe i jednorazowe |
| GC | finalizatory, wskrzeszanie, `SuppressFinalize`/`ReRegisterForFinalize`, słabe referencje, długa lista wiązana przeplatana śmieciami, która musi przetrwać kompaktowanie |
| Stream, Serialization, Json | `MemoryStream`, `StreamReader`/`StreamWriter`, CRC32, `BinaryFormatter`, nanoFramework.Json (mocno oparty na refleksji) |
| Event, Socket, RuntimeNative | eventy zarządzane przez natywny `EventSink`, `WeakDelegate`, echo TCP i UDP po loopbacku z kilku wątków, `SystemInfo`, `GC.Run`, `ExecutionConstraint` |

Każdy moduł przechodzi pięć rund, a między rundami wymuszany jest GC z kompaktowaniem. Ostatnia wypisana linia to `HEAPSTRESS RESULT: PASS` albo `HEAPSTRESS RESULT: FAIL (<n> failed checks)`. Pozostałe linie zaczynają się od `HEAPSTRESS`, z wyjątkiem własnego zrzutu CLR każdego rzuconego wyjątku (linie zaczynające się od `++++`), w tym wyjątków, które aplikacja rzuca celowo.

| Prefiks | Znaczenie |
|---|---|
| `HEAPSTRESS FAIL` | sprawdzenie nie przeszło; liczone |
| `HEAPSTRESS KNOWN` | sprawdzenie, które nie przechodzi ze znanego już powodu; zgłaszane, nieliczone |
| `HEAPSTRESS KNOWN ISSUE NOW PASSES` | znany problem zniknął; zamień to sprawdzenie w zwykłe |
| `HEAPSTRESS SKIP` | funkcja, której ten host nie implementuje |

### Budowanie aplikacji

Aplikacja buduje się na Linuksie bez Visual Studio, Roslynem z .NET SDK i MetadataProcessorem nanoFramework uruchamianym pod Mono:

```bash
sudo apt-get install dotnet-sdk-8.0 mono-complete     # raz; w dev containerze już jest
targets/posix/tests/HeapStress/build.sh
```

Skrypt pobiera z NuGet pakiety wymienione w `packages.config`, kompiluje i konwertuje wynik do `build/heapstress/HeapStress.pe`. Zapisuje też `build/heapstress/pe-files.txt`, pełną listę plików `.pe` w kolejności ładowania. Wersje pakietów są przypięte: ich natywne sumy kontrolne muszą pasować do natywnego kodu wkompilowanego w ten target, a pakiet z inną sumą się nie załaduje. Ten build ładuje tylko PE v1 (`NFMRK1`), więc skrypt używa MetadataProcessora 3.x.

`HeapStress.nfproj` buduje te same źródła w Visual Studio albo w VS Code z rozszerzeniem nanoFramework.

### Uruchamianie

```bash
# natywnie, szybkie sprawdzenie, że przechodzi
./build/posix64-memcheck/bin/nanoFramework.nanoCLR.test $(cat build/heapstress/pe-files.txt)

# natywnie z GC stress: błąd CLR wychodzi jako crash
NANOCLR_GC_STRESS=1 ./build/posix64-memcheck/bin/nanoFramework.nanoCLR.test $(cat build/heapstress/pe-files.txt)

# pod valgrindem: błąd CLR wychodzi jako raport przy błędnym dostępie
NANOCLR_HEAP_SELFTEST=1 NANOCLR_GC_STRESS=50 \
valgrind --track-origins=yes --num-callers=30 --log-file=vg.log \
    ./build/posix64-memcheck/bin/nanoFramework.nanoCLR.test $(cat build/heapstress/pe-files.txt)

# wszystko naraz: kwarantanna i kompaktowanie po każdym GC ze stresu
NANOCLR_HEAP_SELFTEST=1 NANOCLR_HEAP_QUARANTINE=1 NANOCLR_GC_STRESS=20 NANOCLR_COMPACT_STRESS=1 \
valgrind --track-origins=yes --num-callers=30 --log-file=vg.log \
    ./build/posix64-memcheck/bin/nanoFramework.nanoCLR.test $(cat build/heapstress/pe-files.txt)
```

Żeby zobaczyć tylko wyjście samej aplikacji, filtruj przez `grep ^HEAPSTRESS`.

## Czytanie raportu

Opis adresu zawsze brzmi `... bytes inside a block of size <rozmiar sterty> alloc'd ... HeapLocation (Memory.cpp)`, bo cała sterta to jeden `malloc`. Nic nie mówi o obiekcie. Liczy się:

- **stos wywołań błędu**: który kod natywny dotknął pamięci;
- dla wartości niezainicjalizowanych **`Uninitialised value was created by a client request`** ze stosem alokacji: `ExtractBlocks` ← funkcja, która zaalokowała obiekt, np. `CLR_RT_StackFrame::Push` dla ramek stosu albo `NewObject` dla instancji klas.

Kiedy stos nie wystarcza, zatrzymaj się na błędzie (`--vgdb=yes --vgdb-error=1`, potem `gdb` z `target remote | vgdb`) i obejrzyj stan: którą ramkę stosu i który slot skanuje GC, albo typ i rozmiar obiektu (`ptr->DataType()`, `ptr->DataSize()`).

### Znaleziska

Wszystko, co do tej pory znalazły te narzędzia, z opisem, jak zostało znalezione, i łatką weryfikacyjną tam, gdzie jest, jest w [heap-memcheck-findings](heap-memcheck-findings/README.md). Te pliki są też przykładami czytania raportu.

## Ograniczenia

- **Kwarantanna trwa jedną kolekcję.** Nieaktualny wskaźnik użyty dopiero po następnym GC może trafić w nowy obiekt i przejść niezgłoszony.
- **Sąsiednie obiekty.** Między obiektami nie ma red zones, więc zapis za końcem obiektu do **żywego** sąsiada nie jest wykrywany; wykrywane są tylko zapisy do bloków wolnych, w kwarantannie albo w cache.
- **Relokacja.** Wskaźnik, który CLR przesunie na zły, ale poprawny adres, nie jest wykrywany. Wskaźnik pozostawiony pod starym adresem jest wykrywany tylko, dopóki ten adres jest wolny, a po kompaktowaniu to wyjątek (patrz [Compaction stress](#compaction-stress)).
- **Bufory ramek inline.** Rekordy `CLR_RT_InlineBuffer` leżą w statycznej tablicy, nie na stercie, i nie mają adnotacji.

Pamięć natywna poza stertą zarządzaną nie potrzebuje adnotacji: na tym hoście `platform_malloc` to zwykły `malloc`, więc memcheck sprawdza ją jak każdą inną alokację, łącznie z wyciekami.

## Jak to jest zbudowane

### Podejście

Memcheck trzyma dla każdego bajtu procesu dwa rodzaje stanu cienia: bit A (czy program może się do niego odwołać) i bity V (czy coś określiło jego wartość). Każdy odczyt i zapis jest z nimi porównywany, a kopiowanie przenosi bity V ze źródła do celu. Z tych bitów biorą się jego błędy: dostęp do bajtu bez bitu A to `Invalid read`/`Invalid write`, rozgałęzienie albo wywołanie systemowe na niezdefiniowanej wartości to `Conditional jump ...`/`Use of uninitialised value`.

`malloc` ustawia te bity dla pamięci natywnej, ale sterta zarządzana to jeden blok z `malloc` (`HeapLocation` w `Memory.cpp`), który CLR dzieli sam. Build ustawia więc bity ręcznie, client requestami memchecka, wszędzie tam, gdzie CLR zmienia, czym jest dany fragment sterty:

| Client request | Ustawia | Do czego |
|---|---|---|
| `VALGRIND_MAKE_MEM_UNDEFINED` | dostępne, wartość niezdefiniowana | nowy obiekt; wnętrze bloku wolnego albo z cache, który zaraz zostanie wydany albo zapisany |
| `VALGRIND_MAKE_MEM_NOACCESS` | niedostępne | wnętrze bloku wolnego, z cache albo w kwarantannie |
| `VALGRIND_MAKE_MEM_DEFINED` | dostępne, wartość zdefiniowana | cała sterta, gdy miękki restart CLR dostaje ją z powrotem (patrz [Cykl życia bloku](#cykl-życia-bloku)) |

Poza tym miękkim restartem bajt staje się znów zdefiniowany dopiero, gdy CLR go zapisze.

Decyzje projektowe:

- **Bezstanowe requesty.** `MAKE_MEM_*` zmienia tylko bity cienia zakresu; memcheck nie prowadzi rejestru obiektów. Hooki nie potrzebują więc własnej ewidencji i nie mogą się rozjechać z alokatorem: każdy opisuje stertę taką, jaka jest w tym miejscu. Ceną jest opis adresu w raportach, który wskazuje tylko całą stertę (patrz [Czytanie raportu](#czytanie-raportu)). Same bity cienia przeżywają jednak sesję CLR: miękki restart dostaje z powrotem tę samą stertę, więc host oznacza ją jako zdefiniowaną, zanim nowa sesja ją przeczyta.
- **Układ sterty się nie zmienia.** Obiekty zachowują rozmiar i położenie, bez red zones i bez dopełnienia, więc sterta zapełnia się i kompaktuje dokładnie jak w zwykłym buildzie, a build i386 ma heap blocki urządzenia. Jedynym wyjątkiem jest kwarantanna, i ona tylko opóźnia ponowne użycie.
- **Hooki we wspólnym kodzie, requesty w targecie.** `src/` wywołuje tylko makra z `nanoCLR_HeapAnnotations.h`, które są puste, dopóki target nie zdefiniuje `NANOCLR_HEAP_ANNOTATIONS` i nie dostarczy `nanoCLR_HeapAnnotations_target.h`. Buildy na urządzenia kompilują się do tego samego kodu co wcześniej; inny checker (ASan, patrz [Odnośniki](#odnośniki)) potrzebuje tylko innego nagłówka targetu. Taki target musi mieć wyłączone `NANOCLR_FILL_MEMORY_WITH_DIRTY_PATTERN`, jak POSIX; urządzenie wirtualne Windows włącza je w Debug. Z nim `Debug_ClearBlock` zapisuje wzorzec do wolnych bloków, które hooki uczyniły niedostępnymi, i oznacza jako zdefiniowane wszystkie heap blocki nowego obiektu poza pierwszym.
- **Memcheck, nie mempools ani nowe narzędzie.** Memory pools (`VALGRIND_MEMPOOL_*`) dałyby w każdym raporcie stos alokacji i zwolnienia obiektu, ale wymagają requestu na każdy obiekt, także gdy kompaktowanie go przesuwa, a kompaktowanie przesuwa całe ciągi obiektów jednym `memmove`, bez odwiedzania każdego z nich. Osobne narzędzie valgrinda musiałoby od nowa zaimplementować pamięć cienia, którą memcheck już ma.

### Układ heap blocka

Sterta to tablica `CLR_RT_HeapBlock`. Obiekt to ich ciąg; pierwszy zawiera nagłówek, 32-bitowy `CLR_RT_HeapBlock_Id` (typ danych, flagi i rozmiar ciągu w heap blockach), a reszta to dane obiektu.

| Build | `sizeof(CLR_RT_HeapBlock)` | Układ heap blocka |
|---|---|---|
| i386 (`posix-x86-*`), jak targety wbudowane | 12 bajtów | id na 0, dane na 4..11 |
| x86-64 (`posix-x64-*`) | 24 bajty | id na 0, 4 bajty wyrównania, dane na 8..23 |

Wolny blok (`DATATYPE_FREEBLOCK`) i blok odłożony do event cache (`DATATYPE_CACHEDBLOCK`) to `CLR_RT_HeapBlock_Node`: ten sam pierwszy heap block, w którego danych są dowiązania `next` i `prev` listy dwukierunkowej. Reszta ciągu jest nieużywana. Dlatego `FREE` zostawia pierwszy heap block dostępny, a niedostępną robi tylko resztę: przejście po free liście w `ExtractBlocks`, `InsertInOrder` i kompaktowanie czytają dowiązania, a każde przejście po stercie (sweep, kompaktowanie, walidacja sterty do poziomu 2) czyta id, żeby przejść do następnego bloku. Blok w kwarantannie nie jest na żadnej liście, więc dostępne zostaje tylko jego id.

Adnotacje działają na całych heap blockach, poza 4-bajtowym id w kwarantannie. Na x86-64 wyrównanie za id nigdy nie jest zapisywane, więc w każdym obiekcie pozostaje niezdefiniowane; nie powoduje to raportów, bo memcheck zgłasza niezdefiniowane bajty tylko wtedy, gdy od nich zależy decyzja, a nie przy kopiowaniu.

### Cykl życia bloku

| Przejście | Gdzie | Adnotacja |
|---|---|---|
| sterta wydana przy starcie CLR | `HeapLocation` (`Memory.cpp`), `CLR_RT_HeapCluster::HeapCluster_Initialize` | przy pierwszym starcie sterta jest świeża z `malloc` i wypełniona wzorcem, więc zdefiniowana. Przy miękkim restarcie ta sama pamięć wraca z adnotacjami poprzedniej sesji, a `HeapLocation` znów oznacza ją całą jako zdefiniowaną (`MAKE_MEM_DEFINED`). `HeapCluster_Initialize` czyta ją potem, żeby odzyskać obiekty, które przeżywają restart, a jego `RecoverFromGC` opisuje wolne miejsce jak poniżej |
| znaleziony wolny blok dla alokacji | `CLR_RT_HeapCluster::ExtractBlocks` | `UNFREE` na całym wolnym bloku, żeby reszta mogła dostać nowy nagłówek węzła w jego środku |
| reszta po podziale | `ExtractBlocks` | `FREE` na reszcie, teraz osobnym węźle free listy |
| zaalokowana część | `ExtractBlocks`, po odpięciu | `ALLOC`: cały ciąg razem z pierwszym blokiem staje się niezdefiniowany; CLR zapisuje potem id i zeruje obiekt, gdy ustawione jest `HB_InitializeToZero`. Bez tej flagi każde pole, którego CLR nie zapisze, pozostaje niezdefiniowane, i to właśnie znajduje niezapisane sloty ramek stosu |
| obiekt umiera, sweep | `RecoverFromGC` | ciągi martwych bloków są łączone; `RELINK` pozwala zapisać pierwszy blok (może to być blok z kwarantanny z dostępnym tylko id), ciąg jest dopinany do free listy, a `FREE` ukrywa resztę. Nieaktualny wskaźnik do dowolnego bloku poza pierwszym w ciągu daje teraz `Invalid read` |
| obiekt umiera, z kwarantanną | `RecoverFromGC` | ciąg obiektów zmarłych w tej kolekcji dostaje id wolnego bloku i `QUARANTINE`; nie jest dopinany (patrz [Kwarantanna](#kwarantanna)) |
| wstawienie wolnego bloku ze scalaniem | `CLR_RT_HeapCluster::InsertInOrder` | `FREE` na scalonym bloku, co ukrywa też nagłówek węzła wchłoniętego sąsiada |
| kompaktowanie, cel | `CLR_RT_GarbageCollector::Heap_Compact` | `UNFREE` na wolnym obszarze, zanim `memmove` zapisze do niego żywe obiekty |
| kompaktowanie, przesunięcie | `memmove` | memcheck kopiuje bity V razem z każdym bajtem, który kopiuje program, więc niezdefiniowane pole jest niezdefiniowane także pod nowym adresem. Bity A należą do adresu i się nie przesuwają, dlatego `UNFREE` najpierw otwiera cel |
| kompaktowanie, co przesunięcie zostawia wolne | `Heap_Compact` → `InsertInOrder` | `FREE` na tej części starego zakresu, której nie pokrywa żaden przesunięty obiekt: gdy ciąg zsuwa się do wolnego bloku tuż przed nim, tylko ostatnie bloki starego zakresu, tyle, ile miał wolny blok (wszystkie, jeśli ciąg jest krótszy); gdy jest kopiowany do wolnego bloku gdzie indziej, cały stary zakres. Surowy wskaźnik ze starym adresem daje `Invalid read` tylko wtedy, gdy adres leży w tej części i żadne późniejsze przesunięcie jej nie zapełni (patrz [Compaction stress](#compaction-stress)) |
| kompaktowanie, niewykorzystany wolny obszar | `Heap_Compact` | znów `FREE` na obszarze, który `UNFREE` otworzył, ale nic do niego nie przesunięto |
| zwolnienie do event cache | `CLR_RT_EventCache::Append_Node` | `FREE`, ten sam kształt co wolny blok; dla GC blok pozostaje żywy, ale jest nieużywany |
| pobranie z event cache | `Extract_Node_Fast`, `Extract_Node_Slow` | `UNFREE`: reszta węzła staje się zapisywalna i niezdefiniowana. Pierwszy blok zachowuje dowiązania, bo CLR polega na tym, że dowiązania odpiętego węzła są zerowe. `Extract_Node_Slow` odkłada nieużyty ogon większego węzła z powrotem przez `Append_Node` |

`NANOCLR_HEAP_STRESS_BEFORE_ALLOCATION` w `CLR_RT_ExecutionEngine::ExtractHeapBlocks` nie jest adnotacją: to miejsce, w którym GC stress uruchamia kolekcję, compaction stress ją planuje, a self-test się wykonuje, zanim alokacja weźmie jakąkolwiek pamięć sterty.

### Hooki i pliki

| Hook | Gdzie | Znaczenie |
|---|---|---|
| `NANOCLR_HEAP_ANNOTATE_ALLOC` | `CLR_RT_HeapCluster::ExtractBlocks` | nowy obiekt, niezainicjalizowany |
| `NANOCLR_HEAP_ANNOTATE_UNFREE` | `ExtractBlocks` (przed podziałem wolnego bloku), `Heap_Compact` (przed `memmove`), `CLR_RT_EventCache::Extract_Node_Fast`/`_Slow` | wnętrze bloku wolnego albo z cache staje się zapisywalne i niezainicjalizowane; nagłówek i dowiązania zostają |
| `NANOCLR_HEAP_ANNOTATE_FREE` | `RecoverFromGC` (sweep), `InsertInOrder`, reszta w `ExtractBlocks`, `Heap_Compact`, `CLR_RT_EventCache::Append_Node` | blok wolny albo w cache: nagłówek i dowiązania dostępne, reszta niedostępna |
| `NANOCLR_HEAP_ANNOTATE_QUARANTINE` | `RecoverFromGC` | blok w kwarantannie: dostępny tylko nagłówek |
| `NANOCLR_HEAP_ANNOTATE_RELINK` | `RecoverFromGC` | pierwszy blok ciągu staje się zapisywalny, zanim zostanie zamieniony w węzeł free listy |
| `NANOCLR_HEAP_QUARANTINE_ENABLED` | `RecoverFromGC` | czy umierające obiekty idą do kwarantanny; `false` przy wyłączonych adnotacjach, więc kod znika przy kompilacji |
| `NANOCLR_HEAP_STRESS_BEFORE_ALLOCATION` | `CLR_RT_ExecutionEngine::ExtractHeapBlocks` | GC i compaction stress oraz self-test |

| Plik | Zawartość |
|---|---|
| `src/CLR/Include/nanoCLR_HeapAnnotations.h` | hooki i ich puste domyślne definicje |
| `targets/posix/Include/nanoCLR_HeapAnnotations_target.h` | hooki jako client requesty memchecka (`VALGRIND_MAKE_MEM_*`) |
| `targets/posix/nanoCLR/HeapAnnotations.cpp` | GC i compaction stress, przełącznik kwarantanny i self-test |
| `targets/posix/nanoCLR/Memory.cpp` | pamięć sterty; przy miękkim restarcie CLR znów oznacza ją jako zdefiniowaną |
| `targets/posix/nanoCLR/CMakeLists.txt` | opcja `NANO_POSIX_HEAP_MEMCHECK`, której nie przyjmuje razem z walidacją sterty 3 albo 4 |
| `targets/posix/CMakePresets.json` | presety `posix-x64-memcheck` i `posix-x86-memcheck` |
| `targets/posix/tests/HeapStress/` | aplikacja stresowa i jej skrypt budujący |
| `targets/posix/tests/GCCompactionSoak/` | niekończące się mielenie alokatorem z wymuszonymi kompakcjami i jego skrypt budujący |
| `targets/posix/doc/heap-memcheck-findings/` | znaleziska, jak zostały znalezione, i łatki weryfikacyjne |
| `.devcontainer/POSIX/` | dev container ze wszystkimi powyższymi narzędziami |
| `.vscode/tasks.json`, `.vscode/launch.json` | budowanie, self-test, przebiegi valgrinda i debugowanie z gdb albo vgdb |

## Odnośniki

Dokumentacja valgrinda 3.22; lokalna kopia jest w pakiecie `valgrind` w `/usr/share/doc/valgrind/html/`. Komentarze w `/usr/include/valgrind/memcheck.h` i `valgrind.h` to najdokładniejszy opis każdego requestu.

Client requesty, których używa ten build, i te, którymi można go rozszerzyć:

- [Memcheck client requests](https://valgrind.org/docs/manual/mc-manual.html#mc-manual.clientreqs): `VALGRIND_MAKE_MEM_NOACCESS/UNDEFINED/DEFINED`, `VALGRIND_CHECK_MEM_IS_*`, `VALGRIND_CREATE_BLOCK`
- [Memory pools](https://valgrind.org/docs/manual/mc-manual.html#mc-manual.mempools): `VALGRIND_CREATE_MEMPOOL`, `VALGRIND_MEMPOOL_ALLOC/FREE/CHANGE`, które dałyby każdemu obiektowi własne „alloc'd at / freed at” w raportach
- [Core client requests](https://valgrind.org/docs/manual/manual-core-adv.html#manual-core-adv.clientreq): `VALGRIND_PRINTF`, `RUNNING_ON_VALGRIND`, `VALGRIND_DISABLE_ERROR_REPORTING`
- [How memcheck tracks memory](https://valgrind.org/docs/manual/mc-manual.html#mc-manual.machine): bity A (addressable) i V (valid) stojące za „defined” i „undefined”
- [Memcheck monitor commands](https://valgrind.org/docs/manual/mc-manual.html#mc-manual.monitor-commands): `get_vbits`, `check_memory`, `who_points_at` z gdb przez vgdb

Pisanie osobnego narzędzia valgrinda, które samo instrumentuje kod:

- [Writing a New Valgrind Tool](https://valgrind.org/docs/manual/manual-writing-tools.html)
- [Valgrind technical documentation](https://valgrind.org/docs/manual/tech-docs.html)

Bez valgrinda:

- [AddressSanitizer manual poisoning](https://github.com/google/sanitizers/wiki/AddressSanitizerManualPoisoning): `ASAN_POISON_MEMORY_REGION` / `ASAN_UNPOISON_MEMORY_REGION` z `<sanitizer/asan_interface.h>`. Wariant hooków dla ASan potrzebowałby tylko innego `nanoCLR_HeapAnnotations_target.h` i działa dużo szybciej niż valgrind, ale ASan nie śledzi niezainicjalizowanej pamięci, więc nie zobaczyłby błędów takich jak [znalezisko 1](heap-memcheck-findings/01-pushinline-phantom-slot.md).
