//
// Copyright (c) .NET Foundation and Contributors
// See LICENSE file in the project root for full license information.
//

// CLR debug output — override the weak no-op from Diagnostics_stub.cpp so
// debug messages actually appear on stdout during development.
// When a debug-print callback has been registered by the managed host
// (via nanoCLR_SetDebugPrintCallback), forward output to it as well.

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <nanoCLR_Runtime.h>
#include "nanoCLR_native.h"

void CLR_Debug::Emit(const char *text, int len)
{
    if (text == nullptr)
        return;

    if (g_DebugPrintCallback != nullptr)
    {
        if (len < 0)
        {
            g_DebugPrintCallback(text);
        }
        else
        {
            // Null-terminate a copy for the callback.
            // Use a stack buffer for short messages; fall back to heap for longer ones.
            const size_t msgLen = (size_t)len;
            if (msgLen < 512)
            {
                char buf[512];
                memcpy(buf, text, msgLen);
                buf[msgLen] = '\0';
                g_DebugPrintCallback(buf);
            }
            else
            {
                // Heap allocation avoids silent truncation for large debug payloads.
                char *buf = static_cast<char *>(malloc(msgLen + 1));
                if (buf != nullptr)
                {
                    memcpy(buf, text, msgLen);
                    buf[msgLen] = '\0';
                    g_DebugPrintCallback(buf);
                    free(buf);
                }
                else
                {
                    // Allocation failed: deliver what fits on the stack rather than dropping the message.
                    char fallback[512];
                    memcpy(fallback, text, sizeof(fallback) - 1);
                    fallback[sizeof(fallback) - 1] = '\0';
                    g_DebugPrintCallback(fallback);
                }
            }
        }
        return;
    }

    if (len < 0)
        fputs(text, stdout);
    else
        fwrite(text, 1, (size_t)len, stdout);
    fflush(stdout);
}

int CLR_Debug::PrintfV(const char *format, va_list arg)
{
    char buf[512];
    int n = vsnprintf(buf, sizeof(buf), format, arg);
    CLR_Debug::Emit(buf, -1);
    return n;
}

int CLR_Debug::Printf(const char *format, ...)
{
    va_list arg;
    va_start(arg, format);
    int n = CLR_Debug::PrintfV(format, arg);
    va_end(arg);
    return n;
}
