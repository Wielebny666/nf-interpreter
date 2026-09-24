//
// Copyright (c) .NET Foundation and Contributors
// See LICENSE file in the project root for full license information.
//

// The socket layer System.Net sits on, implemented over the host's BSD sockets.
//
// Everything above this file (NativeSocket and friends) was written against
// lwIP: every socket is non-blocking, a call that would block reports
// EWOULDBLOCK, and the managed thread then parks on Event_Socket until the
// stack says something happened. On the devices lwIP raises that event itself.
// On a host nothing does, so a monitor thread poll()s the descriptors the
// interpreter is waiting on and sets SYSTEM_EVENT_FLAG_SOCKET when one of them
// moves. That thread never touches CLR state; Events_Set is all it calls.

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <nanoCLR_Runtime.h>
#include <nanoHAL_v2.h>

#if !defined(NANOCLR_POSIX_SOCKETS)
#error "Sockets_POSIX.cpp needs NANOCLR_POSIX_SOCKETS so that nanoPAL.h pulls in the socket declarations."
#endif

namespace
{
// The last failure, already translated to the SOCK_E* numbering the managed
// SocketException carries. Per thread because the interpreter and the DNS
// workers must not overwrite each other's.
thread_local int t_lastError = 0;

int MapError(int error)
{
    switch (error)
    {
        case EINTR:
            return SOCK_EINTR;
        case EBADF:
            return SOCK_EBADF;
        case EACCES:
        case EPERM:
            return SOCK_EACCES;
        case EFAULT:
            return SOCK_EFAULT;
        case EINVAL:
            return SOCK_EINVAL;
        case EMFILE:
        case ENFILE:
            return SOCK_EMFILE;
        case EAGAIN: // == EWOULDBLOCK
        case EINPROGRESS:
            return SOCK_EWOULDBLOCK;
        case EALREADY:
            return SOCK_EALREADY;
        case ENOTSOCK:
            return SOCK_ENOTSOCK;
        case EDESTADDRREQ:
            return SOCK_EDESTADDRREQ;
        case EMSGSIZE:
            return SOCK_EMSGSIZE;
        case EPROTOTYPE:
            return SOCK_EPROTOTYPE;
        case ENOPROTOOPT:
            return SOCK_ENOPROTOOPT;
        case EPROTONOSUPPORT:
            return SOCK_EPROTONOSUPPORT;
        case EOPNOTSUPP:
            return SOCK_EOPNOTSUPP;
        case EAFNOSUPPORT:
            return SOCK_EAFNOSUPPORT;
        case EADDRINUSE:
            return SOCK_EADDRINUSE;
        case EADDRNOTAVAIL:
            return SOCK_EADDRNOTAVAIL;
        case ENETDOWN:
            return SOCK_ENETDOWN;
        case ENETUNREACH:
            return SOCK_ENETUNREACH;
        case ENETRESET:
            return SOCK_ENETRESET;
        case ECONNABORTED:
            return SOCK_ECONNABORTED;
        case ECONNRESET:
            return SOCK_ECONNRESET;
        case ENOBUFS:
        case ENOMEM:
            return SOCK_ENOBUFS;
        case EISCONN:
            return SOCK_EISCONN;
        case ENOTCONN:
            return SOCK_ENOTCONN;
        case ESHUTDOWN:
        case EPIPE:
            return SOCK_ESHUTDOWN;
        case ETIMEDOUT:
            return SOCK_ETIMEDOUT;
        case ECONNREFUSED:
            return SOCK_ECONNREFUSED;
        case EHOSTDOWN:
            return SOCK_EHOSTDOWN;
        case EHOSTUNREACH:
            return SOCK_EHOSTUNREACH;
        default:
            return error;
    }
}

// Record errno as the last error and hand back the value the callers expect.
int Fail(int error = errno)
{
    t_lastError = MapError(error);
    return SOCK_SOCKET_ERROR;
}

// ─── Readiness monitor ──────────────────────────────────────────────────────

// Watches the descriptors a managed thread is parked on. The interest is
// one-shot: the moment a descriptor is ready the entry is dropped and the
// interpreter is woken, and it registers again if it still has to wait.
// Level-triggered POLLOUT would otherwise spin forever on a writable socket.
class ReadinessMonitor
{
  public:
    void Watch(int fd, short events)
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);

            if (!EnsureRunning())
            {
                return;
            }

