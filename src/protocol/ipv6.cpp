#include "ioscpp/protocol/ipv6.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace ioscpp::protocol
{
namespace
{

Error protocol_error(std::string message)
{
    return Error{ErrorCode::Protocol, std::move(message)};
}

/// The bytes one transport read moves at a time.
constexpr std::size_t kReadSize = 16384;

} // namespace

std::array<std::byte, kIpv6HeaderSize> Ipv6Header::encode() const noexcept
{
    std::array<std::byte, kIpv6HeaderSize> bytes{};
    bytes[0] = static_cast<std::byte>(((version & 0x0f) << 4) | ((traffic_class >> 4) & 0x0f));
    bytes[1] = static_cast<std::byte>(((traffic_class & 0x0f) << 4) | ((flow_label >> 16) & 0x0f));
    bytes[2] = static_cast<std::byte>((flow_label >> 8) & 0xff);
    bytes[3] = static_cast<std::byte>(flow_label & 0xff);
    bytes[4] = static_cast<std::byte>((payload_length >> 8) & 0xff);
    bytes[5] = static_cast<std::byte>(payload_length & 0xff);
    bytes[6] = static_cast<std::byte>(next_header);
    bytes[7] = static_cast<std::byte>(hop_limit);
    for (std::size_t i = 0; i < source.size(); ++i)
    {
        bytes[8 + i] = source[i];
        bytes[24 + i] = destination[i];
    }
    return bytes;
}

Ipv6Header Ipv6Header::decode(std::span<const std::byte, kIpv6HeaderSize> bytes) noexcept
{
    Ipv6Header header;
    header.version = static_cast<std::uint8_t>(static_cast<unsigned>(bytes[0]) >> 4);
    header.traffic_class = static_cast<std::uint8_t>(((static_cast<unsigned>(bytes[0]) & 0x0f) << 4) |
                                                     ((static_cast<unsigned>(bytes[1]) >> 4) & 0x0f));
    header.flow_label = (static_cast<std::uint32_t>(static_cast<unsigned>(bytes[1]) & 0x0f) << 16) |
                        (static_cast<std::uint32_t>(bytes[2]) << 8) | static_cast<std::uint32_t>(bytes[3]);
    header.payload_length =
        static_cast<std::uint16_t>((static_cast<unsigned>(bytes[4]) << 8) | static_cast<unsigned>(bytes[5]));
    header.next_header = static_cast<std::uint8_t>(bytes[6]);
    header.hop_limit = static_cast<std::uint8_t>(bytes[7]);
    for (std::size_t i = 0; i < header.source.size(); ++i)
    {
        header.source[i] = bytes[8 + i];
        header.destination[i] = bytes[24 + i];
    }
    return header;
}

Ipv6Framer::Ipv6Framer(Transport &transport, std::size_t max_packet_size) noexcept
    : transport_(&transport)
    , max_packet_size_(max_packet_size)
{
}

Status Ipv6Framer::fill(std::size_t count)
{
    // The consumed prefix is dropped once, before reading, so the buffer stays
    // bounded by the largest packet rather than growing with every packet.
    if (offset_ > 0)
    {
        buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(offset_));
        offset_ = 0;
    }

    while (buffer_.size() < count)
    {
        std::array<std::byte, kReadSize> chunk{};
        auto read = transport_->read(chunk);
        if (!read)
        {
            return tl::unexpected(read.error());
        }
        if (*read == 0)
        {
            return tl::unexpected(protocol_error("the tunnel closed mid-packet"));
        }
        buffer_.insert(buffer_.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(*read));
    }
    return {};
}

Result<std::vector<std::byte>> Ipv6Framer::read_packet()
{
    if (Status status = fill(kIpv6HeaderSize); !status)
    {
        return tl::unexpected(status.error());
    }

    const Ipv6Header header =
        Ipv6Header::decode(std::span<const std::byte, kIpv6HeaderSize>(buffer_.data() + offset_, kIpv6HeaderSize));
    if (header.version != 6)
    {
        return tl::unexpected(protocol_error("the tunnel packet is not IPv6"));
    }

    const std::size_t total = kIpv6HeaderSize + header.payload_length;
    if (total > max_packet_size_)
    {
        return tl::unexpected(protocol_error("the tunnel packet is larger than the negotiated MTU"));
    }
    if (Status status = fill(total); !status)
    {
        return tl::unexpected(status.error());
    }

    std::vector<std::byte> packet(buffer_.begin() + static_cast<std::ptrdiff_t>(offset_),
                                  buffer_.begin() + static_cast<std::ptrdiff_t>(offset_ + total));
    offset_ += total;
    return packet;
}

Status Ipv6Framer::write_packet(std::span<const std::byte> packet)
{
    return transport_->write(packet);
}

} // namespace ioscpp::protocol
