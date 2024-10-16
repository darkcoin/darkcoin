// Copyright (c) 2020-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <compat.h>
#include <logging.h>
#include <threadinterrupt.h>
#include <tinyformat.h>
#include <util/sock.h>
#include <util/system.h>
#include <util/time.h>

#include <memory>
#include <stdexcept>
#include <string>

#ifdef WIN32
#include <codecvt>
#include <locale>
#endif

#ifdef USE_EPOLL
#include <sys/epoll.h>
#endif

#ifdef USE_KQUEUE
#include <sys/event.h>
#endif

#ifdef USE_POLL
#include <poll.h>
#endif

static inline bool IOErrorIsPermanent(int err)
{
    return err != WSAEAGAIN && err != WSAEINTR && err != WSAEWOULDBLOCK && err != WSAEINPROGRESS;
}

Sock::Sock() : m_socket(INVALID_SOCKET) {}

Sock::Sock(SOCKET s, SocketEventsMode event_mode, std::optional<int> fd_mode)
    : m_socket(s),
      m_event_mode(event_mode),
      m_fd_mode(fd_mode)
{
    // Do not allow for the *possibility* of intentionally setting up a valid socket
    // with an unknown event mode.
    if (s != INVALID_SOCKET) {
        assert(m_event_mode != SocketEventsMode::Unknown);
    }

    // If we are using {epoll, kqueue} socket event modes, there must exist a file
    // descriptor for manipulating the socket with the {epoll, kqueue} instance
    if (m_event_mode == SocketEventsMode::EPoll || m_event_mode == SocketEventsMode::KQueue) {
        assert(m_fd_mode != std::nullopt && m_fd_mode != INVALID_SOCKET);
    }
}

Sock::Sock(Sock&& other)
{
    m_socket = other.m_socket;
    m_event_mode = other.m_event_mode;
    m_fd_mode = other.m_fd_mode;
    other.m_socket = INVALID_SOCKET;
    other.m_event_mode = SocketEventsMode::Unknown;
    other.m_fd_mode = std::nullopt;
}

Sock::~Sock() { Reset(); }

Sock& Sock::operator=(Sock&& other)
{
    Reset();
    m_socket = other.m_socket;
    m_event_mode = other.m_event_mode;
    m_fd_mode = other.m_fd_mode;
    other.m_socket = INVALID_SOCKET;
    other.m_event_mode = SocketEventsMode::Unknown;
    other.m_fd_mode = std::nullopt;
    return *this;
}

SOCKET Sock::Get() const { return m_socket; }

SOCKET Sock::Release()
{
    const SOCKET s = m_socket;
    m_socket = INVALID_SOCKET;
    m_event_mode = SocketEventsMode::Unknown;
    m_fd_mode = std::nullopt;
    return s;
}

void Sock::Reset() { CloseSocket(m_socket); }

ssize_t Sock::Send(const void* data, size_t len, int flags) const
{
    return send(m_socket, static_cast<const char*>(data), len, flags);
}

ssize_t Sock::Recv(void* buf, size_t len, int flags) const
{
    return recv(m_socket, static_cast<char*>(buf), len, flags);
}

int Sock::Connect(const sockaddr* addr, socklen_t addr_len) const
{
    return connect(m_socket, addr, addr_len);
}

int Sock::Bind(const sockaddr* addr, socklen_t addr_len) const
{
    return bind(m_socket, addr, addr_len);
}

int Sock::Listen(int backlog) const
{
    return listen(m_socket, backlog);
}

std::unique_ptr<Sock> Sock::Accept(sockaddr* addr, socklen_t* addr_len) const
{
#ifdef WIN32
    static constexpr auto ERR = INVALID_SOCKET;
#else
    static constexpr auto ERR = SOCKET_ERROR;
#endif

    std::unique_ptr<Sock> sock;

    const auto socket = accept(m_socket, addr, addr_len);
    if (socket != ERR) {
        try {
            sock = std::make_unique<Sock>(socket, m_event_mode, m_fd_mode);
        } catch (const std::exception&) {
#ifdef WIN32
            closesocket(socket);
#else
            close(socket);
#endif
        }
    }

    return sock;
}

int Sock::GetSockOpt(int level, int opt_name, void* opt_val, socklen_t* opt_len) const
{
    return getsockopt(m_socket, level, opt_name, static_cast<char*>(opt_val), opt_len);
}

int Sock::SetSockOpt(int level, int opt_name, const void* opt_val, socklen_t opt_len) const
{
    return setsockopt(m_socket, level, opt_name, static_cast<const char*>(opt_val), opt_len);
}

int Sock::GetSockName(sockaddr* name, socklen_t* name_len) const
{
    return getsockname(m_socket, name, name_len);
}

