#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
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

/// The 4-byte little-endian magic every RemoteXPC wrapper starts with.
inline constexpr std::uint32_t kXpcWrapperMagic = 0x29B00B92;

/// The 4-byte little-endian magic every RemoteXPC payload starts with.
inline constexpr std::uint32_t kXpcPayloadMagic = 0x42133742;

/// The RemoteXPC payload's protocol version.
inline constexpr std::uint32_t kXpcPayloadVersion = 5;

/// The wrapper's fixed header: the magic, the flags, the 64-bit body length, the message id.
inline constexpr std::size_t kXpcWrapperHeaderSize = 4 + 4 + 8 + 8;

/// The wrapper's flags. `AlwaysSet` is present on every message.
inline constexpr std::uint32_t kXpcFlagAlwaysSet = 0x00000001;
inline constexpr std::uint32_t kXpcFlagPing = 0x00000002;
inline constexpr std::uint32_t kXpcFlagDataPresent = 0x00000100;
inline constexpr std::uint32_t kXpcFlagWantingReply = 0x00010000;
inline constexpr std::uint32_t kXpcFlagReply = 0x00020000;
inline constexpr std::uint32_t kXpcFlagFileTxStreamRequest = 0x00100000;
inline constexpr std::uint32_t kXpcFlagFileTxStreamResponse = 0x00200000;
inline constexpr std::uint32_t kXpcFlagInitHandshake = 0x00400000;

/// The type word that opens every `xpc` object.
enum class XpcType : std::uint32_t
{
    Null = 0x00001000,
    Boolean = 0x00002000,
    Int64 = 0x00003000,
    Uint64 = 0x00004000,
    Double = 0x00005000,
    Date = 0x00007000,
    Data = 0x00008000,
    String = 0x00009000,
    Uuid = 0x0000A000,
    Array = 0x0000E000,
    Dictionary = 0x0000F000
};

/// A date, as the nanoseconds since 1970-01-01T00:00:00Z, the `xpc` epoch.
struct XpcDate
{
    std::uint64_t nanoseconds = 0;

    friend bool operator==(const XpcDate &, const XpcDate &) = default;
};

/**
 * @brief An `xpc` object, the value kind RemoteXPC carries.
 *
 * RemoteXPC does not use property lists: a message's body is an `xpc` object, so it needs its
 * own codec. The value is a variant over the eleven kinds the CoreDevice services use, and the
 * accessors return an empty `optional` (or a null pointer) when the held kind does not match
 * rather than converting. Every field is little-endian, a string is NUL-terminated, and every string,
 * data blob, array, and dictionary is padded to a 4-byte boundary, so the codec mirrors the plist
 * codec's shape: a type word, then a length-prefixed value.
 *
 * `Int64` and `Uint64` are distinct kinds, because the RSD handshake carries signed and unsigned
 * values that must not be conflated. A dictionary keeps its keys sorted, so a serialized dictionary is
 * deterministic, which the device accepts because XPC object order is not significant.
 */
class IOSCPP_API Xpc
{
public:
    using Array = std::vector<Xpc>;
    using Dictionary = std::map<std::string, Xpc, std::less<>>;
    using Uuid = std::array<std::byte, 16>;

    /// A null value.
    Xpc() noexcept = default;
    Xpc(std::nullptr_t) noexcept
    {
    }

    Xpc(bool value);
    Xpc(int value);
    Xpc(std::int64_t value);
    Xpc(double value);
    Xpc(std::string value);
    Xpc(const char *value);

    /// A `Uint64` value, which is distinct from `Int64`.
    static Xpc uint64(std::uint64_t value);
    /// A `Date` value, in nanoseconds since 1970-01-01T00:00:00Z.
    static Xpc date(std::uint64_t nanoseconds);
    /// A `Date` value.
    static Xpc date(XpcDate value);
    /// A `Data` value, a byte blob.
    static Xpc data(std::vector<std::byte> value);
    /// A `Uuid` value.
    static Xpc uuid(Uuid value);
    static Xpc array(Array values);
    static Xpc dictionary(Dictionary values);

