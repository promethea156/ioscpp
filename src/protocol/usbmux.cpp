#include "ioscpp/protocol/usbmux.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace ioscpp::protocol
{
namespace
{

/// Writes `value` into `bytes` at `offset`, big-endian.
void put_u16(std::span<std::byte> bytes, std::size_t offset, std::uint16_t value) noexcept
{
    bytes[offset + 0] = static_cast<std::byte>((value >> 8) & 0xff);
    bytes[offset + 1] = static_cast<std::byte>(value & 0xff);
}

void put_u32(std::span<std::byte> bytes, std::size_t offset, std::uint32_t value) noexcept
{
    bytes[offset + 0] = static_cast<std::byte>((value >> 24) & 0xff);
    bytes[offset + 1] = static_cast<std::byte>((value >> 16) & 0xff);
    bytes[offset + 2] = static_cast<std::byte>((value >> 8) & 0xff);
    bytes[offset + 3] = static_cast<std::byte>(value & 0xff);
}

std::uint16_t get_u16(std::span<const std::byte> bytes, std::size_t offset) noexcept
{
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(bytes[offset + 0]) << 8) |
                                      static_cast<std::uint16_t>(bytes[offset + 1]));
}

std::uint32_t get_u32(std::span<const std::byte> bytes, std::size_t offset) noexcept
{
    return (static_cast<std::uint32_t>(bytes[offset + 0]) << 24) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 16) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 8) | static_cast<std::uint32_t>(bytes[offset + 3]);
}

} // namespace

std::size_t MuxHeader::wire_size(std::uint32_t version) noexcept
{
    return version >= 2 ? kMuxHeaderSize : kMuxHeaderSizeV1;
}

std::array<std::byte, kMuxHeaderSize> MuxHeader::encode(std::uint32_t version) const noexcept
{
    std::array<std::byte, kMuxHeaderSize> bytes{};
    put_u32(bytes, 0, protocol);
    put_u32(bytes, 4, length);
    if (version >= 2)
    {
        put_u32(bytes, 8, magic);
        put_u16(bytes, 12, tx_seq);
        put_u16(bytes, 14, rx_seq);
    }
    return bytes;
}

MuxHeader MuxHeader::decode(std::span<const std::byte> bytes, std::uint32_t version) noexcept
{
    MuxHeader header;
    header.protocol = get_u32(bytes, 0);
    header.length = get_u32(bytes, 4);
    if (version >= 2 && bytes.size() >= kMuxHeaderSize)
    {
        header.magic = get_u32(bytes, 8);
        header.tx_seq = get_u16(bytes, 12);
        header.rx_seq = get_u16(bytes, 14);
    }
    return header;
}

std::array<std::byte, VersionHeader::kSize> VersionHeader::encode() const noexcept
{
    std::array<std::byte, kSize> bytes{};
    put_u32(bytes, 0, major);
    put_u32(bytes, 4, minor);
    put_u32(bytes, 8, padding);
    return bytes;
}

VersionHeader VersionHeader::decode(std::span<const std::byte, kSize> bytes) noexcept
{
    VersionHeader header;
    header.major = get_u32(bytes, 0);
    header.minor = get_u32(bytes, 4);
    header.padding = get_u32(bytes, 8);
    return header;
}

std::array<std::byte, kTcpHeaderSize> TcpHeader::encode() const noexcept
{
    std::array<std::byte, kTcpHeaderSize> bytes{};
    put_u16(bytes, 0, source_port);
    put_u16(bytes, 2, destination_port);
    put_u32(bytes, 4, sequence);
    put_u32(bytes, 8, acknowledgement);
    // The data offset is the high nibble and the reserved bits the low nibble.
    bytes[12] = static_cast<std::byte>(static_cast<std::uint8_t>(data_offset << 4));
    bytes[13] = static_cast<std::byte>(flags);
    put_u16(bytes, 14, window);
    put_u16(bytes, 16, checksum);
    put_u16(bytes, 18, urgent);
    return bytes;
}

TcpHeader TcpHeader::decode(std::span<const std::byte, kTcpHeaderSize> bytes) noexcept
{
    TcpHeader header;
    header.source_port = get_u16(bytes, 0);
    header.destination_port = get_u16(bytes, 2);
    header.sequence = get_u32(bytes, 4);
    header.acknowledgement = get_u32(bytes, 8);
    header.data_offset = static_cast<std::uint8_t>(static_cast<std::uint8_t>(bytes[12]) >> 4);
    header.flags = static_cast<std::uint8_t>(bytes[13]);
    header.window = get_u16(bytes, 14);
    header.checksum = get_u16(bytes, 16);
    header.urgent = get_u16(bytes, 18);
    return header;
}

} // namespace ioscpp::protocol