            m_interest[fd] |= events;
        }

        Poke();
    }

    void Forget(int fd)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_interest.erase(fd);
    }

    void Stop()
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);

            if (!m_running)
            {
                return;
            }

            m_stop = true;
        }

        Poke();
        m_thread.join();

        std::lock_guard<std::mutex> lock(m_mutex);
        close(m_wake[0]);
        close(m_wake[1]);
        m_interest.clear();
        m_running = false;
        m_stop = false;
    }

  private:
    // m_mutex held
    bool EnsureRunning()
    {
        if (m_running)
        {
            return true;
        }

        if (pipe(m_wake) != 0)
        {
            return false;
        }

        fcntl(m_wake[0], F_SETFL, O_NONBLOCK);
        fcntl(m_wake[1], F_SETFL, O_NONBLOCK);
        fcntl(m_wake[0], F_SETFD, FD_CLOEXEC);
        fcntl(m_wake[1], F_SETFD, FD_CLOEXEC);

        m_running = true;
        m_thread = std::thread([this]() { Run(); });
        return true;
    }

    void Poke()
    {
        const char byte = 0;

        // a full pipe already means a wake-up is pending
        (void)!write(m_wake[1], &byte, 1);
    }

    void Run()
    {
        std::vector<pollfd> fds;

        while (true)
        {
            fds.clear();
            fds.push_back({m_wake[0], POLLIN, 0});

            {
                std::lock_guard<std::mutex> lock(m_mutex);

                if (m_stop)
                {
                    return;
                }

                for (const auto &entry : m_interest)
                {
                    fds.push_back({entry.first, entry.second, 0});
                }
            }

            if (poll(fds.data(), fds.size(), -1) < 0)
            {
                if (errno == EINTR)
                {
                    continue;
                }
                return;
            }

            if (fds[0].revents != 0)
            {
                char drain[64];

                while (read(m_wake[0], drain, sizeof(drain)) > 0)
                {
                }
            }

            bool fired = false;

            {
                std::lock_guard<std::mutex> lock(m_mutex);

                for (size_t i = 1; i < fds.size(); i++)
                {
                    if (fds[i].revents != 0)
                    {
                        m_interest.erase(fds[i].fd);
                        fired = true;
                    }
                }
            }

            if (fired)
            {
                Events_Set(SYSTEM_EVENT_FLAG_SOCKET);
            }
        }
    }

    std::mutex m_mutex;
    std::unordered_map<int, short> m_interest;
    std::thread m_thread;
    int m_wake[2] = {-1, -1};
    bool m_running = false;
    bool m_stop = false;
};

ReadinessMonitor s_monitor;

// ─── Bookkeeping ────────────────────────────────────────────────────────────

// Every descriptor this layer handed out, so a CLR reboot can close what the
// managed program left open instead of leaking it into the next run.
std::mutex s_socketsMutex;
std::unordered_set<int> s_sockets;

// Reading SO_ERROR clears it, and a later poll() on the same descriptor then
// shows nothing wrong. A refused connect is noticed by whichever select gets
// there first, so the error is kept here until the socket is closed or the
// program asks for it through SO_ERROR.
std::unordered_map<int, int> s_pendingErrors;

void Track(int fd)
{
    std::lock_guard<std::mutex> lock(s_socketsMutex);
    s_sockets.insert(fd);
}

bool Untrack(int fd)
{
    std::lock_guard<std::mutex> lock(s_socketsMutex);
    s_pendingErrors.erase(fd);
    return s_sockets.erase(fd) != 0;
}

// ─── Address conversion ─────────────────────────────────────────────────────

// Only IPv4 for now: SOCK_sockaddr is 16 bytes without LWIP_IPV6, exactly as it
// is on the devices that build without it.
bool ToNative(const SOCK_sockaddr *from, int fromLen, sockaddr_in &to)
{
    if (from == nullptr || fromLen < (int)sizeof(SOCK_sockaddr_in) || from->sa_family != SOCK_AF_INET)
    {
        return false;
    }

    const SOCK_sockaddr_in *in = reinterpret_cast<const SOCK_sockaddr_in *>(from);

    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = in->sin_port;
    memcpy(&to.sin_addr.s_addr, &in->sin_addr.S_un.S_addr, sizeof(to.sin_addr.s_addr));

    return true;
}

void FromNative(const sockaddr_in &from, SOCK_sockaddr *to, int *toLen)
{
    SOCK_sockaddr_in *out = reinterpret_cast<SOCK_sockaddr_in *>(to);

    memset(out, 0, sizeof(*out));
    out->sin_family = SOCK_AF_INET;
    out->sin_port = from.sin_port;
    memcpy(&out->sin_addr.S_un.S_addr, &from.sin_addr.s_addr, sizeof(from.sin_addr.s_addr));

    if (toLen != nullptr)
    {
        *toLen = sizeof(SOCK_sockaddr_in);
    }
}

