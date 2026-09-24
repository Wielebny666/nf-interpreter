//
// Copyright (c) .NET Foundation and Contributors
// See LICENSE file in the project root for full license information.
//

#ifndef TCP_WIRE_PROTOCOL_H
#define TCP_WIRE_PROTOCOL_H

// Exposes the Wire Protocol over TCP so a debugger can attach with a
// tcpip://host:port device path. Returns false if the port cannot be opened.
// host is the address announced to debuggers; broadcastPort 0 disables discovery.
bool TcpWireProtocol_Start(
    int port,
    const char *host,
    int broadcastPort,
    const char *broadcastAddress,
    int announceInterval);

void TcpWireProtocol_Stop();

#endif // TCP_WIRE_PROTOCOL_H
