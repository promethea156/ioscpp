#include "ioscpp/protocol/dtx.hpp"

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

/// Appends `value` as a 16-bit little-endian word.
void append_u16(std::vector<std::byte> &out, std::uint16_t value)
{
    for (unsigned i = 0; i < 2; ++i)
    {
        out.push_back(static_cast<std::byte>((value >> (8 * i)) & 0xff));
    }
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

void encode_value(const DtxValue &value, std::vector<std::byte> &out)
{
    append_u32(out, static_cast<std::uint32_t>(value.type()));
    switch (value.type())
    {
        case DtxValueType::Null:
            break;
        case DtxValueType::Int32:
            append_u32(out, static_cast<std::uint32_t>(value.int32().value_or(0)));
            break;
        case DtxValueType::Int64:
            append_u64(out, static_cast<std::uint64_t>(value.int64().value_or(0)));
            break;
        case DtxValueType::Double:
            append_u64(out, std::bit_cast<std::uint64_t>(value.real().value_or(0)));
            break;
        case DtxValueType::String:
        {
            const std::string_view text = value.string().value_or(std::string_view{});
            append_u32(out, static_cast<std::uint32_t>(text.size()));
            for (const char c : text)
            {
                out.push_back(static_cast<std::byte>(c));
            }
            break;
        }
        case DtxValueType::Buffer:
        {
            const std::span<const std::byte> bytes = value.buffer().value_or(std::span<const std::byte>{});
            append_u32(out, static_cast<std::uint32_t>(bytes.size()));
            out.insert(out.end(), bytes.begin(), bytes.end());
            break;
        }
    }
}

/// A bounds-checked reader over a DTX message.
struct Reader
{
    std::span<const std::byte> bytes;
    std::size_t offset = 0;

    bool has(std::size_t count) const noexcept
    {
        return count <= bytes.size() - offset;
    }

    Result<std::uint8_t> u8()
    {
        if (!has(1))
        {
            return tl::unexpected(protocol_error("a DTX message is truncated"));
        }
        const std::uint8_t value = std::to_integer<std::uint8_t>(bytes[offset]);
        offset += 1;
        return value;
    }

    Result<std::uint16_t> u16()
    {
        if (!has(2))
        {
            return tl::unexpected(protocol_error("a DTX message is truncated"));
        }
        std::uint16_t value = 0;
        for (unsigned i = 0; i < 2; ++i)
        {
            value |= static_cast<std::uint16_t>(std::to_integer<unsigned char>(bytes[offset + i])) << (8 * i);
        }
        offset += 2;
        return value;
    }

    Result<std::uint32_t> u32()
    {
        if (!has(4))
        {
            return tl::unexpected(protocol_error("a DTX message is truncated"));
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
            return tl::unexpected(protocol_error("a DTX message is truncated"));
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
            return tl::unexpected(protocol_error("a DTX message is truncated"));
        }
        const std::span<const std::byte> view = bytes.subspan(offset, count);
        offset += count;
        return view;
    }
};

Result<DtxValue> decode_value(Reader &reader)
{
    auto type = reader.u32();
    if (!type)
    {
        return tl::unexpected(type.error());
    }
    switch (static_cast<DtxValueType>(*type))
    {
        case DtxValueType::Null:
            return DtxValue();
        case DtxValueType::Int32:
        {
            auto value = reader.u32();
            if (!value)
            {
                return tl::unexpected(value.error());
            }
            return DtxValue(static_cast<std::int32_t>(*value));
        }
        case DtxValueType::Int64:
        {
            auto value = reader.u64();
            if (!value)
            {
                return tl::unexpected(value.error());
            }
            return DtxValue(static_cast<std::int64_t>(*value));
        }
        case DtxValueType::Double:
        {
            auto value = reader.u64();
            if (!value)
            {
                return tl::unexpected(value.error());
            }
            return DtxValue(std::bit_cast<double>(*value));
        }
        case DtxValueType::String:
        {
            auto length = reader.u32();
            if (!length)
            {
                return tl::unexpected(length.error());
            }
            auto text = reader.take(*length);
            if (!text)
            {
                return tl::unexpected(text.error());
            }
            return DtxValue(std::string(reinterpret_cast<const char *>(text->data()), text->size()));
        }
        case DtxValueType::Buffer:
        {
            auto length = reader.u32();
            if (!length)
            {
                return tl::unexpected(length.error());
            }
            auto bytes = reader.take(*length);
            if (!bytes)
            {
                return tl::unexpected(bytes.error());
            }
            return DtxValue::buffer(std::vector<std::byte>(bytes->begin(), bytes->end()));
        }
    }
    return tl::unexpected(protocol_error("a DTX auxiliary value has an unknown type"));
}

/// Decodes the auxiliary dictionary. Each entry is a key and a value, and the key
/// is discarded without checking it; the keys the services here send are null.
Result<Dtx::Auxiliary> decode_auxiliary(Reader &reader)
{
    auto magic = reader.u32();
    if (!magic)
    {
        return tl::unexpected(magic.error());
    }
    if ((*magic & 0xffu) != kDtxAuxiliaryType)
    {
        return tl::unexpected(protocol_error("the DTX auxiliary dictionary has a bad magic"));
    }
    // A reserved 32-bit field; `encode` writes 0 and the value is unused.
    if (auto skipped = reader.take(4); !skipped)
    {
        return tl::unexpected(skipped.error());
    }
    auto body_length = reader.u64();
    if (!body_length)
    {
        return tl::unexpected(body_length.error());
    }
    auto body = reader.take(static_cast<std::size_t>(*body_length));
    if (!body)
    {
        return tl::unexpected(body.error());
    }

    Reader entries{*body, 0};
    Dtx::Auxiliary auxiliary;
    while (entries.offset < entries.bytes.size())
    {
        auto key = decode_value(entries);
        if (!key)
        {
            return tl::unexpected(key.error());
        }
        auto value = decode_value(entries);
        if (!value)
        {
            return tl::unexpected(value.error());
        }
        auxiliary.push_back(std::move(*value));
    }
    return auxiliary;
}

} // namespace

