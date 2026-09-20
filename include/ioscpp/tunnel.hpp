#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "ioscpp/crypto/pairing.hpp"
#include "ioscpp/error.hpp"
#include "ioscpp/export.hpp"
#include "ioscpp/stream.hpp"
#include "ioscpp/transport.hpp"

namespace ioscpp
{

/**
 * @brief An open CoreDevice tunnel to a device.
 *
 * On iOS 17.4 and later the developer services are behind a `CoreDeviceProxy`
 * handshake that hands out an RSD IPv6 address and port. After the handshake the
 * stream is a raw IPv6 packet stream with no packet boundaries, so a `TcpLink`
 * re-frames it and runs a small TCP client over it before the RSD port is reachable
 * (`docs/10-coredevice-tunnel.md`). The RSD connection itself follows.
 *
 * `CoreDeviceProxy` sets `EnableServiceSSL`, so the stream is wrapped in TLS
 * before the handshake; @ref read and @ref write go through it when it is present.
 *
 * The tunnel owns its stream, so it must be destroyed before
 * @ref Device::disconnect, which tears the connection down.
 *
 * A `Tunnel` is a @ref Transport, so a re-framer reads the tunnel's IPv6
 * packets from it directly. `close` closes the TLS session and the stream, and a
 * second call is a no-op.
 *
 * A `Tunnel` is not thread-safe. It shares its stream and TLS session, so
 * concurrent calls must be serialized by the caller.
 */
class IOSCPP_API Tunnel : public Transport
{
public:
    /**
     * @brief Runs the `CDTunnel` handshake over `stream` and returns the tunnel.
     *
     * Wraps `stream` in TLS when `enable_ssl` is set, sends the handshake
     * request, and reads the device's answer into the RSD address, port, and MTU.
     * The handshake can fail, and a constructor cannot report that, so this is a
     * named factory and the constructor is private.
     */
    static Result<Tunnel> open(Stream stream, crypto::Pairing &pairing, bool enable_ssl);

    ~Tunnel() override;
    Tunnel(Tunnel &&) noexcept;
    Tunnel &operator=(Tunnel &&) noexcept;
    Tunnel(const Tunnel &) = delete;
    Tunnel &operator=(const Tunnel &) = delete;

    /// The tunnel-local IPv6 address this host uses as a source.
    std::string_view client_address() const noexcept;

    /// The RSD IPv6 address on the device.
    std::string_view address() const noexcept;

    /// The RSD port on the device.
    std::uint16_t port() const noexcept;

    /// The MTU the tunnel's re-framer uses.
    std::uint16_t mtu() const noexcept;

    /**
     * @brief Reads up to `buffer.size()` bytes from the tunnel.
     *
     * The bytes are decrypted when the service asked for SSL. A TLS record may
     * be shorter than `buffer`, so this returns what one record holds, like a
     * transport's read, and the re-framer loops for the rest.
     */
    Result<std::size_t> read(std::span<std::byte> buffer) override;

    /// Writes `data` to the tunnel, encrypting it when the service asked for SSL.
    Status write(std::span<const std::byte> data) override;

    /// Closes the TLS session and the stream. A second call is a no-op.
    void close() override;

private:
    explicit Tunnel(Stream stream);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ioscpp
