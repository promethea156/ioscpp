#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>

#include "ioscpp/afc.hpp"
#include "ioscpp/crypto/pairing.hpp"
#include "ioscpp/error.hpp"
#include "ioscpp/export.hpp"
#include "ioscpp/lockdown.hpp"
#include "ioscpp/stream.hpp"
#include "ioscpp/transport.hpp"
#include "ioscpp/tunnel.hpp"

namespace ioscpp
{

/**
 * @brief A connected, paired iOS device.
 *
 * `connect` composes the whole stack: it opens the transport, negotiates the mux
 * version, opens `lockdownd` on port 62078, pairs when the record is not complete,
 * starts the session, which upgrades `lockdownd` to TLS when the device asks for it,
 * and reads the device's identity.
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
     * address, port, and MTU. The returned `Tunnel` is the raw IPv6 packet
     * stream a `TcpLink` re-frames to reach the RSD port
     * (`docs/10-coredevice-tunnel.md`); the RSD connection itself follows.
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

/**
 * @brief Connects, retrying the whole open and connect with a bounded
 * exponential backoff.
 *
 * A device can reset the mux link, or be unplugged and replugged, so it
 * re-enumerates with a new USB address and the old transport is stale. `open`
 * re-discovers the device and opens a fresh transport, and `connect` performs the
 * handshake on it; both are retried together, because a device that re-enumerates
 * invalidates the transport handle. `transport` is where the opened transport lives
 * and is emplaced before each connect, so the returned value borrows it and the
 * transport must outlive it; on a failed connect it is reset again.
 *
 * `open` is a callable returning `Result<TransportT>`, for example
 * `[] { return usb::UsbTransport::open(id); }`. `connect` is a callable taking
 * `TransportT &` and returning the value to keep, for example
 * `[](usb::UsbTransport &t) { return Device::connect(t, pairing); }`.
 *
 * The delay before the second attempt is `backoff` and doubles for each later
 * attempt, so the wait is bounded by `attempts` and the total stays finite. This is
 * also how a dropped link is recovered: close the device, then call this again,
 * which replaces the transport in `transport` and repeats the handshake.
 *
 * @return the connected result, or the last error once every attempt failed.
 */
template <typename TransportT, typename Open, typename Connect>
auto connect_with_retry(Open open, Connect connect, std::optional<TransportT> &transport, int attempts = 5,
                        std::chrono::milliseconds backoff = std::chrono::milliseconds{250})
    -> std::invoke_result_t<Connect, TransportT &>
{
    std::invoke_result_t<Connect, TransportT &> result =
        tl::unexpected(Error{ErrorCode::Transport, "the transport was not opened"});
    for (int attempt = 0; attempt < attempts; ++attempt)
    {
        if (attempt > 0)
        {
            // A bounded doubling, so the wait grows but cannot overflow.
            const int shift = std::min(attempt - 1, 20);
            std::this_thread::sleep_for(backoff * (1 << shift));
        }

        auto opened = open();
        if (!opened)
        {
            result = tl::unexpected(opened.error());
            continue;
        }
        // The result keeps a reference to the transport, so the transport is
        // emplaced before `connect` and never moved once it holds a device.
        transport.emplace(std::move(*opened));

        auto connected = connect(*transport);
        if (!connected)
        {
            transport.reset();
            result = tl::unexpected(connected.error());
            continue;
        }
        return connected;
    }
    return result;
}

} // namespace ioscpp
