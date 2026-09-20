#include "ioscpp/tcp_link.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ioscpp/error.hpp"
#include "ioscpp/protocol/ipv6.hpp"
#include "ioscpp/protocol/usbmux.hpp"
#include "ioscpp/transport.hpp"

namespace ioscpp
{
namespace
{

Error protocol_error(std::string message)
{
    return Error{ErrorCode::Protocol, std::move(message)};
}

/// The IPv6 next-header value for TCP.
constexpr std::uint8_t kNextTcp = 6;

/// The hop limit the link sends, the same value a host stack uses.
constexpr std::uint8_t kHopLimit = 64;

/// The advertised receive window.
constexpr std::uint16_t kTcpWindow = 0xffff;

/// The first ephemeral source port, one per connection.
std::uint16_t next_source_port = 49152;

/// Parses one side of an IPv6 address, `::`-separated hex groups.
bool parse_groups(std::string_view text, std::vector<std::uint16_t> &out)
{
    if (text.empty())
    {
        return true;
    }
    std::size_t start = 0;
    while (true)
    {
        const std::size_t colon = text.find(':', start);
        const std::string_view group = text.substr(start, colon == std::string_view::npos ? colon : colon - start);
        if (group.empty() || group.size() > 4)
        {
            return false;
        }
        std::uint16_t value = 0;
        for (const char c : group)
        {
            value = static_cast<std::uint16_t>(value << 4);
            if (c >= '0' && c <= '9')
            {
                value = static_cast<std::uint16_t>(value | (c - '0'));
            }
            else if (c >= 'a' && c <= 'f')
            {
                value = static_cast<std::uint16_t>(value | (c - 'a' + 10));
            }
            else if (c >= 'A' && c <= 'F')
            {
                value = static_cast<std::uint16_t>(value | (c - 'A' + 10));
            }
            else
            {
                return false;
            }
        }
        out.push_back(value);
        if (colon == std::string_view::npos)
        {
            return true;
        }
        start = colon + 1;
    }
}

/// Parses `text`, an IPv6 address, into its 16 bytes.
Result<std::array<std::byte, 16>> parse_ipv6(std::string_view text)
{
    const std::size_t gap = text.find("::");
    const std::string_view left = gap == std::string_view::npos ? text : text.substr(0, gap);
    const std::string_view right = gap == std::string_view::npos ? std::string_view{} : text.substr(gap + 2);

    std::vector<std::uint16_t> left_groups;
    std::vector<std::uint16_t> right_groups;
    if (!parse_groups(left, left_groups) || !parse_groups(right, right_groups))
    {
        return tl::unexpected(protocol_error("the tunnel address is not IPv6"));
    }
    if (gap == std::string_view::npos)
    {
        if (left_groups.size() != 8)
        {
            return tl::unexpected(protocol_error("the tunnel address is not IPv6"));
        }
    }
    else if (left_groups.size() + right_groups.size() > 8)
    {
        return tl::unexpected(protocol_error("the tunnel address is not IPv6"));
    }

    std::array<std::byte, 16> address{};
    std::size_t index = 0;
    for (const std::uint16_t group : left_groups)
    {
        address[index++] = static_cast<std::byte>((group >> 8) & 0xff);
        address[index++] = static_cast<std::byte>(group & 0xff);
    }
    index = 16 - right_groups.size() * 2;
    for (const std::uint16_t group : right_groups)
    {
        address[index++] = static_cast<std::byte>((group >> 8) & 0xff);
        address[index++] = static_cast<std::byte>(group & 0xff);
    }
    return address;
}

/// Adds the 16-bit words of `bytes` to a running one's complement sum.
void add_words(std::uint32_t &sum, std::span<const std::byte> bytes)
{
    std::size_t i = 0;
    for (; i + 1 < bytes.size(); i += 2)
    {
        sum += (static_cast<unsigned>(bytes[i]) << 8) | static_cast<unsigned>(bytes[i + 1]);
    }
    if (i < bytes.size())
    {
        sum += static_cast<unsigned>(bytes[i]) << 8;
    }
}

std::uint16_t finish_checksum(std::uint32_t sum)
{
    while ((sum >> 16) != 0)
    {
        sum = (sum & 0xffff) + (sum >> 16);
    }
    return static_cast<std::uint16_t>(~sum);
}

/// The TCP checksum over the IPv6 pseudo-header and `segment`.
std::uint16_t tcp_checksum(const std::array<std::byte, 16> &source, const std::array<std::byte, 16> &destination,
                           std::span<const std::byte> segment)
{
    std::array<std::byte, 40> pseudo{};
    std::memcpy(pseudo.data(), source.data(), source.size());
    std::memcpy(pseudo.data() + 16, destination.data(), destination.size());
    const std::uint32_t length = static_cast<std::uint32_t>(segment.size());
    pseudo[32] = static_cast<std::byte>((length >> 24) & 0xff);
    pseudo[33] = static_cast<std::byte>((length >> 16) & 0xff);
    pseudo[34] = static_cast<std::byte>((length >> 8) & 0xff);
    pseudo[35] = static_cast<std::byte>(length & 0xff);
    pseudo[39] = static_cast<std::byte>(kNextTcp);

    std::uint32_t sum = 0;
    add_words(sum, pseudo);
    add_words(sum, segment);
    return finish_checksum(sum);
}

/// One TCP segment received from the device.
struct Segment
{
    std::uint8_t flags = 0;
    std::uint32_t sequence = 0;
    std::vector<std::byte> payload;
};

} // namespace

struct TcpLink::Impl
{
    Impl(Transport &value, std::string_view client, std::uint16_t max_transfer)
        : tunnel(&value)
        , framer(value)
        , mtu(max_transfer)
    {
        auto parsed = parse_ipv6(client);
        source_valid = parsed.has_value();
        if (parsed)
        {
            source = *parsed;
        }
        mss = static_cast<std::size_t>(mtu - protocol::kIpv6HeaderSize - protocol::kTcpHeaderSize);
        source_port = next_source_port++;
    }

