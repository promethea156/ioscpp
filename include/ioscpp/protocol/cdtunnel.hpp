#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ioscpp/error.hpp"
#include "ioscpp/export.hpp"

namespace ioscpp::protocol
{

/// The 8-byte ASCII magic every CoreDevice tunnel frame starts with.
inline constexpr std::string_view kCdtunnelMagic = "CDTunnel";

/// The size of a `CDTunnel` frame's header: the magic and the 16-bit length.
inline constexpr std::size_t kCdtunnelHeaderSize = 8 + 2;

/// The MTU the host requests in the handshake, the IPv6 minimum (go-ios).
inline constexpr std::uint16_t kCdtunnelDefaultMtu = 1280;

/**
 * @brief The host's `clientHandshakeRequest`.
 *
 * The host sends this as the body of the `CDTunnel` frame, and the device answers
 * with a @ref CdtunnelResponse.
 */
struct IOSCPP_API CdtunnelRequest
{
    /// The MTU the host asks the device to use for the tunnel link.
    std::uint16_t mtu = kCdtunnelDefaultMtu;

    /// Serializes the request JSON.
    std::string to_json() const;
};

/**
 * @brief The device's `serverHandshakeResponse`.
 *
 * `server_address` and `server_rsd_port` are the RSD endpoint; `client_address` is
 * the tunnel-local IPv6 address and `client_mtu` sizes the re-framer's buffer.
 */
struct IOSCPP_API CdtunnelResponse
{
    /// The tunnel-local IPv6 address the host uses on the link.
    std::string client_address;
    /// The MTU the host uses to size its re-framer's buffer.
    std::uint16_t client_mtu = 0;
    /// The IPv6 address of the RSD on the device.
    std::string server_address;
    /// The RSD port on the device.
    std::uint16_t server_rsd_port = 0;

    /// Parses the response JSON, failing on a missing or mistyped field.
    static Result<CdtunnelResponse> parse(std::string_view json);
};

/// Encodes a `CDTunnel` frame: the magic, a 16-bit big-endian body length, the body.
IOSCPP_API std::vector<std::byte> cdtunnel_encode(std::string_view body);

/**
 * @brief Reads a `CDTunnel` frame's header, returning its body length.
 *
 * The header is read first and the body length then read, because the tunnel's
 * stream is boundary-less and a body length is not known before the header. A bad
 * magic is an `ErrorCode::Protocol` error.
 */
IOSCPP_API Result<std::uint16_t> cdtunnel_header_length(std::span<const std::byte, kCdtunnelHeaderSize> header);

/**
 * @brief Decodes a `CDTunnel` frame body.
 *
 * The span must hold the whole frame, and its length must match the length word,
 * so a partial or coalesced frame is an `ErrorCode::Protocol` error.
 */
IOSCPP_API Result<std::string> cdtunnel_decode(std::span<const std::byte> frame);

} // namespace ioscpp::protocol