int NativeRecvFlags(int flags)
{
    int native = 0;

    if (flags & 1)
    {
        native |= MSG_OOB;
    }
    if (flags & SOCKET_READ_PEEK_OPTION)
    {
        native |= MSG_PEEK;
    }

    return native;
}

int NativeSendFlags(int flags)
{
    int native = NativeRecvFlags(flags);

#if defined(MSG_NOSIGNAL)
    // a closed peer must surface as EPIPE, not kill the host with SIGPIPE
    native |= MSG_NOSIGNAL;
#endif

    return native;
}

// ─── Options ────────────────────────────────────────────────────────────────

struct NativeOption
{
    int level;
    int name;
};

// Translate the managed (level, name) pair. name == 0 in the result means the
// option has no host equivalent.
NativeOption MapOption(int level, int name)
{
    switch (level)
    {
        case SOCK_IPPROTO_IP:
            switch (name)
            {
                case SOCK_IPO_TTL:
                    return {IPPROTO_IP, IP_TTL};
                case SOCK_IPO_TOS:
                    return {IPPROTO_IP, IP_TOS};
                case SOCK_IPO_MULTICAST_IF:
                    return {IPPROTO_IP, IP_MULTICAST_IF};
                case SOCK_IPO_MULTICAST_TTL:
                    return {IPPROTO_IP, IP_MULTICAST_TTL};
                case SOCK_IPO_MULTICAST_LOOP:
                    return {IPPROTO_IP, IP_MULTICAST_LOOP};
                case SOCK_IPO_ADD_MEMBERSHIP:
                    return {IPPROTO_IP, IP_ADD_MEMBERSHIP};
                case SOCK_IPO_DROP_MEMBERSHIP:
                    return {IPPROTO_IP, IP_DROP_MEMBERSHIP};
                default:
                    return {IPPROTO_IP, 0};
            }

        case SOCK_IPPROTO_TCP:
            switch (name)
            {
                case SOCK_TCP_NODELAY:
                    return {IPPROTO_TCP, TCP_NODELAY};
                case SOCK_SOCKO_KEEPALIVE:
#if defined(TCP_KEEPALIVE)
                    return {IPPROTO_TCP, TCP_KEEPALIVE};
#else
                    return {IPPROTO_TCP, TCP_KEEPIDLE};
#endif
                default:
                    return {IPPROTO_TCP, 0};
            }

        case SOCK_IPPROTO_UDP:
        case SOCK_IPPROTO_ICMP:
        case SOCK_IPPROTO_IGMP:
        case SOCK_IPPROTO_IPV4:
        case SOCK_SOL_SOCKET:
            switch (name)
            {
                case SOCK_SOCKO_DONTLINGER:
                case SOCK_SOCKO_LINGER:
                    return {SOL_SOCKET, SO_LINGER};
                case SOCK_SOCKO_SENDTIMEOUT:
                    return {SOL_SOCKET, SO_SNDTIMEO};
                case SOCK_SOCKO_RECEIVETIMEOUT:
                    return {SOL_SOCKET, SO_RCVTIMEO};
                case SOCK_SOCKO_EXCLUSIVEADDRESSUSE:
                case SOCK_SOCKO_REUSEADDRESS:
                    return {SOL_SOCKET, SO_REUSEADDR};
                case SOCK_SOCKO_KEEPALIVE:
                    return {SOL_SOCKET, SO_KEEPALIVE};
                case SOCK_SOCKO_ERROR:
                    return {SOL_SOCKET, SO_ERROR};
                case SOCK_SOCKO_BROADCAST:
                    return {SOL_SOCKET, SO_BROADCAST};
                case SOCK_SOCKO_RECEIVEBUFFER:
                    return {SOL_SOCKET, SO_RCVBUF};
                case SOCK_SOCKO_SENDBUFFER:
                    return {SOL_SOCKET, SO_SNDBUF};
                case SOCK_SOCKO_ACCEPTCONNECTION:
                    return {SOL_SOCKET, SO_ACCEPTCONN};
                case SOCK_SOCKO_DONTROUTE:
                    return {SOL_SOCKET, SO_DONTROUTE};
                case SOCK_SOCKO_OUTOFBANDINLINE:
                    return {SOL_SOCKET, SO_OOBINLINE};
                case SOCK_SOCKO_DEBUG:
                    return {SOL_SOCKET, SO_DEBUG};
                case SOCK_SOCKO_SENDLOWWATER:
                    return {SOL_SOCKET, SO_SNDLOWAT};
                case SOCK_SOCKO_RECEIVELOWWATER:
                    return {SOL_SOCKET, SO_RCVLOWAT};
                default:
                    return {SOL_SOCKET, 0};
            }

        default:
            return {0, 0};
    }
}

