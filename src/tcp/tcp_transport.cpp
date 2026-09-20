#include "ioscpp/tcp/tcp_transport.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#    include <winsock2.h>
#    include <ws2tcpip.h>
#else
#    include <fcntl.h>
#    include <netdb.h>
#    include <netinet/tcp.h>
#    include <sys/select.h>
#    include <sys/socket.h>
#    include <sys/time.h>
#    include <unistd.h>

#    include <cerrno>
#endif

namespace ioscpp::tcp
{
namespace
{

#if defined(_WIN32)
using socket_t = SOCKET;
constexpr socket_t kInvalidSocket = INVALID_SOCKET;
#else
using socket_t = int;
constexpr socket_t kInvalidSocket = -1;
#endif

void close_socket(socket_t socket) noexcept
{
#if defined(_WIN32)
    ::closesocket(socket);
#else
    ::close(socket);
#endif
}

std::string last_socket_error()
{
#if defined(_WIN32)
    return std::system_category().message(WSAGetLastError());
#else
    return std::strerror(errno);
#endif
}

bool timed_out()
{
#if defined(_WIN32)
    const int error = WSAGetLastError();
    return error == WSAETIMEDOUT || error == WSAEWOULDBLOCK;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINPROGRESS;
#endif
}

#if defined(_WIN32)
// Winsock must be initialized before any socket call. The function-local static
// initializes it once, thread-safely, and `WSACleanup` runs at process exit.
struct SocketRuntime
{
    SocketRuntime()
    {
        WSADATA data{};
        error = WSAStartup(MAKEWORD(2, 2), &data);
    }

    ~SocketRuntime()
    {
        if (error == 0)
        {
            WSACleanup();
        }
    }

    int error = 0;
};
#endif

Status ensure_sockets()
{
#if defined(_WIN32)
    static SocketRuntime runtime;
    if (runtime.error != 0)
    {
        return tl::unexpected(
            Error{ErrorCode::Transport, "WSAStartup: " + std::system_category().message(runtime.error)});
    }
#endif
    return {};
}

/// Starts a non-blocking connect, waits for writability, then reads `SO_ERROR`
/// and restores the blocking mode.
bool connect_with_timeout(socket_t socket, const sockaddr *address, int address_length, unsigned int timeout_ms)
{
#if defined(_WIN32)
    u_long mode = 1;
    if (ioctlsocket(socket, FIONBIO, &mode) != 0)
    {
        return false;
    }
#else
    const int flags = fcntl(socket, F_GETFL, 0);
    if (flags < 0 || fcntl(socket, F_SETFL, flags | O_NONBLOCK) < 0)
    {
        return false;
    }
#endif

    bool connected = ::connect(socket, address, static_cast<socklen_t>(address_length)) == 0;
    if (!connected && !timed_out())
    {
        return false;
    }
    if (!connected)
    {
        fd_set writable;
        FD_ZERO(&writable);
        FD_SET(socket, &writable);
        // The `timeval` member types differ by platform, so the fields are
        // assigned rather than brace-initialized to avoid a narrowing error.
        timeval timeout{};
        timeout.tv_sec = static_cast<decltype(timeout.tv_sec)>(timeout_ms / 1000);
        timeout.tv_usec = static_cast<decltype(timeout.tv_usec)>((timeout_ms % 1000) * 1000);
        if (::select(static_cast<int>(socket) + 1, nullptr, &writable, nullptr, &timeout) <= 0)
        {
            return false;
        }
        int error = 0;
        socklen_t size = sizeof(error);
        if (::getsockopt(socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&error), &size) != 0 || error != 0)
        {
            return false;
        }
    }

#if defined(_WIN32)
    mode = 0;
    return ioctlsocket(socket, FIONBIO, &mode) == 0;
#else
    return fcntl(socket, F_SETFL, flags) == 0;
#endif
}

void set_timeout(socket_t socket, int option, unsigned int timeout_ms)
{
#if defined(_WIN32)
    const DWORD timeout = timeout_ms;
    (void)::setsockopt(socket, SOL_SOCKET, option, reinterpret_cast<const char *>(&timeout), sizeof(timeout));
#else
    timeval timeout{};
    timeout.tv_sec = static_cast<decltype(timeout.tv_sec)>(timeout_ms / 1000);
    timeout.tv_usec = static_cast<decltype(timeout.tv_usec)>((timeout_ms % 1000) * 1000);
    (void)::setsockopt(socket, SOL_SOCKET, option, &timeout, sizeof(timeout));
#endif
}

} // namespace

struct TcpTransport::Impl
{
    socket_t socket = kInvalidSocket;
    std::string endpoint;
    unsigned int transfer_timeout_ms = TcpTransport::kDefaultTransferTimeoutMs;
    unsigned int connect_timeout_ms = TcpTransport::kDefaultConnectTimeoutMs;
    unsigned int transfer_budget_ms = TcpTransport::kDefaultTransferBudgetMs;

    ~Impl()
    {
        if (socket != kInvalidSocket)
        {
            close_socket(socket);
        }
    }
};

TcpTransport::TcpTransport()
    : impl_(std::make_unique<Impl>())
{
}

TcpTransport::TcpTransport(TcpTransport &&) noexcept = default;
TcpTransport &TcpTransport::operator=(TcpTransport &&) noexcept = default;

TcpTransport::~TcpTransport() = default;

