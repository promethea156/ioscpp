#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "ioscpp/afc.hpp"
#include "ioscpp/crypto/pairing.hpp"
#include "ioscpp/error.hpp"
#include "ioscpp/export.hpp"
#include "ioscpp/lockdown.hpp"
#include "ioscpp/stream.hpp"
#include "ioscpp/transport.hpp"

namespace ioscpp
{

/**
 * @brief An open CoreDevice tunnel to a device.
 *
 * On iOS 17.4 and later the developer services are behind a `CoreDeviceProxy`
 * handshake that hands out an RSD IPv6 address and port. After the handshake the
 * `stream` is a raw IPv6 byte stream with no packet boundaries, so it is re-framed
 * and carried by a userspace TCP/IP link before the RSD port is reachable
 * (`docs/10-coredevice-tunnel.md`). The tunnel grows to own that link and the RSD
 * connection in later increments.
 *
 * The `stream` keeps the connection alive, so the tunnel must be destroyed before
 * @ref Device::disconnect, which tears the connection down.
 */
struct IOSCPP_API Tunnel
{
    /// The `CoreDeviceProxy` connection, now the tunnel's raw IPv6 byte stream.
    Stream stream;
    /// The RSD IPv6 address on the device.
    std::string address;
    /// The RSD port on the device.
    std::uint16_t port = 0;
    /// The MTU the tunnel's re-framer uses.
    std::uint16_t mtu = 0;
};

/**
 * @brief A connected, paired iOS device.
 *
 * `connect` composes the whole stack: it opens the transport, negotiates the mux
 * version, opens `lockdownd` on port 62078, pairs when the record is not complete,
 * starts the session (which switches `lockdownd` to TLS), and reads the device's
 * identity.
 *
 * Every later feature hangs off the `lockdownd` client: `start_service` returns a raw
 * stream to any service, and `open_afc` returns the file client.
 *
 * A `Device` is not thread-safe. It shares its connection and `lockdownd` client, so
 * concurrent calls must be serialized by the caller.
 */
class IOSCPP_API Device
{
public:
    /**
     * @brief Connects to the device behind `transport`.
     *
     * `pairing` is completed in place when it is not already paired. The device may
     * show the *Trust This Computer?* prompt, so this blocks until it is answered.
     */
    static Result<Device> connect(Transport &transport, crypto::Pairing &pairing);

    ~Device();
    Device(Device &&) noexcept;
    Device &operator=(Device &&) noexcept;
    Device(const Device &) = delete;
    Device &operator=(const Device &) = delete;

    /// The device's unique id.
    std::string_view udid() const noexcept;

    /// The device's `ProductType`, for example `iPhone14,2`.
    std::string_view product_type() const noexcept;

    /// The device's `ProductVersion`, for example `17.4`.
    std::string_view product_version() const noexcept;

    /// The `lockdownd` client.
    Lockdown &lockdown() noexcept;

    /// Starts the service `name` and returns a stream to its port.
    Result<Stream> start_service(std::string_view name);

    /// Starts `com.apple.afc` and returns a file client on it.
    Result<Afc> open_afc();

    /**
     * @brief Opens the CoreDevice tunnel and returns its parameters.
     *
     * Starts `com.apple.internal.devicecompute.CoreDeviceProxy`, sends the
     * `CDTunnel` handshake request, and reads the device's answer into the RSD
     * address, port, and MTU. This is the first increment of Slice 9
     * (`docs/10-coredevice-tunnel.md`); the tunnel's link and RSD connection
     * follow, so the returned address and port are not reachable yet.
     *
     * The service does not exist before iOS 17.4, so the call is an
     * `ErrorCode::Device` error there.
     */
    Result<Tunnel> tunnel();

    /**
     * @brief Disconnects from the device, innermost first.
     *
     * Closes the TLS `close_notify`, then the `lockdownd` stream, then the mux
     * and the transport. A service stream the caller opened, such as an `Afc`, must
     * be destroyed first so its reset is sent while the link is still up. The call
     * is idempotent, and the destructor routes through it, so there is one path.
     *
     * The transport is closed here but owned by the caller, so a reconnect means
     * destroying this `Device`, opening a fresh transport (re-discovering the device
     * by its serial), and calling @ref connect again. `Device` is tied to one
     * transport, so it cannot be pointed at another.
     */
    void disconnect() noexcept;

private:
    Device(std::shared_ptr<Connection> connection, Lockdown lockdown, crypto::Pairing &pairing);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ioscpp
