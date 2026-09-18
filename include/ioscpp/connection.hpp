#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string_view>

#include "ioscpp/error.hpp"
#include "ioscpp/export.hpp"
#include "ioscpp/protocol/plist.hpp"
#include "ioscpp/protocol/usbmux.hpp"
#include "ioscpp/session.hpp"
#include "ioscpp/transport.hpp"

namespace ioscpp
{

class Stream;

/**
 * @brief An open link to a device.
 *
 * There are two ways in. Over the device's USB interface, `open` performs the
 * version negotiation: it sends a `MUX_PROTO_VERSION` request with the host's
 * version (2.0), reads the device's answer, and, when the device reports version
 * 2, sends the one-byte `MUX_PROTO_SETUP` packet that enables the v2 framing.
 * This is the host side of `usbmuxd`'s `device_add` and `device_version_input`:
 *
 *   https://github.com/libimobiledevice/usbmuxd/blob/master/src/device.c
 *
 * Over a socket to a running `usbmuxd`, there is no mux framing at all: `open`
 * sends a `ListDevices` plist and remembers the first device's id, and every
 * `Stream::open` opens a fresh socket, sends a `Connect` plist for the port, and
 * then speaks the port's raw bytes. Such a transport reports `is_usbmuxd()` and
 * provides `reopen()` for the fresh socket.
 *
 * A port is opened with `Stream::open`, which is the SYN / SYN|ACK / ACK exchange
 * from `usbmuxd`'s `device_start_connect` and `device_tcp_input` on a direct link.
 *
 * A `Connection` is not thread-safe. It shares the session's transport and the
 * stream routing state, so concurrent calls must be serialized by the caller.
 */
class IOSCPP_API Connection
{
public:
    /**
     * @brief Opens `transport` and performs the handshake above.
     *
     * The handshake can fail, and a constructor cannot report that, so this is a
     * named factory and the constructor is private.
     */
    static Result<Connection> open(Transport &transport);

    Connection(const Connection &) = delete;
    Connection &operator=(const Connection &) = delete;

    /// A connection is returned by value, so it moves.
    Connection(Connection &&) noexcept = default;
    Connection &operator=(Connection &&) noexcept = default;

    /// The negotiated mux version: 1 or 2 on a direct link, 0 over `usbmuxd`.
    std::uint32_t version() const noexcept
    {
        return session_.version();
    }

    /// The device's USB serial, or empty when the transport has none.
    std::string_view serial() const noexcept
    {
        return session_.serial();
    }

    /// Whether the link is a socket to a running `usbmuxd`.
    bool is_usbmuxd() const noexcept
    {
        return usbmuxd_;
    }

private:
    friend class Stream;

    explicit Connection(Transport &transport);

    /// Allocates a unique, non-zero local port.
    std::uint16_t allocate_local_port() noexcept
    {
        return next_port_++;
    }

    /// Sends a mux frame carrying `header` and an optional payload.
    Status send_tcp(const protocol::TcpHeader &header, std::span<const std::byte> payload = {});

    /// Reads the next frame.
    Result<Frame> receive();

    /// Sends a `usbmuxd` plist request and returns its plist answer.
    Result<protocol::Plist> muxd_request(protocol::Plist request);

    Session session_;
    std::uint16_t next_port_ = 1;
    bool negotiated_ = false;

    // The `usbmuxd` path: the device's id, and a factory for a fresh socket.
    bool usbmuxd_ = false;
    std::uint32_t device_id_ = 0;
    std::function<Result<std::unique_ptr<Transport>>()> open_transport_;
};

} // namespace ioscpp
