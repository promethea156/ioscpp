#include "ioscpp/protocol/remotexpc.hpp"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
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

/// Appends `value` as a 32-bit little-endian word.
void append_u32(std::vector<std::byte> &out, std::uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i)
    {
        out.push_back(static_cast<std::byte>((value >> (8 * i)) & 0xff));
    }
}

/// Appends `value` as a 64-bit little-endian word.
void append_u64(std::vector<std::byte> &out, std::uint64_t value)
{
    for (unsigned i = 0; i < 8; ++i)
    {
        out.push_back(static_cast<std::byte>((value >> (8 * i)) & 0xff));
    }
}

/// Pads `out` to the next 4-byte boundary, which every string, data blob, and
/// dictionary key ends on.
void pad(std::vector<std::byte> &out)
{
    while (out.size() % 4 != 0)
    {
        out.push_back(std::byte{0});
    }
}

void encode_object(const Xpc &value, std::vector<std::byte> &out);

/// Appends a dictionary key: a NUL-terminated string, aligned, with no type or length.
void encode_key(std::string_view key, std::vector<std::byte> &out)
{
    for (const char c : key)
    {
        out.push_back(static_cast<std::byte>(c));
    }
    out.push_back(std::byte{0});
    pad(out);
}

void encode_object(const Xpc &value, std::vector<std::byte> &out)
{
    append_u32(out, static_cast<std::uint32_t>(value.type()));
    switch (value.type())
    {
        case XpcType::Null:
            break;
        case XpcType::Boolean:
            append_u32(out, value.boolean().value_or(false) ? 1u : 0u);
            break;
        case XpcType::Int64:
            append_u64(out, static_cast<std::uint64_t>(value.int64().value_or(0)));
            break;
        case XpcType::Uint64:
            append_u64(out, value.uint64().value_or(0));
            break;
        case XpcType::Double:
            append_u64(out, std::bit_cast<std::uint64_t>(value.real().value_or(0)));
            break;
        case XpcType::Date:
            append_u64(out, value.date().value_or(XpcDate{}).nanoseconds);
            break;
        case XpcType::Data:
        {
            const std::span<const std::byte> bytes = value.data().value_or(std::span<const std::byte>{});
            append_u32(out, static_cast<std::uint32_t>(bytes.size()));
            out.insert(out.end(), bytes.begin(), bytes.end());
            pad(out);
            break;
        }
        case XpcType::String:
        {
            const std::string_view text = value.string().value_or(std::string_view{});
            append_u32(out, static_cast<std::uint32_t>(text.size() + 1));
            for (const char c : text)
            {
                out.push_back(static_cast<std::byte>(c));
            }
            out.push_back(std::byte{0});
            pad(out);
            break;
        }
        case XpcType::Uuid:
        {
            const Xpc::Uuid uuid = value.uuid().value_or(Xpc::Uuid{});
            out.insert(out.end(), uuid.begin(), uuid.end());
            break;
        }
        case XpcType::Array:
        {
            std::vector<std::byte> body;
            const Xpc::Array &array = *value.array();
            append_u32(body, static_cast<std::uint32_t>(array.size()));
            for (const Xpc &element : array)
            {
                encode_object(element, body);
            }
            append_u32(out, static_cast<std::uint32_t>(body.size()));
            out.insert(out.end(), body.begin(), body.end());
            break;
        }
        case XpcType::Dictionary:
        {
            std::vector<std::byte> body;
            const Xpc::Dictionary &dictionary = *value.dictionary();
            append_u32(body, static_cast<std::uint32_t>(dictionary.size()));
            for (const auto &[key, member] : dictionary)
            {
                encode_key(key, body);
                encode_object(member, body);
            }
            append_u32(out, static_cast<std::uint32_t>(body.size()));
            out.insert(out.end(), body.begin(), body.end());
            break;
        }
    }
}

/// A bounds-checked reader over an `xpc` object stream.
struct Reader
{
    std::span<const std::byte> bytes;
    std::size_t offset = 0;