// ─── Name resolution ────────────────────────────────────────────────────────

// The interpreter must not sit in getaddrinfo, so a lookup runs on a worker and
// the managed call is retried until it is done: the first call answers
// EWOULDBLOCK and starts the worker, which sets the socket event on completion,
// and the retry collects the result. Lookups are keyed by name because that is
// all a retry carries.
struct Lookup
{
    bool done = false;
    int error = 0;
    std::vector<uint32_t> addresses;
    std::string canonicalName;
    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
};

std::mutex s_lookupMutex;
std::unordered_map<std::string, Lookup> s_lookups;

// A result nobody came back for (the managed side timed out) is not kept forever.
constexpr auto c_abandonedLookupAge = std::chrono::seconds(120);

void ResolveOnWorker(std::string name)
{
    Lookup result;
    addrinfo hints;
    addrinfo *found = nullptr;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_CANONNAME;

    int rc = getaddrinfo(name.c_str(), nullptr, &hints, &found);

    if (rc == 0)
    {
        for (addrinfo *ai = found; ai != nullptr; ai = ai->ai_next)
        {
            if (ai->ai_family == AF_INET)
            {
                result.addresses.push_back(reinterpret_cast<sockaddr_in *>(ai->ai_addr)->sin_addr.s_addr);
            }
        }

        if (found->ai_canonname != nullptr)
        {
            result.canonicalName = found->ai_canonname;
        }

        freeaddrinfo(found);

        if (result.addresses.empty())
        {
            result.error = SOCK_HOST_NOT_FOUND;
        }
    }
    else if (rc == EAI_AGAIN)
    {
        result.error = SOCK_TRY_AGAIN;
    }
    else if (rc == EAI_FAIL)
    {
        result.error = SOCK_NO_RECOVERY;
    }
    else
    {
        result.error = SOCK_HOST_NOT_FOUND;
    }

    {
        std::lock_guard<std::mutex> lock(s_lookupMutex);
        Lookup &entry = s_lookups[name];

        entry.done = true;
        entry.error = result.error;
        entry.addresses = std::move(result.addresses);
        entry.canonicalName = std::move(result.canonicalName);
    }

    Events_Set(SYSTEM_EVENT_FLAG_SOCKET);
}

// One allocation per record, the way the lwIP driver does it, so that
// SOCK_freeaddrinfo is a walk of free() calls.
SOCK_addrinfo *MakeRecord(uint32_t address, const char *canonicalName)
{
    const size_t nameSize = canonicalName != nullptr ? strlen(canonicalName) + 1 : 0;
    const size_t total = sizeof(SOCK_addrinfo) + sizeof(SOCK_sockaddr_in) + nameSize;
    uint8_t *block = static_cast<uint8_t *>(calloc(1, total));

    if (block == nullptr)
    {
        return nullptr;
    }

    SOCK_addrinfo *ai = reinterpret_cast<SOCK_addrinfo *>(block);
    uint8_t *saBytes = block + sizeof(SOCK_addrinfo);
    SOCK_sockaddr_in *sa = reinterpret_cast<SOCK_sockaddr_in *>(saBytes);

    sa->sin_family = SOCK_AF_INET;
    memcpy(&sa->sin_addr.S_un.S_addr, &address, sizeof(address));

    ai->ai_family = SOCK_AF_INET;
    ai->ai_addrlen = sizeof(SOCK_sockaddr_in);
    ai->ai_addr = reinterpret_cast<SOCK_sockaddr *>(saBytes);

    if (nameSize != 0)
    {
        ai->ai_canonname = reinterpret_cast<char *>(block + sizeof(SOCK_addrinfo) + sizeof(SOCK_sockaddr_in));
        memcpy(ai->ai_canonname, canonicalName, nameSize);
    }

    return ai;
}

