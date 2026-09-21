#include "ioscpp/house_arrest.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "ioscpp/device.hpp"
#include "ioscpp/plist_service.hpp"
#include "ioscpp/protocol/plist.hpp"
#include "ioscpp/rsd.hpp"
#include "ioscpp/stream.hpp"
#include "ioscpp/tcp_link.hpp"

namespace ioscpp
{
namespace
{

/// The mux-link service and its iOS 17.4+ RSD shim.
constexpr std::string_view kHouseArrestService = "com.apple.mobile.house_arrest";
constexpr std::string_view kHouseArrestShim = "com.apple.mobile.house_arrest.shim.remote";

/// The two vend commands, which choose the root the AFC session sees.
constexpr std::string_view kVendContainer = "VendContainer";
constexpr std::string_view kVendDocuments = "VendDocuments";

Error device_error(std::string message)
{
    return Error{ErrorCode::Device, std::move(message)};
}

/// Sends the vend command over `stream` and checks the device's answer.
Status vend(ByteStream &stream, std::string_view bundle_id, bool documents_only)
{
    PlistService service(stream);
    protocol::Plist::Dictionary command{
        {"Command", protocol::Plist(documents_only ? kVendDocuments : kVendContainer)},
        {"Identifier", protocol::Plist(bundle_id)},
    };
    if (Status status = service.send(protocol::Plist::dictionary(std::move(command))); !status)
    {
        return status;
    }

    auto answer = service.receive();
    if (!answer)
    {
        return tl::unexpected(answer.error());
    }
    if (const protocol::Plist *error = answer->find("Error"); error != nullptr)
    {
        return tl::unexpected(device_error(error->string_or("the device refused the vend command")));
    }
    return {};
}

} // namespace

struct HouseArrest::Impl
{
    /// The RSD service link, when the client owns it. The mux path is owned by `afc`.
    std::optional<TcpLink> owned_link;
    std::optional<Afc> afc;
};

HouseArrest::HouseArrest() = default;
HouseArrest::~HouseArrest() = default;
HouseArrest::HouseArrest(HouseArrest &&) noexcept = default;
HouseArrest &HouseArrest::operator=(HouseArrest &&) noexcept = default;

Result<HouseArrest> HouseArrest::start(Device &device, std::string_view bundle_id, bool documents_only)
{
    auto stream = device.start_service(kHouseArrestService);
    if (!stream)
    {
        return tl::unexpected(stream.error());
    }
    if (Status status = vend(*stream, bundle_id, documents_only); !status)
    {
        return tl::unexpected(status.error());
    }

    auto afc = Afc::start(std::move(*stream));
    if (!afc)
    {
        return tl::unexpected(afc.error());
    }

    HouseArrest house_arrest;
    house_arrest.impl_ = std::make_unique<Impl>();
    house_arrest.impl_->afc = std::move(*afc);
    return house_arrest;
}

Result<HouseArrest> HouseArrest::start(Rsd &rsd, std::string_view bundle_id, bool documents_only)
{
    auto link = rsd.start_service(kHouseArrestShim);
    if (!link)
    {
        return tl::unexpected(link.error());
    }
    if (Status status = vend(*link, bundle_id, documents_only); !status)
    {
        return tl::unexpected(status.error());
    }

    HouseArrest house_arrest;
    house_arrest.impl_ = std::make_unique<Impl>();
    house_arrest.impl_->owned_link = std::move(*link);

    // The AFC client borrows the link, so the link stays in the heap-allocated
    // `Impl`, whose address does not change when the `HouseArrest` moves.
    auto afc = Afc::start(*house_arrest.impl_->owned_link);
    if (!afc)
    {
        return tl::unexpected(afc.error());
    }
    house_arrest.impl_->afc = std::move(*afc);
    return house_arrest;
}

Result<HouseArrest> HouseArrest::start(ByteStream &stream, std::string_view bundle_id, bool documents_only)
{
    if (Status status = vend(stream, bundle_id, documents_only); !status)
    {
        return tl::unexpected(status.error());
    }

    auto afc = Afc::start(stream);
    if (!afc)
    {
        return tl::unexpected(afc.error());
    }

    HouseArrest house_arrest;
    house_arrest.impl_ = std::make_unique<Impl>();
    house_arrest.impl_->afc = std::move(*afc);
    return house_arrest;
}

Afc &HouseArrest::afc() noexcept
{
    return *impl_->afc;
}

void HouseArrest::close() noexcept
{
    if (impl_ == nullptr)
    {
        return;
    }
    impl_->afc.reset();
    if (impl_->owned_link.has_value())
    {
        impl_->owned_link->close();
        impl_->owned_link.reset();
    }
}

} // namespace ioscpp