bool Sock::Wait(std::chrono::milliseconds timeout, Event requested, Event* occurred) const
{
    EventsPerSock events_per_sock{std::make_pair(m_socket, Events{requested})};

    /* We need to specify that we only want level-triggered modes used because we are expecting
       a direct correlation between the events reported and the one socket we are querying */
    if (!IWaitMany(m_event_mode, m_fd_mode, m_wrap_func, timeout, events_per_sock, /* lt_only = */ true)) {
        return false;
    }

    if (occurred != nullptr) {
        *occurred = events_per_sock.begin()->second.occurred;
    }

    return true;
}

bool Sock::WaitMany(std::chrono::milliseconds timeout, EventsPerSock& events_per_sock) const
{
    return IWaitMany(m_event_mode, m_fd_mode, m_wrap_func, timeout, events_per_sock);
}

bool Sock::IWaitMany(SocketEventsMode event_mode, std::optional<int> fd_mode, wrap_fn wrap_func,
                     std::chrono::milliseconds timeout, EventsPerSock& events_per_sock, bool lt_only)
{
    switch (event_mode)
    {
#ifdef USE_POLL
        case SocketEventsMode::Poll:
            return WaitManyPoll(wrap_func, timeout, events_per_sock);
#endif /* USE_POLL */
        case SocketEventsMode::Select:
            return WaitManySelect(wrap_func, timeout, events_per_sock);
#ifdef USE_EPOLL
        case SocketEventsMode::EPoll:
            /* If we explictly need to use a level triggered mode, skip below to fallback */
            if (lt_only) break;
            assert(fd_mode);
            return WaitManyEPoll(fd_mode.value(), wrap_func, timeout, events_per_sock);
#endif /* USE_EPOLL */
#ifdef USE_KQUEUE
        case SocketEventsMode::KQueue:
            /* If we explictly need to use a level triggered mode, skip below to fallback */
            if (lt_only) break;
            assert(fd_mode);
            return WaitManyKQueue(fd_mode.value(), wrap_func, timeout, events_per_sock);
#endif /* USE_KQUEUE */
        default:
            assert(false);
    }
    /* We should only be here if we need a level-triggered mode but the instance is edge-triggered instead */
    assert(lt_only);
#ifdef USE_POLL
    return WaitManyPoll(wrap_func, timeout, events_per_sock);
#else
    return WaitManySelect(wrap_func, timeout, events_per_sock);
#endif /* USE_POLL */
}

#ifdef USE_EPOLL
bool Sock::WaitManyEPoll(int fd_mode, wrap_fn wrap_func,
                         std::chrono::milliseconds timeout, EventsPerSock& events_per_sock)
{
    std::array<epoll_event, MAX_EVENTS> events;

    int ret{SOCKET_ERROR};
    wrap_func([&](){
        ret = epoll_wait(fd_mode, events.data(), events.size(), count_milliseconds(timeout));
    });
    if (ret == SOCKET_ERROR) {
        return false;
    }

    /* Events reported do not correspond to sockets requested in edge-triggered modes, we will clear the
       entire map before populating it with our events data. */
    events_per_sock.clear();

    for (int idx = 0; idx < ret; idx++) {
        auto& ev = events[idx];
        Event occurred = 0;
        if (ev.events & (EPOLLERR | EPOLLHUP)) {
            occurred |= ERR;
        } else {
            if (ev.events & EPOLLIN) {
                occurred |= RECV;
            }
            if (ev.events & EPOLLOUT) {
                occurred |= SEND;
            }
        }
        events_per_sock.emplace(static_cast<SOCKET>(ev.data.fd), Sock::Events{occurred, occurred});
    }

    return true;
}
#endif /* USE_EPOLL */

#ifdef USE_KQUEUE
bool Sock::WaitManyKQueue(int fd_mode, wrap_fn wrap_func,
                          std::chrono::milliseconds timeout, EventsPerSock& events_per_sock)
{
    std::array<struct kevent, MAX_EVENTS> events;
    struct timespec ts = MillisToTimespec(timeout);

    int ret{SOCKET_ERROR};
    wrap_func([&](){
        ret = kevent(fd_mode, nullptr, 0, events.data(), events.size(), &ts);
    });
    if (ret == SOCKET_ERROR) {
        return false;
    }

    /* Events reported do not correspond to sockets requested in edge-triggered modes, we will clear the
       entire map before populating it with our events data. */
    events_per_sock.clear();

    for (int idx = 0; idx < ret; idx++) {
        auto& ev = events[idx];
        Event occurred = 0;
        if (ev.flags & (EV_ERROR | EV_EOF)) {
            occurred |= ERR;
        } else {
            if (ev.filter == EVFILT_READ) {
                occurred |= RECV;
            }
            if (ev.filter == EVFILT_WRITE) {
                occurred |= SEND;
            }
        }
        events_per_sock.emplace(static_cast<SOCKET>(ev.ident), Sock::Events{occurred, occurred});
    }

    return true;
}
#endif /* USE_KQUEUE */

