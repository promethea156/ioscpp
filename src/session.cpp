#include "ioscpp/session.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "ioscpp/error.hpp"
#include "ioscpp/protocol/usbmux.hpp"

namespace ioscpp
{
namespace
{

/// The largest frame the mux link carries.
///
/// `usbmuxd` sends up to `USB_MTU` (3 × 16 KiB) per transfer, so a frame is
/// bounded well above that to catch a desynchronized length.
constexpr std::uint32_t kMaxFrameSize = 3 * 16384;

Error protocol_error(std::string message)
{
    return Error{ErrorCode::Protocol, std::move(message)};
}

/// Reads exactly `buffer.size()` bytes, or fails.
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
            return tl::unexpected(protocol_error("the device closed the link mid-frame"));
        }
        offset += *read;
    }
    return {};
}

} // namespace

Session::Session(Transport &transport) noexcept
    : transport_(&transport)
{
}

Status Session::send(protocol::MuxProtocol protocol, std::span<const std::byte> payload)
{
    const std::size_t header_size = protocol::MuxHeader::wire_size(version_);
    if (payload.size() > kMaxFrameSize - header_size)
    {
        return tl::unexpected(Error{ErrorCode::InvalidArgument, "the payload does not fit in one mux frame"});
    }

    protocol::MuxHeader header;
    header.protocol = static_cast<std::uint32_t>(protocol);
    header.length = static_cast<std::uint32_t>(header_size + payload.size());
    header.magic = protocol::kMuxMagic;
    header.tx_seq = tx_seq_++;
    header.rx_seq = rx_seq_;

    const std::array<std::byte, protocol::kMuxHeaderSize> encoded = header.encode(version_);

    // A frame is written as one transport write, matching `usbmuxd`, which builds
    // the header and the payload into one buffer before sending.
    std::vector<std::byte> frame(encoded.begin(), encoded.begin() + static_cast<std::ptrdiff_t>(header_size));
    frame.insert(frame.end(), payload.begin(), payload.end());
    return transport_->write(frame);
}

Result<Frame> Session::receive()
{
    const std::size_t header_size = protocol::MuxHeader::wire_size(version_);

    std::array<std::byte, protocol::kMuxHeaderSize> header_bytes{};
    if (Status status = read_exact(*transport_, std::span<std::byte>(header_bytes).first(header_size)); !status)
    {
        return tl::unexpected(status.error());
    }

    const protocol::MuxHeader header =
        protocol::MuxHeader::decode(std::span<const std::byte>(header_bytes).first(header_size), version_);

    if (version_ >= 2 && header.magic != protocol::kMuxMagic)
    {
        return tl::unexpected(protocol_error("the mux magic does not match"));
    }
    if (header.length < header_size || header.length > kMaxFrameSize)
    {
        return tl::unexpected(protocol_error("the mux frame length is out of range"));
    }

    Frame frame;
    frame.header = header;
    frame.payload.resize(header.length - header_size);
    if (Status status = read_exact(*transport_, frame.payload); !status)
    {
        return tl::unexpected(status.error());
    }

    rx_seq_ = header.tx_seq;
    return frame;
}

Result<std::unique_ptr<Transport>> Transport::reopen() const
{
    return tl::unexpected(Error{ErrorCode::Protocol, "the transport does not support reopen"});
}

} // namespace ioscpp
