#include "ioscpp/rsd.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ioscpp/plist_service.hpp"
#include "ioscpp/protocol/http2.hpp"
#include "ioscpp/protocol/plist.hpp"
#include "ioscpp/protocol/remotexpc.hpp"

namespace ioscpp
{
namespace
{

Error protocol_error(std::string message)
{
    return Error{ErrorCode::Protocol, std::move(message)};
}

Error device_error(std::string message)
{
    return Error{ErrorCode::Device, std::move(message)};
}

/// The suffix a lockdown service shimmed over the RSD carries.
constexpr std::string_view kShimSuffix = ".shim.remote";

/// The device handshake the RSD answers `Properties` and `Services` to.
protocol::Xpc device_handshake(const RsdUuid &uuid)
{
    protocol::Xpc::Dictionary properties;
    properties.emplace("RemoteXPCVersionFlags", protocol::Xpc::uint64(0x0100000000000006ULL));
    properties.emplace("SensitivePropertiesVisible", protocol::Xpc(true));

    protocol::Xpc::Dictionary handshake;
    handshake.emplace("MessageType", protocol::Xpc("Handshake"));
    handshake.emplace("MessagingProtocolVersion", protocol::Xpc::uint64(7));
    handshake.emplace("UUID", protocol::Xpc::uuid(uuid));
    handshake.emplace("Properties", protocol::Xpc::dictionary(std::move(properties)));
    handshake.emplace("Services", protocol::Xpc::dictionary({}));
    return protocol::Xpc::dictionary(std::move(handshake));
}

/// The reply's `Properties` and `Services`, parsed into the service dictionary.
std::map<std::string, RsdService, std::less<>> parse_services(const protocol::Xpc &reply)
{
    std::map<std::string, RsdService, std::less<>> services;
    const protocol::Xpc *entries = reply.find("Services");
    if (entries == nullptr || entries->dictionary() == nullptr)
    {
        return services;
    }
    for (const auto &[name, entry] : *entries->dictionary())
    {
        RsdService service;
        if (const protocol::Xpc *port = entry.find("Port"); port != nullptr && port->string().has_value())
        {
            service.port = static_cast<std::uint16_t>(std::stoi(std::string(*port->string())));
        }
        if (const protocol::Xpc *properties = entry.find("Properties"); properties != nullptr)
        {
            const protocol::Xpc *uses = properties->find("UsesRemoteXPC");
            service.uses_remote_xpc = uses != nullptr && uses->boolean().value_or(false);
        }
        services.emplace(name, service);
    }
    return services;
}

} // namespace

RsdUuid rsd_uuid(std::string_view host_id)
{
    // FNV-1a over the host id, which is stable across runs and processes.
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    for (const char c : host_id)
    {
        hash ^= static_cast<unsigned char>(c);
        hash *= 0x100000001b3ULL;
    }
    RsdUuid uuid{};
    for (std::size_t i = 0; i < uuid.size(); ++i)
    {
        uuid[i] = static_cast<std::byte>((hash >> ((i % 8) * 8)) & 0xff);
    }
    return uuid;
}

struct Rsd::Impl
{
    Tunnel *tunnel = nullptr;
    std::optional<TcpLink> link;
    std::optional<protocol::Http2> http2;
    std::map<std::string, RsdService, std::less<>> services;

