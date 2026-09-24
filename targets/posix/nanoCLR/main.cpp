//
// Copyright (c) .NET Foundation and Contributors
// See LICENSE file in the project root for full license information.
//

// Test harness / standalone runner for nanoFramework.nanoCLR.dylib / .so.
// Calls through the public exported API — no direct CLR symbols.
//
// Usage:
//   nanoFramework.nanoCLR.test [--assemblies] file1.pe [file2.pe ...]
//
// All positional arguments (and arguments after --assemblies) that end in
// .pe are treated as managed assembly files to load before running the CLR.
// The CLR resolves references and executes the entry-point assembly.
// Assemblies must be provided in dependency order (mscorlib.pe first).

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>
#include <string>

#include "nanoCLR_native.h"

#if defined(NANOCLR_ENABLE_SOURCELEVELDEBUGGING)
#include "TcpWireProtocol.h"
#endif

namespace
{
    namespace fs = std::filesystem;

    // Expand a directory argument into the .pe files it holds.
    //
    // Saves every caller from tracking which assemblies a project currently
    // references: point at the output directory and whatever is there gets
    // loaded. mscorlib is pulled to the front because everything references it.
    bool CollectFromDirectory(const std::string &directory, std::vector<std::string> &peFiles)
    {
        std::error_code ec;
        std::vector<std::string> found;

        for (const auto &entry : fs::directory_iterator(directory, ec))
        {
            if (!entry.is_regular_file())
                continue;

            if (entry.path().extension() != ".pe")
                continue;

            found.push_back(entry.path().string());
        }

        if (ec)
        {
            std::cerr << "error: cannot read directory '" << directory << "': " << ec.message() << "\n";
            return false;
        }

        if (found.empty())
        {
            std::cerr << "error: no .pe files in '" << directory << "'\n";
            return false;
        }

        std::sort(found.begin(), found.end());

        const auto mscorlib = std::find_if(found.begin(), found.end(), [](const std::string &path) {
            return fs::path(path).filename() == "mscorlib.pe";
        });

        if (mscorlib != found.end())
        {
            std::rotate(found.begin(), mscorlib, mscorlib + 1);
        }

        peFiles.insert(peFiles.end(), found.begin(), found.end());

        return true;
    }

    void PrintUsage()
    {
        std::cout
            << "Usage: nanoFramework.nanoCLR.test [options] [--assemblies] <file.pe|directory> ...\n"
            << "\n"
            << "Assemblies are loaded in the order given; mscorlib.pe first, the\n"
            << "application last. A directory is expanded into the .pe files it\n"
            << "holds, sorted, with mscorlib.pe moved to the front.\n"
            << "\n"
            << "Options:\n"
            << "  --forcegc                   collect before every allocation\n"
            << "  --compactionaftergc         compact the heap after every collection\n"
            << "  --waitfordebugger           wait for a debugger before running\n"
            << "  --loopafterexit             stay in the debugger loop after the program exits\n"
            << "  --maxcontextswitches <n>    scheduler quantum (default 50)\n"
#if defined(NANOCLR_ENABLE_SOURCELEVELDEBUGGING)
            << "  --networkport <n>           expose the debugger on tcpip://<host>:<n>\n"
            << "  --host <address>            address announced to debuggers (default 127.0.0.1)\n"
            << "  --broadcastport <n>         discovery port, 0 disables (default 23657)\n"
            << "  --broadcastaddress <addr>   where to announce (default 255.255.255.255)\n"
            << "  --announceinterval <s>      repeat the announcement, 0 for once (default 5)\n"
            << "  --flashimage <path>         keep the simulated flash in this file\n"
#endif
            << "  -h, --help                  this text\n";
    }

    void PrintBanner()
    {
        std::cout << ".NET nanoFramework nanoCLR " << NANOCLR_PLATFORM_NAME
                  << " v" << NANOCLR_POSIX_VERSION_STRING << "\n";
        std::cout << "Copyright (c) .NET Foundation and Contributors\n\n";
    }

    // Read a file into a byte vector. Returns false on error.
    bool ReadFile(const char *path, std::vector<uint8_t> &out)
    {
        std::ifstream in(path, std::ios::binary | std::ios::ate);
        if (!in.is_open())
        {
            std::cerr << "error: cannot open '" << path << "'\n";
            return false;
        }

        auto size = in.tellg();
        if (size <= 0)
        {
            std::cerr << "error: empty file '" << path << "'\n";
            return false;
        }

        in.seekg(0);
        out.resize(static_cast<size_t>(size));
        const auto bytesToRead = static_cast<std::streamsize>(out.size());
        if (!in.read(reinterpret_cast<char *>(out.data()), bytesToRead))
        {
            std::cerr << "error: failed to read '" << path << "'\n";
            return false;
        }

        return true;
    }

    // Convert a narrow (ASCII) path to a char16_t string.
    // char16_t matches the 2-byte UTF-16 LE units that nanoCLR_LoadAssembly expects
    // (same layout as CharSet.Unicode marshalling from the C# host).
    std::u16string ToChar16(const char *s)
    {
        return std::u16string(s, s + std::strlen(s));
    }

} // namespace