DtxValue::DtxValue(int value)
    : value_(static_cast<std::int32_t>(value))
{
}

DtxValue::DtxValue(std::int64_t value)
    : value_(value)
{
}

DtxValue::DtxValue(double value)
    : value_(value)
{
}

DtxValue::DtxValue(std::string value)
    : value_(std::move(value))
{
}

DtxValue::DtxValue(const char *value)
    : value_(std::string(value))
{
}

DtxValue DtxValue::buffer(Buffer value)
{
    DtxValue dtx_value;
    dtx_value.value_ = std::move(value);
    return dtx_value;
}

DtxValueType DtxValue::type() const noexcept
{
    switch (value_.index())
    {
        case 0:
            return DtxValueType::Null;
        case 1:
            return DtxValueType::Int32;
        case 2:
            return DtxValueType::Int64;
        case 3:
            return DtxValueType::Double;
        case 4:
            return DtxValueType::String;
        default:
            return DtxValueType::Buffer;
    }
}

bool DtxValue::is_null() const noexcept
{
    return value_.index() == 0;
}

bool DtxValue::is_int32() const noexcept
{
    return value_.index() == 1;
}

bool DtxValue::is_int64() const noexcept
{
    return value_.index() == 2;
}

bool DtxValue::is_double() const noexcept
{
    return value_.index() == 3;
}

bool DtxValue::is_string() const noexcept
{
    return value_.index() == 4;
}

bool DtxValue::is_buffer() const noexcept
{
    return value_.index() == 5;
}

std::optional<std::int32_t> DtxValue::int32() const noexcept
{
    if (const std::int32_t *value = std::get_if<std::int32_t>(&value_))
    {
        return *value;
    }
    return std::nullopt;
}

std::optional<std::int64_t> DtxValue::int64() const noexcept
{
    if (const std::int64_t *value = std::get_if<std::int64_t>(&value_))
    {
        return *value;
    }
    return std::nullopt;
}

