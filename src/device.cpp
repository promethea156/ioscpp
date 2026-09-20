#include "ioscpp/device.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ioscpp/afc.hpp"
#include "ioscpp/connection.hpp"
#include "ioscpp/crypto/pairing.hpp"
#include "ioscpp/error.hpp"
#include "ioscpp/lockdown.hpp"
#include "ioscpp/protocol/cdtunnel.hpp"
#include "ioscpp/protocol/plist.hpp"
#include "ioscpp/protocol/usbmux.hpp"
#include "ioscpp/stream.hpp"

namespace ioscpp
{
namespace
{

/// The service name of the device's media file service.
constexpr std::string_view kAfcService = "com.apple.afc";

/// The lockdown service that hands out the CoreDevice tunnel, on iOS 17.4+.
constexpr std::string_view kCoreDeviceProxyService = "com.apple.internal.devicecompute.CoreDeviceProxy";

} // namespace

struct Device::Impl
{
    Impl(std::shared_ptr<Connection> connection, Lockdown lockdown, crypto::Pairing *pairing)
        : connection(std::move(connection))
        , lockdown(std::move(lockdown))
        , pairing(pairing)
    {
    }

    std::shared_ptr<Connection> connection;
    Lockdown lockdown;
    crypto::Pairing *pairing;
    bool disconnected = false;
    std::string udid;
    std::string product_type;
    std::string product_version;
};

Device::Device(std::shared_ptr<Connection> connection, Lockdown lockdown, crypto::Pairing &pairing)
    : impl_(std::make_unique<Impl>(std::move(connection), std::move(lockdown), &pairing))
{
}

Device::~Device()
{
    disconnect();
}

Device::Device(Device &&) noexcept = default;

Device &Device::operator=(Device &&other) noexcept
{
    if (this != &other)
    {
        disconnect();
        impl_ = std::move(other.impl_);
    }
    return *this;
}

void Device::disconnect() noexcept
{
    if (impl_ == nullptr || impl_->disconnected)
    {
        return;
    }
    impl_->disconnected = true;
    impl_->lockdown.close();
    impl_->connection->close();
}

Result<Device> Device::connect(Transport &transport, crypto::Pairing &pairing)
{
    auto opened = Connection::open(transport);
    if (!opened)
    {
        return tl::unexpected(opened.error());
    }
    // The connection is held by a `shared_ptr`, because the streams below keep a
    // reference to it and outlive the local scope.
    auto connection = std::make_shared<Connection>(std::move(*opened));

    auto stream = Stream::open(connection, protocol::kLockdownPort);
    if (!stream)
    {
        return tl::unexpected(stream.error());
    }

    auto lockdown = Lockdown::start(std::move(*stream));
    if (!lockdown)
    {
        return tl::unexpected(lockdown.error());
    }

    // The unique id names the pairing record, so it is learned first. A fresh
    // device answers `GetValue` before a session has started.
    if (auto udid = lockdown->get_value({}, "UniqueDeviceID"); udid)
    {
        if (const protocol::Plist *value = udid->find("Value"); value != nullptr)
        {
            pairing.set_udid(value->string_or());
        }
    }

    if (!pairing.paired())
    {
        if (Status status = crypto::pair(*lockdown, pairing); !status)
        {
            return tl::unexpected(status.error());
        }
        if (Status status = pairing.save(); !status)
        {
            return tl::unexpected(status.error());
        }
    }

    if (Status status = lockdown->start_session(pairing); !status)
    {
        return tl::unexpected(status.error());
    }

    Device device(std::move(connection), std::move(*lockdown), pairing);
    if (auto product_type = device.impl_->lockdown.get_value({}, "ProductType"); product_type)
    {
        if (const protocol::Plist *value = product_type->find("Value"); value != nullptr)
        {
            device.impl_->product_type = value->string_or();
        }
    }
    if (auto product_version = device.impl_->lockdown.get_value({}, "ProductVersion"); product_version)
    {
        if (const protocol::Plist *value = product_version->find("Value"); value != nullptr)
        {
            device.impl_->product_version = value->string_or();
        }
    }
    device.impl_->udid = std::string(pairing.udid());
    if (std::getenv("IOSCPP_TRACE") != nullptr)
    {
        std::fprintf(stderr, "[device] product type=%s version=%s\n", device.impl_->product_type.c_str(),
                     device.impl_->product_version.c_str());
    }
    return device;
}

std::string_view Device::udid() const noexcept
{
    return impl_->udid;
}

std::string_view Device::product_type() const noexcept
{
    return impl_->product_type;
}

std::string_view Device::product_version() const noexcept
{
    return impl_->product_version;
}

Lockdown &Device::lockdown() noexcept
{
    return impl_->lockdown;
}

Result<Stream> Device::start_service(std::string_view name)
{
    auto port = impl_->lockdown.start_service(name);
    if (!port)
    {
        return tl::unexpected(port.error());
    }
    return Stream::open(impl_->connection, *port);
}

Result<Afc> Device::open_afc()
{
    auto stream = start_service(kAfcService);
    if (!stream)
    {
        return tl::unexpected(stream.error());
    }
    return Afc::start(std::move(*stream));
}

Result<Tunnel> Device::tunnel()
{
    auto stream = start_service(kCoreDeviceProxyService);
    if (!stream)
    {
        return tl::unexpected(stream.error());
    }

    const protocol::CdtunnelRequest request;
    const std::vector<std::byte> frame = protocol::cdtunnel_encode(request.to_json());
    if (Status status = stream->write(frame); !status)
    {
        return tl::unexpected(status.error());
    }

    // The tunnel's stream has no packet boundaries, so the header is read first
    // and its length then decides how much of the body to read.
    std::array<std::byte, protocol::kCdtunnelHeaderSize> header{};
    if (Status status = stream->read(header); !status)
    {
        return tl::unexpected(status.error());
    }
    auto length = protocol::cdtunnel_header_length(header);
    if (!length)
    {
        return tl::unexpected(length.error());
    }

    std::vector<std::byte> body(*length);
    if (Status status = stream->read(body); !status)
    {
        return tl::unexpected(status.error());
    }

    auto response =
        protocol::CdtunnelResponse::parse(std::string(reinterpret_cast<const char *>(body.data()), body.size()));
    if (!response)
    {
        return tl::unexpected(response.error());
    }

    return Tunnel{std::move(*stream), std::move(response->server_address), response->server_rsd_port,
                  response->client_mtu};
}

} // namespace ioscpp