    bool has(std::size_t count) const noexcept
    {
        return count <= bytes.size() - offset;
    }

    Result<std::uint32_t> u32()
    {
        if (!has(4))
        {
            return tl::unexpected(protocol_error("an xpc object is truncated"));
        }
        std::uint32_t value = 0;
        for (unsigned i = 0; i < 4; ++i)
        {
            value |= static_cast<std::uint32_t>(std::to_integer<unsigned char>(bytes[offset + i])) << (8 * i);
        }
        offset += 4;
        return value;
    }

    Result<std::uint64_t> u64()
    {
        if (!has(8))
        {
            return tl::unexpected(protocol_error("an xpc object is truncated"));
        }
        std::uint64_t value = 0;
        for (unsigned i = 0; i < 8; ++i)
        {
            value |= static_cast<std::uint64_t>(std::to_integer<unsigned char>(bytes[offset + i])) << (8 * i);
        }
        offset += 8;
        return value;
    }

    Result<std::span<const std::byte>> take(std::size_t count)
    {
        if (!has(count))
        {
            return tl::unexpected(protocol_error("an xpc object is truncated"));
        }
        const std::span<const std::byte> view = bytes.subspan(offset, count);
        offset += count;
        return view;
    }

    /// Skips to the next 4-byte boundary, which a string, data blob, or
    /// dictionary key ends on. An array or dictionary is already aligned.
    Status skip_padding()
    {
        const std::size_t remainder = offset % 4;
        if (remainder == 0)
        {
            return {};
        }
        if (!has(4 - remainder))
        {
            return tl::unexpected(protocol_error("an xpc object is truncated"));
        }
        offset += 4 - remainder;
        return {};
    }
};

Result<std::string> decode_key(Reader &reader)
{
    std::string key;
    while (true)
    {
        auto byte = reader.take(1);
        if (!byte)
        {
            return tl::unexpected(byte.error());
        }
        if ((*byte)[0] == std::byte{0})
        {
            break;
        }
        key.push_back(static_cast<char>((*byte)[0]));
    }
    if (auto status = reader.skip_padding(); !status)
    {
        return tl::unexpected(status.error());
    }
    return key;
}

