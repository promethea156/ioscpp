#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

#include "ioscpp/device.hpp"
#include "ioscpp/error.hpp"
#include "ioscpp/export.hpp"

namespace ioscpp
{

/// The result of running a command over a device service.
struct IOSCPP_API CommandResult
{
    /// Combined standard output and standard error.
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
 * @note Not yet validated on a device: on iOS 17+ the mux-link `installation_proxy` accepts
 * the connection but does not answer, so this is blocked on the `RSD` tunnel.
 */
Result<PackageResult> IOSCPP_API install(Device &device, const std::filesystem::path &ipa);

/// Uninstalls `bundle_id` from the device.
Result<PackageResult> IOSCPP_API uninstall(Device &device, std::string_view bundle_id);

/**
 * @brief Launches `bundle_id`.
 *
 * Uses the process-control service. An app that cannot be started is
 * `success == false` with the reason, not an `Error`.
 *
 * @note Not yet validated on a device: the real process-control service is `DTX`, not a
 * plist one, so this is blocked on the `RSD` tunnel.
 */
Result<CommandResult> IOSCPP_API launch(Device &device, std::string_view bundle_id);

/// Force-stops `bundle_id` and removes its process.
Status IOSCPP_API close(Device &device, std::string_view bundle_id);

/**
 * @brief Whether `bundle_id` is running.
 *
 * Asks the process-control service for the process list. An app that is not running
 * is a definite `false`, not an error.
 */
Result<bool> IOSCPP_API is_running(Device &device, std::string_view bundle_id);

} // namespace ioscpp
