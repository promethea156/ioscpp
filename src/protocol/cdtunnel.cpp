#include "ioscpp/protocol/cdtunnel.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ioscpp/protocol/json.hpp"

namespace ioscpp::protocol
{
namespace
{

Error protocol_error(std::string message)
{
    return Error{ErrorCode::Protocol, std::move(message)};
}

/// The magic, the 16-bit length, and nothing else.
constexpr std::size_t kPrefixSize = 8 + 2;

} // namespace

std::string CdtunnelRequest::to_json() const
{
    Json::Object request{
        {"mtu", Json(static_cast<std::int64_t>(mtu))},
        {"type", Json("clientHandshakeRequest")},
    };
    return Json::object(std::move(request)).to_string();
}

Result<CdtunnelResponse> CdtunnelResponse::parse(std::string_view json)
{
    auto parsed = Json::parse(json);
    if (!parsed)
    {
        return tl::unexpected(parsed.error());
    }

    const Json *client = parsed->find("clientParameters");
    if (client == nullptr || client->object() == nullptr)
    {
        return tl::unexpected(protocol_error("the tunnel handshake has no clientParameters"));
    }

    const Json *client_address = client->find("address");
    if (client_address == nullptr || !client_address->string().has_value())
    {
        return tl::unexpected(protocol_error("the tunnel handshake has no client address"));
    }

    const Json *client_mtu = client->find("mtu");
    if (client_mtu == nullptr || !client_mtu->number().has_value())
    {
        return tl::unexpected(protocol_error("the tunnel handshake has no client MTU"));
    }

    const Json *server_address = parsed->find("serverAddress");
    if (server_address == nullptr || !server_address->string().has_value())
    {
        return tl::unexpected(protocol_error("the tunnel handshake has no server address"));
    }

    const Json *server_rsd_port = parsed->find("serverRSDPort");
    if (server_rsd_port == nullptr || !server_rsd_port->number().has_value())
    {
        return tl::unexpected(protocol_error("the tunnel handshake has no RSD port"));
    }

    CdtunnelResponse response;
    response.client_address = std::string(*client_address->string());
    response.client_mtu = static_cast<std::uint16_t>(*client_mtu->number());
    response.server_address = std::string(*server_address->string());
    response.server_rsd_port = static_cast<std::uint16_t>(*server_rsd_port->number());
    return response;
}

std::vector<std::byte> cdtunnel_encode(std::string_view body)
{
    std::vector<std::byte> frame(kPrefixSize + body.size());
    std::memcpy(frame.data(), kCdtunnelMagic.data(), kCdtunnelMagic.size());
    frame[kCdtunnelMagic.size()] = static_cast<std::byte>((body.size() >> 8) & 0xff);
    frame[kCdtunnelMagic.size() + 1] = static_cast<std::byte>(body.size() & 0xff);
    std::memcpy(frame.data() + kPrefixSize, body.data(), body.size());
    return frame;
}

Result<std::string> cdtunnel_decode(std::span<const std::byte> frame)
{
    if (frame.size() < kPrefixSize)
    {
        return tl::unexpected(protocol_error("the tunnel frame is shorter than its header"));
    }
    if (std::memcmp(frame.data(), kCdtunnelMagic.data(), kCdtunnelMagic.size()) != 0)
    {
        return tl::unexpected(protocol_error("the tunnel frame has a bad magic"));
    }
    const std::size_t length = (static_cast<std::size_t>(frame[kCdtunnelMagic.size()]) << 8) |
                               static_cast<std::size_t>(frame[kCdtunnelMagic.size() + 1]);
    if (frame.size() != kPrefixSize + length)
    {
        return tl::unexpected(protocol_error("the tunnel frame length does not match its body"));
    }
    return std::string(reinterpret_cast<const char *>(frame.data() + kPrefixSize), length);
}

} // namespace ioscpp::protocol
