#pragma once

#include <memory>
#include <string_view>

#include "ioscpp/afc.hpp"
#include "ioscpp/byte_stream.hpp"
#include "ioscpp/error.hpp"
#include "ioscpp/export.hpp"

namespace ioscpp
{

class Device;
class Rsd;

/**
 * @brief An `AFC` session rooted at one installed app's container, over `house_arrest`.
 *
 * `AFC` alone reaches the device's media root. Reaching one app's own sandbox needs
 * `com.apple.mobile.house_arrest`: the service is started like any other, the bundle
 * id then travels in a `VendContainer` (or `VendDocuments`) command, and the device
 * answers with the container vended over `AFC` on the same connection.
 *
 * `start` performs that handshake and returns a client whose @ref afc is rooted at the
 * container, so `list`, `stat`, `pull`, and `push` act inside the app. A
 * `VendDocuments` vend exposes only the `Documents` subtree; the default
 * `VendContainer` exposes the whole container.
 *
 * The mux-link service is `com.apple.mobile.house_arrest`; on iOS 17.4 and later the
 * RSD tunnel carries the same service as
 * `com.apple.mobile.house_arrest.shim.remote`, so there is an overload for each, plus
 * one over a service stream the caller already started. A service the device does not
 * advertise, or an app that is not installed, is an `ErrorCode::Device` error.
 *
 * A `HouseArrest` is not thread-safe, like the `Afc` it holds. When it owns the
 * service connection the caller must destroy the `HouseArrest` before the device or the
 * RSD it was started from, so the reset is sent while the link is still up.
 */
class IOSCPP_API HouseArrest
{
public:
    /// Vends `bundle_id`'s container over the mux link.
    static Result<HouseArrest> start(Device &device, std::string_view bundle_id, bool documents_only = false);

    /// Vends `bundle_id`'s container over the RSD tunnel.
    static Result<HouseArrest> start(Rsd &rsd, std::string_view bundle_id, bool documents_only = false);

    /// Vends `bundle_id`'s container over an already-started service `stream`.
    static Result<HouseArrest> start(ByteStream &stream, std::string_view bundle_id, bool documents_only = false);

    ~HouseArrest();
    HouseArrest(HouseArrest &&) noexcept;
    HouseArrest &operator=(HouseArrest &&) noexcept;
    HouseArrest(const HouseArrest &) = delete;
    HouseArrest &operator=(const HouseArrest &) = delete;

    /// The `AFC` client rooted at the vended container.
    Afc &afc() noexcept;

    /// Closes the `AFC` session and the service stream. A second call is a no-op.
    void close() noexcept;

private:
    HouseArrest();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ioscpp
