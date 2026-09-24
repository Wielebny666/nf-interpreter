//
// Copyright (c) .NET Foundation and Contributors
// See LICENSE file in the project root for full license information.
//

// Target-side pieces the debugger and Wire Protocol expect from a HAL,
// mirroring targets/win32/nanoCLR/Various.cpp. Only built when the debugger
// stack is enabled (NANO_POSIX_ENABLE_DEBUGGER).

#include <cstring>

#include <nanoHAL_v2.h>
#include <nanoHAL_ReleaseInfo.h>
#include <nanoPAL_BlockStorage.h>

// There is no physical channel on a POSIX host: the transport is whatever the
// managed host registers through the Wire Protocol callbacks, so the port
// handles and memory ranges are all zero.
HAL_SYSTEM_CONFIG HalSystemConfig = {
    {true}, // HAL_DRIVER_CONFIG_HEADER Header

    0,      // COM_HANDLE DebuggerPort
    0,      // COM_HANDLE DebugTextPort
    921600, // unsigned int USART_DefaultBaudRate
    0,      // COM_HANDLE stdio

    {0, 0}, // HAL_SYSTEM_MEMORY_CONFIG RAM1
    {0, 0}, // HAL_SYSTEM_MEMORY_CONFIG FLASH
};

// The debugger port is owned by the managed host, so the CLR must not tear it
// down when a program exits.
bool g_fDoNotUninitializeDebuggerPort = false;

// called from WireProtocol_Message.c, so it needs C linkage
extern "C" void WP_Message_PrepareReception_Platform()
{
    // empty on purpose, there is nothing to configure
}

void NFReleaseInfo::Init(
    NFReleaseInfo &releaseInfo,
    unsigned short int major,
    unsigned short int minor,
    unsigned short int build,
    unsigned short int revision,
    const char *info,
    size_t infoLen,
    const char *target,
    size_t targetLen,
    const char *platform,
    size_t platformLen)
{
    NFVersion::Init(releaseInfo.Version, major, minor, build, revision);

    releaseInfo.InfoString[0] = 0;
    releaseInfo.TargetName[0] = 0;
    releaseInfo.PlatformName[0] = 0;

    auto copyInto = [](unsigned char *dst, size_t dstSize, const char *src, size_t srcLen) {
        if (src == nullptr)
        {
            return;
        }

        const size_t len = (srcLen < dstSize - 1) ? srcLen : dstSize - 1;
        std::memcpy(dst, src, len);
        dst[len] = 0;
    };

    copyInto(releaseInfo.InfoString, sizeof(releaseInfo.InfoString), info, infoLen);
    copyInto(releaseInfo.TargetName, sizeof(releaseInfo.TargetName), target, targetLen);
    copyInto(releaseInfo.PlatformName, sizeof(releaseInfo.PlatformName), platform, platformLen);
}
