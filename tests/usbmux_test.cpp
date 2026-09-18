#include "ioscpp/protocol/usbmux.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <span>

using namespace ioscpp::protocol;

TEST_CASE("a v2 mux header round-trips", "[usbmux]")
{
    MuxHeader header;
    header.protocol = static_cast<std::uint32_t>(MuxProtocol::Tcp);
    header.length = 36;
    header.tx_seq = 7;
    header.rx_seq = 9;

    const std::array<std::byte, kMuxHeaderSize> encoded = header.encode(2);
    const MuxHeader decoded = MuxHeader::decode(encoded, 2);

    CHECK(decoded.protocol == header.protocol);
    CHECK(decoded.length == header.length);
    CHECK(decoded.magic == kMuxMagic);
    CHECK(decoded.tx_seq == header.tx_seq);
    CHECK(decoded.rx_seq == header.rx_seq);
}

TEST_CASE("a v1 mux header omits the v2 fields", "[usbmux]")
{
    CHECK(MuxHeader::wire_size(1) == kMuxHeaderSizeV1);
    CHECK(MuxHeader::wire_size(2) == kMuxHeaderSize);

    MuxHeader header;
    header.protocol = static_cast<std::uint32_t>(MuxProtocol::Version);
    header.length = 20;

    const std::array<std::byte, kMuxHeaderSize> encoded = header.encode(1);
    CHECK(static_cast<std::uint8_t>(encoded[0]) == 0);
    CHECK(static_cast<std::uint8_t>(encoded[3]) == 0);
    CHECK(static_cast<std::uint8_t>(encoded[4]) == 0);
    CHECK(static_cast<std::uint8_t>(encoded[7]) == 20);
}

TEST_CASE("a TCP header round-trips", "[usbmux]")
{
    TcpHeader header;
    header.source_port = 1234;
    header.destination_port = 62078;
    header.sequence = 1;
    header.acknowledgement = 2;
    header.flags = TcpSyn | TcpAck;
    header.window = 512;

    const std::array<std::byte, kTcpHeaderSize> encoded = header.encode();
    const TcpHeader decoded = TcpHeader::decode(encoded);

    CHECK(decoded.source_port == header.source_port);
    CHECK(decoded.destination_port == header.destination_port);
    CHECK(decoded.sequence == header.sequence);
    CHECK(decoded.acknowledgement == header.acknowledgement);
    CHECK(decoded.data_offset == 5);
    CHECK(decoded.flags == header.flags);
    CHECK(decoded.window == header.window);
}

TEST_CASE("a version header is big-endian", "[usbmux]")
{
    VersionHeader header;
    header.major = 2;

    const std::array<std::byte, VersionHeader::kSize> encoded = header.encode();
    CHECK(static_cast<std::uint8_t>(encoded[3]) == 2);
    CHECK(static_cast<std::uint8_t>(encoded[7]) == 0);

    const VersionHeader decoded = VersionHeader::decode(encoded);
    CHECK(decoded.major == 2);
}
