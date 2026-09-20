#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

#include "ioscpp/byte_stream.hpp"
#include "ioscpp/error.hpp"
#include "ioscpp/export.hpp"
#include "ioscpp/transport.hpp"

namespace ioscpp
{

/**
 * @brief A userspace TCP connection over the CoreDevice tunnel.
 *
 * The tunnel carries the device's IPv6 packets as one byte stream with no packet
 * boundaries, so this re-frames the stream by the IPv6 header and runs a small TCP
 * client over it. Only outbound connections to the RSD ports are needed, so there
 * is no ARP, DHCP, routing, or ICMP.
 *
 * `connect` performs the SYN / SYN|ACK / ACK exchange to `address:port` and leaves
 * the connection established; `read` and `write` then move the connection's bytes.
 * The mux below the tunnel is reliable, so a packet lost on the wire is not
 * retransmitted here.
 *
 * The link borrows `tunnel`, which must outlive it, and the tunnel's IPv6 packets
 * carry `client_address` as their source, because the device answers the source it
 * sees. A `TcpLink` is not thread-safe, so concurrent calls must be serialized by
 * the caller.
 */
class IOSCPP_API TcpLink : public Transport, public ByteStream
{
public:
    /**
     * @brief Opens a link over `tunnel`, which the caller keeps alive.
     *
     * `client_address` is the tunnel-local IPv6 address the packets carry as their
     * source, which is @ref Tunnel::client_address, and `mtu` sizes a segment,
     * which is @ref Tunnel::mtu.
     */
    static Result<TcpLink> open(Transport &tunnel, std::string_view client_address, std::uint16_t mtu);

    ~TcpLink();
    TcpLink(TcpLink &&) noexcept;
    TcpLink &operator=(TcpLink &&) noexcept;
    TcpLink(const TcpLink &) = delete;
    TcpLink &operator=(const TcpLink &) = delete;

    /// Opens a TCP connection to `[address]:port` over the tunnel.
    Status connect(std::string_view address, std::uint16_t port);

    /// Reads up to `buffer.size()` bytes from the connection, or 0 at its end.
    Result<std::size_t> read(std::span<std::byte> buffer) override;

    /// Reads exactly `buffer.size()` bytes, looping over @ref read.
    Status read_exact(std::span<std::byte> buffer) override;

    /// Writes the whole of `data` to the connection.
    Status write(std::span<const std::byte> data) override;

    /// Closes the connection, sending a FIN.
    void close() override;

private:
    explicit TcpLink(Transport &tunnel, std::string_view client_address, std::uint16_t mtu);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ioscpp
