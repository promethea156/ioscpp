#include "ioscpp/dtx_connection.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ioscpp/protocol/keyed_archive.hpp"

namespace ioscpp
{
namespace
{

Error protocol_error(std::string message)
{
    return Error{ErrorCode::Protocol, std::move(message)};
}

/// Reads the little-endian 32-bit word at `offset`.
std::uint32_t read_u32(std::span<const std::byte> bytes, std::size_t offset)
{
    std::uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i)
    {
        value |= static_cast<std::uint32_t>(std::to_integer<unsigned char>(bytes[offset + i])) << (8 * i);
    }
    return value;
}

/// Archives `value` into the payload of a method call or channel request.
std::vector<std::byte> archived(std::string_view selector)
{
    return protocol::KeyedArchive::archive(protocol::Plist(std::string(selector)));
}

} // namespace

DtxChannel::DtxChannel(DtxConnection *connection, std::int32_t code, std::string identifier)
    : connection_(connection)
    , code_(code)
    , identifier_(std::move(identifier))
{
}

std::int32_t DtxChannel::code() const noexcept
{
    return code_;
}

const std::string &DtxChannel::identifier() const noexcept
{
    return identifier_;
}

Result<protocol::Dtx> DtxChannel::method_call(std::string_view selector, std::span<const protocol::Plist> arguments)
{
    protocol::Dtx::Auxiliary auxiliary;
    auxiliary.reserve(arguments.size());
    for (const protocol::Plist &argument : arguments)
    {
        auxiliary.push_back(protocol::DtxValue::buffer(protocol::KeyedArchive::archive(argument)));
    }
    return method_call_with_auxiliary(selector, std::move(auxiliary));
}

Result<protocol::Dtx> DtxChannel::method_call_with_auxiliary(std::string_view selector,
                                                             protocol::Dtx::Auxiliary auxiliary)
{
    if (connection_ == nullptr)
    {
        return tl::unexpected(protocol_error("the DTX channel is not attached to a connection"));
    }
    return connection_->invoke(code_, selector, std::move(auxiliary));
}

DtxConnection::DtxConnection(ByteStream &stream)
    : stream_(&stream)
{
}

Status DtxConnection::send(const protocol::Dtx &message)
{
    const std::vector<std::byte> frame = message.encode();
    return stream_->write(frame);
}

Result<protocol::Dtx> DtxConnection::receive()
{
    std::vector<std::byte> header(protocol::kDtxHeaderSize);
    if (auto status = stream_->read_exact(header); !status)
    {
        return tl::unexpected(status.error());
    }

    // The header size and the message length size the rest of the frame, which the
    // codec then parses whole. Reading the header first is what lets the connection
    // know how much of the stream belongs to this message.
    const std::uint32_t magic = read_u32(header, 0);
    const std::uint32_t header_size = read_u32(header, 4);
    const std::uint32_t message_length = read_u32(header, 12);
    if (magic != protocol::kDtxMagic)
    {
        return tl::unexpected(protocol_error("the DTX message has a bad magic"));
    }
    if (header_size < protocol::kDtxHeaderSize)
    {
        return tl::unexpected(protocol_error("the DTX message header is too short"));
    }
    if (message_length > protocol::kDtxMaxMessageSize)
    {
        return tl::unexpected(protocol_error("the DTX message is too large"));
    }

    std::vector<std::byte> frame(static_cast<std::size_t>(header_size) + message_length);
    std::copy(header.begin(), header.end(), frame.begin());
    if (frame.size() > protocol::kDtxHeaderSize)
    {
        auto rest = std::span<std::byte>(frame).subspan(protocol::kDtxHeaderSize);
        if (auto status = stream_->read_exact(rest); !status)
        {
            return tl::unexpected(status.error());
        }
    }

    auto message = protocol::Dtx::parse(frame);
    if (!message)
    {
        return tl::unexpected(message.error());
    }
    if (message->expects_reply)
    {
        if (auto status = send(protocol::Dtx::ack(*message)); !status)
        {
            return tl::unexpected(status.error());
        }
    }
    return message;
}

Result<protocol::Dtx> DtxConnection::send_and_await_reply(protocol::Dtx request)
{
    if (request.identifier == 0)
    {
        request.identifier = next_identifier_++;
    }
    request.conversation_index = 0;
    request.expects_reply = true;
    if (auto status = send(request); !status)
    {
        return tl::unexpected(status.error());
    }

    // The device may send a notification or another request before it answers, so a
    // message that is not this reply is queued rather than dropped.
    while (true)
    {
        auto message = receive();
        if (!message)
        {
            return tl::unexpected(message.error());
        }
        if (message->identifier == request.identifier && message->conversation_index > 0)
        {
            return *message;
        }
        pending_.push_back(std::move(*message));
    }
}

Result<DtxChannel> DtxConnection::request_channel(std::string_view identifier)
{
    const std::int32_t code = next_channel_code_++;
    protocol::Dtx::Auxiliary auxiliary;
    auxiliary.push_back(protocol::DtxValue(static_cast<std::int32_t>(code)));
    auxiliary.push_back(
        protocol::DtxValue::buffer(protocol::KeyedArchive::archive(protocol::Plist(std::string(identifier)))));

    auto reply = invoke(kDtxGlobalChannel, kDtxRequestChannelSelector, std::move(auxiliary));
    if (!reply)
    {
        return tl::unexpected(reply.error());
    }
    return DtxChannel(this, code, std::string(identifier));
}

bool DtxConnection::has_pending() const noexcept
{
    return !pending_.empty();
}

Result<protocol::Dtx> DtxConnection::next_pending()
{
    if (pending_.empty())
    {
        return tl::unexpected(protocol_error("no DTX message is pending"));
    }
    protocol::Dtx message = std::move(pending_.front());
    pending_.pop_front();
    return message;
}

Result<protocol::Dtx> DtxConnection::invoke(std::int32_t channel_code, std::string_view selector,
                                            protocol::Dtx::Auxiliary auxiliary)
{
    protocol::Dtx request;
    request.channel_code = channel_code;
    request.message_type = protocol::DtxMessageType::Dispatch;
    request.payload = archived(selector);
    request.auxiliary = std::move(auxiliary);
    return send_and_await_reply(std::move(request));
}

} // namespace ioscpp
