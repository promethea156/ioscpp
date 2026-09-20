#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

#include "ioscpp/device.hpp"
#include "ioscpp/error.hpp"
#include "ioscpp/export.hpp"
#include "ioscpp/rsd.hpp"

namespace ioscpp
{

/// The result of running a command over a device service.
struct IOSCPP_API CommandResult
{
    /// The device's message: for an install or uninstall, its failure reason.
    std::string output;
    /// The service's status code.
    std::int32_t status = 0;
    /// Whether the device reported the command worked.
    bool success = false;
};

/**
 * @brief The result of installing or uninstalling an app.
 *
 * `installation_proxy` reports its outcome as a status and an optional error
 * string, so `success` covers both and `failure_reason()` extracts the message.
 */
struct IOSCPP_API PackageResult : CommandResult
{
    /// The reason inside the `Error` field, or an empty string when there is none.
    std::string failure_reason() const;

    /// The device's `Error` message, kept for @ref failure_reason.
    std::string failure_reason_cache;
};

/**
 * @brief Installs `ipa` on the device.
 *
 * The IPA is uploaded into `/PublicStaging` over `AFC` and installed from there
 * with `installation_proxy`, which is what `ideviceinstaller` does. An install the
 * device refuses is a normal outcome, so it is `success == false` with the reason, not
 * an `Error`.
 *
 * @warning The install replaces an existing copy of the same bundle and loses its data.
 *
 * @note This is the pre-17.4 path: on iOS 17+ the mux-link `installation_proxy`
 * accepts the connection but does not answer (`docs/04-blockers.md`), so the RSD
 * overload below is the one to use there.
 */
Result<PackageResult> IOSCPP_API install(Device &device, const std::filesystem::path &ipa);

/// Uninstalls `bundle_id` from the device over the mux link.
Result<PackageResult> IOSCPP_API uninstall(Device &device, std::string_view bundle_id);

/**
 * @brief Installs `ipa` on the device over the RSD tunnel.
 *
 * On iOS 17.4 and later the installer is not on the mux link but on the RSD
 * `com.apple.mobile.installation_proxy.shim.remote` service, so the IPA is
 * uploaded into `/PublicStaging` over the RSD `AFC` shim and installed from
 * there with the installer shim (`docs/10-coredevice-tunnel.md`). An install the
 * device refuses is a normal outcome, so it is `success == false` with the reason,
 * not an `Error`.
 *
 * @warning The install replaces an existing copy of the same bundle and loses its data.
 */
Result<PackageResult> IOSCPP_API install(Rsd &rsd, const std::filesystem::path &ipa);

/// Uninstalls `bundle_id` from the device over the RSD tunnel.
Result<PackageResult> IOSCPP_API uninstall(Rsd &rsd, std::string_view bundle_id);

/**
 * @brief Launches `bundle_id` over the mux link and returns its process id.
 *
 * The process-control service is not a plist one: it is a `DTX` channel on
 * `com.apple.instruments.remoteserver` (`docs/04-blockers.md`). A launch the
 * device refuses is an `ErrorCode::Device` error.
 *
 * @note This is the pre-17 path. On iOS 17+ the service moved to the RSD
 * `dtservicehub`, so the overload below is the one to use there.
 */
Result<std::uint64_t> IOSCPP_API launch(Device &device, std::string_view bundle_id);

/// Kills the process `pid` over the mux link with `SIGKILL`.
Status IOSCPP_API close(Device &device, std::uint64_t pid);

/// Whether `bundle_id` is running, over the mux link.
Result<bool> IOSCPP_API is_running(Device &device, std::string_view bundle_id);

/**
 * @brief Launches `bundle_id` over the RSD tunnel and returns its process id.
 *
 * On iOS 17.4 and later the process-control service is not on the mux link but
 * on the RSD `com.apple.instruments.dtservicehub` service, so the launch rides
 * the tunnel and the DTX channel (`docs/10-coredevice-tunnel.md`). A launch the
 * device refuses is an `ErrorCode::Device` error.
 */
Result<std::uint64_t> IOSCPP_API launch(Rsd &rsd, std::string_view bundle_id);

/// Kills the process `pid` over the RSD tunnel with `SIGKILL`.
Status IOSCPP_API close(Rsd &rsd, std::uint64_t pid);

/// Whether `bundle_id` is running, over the RSD tunnel.
Result<bool> IOSCPP_API is_running(Rsd &rsd, std::string_view bundle_id);

} // namespace ioscpp