    Transport *tunnel;
    protocol::Ipv6Framer framer;
    std::uint16_t mtu;
    std::size_t mss;
    bool source_valid = false;
    std::array<std::byte, 16> source{};
    std::array<std::byte, 16> destination{};
    std::uint16_t source_port = 0;
    std::uint16_t peer_port = 0;
    std::uint32_t send_seq = 0;
    std::uint32_t recv_seq = 0;
    bool established = false;
    bool closed = false;
    std::vector<std::byte> incoming;
    std::size_t incoming_offset = 0;

    Status send_segment(std::uint8_t flags, std::span<const std::byte> payload)
    {
        protocol::TcpHeader tcp;
        tcp.source_port = source_port;
        tcp.destination_port = peer_port;
        tcp.sequence = send_seq;
        tcp.acknowledgement = recv_seq;
        tcp.flags = flags;
        tcp.window = kTcpWindow;

        std::vector<std::byte> segment(protocol::kTcpHeaderSize + payload.size());
        const auto tcp_bytes = tcp.encode();
        std::memcpy(segment.data(), tcp_bytes.data(), protocol::kTcpHeaderSize);
        std::memcpy(segment.data() + protocol::kTcpHeaderSize, payload.data(), payload.size());

        // The device's TCP verifies the checksum, so it is computed over the
        // IPv6 pseudo-header and the segment, and written at the header offset.
        const std::uint16_t checksum = tcp_checksum(source, destination, segment);
        segment[16] = static_cast<std::byte>((checksum >> 8) & 0xff);
        segment[17] = static_cast<std::byte>(checksum & 0xff);

        protocol::Ipv6Header ipv6;
        ipv6.payload_length = static_cast<std::uint16_t>(segment.size());
        ipv6.next_header = kNextTcp;
        ipv6.hop_limit = kHopLimit;
        ipv6.source = source;
        ipv6.destination = destination;
        const auto ipv6_bytes = ipv6.encode();

        std::vector<std::byte> packet(protocol::kIpv6HeaderSize + segment.size());
        std::memcpy(packet.data(), ipv6_bytes.data(), protocol::kIpv6HeaderSize);
        std::memcpy(packet.data() + protocol::kIpv6HeaderSize, segment.data(), segment.size());
        if (Status status = framer.write_packet(packet); !status)
        {
            return status;
        }

        send_seq += static_cast<std::uint32_t>(payload.size()) +
                    (((flags & protocol::TcpSyn) != 0 || (flags & protocol::TcpFin) != 0) ? 1 : 0);
        return {};
    }

    Result<Segment> receive_segment()
    {
        for (;;)
        {
            auto packet = framer.read_packet();
            if (!packet)
            {
                return tl::unexpected(packet.error());
            }
            if (packet->size() < protocol::kIpv6HeaderSize + protocol::kTcpHeaderSize)
            {
                continue;
            }
            const protocol::Ipv6Header ipv6 = protocol::Ipv6Header::decode(
                std::span<const std::byte, protocol::kIpv6HeaderSize>(packet->data(), protocol::kIpv6HeaderSize));
            if (ipv6.next_header != kNextTcp)
            {
                continue;
            }
            const protocol::TcpHeader tcp =
                protocol::TcpHeader::decode(std::span<const std::byte, protocol::kTcpHeaderSize>(
                    packet->data() + protocol::kIpv6HeaderSize, protocol::kTcpHeaderSize));
            if (tcp.destination_port != source_port || tcp.source_port != peer_port)
            {
                continue;
            }
            Segment segment;
            segment.flags = tcp.flags;
            segment.sequence = tcp.sequence;
            segment.payload.assign(
                packet->begin() + static_cast<std::ptrdiff_t>(protocol::kIpv6HeaderSize + protocol::kTcpHeaderSize),
                packet->end());
            return segment;
        }
    }

    Status write(std::span<const std::byte> data)
    {
        std::size_t offset = 0;
        while (offset < data.size())
        {
            const std::size_t chunk = std::min(mss, data.size() - offset);
            if (Status status = send_segment(protocol::TcpAck, data.subspan(offset, chunk)); !status)
            {
                return status;
            }
            offset += chunk;
        }
        return {};
    }

