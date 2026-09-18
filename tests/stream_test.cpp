#include "ioscpp/stream.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "ioscpp/connection.hpp"
#include "ioscpp/error.hpp"
#include "ioscpp/protocol/usbmux.hpp"
#include "ioscpp/testing/mock_transport.hpp"

using namespace ioscpp;
using namespace ioscpp::protocol;

namespace
{

constexpr std::uint16_t port = 62078;

/// The local port the first stream is allocated, matching `Connection::next_port_`.
constexpr std::uint16_t local_port = 1;

/// Feeds a version-1.0 handshake answer, so no setup packet is needed.
void feed_version(testing::MockTransport &transport)
{
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

/// Opens a connection and feeds the version answer.
std::shared_ptr<Connection> open_connection(testing::MockTransport &transport)
{
    feed_version(transport);
    auto opened = Connection::open(transport);
    REQUIRE(opened.has_value());
    return std::make_shared<Connection>(std::move(*opened));
}

} // namespace

TEST_CASE("a stream completes the connect handshake", "[stream]")
{
    testing::MockTransport transport;
    auto connection = open_connection(transport);

    transport.feed(tcp_frame(port, local_port, TcpSyn | TcpAck));
    auto stream = Stream::open(connection, port);

    REQUIRE(stream.has_value());
    CHECK(stream->port() == port);
}

TEST_CASE("a refused port is a device error", "[stream]")
{
    testing::MockTransport transport;
    auto connection = open_connection(transport);

    transport.feed(tcp_frame(port, local_port, TcpRst));
    auto stream = Stream::open(connection, port);

    REQUIRE_FALSE(stream.has_value());
    CHECK(stream.error().code == ErrorCode::Device);
}

TEST_CASE("a stream write sends a PSH|ACK frame", "[stream]")
{
    testing::MockTransport transport;
    auto connection = open_connection(transport);

    transport.feed(tcp_frame(port, local_port, TcpSyn | TcpAck));
    auto stream = Stream::open(connection, port);
    REQUIRE(stream.has_value());

    const std::size_t before = transport.written().size();
    const std::array<std::byte, 3> data{std::byte{'a'}, std::byte{'b'}, std::byte{'c'}};
    REQUIRE(stream->write(data).has_value());

    // The write is one mux frame: the v1 header, a TCP header, and the payload.
    const std::vector<std::byte> &written = transport.written();
    REQUIRE(written.size() - before == kMuxHeaderSizeV1 + kTcpHeaderSize + data.size());

    const TcpHeader tcp = TcpHeader::decode(
        std::span<const std::byte, kTcpHeaderSize>(written.data() + before + kMuxHeaderSizeV1, kTcpHeaderSize));
    CHECK(tcp.source_port == local_port);
    CHECK(tcp.destination_port == port);
    CHECK(tcp.flags == (TcpPsh | TcpAck));
    CHECK(written.back() == data.back());
}