int BuildResult(const std::vector<uint32_t> &addresses, const char *canonicalName, SOCK_addrinfo **res)
{
    SOCK_addrinfo *head = nullptr;
    SOCK_addrinfo **tail = &head;

    for (uint32_t address : addresses)
    {
        // the canonical name travels once, on the first record
        SOCK_addrinfo *ai = MakeRecord(address, tail == &head ? canonicalName : nullptr);

        if (ai == nullptr)
        {
            SOCK_freeaddrinfo(head);
            t_lastError = SOCK_ENOBUFS;
            return SOCK_SOCKET_ERROR;
        }

        *tail = ai;
        tail = &ai->ai_next;
    }

    *res = head;
    return 0;
}

} // namespace

// ─── Lifecycle ──────────────────────────────────────────────────────────────

bool Network_Initialize()
{
    return true;
}

bool Network_Uninitialize()
{
    SOCKETS_CloseConnections();
    s_monitor.Stop();
    return true;
}

void SOCKETS_CloseConnections()
{
    std::unordered_set<int> open;

    {
        std::lock_guard<std::mutex> lock(s_socketsMutex);
        open.swap(s_sockets);
    }

    for (int fd : open)
    {
        s_monitor.Forget(fd);
        close(fd);
    }
}

// ─── BSD sockets ────────────────────────────────────────────────────────────

int SOCK_socket(int family, int type, int protocol)
{
    bool valid;

    if (type == SOCK_SOCK_STREAM)
    {
        valid = (protocol == SOCK_IPPROTO_IP || protocol == SOCK_IPPROTO_TCP);
    }
    else if (type == SOCK_SOCK_DGRAM)
    {
        valid = (protocol == SOCK_IPPROTO_IP || protocol == SOCK_IPPROTO_UDP);
    }
    else
    {
        // raw sockets need privileges the host process should not assume
        valid = false;
    }

    if (!valid)
    {
        return Fail(EPROTONOSUPPORT);
    }

    if (family != SOCK_AF_INET)
    {
        return Fail(EAFNOSUPPORT);
    }

    const int nativeType = (type == SOCK_SOCK_STREAM) ? SOCK_STREAM : SOCK_DGRAM;
    const int fd = socket(AF_INET, nativeType, 0);

    if (fd < 0)
    {
        return Fail();
    }

    fcntl(fd, F_SETFD, FD_CLOEXEC);
#if defined(SO_NOSIGPIPE)
    {
        int on = 1;
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
    }
#endif

    Track(fd);
    return fd;
}

int SOCK_bind(int socket, const SOCK_sockaddr *address, int addressLen)
{
    sockaddr_in sa;

    if (!ToNative(address, addressLen, sa))
    {
        return Fail(EAFNOSUPPORT);
    }

    if (bind(socket, reinterpret_cast<sockaddr *>(&sa), sizeof(sa)) != 0)
    {
        return Fail();
    }

    return 0;
}

int SOCK_connect(int socket, const SOCK_sockaddr *address, int addressLen)
{
    sockaddr_in sa;

    if (!ToNative(address, addressLen, sa))
    {
        return Fail(EAFNOSUPPORT);
    }

    if (connect(socket, reinterpret_cast<sockaddr *>(&sa), sizeof(sa)) != 0)
    {
        const int error = errno;

        if (error == EINPROGRESS)
        {
            s_monitor.Watch(socket, POLLOUT);
        }

        return Fail(error);
    }

    return 0;
}

int SOCK_send(int socket, const char *buf, int len, int flags)
{
    const ssize_t sent = send(socket, buf, len, NativeSendFlags(flags));

    if (sent < 0)
    {
        const int error = errno;

        if (error == EAGAIN)
        {
            s_monitor.Watch(socket, POLLOUT);
        }

        return Fail(error);
    }

    return (int)sent;
}

int SOCK_recv(int socket, char *buf, int len, int flags)
{
    const ssize_t received = recv(socket, buf, len, NativeRecvFlags(flags));

    if (received < 0)
    {
        const int error = errno;

        if (error == EAGAIN)
        {
            s_monitor.Watch(socket, POLLIN);
        }

        return Fail(error);
    }

    return (int)received;
}

int SOCK_close(int socket)
{
    s_monitor.Forget(socket);

    // Closing a descriptor this layer never handed out (or already closed)
    // could hit an unrelated descriptor of the host, which the debugger
    // transport also lives in.
    if (!Untrack(socket))
    {
        return Fail(EBADF);
    }

    if (close(socket) != 0)
    {
        return Fail();
    }

    return 0;
}

int SOCK_listen(int socket, int backlog)
{
    if (listen(socket, backlog) != 0)
    {
        return Fail();
    }

    return 0;
}

