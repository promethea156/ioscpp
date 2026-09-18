#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "ioscpp/error.hpp"
#include "ioscpp/export.hpp"

namespace ioscpp::protocol
{

/// Size in bytes of a mux frame header.
///
/// The v1 header is the first two fields alone; the magic and the two sequence
/// numbers exist only on a device that negotiated mux version 2. The full size is
/// the constant below, and @ref MuxHeader::wire_size returns the size for a version.
inline constexpr std::size_t kMuxHeaderSize = 16;

/// The two fields a v1 header carries.
inline constexpr std::size_t kMuxHeaderSizeV1 = 8;

/// The magic every version-2 mux header carries.
inline constexpr std::uint32_t kMuxMagic = 0xfeedface;

/// The `lockdownd` port on a device.
inline constexpr std::uint16_t kLockdownPort = 62078;

/// The `usbmuxd` socket port on a host, for a `TcpTransport`.
inline constexpr std::uint16_t kUsbmuxdPort = 27015;

/// The mux protocol id carried in a frame header.
enum class MuxProtocol : std::uint32_t
{
    /// The version handshake. Carries a @ref VersionHeader.
    Version = 0,
    /// A control message, for example a device error.
    Control = 1,
    /// The version-2 setup packet, a one-byte `\x07` payload.
    Setup = 2,
    /// A TCP-like connection frame, carrying a @ref TcpHeader.
    Tcp = 6
};

/**
 * @brief A mux frame header.
 *
 * This mirrors `usbmuxd`'s `struct mux_header`: four 32-bit and two 16-bit words,
 * sent big-endian:
 *
 *   https://github.com/libimobiledevice/usbmuxd/blob/master/src/device.c
 *
 * `length` covers the whole frame, header included, so a payload's size is
 * `length - wire_size(version)`. `magic` and the two sequence numbers are present
 * only on a device that negotiated version 2.
 *
 * The sequence numbers are the mux layer's own, not TCP's: the sender increments
 * `tx_seq` for every frame, and echoes the peer's last `tx_seq` back as `rx_seq`.
 */
struct IOSCPP_API MuxHeader
{
    std::uint32_t protocol = 0;
    /// The whole frame size, header included.
    std::uint32_t length = 0;
    std::uint32_t magic = kMuxMagic;
    std::uint16_t tx_seq = 0;
    std::uint16_t rx_seq = 0;

    /// The header size for a negotiated version: 8 for v1, 16 for v2.
    static std::size_t wire_size(std::uint32_t version) noexcept;

    /// Serializes the header for `version`, which decides whether the v2 fields are sent.
    std::array<std::byte, kMuxHeaderSize> encode(std::uint32_t version) const noexcept;

    /// Parses a header for `version`.
    static MuxHeader decode(std::span<const std::byte> bytes, std::uint32_t version) noexcept;
};

/// The version handshake payload: `major`, `minor`, and a padding word.
struct IOSCPP_API VersionHeader
{
    std::uint32_t major = 2;
    std::uint32_t minor = 0;
    std::uint32_t padding = 0;

    static constexpr std::size_t kSize = 12;

    std::array<std::byte, kSize> encode() const noexcept;
    static VersionHeader decode(std::span<const std::byte, kSize> bytes) noexcept;
};

/// A TCP flag in a @ref TcpHeader.
enum TcpFlag : std::uint8_t
{
    TcpFin = 0x01,
    TcpSyn = 0x02,
    TcpRst = 0x04,
    TcpPsh = 0x08,
    TcpAck = 0x10,
    TcpUrg = 0x20
};

/// Size in bytes of a TCP header.
inline constexpr std::size_t kTcpHeaderSize = 20;

/**
 * @brief A TCP-like header inside a @ref MuxProtocol::Tcp frame.
 *
 * The fields are the standard BSD ones, big-endian, and `data_offset` is in 32-bit
 * words and lives in the high nibble of its byte. `usbmuxd` builds these with the
 * host's `struct tcphdr`, so the layout is the wire layout:
 *
 *   https://github.com/libimobiledevice/usbmuxd/blob/master/src/device.c
 */
struct IOSCPP_API TcpHeader
{
    std::uint16_t source_port = 0;
    std::uint16_t destination_port = 0;
    std::uint32_t sequence = 0;
    std::uint32_t acknowledgement = 0;
    std::uint8_t data_offset = 5;
    std::uint8_t flags = 0;
    std::uint16_t window = 0;
    std::uint16_t checksum = 0;
    std::uint16_t urgent = 0;

    std::array<std::byte, kTcpHeaderSize> encode() const noexcept;
    static TcpHeader decode(std::span<const std::byte, kTcpHeaderSize> bytes) noexcept;
};

} // namespace ioscpp::protocol