Result<Xpc> decode_object(Reader &reader)
{
    auto type_word = reader.u32();
    if (!type_word)
    {
        return tl::unexpected(type_word.error());
    }
    switch (static_cast<XpcType>(*type_word))
    {
        case XpcType::Null:
            return Xpc();
        case XpcType::Boolean:
        {
            auto value = reader.u32();
            if (!value)
            {
                return tl::unexpected(value.error());
            }
            return Xpc(*value != 0);
        }
        case XpcType::Int64:
        {
            auto value = reader.u64();
            if (!value)
            {
                return tl::unexpected(value.error());
            }
            return Xpc(static_cast<std::int64_t>(*value));
        }
        case XpcType::Uint64:
        {
            auto value = reader.u64();
            if (!value)
            {
                return tl::unexpected(value.error());
            }
            return Xpc::uint64(*value);
        }
        case XpcType::Double:
        {
            auto value = reader.u64();
            if (!value)
            {
                return tl::unexpected(value.error());
            }
            return Xpc(std::bit_cast<double>(*value));
        }
        case XpcType::Date:
        {
            auto value = reader.u64();
            if (!value)
            {
                return tl::unexpected(value.error());
            }
            return Xpc::date(*value);
        }
        case XpcType::Data:
        {
            auto length = reader.u32();
            if (!length)
            {
                return tl::unexpected(length.error());
            }
            auto payload = reader.take(*length);
            if (!payload)
            {
                return tl::unexpected(payload.error());
            }
            std::vector<std::byte> data(payload->begin(), payload->end());
            if (auto status = reader.skip_padding(); !status)
            {
                return tl::unexpected(status.error());
            }
            return Xpc::data(std::move(data));
        }
        case XpcType::String:
        {
            auto length = reader.u32();
            if (!length)
            {
                return tl::unexpected(length.error());
            }
            auto payload = reader.take(*length);
            if (!payload)
            {
                return tl::unexpected(payload.error());
            }
            std::string text(reinterpret_cast<const char *>(payload->data()), payload->size());
            // The length includes the NUL terminator; drop it and any defensive
            // extra NULs.
            while (!text.empty() && text.back() == '\0')
            {
                text.pop_back();
            }
            if (auto status = reader.skip_padding(); !status)
            {
                return tl::unexpected(status.error());
            }
            return Xpc(std::move(text));
        }
        case XpcType::Uuid:
        {
            auto payload = reader.take(16);
            if (!payload)
            {
                return tl::unexpected(payload.error());
            }
            Xpc::Uuid uuid{};
            std::copy(payload->begin(), payload->end(), uuid.begin());
            return Xpc::uuid(uuid);
        }
        case XpcType::Array:
        {
            auto length = reader.u32();
            if (!length)
            {
                return tl::unexpected(length.error());
            }
            const std::size_t start = reader.offset;
            auto count = reader.u32();
            if (!count)
            {
                return tl::unexpected(count.error());
            }
            Xpc::Array array;
            array.reserve(*count);
            for (std::uint32_t i = 0; i < *count; ++i)
            {
                auto element = decode_object(reader);
                if (!element)
                {
                    return tl::unexpected(element.error());
                }
                array.push_back(std::move(*element));
            }
            if (reader.offset - start != *length)
            {
                return tl::unexpected(protocol_error("an xpc array length does not match its entries"));
            }
            return Xpc::array(std::move(array));
        }
        case XpcType::Dictionary:
        {
            auto length = reader.u32();
            if (!length)
            {
                return tl::unexpected(length.error());
            }
            const std::size_t start = reader.offset;
            auto count = reader.u32();
            if (!count)
            {
                return tl::unexpected(count.error());
            }
            Xpc::Dictionary dictionary;
            for (std::uint32_t i = 0; i < *count; ++i)
            {
                auto key = decode_key(reader);
                if (!key)
                {
                    return tl::unexpected(key.error());
                }
                auto value = decode_object(reader);
                if (!value)
                {
                    return tl::unexpected(value.error());
                }
                dictionary.emplace(std::move(*key), std::move(*value));
            }
            if (reader.offset - start != *length)
            {
                return tl::unexpected(protocol_error("an xpc dictionary length does not match its entries"));
            }
            return Xpc::dictionary(std::move(dictionary));
        }
    }
    return tl::unexpected(protocol_error("an xpc object has an unknown type"));
}

} // namespace

Xpc::Xpc(bool value)
    : value_(value)
{
}

Xpc::Xpc(int value)
    : value_(static_cast<std::int64_t>(value))
{
}

Xpc::Xpc(std::int64_t value)
    : value_(value)
{
}

Xpc::Xpc(double value)
    : value_(value)
{
}

Xpc::Xpc(std::string value)
    : value_(std::move(value))
{
}

Xpc::Xpc(const char *value)
    : value_(std::string(value))
{
}

Xpc Xpc::uint64(std::uint64_t value)
{
    Xpc xpc;
    xpc.value_ = value;
    return xpc;
}

Xpc Xpc::date(std::uint64_t nanoseconds)
{
    return date(XpcDate{nanoseconds});
}

Xpc Xpc::date(XpcDate value)
{
    Xpc xpc;
    xpc.value_ = value;
    return xpc;
}

Xpc Xpc::data(std::vector<std::byte> value)
{
    Xpc xpc;
    xpc.value_ = std::move(value);
    return xpc;
}

Xpc Xpc::uuid(Uuid value)
{
    Xpc xpc;
    xpc.value_ = value;
    return xpc;
}

Xpc Xpc::array(Array values)
{
    Xpc xpc;
    xpc.value_ = std::move(values);
    return xpc;
}

Xpc Xpc::dictionary(Dictionary values)
{
    Xpc xpc;
    xpc.value_ = std::move(values);
    return xpc;
}