int SOCK_accept(int socket, SOCK_sockaddr *address, int *addressLen)
{
    sockaddr_in peer;
    socklen_t peerLen = sizeof(peer);
    const int fd = accept(socket, reinterpret_cast<sockaddr *>(&peer), &peerLen);

    if (fd < 0)
    {
        const int error = errno;

        if (error == EAGAIN)
        {
            s_monitor.Watch(socket, POLLIN);
        }

        return Fail(error);
    }

    fcntl(fd, F_SETFD, FD_CLOEXEC);
#if defined(SO_NOSIGPIPE)
    {
        int on = 1;
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
    }
#endif
    Track(fd);

    if (address != nullptr && peer.sin_family == AF_INET)
    {
        FromNative(peer, address, addressLen);
    }

    return fd;
}

int SOCK_shutdown(int socket, int how)
{
    int native;

    switch (how)
    {
        case 0:
            native = SHUT_RD;
            break;
        case 1:
            native = SHUT_WR;
            break;
        default:
            native = SHUT_RDWR;
            break;
    }

    if (shutdown(socket, native) != 0)
    {
        return Fail();
    }

    return 0;
}

int SOCK_getaddrinfo(const char *nodename, char *servname, const SOCK_addrinfo *hints, SOCK_addrinfo **res)
{
    (void)servname;
    (void)hints;

    if (nodename == nullptr || res == nullptr)
    {
        t_lastError = SOCK_EINVAL;
        return SOCK_SOCKET_ERROR;
    }

    *res = nullptr;

    // a literal address needs no resolver, and no worker either
    in_addr literal;

    if (inet_pton(AF_INET, nodename, &literal) == 1)
    {
        return BuildResult({literal.s_addr}, nodename, res);
    }

    const std::string name(nodename);
    std::unique_lock<std::mutex> lock(s_lookupMutex);
    const auto now = std::chrono::steady_clock::now();

    for (auto it = s_lookups.begin(); it != s_lookups.end();)
    {
        if (it->second.done && now - it->second.started > c_abandonedLookupAge)
        {
            it = s_lookups.erase(it);
        }
        else
        {
            ++it;
        }
    }

    auto found = s_lookups.find(name);

    if (found == s_lookups.end())
    {
        s_lookups[name];
        lock.unlock();

        std::thread(ResolveOnWorker, name).detach();

        t_lastError = SOCK_EWOULDBLOCK;
        return SOCK_SOCKET_ERROR;
    }

    if (!found->second.done)
    {
        t_lastError = SOCK_EWOULDBLOCK;
        return SOCK_SOCKET_ERROR;
    }

    Lookup result = std::move(found->second);
    s_lookups.erase(found);
    lock.unlock();

    if (result.error != 0)
    {
        t_lastError = result.error;
        return SOCK_SOCKET_ERROR;
    }

    return BuildResult(result.addresses, result.canonicalName.empty() ? nodename : result.canonicalName.c_str(), res);
}

void SOCK_freeaddrinfo(SOCK_addrinfo *ai)
{
    while (ai != nullptr)
    {
        SOCK_addrinfo *next = ai->ai_next;

        free(ai);
        ai = next;
    }
}

int SOCK_ioctl(int socket, int cmd, int *data)
{
    if (cmd == (int)SOCK_FIONBIO)
    {
        const int flags = fcntl(socket, F_GETFL, 0);

        if (flags < 0)
        {
            return Fail();
        }

        const int wanted = (*data != 0) ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);

        if (fcntl(socket, F_SETFL, wanted) != 0)
        {
            return Fail();
        }

        return 0;
    }

    if (cmd == (int)SOCK_FIONREAD)
    {
        int pending = 0;

        if (ioctl(socket, FIONREAD, &pending) != 0)
        {
            return Fail();
        }

        *data = pending;
        return 0;
    }

    return Fail(EINVAL);
}

int SOCK_getlasterror()
{
    return t_lastError;
}

// Every call in a sequence is made on the interpreter thread, straight after the
// one that failed, so the last error is the socket's last error.
int SOCK_getsocklasterror(int socket)
{
    (void)socket;
    return t_lastError;
}