int main(int argc, char **argv)
{
    PrintBanner();

    // ── Command line ─────────────────────────────────────────────────────────
    // Accepts:  [options] [--assemblies] file.pe file2.pe ...
    // The --assemblies flag is accepted but not required (mirrors nanoclr CLI),
    // and the option names match the equivalent nanoclr ones.
    std::vector<std::string> peFiles;
    int networkPort = 0;
    int broadcastPort = 23657;
    std::string broadcastAddress = "255.255.255.255";
    int announceInterval = 5;
    std::string networkHost = "127.0.0.1";

    NANO_CLR_SETTINGS settings{};
    settings.MaxContextSwitches = 50;
    settings.WaitForDebugger = false;
    settings.EnterDebuggerLoopAfterExit = false;
    settings.PerformGarbageCollection = false;
    settings.PerformHeapCompaction = false;

    for (int i = 1; i < argc; i++)
    {
        const std::string arg(argv[i]);

        if (arg == "--assemblies")
        {
            continue; // accepted for symmetry with the CLI, carries no value
        }

        if (arg == "--help" || arg == "-h")
        {
            PrintUsage();
            return 0;
        }

        if (arg == "--forcegc")
        {
            settings.PerformGarbageCollection = true;
            continue;
        }

        if (arg == "--compactionaftergc")
        {
            // Without this the CLR never compacts unless managed code asks it
            // to, and since the 2.0 API dropped the compacting overload of
            // GC.Run there is no way to ask from managed code at all.
            settings.PerformHeapCompaction = true;
            continue;
        }

        if (arg == "--waitfordebugger")
        {
            settings.WaitForDebugger = true;
            continue;
        }

        if (arg == "--loopafterexit")
        {
            settings.EnterDebuggerLoopAfterExit = true;
            continue;
        }

#if defined(NANOCLR_ENABLE_SOURCELEVELDEBUGGING)
        if (arg == "--networkport")
        {
            if (i + 1 >= argc)
            {
                std::cerr << "error: --networkport needs a value\n";
                return 1;
            }

            networkPort = std::atoi(argv[++i]);
            continue;
        }
#endif

#if defined(NANOCLR_ENABLE_SOURCELEVELDEBUGGING)
        if (arg == "--host")
        {
            if (i + 1 >= argc)
            {
                std::cerr << "error: --host needs a value\n";
                return 1;
            }

            networkHost = argv[++i];
            continue;
        }

        if (arg == "--broadcastaddress")
        {
            if (i + 1 >= argc)
            {
                std::cerr << "error: --broadcastaddress needs a value\n";
                return 1;
            }

            broadcastAddress = argv[++i];
            continue;
        }

        if (arg == "--announceinterval")
        {
            if (i + 1 >= argc)
            {
                std::cerr << "error: --announceinterval needs a value\n";
                return 1;
            }

            announceInterval = std::atoi(argv[++i]);
            continue;
        }

        if (arg == "--broadcastport")
        {
            if (i + 1 >= argc)
            {
                std::cerr << "error: --broadcastport needs a value\n";
                return 1;
            }

            broadcastPort = std::atoi(argv[++i]);
            continue;
        }

        if (arg == "--flashimage")
        {
            if (i + 1 >= argc)
            {
                std::cerr << "error: --flashimage needs a value\n";
                return 1;
            }

            // The flash lives in the library, on the far side of a boundary that
            // exports only the documented nanoCLR_ API, so the path is handed over
            // the way the heap size already is.
            setenv("NANOCLR_FLASH_IMAGE", argv[++i], 1);
            continue;
        }
#endif

        if (arg == "--maxcontextswitches")
        {
            if (i + 1 >= argc)
            {
                std::cerr << "error: --maxcontextswitches needs a value\n";
                return 1;
            }

            settings.MaxContextSwitches = (unsigned short)std::atoi(argv[++i]);
            continue;
        }

        if (arg.size() >= 3 && arg.compare(arg.size() - 3, 3, ".pe") == 0)
        {
            peFiles.push_back(arg);
            continue;
        }

        std::error_code ec;
        if (fs::is_directory(arg, ec))
        {
            if (!CollectFromDirectory(arg, peFiles))
                return 1;

            continue;
        }

        std::cerr << "error: unexpected argument '" << arg
                  << "' (expected an option, a .pe file or a directory)\n";
        PrintUsage();
        return 1;
    }

    // ── Load assemblies into the CLR before starting ─────────────────────────
    for (const auto &path : peFiles)
    {
        std::vector<uint8_t> data;
        if (!ReadFile(path.c_str(), data))
            return 1;

        std::u16string wname = ToChar16(path.c_str());
        int hr = nanoCLR_LoadAssembly(wname.c_str(), data.data(), data.size());
        if (hr != 0)
        {
            std::cerr << "error: failed to load '" << path
                      << "' (hr=0x" << std::hex << hr << ")\n";
            return 1;
        }

        std::cout << "Loaded: " << path << "\n";
    }

    if (!peFiles.empty())
        std::cout << "\n";

    // ── Run the CLR ──────────────────────────────────────────────────────────
#if defined(NANOCLR_ENABLE_SOURCELEVELDEBUGGING)
    if (networkPort > 0 && !TcpWireProtocol_Start(
            networkPort,
            networkHost.c_str(),
            broadcastPort,
            broadcastAddress.c_str(),
            announceInterval))
    {
        return 1;
    }
#endif

    nanoCLR_Run(settings);

#if defined(NANOCLR_ENABLE_SOURCELEVELDEBUGGING)
    if (networkPort > 0)
    {
        TcpWireProtocol_Stop();
    }
#endif

    return 0;
}
