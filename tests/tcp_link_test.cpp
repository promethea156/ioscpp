#include "ioscpp/tcp_link.hpp"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "ioscpp/error.hpp"
#include "ioscpp/protocol/ipv6.hpp"
#include "ioscpp/protocol/usbmux.hpp"
#include "ioscpp/transport.hpp"

using namespace ioscpp;
using namespace ioscpp::protocol;

namespace
{

/// The device's side of the tunnel, scripted as an IPv6/TCP peer.
///
/// It parses each IPv6 packet the link writes, answers the SYN with a SYN|ACK,
/// acknowledges data, and can send data back. The link does not verify the
/// checksum of a received segment, so the peer leaves it zero.
class ScriptedPeer : public Transport
{
public:
    Result<std::size_t> read(std::span<std::byte> buffer) override
    {
        if (incoming_offset_ == incoming_.size())
        {
            return 0;
        }
        const std::size_t count = std::min(incoming_.size() - incoming_offset_, buffer.size());
        std::copy_n(incoming_.begin() + static_cast<std::ptrdiff_t>(incoming_offset_),
                    static_cast<std::ptrdiff_t>(count), buffer.begin());
        incoming_offset_ += count;
        return count;
    }

    Status write(std::span<const std::byte> data) override
    {
        if (data.size() < kIpv6HeaderSize + kTcpHeaderSize)
        {
            return {};
        }
        const Ipv6Header ipv6 =
            Ipv6Header::decode(std::span<const std::byte, kIpv6HeaderSize>(data.data(), kIpv6HeaderSize));
        const TcpHeader tcp = TcpHeader::decode(
            std::span<const std::byte, kTcpHeaderSize>(data.data() + kIpv6HeaderSize, kTcpHeaderSize));
        const std::span<const std::byte> payload = data.subspan(kIpv6HeaderSize + kTcpHeaderSize);

        if ((tcp.flags & TcpRst) != 0)
        {
            return {};
        }
        if ((tcp.flags & TcpSyn) != 0)
        {
            client_source_ = ipv6.source;
            client_port_ = tcp.source_port;
            client_next_ = tcp.sequence + 1;
            reply(TcpSyn | TcpAck, ipv6, tcp, server_seq_, client_next_, {});
            server_seq_ += 1;
            return {};
        }
        if (!payload.empty())
        {
            received_.insert(received_.end(), payload.begin(), payload.end());
            reply(TcpAck, ipv6, tcp, server_seq_, tcp.sequence + static_cast<std::uint32_t>(payload.size()), {});
            return {};
        }
        if ((tcp.flags & TcpFin) != 0)
        {
            reply(TcpAck | TcpFin, ipv6, tcp, server_seq_, tcp.sequence + 1, {});
        }
        return {};
    }

    void close() override
    {
    }

    /// Sends `payload` to the link as one IPv6/TCP segment.
    void send(std::span<const std::byte> payload)
    {
        Ipv6Header ipv6;
        ipv6.payload_length = static_cast<std::uint16_t>(kTcpHeaderSize + payload.size());
        ipv6.next_header = 6;
        ipv6.hop_limit = 64;
        ipv6.source = server_source_;
        ipv6.destination = client_source_;

        TcpHeader tcp;
        tcp.source_port = server_port_;
        tcp.destination_port = client_port_;
        tcp.sequence = server_seq_;
        tcp.acknowledgement = client_next_;
        tcp.flags = TcpAck;
        tcp.window = 0xffff;

        const auto ipv6_bytes = ipv6.encode();
        const auto tcp_bytes = tcp.encode();
        std::vector<std::byte> packet(ipv6_bytes.begin(), ipv6_bytes.end());
        packet.insert(packet.end(), tcp_bytes.begin(), tcp_bytes.end());
        packet.insert(packet.end(), payload.begin(), payload.end());
        incoming_.insert(incoming_.end(), packet.begin(), packet.end());
        server_seq_ += static_cast<std::uint32_t>(payload.size());
    }

    /// The bytes the link wrote as data, in order.
    const std::vector<std::byte> &received() const noexcept
    {
        return received_;
    }

    /// Rejects the next connect with a reset instead of a SYN|ACK.
    void refuse()
    {
        refuse_ = true;
    }

    static constexpr std::size_t kIpv6HeaderSize = 40;
    static constexpr std::size_t kTcpHeaderSize = 20;

