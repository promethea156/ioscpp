#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "ioscpp/error.hpp"
#include "ioscpp/export.hpp"
#include "ioscpp/tcp_link.hpp"
#include "ioscpp/tunnel.hpp"

namespace ioscpp
{

/// The RSD port on the device, which `remoted` listens on.
inline constexpr std::uint16_t kRsdPort = 58783;

/// The host UUID the RSD handshake identifies this peer with, 16 bytes.
using RsdUuid = std::array<std::byte, 16>;

/**
 * @brief The RSD handshake UUID for `host_id`, the pairing record's host id.
 *
 * The device keeps one RSD connection per tunnel and re-attaches the tunnel when the
 * UUID changes, so every connection to one tunnel, across runs and processes, must
 * present the same UUID. `rsd_uuid` derives it by hashing the host id with FNV-1a,
 * which is stable across runs and processes.
 */
RsdUuid IOSCPP_API rsd_uuid(std::string_view host_id);

/**
 * @brief A service the RSD advertises, and how it is reached.
 *
 * The device handshake's `Services` dictionary lists every service by name and
 * gives each a `Port` and a `Properties` dictionary. A `.shim.remote` service is a
 * lockdown service shimmed over the RSD, so `start_service` runs its `RSDCheckin`;
 * a native service speaks its own protocol on the plain connection.
 */
struct IOSCPP_API RsdService
{
    /// The port the service listens on.
    std::uint16_t port = 0;
    /// Whether the service speaks RemoteXPC rather than a lockdown-style check-in.
    bool uses_remote_xpc = false;
};

/**
 * @brief A Remote Service Discovery connection to the device's RSD port.
 *
 * On iOS 17.4 and later a `CoreDevice` service is not on a `lockdownd` port: the
 * `CoreDeviceProxy` handshake hands out the RSD address and port, and the RSD
 * connection itself is reached over the tunnel (`docs/10-coredevice-tunnel.md`).
 *
 * `connect` runs the RemoteXPC connection setup (the init, term, and init-handshake
 * frames) and then the device handshake, whose answer carries the device
 * `Properties` and the `Services` dictionary. `services` exposes that dictionary,
 * and `start_service` opens a connection to a named service's port and runs its
 * `RSDCheckin`, returning the byte stream the service speaks on.
 *
 * The connection borrows `tunnel`, which must outlive it. An `Rsd` is not
 * thread-safe, so concurrent calls must be serialized by the caller.
 */
class IOSCPP_API Rsd
{
public:
    /**
     * @brief Connects to the RSD port over `tunnel`, which the caller keeps alive.
     *
     * `uuid` is the host UUID the device handshake identifies this peer with. It
     * must be the same for every connection to one tunnel, because the device
     * re-attaches the tunnel when the UUID changes.
     */
    static Result<Rsd> connect(Tunnel &tunnel, const RsdUuid &uuid);

    ~Rsd();
    Rsd(Rsd &&) noexcept;
    Rsd &operator=(Rsd &&) noexcept;
    Rsd(const Rsd &) = delete;
    Rsd &operator=(const Rsd &) = delete;

    /// The `Services` dictionary the device handshake answered with.
    const std::map<std::string, RsdService, std::less<>> &services() const noexcept;

    /**
     * @brief Starts the service `name` and returns a stream to its port.
     *
     * Opens a fresh connection to the service's port over the tunnel. A
     * `.shim.remote` service then runs its `RSDCheckin` before the connection is
     * returned for the service's own protocol; a native service skips it.
     * A service the device does not advertise is an `ErrorCode::Device` error.
     */
    Result<TcpLink> start_service(std::string_view name);

    /// Closes the RSD connection and its stream. A second call is a no-op.
    void close();

private:
    explicit Rsd(Tunnel &tunnel);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ioscpp