std::optional<double> DtxValue::real() const noexcept
{
    if (const double *value = std::get_if<double>(&value_))
    {
        return *value;
    }
    return std::nullopt;
}

std::optional<std::string_view> DtxValue::string() const noexcept
{
    if (const std::string *value = std::get_if<std::string>(&value_))
    {
        return *value;
    }
    return std::nullopt;
}

std::optional<std::span<const std::byte>> DtxValue::buffer() const noexcept
{
    if (const Buffer *value = std::get_if<Buffer>(&value_))
    {
        return std::span<const std::byte>(*value);
    }
    return std::nullopt;
}

std::string DtxValue::string_or(std::string_view fallback) const
{
    const std::optional<std::string_view> value = string();
    return value.has_value() ? std::string(*value) : std::string(fallback);
}

std::optional<std::uint64_t> DtxValue::as_uint64() const noexcept
{
    if (const std::int32_t *value = std::get_if<std::int32_t>(&value_))
    {
        return static_cast<std::uint64_t>(*value);
    }
    if (const std::int64_t *value = std::get_if<std::int64_t>(&value_))
    {
        return static_cast<std::uint64_t>(*value);
    }
    return std::nullopt;
}

Dtx Dtx::ack(const Dtx &message)
{
    Dtx ack;
    ack.identifier = message.identifier;
    ack.conversation_index = message.conversation_index + 1;
    ack.channel_code = message.channel_code;
    ack.expects_reply = false;
    ack.message_type = DtxMessageType::Ok;
    return ack;
}

