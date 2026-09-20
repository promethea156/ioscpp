#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "ioscpp/error.hpp"
#include "ioscpp/export.hpp"
#include "ioscpp/transport.hpp"

namespace ioscpp::protocol
{

/// The size in bytes of the fixed IPv6 header.
inline constexpr std::size_t kIpv6HeaderSize = 40;

/// The largest packet the re-framer accepts: the fixed header and a 16-bit payload.
inline constexpr std::size_t kIpv6MaxPacketSize = kIpv6HeaderSize + 65535;

/**
 * @brief The fixed IPv6 header.
 *
 * `decode` fills every field, but the framer uses only the version and the payload
 * length, and a `TcpLink` also reads the next header. The payload length counts
 * every byte after the fixed header, including any extension headers.
 */
struct IOSCPP_API Ipv6Header
{
    std::uint8_t version = 6;
    std::uint8_t traffic_class = 0;
    std::uint32_t flow_label = 0;
    std::uint16_t payload_length = 0;
    std::uint8_t next_header = 0;
    std::uint8_t hop_limit = 0;
    std::array<std::byte, 16> source{};
    std::array<std::byte, 16> destination{};

    /// Serializes the fixed header.
    std::array<std::byte, kIpv6HeaderSize> encode() const noexcept;

    /// Parses the fixed header.
    static Ipv6Header decode(std::span<const std::byte, kIpv6HeaderSize> bytes) noexcept;
};

/**
 * @brief Reads whole IPv6 packets from the tunnel's raw byte stream.
 *
 * The CoreDevice tunnel is a TCP byte stream with no packet boundaries, so one
 * transport read is not one packet: it can return a partial packet or several
 * coalesced ones. This re-frames the stream by the fixed header and its 16-bit
 * payload length, so @ref read_packet returns exactly one whole packet.
 *
 * A packet larger than `max_packet_size` fails loudly rather than being
 * truncated, because a truncation would desync every packet after it. Writes are
 * already packet-aligned, so @ref write_packet passes them straight through.
 *
 * An `Ipv6Framer` is not thread-safe; it buffers across reads, so concurrent
 * calls must be serialized by the caller.
 */
class IOSCPP_API Ipv6Framer
{
public:
    /**
     * @brief Re-frames `transport`'s byte stream.
     *
     * `max_packet_size` is the largest packet accepted. The handshake's
     * `clientParameters.mtu` is what a caller passes here, so a packet larger
     * than the negotiated MTU is rejected rather than truncated.
     */
    explicit Ipv6Framer(Transport &transport, std::size_t max_packet_size = kIpv6MaxPacketSize) noexcept;

    Ipv6Framer(const Ipv6Framer &) = delete;
    Ipv6Framer &operator=(const Ipv6Framer &) = delete;

    /// Reads exactly one whole IPv6 packet, buffering across reads.
    Result<std::vector<std::byte>> read_packet();

    /// Writes one whole IPv6 packet.
    Status write_packet(std::span<const std::byte> packet);

private:
    /// Ensures `count` bytes are available from `offset_`, reading more as needed.
    Status fill(std::size_t count);

    Transport *transport_;
    std::size_t max_packet_size_;
    std::vector<std::byte> buffer_;
    std::size_t offset_ = 0;
};

} // namespace ioscpp::protocol