    void reply(std::uint8_t flags, const Ipv6Header &request_ipv6, const TcpHeader &request_tcp, std::uint32_t seq,
               std::uint32_t ack, std::span<const std::byte> payload)
    {
        if (refuse_)
        {
            flags = TcpRst;
        }

        Ipv6Header ipv6;
        ipv6.payload_length = static_cast<std::uint16_t>(kTcpHeaderSize + payload.size());
        ipv6.next_header = 6;
        ipv6.hop_limit = 64;
        ipv6.source = request_ipv6.destination;
        ipv6.destination = request_ipv6.source;

        TcpHeader tcp;
        tcp.source_port = request_tcp.destination_port;
        tcp.destination_port = request_tcp.source_port;
        tcp.sequence = seq;
        tcp.acknowledgement = ack;
        tcp.flags = flags;
        tcp.window = 0xffff;

        const auto ipv6_bytes = ipv6.encode();
        const auto tcp_bytes = tcp.encode();
        std::vector<std::byte> packet(ipv6_bytes.begin(), ipv6_bytes.end());
        packet.insert(packet.end(), tcp_bytes.begin(), tcp_bytes.end());
        packet.insert(packet.end(), payload.begin(), payload.end());
        incoming_.insert(incoming_.end(), packet.begin(), packet.end());
    }

    std::vector<std::byte> incoming_;
    std::size_t incoming_offset_ = 0;
    std::vector<std::byte> received_;

    std::array<std::byte, 16> client_source_{};
    std::uint16_t client_port_ = 0;
    std::uint32_t client_next_ = 0;

    std::array<std::byte, 16> server_source_{};
    std::uint16_t server_port_ = 0;
    std::uint32_t server_seq_ = 1000;

    bool refuse_ = false;
};

constexpr std::string_view kClientAddress = "fe80::1";
constexpr std::string_view kServerAddress = "fe80::2";

} // namespace

TEST_CASE("a link completes the TCP connect handshake", "[tcp-link]")
{
    ScriptedPeer peer;
    peer.server_source_ = std::array<std::byte, 16>{};
    peer.server_source_[15] = std::byte{0x02};
    peer.server_port_ = 54323;

    auto link = TcpLink::open(peer, kClientAddress, 1280);
    REQUIRE(link.has_value());
    CHECK(link->connect(kServerAddress, 54323).has_value());
}

TEST_CASE("a refused port is a device error", "[tcp-link]")
{
    ScriptedPeer peer;
    peer.refuse();

    auto link = TcpLink::open(peer, kClientAddress, 1280);
    REQUIRE(link.has_value());

    auto connected = link->connect(kServerAddress, 54323);
    REQUIRE_FALSE(connected.has_value());
    CHECK(connected.error().code == ErrorCode::Device);
}

TEST_CASE("a link write reaches the peer as data", "[tcp-link]")
{
    ScriptedPeer peer;
    peer.server_port_ = 54323;

    auto link = TcpLink::open(peer, kClientAddress, 1280);
    REQUIRE(link.has_value());
    REQUIRE(link->connect(kServerAddress, 54323).has_value());

    const std::array<std::byte, 5> data{std::byte{'h'}, std::byte{'e'}, std::byte{'l'}, std::byte{'l'}, std::byte{'o'}};
    REQUIRE(link->write(data).has_value());

    const std::vector<std::byte> &received = peer.received();
    REQUIRE(received.size() == data.size());
    CHECK(std::equal(received.begin(), received.end(), data.begin()));
}

TEST_CASE("a link read returns the peer's data", "[tcp-link]")
{
    ScriptedPeer peer;
    peer.server_port_ = 54323;

    auto link = TcpLink::open(peer, kClientAddress, 1280);
    REQUIRE(link.has_value());
    REQUIRE(link->connect(kServerAddress, 54323).has_value());

    const std::array<std::byte, 4> data{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
    peer.send(data);

    std::array<std::byte, 8> buffer{};
    auto read = link->read(buffer);
    REQUIRE(read.has_value());
    REQUIRE(*read == data.size());
    CHECK(std::equal(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(*read), data.begin()));
}

TEST_CASE("a bad tunnel address is an invalid argument", "[tcp-link]")
{
    ScriptedPeer peer;
    auto link = TcpLink::open(peer, "not-an-address", 1280);
    REQUIRE_FALSE(link.has_value());
    CHECK(link.error().code == ErrorCode::InvalidArgument);
}
