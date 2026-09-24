//
// Copyright (c) .NET Foundation and Contributors
// See LICENSE file in the project root for full license information.
//

// Wire Protocol HAL glue for the POSIX host target.
//
// There is no physical channel here. The transport lives in the managed host
// (nanoFramework.nanoCLR.Host), which registers a pair of callbacks through
// nanoCLR_SetWireProtocolReceiveCallback / ...TransmitCallback. These three
// functions override the weak defaults in src/CLR/WireProtocol and forward
// bytes to those callbacks, mirroring the win32 virtual device.

#include <cstring>
#include <vector>

#include <nanoCLR_native.h>
#include <WireProtocol.h>
#include <WireProtocol_HAL_Interface.h>

void WP_ReceiveBytes(uint8_t **ptr, uint32_t *size)
{
    if (ptr == nullptr || size == nullptr || *size == 0)
    {
        return;
    }

    if (g_WireProtocolReceiveCallback == nullptr)
    {
        return;
    }

    const int received = g_WireProtocolReceiveCallback(*ptr, *size);

    if (received > 0)
    {
        *ptr += received;
        *size -= (uint32_t)received;
    }
}

uint8_t WP_TransmitMessage(WP_Message *message)
{
    if (message == nullptr || g_WireProtocolTransmitCallback == nullptr)
    {
        return false;
    }

    const size_t headerSize = sizeof(message->m_header);
    const size_t payloadSize = message->m_header.m_size;

    // header and payload have to reach the transport as a single write
    std::vector<uint8_t> data(headerSize + payloadSize);
    std::memcpy(data.data(), &message->m_header, headerSize);

    if (payloadSize > 0 && message->m_payload != nullptr)
    {
        std::memcpy(data.data() + headerSize, message->m_payload, payloadSize);
    }

    g_WireProtocolTransmitCallback(data.data(), data.size());

    return true;
}

void WP_CheckAvailableIncomingData()
{
    // Nothing to poll: the managed host pushes bytes through the receive callback.
}
