#include "ioscpp/dtx_connection.hpp"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string>
#include <vector>

#include "ioscpp/byte_stream.hpp"
#include "ioscpp/error.hpp"
#include "ioscpp/protocol/dtx.hpp"
#include "ioscpp/protocol/keyed_archive.hpp"
#include "ioscpp/protocol/plist.hpp"

using namespace ioscpp;
using namespace ioscpp::protocol;

namespace
{

std::vector<std::byte> bytes(std::initializer_list<unsigned> values)
{
    std::vector<std::byte> out;
    for (const unsigned value : values)
    {
        out.push_back(static_cast<std::byte>(value));
    }
    return out;
}

/// An in-memory `ByteStream` a test feeds and inspects.
class MemoryByteStream : public ByteStream
{
public:
    /// Queues `data` to be returned by subsequent `read_exact` calls.
    void feed(std::span<const std::byte> data)
    {
        incoming_.insert(incoming_.end(), data.begin(), data.end());
    }

    Status read_exact(std::span<std::byte> buffer) override
    {
        if (read_position_ + buffer.size() > incoming_.size())
        {
            return tl::unexpected(Error{ErrorCode::Io, "the stream ended early"});
        }
        std::copy_n(incoming_.begin() + static_cast<std::ptrdiff_t>(read_position_),
                    static_cast<std::ptrdiff_t>(buffer.size()), buffer.begin());
        read_position_ += buffer.size();
        return {};
    }

    Status write(std::span<const std::byte> data) override
    {
        written_.insert(written_.end(), data.begin(), data.end());
        return {};
    }

    void close() override
    {
    }

    const std::vector<std::byte> &written() const noexcept
    {
        return written_;
    }

    void clear_written()
    {
        written_.clear();
    }

private:
    std::vector<std::byte> incoming_;
    std::size_t read_position_ = 0;
    std::vector<std::byte> written_;
};

/// A message the device sends, with the identifier and conversation index set.
Dtx message(std::uint32_t identifier, std::uint32_t conversation_index, std::int32_t channel_code)
{
    Dtx dtx;
    dtx.identifier = identifier;
    dtx.conversation_index = conversation_index;
    dtx.channel_code = channel_code;
    return dtx;
}

} // namespace

TEST_CASE("a DTX connection acknowledges a message that expects a reply", "[dtx_connection]")
{
    Dtx capabilities = message(1, 0, kDtxGlobalChannel);
    capabilities.expects_reply = true;
    capabilities.message_type = DtxMessageType::Dispatch;
    capabilities.payload = KeyedArchive::archive(Plist("_notifyOfPublishedCapabilities:"));
    capabilities.auxiliary = {DtxValue::buffer(
        KeyedArchive::archive(Plist::dictionary({{"com.apple.private.DTXConnection", Plist(std::int64_t{1})}})))};

    MemoryByteStream stream;
    stream.feed(capabilities.encode());

    DtxConnection connection(stream);
    auto received = connection.receive();

    REQUIRE(received.has_value());
    CHECK(received->identifier == 1);
    CHECK(received->channel_code == kDtxGlobalChannel);
    CHECK(received->message_type == DtxMessageType::Dispatch);

    // The connection answers the handshake with an acknowledgement, so the device
    // does not wait on one.
    auto ack = Dtx::parse(stream.written());
    REQUIRE(ack.has_value());
    CHECK(ack->identifier == 1);
    CHECK(ack->conversation_index == 1);
    CHECK(ack->channel_code == kDtxGlobalChannel);
    CHECK_FALSE(ack->expects_reply);
    CHECK(ack->message_type == DtxMessageType::Ok);
}

TEST_CASE("a DTX connection opens a channel from the global channel", "[dtx_connection]")
{
    MemoryByteStream stream;
    stream.feed(message(1, 1, kDtxGlobalChannel).encode());

    DtxConnection connection(stream);
    auto channel = connection.request_channel("com.apple.instruments.server.services.processcontrol");

    REQUIRE(channel.has_value());
    CHECK(channel->code() == 1);
    CHECK(channel->identifier() == "com.apple.instruments.server.services.processcontrol");

    auto request = Dtx::parse(stream.written());
    REQUIRE(request.has_value());
    CHECK(request->identifier == 1);
    CHECK(request->conversation_index == 0);
    CHECK(request->channel_code == kDtxGlobalChannel);
    CHECK(request->expects_reply);
    CHECK(request->message_type == DtxMessageType::Dispatch);

    auto selector = KeyedArchive::unarchive(request->payload);
    REQUIRE(selector.has_value());
    CHECK(selector->string_or() == kDtxRequestChannelSelector);

    REQUIRE(request->auxiliary.size() == 2);
    CHECK(request->auxiliary[0].int32() == 1);
    REQUIRE(request->auxiliary[1].buffer().has_value());
    auto identifier = KeyedArchive::unarchive(*request->auxiliary[1].buffer());
    REQUIRE(identifier.has_value());
    CHECK(identifier->string_or() == "com.apple.instruments.server.services.processcontrol");
}

