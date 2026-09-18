#pragma once

#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include "ioscpp/error.hpp"
#include "ioscpp/export.hpp"
#include "ioscpp/transport.hpp"

namespace ioscpp::tcp
{

/**
 * @brief A Transport over a TCP socket, backed by the platform's sockets.
 *
 * This is the transport to a running `usbmuxd`, which owns the device and
 * multiplexes every port over one host-side daemon. The daemon speaks a plist
 * handshake rather than the direct mux framing, so @ref is_usbmuxd is true and
 * @ref reopen opens the fresh socket a new device port needs.
 *
 * The socket uses `TCP_NODELAY`, because the plist length and its payload are
 * separate writes and Nagle would coalesce them. A read returns whatever one
 * `recv` delivers, which may be partial, and the layer above reads on.
 *
 * @note There is no third-party dependency, unlike `usb::UsbTransport`, so this
 * type is always available.
 */
class IOSCPP_API TcpTransport : public Transport
{
public:
    /// The default timeout for the connection attempt, in milliseconds.
    static constexpr unsigned int kDefaultConnectTimeoutMs = 10000;

    /// The default timeout for a single `recv` or `send`, in milliseconds.
    static constexpr unsigned int kDefaultTransferTimeoutMs = 5000;

    /// The default total a read waits before it gives up, in milliseconds.
    static constexpr unsigned int kDefaultTransferBudgetMs = 120000;

    /**
     * @brief Connects to `endpoint`, a `host:port` pair.
     *
     * `host` is a name or a literal address, and `port` is a service name or a
     * number; both are resolved with `getaddrinfo`, so `localhost:27015`,
     * `127.0.0.1:27015`, and `[::1]:27015` all work. An IPv6 literal is
     * written in brackets. The default endpoint is the local `usbmuxd`.
     *
     * Opening can fail, and a constructor cannot report that, so this is a named
     * factory and the constructor is private.
     */
    static Result<TcpTransport> open(std::string_view endpoint = "localhost:27015",
                                     unsigned int connect_timeout_ms = kDefaultConnectTimeoutMs,
                                     unsigned int transfer_timeout_ms = kDefaultTransferTimeoutMs,
                                     unsigned int transfer_budget_ms = kDefaultTransferBudgetMs);

    ~TcpTransport() override;

    TcpTransport(const TcpTransport &) = delete;
    TcpTransport &operator=(const TcpTransport &) = delete;

    /// A transport is returned by value, so it moves.
    TcpTransport(TcpTransport &&) noexcept;
    TcpTransport &operator=(TcpTransport &&) noexcept;

    /// Sets the timeout applied to each later `recv` and `send`, in milliseconds.
    void set_transfer_timeout(unsigned int milliseconds) noexcept;

    /// The timeout applied to each `recv` and `send`, in milliseconds.
    unsigned int transfer_timeout() const noexcept;

    /**
     * @brief Sets the total a read or write waits across retries, in milliseconds.
     *
     * A single `recv` or `send` may time out having moved nothing, which is
     * retried while the budget lasts, so a normal operation fails fast while a
     * pairing exchange waits for the trust prompt.
     */
    void set_transfer_budget(unsigned int milliseconds) noexcept;

    /// The total a read or write waits across retries, in milliseconds.
    unsigned int transfer_budget() const noexcept;

    Result<std::size_t> read(std::span<std::byte> buffer) override;
    Status write(std::span<const std::byte> data) override;
    void close() override;

    /// The `host:port` endpoint.
    std::string_view serial() const noexcept override;

    bool is_usbmuxd() const noexcept override
    {
        return true;
    }

    Result<std::unique_ptr<Transport>> reopen() const override;

private:
    // Opening is done by `open`, so the constructor is private.
    TcpTransport();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ioscpp::tcp