Result<TcpTransport> TcpTransport::open(std::string_view endpoint, unsigned int connect_timeout_ms,
                                        unsigned int transfer_timeout_ms, unsigned int transfer_budget_ms)
{
    if (Status ready = ensure_sockets(); !ready)
    {
        return tl::unexpected(ready.error());
    }

    // `endpoint` is `host:port`. The host may be an IPv6 literal in brackets,
    // which itself contains colons, so the split is at the last one.
    const auto separator = endpoint.rfind(':');
    if (separator == std::string_view::npos)
    {
        return tl::unexpected(Error{ErrorCode::InvalidArgument, "the endpoint is not a host:port pair"});
    }
    std::string host(endpoint.substr(0, separator));
    const std::string port(endpoint.substr(separator + 1));
    if (host.size() >= 2 && host.front() == '[' && host.back() == ']')
    {
        host = host.substr(1, host.size() - 2);
    }
    if (host.empty() || port.empty())
    {
        return tl::unexpected(Error{ErrorCode::InvalidArgument, "the endpoint is not a host:port pair"});
    }

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo *addresses = nullptr;
    const int resolved = ::getaddrinfo(host.c_str(), port.c_str(), &hints, &addresses);
    if (resolved != 0)
    {
#if defined(_WIN32)
        const std::string reason = ::gai_strerrorA(resolved);
#else
        const std::string reason = ::gai_strerror(resolved);
#endif
        return tl::unexpected(Error{ErrorCode::Transport, "getaddrinfo: " + reason});
    }

    TcpTransport transport;
    transport.impl_->endpoint = std::string(endpoint);
    transport.impl_->transfer_timeout_ms = transfer_timeout_ms;
    transport.impl_->connect_timeout_ms = connect_timeout_ms;
    transport.impl_->transfer_budget_ms = transfer_budget_ms;

    for (addrinfo *address = addresses; address != nullptr; address = address->ai_next)
    {
        const socket_t socket = ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (socket == kInvalidSocket)
        {
            continue;
        }
        if (connect_with_timeout(socket, address->ai_addr, static_cast<int>(address->ai_addrlen), connect_timeout_ms))
        {
            transport.impl_->socket = socket;
            break;
        }
        close_socket(socket);
    }
    ::freeaddrinfo(addresses);

    if (transport.impl_->socket == kInvalidSocket)
    {
        return tl::unexpected(
            Error{ErrorCode::Transport, "could not connect to " + std::string(endpoint) + ": " + last_socket_error()});
    }

    // The plist length and its payload are separate writes, so Nagle would hold
    // the length back and add a round trip; it is disabled.
    const int enabled = 1;
    (void)::setsockopt(transport.impl_->socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&enabled),
                       sizeof(enabled));
    set_timeout(transport.impl_->socket, SO_RCVTIMEO, transfer_timeout_ms);
    set_timeout(transport.impl_->socket, SO_SNDTIMEO, transfer_timeout_ms);

    return transport;
}

void TcpTransport::set_transfer_timeout(unsigned int milliseconds) noexcept
{
    impl_->transfer_timeout_ms = milliseconds;
    if (impl_->socket != kInvalidSocket)
    {
        set_timeout(impl_->socket, SO_RCVTIMEO, milliseconds);
        set_timeout(impl_->socket, SO_SNDTIMEO, milliseconds);
    }
}

unsigned int TcpTransport::transfer_timeout() const noexcept
{
    return impl_->transfer_timeout_ms;
}

void TcpTransport::set_transfer_budget(unsigned int milliseconds) noexcept
{
    impl_->transfer_budget_ms = milliseconds;
}

unsigned int TcpTransport::transfer_budget() const noexcept
{
    return impl_->transfer_budget_ms;
}

Result<std::size_t> TcpTransport::read(std::span<std::byte> buffer)
{
    if (buffer.empty())
    {
        return std::size_t{0};
    }

    // A `recv` returns as soon as any bytes arrive, which may be fewer than
    // `buffer.size()`. The session reads on until it has a whole frame. A timeout
    // that moved nothing is retried while the budget lasts.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(impl_->transfer_budget_ms);
    while (true)
    {
        const int received =
            ::recv(impl_->socket, reinterpret_cast<char *>(buffer.data()), static_cast<int>(buffer.size()), 0);
        if (received > 0)
        {
            return static_cast<std::size_t>(received);
        }
        if (received == 0)
        {
            return std::size_t{0};
        }
        if (!timed_out() || std::chrono::steady_clock::now() >= deadline)
        {
            return tl::unexpected(Error{ErrorCode::Transport, "recv: " + last_socket_error()});
        }
    }
}

Status TcpTransport::write(std::span<const std::byte> data)
{
    std::size_t total = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(impl_->transfer_budget_ms);
    while (total < data.size())
    {
        const int sent = ::send(impl_->socket, reinterpret_cast<const char *>(data.data()) + total,
                                static_cast<int>(data.size() - total), 0);
        if (sent > 0)
        {
            total += static_cast<std::size_t>(sent);
            continue;
        }
        if (sent == 0)
        {
            return tl::unexpected(Error{ErrorCode::Transport, "the peer closed the connection"});
        }
        if (!timed_out() || std::chrono::steady_clock::now() >= deadline)
        {
            return tl::unexpected(Error{ErrorCode::Transport, "send: " + last_socket_error()});
        }
    }
    return {};
}

void TcpTransport::close()
{
    if (impl_->socket != kInvalidSocket)
    {
        close_socket(impl_->socket);
        impl_->socket = kInvalidSocket;
    }
}

std::string_view TcpTransport::serial() const noexcept
{
    return impl_->endpoint;
}

Result<std::unique_ptr<Transport>> TcpTransport::reopen() const
{
    auto transport = TcpTransport::open(impl_->endpoint, impl_->connect_timeout_ms, impl_->transfer_timeout_ms,
                                        impl_->transfer_budget_ms);
    if (!transport)
    {
        return tl::unexpected(transport.error());
    }
    return std::make_unique<TcpTransport>(std::move(*transport));
}

} // namespace ioscpp::tcp
