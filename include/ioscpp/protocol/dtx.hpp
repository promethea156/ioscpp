#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "ioscpp/error.hpp"
#include "ioscpp/export.hpp"

namespace ioscpp::protocol
{

/// The 4-byte little-endian magic every DTX message starts with.
inline constexpr std::uint32_t kDtxMagic = 0x1F3D5B79;

/// The DTX message header's fixed length.
inline constexpr std::size_t kDtxHeaderSize = 32;

/// The DTX payload header's fixed length.
inline constexpr std::size_t kDtxPayloadHeaderSize = 16;

/// The DTX auxiliary dictionary header's fixed length.
inline constexpr std::size_t kDtxAuxiliaryHeaderSize = 16;

/// The type code of a DTX auxiliary dictionary, in the magic's low byte.
inline constexpr std::uint32_t kDtxAuxiliaryType = 0x000000F0;

/// The type word that opens every DTX auxiliary dictionary, with the `0x100` flag set.
inline constexpr std::uint32_t kDtxAuxiliaryMagic = 0x000001F0;

/// The largest assembled message, so a hostile length cannot ask for a huge allocation.
inline constexpr std::uint32_t kDtxMaxMessageSize = 128u * 1024u * 1024u;

/// The transport flag in the message header. Bit 0 is `ExpectsReply`.
inline constexpr std::uint32_t kDtxFlagExpectsReply = 0x1;

/// The message type in the payload header.
enum class DtxMessageType : std::uint32_t
{
    Ok = 0,
    Data = 1,
    Dispatch = 2,
    Object = 3,
    Error = 4,
    Barrier = 5,
    Primitive = 6,
    Compressed = 7,
    ProxiedMessage = 8
};

/// The type word that opens every DTX auxiliary value.
enum class DtxValueType : std::uint32_t
{
    Null = 0x0A,
    String = 0x01,
    Buffer = 0x02,
    Int32 = 0x03,
    Int64 = 0x06,
    Double = 0x09
};

/**
 * @brief A DTX auxiliary value, the argument kind a method call carries.
 *
 * The DTX auxiliary dictionary is a list of primitives, and a method call's arguments
 * ride in it: a string or buffer for an argument, an integer for a channel code, and a
 * null for a slot with nothing. A buffer holds either raw bytes or an
 * `NSKeyedArchive`-encoded object, which the caller encodes and decodes itself.
 *
 * A string and a buffer are length-prefixed and are **not** padded, unlike an `xpc`
 * object. A nested dictionary is not modelled yet, because the services here do not use one.
 */
class IOSCPP_API DtxValue
{
public:
    using Buffer = std::vector<std::byte>;

    /// A null value.
    DtxValue() noexcept = default;
    DtxValue(std::nullptr_t) noexcept
    {
    }

    DtxValue(int value);
    DtxValue(std::int64_t value);
    DtxValue(double value);
    DtxValue(std::string value);
    DtxValue(const char *value);

    /// A buffer value, either raw bytes or an `NSKeyedArchive`-encoded object.
    static DtxValue buffer(Buffer value);

    /// The kind of value held.
    DtxValueType type() const noexcept;

    bool is_null() const noexcept;
    bool is_int32() const noexcept;
    bool is_int64() const noexcept;
    bool is_double() const noexcept;
    bool is_string() const noexcept;
    bool is_buffer() const noexcept;

    /// The 32-bit integer, or nothing when the value is not an `Int32`.
    std::optional<std::int32_t> int32() const noexcept;
    /// The 64-bit integer, or nothing when the value is not an `Int64`.
    std::optional<std::int64_t> int64() const noexcept;
    /// The real, or nothing when the value is not a `Double`.
    std::optional<double> real() const noexcept;
    /// The string, or nothing when the value is not a string.
    std::optional<std::string_view> string() const noexcept;
    /// The bytes, or nothing when the value is not a buffer.
    std::optional<std::span<const std::byte>> buffer() const noexcept;

    /// The string, or `fallback` when the value is not a string.
    std::string string_or(std::string_view fallback = {}) const;

    /// The value converted to an unsigned integer; a negative integer wraps to
    /// its two's-complement bit pattern.
    std::optional<std::uint64_t> as_uint64() const noexcept;

private:
    using Value = std::variant<std::monostate, std::int32_t, std::int64_t, double, std::string, Buffer>;

    Value value_;
};

/**
 * @brief One DTX message, the format a `com.apple.dvt.*` service speaks.
 *
 * A `dvt` service does not exchange plists: it exchanges DTX messages, which have a
 * fixed 32-byte header, a 16-byte payload header, an optional auxiliary dictionary,
 * and then the payload bytes (`docs/04-blockers.md`). The header carries the identifier
 * a reply shares, the conversation index (0 for a request, incremented for each reply),
 * the channel code, and the `ExpectsReply` flag, and the payload header carries the
 * message type and the two lengths.
 *
 * `encode` writes the whole frame, so the two lengths and the header size are derived
 * rather than left to the caller, and `parse` reads one whole message. Fragmentation is
 * not modelled yet, because the services here exchange small messages; a fragment is left
 * to a later increment.
 */
class IOSCPP_API Dtx
{
public:
    using Auxiliary = std::vector<DtxValue>;
    using Payload = std::vector<std::byte>;

    /// The 0-based index of this fragment in a fragmented message.
    std::uint16_t fragment_index = 0;
    /// The number of fragments in a fragmented message.
    std::uint16_t fragment_count = 1;
    /// The identifier a request and its reply share.
    std::uint32_t identifier = 0;
    /// The message's position in its conversation: 0 for a request, and
    /// incremented for each reply.
    std::uint32_t conversation_index = 0;
    /// The channel the message rides on. Channel 0 is the global channel.
    std::int32_t channel_code = 0;
    /// Whether the device must answer this message.
    bool expects_reply = false;
    /// The message type.
    DtxMessageType message_type = DtxMessageType::Ok;
    /// The auxiliary arguments, which are a list of primitives.
    Auxiliary auxiliary;
    /// The payload bytes, usually an `NSKeyedArchive`-encoded object.
    Payload payload;

    /// Builds the acknowledgement a message that expects a reply must receive.
    static Dtx ack(const Dtx &message);

    /// Encodes the whole frame, deriving the lengths and the header size.
    std::vector<std::byte> encode() const;

    /// Decodes one message, which must consume `bytes` exactly.
    static Result<Dtx> parse(std::span<const std::byte> bytes);

    /// Whether the message carries an error payload.
    bool is_error() const noexcept;
};

} // namespace ioscpp::protocol