    Result<std::size_t> read(std::span<std::byte> buffer)
    {
        while (incoming_offset == incoming.size())
        {
            incoming.clear();
            incoming_offset = 0;
            if (closed)
            {
                return 0;
            }

            auto segment = receive_segment();
            if (!segment)
            {
                return tl::unexpected(segment.error());
            }
            if ((segment->flags & protocol::TcpRst) != 0)
            {
                closed = true;
                return tl::unexpected(Error{ErrorCode::Device, "the device reset the connection"});
            }
            if ((segment->flags & protocol::TcpFin) != 0)
            {
                recv_seq = segment->sequence + static_cast<std::uint32_t>(segment->payload.size()) + 1;
                (void)send_segment(protocol::TcpAck | protocol::TcpFin, {});
                closed = true;
                if (segment->payload.empty())
                {
                    return 0;
                }
                incoming = std::move(segment->payload);
                continue;
            }
            if (!segment->payload.empty())
            {
                // The mux below the tunnel is reliable, so a segment arrives in
                // order and is acknowledged without a reordering buffer.
                recv_seq = segment->sequence + static_cast<std::uint32_t>(segment->payload.size());
                if (Status status = send_segment(protocol::TcpAck, {}); !status)
                {
                    return tl::unexpected(status.error());
                }
                incoming = std::move(segment->payload);
            }
        }

        const std::size_t count = std::min(incoming.size() - incoming_offset, buffer.size());
        std::copy_n(incoming.begin() + static_cast<std::ptrdiff_t>(incoming_offset), static_cast<std::ptrdiff_t>(count),
                    buffer.begin());
        incoming_offset += count;
        return count;
    }
};

TcpLink::TcpLink(Transport &tunnel, std::string_view client_address, std::uint16_t mtu)
    : impl_(std::make_unique<Impl>(tunnel, client_address, mtu))
{
}

TcpLink::~TcpLink() = default;
TcpLink::TcpLink(TcpLink &&) noexcept = default;
TcpLink &TcpLink::operator=(TcpLink &&) noexcept = default;

Result<TcpLink> TcpLink::open(Transport &tunnel, std::string_view client_address, std::uint16_t mtu)
{
    if (mtu <= protocol::kIpv6HeaderSize + protocol::kTcpHeaderSize)
    {
        return tl::unexpected(Error{ErrorCode::InvalidArgument, "the tunnel MTU leaves no room for a segment"});
    }
    if (!parse_ipv6(client_address).has_value())
    {
        return tl::unexpected(Error{ErrorCode::InvalidArgument, "the tunnel-local address is not IPv6"});
    }
    return TcpLink(tunnel, client_address, mtu);
}

Status TcpLink::connect(std::string_view address, std::uint16_t port)
{
    Impl &impl = *impl_;
    if (!impl.source_valid)
    {
        return tl::unexpected(protocol_error("the tunnel-local address is not IPv6"));
    }

    auto parsed = parse_ipv6(address);
    if (!parsed)
    {
        return tl::unexpected(parsed.error());
    }
    impl.destination = *parsed;
    impl.peer_port = port;
    impl.send_seq = 0;
    impl.recv_seq = 0;

    if (Status status = impl.send_segment(protocol::TcpSyn, {}); !status)
    {
        return status;
    }

    // The connect waits for the SYN|ACK and ignores anything else, because the
    // tunnel may still carry a stale segment from a previous connection.
    for (;;)
    {
        auto segment = impl.receive_segment();
        if (!segment)
        {
            return tl::unexpected(segment.error());
        }
        if ((segment->flags & protocol::TcpRst) != 0)
        {
            return tl::unexpected(Error{ErrorCode::Device, "the RSD refused the connection"});
        }
        if ((segment->flags & (protocol::TcpSyn | protocol::TcpAck)) == (protocol::TcpSyn | protocol::TcpAck))
        {
            impl.recv_seq = segment->sequence + 1;
            if (Status status = impl.send_segment(protocol::TcpAck, {}); !status)
            {
                return status;
            }
            impl.established = true;
            return {};
        }
    }
}

Result<std::size_t> TcpLink::read(std::span<std::byte> buffer)
{
    return impl_->read(buffer);
}

Status TcpLink::read_exact(std::span<std::byte> buffer)
{
    std::size_t offset = 0;
    while (offset < buffer.size())
    {
        auto read = impl_->read(buffer.subspan(offset));
        if (!read)
        {
            return tl::unexpected(read.error());
        }
        if (*read == 0)
        {
            return tl::unexpected(Error{ErrorCode::Protocol, "the connection ended mid-message"});
        }
        offset += *read;
    }
    return {};
}

Status TcpLink::write(std::span<const std::byte> data)
{
    return impl_->write(data);
}

void TcpLink::close()
{
    if (impl_ == nullptr || impl_->closed)
    {
        return;
    }
    impl_->closed = true;
    if (impl_->established)
    {
        (void)impl_->send_segment(protocol::TcpFin | protocol::TcpAck, {});
    }
}

} // namespace ioscpp
