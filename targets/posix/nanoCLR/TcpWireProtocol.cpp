//
// Copyright (c) .NET Foundation and Contributors
// See LICENSE file in the project root for full license information.
//

// TCP transport for the Wire Protocol, used by the test harness.
//
// The CLR itself carries no transport: it hands bytes to whatever the host
// registers through nanoCLR_SetWireProtocolReceiveCallback and
// ...TransmitCallback. On Windows and on 64-bit hosts that host is the managed
// nanoclr tool, which brings its own TCP, serial and named pipe ports. A 32-bit
// POSIX build has no managed host to lean on - .NET has no linux-x86 runtime -
// so the transport lives here.
//
// The debugger client connects with a tcpip://host:port device path.

#include "TcpWireProtocol.h"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <string>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <nanoCLR_native.h>

namespace
{
std::atomic<int> s_listenSocket{-1};
std::atomic<int> s_clientSocket{-1};
std::atomic<bool> s_stop{false};
std::thread s_thread;

// NANOCLR_WP_TRACE=1 reports what crosses the transport, which is the only
// way to tell a silent client from a silent CLR.
bool s_trace = false;
std::atomic<unsigned long> s_bytesIn{0};
std::atomic<unsigned long> s_bytesOut{0};
std::atomic<unsigned long> s_pollCount{0};
std::atomic<unsigned long> s_rxPackets{0};
std::atomic<unsigned long> s_txPackets{0};

// Where the connection that is open now started, so closing it can report what
// it cost. A client that wants to know why a deployment took the time it did
// needs the totals more than it needs the hex dump, and the totals are cheap
// enough to keep without being asked.
struct ConnectionMark
{
    unsigned long atMs;
    unsigned long bytesIn;
    unsigned long bytesOut;
    unsigned long rxPackets;
    unsigned long txPackets;
    unsigned long polls;
};

ConnectionMark s_mark{};

// milliseconds since the transport started, so the trace shows where time goes
unsigned long TraceClock()
{
    static const auto start = std::chrono::steady_clock::now();
    return (unsigned long)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
}
std::string s_host;
int s_port = 0;
int s_broadcastPort = 0;
int s_connectionCount = 0;
std::string s_broadcastAddress;
int s_announceInterval = 0;
std::thread s_announceThread;
std::thread s_processThread;

void CloseConnection()
{
    const int client = s_clientSocket.exchange(-1);

    if (client < 0)
    {
        // already closed; nothing to report a second time
        return;
    }

    close(client);

    const unsigned long now = TraceClock();
    const unsigned long elapsed = now - s_mark.atMs;
    const unsigned long rx = s_bytesIn.load() - s_mark.bytesIn;
    const unsigned long tx = s_bytesOut.load() - s_mark.bytesOut;
    const unsigned long rxPackets = s_rxPackets.load() - s_mark.rxPackets;
    const unsigned long txPackets = s_txPackets.load() - s_mark.txPackets;
    const unsigned long polls = s_pollCount.load() - s_mark.polls;

    // One line per session, always. It is what tells a client whether its time
    // went on the wire or on waiting, and it costs nothing to produce.
    std::printf(
        "[%6lu ms] WP: connection #%d closed after %lu ms | rx %lu bytes in %lu packets"
        " | tx %lu bytes in %lu packets | %lu polls",
        now,
        s_connectionCount,
        elapsed,
        rx,
        rxPackets,
        tx,
        txPackets,
        polls);

    if (elapsed > 0)
    {
        std::printf(" | %lu bytes/s in", (rx * 1000) / elapsed);
    }

    std::printf("\n");
    std::fflush(stdout);
}

// The CLR polls for input, so a read must never block the state machine.
int ReceiveBytes(const uint8_t *data, size_t size)
{
    s_pollCount.fetch_add(1);

    const int client = s_clientSocket.load();

    if (client < 0)
    {
        // No debugger attached. The protocol loop keeps running so that the
        // next one is answered at once, but without a pause here it would spin
        // a core flat out for nothing.
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return 0;
    }

    if (size == 0)
    {
        return 0;
    }

    // the buffer is ours to fill; the const is an artefact of the shared
    // callback signature, which describes the transmit direction
    const ssize_t received = recv(client, const_cast<uint8_t *>(data), size, MSG_DONTWAIT);

    if (received > 0)
    {
        const unsigned long total = s_bytesIn.fetch_add((unsigned long)received) + received;
        s_rxPackets.fetch_add(1);

        if (s_trace)
        {
            std::printf("[%6lu ms] WP rx: %d bytes (total %lu) |", TraceClock(), (int)received, total);

            for (ssize_t i = 0; i < received && i < 40; i++)
            {
                std::printf(" %02x", data[i]);
            }

            std::printf("\n");
            std::fflush(stdout);
        }

        return (int)received;
    }

    if (received == 0)
    {
        // Orderly shutdown by the debugger. Tell the CLR to leave its
        // processing loop as well, otherwise it keeps polling a dead socket and
        // the accept loop never gets to take the next connection - and clients
        // do reconnect: the watcher probes first and the debug engine follows.
        CloseConnection();
    }

    return 0;
}

int TransmitBytes(const uint8_t *data, size_t size)
{
    const int client = s_clientSocket.load();

    if (client < 0 || size == 0)
    {
        return 0;
    }

    size_t sent = 0;

    while (sent < size)
    {
        const ssize_t written = send(client, data + sent, size - sent, MSG_NOSIGNAL);

        if (written <= 0)
        {
            if (errno == EINTR)
            {
                continue;
            }

            CloseConnection();

            return (int)sent;
        }

        sent += (size_t)written;
    }

    const unsigned long total = s_bytesOut.fetch_add((unsigned long)sent) + sent;
    s_txPackets.fetch_add(1);

    if (s_trace)
    {
        std::printf("[%6lu ms] WP tx: %u bytes (total %lu) |", TraceClock(), (unsigned)sent, total);

        for (size_t i = 0; i < size && i < 48; i++)
        {
            std::printf(" %02x", data[i]);
        }

        std::printf(" | \"");

        for (size_t i = 0; i < size && i < 8; i++)
        {
            std::printf("%c", (data[i] >= 32 && data[i] < 127) ? (char)data[i] : '.');
        }

        std::printf("\"\n");
        std::fflush(stdout);
    }

    return (int)sent;
}

// Discovery, as the managed host does it: a single UDP datagram on the debug
// broadcast port saying "+:host:port" when the device comes up and "-:host:port"
// when it goes away. Without this a debugger has no way to find the device,
// because its client only connects to what its watcher has enumerated.
void Announce(const char *command, const char *host, int port, int broadcastPort, const char *broadcastAddress)
{
    const int sock = socket(AF_INET, SOCK_DGRAM, 0);

    if (sock < 0)
    {
        return;
    }

    int one = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));

    char message[80];
    const int length = std::snprintf(message, sizeof(message), "%s:%s:%d", command, host, port);

    sockaddr_in target{};
    target.sin_family = AF_INET;
    target.sin_port = htons((uint16_t)broadcastPort);

    // Exactly one datagram. Sending a second copy elsewhere makes the watcher
    // see two arrivals of the same device and run two validations against each
    // other, so the address is a choice rather than a list. The limited
    // broadcast reaches listeners on the same link; across a container network
    // a subnet-directed address such as 172.25.255.255 may be needed instead.
    if (inet_pton(AF_INET, broadcastAddress, &target.sin_addr) != 1)
    {
        target.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    }
    sendto(sock, message, (size_t)length, 0, (sockaddr *)&target, sizeof(target));

    close(sock);

    if (s_trace)
    {
        std::printf("WP announce: %s\n", message);
        std::fflush(stdout);
    }
}

