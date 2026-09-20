#include "ioscpp/tunnel.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "ioscpp/crypto/pairing.hpp"
#include "ioscpp/protocol/cdtunnel.hpp"
#include "ioscpp/stream.hpp"

namespace ioscpp
{
namespace
{

Error protocol_error(std::string message)
{
    return Error{ErrorCode::Protocol, std::move(message)};
}

} // namespace

struct Tunnel::Impl
{
    explicit Impl(Stream value)
        : stream(std::move(value))
    {
    }

    Stream stream;
    std::optional<crypto::TlsSession> tls;
    std::string address;
    std::uint16_t port = 0;
    std::uint16_t mtu = 0;

    Result<std::size_t> read(std::span<std::byte> buffer)
    {
        if (!tls.has_value())
        {
            // A stream read is exact, so a success fills the buffer.
            if (Status status = stream.read(buffer); !status)
            {
                return tl::unexpected(status.error());
            }
            return buffer.size();
        }
        return tls->read(buffer);
    }

    Status write(std::span<const std::byte> data)
    {
        return tls.has_value() ? tls->write(data) : stream.write(data);
    }

    /// Reads exactly `buffer.size()` bytes, or fails.
    Status read_exact(std::span<std::byte> buffer)
    {
        std::size_t offset = 0;
        while (offset < buffer.size())
        {
            auto read = this->read(buffer.subspan(offset));
            if (!read)
            {
                return tl::unexpected(read.error());
            }
            if (*read == 0)
            {
                return tl::unexpected(protocol_error("the tunnel closed mid-message"));
            }
            offset += *read;
        }
        return {};
    }
};

Tunnel::Tunnel(Stream stream)
    : impl_(std::make_unique<Impl>(std::move(stream)))
{
}

Tunnel::~Tunnel() = default;
Tunnel::Tunnel(Tunnel &&) noexcept = default;
Tunnel &Tunnel::operator=(Tunnel &&) noexcept = default;

Result<Tunnel> Tunnel::open(Stream stream, crypto::Pairing &pairing, bool enable_ssl)
{
    // The stream is moved into the `Impl` before the TLS session binds to it, so
    // the session's stream pointer stays valid when the `Tunnel` moves.
    Tunnel tunnel(std::move(stream));

    if (enable_ssl)
    {
        auto started = crypto::TlsSession::start(tunnel.impl_->stream, pairing);
        if (!started)
        {
            return tl::unexpected(started.error());
        }
        tunnel.impl_->tls.emplace(std::move(*started));
    }

    const protocol::CdtunnelRequest request;
    const std::vector<std::byte> frame = protocol::cdtunnel_encode(request.to_json());
    if (Status status = tunnel.write(frame); !status)
    {
        return tl::unexpected(status.error());
    }

    // The tunnel's stream has no packet boundaries, so the header is read first
    // and its length then decides how much of the body to read.
    std::array<std::byte, protocol::kCdtunnelHeaderSize> header{};
    if (Status status = tunnel.impl_->read_exact(header); !status)
    {
        return tl::unexpected(status.error());
    }
    auto length = protocol::cdtunnel_header_length(header);
    if (!length)
    {
        return tl::unexpected(length.error());
    }

    std::vector<std::byte> body(*length);
    if (Status status = tunnel.impl_->read_exact(body); !status)
    {
        return tl::unexpected(status.error());
    }

    auto response =
        protocol::CdtunnelResponse::parse(std::string(reinterpret_cast<const char *>(body.data()), body.size()));
    if (!response)
    {
        return tl::unexpected(response.error());
    }

    tunnel.impl_->address = std::move(response->server_address);
    tunnel.impl_->port = response->server_rsd_port;
    tunnel.impl_->mtu = response->client_mtu;
    return tunnel;
}

std::string_view Tunnel::address() const noexcept
{
    return impl_->address;
}

std::uint16_t Tunnel::port() const noexcept
{
    return impl_->port;
}

std::uint16_t Tunnel::mtu() const noexcept
{
    return impl_->mtu;
}

Result<std::size_t> Tunnel::read(std::span<std::byte> buffer)
{
    return impl_->read(buffer);
}

Status Tunnel::write(std::span<const std::byte> data)
{
    return impl_->write(data);
}

} // namespace ioscpp
