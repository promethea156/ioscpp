#include "ioscpp/protocol/dtx.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string>
#include <vector>

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

} // namespace

TEST_CASE("a DTX message round-trips its auxiliary and payload", "[dtx]")
{
    Dtx message;
    message.identifier = 1;
    message.channel_code = 0;
    message.expects_reply = true;
    message.message_type = DtxMessageType::Dispatch;
    message.auxiliary = {DtxValue(std::int32_t{7})};
    message.payload = bytes({0x68, 0x69});

    const std::vector<std::byte> encoded = message.encode();
    auto decoded = Dtx::parse(encoded);
    REQUIRE(decoded.has_value());
    CHECK(decoded->identifier == 1);
    CHECK(decoded->conversation_index == 0);
    CHECK(decoded->channel_code == 0);
    CHECK(decoded->expects_reply);
    CHECK(decoded->message_type == DtxMessageType::Dispatch);
    REQUIRE(decoded->auxiliary.size() == 1);
    CHECK(decoded->auxiliary[0].int32() == 7);
    CHECK(decoded->payload == message.payload);
}

TEST_CASE("a DTX message encodes the pinned byte vector", "[dtx]")
{
    Dtx message;
    message.identifier = 1;
    message.channel_code = 0;
    message.expects_reply = true;
    message.message_type = DtxMessageType::Dispatch;
    message.auxiliary = {DtxValue(std::int32_t{7})};
    message.payload = bytes({0x68, 0x69});

    // The magic and the header size, then the fragment index and count, then the
    // message length (16 + 28 + 2), the identifier, the conversation index, the
    // channel code, and the expects-reply flag. Then the payload header (the type,
    // the three reserved bytes, the auxiliary size with its 16-byte header, the
    // total size, and the flags), the auxiliary header, the null key and the
    // `Int32` value, and the payload.
    const std::vector<std::byte> expected = bytes({
        0x79, 0x5b, 0x3d, 0x1f, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x2e, 0x00, 0x00, 0x00,
        0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
        0x02, 0x00, 0x00, 0x00, 0x1c, 0x00, 0x00, 0x00, 0x1e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0xf0, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x0a, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x68, 0x69,
    });
    CHECK(message.encode() == expected);
}

TEST_CASE("a DTX auxiliary value round-trips every primitive", "[dtx]")
{
    Dtx message;
    message.message_type = DtxMessageType::Object;
    message.auxiliary = {
        DtxValue(),    DtxValue(std::int32_t{42}),     DtxValue(std::int64_t{1} << 40),
        DtxValue(3.5), DtxValue(std::string("hello")), DtxValue::buffer(bytes({0x01, 0x02, 0x03})),
    };

    const std::vector<std::byte> encoded = message.encode();
    auto decoded = Dtx::parse(encoded);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->auxiliary.size() == 6);
    CHECK(decoded->auxiliary[0].is_null());
    CHECK(decoded->auxiliary[1].int32() == 42);
    CHECK(decoded->auxiliary[2].int64() == (std::int64_t{1} << 40));
    CHECK(decoded->auxiliary[3].real() == 3.5);
    CHECK(decoded->auxiliary[4].string() == "hello");
    REQUIRE(decoded->auxiliary[5].buffer().has_value());
    CHECK(decoded->auxiliary[5].buffer()->size() == 3);
}

TEST_CASE("a DTX message without auxiliary data is sized without an auxiliary header", "[dtx]")
{
    Dtx message;
    message.message_type = DtxMessageType::Ok;

    const std::vector<std::byte> encoded = message.encode();
    CHECK(encoded.size() == kDtxHeaderSize + kDtxPayloadHeaderSize);

    auto decoded = Dtx::parse(encoded);
    REQUIRE(decoded.has_value());
    CHECK(decoded->auxiliary.empty());
    CHECK(decoded->payload.empty());
}

TEST_CASE("a DTX acknowledgement answers the request", "[dtx]")
{
    Dtx request;
    request.identifier = 5;
    request.channel_code = 3;
    request.expects_reply = true;
    request.message_type = DtxMessageType::Dispatch;

    const Dtx ack = Dtx::ack(request);
    CHECK(ack.identifier == 5);
    CHECK(ack.conversation_index == 1);
    CHECK(ack.channel_code == 3);
    CHECK_FALSE(ack.expects_reply);
    CHECK(ack.message_type == DtxMessageType::Ok);
    CHECK(ack.auxiliary.empty());
    CHECK(ack.payload.empty());

    const std::vector<std::byte> encoded = ack.encode();
    CHECK(encoded.size() == kDtxHeaderSize + kDtxPayloadHeaderSize);
    auto decoded = Dtx::parse(encoded);
    REQUIRE(decoded.has_value());
    CHECK(decoded->message_type == DtxMessageType::Ok);
    CHECK(decoded->identifier == 5);
    CHECK(decoded->conversation_index == 1);
}

TEST_CASE("a bad DTX magic is a protocol error", "[dtx]")
{
    Dtx message;
    std::vector<std::byte> encoded = message.encode();
    encoded[0] = std::byte{'X'};

    auto decoded = Dtx::parse(encoded);
    REQUIRE_FALSE(decoded.has_value());
    CHECK(decoded.error().code == ErrorCode::Protocol);
}

TEST_CASE("a truncated DTX message is a protocol error", "[dtx]")
{
    Dtx message;
    message.auxiliary = {DtxValue(std::int32_t{1})};
    message.payload = bytes({0x01});
    const std::vector<std::byte> encoded = message.encode();

    for (std::size_t length = 0; length < encoded.size(); ++length)
    {
        auto decoded = Dtx::parse(std::span<const std::byte>(encoded.data(), length));
        REQUIRE_FALSE(decoded.has_value());
        CHECK(decoded.error().code == ErrorCode::Protocol);
    }
}

TEST_CASE("a DTX message with trailing bytes is a protocol error", "[dtx]")
{
    Dtx message;
    std::vector<std::byte> encoded = message.encode();
    encoded.push_back(std::byte{0});

    auto decoded = Dtx::parse(encoded);
    REQUIRE_FALSE(decoded.has_value());
    CHECK(decoded.error().code == ErrorCode::Protocol);
}
