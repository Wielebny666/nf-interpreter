//
// Copyright (c) .NET Foundation and Contributors
// See LICENSE file in the project root for full license information.
//

// Debug text port for the POSIX host.
//
// CLR_Debug output (Printf/Emit) and the CLR_RT_DUMP helpers come from the shared
// src/CLR/Diagnostics/Info.cpp, exactly as on a device. Info.cpp line-buffers the
// text, broadcasts it to an attached debugger as Monitor_Message, and writes it to
// HalSystemConfig.DebugTextPort through DebuggerPort_Write. On this host that port
// is stdout, or the managed host's debug-print callback when one is registered
// (nanoCLR_SetDebugPrintCallback).

#include <cstdio>
#include <cstring>
#include <string>

#include <nanoCLR_Runtime.h>
#include <nanoHAL_v2.h>
#include <nanoPAL_COM.h>
#include "nanoCLR_native.h"

// There is no physical channel on a POSIX host. The Wire Protocol transport is
// whatever the managed host registers through its callbacks (port 0), and debug
// text goes to stdout. DebugTextPort is kept different from DebuggerPort so that
// Info.cpp keeps writing text locally while a debugger is attached, instead of
// sending it only over the Wire Protocol.
HAL_SYSTEM_CONFIG HalSystemConfig = {
    {true}, // HAL_DRIVER_CONFIG_HEADER Header

    0,      // COM_HANDLE DebuggerPort
    1,      // COM_HANDLE DebugTextPort
    921600, // unsigned int USART_DefaultBaudRate
    0,      // COM_HANDLE stdio

    {0, 0}, // HAL_SYSTEM_MEMORY_CONFIG RAM1
    {0, 0}, // HAL_SYSTEM_MEMORY_CONFIG FLASH
};

#if !defined(NANOCLR_ENABLE_SOURCELEVELDEBUGGING)
// Info.cpp's Monitor_Message broadcast references the debugger instance. Without
// the debugger stack (Debugger_stub.cpp) nothing defines it, and it stays NULL:
// the broadcast is never reached because the debugger is never enabled.
CLR_DBG_Debugger *g_CLR_DBG_Debugger = NULL;
#endif

int DebuggerPort_Write(COM_HANDLE comPortNum, const char *data, size_t size, int maxRetries)
{
    (void)comPortNum;
    (void)maxRetries;

    if (data == nullptr || size == 0)
    {
        return 0;
    }

    if (g_DebugPrintCallback != nullptr)
    {
        // the callback takes a null-terminated string
        std::string text(data, size);
        g_DebugPrintCallback(text.c_str());
    }
    else
    {
        fwrite(data, 1, size, stdout);
    }

    return (int)size;
}

bool DebuggerPort_Flush(COM_HANDLE comPortNum)
{
    (void)comPortNum;

    fflush(stdout);

    return true;
}
