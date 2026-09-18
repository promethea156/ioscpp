#include "ioscpp/lockdown.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ioscpp/crypto/pairing.hpp"
#include "ioscpp/error.hpp"
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

void put_be32(std::span<std::byte> bytes, std::uint32_t value) noexcept
{
    bytes[0] = static_cast<std::byte>((value >> 24) & 0xff);
    bytes[1] = static_cast<std::byte>((value >> 16) & 0xff);
    bytes[2] = static_cast<std::byte>((value >> 8) & 0xff);
    bytes[3] = static_cast<std::byte>(value & 0xff);
}

std::uint32_t get_be32(std::span<const std::byte> bytes) noexcept
{
    return (static_cast<std::uint32_t>(bytes[0]) << 24) | (static_cast<std::uint32_t>(bytes[1]) << 16) |
           (static_cast<std::uint32_t>(bytes[2]) << 8) | static_cast<std::uint32_t>(bytes[3]);
}

} // namespace

struct Lockdown::Impl
{
    explicit Impl(Stream value)
        : stream(std::move(value))
    {
    }

    Stream stream;
    std::optional<crypto::TlsSession> tls;

    Status write(std::span<const std::byte> data)
    {
        return tls.has_value() ? tls->write(data) : stream.write(data);
    }

    Status read(std::span<std::byte> buffer)
    {
        // A stream read is exact, but a TLS read returns what one record holds,
        // so the loop reads on until `buffer` is filled.
        std::size_t offset = 0;
        while (offset < buffer.size())
        {
            if (!tls.has_value())
            {
                return stream.read(buffer.subspan(offset));
            }
            auto read = tls->read(buffer.subspan(offset));
            if (!read)
            {
                return tl::unexpected(read.error());
            }
            if (*read == 0)
            {
                return tl::unexpected(protocol_error("the link closed mid-message"));
            }
            offset += *read;
        }
        return {};
    }
};

Lockdown::Lockdown(Stream stream)
    : impl_(std::make_unique<Impl>(std::move(stream)))
{
}

Lockdown::~Lockdown() = default;
Lockdown::Lockdown(Lockdown &&) noexcept = default;
Lockdown &Lockdown::operator=(Lockdown &&) noexcept = default;

Result<Lockdown> Lockdown::start(Stream stream)
{
    return Lockdown(std::move(stream));
}

Stream &Lockdown::stream() noexcept
{
    return impl_->stream;
}

Result<protocol::Plist> Lockdown::request(protocol::Plist request)
{
    const std::string body = request.to_xml();

    std::vector<std::byte> message(4 + body.size());
    put_be32(message, static_cast<std::uint32_t>(message.size()));
    std::copy(reinterpret_cast<const std::byte *>(body.data()),
              reinterpret_cast<const std::byte *>(body.data() + body.size()), message.begin() + 4);

    if (Status status = impl_->write(message); !status)
    {
        return tl::unexpected(status.error());
    }

    std::array<std::byte, 4> length_bytes{};
    if (Status status = impl_->read(length_bytes); !status)
    {
        return tl::unexpected(status.error());
    }
    const std::uint32_t length = get_be32(length_bytes);
    if (length < 8)
    {
        return tl::unexpected(protocol_error("the lockdownd answer is too short"));
    }

    std::vector<std::byte> answer(length);
    if (Status status = impl_->read(answer); !status)
    {
        return tl::unexpected(status.error());
    }

    auto answer_plist = protocol::Plist::parse(answer);
    if (!answer_plist)
    {
        return tl::unexpected(answer_plist.error());
    }

    if (const protocol::Plist *error = answer_plist->find("Error"); error != nullptr)
    {
        return tl::unexpected(Error{ErrorCode::Device, error->string_or("the device refused the request")});
    }
    return answer_plist;
}

Result<protocol::Plist> Lockdown::query(std::string_view type)
{
    protocol::Plist::Dictionary request{
        {"Request", protocol::Plist("QueryType")},
        {"Type", protocol::Plist(type)},
    };
    return this->request(protocol::Plist::dictionary(std::move(request)));
}

Result<protocol::Plist> Lockdown::get_value(std::string_view domain, std::string_view key)
{
    protocol::Plist::Dictionary request{{"Request", protocol::Plist("GetValue")}};
    if (!domain.empty())
    {
        request.emplace("Domain", protocol::Plist(domain));
    }
    if (!key.empty())
    {
        request.emplace("Key", protocol::Plist(key));
    }
    return this->request(protocol::Plist::dictionary(std::move(request)));
}

Status Lockdown::start_session(const crypto::Pairing &pairing)
{
    protocol::Plist::Dictionary request{
        {"HostID", protocol::Plist(pairing.host_id())},
        {"Request", protocol::Plist("StartSession")},
        {"SystemBUID", protocol::Plist(pairing.system_buid())},
    };

    auto answer = this->request(protocol::Plist::dictionary(std::move(request)));
    if (!answer)
    {
        return tl::unexpected(answer.error());
    }

    const protocol::Plist *enable_ssl = answer->find("EnableSessionSSL");
    if (enable_ssl != nullptr && enable_ssl->boolean().value_or(false))
    {
        auto tls = crypto::TlsSession::start(impl_->stream, pairing);
        if (!tls)
        {
            return tl::unexpected(tls.error());
        }
        impl_->tls.emplace(std::move(*tls));
    }
    return {};
}

Result<std::uint16_t> Lockdown::start_service(std::string_view name)
{
    protocol::Plist::Dictionary request{
        {"Request", protocol::Plist("StartService")},
        {"Service", protocol::Plist(name)},
    };

    auto answer = this->request(protocol::Plist::dictionary(std::move(request)));
    if (!answer)
    {
        return tl::unexpected(answer.error());
    }

    const protocol::Plist *port = answer->find("Port");
    if (port == nullptr || !port->integer().has_value())
    {
        return tl::unexpected(protocol_error("the device did not send a service port"));
    }
    return static_cast<std::uint16_t>(*port->integer());
}

} // namespace ioscpp