void AcceptLoop(int port)
{
    while (!s_stop.load())
    {
        sockaddr_in address{};
        socklen_t addressLength = sizeof(address);

        const int client = accept(s_listenSocket.load(), (sockaddr *)&address, &addressLength);

        if (client < 0)
        {
            if (s_stop.load())
            {
                break;
            }

            continue;
        }

        // debugger traffic is small and latency sensitive
        int one = 1;
        setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        std::printf("[%6lu ms] Wire Protocol: debugger connected from %s (connection #%d)\n",
                    TraceClock(),
                    inet_ntoa(address.sin_addr),
                    ++s_connectionCount);
        std::fflush(stdout);

        // Hand the socket to the protocol loop, which is already running, and
        // reset reception for the newcomer. Waiting for that loop to finish
        // before accepting was what made every debugger after the first fail:
        // the state machine takes up to its header timeout to notice a closed
        // connection, and the next client sat unanswered in the backlog until
        // its own timeout expired.
        const int previous = s_clientSocket.exchange(client);

        if (previous >= 0)
        {
            close(previous);
        }

        s_mark = {
            TraceClock(),
            s_bytesIn.load(),
            s_bytesOut.load(),
            s_rxPackets.load(),
            s_txPackets.load(),
            s_pollCount.load(),
        };

        nanoCLR_WireProtocolOpen();
    }
}
} // namespace

