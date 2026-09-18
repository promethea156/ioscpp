#include "ioscpp/stream.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "ioscpp/connection.hpp"
#include "ioscpp/error.hpp"
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

/// The largest payload one mux frame carries, matching `usbmuxd`'s `USB_MTU`.
constexpr std::size_t kMaxPayload = 3 * 16384 - protocol::kMuxHeaderSize - protocol::kTcpHeaderSize;

void put_le16(std::span<std::byte> bytes, std::size_t offset, std::uint16_t value) noexcept
{
    bytes[offset + 0] = static_cast<std::byte>(value & 0xff);
    bytes[offset + 1] = static_cast<std::byte>((value >> 8) & 0xff);
}

void put_le32(std::span<std::byte> bytes, std::size_t offset, std::uint32_t value) noexcept
{
    bytes[offset + 0] = static_cast<std::byte>(value & 0xff);
    bytes[offset + 1] = static_cast<std::byte>((value >> 8) & 0xff);
    bytes[offset + 2] = static_cast<std::byte>((value >> 16) & 0xff);
    bytes[offset + 3] = static_cast<std::byte>((value >> 24) & 0xff);
}

std::uint32_t get_le32(std::span<const std::byte> bytes, std::size_t offset) noexcept
{
    return static_cast<std::uint32_t>(bytes[offset + 0]) | (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
           (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

Status read_exact(Transport &transport, std::span<std::byte> buffer)
{
    std::size_t offset = 0;
    while (offset < buffer.size())
    {
        auto read = transport.read(buffer.subspan(offset));
        if (!read)
        {
            return tl::unexpected(read.error());
        }
        if (*read == 0)
        {
            return tl::unexpected(protocol_error("the device closed the port mid-message"));
        }
        offset += *read;
    }
    return {};
}

constexpr std::uint32_t kConnect = 2;
constexpr std::uint32_t kResult = 1;
constexpr std::uint32_t kResultOk = 0;

} // namespace

Stream::Stream(std::shared_ptr<Connection> connection, std::uint16_t local_port, std::uint16_t remote_port)
    : connection_(std::move(connection))
    , local_port_(local_port)
    , remote_port_(remote_port)
{
}

Result<Stream> Stream::open(std::shared_ptr<Connection> connection, std::uint16_t port)
{
    if (connection->is_usbmuxd())
    {
        auto transport = connection->open_transport_();
        if (!transport)
        {
            return tl::unexpected(transport.error());
        }

        // The daemon expects a `Connect` message: a length prefix, a header, the
        // device id, the port in little-endian, and a reserved word.
        std::vector<std::byte> message(16 + 8);
        put_le32(message, 0, static_cast<std::uint32_t>(message.size()));
        put_le32(message, 4, 1); // plist version
        put_le32(message, 8, kConnect);
        put_le32(message, 12, 1); // tag
        put_le32(message, 16, connection->device_id_);
        put_le16(message, 20, port);
        put_le16(message, 22, 0);

        if (Status status = (*transport)->write(message); !status)
        {
            return tl::unexpected(status.error());
        }

        std::array<std::byte, 4> length_bytes{};
        if (Status status = read_exact(**transport, length_bytes); !status)
        {
            return tl::unexpected(status.error());
        }
        const std::uint32_t length = get_le32(length_bytes, 0);
        if (length < 16)
        {
            return tl::unexpected(protocol_error("the usbmuxd connect answer is too short"));
        }
        std::vector<std::byte> answer(length - 4);
        if (Status status = read_exact(**transport, answer); !status)
        {
            return tl::unexpected(status.error());
        }
        if (get_le32(answer, 4) != kResult || get_le32(answer, 12) != kResultOk)
        {
            return tl::unexpected(Error{ErrorCode::Device, "usbmuxd refused the port"});
        }

        Stream stream(connection, 0, port);
        stream.usbmuxd_ = true;
        stream.owned_transport_ = std::move(*transport);
        return stream;
    }

    // The direct link: SYN / SYN|ACK / ACK.
    const std::uint16_t local_port = connection->allocate_local_port();

    protocol::TcpHeader syn;
    syn.source_port = local_port;
    syn.destination_port = port;
    syn.sequence = 0;
    syn.acknowledgement = 0;
    syn.flags = protocol::TcpSyn;
    syn.window = 131072 >> 8;
    if (Status status = connection->send_tcp(syn); !status)
    {
        return tl::unexpected(status.error());
    }

    for (;;)
    {
        auto frame = connection->receive();
        if (!frame)
        {
            return tl::unexpected(frame.error());
        }
        if (frame->header.protocol != static_cast<std::uint32_t>(protocol::MuxProtocol::Tcp) ||
            frame->payload.size() < protocol::kTcpHeaderSize)
        {
            continue;
        }

        const protocol::TcpHeader tcp = protocol::TcpHeader::decode(
            std::span<const std::byte, protocol::kTcpHeaderSize>(frame->payload.data(), protocol::kTcpHeaderSize));
        if (tcp.destination_port != local_port || tcp.source_port != port)
        {
            continue;
        }

        if ((tcp.flags & protocol::TcpRst) != 0)
        {
            return tl::unexpected(Error{ErrorCode::Device, "the device refused the port"});
        }
        if (tcp.flags != (protocol::TcpSyn | protocol::TcpAck))
        {
            return tl::unexpected(protocol_error("the device did not answer the SYN with a SYN|ACK"));
        }

        Stream stream(connection, local_port, port);
        stream.tx_seq_ = 1;
        stream.tx_ack_ = tcp.sequence + 1;

        protocol::TcpHeader ack;
        ack.source_port = local_port;
        ack.destination_port = port;
        ack.sequence = stream.tx_seq_;
        ack.acknowledgement = stream.tx_ack_;
        ack.flags = protocol::TcpAck;
        if (Status status = connection->send_tcp(ack); !status)
        {
            return tl::unexpected(status.error());
        }
        return stream;
    }
}

Stream::Stream(Stream &&other) noexcept
    : connection_(std::move(other.connection_))
    , local_port_(other.local_port_)
    , remote_port_(other.remote_port_)
    , tx_seq_(other.tx_seq_)
    , tx_ack_(other.tx_ack_)
    , closed_(other.closed_)
    , usbmuxd_(other.usbmuxd_)
    , owned_transport_(std::move(other.owned_transport_))
    , incoming_(std::move(other.incoming_))
    , incoming_offset_(other.incoming_offset_)
{
    other.closed_ = true;
}

Stream &Stream::operator=(Stream &&other) noexcept
{
    if (this != &other)
    {
        close_now();
        connection_ = std::move(other.connection_);
        local_port_ = other.local_port_;
        remote_port_ = other.remote_port_;
        tx_seq_ = other.tx_seq_;
        tx_ack_ = other.tx_ack_;
        closed_ = other.closed_;
        usbmuxd_ = other.usbmuxd_;
        owned_transport_ = std::move(other.owned_transport_);
        incoming_ = std::move(other.incoming_);
        incoming_offset_ = other.incoming_offset_;
        other.closed_ = true;
    }
    return *this;
}

Stream::~Stream()
{
    close_now();
}

void Stream::close_now() noexcept
{
    if (closed_ || connection_ == nullptr)
    {
        return;
    }
    closed_ = true;

    if (usbmuxd_)
    {
        if (owned_transport_ != nullptr)
        {
            owned_transport_->close();
        }
        return;
    }

    protocol::TcpHeader rst;
    rst.source_port = local_port_;
    rst.destination_port = remote_port_;
    rst.sequence = tx_seq_;
    rst.acknowledgement = tx_ack_;
    rst.flags = protocol::TcpRst;
    (void)connection_->send_tcp(rst);
}

Status Stream::write(std::span<const std::byte> data)
{
    if (usbmuxd_)
    {
        return owned_transport_->write(data);
    }

    std::size_t offset = 0;
    while (offset < data.size())
    {
        const std::size_t chunk = std::min(kMaxPayload, data.size() - offset);
        protocol::TcpHeader header;
        header.source_port = local_port_;
        header.destination_port = remote_port_;
        header.sequence = tx_seq_;
        header.acknowledgement = tx_ack_;
        header.flags = protocol::TcpPsh | protocol::TcpAck;
        if (Status status = connection_->send_tcp(header, data.subspan(offset, chunk)); !status)
        {
            return tl::unexpected(status.error());
        }
        tx_seq_ += static_cast<std::uint32_t>(chunk);
        offset += chunk;
    }
    return {};
}

Result<bool> Stream::receive_more()
{
    if (usbmuxd_)
    {
        std::array<std::byte, 16384> buffer{};
        auto read = owned_transport_->read(buffer);
        if (!read)
        {
            return tl::unexpected(read.error());
        }
        if (*read == 0)
        {
            return false;
        }
        incoming_.insert(incoming_.end(), buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(*read));
        return true;
    }

    for (;;)
    {
        auto frame = connection_->receive();
        if (!frame)
        {
            return tl::unexpected(frame.error());
        }
        if (frame->header.protocol != static_cast<std::uint32_t>(protocol::MuxProtocol::Tcp) ||
            frame->payload.size() < protocol::kTcpHeaderSize)
        {
            continue;
        }

        const protocol::TcpHeader tcp = protocol::TcpHeader::decode(
            std::span<const std::byte, protocol::kTcpHeaderSize>(frame->payload.data(), protocol::kTcpHeaderSize));
        if (tcp.destination_port != local_port_ || tcp.source_port != remote_port_)
        {
            continue;
        }

        if ((tcp.flags & protocol::TcpRst) != 0)
        {
            closed_ = true;
            return false;
        }

        const std::span<const std::byte> payload =
            std::span<const std::byte>(frame->payload).subspan(protocol::kTcpHeaderSize);

        tx_ack_ = tcp.sequence + static_cast<std::uint32_t>(payload.size());

        protocol::TcpHeader ack;
        ack.source_port = local_port_;
        ack.destination_port = remote_port_;
        ack.sequence = tx_seq_;
        ack.acknowledgement = tx_ack_;
        ack.flags = protocol::TcpAck;
        if (Status status = connection_->send_tcp(ack); !status)
        {
            return tl::unexpected(status.error());
        }

        if (!payload.empty())
        {
            incoming_.insert(incoming_.end(), payload.begin(), payload.end());
            return true;
        }
    }
}

Status Stream::read(std::span<std::byte> buffer)
{
    std::size_t offset = 0;
    while (offset < buffer.size())
    {
        if (incoming_offset_ == incoming_.size())
        {
            incoming_.clear();
            incoming_offset_ = 0;
            auto more = receive_more();
            if (!more)
            {
                return tl::unexpected(more.error());
            }
            if (!*more)
            {
                return tl::unexpected(
                    Error{ErrorCode::Protocol, "the device reset the port before the read was satisfied"});
            }
        }

        const std::size_t available = incoming_.size() - incoming_offset_;
        const std::size_t count = std::min(available, buffer.size() - offset);
        std::copy_n(incoming_.begin() + static_cast<std::ptrdiff_t>(incoming_offset_),
                    static_cast<std::ptrdiff_t>(count), buffer.begin() + static_cast<std::ptrdiff_t>(offset));
        incoming_offset_ += count;
        offset += count;
    }
    return {};
}

Result<std::vector<std::byte>> Stream::read_all()
{
    std::vector<std::byte> output;
    while (true)
    {
        if (incoming_offset_ < incoming_.size())
        {
            output.insert(output.end(), incoming_.begin() + static_cast<std::ptrdiff_t>(incoming_offset_),
                          incoming_.end());
            incoming_offset_ = incoming_.size();
            continue;
        }
        incoming_.clear();
        incoming_offset_ = 0;
        auto more = receive_more();
        if (!more)
        {
            return tl::unexpected(more.error());
        }
        if (!*more)
        {
            return output;
        }
    }
}

} // namespace ioscpp