#ifdef USE_POLL
bool Sock::WaitManyPoll(wrap_fn wrap_func, std::chrono::milliseconds timeout, EventsPerSock& events_per_sock)
{
    std::vector<pollfd> pfds;
    for (const auto& [socket, events] : events_per_sock) {
        pfds.emplace_back();
        auto& pfd = pfds.back();
        pfd.fd = socket;
        if (events.requested & RECV) {
            pfd.events |= POLLIN;
        }
        if (events.requested & SEND) {
            pfd.events |= POLLOUT;
        }
    }

    int ret{SOCKET_ERROR};
    wrap_func([&](){
        ret = poll(pfds.data(), pfds.size(), count_milliseconds(timeout));
    });
    if (ret == SOCKET_ERROR) {
        return false;
    }

    assert(pfds.size() == events_per_sock.size());
    size_t i{0};
    for (auto& [socket, events] : events_per_sock) {
        assert(socket == static_cast<SOCKET>(pfds[i].fd));
        events.occurred = 0;
        if (pfds[i].revents & POLLIN) {
            events.occurred |= RECV;
        }
        if (pfds[i].revents & POLLOUT) {
            events.occurred |= SEND;
        }
        if (pfds[i].revents & (POLLERR | POLLHUP)) {
            events.occurred |= ERR;
        }
        ++i;
    }

    return true;
}
#endif /* USE_POLL */

bool Sock::WaitManySelect(wrap_fn wrap_func, std::chrono::milliseconds timeout, EventsPerSock& events_per_sock)
{
    fd_set recv;
    fd_set send;
    fd_set err;
    FD_ZERO(&recv);
    FD_ZERO(&send);
    FD_ZERO(&err);
    SOCKET socket_max{0};

    for (const auto& [sock, events] : events_per_sock) {
        const auto& s = sock;
        if (!IsSelectableSocket(s)) {
            return false;
        }
        if (events.requested & RECV) {
            FD_SET(s, &recv);
        }
        if (events.requested & SEND) {
            FD_SET(s, &send);
        }
        FD_SET(s, &err);
        socket_max = std::max(socket_max, s);
    }

    timeval tv = MillisToTimeval(timeout);

    int ret{SOCKET_ERROR};
    wrap_func([&](){
        ret = select(socket_max + 1, &recv, &send, &err, &tv);
    });
    if (ret == SOCKET_ERROR) {
        return false;
    }

    for (auto& [sock, events] : events_per_sock) {
        const auto& s = sock;
        events.occurred = 0;
        if (FD_ISSET(s, &recv)) {
            events.occurred |= RECV;
        }
        if (FD_ISSET(s, &send)) {
            events.occurred |= SEND;
        }
        if (FD_ISSET(s, &err)) {
            events.occurred |= ERR;
        }
    }

    return true;
}

void Sock::SetWrapFn(wrap_fn wrap_func)
{
    m_wrap_func = wrap_func;
}

void Sock::SendComplete(const std::string& data,
                        std::chrono::milliseconds timeout,
                        CThreadInterrupt& interrupt) const
{
    const auto deadline = GetTime<std::chrono::milliseconds>() + timeout;
    size_t sent{0};

    for (;;) {
        const ssize_t ret{Send(data.data() + sent, data.size() - sent, MSG_NOSIGNAL)};

        if (ret > 0) {
            sent += static_cast<size_t>(ret);
            if (sent == data.size()) {
                break;
            }
        } else {
            const int err{WSAGetLastError()};
            if (IOErrorIsPermanent(err)) {
                throw std::runtime_error(strprintf("send(): %s", NetworkErrorString(err)));
            }
        }

        const auto now = GetTime<std::chrono::milliseconds>();

        if (now >= deadline) {
            throw std::runtime_error(strprintf(
                "Send timeout (sent only %u of %u bytes before that)", sent, data.size()));
        }

        if (interrupt) {
            throw std::runtime_error(strprintf(
                "Send interrupted (sent only %u of %u bytes before that)", sent, data.size()));
        }

        // Wait for a short while (or the socket to become ready for sending) before retrying
        // if nothing was sent.
        const auto wait_time = std::min(deadline - now, std::chrono::milliseconds{MAX_WAIT_FOR_IO});
        (void)Wait(wait_time, SEND);
    }
}