int SOCK_select(
    int nfds,
    SOCK_fd_set *readfds,
    SOCK_fd_set *writefds,
    SOCK_fd_set *exceptfds,
    const SOCK_timeval *timeout)
{
    (void)nfds;

    std::vector<pollfd> fds;
    std::unordered_map<int, size_t> slot;

    auto add = [&](SOCK_fd_set *set, short events) {
        if (set == nullptr)
        {
            return;
        }

        for (unsigned int i = 0; i < set->fd_count; i++)
        {
            auto it = slot.find(set->fd_array[i]);

            if (it == slot.end())
            {
                slot[set->fd_array[i]] = fds.size();
                fds.push_back({set->fd_array[i], events, 0});
            }
            else
            {
                fds[it->second].events |= events;
            }
        }
    };

    add(readfds, POLLIN);
    add(writefds, POLLOUT);
    add(exceptfds, POLLPRI);

    int timeoutMs = -1;

    if (timeout != nullptr)
    {
        timeoutMs = (int)(timeout->tv_sec * 1000 + (timeout->tv_usec + 999) / 1000);
    }

    if (poll(fds.data(), fds.size(), timeoutMs) < 0)
    {
        return Fail();
    }

    // A refused connect or a reset shows up as POLLERR, once. Remember the
    // reason, and keep reporting it, for the caller that sees the exception set.
    {
        std::lock_guard<std::mutex> lock(s_socketsMutex);

        for (pollfd &p : fds)
        {
            if (p.revents & POLLERR)
            {
                int soError = 0;
                socklen_t soLen = sizeof(soError);

                if (getsockopt(p.fd, SOL_SOCKET, SO_ERROR, &soError, &soLen) == 0 && soError != 0)
                {
                    s_pendingErrors[p.fd] = MapError(soError);
                }
            }

            auto pending = s_pendingErrors.find(p.fd);

            if (pending != s_pendingErrors.end())
            {
                p.revents |= POLLERR;
                t_lastError = pending->second;
            }
        }
    }

    int ready = 0;

    auto collect = [&](SOCK_fd_set *set, short mask) {
        if (set == nullptr)
        {
            return;
        }

        unsigned int kept = 0;

        for (unsigned int i = 0; i < set->fd_count; i++)
        {
            if (fds[slot[set->fd_array[i]]].revents & mask)
            {
                set->fd_array[kept++] = set->fd_array[i];
            }
        }

        set->fd_count = kept;
        ready += kept;
    };

    // a peer that hung up is readable (recv reports it); an error is exceptional
    collect(readfds, POLLIN | POLLHUP | POLLERR);
    collect(writefds, POLLOUT | POLLERR);
    collect(exceptfds, POLLPRI | POLLERR | POLLNVAL);

    if (ready == 0)
    {
        // Nothing yet. If the caller is only sampling, it is about to park on
        // the socket event, so tell the monitor what would wake it.
        if (timeoutMs == 0)
        {
            for (const pollfd &p : fds)
            {
                s_monitor.Watch(p.fd, p.events);
            }
        }
    }

    return ready;
}

int SOCK_setsockopt(int socket, int level, int optname, const char *optval, int optlen)
{
    const NativeOption option = MapOption(level, optname);

    if (option.name == 0)
    {
        return Fail(ENOPROTOOPT);
    }

    if (optval == nullptr || optlen < (int)sizeof(int))
    {
        return Fail(EINVAL);
    }

    const int value = *reinterpret_cast<const int *>(optval);
    const void *native = optval;
    socklen_t nativeLen = optlen;
    linger lingerValue;
    timeval timeoutValue;
    int flipped;

    switch (optname)
    {
        case SOCK_SOCKO_LINGER:
            if (value < 0)
            {
                return Fail(EINVAL);
            }

            lingerValue.l_onoff = 1;
            lingerValue.l_linger = value;
            native = &lingerValue;
            nativeLen = sizeof(lingerValue);
            break;

        case SOCK_SOCKO_DONTLINGER:
            lingerValue.l_onoff = 0;
            lingerValue.l_linger = 0;
            native = &lingerValue;
            nativeLen = sizeof(lingerValue);
            break;

        case SOCK_SOCKO_EXCLUSIVEADDRESSUSE:
            flipped = !value;
            native = &flipped;
            nativeLen = sizeof(flipped);
            break;

        case SOCK_SOCKO_SENDTIMEOUT:
        case SOCK_SOCKO_RECEIVETIMEOUT:
            // the managed value is milliseconds
            timeoutValue.tv_sec = value / 1000;
            timeoutValue.tv_usec = (value % 1000) * 1000;
            native = &timeoutValue;
            nativeLen = sizeof(timeoutValue);
            break;

        default:
            break;
    }

    if (setsockopt(socket, option.level, option.name, native, nativeLen) != 0)
    {
        return Fail();
    }

    return 0;
}

