#include "ioscpp/afc.hpp"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "ioscpp/connection.hpp"
#include "ioscpp/error.hpp"
#include "ioscpp/protocol/usbmux.hpp"
#include "ioscpp/stream.hpp"
#include "ioscpp/testing/mock_transport.hpp"

using namespace ioscpp;
using namespace ioscpp::protocol;

namespace
{

constexpr std::array<char, 8> kMagic{'C', 'F', 'A', '6', 'L', 'P', 'A', 'A'};
constexpr std::uint16_t port = 62078;

/// A string with its embedded NULs kept, which `std::string(const char *)` drops.
template <std::size_t N>
std::string bytes_of(const char (&text)[N])
{
    return std::string(text, N - 1);
}

void put_le64(std::vector<std::byte> &out, std::uint64_t value)
{
    for (std::size_t i = 0; i < 8; ++i)
    {
        out.push_back(static_cast<std::byte>((value >> (8 * i)) & 0xff));
    }
}

/// Builds an AFC packet with `operation` and `payload`.
std::vector<std::byte> afc_packet(std::uint64_t operation, std::span<const std::byte> payload)
{
    std::vector<std::byte> packet(40 + payload.size());
    std::memcpy(packet.data(), kMagic.data(), kMagic.size());
    std::size_t position = 8;
    auto write = [&](std::uint64_t value)
    {
        for (std::size_t i = 0; i < 8; ++i)
        {
            packet[position + i] = static_cast<std::byte>((value >> (8 * i)) & 0xff);
        }
        position += 8;
    };
    write(packet.size());
    write(packet.size());
    write(1);
    write(operation);
    std::copy(payload.begin(), payload.end(), packet.begin() + 40);
    return packet;
}

/// Builds a mux frame carrying a TCP header with `flags`.
std::vector<std::byte> tcp_frame(std::uint16_t source, std::uint16_t destination, std::uint8_t flags)
{
    TcpHeader tcp;
    tcp.source_port = source;
    tcp.destination_port = destination;
    tcp.flags = flags;

    const std::array<std::byte, kTcpHeaderSize> tcp_bytes = tcp.encode();
    MuxHeader header;
    header.protocol = static_cast<std::uint32_t>(MuxProtocol::Tcp);
    header.length = static_cast<std::uint32_t>(kMuxHeaderSizeV1 + kTcpHeaderSize);
    const std::array<std::byte, kMuxHeaderSize> header_bytes = header.encode(1);

    std::vector<std::byte> frame(header_bytes.begin(), header_bytes.begin() + kMuxHeaderSizeV1);
    frame.insert(frame.end(), tcp_bytes.begin(), tcp_bytes.end());
    return frame;
}

/// Builds a mux frame carrying a TCP header and `payload` as its data.
std::vector<std::byte> tcp_payload(std::uint16_t source, std::uint16_t destination, std::span<const std::byte> payload)
{
    TcpHeader tcp;
    tcp.source_port = source;
    tcp.destination_port = destination;
    tcp.flags = TcpPsh | TcpAck;

    const std::array<std::byte, kTcpHeaderSize> tcp_bytes = tcp.encode();
    MuxHeader header;
    header.protocol = static_cast<std::uint32_t>(MuxProtocol::Tcp);
    header.length = static_cast<std::uint32_t>(kMuxHeaderSizeV1 + kTcpHeaderSize + payload.size());
    const std::array<std::byte, kMuxHeaderSize> header_bytes = header.encode(1);

    std::vector<std::byte> frame(header_bytes.begin(), header_bytes.begin() + kMuxHeaderSizeV1);
    frame.insert(frame.end(), tcp_bytes.begin(), tcp_bytes.end());
    frame.insert(frame.end(), payload.begin(), payload.end());
    return frame;
}

/// Opens a connection, a stream, and an AFC client over `transport`.
Afc open_afc(testing::MockTransport &transport, std::uint16_t port)
{
    // The version handshake answers version 1, so no setup packet is needed.
    VersionHeader version;
    version.major = 1;
    MuxHeader header;
    header.protocol = static_cast<std::uint32_t>(MuxProtocol::Version);
    header.length = static_cast<std::uint32_t>(kMuxHeaderSizeV1 + VersionHeader::kSize);
    const std::array<std::byte, kMuxHeaderSize> header_bytes = header.encode(0);
    std::vector<std::byte> answer(header_bytes.begin(), header_bytes.begin() + kMuxHeaderSizeV1);
    const auto version_bytes = version.encode();
    answer.insert(answer.end(), version_bytes.begin(), version_bytes.end());
    transport.feed(answer);

    auto opened = Connection::open(transport);
    REQUIRE(opened.has_value());
    auto connection = std::make_shared<Connection>(std::move(*opened));

    transport.feed(tcp_frame(port, 1, TcpSyn | TcpAck));
    auto stream = Stream::open(connection, port);
    REQUIRE(stream.has_value());

    auto afc = Afc::start(std::move(*stream));
    REQUIRE(afc.has_value());

    return std::move(*afc);
}

} // namespace