TEST_CASE("a DTX channel archives the selector and each argument", "[dtx_connection]")
{
    MemoryByteStream stream;
    stream.feed(message(1, 1, kDtxGlobalChannel).encode());

    DtxConnection connection(stream);
    auto channel = connection.request_channel("com.apple.instruments.server.services.processcontrol");
    REQUIRE(channel.has_value());
    stream.clear_written();

    Dtx reply = message(2, 1, 1);
    reply.message_type = DtxMessageType::Object;
    reply.payload = KeyedArchive::archive(Plist(std::int64_t{12345}));
    stream.feed(reply.encode());

    const std::vector<Plist> arguments = {Plist("/private/"), Plist("com.example.app")};
    auto result = channel->method_call(
        "launchSuspendedProcessWithDevicePath:bundleIdentifier:environment:arguments:options:", arguments);
    REQUIRE(result.has_value());
    REQUIRE_FALSE(result->payload.empty());
    auto pid = KeyedArchive::unarchive(result->payload);
    REQUIRE(pid.has_value());
    CHECK(pid->integer() == 12345);

    auto request = Dtx::parse(stream.written());
    REQUIRE(request.has_value());
    CHECK(request->identifier == 2);
    CHECK(request->channel_code == 1);
    CHECK(request->expects_reply);
    CHECK(request->message_type == DtxMessageType::Dispatch);

    auto selector = KeyedArchive::unarchive(request->payload);
    REQUIRE(selector.has_value());
    CHECK(selector->string_or() ==
          "launchSuspendedProcessWithDevicePath:bundleIdentifier:environment:arguments:options:");

    REQUIRE(request->auxiliary.size() == 2);
    REQUIRE(request->auxiliary[0].buffer().has_value());
    auto first = KeyedArchive::unarchive(*request->auxiliary[0].buffer());
    REQUIRE(first.has_value());
    CHECK(first->string_or() == "/private/");
    REQUIRE(request->auxiliary[1].buffer().has_value());
    auto second = KeyedArchive::unarchive(*request->auxiliary[1].buffer());
    REQUIRE(second.has_value());
    CHECK(second->string_or() == "com.example.app");
}

TEST_CASE("a DTX connection queues a message that arrives before the reply", "[dtx_connection]")
{
    Dtx notification = message(7, 0, kDtxGlobalChannel);
    notification.expects_reply = true;
    notification.message_type = DtxMessageType::Dispatch;
    notification.payload = KeyedArchive::archive(Plist("outputReceived:fromProcess:atTime:"));

    MemoryByteStream stream;
    stream.feed(notification.encode());
    stream.feed(message(1, 1, kDtxGlobalChannel).encode());

    DtxConnection connection(stream);
    auto reply = connection.send_and_await_reply(message(0, 0, kDtxGlobalChannel));

    REQUIRE(reply.has_value());
    CHECK(reply->identifier == 1);
    CHECK(reply->conversation_index == 1);
    CHECK(connection.has_pending());

    auto pending = connection.next_pending();
    REQUIRE(pending.has_value());
    CHECK(pending->identifier == 7);
    CHECK(pending->message_type == DtxMessageType::Dispatch);
    CHECK_FALSE(connection.has_pending());

    // The notification expects a reply, so it was acknowledged, and the reply did
    // not, so the only frame written besides the request is the acknowledgement.
    const std::span<const std::byte> written(stream.written());
    auto ack = Dtx::parse(written.subspan(written.size() - (kDtxHeaderSize + kDtxPayloadHeaderSize)));
    REQUIRE(ack.has_value());
    CHECK(ack->identifier == 7);
    CHECK(ack->conversation_index == 1);
    CHECK(ack->message_type == DtxMessageType::Ok);
}

TEST_CASE("a DTX connection rejects a bad magic", "[dtx_connection]")
{
    MemoryByteStream stream;
    stream.feed(
        bytes({0x58, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x10, 0x00, 0x00, 0x00,
               0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}));

    DtxConnection connection(stream);
    auto received = connection.receive();

    REQUIRE_FALSE(received.has_value());
    CHECK(received.error().code == ErrorCode::Protocol);
}
