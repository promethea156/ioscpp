#include "ioscpp/device.hpp"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "ioscpp/afc.hpp"
#include "ioscpp/connection.hpp"
#include "ioscpp/crypto/pairing.hpp"
#include "ioscpp/error.hpp"
#include "ioscpp/lockdown.hpp"
#include "ioscpp/protocol/plist.hpp"
#include "ioscpp/protocol/usbmux.hpp"
#include "ioscpp/stream.hpp"

namespace ioscpp
{
namespace
{

/// The service name of the device's media file service.
constexpr std::string_view kAfcService = "com.apple.afc";

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

} // namespace ioscpp