XpcType Xpc::type() const noexcept
{
    switch (value_.index())
    {
        case 0:
            return XpcType::Null;
        case 1:
            return XpcType::Boolean;
        case 2:
            return XpcType::Int64;
        case 3:
            return XpcType::Uint64;
        case 4:
            return XpcType::Double;
        case 5:
            return XpcType::Date;
        case 6:
            return XpcType::Data;
        case 7:
            return XpcType::String;
        case 8:
            return XpcType::Uuid;
        case 9:
            return XpcType::Array;
        default:
            return XpcType::Dictionary;
    }
}

bool Xpc::is_null() const noexcept
{
    return value_.index() == 0;
}

bool Xpc::is_boolean() const noexcept
{
    return value_.index() == 1;
}

bool Xpc::is_int64() const noexcept
{
    return value_.index() == 2;
}

bool Xpc::is_uint64() const noexcept
{
    return value_.index() == 3;
}

bool Xpc::is_double() const noexcept
{
    return value_.index() == 4;
}

bool Xpc::is_date() const noexcept
{
    return value_.index() == 5;
}

bool Xpc::is_data() const noexcept
{
    return value_.index() == 6;
}

bool Xpc::is_string() const noexcept
{
    return value_.index() == 7;
}

bool Xpc::is_uuid() const noexcept
{
    return value_.index() == 8;
}

bool Xpc::is_array() const noexcept
{
    return value_.index() == 9;
}

bool Xpc::is_dictionary() const noexcept
{
    return value_.index() == 10;
}

std::optional<bool> Xpc::boolean() const noexcept
{
    if (const bool *value = std::get_if<bool>(&value_))
    {
        return *value;
    }
    return std::nullopt;
}

std::optional<std::int64_t> Xpc::int64() const noexcept
{
    if (const std::int64_t *value = std::get_if<std::int64_t>(&value_))
    {
        return *value;
    }
    return std::nullopt;
}

std::optional<std::uint64_t> Xpc::uint64() const noexcept
{
    if (const std::uint64_t *value = std::get_if<std::uint64_t>(&value_))
    {
        return *value;
    }
    return std::nullopt;
}

std::optional<double> Xpc::real() const noexcept
{
    if (const double *value = std::get_if<double>(&value_))
    {
        return *value;
    }
    return std::nullopt;
}

std::optional<XpcDate> Xpc::date() const noexcept
{
    if (const XpcDate *value = std::get_if<XpcDate>(&value_))
    {
        return *value;
    }
    return std::nullopt;
}

std::optional<std::span<const std::byte>> Xpc::data() const noexcept
{
    if (const std::vector<std::byte> *value = std::get_if<std::vector<std::byte>>(&value_))
    {
        return std::span<const std::byte>(*value);
    }
    return std::nullopt;
}

std::optional<std::string_view> Xpc::string() const noexcept
{
    if (const std::string *value = std::get_if<std::string>(&value_))
    {
        return *value;
    }
    return std::nullopt;
}

std::optional<Xpc::Uuid> Xpc::uuid() const noexcept
{
    if (const Uuid *value = std::get_if<Uuid>(&value_))
    {
        return *value;
    }
    return std::nullopt;
}

const Xpc::Array *Xpc::array() const noexcept
{
    return std::get_if<Array>(&value_);
}

const Xpc::Dictionary *Xpc::dictionary() const noexcept
{
    return std::get_if<Dictionary>(&value_);
}

std::string Xpc::string_or(std::string_view fallback) const
{
    const std::optional<std::string_view> value = string();
    return value.has_value() ? std::string(*value) : std::string(fallback);
}

const Xpc *Xpc::find(std::string_view key) const noexcept
{
    const Dictionary *dictionary = this->dictionary();
    if (dictionary == nullptr)
    {
        return nullptr;
    }
    const auto found = dictionary->find(key);
    return found == dictionary->end() ? nullptr : &found->second;
}

std::vector<std::byte> Xpc::to_bytes() const
{
    std::vector<std::byte> out;
    encode_object(*this, out);
    return out;
}

