# Budowanie rozszerzenia VS Code z poprawioną biblioteką debuggera

Jak zbudować rozszerzenie nanoFramework dla VS Code tak, żeby jego mostek debugowania używał biblioteki debuggera z gałęzi `fix/tcpip-transport-and-build`, potrzebnej do debugowania hosta POSIX po TCP. Jak podłączyć zbudowane rozszerzenie do hosta i jakie komendy wywołać, żeby debugować i testować przez Wire Protocol, opisuje [HANDOFF.pl.md, „Debugowanie i testy przez Wire Protocol krok po kroku"](HANDOFF.pl.md#debugowanie-i-testy-przez-wire-protocol-krok-po-kroku). Wersja angielska: [VSCODE-EXTENSION.md](VSCODE-EXTENSION.md); obie są utrzymywane w zgodzie.

Stan na 2026-09-30.

## Jak to się składa

Rozszerzenie (TypeScript) uruchamia osobny proces .NET, mostek debugowania `nanoFramework.Tools.DebugBridge.dll`, i rozmawia z nim JSON-em po stdin/stdout. Mostek mówi do urządzenia Wire Protocolem przez `nanoFramework.Tools.DebugLibrary.Net` z repozytorium nf-debugger. Upstream mostek bierze tę bibliotekę z NuGeta (`nanoFramework.Tools.Debugger.Net`); na gałęzi `exception-breakpoints` buduje ją zamiast tego z lokalnego checkoutu nf-debuggera, jeśli taki jest:

```
nf-VSCodeExtension (exception-breakpoints)
 └─ src/debugger/bridge/dotnet/nanoFramework.Tools.DebugBridge   (net10.0)
     └─ ProjectReference → ../nf-debugger-local-fixes/nanoFramework.Tools.DebugLibrary.Net   (net8.0)
                           worktree nf-debuggera, gałąź fix/tcpip-transport-and-build
```

**Jeśli lokalnego checkoutu nie ma, build się nie wywala.** Po cichu bierze najnowszy pakiet z NuGeta, który nie ma żadnej z poprawek poniżej. Po każdym buildzie sprawdź, z czego zbudowano mostek (zob. [Sprawdzenie, z czego zbudowano mostek](#sprawdzenie-z-czego-zbudowano-mostek)).

## Repozytoria i gałęzie

| Repozytorium | Ścieżka | Gałąź | Gdzie jest |
|---|---|---|---|
| nf-VSCodeExtension | `/workspace/nf-VSCodeExtension` | `exception-breakpoints` | fork: remote `wielebny`, https://github.com/Wielebny666/nf-VSCodeExtension |
| nf-debugger | `/workspace/nf-debugger-local-fixes` (worktree `/workspace/nf-debugger`) | `fix/tcpip-transport-and-build` | fork: remote `wielebny`, https://github.com/Wielebny666/nf-debugger |

Obie gałęzie są zrebase'owane na `origin/main` nanoframework: nf-VSCodeExtension na `e1721d3` (#589), nf-debugger na `c789b5a` (#405). Gałęzie sprzed tego rebase'u są zachowane jako `backup/exception-breakpoints-pre-rebase` i `backup/local-fixes-pre-rebase`.

### fix/tcpip-transport-and-build (nf-debugger)

| Commit | Co naprawia |
|---|---|
| `37278a5` PortTcpIp: fix AddDevice regex | `AddDevice()` używał literału regexa z JavaScriptu (razem z cudzysłowami i flagami `gm`) jako wzorca .NET i nigdy nic nie dopasowywał, więc urządzenia TCP nie dało się dodać po adresie. Wzorzec przyjmuje teraz `tcpip://host:port` i `host:port`. |
| `a1c57d0` TCP transport: set NoDelay and send header+payload in one write | Nagłówek i payload pakietu szły dwoma zapisami; przy włączonym Nagle'u drugi czekał na opóźniony ACK drugiej strony (~40 ms). Deployment ~44 KB po TCP: 11,1 s przed, 1,6 s po. |
| `4db8afc` Fix Compile include casing for TICC32xx.TargetCapabilities.cs | W projitems było `TiCC32xx`, plik nazywa się `TICC32xx`. **Bez tego biblioteka nie buduje się na Linuksie.** |

Ten sam commit co `37278a5` naprawiał też współbieżny start `DeviceWatchera` (dwa sockety bindujące port discovery, `SocketException` 22). Tę część usunięto przy rebase'ie: upstreamowy #404 przepisał cykl życia watchera i to pokrywa.

### exception-breakpoints (nf-VSCodeExtension)

| Commit | Co robi |
|---|---|
| `c359ca2` | buduje mostek z lokalnego checkoutu nf-debuggera (zob. wyżej); poprawia przepisywanie HintPath dla projektów leżących obok swojego katalogu `packages` |
| `3ef6992` … `267feca` | mostek i adapter debugowania: zatrzymanie na rzuconych i nieobsłużonych wyjątkach, ramki stosu od najgłębszej, sequence pointy z PDB, stepping i wiązanie breakpointów na starcie, stdout tylko dla protokołu JSON, pola C# `const`, opróżnianie kolejki trafionych breakpointów, brak odpowiedzi na zapytanie o stan to nie zatrzymanie, identyfikatory breakpointów zwracane do VS Code, restart aplikacji przy restarcie sesji attach |
| `4ded7cf` | nazwy assembly ze współdzielonej tablicy stringów nanoCLR: assembly takie jak `System.Threading` zapisują nazwę jako token tablicy współdzielonej, a sprawdzanie zgodności deploymentu je odrzucało („Unsupported shared assembly-name token") |
| `ebc5743` | projekty v2 buduje się `dotnet msbuild` także na natywnym Linuksie i macOS, nie tylko w WSL; msbuild z Mono nie ma kompilatora C# 13 |

## Wymagania

| Narzędzie | Sprawdzona wersja | Do czego |
|---|---|---|
| .NET SDK | 10.0.112 (oraz 8.0.131) | mostek celuje w `net10.0`, biblioteka debuggera w `net8.0`; SDK 10 buduje oba |
| runtime .NET 10 | 10.0.12 | na maszynie, na której działa rozszerzenie: uruchamia mostek jako `dotnet nanoFramework.Tools.DebugBridge.dll` |
| Node.js, npm | 24.21.0 | rozszerzenie, gulp, `vsce` (wszystko w `node_modules` po `npm install`) |
| PowerShell 7+ | `pwsh` | `scripts/build.ps1`, który ściąga SDK nanoFramework do `dist/utils` |
| Mono (`mono-complete`, z `msbuild`), `nuget` | z repozytorium Mono Project | budowanie projektów v1 na Linuksie; `mono-complete` z Ubuntu nie ma `msbuild` |
| git, dostęp do sieci | | repozytoria, NuGet, pakiety rozszerzenia VS |

Usługa [`builder`](#obraz-do-budowania-usługa-builder) dev containera tego repozytorium ma to wszystko; wersje powyżej sprawdzono w kontenerze zbudowanym z tego samego przepisu. **Usługa `posix` ([Dockerfile.POSIX](../../../.devcontainer/POSIX/Dockerfile.POSIX)) tego nie ma**: ma tylko .NET SDK 8 i Mono, bez .NET 10, Node.js i PowerShella. Buduj rozszerzenie w `builder` albo doinstaluj brakujące narzędzia.

### Obraz do budowania: usługa `builder`

Drugi obraz w [.devcontainer/POSIX](../../../.devcontainer/POSIX), do budowania zarządzanych projektów nanoFramework (`.nfproj` v1 i v2) na Linuksie oraz samego rozszerzenia.

| | |
|---|---|
| Definicja | [`Dockerfile.NFBUILD`](../../../.devcontainer/POSIX/Dockerfile.NFBUILD) (jedyne źródło wersji) |
| Usługa | `builder` w [`docker-compose.yml`](../../../.devcontainer/POSIX/docker-compose.yml), w profilu `builder`: `docker compose up` i dev container go nie budują ani nie uruchamiają |
| Budowanie i uruchomienie | `docker compose -f .devcontainer/POSIX/docker-compose.yml run --rm builder` (za pierwszym razem buduje obraz); `... build builder` po zmianie Dockerfile'a |
| Gotowy obraz | `NF_BUILDER_IMAGE`, w środowisku albo w `.devcontainer/POSIX/.env`; domyślnie `nf-builder:local` |
| Montowania | `NF_WORKSPACE` (domyślnie katalog nadrzędny tego repozytorium) jako `/workspace`, to repozytorium jako `/workspace/nf-interpreter` |

Co w nim jest i po co:

| Składnik | Źródło | Po co |
|---|---|---|
| Ubuntu 24.04 | | baza |
| .NET SDK 8 i 10 | archiwum Ubuntu | `dotnet msbuild` dla projektów v2 (C# 13+), mostek (`net10.0`), nanoff |
| `mono-complete` z `msbuild` | repozytorium Mono Project | projekty v1; pakiet z Ubuntu ma tylko `xbuild`, a rozszerzenie wywołuje `msbuild` wprost |
| PowerShell 7 | repozytorium pakietów Microsoftu, dodane po Mono, żeby mu nie przeszkadzało | `scripts/build.ps1` |
| Node.js 24 z npm | NodeSource; Ubuntu 24.04 ma Node 18 | rozszerzenie, gulp, `vsce` |
| `nuget` | `nuget.exe` z nuget.org za shimem, który uruchamia go pod Mono; Ubuntu 24.04 nie ma pakietu `nuget` | restore projektów v1 |
| nanoff 2.5.162 | globalne narzędzie .NET w `/opt/dotnet-tools`, kopiowane do `~/.dotnet/tools` przy pierwszym starcie | wymagane przez rozszerzenie, które przy każdym starcie uruchamia `dotnet tool update nanoff` |
| git, curl, python3, file, unzip, sudo | archiwum Ubuntu | |

Nie ma CMake ani cross-toolchainów: natywny CLR buduje się w usłudze `posix`. Trzecia usługa, `full` ([`Dockerfile.FULL`](../../../.devcontainer/POSIX/Dockerfile.FULL), profil `full`, `NANOCLR_FULL_IMAGE`), ma oba w jednym obrazie, z montowaniami jak w `builder`: `docker compose -f .devcontainer/POSIX/docker-compose.yml run --rm full`.

Kontener działa jako użytkownik `nano` (UID/GID 1000, sudo bez hasła), `HOME=/home/nano`, katalog roboczy `/workspace`. Mostek oczekuje repozytoriów obok siebie pod `/workspace`, więc klonuj je do katalogu, który wskazuje `NF_WORKSPACE` (zob. [Przygotowanie od zera](#przygotowanie-od-zera)).

Entrypoint `nf-entrypoint.sh` przy pierwszym starcie kopiuje nanoff z `/opt/dotnet-tools` do `~/.dotnet/tools`. Narzędzi zainstalowanych w warstwie obrazu nie da się przenieść w trakcie działania na overlayfs (`EXDEV`, „Invalid cross-device link"), a `dotnet tool update` je przenosi, więc aktualizacja nanoffa przez rozszerzenie padałaby za każdym razem. Gdy entrypoint jest nadpisany (np. `docker run --entrypoint`), nanoff nadal działa z `/opt/dotnet-tools`, który zostaje w `PATH`, ale nie da się go zaktualizować.

## Przygotowanie od zera

Mostek szuka biblioteki debuggera w `../nf-debugger-local-fixes` względem katalogu głównego nf-VSCodeExtension, więc oba muszą leżeć obok siebie:

```bash
cd /workspace

# nf-debugger z poprawkami, jako worktree obok rozszerzenia
git clone https://github.com/nanoframework/nf-debugger.git
git -C nf-debugger remote add wielebny https://github.com/Wielebny666/nf-debugger.git
git -C nf-debugger fetch wielebny
git -C nf-debugger worktree add ../nf-debugger-local-fixes -b fix/tcpip-transport-and-build wielebny/fix/tcpip-transport-and-build

# rozszerzenie
git clone https://github.com/nanoframework/nf-VSCodeExtension.git
git -C nf-VSCodeExtension remote add wielebny https://github.com/Wielebny666/nf-VSCodeExtension.git
git -C nf-VSCodeExtension fetch wielebny
git -C nf-VSCodeExtension checkout -b exception-breakpoints wielebny/exception-breakpoints
```

Zwykły klon forka do `/workspace/nf-debugger-local-fixes` też działa; worktree oszczędza tylko drugą kopię historii. Liczy się nazwa katalogu i to, że jest w nim wybrana gałąź `fix/tcpip-transport-and-build`.

Żeby użyć biblioteki debuggera z innego miejsca, ustaw właściwość `NfDebuggerProject` na pełną ścieżkę `nanoFramework.Tools.DebugLibrary.Net.csproj`. `npx gulp build-debug-bridge` nie przekazuje właściwości, ale MSBuild czyta też zmienne środowiskowe jako właściwości, więc działa `NfDebuggerProject=/ścieżka/do/...csproj npx gulp build-debug-bridge`.

## Budowanie

W `/workspace/nf-VSCodeExtension`:

```bash
npm install                                    # uruchamia też 'gulp build', który buduje mostek
pwsh scripts/build.ps1                         # SDK nanoFramework v1.0 i v2.0, szablony, task MetadataProcessora → dist/utils
npx vsce package --out ../vscode-nanoframework-local.vsix --allow-star-activation
```

- `scripts/build.ps1` ściąga pakiety rozszerzenia VS2022 v1 i v2 oraz task MetadataProcessora; pakiet v1 i task są przypięte przez SHA-256, pakiet v2 to najnowszy `2022.14.2.*` z VSIX Gallery. Trzeba go uruchomić ponownie tylko wtedy, gdy nie ma `dist/utils`, po `-Clean` albo gdy skrypt zmienia wersje.
- `vsce package` najpierw uruchamia `vscode:prepublish`, czyli `gulp build-debug-bridge` (`dotnet publish` mostka do `bin/nanoDebugBridge`, który najpierw jest usuwany) i `tsc`. Pakiet ma potem rozszerzenie w `out/`, mostek w `bin/nanoDebugBridge/` i SDK w `dist/utils/`. Całość trwa około 15 s, gdy wszystko jest już odtworzone.
- Nie używaj `npm run package`: jest dla CI. Wpisuje wersję z nbgv do `package.json` i publikuje zmienne CI.
- Gdy zmienił się tylko TypeScript, do uruchomienia rozszerzenia ze źródeł przez F5 („Run Extension") wystarczy `npm run compile` (albo `npm run watch`).

Zainstaluj pakiet w VS Code i przeładuj okno:

```bash
code --install-extension ../vscode-nanoframework-local.vsix --force
```

W dev containerze instaluje to rozszerzenie w serwerze VS Code kontenera.

## Po zmianie gałęzi debuggera

Nic nie przebudowuje mostka samo. Po commicie albo rebase'ie w `/workspace/nf-debugger-local-fixes`:

```bash
cd /workspace/nf-VSCodeExtension
npx gulp build-debug-bridge      # albo zbuduj cały pakiet jeszcze raz przez 'npx vsce package ...'
```

`npm run compile` nie przebudowuje mostka.

## Sprawdzenie, z czego zbudowano mostek

Po każdym buildzie:

```bash
cd /workspace/nf-VSCodeExtension/bin/nanoDebugBridge

# 'project' = zbudowany z lokalnego checkoutu, 'package' = z NuGeta (bez poprawek)
python3 -c "import json; d=json.load(open('nanoFramework.Tools.DebugBridge.deps.json')); print({k: v['type'] for k, v in d['libraries'].items() if 'Debugger' in k})"

# wersja wpisana w bibliotekę kończy się commitem, z którego ją zbudowano
strings nanoFramework.Tools.DebugLibrary.Net.dll | grep -m1 -E '^[0-9]+\.[0-9]+\.[0-9]+\+[0-9a-f]+'
git -C /workspace/nf-debugger-local-fixes rev-parse --short=10 HEAD
```

Poprawny wynik to `{'nanoFramework.Tools.Debugger.Net/1.0.0': 'project'}` i wersja typu `2.5.31+4db8afc7f0`, której końcówka to `HEAD` worktree.

## Testy

```bash
cd /workspace/nf-VSCodeExtension
npx tsc --noEmit -p .          # TypeScript
npm run lint                   # oczekiwane 0 błędów; ostrzeżenia (dziś 180) są z upstreamu

# testy mostka, na Linuksie tylko z tymi dwoma nadpisaniami (zob. niżej)
dotnet run --project src/debugger/bridge/dotnet/nanoFramework.Tools.DebugBridge.Tests \
    -p:RuntimeIdentifier=linux-x64 -p:SelfContained=false
```

Testy mostka wypisują `PASS:` albo `FAIL:` dla każdego przypadku; jest ich 8.

`nanoFramework.Tools.DebugBridge.Tests.csproj` ma `RuntimeIdentifier=win-x64` i `SelfContained=true`. Samodzielny plik wykonywalny nie może odwoływać się do mostka, który jest plikiem wykonywalnym zależnym od zainstalowanego .NET, i build staje z `NETSDK1150` (ta reguła nie zależy od systemu). Dwa nadpisania powyżej to obchodzą bez zmiany pliku; usunięcie tych dwóch linii z csproj naprawia to na stałe i wtedy wszystkie 8 testów przechodzi zwykłym `dotnet run`.

## Aktualizacja gałęzi do upstreamu

nf-debugger:

```bash
cd /workspace/nf-debugger-local-fixes
git fetch origin
git branch -f backup/tcpip-pre-rebase HEAD          # zachowaj poprzedni stan
git rebase origin/main
dotnet build nanoFramework.Tools.DebugLibrary.Net -c Release
git push --force-with-lease wielebny fix/tcpip-transport-and-build
```

nf-VSCodeExtension: to samo z `origin/main` i gałęzią `exception-breakpoints` (`git push --force-with-lease wielebny exception-breakpoints`), potem jeszcze raz zbuduj pakiet.

Upstream aktywnie zmienia części, których dotykają te gałęzie: ostatni rebase miał konflikt w `PortTcpIp/DeviceWatcher.cs` i `PortTcpIpManager.cs` z #404. Gdy upstream rozwiązuje ten sam problem inaczej, weź wersję z upstreamu, zostaw tylko to, czego ona nie pokrywa, i napisz o tym w opisie commita.

## Napotkane problemy

| Objaw | Przyczyna |
|---|---|
| Poprawki TCP nie działają, mostek zachowuje się jak upstream | mostek zbudowano z NuGeta: `nf-debugger-local-fixes` nie leży obok rozszerzenia albo nazywa się inaczej; sprawdź `deps.json` |
| `nanoFramework.Tools.DebugLibrary.Net` nie buduje się na Linuksie, brakuje pliku `TiCC32xx.TargetCapabilities.cs` | gałąź bez `4db8afc` |
| `NETSDK1150` przy uruchamianiu testów mostka | `win-x64` + `SelfContained` w projekcie testów, zob. [Testy](#testy) |
| Projekt v2 nie buduje się na Linuksie, błąd wersji języka C# | rozszerzenie bez `ebc5743`: użyto msbuild z Mono |
| Sprawdzanie zgodności deploymentu kończy się „Unsupported shared assembly-name token" | rozszerzenie bez `4ded7cf` |
| Mostek się nie uruchamia | brak runtime .NET 10 tam, gdzie działa rozszerzenie |
| `build.ps1` pada na ściąganiu | brak dostępu do sieci albo VSIX Gallery zwróciła dla v2 wersję spoza `2022.14.2.*` |