    /// Runs the RemoteXPC connection setup and the device handshake.
    Status handshake(const RsdUuid &uuid);
    /// Runs a service's `RSDCheckin` over `service_link`.
    Status checkin(TcpLink &service_link);
};

Status Rsd::Impl::handshake(const RsdUuid &uuid)
{
    // The connection setup, in the frame order the device validates: HEADERS#1,
    // DATA#1 (init), HEADERS#3, DATA#1 (term), DATA#3 (init handshake).
    if (auto status = http2->open(protocol::kHttp2ControlStream); !status)
    {
        return status;
    }
    protocol::XpcWrapper init;
    init.flags = protocol::kXpcFlagAlwaysSet;
    init.payload = protocol::XpcPayload{protocol::Xpc::dictionary({})};
    if (auto status = http2->write(protocol::kHttp2ControlStream, init.encode()); !status)
    {
        return status;
    }
    if (auto status = http2->open(protocol::kHttp2ReplyStream); !status)
    {
        return status;
    }

    // The term frame carries the literal `0x0201` flags the reference sends, not `AlwaysSet|Reply`.
    protocol::XpcWrapper term;
    term.flags = 0x0201;
    if (auto status = http2->write(protocol::kHttp2ControlStream, term.encode()); !status)
    {
        return status;
    }

    protocol::XpcWrapper handshake;
    handshake.flags = protocol::kXpcFlagAlwaysSet | protocol::kXpcFlagInitHandshake;
    if (auto status = http2->write(protocol::kHttp2ReplyStream, handshake.encode()); !status)
    {
        return status;
    }

    // The device handshake. The init request above consumed message id 0, so this one is id 1, and it
    // is sent without wanting a reply.
    const std::vector<std::byte> request = protocol::XpcWrapper::request(1, device_handshake(uuid), false).encode();
    if (auto status = http2->write(protocol::kHttp2ControlStream, request); !status)
    {
        return status;
    }

    // The device acknowledges the setup with a bodyless reply before the handshake answer, so keep
    // reading until the answer arrives: a wrapper whose body is an empty object is not it.
    while (true)
    {
        auto reply = http2->read();
        if (!reply)
        {
            return tl::unexpected(reply.error());
        }
        auto message = protocol::XpcWrapper::parse(reply->payload);
        if (!message)
        {
            return tl::unexpected(message.error());
        }
        if (!message->payload.has_value())
        {
            continue;
        }
        const auto *dict = message->payload->object.dictionary();
        if (dict == nullptr || dict->empty())
        {
            continue;
        }
        const auto *message_type = message->payload->object.find("MessageType");
        if (message_type == nullptr || message_type->string() != "Handshake")
        {
            continue;
        }
        services = parse_services(message->payload->object);
        return {};
    }
}

Status Rsd::Impl::checkin(TcpLink &service_link)
{
    PlistService service(service_link);

    protocol::Plist::Dictionary request;
    request.emplace("Label", protocol::Plist("ioscpp"));
    request.emplace("ProtocolVersion", protocol::Plist("2"));
    request.emplace("Request", protocol::Plist("RSDCheckin"));
    if (auto status = service.send(protocol::Plist::dictionary(std::move(request))); !status)
    {
        return status;
    }

    auto checkin = service.receive();
    if (!checkin)
    {
        return tl::unexpected(checkin.error());
    }
    const protocol::Plist *request_field = checkin->find("Request");
    if (request_field == nullptr || request_field->string() != "RSDCheckin")
    {
        return tl::unexpected(protocol_error("the service's check-in answer is not an RSDCheckin"));
    }

    auto start = service.receive();
    if (!start)
    {
        return tl::unexpected(start.error());
    }
    request_field = start->find("Request");
    if (request_field == nullptr || request_field->string() != "StartService")
    {
        return tl::unexpected(protocol_error("the service's check-in answer has no StartService"));
    }
    if (const protocol::Plist *error = start->find("Error");
        error != nullptr && error->string().has_value() && !error->string()->empty())
    {
        return tl::unexpected(device_error(std::string(*error->string())));
    }
    return {};
}

Rsd::Rsd(Tunnel &tunnel)
    : impl_(std::make_unique<Impl>())
{
    impl_->tunnel = &tunnel;
}

Rsd::Rsd(Rsd &&) noexcept = default;

Rsd &Rsd::operator=(Rsd &&) noexcept = default;

Rsd::~Rsd()
{
    close();
}

Result<Rsd> Rsd::connect(Tunnel &tunnel, const RsdUuid &uuid)
{
    Rsd rsd(tunnel);

    auto link = TcpLink::open(tunnel, tunnel.client_address(), tunnel.mtu());
    if (!link)
    {
        return tl::unexpected(link.error());
    }
    if (auto status = link->connect(tunnel.address(), tunnel.port()); !status)
    {
        return tl::unexpected(status.error());
    }

    // The link is moved into the connection before the HTTP/2 layer borrows it, so
    // the layer's pointer stays valid rather than pointing at the moved-from link.
    rsd.impl_->link = std::move(*link);

    auto http2 = protocol::Http2::connect(*rsd.impl_->link);
    if (!http2)
    {
        return tl::unexpected(http2.error());
    }
    rsd.impl_->http2 = std::move(*http2);

    if (auto status = rsd.impl_->handshake(uuid); !status)
    {
        return tl::unexpected(status.error());
    }
    return rsd;
}

const std::map<std::string, RsdService, std::less<>> &Rsd::services() const noexcept
{
    return impl_->services;
}

Result<TcpLink> Rsd::start_service(std::string_view name)
{
    const auto found = impl_->services.find(name);
    if (found == impl_->services.end())
    {
        return tl::unexpected(device_error("the device does not advertise the service"));
    }

    auto link = TcpLink::open(*impl_->tunnel, impl_->tunnel->client_address(), impl_->tunnel->mtu());
    if (!link)
    {
        return tl::unexpected(link.error());
    }
    if (auto status = link->connect(impl_->tunnel->address(), found->second.port); !status)
    {
        return tl::unexpected(status.error());
    }
    // A `.shim.remote` service is a lockdown service shimmed over the RSD, so it
    // needs the `RSDCheckin` handshake. A native service like
    // `com.apple.instruments.dtservicehub` speaks its own protocol on the plain
    // connection and resets it when it is checked in to.
    const bool is_shim =
        name.size() >= kShimSuffix.size() && name.substr(name.size() - kShimSuffix.size()) == kShimSuffix;
    if (is_shim)
    {
        if (auto status = impl_->checkin(*link); !status)
        {
            return tl::unexpected(status.error());
        }
    }
    return std::move(*link);
}

void Rsd::close()
{
    if (impl_ != nullptr)
    {
        impl_->http2.reset();
        impl_->link.reset();
    }
}

} // namespace ioscpp