    /// The kind of value held.
    XpcType type() const noexcept;

    bool is_null() const noexcept;
    bool is_boolean() const noexcept;
    bool is_int64() const noexcept;
    bool is_uint64() const noexcept;
    bool is_double() const noexcept;
    bool is_date() const noexcept;
    bool is_data() const noexcept;
    bool is_string() const noexcept;
    bool is_uuid() const noexcept;
    bool is_array() const noexcept;
    bool is_dictionary() const noexcept;

    /// The boolean, or nothing when the value is not a boolean.
    std::optional<bool> boolean() const noexcept;
    /// The signed integer, or nothing when the value is not an `Int64`.
    std::optional<std::int64_t> int64() const noexcept;
    /// The unsigned integer, or nothing when the value is not a `Uint64`.
    std::optional<std::uint64_t> uint64() const noexcept;
    /// The real, or nothing when the value is not a `Double`.
    std::optional<double> real() const noexcept;
    /// The date, or nothing when the value is not a `Date`.
    std::optional<XpcDate> date() const noexcept;
    /// The data bytes, or nothing when the value is not `Data`.
    std::optional<std::span<const std::byte>> data() const noexcept;
    /// The string, or nothing when the value is not a string.
    std::optional<std::string_view> string() const noexcept;
    /// The UUID, or nothing when the value is not a `Uuid`.
    std::optional<Uuid> uuid() const noexcept;
    /// The array, or nullptr when the value is not an array.
    const Array *array() const noexcept;
    /// The dictionary, or nullptr when the value is not a dictionary.
    const Dictionary *dictionary() const noexcept;

    /// The string, or `fallback` when the value is not a string.
    std::string string_or(std::string_view fallback = {}) const;

    /// The value for `key` when this is a dictionary, or nullptr.
    const Xpc *find(std::string_view key) const noexcept;

    /// Encodes the object, without a wrapper, a payload magic, or a type word for itself.
    std::vector<std::byte> to_bytes() const;

    /// Decodes one object, which must consume `bytes` exactly.
    static Result<Xpc> parse(std::span<const std::byte> bytes);

private:
    using Value = std::variant<std::monostate, bool, std::int64_t, std::uint64_t, double, XpcDate,
                               std::vector<std::byte>, std::string, Uuid, Array, Dictionary>;

    Value value_;
};

/**
 * @brief The RemoteXPC payload: the payload magic, the protocol version, and the object.
 *
 * The payload sits inside the wrapper's body. Its length is the wrapper's body length, so an
 * `XpcPayload` consumes its whole span.
 */
struct IOSCPP_API XpcPayload
{
    Xpc object;

    /// Encodes the payload magic, the version, and the object.
    std::vector<std::byte> encode() const;

    /// Decodes the payload, which must consume `bytes` exactly.
    static Result<XpcPayload> parse(std::span<const std::byte> bytes);
};

/**
 * @brief The RemoteXPC wrapper around one message.
 *
 * The wrapper is the fixed header the message rides in: the magic, the flags, the 64-bit body
 * length, and the message id, then the body. The flags word must be set exactly, or the device
 * drops the connection, so @ref request builds a request's flags rather than leaving them to the caller.
 */
struct IOSCPP_API XpcWrapper
{
    /// The flags word, `AlwaysSet` on every message.
    std::uint32_t flags = kXpcFlagAlwaysSet;
    /// The message id, which a request and its reply share.
    std::uint64_t message_id = 0;
    /// The payload, present when the body length is non-zero.
    std::optional<XpcPayload> payload;

    /// Builds a request: `AlwaysSet`, `DataPresent`, and, when asked, `WantingReply`.
    static XpcWrapper request(std::uint64_t message_id, Xpc payload, bool wanting_reply = true);

    /// Encodes the header and the body, normalizing the `AlwaysSet` and `DataPresent` flags.
    std::vector<std::byte> encode() const;

    /// Decodes the wrapper, which must consume `bytes` exactly.
    static Result<XpcWrapper> parse(std::span<const std::byte> bytes);
};

} // namespace ioscpp::protocol
