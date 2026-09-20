#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include "ioscpp/byte_stream.hpp"
#include "ioscpp/dtx_connection.hpp"
#include "ioscpp/error.hpp"
#include "ioscpp/export.hpp"

namespace ioscpp
{

/// The `dvt` process-control channel.
inline constexpr std::string_view kProcessControlChannel = "com.apple.instruments.server.services.processcontrol";

/**
 * @brief The process-control service over a `DTX` channel.
 *
 * `launch`, `close`, and `is_running` do not speak a plist protocol: the real
 * service is the `DTX` one on the `com.apple.instruments.remoteserver` channel
 * (`docs/04-blockers.md`). `DTX` carries the selector in the payload and each
 * argument in the auxiliary dictionary, so a launch sends the selector
 * `launchSuspendedProcessWithDevicePath:bundleIdentifier:environment:arguments:options:`
 * and reads the process id back from the reply's `NSKeyedArchive`.
 *
 * The service borrows `stream`, which must outlive it. A `ProcessControl` is not
 * thread-safe, so concurrent calls must be serialized by the caller.
 */
class IOSCPP_API ProcessControl
{
public:
    /// Borrows `stream` and opens the process-control channel on it.
    static Result<ProcessControl> start(ByteStream &stream);

    /**
     * @brief Launches `bundle_id` and returns its process id.
     *
     * The environment always carries `NSUnbufferedIO=YES`, which keeps the app's
     * stdout and stderr unbuffered so the service can read it; `kill_existing`
     * replaces a running instance. A launch the device refuses is an
     * `ErrorCode::Device` error.
     */
    Result<std::uint64_t> launch(std::string_view bundle_id, std::span<const std::string> arguments = {},
                                 bool kill_existing = true, bool start_suspended = false);

    /// The process id of `bundle_id`, or `0` when it is not running.
    Result<std::uint64_t> process_identifier(std::string_view bundle_id);

    /// Whether `bundle_id` is running.
    Result<bool> is_running(std::string_view bundle_id);

    /**
     * @brief Kills `pid` with `SIGKILL`.
     *
     * The signal is sent with `sendSignal:toPid:` rather than the
     * fire-and-forget `killPid:`, because the reply proves the device acted on
     * it before the channel and the tunnel are torn down.
     */
    Status kill(std::uint64_t pid);

private:
    explicit ProcessControl(ByteStream &stream);

    // The connection is heap-held, so its address stays put when this moves and the
    // channel's back-pointer to it stays valid.
    std::unique_ptr<DtxConnection> connection_;
    DtxChannel channel_;
};

} // namespace ioscpp