bool TcpWireProtocol_Start(
    int port,
    const char *host,
    int broadcastPort,
    const char *broadcastAddress,
    int announceInterval)
{
    const int listenSocket = socket(AF_INET, SOCK_STREAM, 0);

    if (listenSocket < 0)
    {
        std::fprintf(stderr, "error: cannot create a socket: %s\n", std::strerror(errno));
        return false;
    }

    int one = 1;
    setsockopt(listenSocket, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons((uint16_t)port);

    if (bind(listenSocket, (sockaddr *)&address, sizeof(address)) < 0)
    {
        std::fprintf(stderr, "error: cannot bind port %d: %s\n", port, std::strerror(errno));
        close(listenSocket);
        return false;
    }

    // A debugger session is one connection, but a client reaches the device
    // through several in quick succession - discovery, a validation probe, then
    // the session itself. With a backlog of one, the spare SYN is dropped and
    // the client reports the device as missing, so leave room for the queue.
    if (listen(listenSocket, 4) < 0)
    {
        std::fprintf(stderr, "error: cannot listen on port %d: %s\n", port, std::strerror(errno));
        close(listenSocket);
        return false;
    }

    s_listenSocket.store(listenSocket);
    s_stop.store(false);
    // Presence alone used to be enough to turn tracing on, which left no way to
    // ask for it off: a launch configuration that passes the variable at all got
    // a hex dump per packet, and that is the last thing a timing run wants.
    const char *trace = std::getenv("NANOCLR_WP_TRACE");
    s_trace = (trace != nullptr && *trace != '\0' && std::strcmp(trace, "0") != 0);

    nanoCLR_SetWireProtocolReceiveCallback(ReceiveBytes);
    nanoCLR_SetWireProtocolTransmitCallback(TransmitBytes);

    // one protocol loop for the life of the transport, connections come and go
    nanoCLR_WireProtocolOpen();
    s_processThread = std::thread([]() { nanoCLR_WireProtocolProcess(); });

    s_thread = std::thread(AcceptLoop, port);

    std::printf("Wire Protocol: listening on tcpip://%s:%d\n", host, port);
    std::fflush(stdout);

    if (broadcastPort > 0)
    {
        s_host = host;
        s_port = port;
        s_broadcastPort = broadcastPort;
        s_broadcastAddress = broadcastAddress;

        Announce("+", host, port, broadcastPort, broadcastAddress);

        // A single datagram only finds a debugger that was already listening,
        // which turns starting the two sides into a coordination problem.
        // Repeating it costs nothing: the watcher keys devices by address and
        // ignores an arrival it already knows.
        if (announceInterval > 0)
        {
            s_announceInterval = announceInterval;
            s_announceThread = std::thread([]() {
                while (!s_stop.load())
                {
                    for (int elapsed = 0; elapsed < s_announceInterval && !s_stop.load(); elapsed++)
                    {
                        std::this_thread::sleep_for(std::chrono::seconds(1));
                    }

                    if (!s_stop.load())
                    {
                        Announce("+", s_host.c_str(), s_port, s_broadcastPort, s_broadcastAddress.c_str());
                    }
                }
            });
        }
    }

    return true;
}

void TcpWireProtocol_Stop()
{
    if (s_broadcastPort > 0)
    {
        Announce("-", s_host.c_str(), s_port, s_broadcastPort, s_broadcastAddress.c_str());
    }

    s_stop.store(true);

    nanoCLR_WireProtocolClose();

    const int client = s_clientSocket.exchange(-1);
    if (client >= 0)
    {
        shutdown(client, SHUT_RDWR);
        close(client);
    }

    const int listenSocket = s_listenSocket.exchange(-1);
    if (listenSocket >= 0)
    {
        shutdown(listenSocket, SHUT_RDWR);
        close(listenSocket);
    }

    if (s_announceThread.joinable())
    {
        s_announceThread.join();
    }

    if (s_processThread.joinable())
    {
        s_processThread.join();
    }

    if (s_thread.joinable())
    {
        s_thread.join();
    }
}