TEST_CASE("an AFC stat parses the device answer", "[afc]")
{
    testing::MockTransport transport;
    Afc afc = open_afc(transport, port);

    const std::string body = bytes_of("st_mtime\000100\000st_size\00042\000st_mode\00016877\000");
    std::vector<std::byte> payload(reinterpret_cast<const std::byte *>(body.data()),
                                   reinterpret_cast<const std::byte *>(body.data() + body.size()));
    transport.feed(tcp_payload(port, 1, afc_packet(0x02, payload)));

    auto info = afc.stat("/Documents");
    REQUIRE(info.has_value());
    REQUIRE(info->has_value());
    CHECK((*info)->size == 42);
    CHECK((*info)->is_directory());
}

TEST_CASE("an AFC stat of a missing path is empty", "[afc]")
{
    testing::MockTransport transport;
    Afc afc = open_afc(transport, port);

    std::vector<std::byte> error;
    put_le64(error, 8);
    transport.feed(tcp_payload(port, 1, afc_packet(0x01, error)));

    auto info = afc.stat("/missing");
    REQUIRE(info.has_value());
    CHECK_FALSE(info->has_value());
}

TEST_CASE("an AFC list parses directory entries", "[afc]")
{
    testing::MockTransport transport;
    Afc afc = open_afc(transport, port);

    const std::string body = bytes_of(
        "file.txt\000st_mtime\000100\000st_size\00012\000st_mode\00033176\000"
        "DCIM\000st_mtime\00050\000st_size\00096\000st_ifmt\000S_IFDIR\000");
    std::vector<std::byte> payload(reinterpret_cast<const std::byte *>(body.data()),
                                   reinterpret_cast<const std::byte *>(body.data() + body.size()));
    transport.feed(tcp_payload(port, 1, afc_packet(0x02, payload)));

    std::vector<std::byte> done;
    put_le64(done, 0);
    transport.feed(tcp_payload(port, 1, afc_packet(0x01, done)));

    auto entries = afc.list("/");
    REQUIRE(entries.has_value());
    REQUIRE(entries->size() == 2);
    CHECK((*entries)[0].name == "file.txt");
    CHECK((*entries)[0].is_regular());
    CHECK((*entries)[1].name == "DCIM");
    CHECK((*entries)[1].is_directory());
}

TEST_CASE("an AFC error status is a device error", "[afc]")
{
    testing::MockTransport transport;
    Afc afc = open_afc(transport, port);

    std::vector<std::byte> error;
    put_le64(error, 8); // AFC_E_OBJECT_NOT_FOUND
    transport.feed(tcp_payload(port, 1, afc_packet(0x01, error)));

    auto info = afc.stat("/missing");
    REQUIRE(info.has_value());
    CHECK_FALSE(info->has_value());
}
