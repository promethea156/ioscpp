#include "ioscpp/app.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ioscpp/afc.hpp"
#include "ioscpp/device.hpp"
#include "ioscpp/error.hpp"
#include "ioscpp/lockdown.hpp"
#include "ioscpp/protocol/plist.hpp"
#include "ioscpp/stream.hpp"

namespace ioscpp
{
namespace
{

Error protocol_error(std::string message)
{
    return Error{ErrorCode::Protocol, std::move(message)};
}

/// The `installation_proxy` and process-control service names.
constexpr std::string_view kInstallationProxy = "com.apple.mobile.installation_proxy";
constexpr std::string_view kProcessControl = "com.apple.mobile.instruments";

/// The staging directory an IPA is uploaded into before it is installed.
constexpr std::string_view kStagingDirectory = "/PublicStaging";

/**
 * @brief A length-prefixed plist service.
 *
 * `installation_proxy` and process control frame every message the way
 * `lockdownd` does: a 4-byte big-endian length, then an XML plist.
 */
class PlistService
{
public:
    explicit PlistService(ByteStream &stream)
        : stream_(&stream)
    {
    }

    explicit PlistService(Stream stream)
        : owned_(std::move(stream))
        , stream_(&*owned_)
    {
    }

    Status send(const protocol::Plist &message)
    {
        const std::string body = message.to_xml();
        std::vector<std::byte> frame(4 + body.size());
        frame[0] = static_cast<std::byte>((frame.size() >> 24) & 0xff);
        frame[1] = static_cast<std::byte>((frame.size() >> 16) & 0xff);
        frame[2] = static_cast<std::byte>((frame.size() >> 8) & 0xff);
        frame[3] = static_cast<std::byte>(frame.size() & 0xff);
        std::copy(reinterpret_cast<const std::byte *>(body.data()),
                  reinterpret_cast<const std::byte *>(body.data() + body.size()), frame.begin() + 4);
        return stream_->write(frame);
    }

