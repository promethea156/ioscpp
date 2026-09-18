#if defined(_WIN32)
#    define NOMINMAX
#    include <winsock2.h>
#    include <ws2tcpip.h>
using socket_t = SOCKET;
constexpr socket_t kInvalidSocket = INVALID_SOCKET;
#else
#    include <arpa/inet.h>
#    include <netinet/in.h>
#    include <sys/socket.h>
#    include <unistd.h>
using socket_t = int;
constexpr socket_t kInvalidSocket = -1;
#endif

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

#include "ioscpp/tcp/tcp_transport.hpp"

using namespace ioscpp;

namespace
{

#if defined(_WIN32)

/// Starts Winsock once per process, so a test can open its own sockets.
void ensure_winsock()
{
    static const bool started = []
    {
        WSADATA data;
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    (void)started;
}

void close_socket(socket_t socket)
{
    ::closesocket(socket);
}

#else

void ensure_winsock()
{
}

void close_socket(socket_t socket)
{
    ::close(socket);
}

#endif

/// A loopback listener on an ephemeral port, so a client can connect to it.
struct Listener
{
    socket_t socket = kInvalidSocket;
    std::uint16_t port = 0;

    Listener()
    {
        socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        REQUIRE(socket != kInvalidSocket);

        const int reuse = 1;
        (void)::setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&reuse), sizeof(reuse));

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        REQUIRE(::bind(socket, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) == 0);

        auto length = static_cast<socklen_t>(sizeof(address));
        REQUIRE(::getsockname(socket, reinterpret_cast<sockaddr *>(&address), &length) == 0);
        port = ::ntohs(address.sin_port);

        REQUIRE(::listen(socket, 1) == 0);
    }

    ~Listener()
    {
        if (socket != kInvalidSocket)
        {
            close_socket(socket);
        }
    }

    Listener(const Listener &) = delete;
    Listener &operator=(const Listener &) = delete;

    std::string endpoint() const
    {
        return "127.0.0.1:" + std::to_string(port);
    }
};

/// Accepts the pending connection so the client's `open` returns.
socket_t accept_one(const Listener &listener)
{
    const socket_t server = ::accept(listener.socket, nullptr, nullptr);
    REQUIRE(server != kInvalidSocket);
    return server;
}

} // namespace

TEST_CASE("open keeps the transfer timeout and budget", "[transport]")
{
    ensure_winsock();
    Listener listener;
    auto transport = tcp::TcpTransport::open(listener.endpoint(), 1000, 1234, 5678);

    REQUIRE(transport.has_value());
    CHECK(transport->transfer_timeout() == 1234);
    CHECK(transport->transfer_budget() == 5678);
}

TEST_CASE("the setters update both values", "[transport]")
{
    ensure_winsock();
    Listener listener;
    auto transport = tcp::TcpTransport::open(listener.endpoint(), 1000, 1234, 5678);
    REQUIRE(transport.has_value());

    transport->set_transfer_timeout(111);
    transport->set_transfer_budget(222);
    CHECK(transport->transfer_timeout() == 111);
    CHECK(transport->transfer_budget() == 222);
}

TEST_CASE("a read retries a timeout until the budget runs out", "[transport]")
{
    ensure_winsock();
    Listener listener;
    // Each `recv` gives up after 50 ms, but the read waits for the 400 ms budget,
    // so an idle peer is retried instead of failing on the first timeout.
    auto transport = tcp::TcpTransport::open(listener.endpoint(), 1000, 50, 400);
    REQUIRE(transport.has_value());
    const socket_t server = accept_one(listener);

    std::array<std::byte, 16> buffer{};
    const auto start = std::chrono::steady_clock::now();
    const auto read = transport->read(buffer);
    const auto elapsed = std::chrono::steady_clock::now() - start;

    CHECK_FALSE(read.has_value());
    CHECK(elapsed >= std::chrono::milliseconds(350));
    CHECK(elapsed < std::chrono::milliseconds(2000));

    close_socket(server);
}

TEST_CASE("a read and a write round-trip over the socket", "[transport]")
{
    ensure_winsock();
    Listener listener;
    auto transport = tcp::TcpTransport::open(listener.endpoint(), 1000, 5000, 5000);
    REQUIRE(transport.has_value());
    const socket_t server = accept_one(listener);

    const std::string sent = "hello";
    REQUIRE(transport->write(std::as_bytes(std::span(sent))).has_value());

    std::array<char, 5> received{};
    std::size_t got = 0;
    while (got < received.size())
    {
        const int n = ::recv(server, received.data() + got, static_cast<int>(received.size() - got), 0);
        REQUIRE(n > 0);
        got += static_cast<std::size_t>(n);
    }
    CHECK(std::string(received.data(), received.size()) == sent);

    const std::string reply = "world";
    REQUIRE(::send(server, reply.data(), static_cast<int>(reply.size()), 0) == static_cast<int>(reply.size()));

    std::array<std::byte, 5> buffer{};
    std::size_t read_total = 0;
    while (read_total < buffer.size())
    {
        const auto read = transport->read(std::span(buffer).subspan(read_total));
        REQUIRE(read.has_value());
        REQUIRE(*read > 0);
        read_total += *read;
    }
    CHECK(std::string(reinterpret_cast<const char *>(buffer.data()), buffer.size()) == reply);

    close_socket(server);
}