std::vector<std::byte> Dtx::encode() const
{
    std::vector<std::byte> auxiliary_body;
    for (const DtxValue &value : auxiliary)
    {
        append_u32(auxiliary_body, static_cast<std::uint32_t>(DtxValueType::Null));
        encode_value(value, auxiliary_body);
    }

    const std::size_t auxiliary_size = auxiliary.empty() ? 0 : kDtxAuxiliaryHeaderSize + auxiliary_body.size();
    const std::size_t total_size = auxiliary_size + payload.size();
    const std::size_t message_length = kDtxPayloadHeaderSize + total_size;

    std::vector<std::byte> out;
    append_u32(out, kDtxMagic);
    append_u32(out, static_cast<std::uint32_t>(kDtxHeaderSize));
    append_u16(out, fragment_index);
    append_u16(out, fragment_count);
    append_u32(out, static_cast<std::uint32_t>(message_length));
    append_u32(out, identifier);
    append_u32(out, conversation_index);
    append_u32(out, static_cast<std::uint32_t>(channel_code));
    append_u32(out, expects_reply ? kDtxFlagExpectsReply : 0u);

    out.push_back(static_cast<std::byte>(message_type));
    out.push_back(std::byte{0});
    out.push_back(std::byte{0});
    out.push_back(std::byte{0});
    append_u32(out, static_cast<std::uint32_t>(auxiliary_size));
    append_u32(out, static_cast<std::uint32_t>(total_size));
    // The payload's own flags word; always zero for the services here.
    append_u32(out, 0);

    if (!auxiliary.empty())
    {
        append_u32(out, kDtxAuxiliaryMagic);
        // A reserved field in the auxiliary header; always zero.
        append_u32(out, 0);
        append_u64(out, static_cast<std::uint64_t>(auxiliary_body.size()));
        out.insert(out.end(), auxiliary_body.begin(), auxiliary_body.end());
    }
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

Result<Dtx> Dtx::parse(std::span<const std::byte> bytes)
{
    Reader reader{bytes, 0};

    // The message header starts with the magic and its own size.
    auto magic = reader.u32();
    if (!magic)
    {
        return tl::unexpected(magic.error());
    }
    if (*magic != kDtxMagic)
    {
        return tl::unexpected(protocol_error("the DTX message has a bad magic"));
    }
    auto header_size = reader.u32();
    if (!header_size)
    {
        return tl::unexpected(header_size.error());
    }
    if (*header_size < kDtxHeaderSize)
    {
        return tl::unexpected(protocol_error("the DTX message header is too short"));
    }
    if (*header_size > kDtxHeaderSize)
    {
        if (auto skipped = reader.take(*header_size - kDtxHeaderSize); !skipped)
        {
            return tl::unexpected(skipped.error());
        }
    }

    // The rest of the message header: the fragment pair, the length, the
    // identifier, the conversation index, the channel, and whether a reply is
    // expected.
    Dtx message;
    auto fragment_index = reader.u16();
    if (!fragment_index)
    {
        return tl::unexpected(fragment_index.error());
    }
    auto fragment_count = reader.u16();
    if (!fragment_count)
    {
        return tl::unexpected(fragment_count.error());
    }
    message.fragment_index = *fragment_index;
    message.fragment_count = *fragment_count;
    if (message.fragment_count > 1)
    {
        return tl::unexpected(protocol_error("a fragmented DTX message is not supported yet"));
    }

    auto message_length = reader.u32();
    if (!message_length)
    {
        return tl::unexpected(message_length.error());
    }
    if (*message_length > kDtxMaxMessageSize)
    {
        return tl::unexpected(protocol_error("the DTX message is too large"));
    }
    auto identifier = reader.u32();
    if (!identifier)
    {
        return tl::unexpected(identifier.error());
    }
    message.identifier = *identifier;
    auto conversation_index = reader.u32();
    if (!conversation_index)
    {
        return tl::unexpected(conversation_index.error());
    }
    message.conversation_index = *conversation_index;
    auto channel_code = reader.u32();
    if (!channel_code)
    {
        return tl::unexpected(channel_code.error());
    }
    message.channel_code = static_cast<std::int32_t>(*channel_code);
    auto expects_reply = reader.u32();
    if (!expects_reply)
    {
        return tl::unexpected(expects_reply.error());
    }
    message.expects_reply = *expects_reply != 0;

    // The payload header: the type and flags, the auxiliary and total sizes, and
    // the payload's own flags.
    auto message_type = reader.u8();
    if (!message_type)
    {
        return tl::unexpected(message_type.error());
    }
    message.message_type = static_cast<DtxMessageType>(*message_type);
    // Three flag and reserved bytes, unused but consumed to reach the sizes.
    if (auto skipped = reader.take(3); !skipped)
    {
        return tl::unexpected(skipped.error());
    }
    auto auxiliary_size = reader.u32();
    if (!auxiliary_size)
    {
        return tl::unexpected(auxiliary_size.error());
    }
    auto total_size = reader.u32();
    if (!total_size)
    {
        return tl::unexpected(total_size.error());
    }
    // The payload's own flags word; unused, and always zero for the services here.
    if (auto skipped = reader.take(4); !skipped)
    {
        return tl::unexpected(skipped.error());
    }

    if (*total_size < *auxiliary_size)
    {
        return tl::unexpected(protocol_error("the DTX payload header has inconsistent sizes"));
    }
    if (*message_length != kDtxPayloadHeaderSize + *total_size)
    {
        return tl::unexpected(protocol_error("the DTX message header and payload header disagree"));
    }

    // The auxiliary comes first when its size is non-zero, then the payload.
    if (*auxiliary_size > 0)
    {
        auto auxiliary = decode_auxiliary(reader);
        if (!auxiliary)
        {
            return tl::unexpected(auxiliary.error());
        }
        message.auxiliary = std::move(*auxiliary);
    }

    const std::size_t payload_size = *total_size - *auxiliary_size;
    auto payload = reader.take(payload_size);
    if (!payload)
    {
        return tl::unexpected(payload.error());
    }
    message.payload.assign(payload->begin(), payload->end());

    if (reader.offset != bytes.size())
    {
        return tl::unexpected(protocol_error("the DTX message has trailing bytes"));
    }
    return message;
}

bool Dtx::is_error() const noexcept
{
    return message_type == DtxMessageType::Error;
}

} // namespace ioscpp::protocol
