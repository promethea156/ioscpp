#include "ioscpp/app.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

#include "ioscpp/afc.hpp"
#include "ioscpp/device.hpp"
#include "ioscpp/error.hpp"
#include "ioscpp/lockdown.hpp"
#include "ioscpp/plist_service.hpp"
#include "ioscpp/process_control.hpp"
#include "ioscpp/protocol/plist.hpp"
#include "ioscpp/stream.hpp"

namespace ioscpp
{
namespace
{

/// The `installation_proxy` service name and the instruments service names.
constexpr std::string_view kInstallationProxy = "com.apple.mobile.installation_proxy";
constexpr std::string_view kInstruments = "com.apple.instruments.remoteserver";

/// The RSD shim service names the iOS 17.4+ path uses.
constexpr std::string_view kInstallationProxyShim = "com.apple.mobile.installation_proxy.shim.remote";
constexpr std::string_view kAfcShim = "com.apple.afc.shim.remote";
constexpr std::string_view kInstrumentsRsd = "com.apple.instruments.dtservicehub";

/// The staging directory an IPA is uploaded into before it is installed.
constexpr std::string_view kStagingDirectory = "/PublicStaging";

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
Result<PackageResult> run_installer(PlistService &service, protocol::Plist::Dictionary command)
{
    if (Status status = service.send(protocol::Plist::dictionary(std::move(command))); !status)
    {
        return tl::unexpected(status.error());
    }

    PackageResult result;
    for (;;)
    {
        auto status = service.receive();
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
    auto service = open_service(device, kInstallationProxy);
    if (!service)
    {
        return tl::unexpected(service.error());
    }
    return run_installer(*service, std::move(command));
}

Result<PackageResult> uninstall(Device &device, std::string_view bundle_id)
{
    protocol::Plist::Dictionary command{
        {"ApplicationIdentifier", protocol::Plist(bundle_id)},
        {"ClientOptions", protocol::Plist::dictionary({})},
        {"Command", protocol::Plist("Uninstall")},
    };
    auto service = open_service(device, kInstallationProxy);
    if (!service)
    {
        return tl::unexpected(service.error());
    }
    return run_installer(*service, std::move(command));
}

Result<PackageResult> install(Rsd &rsd, const std::filesystem::path &ipa)
{
    if (!std::filesystem::is_regular_file(ipa))
    {
        return tl::unexpected(Error{ErrorCode::InvalidArgument, "the IPA is not a regular file"});
    }

    const std::string remote = std::string(kStagingDirectory) + "/" + ipa.filename().string();
    // The AFC shim and the installer shim are separate connections, so the AFC
    // one is closed before the installer one opens.
    {
        auto link = rsd.start_service(kAfcShim);
        if (!link)
        {
            return tl::unexpected(link.error());
        }
        auto afc = Afc::start(*link);
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

    auto link = rsd.start_service(kInstallationProxyShim);
    if (!link)
    {
        return tl::unexpected(link.error());
    }
    PlistService service(*link);
    protocol::Plist::Dictionary command{
        {"ClientOptions", protocol::Plist::dictionary({{"PackageType", protocol::Plist("Developer")}})},
        {"Command", protocol::Plist("Install")},
        {"PackagePath", protocol::Plist(remote)},
    };
    auto result = run_installer(service, std::move(command));
    if (!result)
    {
        return tl::unexpected(result.error());
    }
    return result;
}

Result<PackageResult> uninstall(Rsd &rsd, std::string_view bundle_id)
{
    auto link = rsd.start_service(kInstallationProxyShim);
    if (!link)
    {
        return tl::unexpected(link.error());
    }
    PlistService service(*link);
    protocol::Plist::Dictionary command{
        {"ApplicationIdentifier", protocol::Plist(bundle_id)},
        {"ClientOptions", protocol::Plist::dictionary({})},
        {"Command", protocol::Plist("Uninstall")},
    };
    return run_installer(service, std::move(command));
}

Result<std::uint64_t> launch(Device &device, std::string_view bundle_id)
{
    auto stream = device.start_service(kInstruments);
    if (!stream)
    {
        return tl::unexpected(stream.error());
    }
    auto control = ProcessControl::start(*stream);
    if (!control)
    {
        return tl::unexpected(control.error());
    }
    return control->launch(bundle_id);
}

Status close(Device &device, std::uint64_t pid)
{
    auto stream = device.start_service(kInstruments);
    if (!stream)
    {
        return tl::unexpected(stream.error());
    }
    auto control = ProcessControl::start(*stream);
    if (!control)
    {
        return tl::unexpected(control.error());
    }
    return control->kill(pid);
}

Result<bool> is_running(Device &device, std::string_view bundle_id)
{
    auto stream = device.start_service(kInstruments);
    if (!stream)
    {
        return tl::unexpected(stream.error());
    }
    auto control = ProcessControl::start(*stream);
    if (!control)
    {
        return tl::unexpected(control.error());
    }
    return control->is_running(bundle_id);
}

Result<std::uint64_t> launch(Rsd &rsd, std::string_view bundle_id)
{
    auto link = rsd.start_service(kInstrumentsRsd);
    if (!link)
    {
        return tl::unexpected(link.error());
    }
    auto control = ProcessControl::start(*link);
    if (!control)
    {
        return tl::unexpected(control.error());
    }
    return control->launch(bundle_id);
}

Status close(Rsd &rsd, std::uint64_t pid)
{
    auto link = rsd.start_service(kInstrumentsRsd);
    if (!link)
    {
        return tl::unexpected(link.error());
    }
    auto control = ProcessControl::start(*link);
    if (!control)
    {
        return tl::unexpected(control.error());
    }
    return control->kill(pid);
}

Result<bool> is_running(Rsd &rsd, std::string_view bundle_id)
{
    auto link = rsd.start_service(kInstrumentsRsd);
    if (!link)
    {
        return tl::unexpected(link.error());
    }
    auto control = ProcessControl::start(*link);
    if (!control)
    {
        return tl::unexpected(control.error());
    }
    return control->is_running(bundle_id);
}

} // namespace ioscpp
