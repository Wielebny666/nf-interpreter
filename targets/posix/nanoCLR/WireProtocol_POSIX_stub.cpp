//
// Copyright (c) .NET Foundation and Contributors
// See LICENSE file in the project root for full license information.
//

// Wire Protocol stubs – the POSIX shared library exposes the WireProtocol API
// but does not connect a real transport.  These stubs satisfy the link.

#include <nanoCLR_Runtime.h>

extern "C" void WP_Message_PrepareReception()
{
    // No-op: no wire protocol transport on POSIX host.
}

extern "C" void WP_Message_Process()
{
    // No-op: no wire protocol transport on POSIX host.
}