Result<Xpc> Xpc::parse(std::span<const std::byte> bytes)
{
    Reader reader{bytes, 0};
    auto value = decode_object(reader);
    if (!value)
    {
        return tl::unexpected(value.error());
    }
    if (reader.offset != bytes.size())
    {
        return tl::unexpected(protocol_error("an xpc object has trailing bytes"));
    }
    return value;
}

std::vector<std::byte> XpcPayload::encode() const
{
    std::vector<std::byte> out;
    append_u32(out, kXpcPayloadMagic);
    append_u32(out, kXpcPayloadVersion);
    const std::vector<std::byte> body = object.to_bytes();
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

Result<XpcPayload> XpcPayload::parse(std::span<const std::byte> bytes)
{
    Reader reader{bytes, 0};
    auto magic = reader.u32();
    if (!magic)
    {
        return tl::unexpected(magic.error());
    }
    if (*magic != kXpcPayloadMagic)
    {
        return tl::unexpected(protocol_error("the xpc payload has a bad magic"));
    }
    auto version = reader.u32();
    if (!version)
    {
        return tl::unexpected(version.error());
    }
    if (*version != kXpcPayloadVersion)
    {
        return tl::unexpected(protocol_error("the xpc payload has an unknown version"));
    }
    auto object = decode_object(reader);
    if (!object)
    {
        return tl::unexpected(object.error());
    }
    if (reader.offset != bytes.size())
    {
        return tl::unexpected(protocol_error("the xpc payload has trailing bytes"));
    }
    XpcPayload payload;
    payload.object = std::move(*object);
    return payload;
}

XpcWrapper XpcWrapper::request(std::uint64_t message_id, Xpc payload, bool wanting_reply)
{
    XpcWrapper wrapper;
    wrapper.flags = kXpcFlagAlwaysSet | kXpcFlagDataPresent;
    if (wanting_reply)
    {
        wrapper.flags |= kXpcFlagWantingReply;
    }
    wrapper.message_id = message_id;
    wrapper.payload = XpcPayload{std::move(payload)};
    return wrapper;
}

std::vector<std::byte> XpcWrapper::encode() const
{
    std::vector<std::byte> body;
    if (payload.has_value())
    {
        body = payload->encode();
    }

    std::vector<std::byte> out;
    append_u32(out, kXpcWrapperMagic);
    append_u32(out, flags);
    append_u64(out, static_cast<std::uint64_t>(body.size()));
    append_u64(out, message_id);
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

Result<XpcWrapper> XpcWrapper::parse(std::span<const std::byte> bytes)
{
    Reader reader{bytes, 0};
    auto magic = reader.u32();
    if (!magic)
    {
        return tl::unexpected(magic.error());
    }
    if (*magic != kXpcWrapperMagic)
    {
        return tl::unexpected(protocol_error("the xpc wrapper has a bad magic"));
    }
    auto flags = reader.u32();
    if (!flags)
    {
        return tl::unexpected(flags.error());
    }
    auto body_length = reader.u64();
    if (!body_length)
    {
        return tl::unexpected(body_length.error());
    }
    auto message_id = reader.u64();
    if (!message_id)
    {
        return tl::unexpected(message_id.error());
    }

    XpcWrapper wrapper;
    wrapper.flags = *flags;
    wrapper.message_id = *message_id;
    if (*body_length == 0)
    {
        if (reader.offset != bytes.size())
        {
            return tl::unexpected(protocol_error("the xpc wrapper has trailing bytes"));
        }
        return wrapper;
    }

    auto body = reader.take(*body_length);
    if (!body)
    {
        return tl::unexpected(body.error());
    }
    if (reader.offset != bytes.size())
    {
        return tl::unexpected(protocol_error("the xpc wrapper has trailing bytes"));
    }
    auto payload = XpcPayload::parse(*body);
    if (!payload)
    {
        return tl::unexpected(payload.error());
    }
    wrapper.payload = std::move(*payload);
    return wrapper;
}

} // namespace ioscpp::protocol
