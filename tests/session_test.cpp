#include "ioscpp/session.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "ioscpp/protocol/usbmux.hpp"
#include "ioscpp/testing/mock_transport.hpp"

using namespace ioscpp;
using namespace ioscpp::protocol;

TEST_CASE("a session writes a v1 header before negotiation", "[session]")
{
    testing::MockTransport transport;
    Session session(transport);

    const std::array<std::byte, 1> payload{std::byte{0x07}};
    REQUIRE(session.send(MuxProtocol::Setup, payload).has_value());

    const std::vector<std::byte> &written = transport.written();
    REQUIRE(written.size() == kMuxHeaderSizeV1 + payload.size());
    CHECK(written[0] == std::byte{0});
    CHECK(written[7] == std::byte{kMuxHeaderSizeV1 + payload.size()});
    CHECK(written[8] == std::byte{0x07});
}

TEST_CASE("a session writes a v2 header after negotiation", "[session]")
{
    testing::MockTransport transport;
    Session session(transport);
    session.set_version(2);

    REQUIRE(session.send(MuxProtocol::Control, {}).has_value());

    const std::vector<std::byte> &written = transport.written();
    REQUIRE(written.size() == kMuxHeaderSize);
    CHECK(written[11] == std::byte{0xce}); // the magic's low byte
    CHECK(written[7] == std::byte{kMuxHeaderSize});
}

TEST_CASE("a session receives a frame", "[session]")
{
    testing::MockTransport transport;
    Session session(transport);
    session.set_version(2);

    MuxHeader header;
    header.protocol = static_cast<std::uint32_t>(MuxProtocol::Tcp);
    header.length = kMuxHeaderSize + 3;
    const std::array<std::byte, kMuxHeaderSize> encoded = header.encode(2);

    std::vector<std::byte> frame(encoded.begin(), encoded.end());
    frame.push_back(std::byte{0x01});
    frame.push_back(std::byte{0x02});
    frame.push_back(std::byte{0x03});
    transport.feed(frame);

    auto received = session.receive();
    REQUIRE(received.has_value());
    CHECK(received->header.protocol == header.protocol);
    REQUIRE(received->payload.size() == 3);
    CHECK(received->payload[2] == std::byte{0x03});
}

TEST_CASE("a bad magic is a protocol error", "[session]")
{
    testing::MockTransport transport;
    Session session(transport);
    session.set_version(2);

    std::vector<std::byte> frame(kMuxHeaderSize, std::byte{0});
    transport.feed(frame);

    auto received = session.receive();
    REQUIRE_FALSE(received.has_value());
    CHECK(received.error().code == ErrorCode::Protocol);
}

TEST_CASE("a truncated frame is a protocol error", "[session]")
{
    testing::MockTransport transport;
    Session session(transport);

    // A length of 8 says no payload follows, but only 4 bytes are queued.
    std::vector<std::byte> frame{std::byte{0}, std::byte{0}, std::byte{0}, std::byte{6}};
    transport.feed(frame);

    auto received = session.receive();
    REQUIRE_FALSE(received.has_value());
    CHECK(received.error().code == ErrorCode::Protocol);
}
