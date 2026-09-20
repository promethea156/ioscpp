#include "ioscpp/protocol/ipv6.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "ioscpp/error.hpp"
#include "ioscpp/testing/mock_transport.hpp"

using namespace ioscpp;
using namespace ioscpp::protocol;

namespace
{

/// Builds one IPv6 packet: the fixed header and `payload_length` payload bytes.
std::vector<std::byte> make_packet(std::uint16_t payload_length, std::uint8_t version = 6)
{
    Ipv6Header header;
    header.version = version;
    header.payload_length = payload_length;
    header.next_header = 17; // UDP, an arbitrary non-zero value
    const auto bytes = header.encode();

    std::vector<std::byte> packet(bytes.begin(), bytes.end());
    packet.resize(kIpv6HeaderSize + payload_length, std::byte{0xab});
    return packet;
}

} // namespace

TEST_CASE("an IPv6 header round-trips", "[ipv6]")
{
    Ipv6Header header;
    header.version = 6;
    header.traffic_class = 0x2a;
    header.flow_label = 0x0abcde;
    header.payload_length = 0x1234;
    header.next_header = 6;
    header.hop_limit = 64;
    header.source[0] = std::byte{0xfe};
    header.source[15] = std::byte{0x01};
    header.destination[15] = std::byte{0x02};

    const auto bytes = header.encode();
    const Ipv6Header decoded =
        Ipv6Header::decode(std::span<const std::byte, kIpv6HeaderSize>(bytes.data(), kIpv6HeaderSize));

    CHECK(decoded.version == 6);
    CHECK(decoded.traffic_class == 0x2a);
    CHECK(decoded.flow_label == 0x0abcde);
    CHECK(decoded.payload_length == 0x1234);
    CHECK(decoded.next_header == 6);
    CHECK(decoded.hop_limit == 64);
    CHECK(decoded.source == header.source);
    CHECK(decoded.destination == header.destination);
}

TEST_CASE("the re-framer splits coalesced packets", "[ipv6]")
{
    testing::MockTransport transport;
    const std::vector<std::byte> first = make_packet(16);
    const std::vector<std::byte> second = make_packet(24);
    transport.feed(first);
    transport.feed(second);

    Ipv6Framer framer(transport);
    auto one = framer.read_packet();
    REQUIRE(one.has_value());
    CHECK(*one == first);

    auto two = framer.read_packet();
    REQUIRE(two.has_value());
    CHECK(*two == second);
}

TEST_CASE("the re-framer reassembles a partial packet", "[ipv6]")
{
    testing::MockTransport transport;
    const std::vector<std::byte> packet = make_packet(32);
    transport.feed(packet);
    // One byte per read, so the header and the payload both arrive split.
    transport.set_read_chunk(1);

    Ipv6Framer framer(transport);
    auto read = framer.read_packet();
    REQUIRE(read.has_value());
    CHECK(*read == packet);
}

TEST_CASE("a non-IPv6 packet is a protocol error", "[ipv6]")
{
    testing::MockTransport transport;
    transport.feed(make_packet(8, 4));

    Ipv6Framer framer(transport);
    auto read = framer.read_packet();
    REQUIRE_FALSE(read.has_value());
    CHECK(read.error().code == ErrorCode::Protocol);
}

TEST_CASE("a truncated packet is a protocol error", "[ipv6]")
{
    testing::MockTransport transport;
    const std::vector<std::byte> packet = make_packet(16);
    // The header promises 16 payload bytes, but the stream ends after 5.
    transport.feed(std::span<const std::byte>(packet).first(kIpv6HeaderSize + 5));

    Ipv6Framer framer(transport);
    auto read = framer.read_packet();
    REQUIRE_FALSE(read.has_value());
    CHECK(read.error().code == ErrorCode::Protocol);
}

TEST_CASE("a packet larger than the MTU is a protocol error", "[ipv6]")
{
    testing::MockTransport transport;
    transport.feed(make_packet(30));

    // A 70-byte packet is larger than the 60-byte maximum.
    Ipv6Framer framer(transport, 60);
    auto read = framer.read_packet();
    REQUIRE_FALSE(read.has_value());
    CHECK(read.error().code == ErrorCode::Protocol);
}
