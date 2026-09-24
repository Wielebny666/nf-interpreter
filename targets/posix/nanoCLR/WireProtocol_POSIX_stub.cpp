//
// Copyright (c) .NET Foundation and Contributors
// See LICENSE file in the project root for full license information.
//

// Wire Protocol stubs, used when the debugger stack is not built in. With
// NANO_POSIX_ENABLE_DEBUGGER the real implementation from src/CLR/WireProtocol
// provides these instead.

#include <nanoCLR_Runtime.h>

#if !defined(NANOCLR_ENABLE_SOURCELEVELDEBUGGING)

extern "C" void WP_Message_PrepareReception()
{
    // No-op: no wire protocol transport on this build.
}

extern "C" void WP_Message_Process()
{
    // No-op: no wire protocol transport on this build.
}

#endif