std::string Sock::RecvUntilTerminator(uint8_t terminator,
                                      std::chrono::milliseconds timeout,
                                      CThreadInterrupt& interrupt,
                                      size_t max_data) const
{
    const auto deadline = GetTime<std::chrono::milliseconds>() + timeout;
    std::string data;
    bool terminator_found{false};

    // We must not consume any bytes past the terminator from the socket.
    // One option is to read one byte at a time and check if we have read a terminator.
    // However that is very slow. Instead, we peek at what is in the socket and only read
    // as many bytes as possible without crossing the terminator.
    // Reading 64 MiB of random data with 262526 terminator chars takes 37 seconds to read
    // one byte at a time VS 0.71 seconds with the "peek" solution below. Reading one byte
    // at a time is about 50 times slower.

    for (;;) {
        if (data.size() >= max_data) {
            throw std::runtime_error(
                strprintf("Received too many bytes without a terminator (%u)", data.size()));
        }

        char buf[512];

        const ssize_t peek_ret{Recv(buf, std::min(sizeof(buf), max_data - data.size()), MSG_PEEK)};

        switch (peek_ret) {
        case -1: {
            const int err{WSAGetLastError()};
            if (IOErrorIsPermanent(err)) {
                throw std::runtime_error(strprintf("recv(): %s", NetworkErrorString(err)));
            }
            break;
        }
        case 0:
            throw std::runtime_error("Connection unexpectedly closed by peer");
        default:
            auto end = buf + peek_ret;
            auto terminator_pos = std::find(buf, end, terminator);
            terminator_found = terminator_pos != end;

            const size_t try_len{terminator_found ? terminator_pos - buf + 1 :
                                                    static_cast<size_t>(peek_ret)};

            const ssize_t read_ret{Recv(buf, try_len, 0)};

            if (read_ret < 0 || static_cast<size_t>(read_ret) != try_len) {
                throw std::runtime_error(
                    strprintf("recv() returned %u bytes on attempt to read %u bytes but previous "
                              "peek claimed %u bytes are available",
                              read_ret, try_len, peek_ret));
            }

            // Don't include the terminator in the output.
            const size_t append_len{terminator_found ? try_len - 1 : try_len};

            data.append(buf, buf + append_len);

            if (terminator_found) {
                return data;
            }
        }

        const auto now = GetTime<std::chrono::milliseconds>();

        if (now >= deadline) {
            throw std::runtime_error(strprintf(
                "Receive timeout (received %u bytes without terminator before that)", data.size()));
        }

        if (interrupt) {
            throw std::runtime_error(strprintf(
                "Receive interrupted (received %u bytes without terminator before that)",
                data.size()));
        }

        // Wait for a short while (or the socket to become ready for reading) before retrying.
        const auto wait_time = std::min(deadline - now, std::chrono::milliseconds{MAX_WAIT_FOR_IO});
        (void)Wait(wait_time, RECV);
    }
}

bool Sock::IsConnected(std::string& errmsg) const
{
    if (m_socket == INVALID_SOCKET) {
        errmsg = "not connected";
        return false;
    }

    char c;
    switch (Recv(&c, sizeof(c), MSG_PEEK)) {
    case -1: {
        const int err = WSAGetLastError();
        if (IOErrorIsPermanent(err)) {
            errmsg = NetworkErrorString(err);
            return false;
        }
        return true;
    }
    case 0:
        errmsg = "closed";
        return false;
    default:
        return true;
    }
}

#ifdef WIN32
std::string NetworkErrorString(int err)
{
    wchar_t buf[256];
    buf[0] = 0;
    if(FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS | FORMAT_MESSAGE_MAX_WIDTH_MASK,
            nullptr, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
            buf, ARRAYSIZE(buf), nullptr))
    {
        return strprintf("%s (%d)", std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>,wchar_t>().to_bytes(buf), err);
    }
    else
    {
        return strprintf("Unknown error (%d)", err);
    }
}
#else
std::string NetworkErrorString(int err)
{
    char buf[256];
    buf[0] = 0;
    /* Too bad there are two incompatible implementations of the
     * thread-safe strerror. */
    const char *s;
#ifdef STRERROR_R_CHAR_P /* GNU variant can return a pointer outside the passed buffer */
    s = strerror_r(err, buf, sizeof(buf));
#else /* POSIX variant always returns message in buffer */
    s = buf;
    if (strerror_r(err, buf, sizeof(buf)))
        buf[0] = 0;
#endif
    return strprintf("%s (%d)", s, err);
}
#endif

bool CloseSocket(SOCKET& hSocket)
{
    if (hSocket == INVALID_SOCKET)
        return false;
#ifdef WIN32
    int ret = closesocket(hSocket);
#else
    int ret = close(hSocket);
#endif
    if (ret) {
        LogPrintf("Socket close failed: %d. Error: %s\n", hSocket, NetworkErrorString(WSAGetLastError()));
    }
    hSocket = INVALID_SOCKET;
    return ret != SOCKET_ERROR;
}