int SOCK_getsockopt(int socket, int level, int optname, char *optval, int *optlen)
{
    const NativeOption option = MapOption(level, optname);

    if (option.name == 0)
    {
        return Fail(ENOPROTOOPT);
    }

    if (optval == nullptr || optlen == nullptr || *optlen < (int)sizeof(int))
    {
        return Fail(EINVAL);
    }

    int result = 0;
    socklen_t nativeLen;

    if (optname == SOCK_SOCKO_LINGER || optname == SOCK_SOCKO_DONTLINGER)
    {
        linger lingerValue;

        nativeLen = sizeof(lingerValue);

        if (getsockopt(socket, option.level, option.name, &lingerValue, &nativeLen) != 0)
        {
            return Fail();
        }

        result = (optname == SOCK_SOCKO_LINGER) ? (lingerValue.l_onoff ? lingerValue.l_linger : 0)
                                                : (lingerValue.l_onoff == 0);
    }
    else if (optname == SOCK_SOCKO_SENDTIMEOUT || optname == SOCK_SOCKO_RECEIVETIMEOUT)
    {
        timeval timeoutValue;

        nativeLen = sizeof(timeoutValue);

        if (getsockopt(socket, option.level, option.name, &timeoutValue, &nativeLen) != 0)
        {
            return Fail();
        }

        result = (int)(timeoutValue.tv_sec * 1000 + timeoutValue.tv_usec / 1000);
    }
    else
    {
        nativeLen = sizeof(result);

        if (getsockopt(socket, option.level, option.name, &result, &nativeLen) != 0)
        {
            return Fail();
        }

        switch (optname)
        {
            case SOCK_SOCKO_ERROR:
            {
                std::lock_guard<std::mutex> lock(s_socketsMutex);
                auto pending = s_pendingErrors.find(socket);

                if (pending != s_pendingErrors.end())
                {
                    result = pending->second;
                    s_pendingErrors.erase(pending);
                }
                else
                {
                    result = result != 0 ? MapError(result) : 0;
                }
            }
            break;

            case SOCK_SOCKO_EXCLUSIVEADDRESSUSE:
                result = (result == 0);
                break;

            case SOCK_SOCKO_ACCEPTCONNECTION:
            case SOCK_SOCKO_BROADCAST:
            case SOCK_SOCKO_KEEPALIVE:
                result = (result != 0);
                break;

            default:
                break;
        }
    }

    memcpy(optval, &result, sizeof(result));
    *optlen = sizeof(result);
    return 0;
}

int SOCK_getpeername(int socket, SOCK_sockaddr *name, int *namelen)
{
    sockaddr_in sa;
    socklen_t len = sizeof(sa);

    if (getpeername(socket, reinterpret_cast<sockaddr *>(&sa), &len) != 0)
    {
        return Fail();
    }

    FromNative(sa, name, namelen);
    return 0;
}

int SOCK_getsockname(int socket, SOCK_sockaddr *name, int *namelen)
{
    sockaddr_in sa;
    socklen_t len = sizeof(sa);

    if (getsockname(socket, reinterpret_cast<sockaddr *>(&sa), &len) != 0)
    {
        return Fail();
    }

    FromNative(sa, name, namelen);
    return 0;
}

int SOCK_recvfrom(int s, char *buf, int len, int flags, SOCK_sockaddr *from, int *fromlen)
{
    sockaddr_in sa;
    socklen_t saLen = sizeof(sa);
    const ssize_t received = recvfrom(s, buf, len, NativeRecvFlags(flags), reinterpret_cast<sockaddr *>(&sa), &saLen);

    if (received < 0)
    {
        const int error = errno;

        if (error == EAGAIN)
        {
            s_monitor.Watch(s, POLLIN);
        }

        return Fail(error);
    }

    if (from != nullptr && saLen >= sizeof(sockaddr_in) && sa.sin_family == AF_INET)
    {
        FromNative(sa, from, fromlen);
    }

    return (int)received;
}

int SOCK_sendto(int s, const char *buf, int len, int flags, const SOCK_sockaddr *to, int tolen)
{
    sockaddr_in sa;

    if (!ToNative(to, tolen, sa))
    {
        return Fail(EAFNOSUPPORT);
    }

    const ssize_t sent = sendto(s, buf, len, NativeSendFlags(flags), reinterpret_cast<sockaddr *>(&sa), sizeof(sa));

    if (sent < 0)
    {
        const int error = errno;

        if (error == EAGAIN)
        {
            s_monitor.Watch(s, POLLOUT);
        }

        return Fail(error);
    }

    return (int)sent;
}