    Result<protocol::Plist> receive()
    {
        std::array<std::byte, 4> length_bytes{};
        if (Status status = stream_->read_exact(length_bytes); !status)
        {
            return tl::unexpected(status.error());
        }
        const std::uint32_t length =
            (static_cast<std::uint32_t>(length_bytes[0]) << 24) | (static_cast<std::uint32_t>(length_bytes[1]) << 16) |
            (static_cast<std::uint32_t>(length_bytes[2]) << 8) | static_cast<std::uint32_t>(length_bytes[3]);
        if (length == 0)
        {
            return tl::unexpected(protocol_error("the service sent an empty plist"));
        }
        std::vector<std::byte> body(length);
        if (Status status = stream_->read_exact(body); !status)
        {
            return tl::unexpected(status.error());
        }
        return protocol::Plist::parse(body);
    }

private:
    /// The mux stream, when the service owns it. The RSD path borrows instead.
    std::optional<Stream> owned_;
    ByteStream *stream_ = nullptr;
};

Result<PlistService> open_service(Device &device, std::string_view name)
{
    auto stream = device.start_service(name);
    if (!stream)
    {
        return tl::unexpected(stream.error());
    }
    return PlistService(std::move(*stream));
}

/// Runs an `installation_proxy` command and reads its status stream to the end.
Result<PackageResult> run_installer(Device &device, protocol::Plist::Dictionary command)
{
    auto service = open_service(device, kInstallationProxy);
    if (!service)
    {
        return tl::unexpected(service.error());
    }
    if (Status status = service->send(protocol::Plist::dictionary(std::move(command))); !status)
    {
        return tl::unexpected(status.error());
    }

    PackageResult result;
    for (;;)
    {
        auto status = service->receive();
        if (!status)
        {
            return tl::unexpected(status.error());
        }

        if (const protocol::Plist *error = status->find("Error"); error != nullptr)
        {
            result.output = error->string_or("installation failed");
            result.failure_reason_cache = result.output;
            return result;
        }

        const protocol::Plist *state = status->find("Status");
        if (state != nullptr && state->string_or() == "Complete")
        {
            result.success = true;
            return result;
        }
        if (const protocol::Plist *percent = status->find("PercentComplete"); percent != nullptr)
        {
            result.output = state != nullptr ? state->string_or() : std::string();
        }
    }
}

} // namespace

std::string PackageResult::failure_reason() const
{
    return failure_reason_cache;
}

Result<PackageResult> install(Device &device, const std::filesystem::path &ipa)
{
    if (!std::filesystem::is_regular_file(ipa))
    {
        return tl::unexpected(Error{ErrorCode::InvalidArgument, "the IPA is not a regular file"});
    }

    const std::string remote = std::string(kStagingDirectory) + "/" + ipa.filename().string();
    // The mux connection carries one service at a time: the AFC stream is closed
    // before the installation_proxy stream opens, because a stream left open
    // discards the other's frames (see `Stream::receive_more`).
    {
        auto afc = device.open_afc();
        if (!afc)
        {
            return tl::unexpected(afc.error());
        }
        (void)afc->make_directory(kStagingDirectory);

        if (Status status = afc->push(ipa, remote); !status)
        {
            return tl::unexpected(status.error());
        }
    }

    // An install names the staged package only: `ApplicationIdentifier` belongs to
    // uninstall, and sending an empty one with the install makes the device refuse
    // it. The package is a development build, so `PackageType` is `Developer`,
    // which is what `pymobiledevice3 apps install --developer` sets.
    protocol::Plist::Dictionary command{
        {"ClientOptions", protocol::Plist::dictionary({{"PackageType", protocol::Plist("Developer")}})},
        {"Command", protocol::Plist("Install")},
        {"PackagePath", protocol::Plist(remote)},
    };
    return run_installer(device, std::move(command));
}

Result<PackageResult> uninstall(Device &device, std::string_view bundle_id)
{
    protocol::Plist::Dictionary command{
        {"ApplicationIdentifier", protocol::Plist(bundle_id)},
        {"ClientOptions", protocol::Plist::dictionary({})},
        {"Command", protocol::Plist("Uninstall")},
    };
    return run_installer(device, std::move(command));
}

Result<CommandResult> launch(Device &device, std::string_view bundle_id)
{
    auto service = open_service(device, kProcessControl);
    if (!service)
    {
        return tl::unexpected(service.error());
    }

    protocol::Plist::Dictionary command{
        {"BundleId", protocol::Plist(bundle_id)},
        {"Command", protocol::Plist("process_launch")},
    };
    if (Status status = service->send(protocol::Plist::dictionary(std::move(command))); !status)
    {
        return tl::unexpected(status.error());
    }

    auto answer = service->receive();
    if (!answer)
    {
        return tl::unexpected(answer.error());
    }

    CommandResult result;
    const protocol::Plist *error = answer->find("Error");
    result.success = error == nullptr;
    result.output = error != nullptr ? error->string_or("launch failed") : std::string();
    return result;
}

Status close(Device &device, std::string_view bundle_id)
{
    auto service = open_service(device, kProcessControl);
    if (!service)
    {
        return tl::unexpected(service.error());
    }

    protocol::Plist::Dictionary command{
        {"BundleId", protocol::Plist(bundle_id)},
        {"Command", protocol::Plist("process_kill")},
    };
    return service->send(protocol::Plist::dictionary(std::move(command)));
}

Result<bool> is_running(Device &device, std::string_view bundle_id)
{
    auto service = open_service(device, kProcessControl);
    if (!service)
    {
        return tl::unexpected(service.error());
    }

    protocol::Plist::Dictionary command{{"Command", protocol::Plist("process_list")}};
    if (Status status = service->send(protocol::Plist::dictionary(std::move(command))); !status)
    {
        return tl::unexpected(status.error());
    }

    auto answer = service->receive();
    if (!answer)
    {
        return tl::unexpected(answer.error());
    }
    if (const protocol::Plist *error = answer->find("Error"); error != nullptr)
    {
        return tl::unexpected(Error{ErrorCode::Device, error->string_or("process_list failed")});
    }

    const protocol::Plist *list = answer->find("ProcessList");
    if (list == nullptr || list->array() == nullptr)
    {
        return false;
    }
    for (const protocol::Plist &process : *list->array())
    {
        const protocol::Plist *name = process.find("BundleId");
        if (name != nullptr && name->string_or() == bundle_id)
        {
            return true;
        }
    }
    return false;
}

} // namespace ioscpp
