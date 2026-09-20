#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "ioscpp/error.hpp"
#include "ioscpp/export.hpp"
#include "ioscpp/protocol/usbmux.hpp"
#include "ioscpp/transport.hpp"

namespace ioscpp
{

/// A complete mux frame: a header plus its optional payload.
struct IOSCPP_API Frame
{
    protocol::MuxHeader header;
    std::vector<std::byte> payload;
};

/**
 * @brief A framed mux session over a transport.
 *
 * This is the layer that deals in complete frames (a header plus its payload) rather
 * than raw bytes, and that owns the mux sequence numbers. It is the host-side
 * counterpart of `usbmuxd`'s `device_data_input`:
 *
 *   https://github.com/libimobiledevice/usbmuxd/blob/master/src/device.c
 *
 * The header size depends on the negotiated mux version: a version-1 device has an
 * 8-byte header and a version-2 device a 16-byte one. Before the handshake the
 * version is unknown, so the initial version request is written with the 8-byte form,
 * which is what `usbmuxd` does (`dev->version` is 0 until the device answers).
 */
class IOSCPP_API Session
{
public:
    explicit Session(Transport &transport) noexcept;

    /// Sends a frame with `protocol` and an optional payload.
    Status send(protocol::MuxProtocol protocol, std::span<const std::byte> payload = {});

    /// Reads exactly one frame.
    ///
    /// `length` must cover the header and not exceed the protocol's maximum, or
    /// the byte stream is desynchronized. The v2 magic is read but not checked,
    /// matching `usbmuxd`, because the device's own value differs from the
    /// host's.
    Result<Frame> receive();

    /// The negotiated mux version: 0 before the handshake, then 1 or 2.
    std::uint32_t version() const noexcept
    {
        return version_;
    }

    /// Sets the negotiated version, which decides the header size from then on.
    void set_version(std::uint32_t version) noexcept
    {
        version_ = version;
    }

    /// Resets the sequence numbers, as the version-2 setup packet does. The
    /// receive sequence restarts at `0xffff`, matching `usbmuxd`.
    void reset_sequences() noexcept
    {
        tx_seq_ = 0;
        rx_seq_ = 0xffff;
    }

    /// The transport's serial, or empty when the transport has none.
    std::string_view serial() const noexcept
    {
        return transport_->serial();
    }

    /// The underlying transport.
    Transport &transport() const noexcept
    {
        return *transport_;
    }

private:
    Transport *transport_;
    std::uint32_t version_ = 0;
    std::uint16_t tx_seq_ = 0;
    std::uint16_t rx_seq_ = 0;
};

} // namespace ioscpp
