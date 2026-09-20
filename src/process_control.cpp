#include "ioscpp/process_control.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ioscpp/protocol/dtx.hpp"
#include "ioscpp/protocol/keyed_archive.hpp"
#include "ioscpp/protocol/plist.hpp"

namespace ioscpp
{
namespace
{

/// The selector that launches a process.
constexpr std::string_view kLaunchSelector =
    "launchSuspendedProcessWithDevicePath:bundleIdentifier:environment:arguments:options:";
/// The selector that resolves a bundle id to its running process id.
constexpr std::string_view kProcessIdentifierSelector = "processIdentifierForBundleIdentifier:";
/// The selector that signals a process, which awaits its reply.
constexpr std::string_view kSignalSelector = "sendSignal:toPid:";
/// The path a launch names, which the device ignores.
constexpr std::string_view kDevicePath = "/private/";
/// `SIGKILL`, the only signal a close sends.
constexpr std::int64_t kSigkill = 9;

Error protocol_error(std::string message)
{
    return Error{ErrorCode::Protocol, std::move(message)};
}

Error device_error(std::string message)
{
    return Error{ErrorCode::Device, std::move(message)};
}

/// Reads the process id a reply carries in its `NSKeyedArchive` payload.
Result<std::uint64_t> reply_pid(const protocol::Dtx &reply, std::string_view what)
{
    if (reply.is_error())
    {
        return tl::unexpected(device_error(std::string(what) + " was refused"));
    }
    auto value = protocol::KeyedArchive::unarchive(reply.payload);
    if (!value)
    {
        return tl::unexpected(value.error());
    }
    const std::optional<std::int64_t> pid = value->integer();
    if (!pid.has_value())
    {
        return tl::unexpected(protocol_error("the process-control reply is not a process id"));
    }
    return static_cast<std::uint64_t>(*pid);
}

} // namespace

ProcessControl::ProcessControl(ByteStream &stream)
    : connection_(std::make_unique<DtxConnection>(stream))
{
}

Result<ProcessControl> ProcessControl::start(ByteStream &stream)
{
    ProcessControl control(stream);
    auto channel = control.connection_->request_channel(kProcessControlChannel);
    if (!channel)
    {
        return tl::unexpected(channel.error());
    }
    control.channel_ = std::move(*channel);
    return control;
}

Result<std::uint64_t> ProcessControl::launch(std::string_view bundle_id, std::span<const std::string> arguments,
                                             bool kill_existing, bool start_suspended)
{
    protocol::Plist::Array list;
    list.reserve(arguments.size());
    for (const std::string &argument : arguments)
    {
        list.push_back(protocol::Plist(argument));
    }
    protocol::Plist::Dictionary environment{{"NSUnbufferedIO", protocol::Plist("YES")}};
    protocol::Plist::Dictionary options{
        {"StartSuspendedKey", protocol::Plist(start_suspended ? std::int64_t{1} : std::int64_t{0})},
        {"KillExisting", protocol::Plist(kill_existing ? std::int64_t{1} : std::int64_t{0})},
    };

    const std::vector<protocol::Plist> call{
        protocol::Plist(std::string(kDevicePath)),           protocol::Plist(std::string(bundle_id)),
        protocol::Plist::dictionary(std::move(environment)), protocol::Plist::array(std::move(list)),
        protocol::Plist::dictionary(std::move(options)),
    };
    auto reply = channel_.method_call(kLaunchSelector, call);
    if (!reply)
    {
        return tl::unexpected(reply.error());
    }
    return reply_pid(*reply, "the launch");
}

Result<std::uint64_t> ProcessControl::process_identifier(std::string_view bundle_id)
{
    const std::vector<protocol::Plist> call{protocol::Plist(std::string(bundle_id))};
    auto reply = channel_.method_call(kProcessIdentifierSelector, call);
    if (!reply)
    {
        return tl::unexpected(reply.error());
    }
    return reply_pid(*reply, "the process lookup");
}

Result<bool> ProcessControl::is_running(std::string_view bundle_id)
{
    auto pid = process_identifier(bundle_id);
    if (!pid)
    {
        return tl::unexpected(pid.error());
    }
    return *pid != 0;
}

Status ProcessControl::kill(std::uint64_t pid)
{
    const std::vector<protocol::Plist> call{protocol::Plist(kSigkill), protocol::Plist(static_cast<std::int64_t>(pid))};
    auto reply = channel_.method_call(kSignalSelector, call);
    if (!reply)
    {
        return tl::unexpected(reply.error());
    }
    if (reply->is_error())
    {
        return tl::unexpected(device_error("the signal was refused"));
    }
    return {};
}

} // namespace ioscpp
