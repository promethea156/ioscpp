#pragma once

#include <cstddef>
#include <memory>
#include <span>
#include <string_view>

#include "ioscpp/error.hpp"
#include "ioscpp/export.hpp"

namespace ioscpp
{

/**
 * @brief Abstract byte channel to a device.
 *
 * A transport is responsible only for moving bytes. All mux framing and protocol
 * logic lives above this interface, which keeps the protocol layer independent of
 * the underlying medium (USB, a socket to a running `usbmuxd`, mock).
 *
 * Implementations are not required to be thread-safe: concurrent use of one
 * transport must be serialized. Different transports are independent, so one
 * transport per thread is how several devices are driven at once.
 */
class IOSCPP_API Transport
{
public:
    virtual ~Transport() = default;

    Transport(const Transport &) = delete;
    Transport &operator=(const Transport &) = delete;

    /**
     * @brief Reads up to `buffer.size()` bytes into `buffer`.
     *
     * @return the number of bytes read, 0 on end of stream, or the reason the
     * link failed.
     */
    virtual Result<std::size_t> read(std::span<std::byte> buffer) = 0;

    /**
     * @brief Writes the whole of `data`.
     *
     * @return nothing, or the reason the link failed.
     */
    virtual Status write(std::span<const std::byte> data) = 0;

    /// Closes the transport, releasing any underlying resources.
    virtual void close() = 0;

    /**
     * @brief The device's USB serial, or empty when the transport has none.
     *
     * A USB device's serial is its `iSerial` string descriptor and a socket
     * transport's is its endpoint. This is the value `usb::DeviceId::serial`
     * selects on, and the default is empty for a transport, such as a mock, that
     * has no serial of its own.
     */
    virtual std::string_view serial() const noexcept
    {
        return {};
    }

    /**
     * @brief Whether this transport is a socket to a running `usbmuxd`.
     *
     * A direct link (the device's USB interface) speaks the mux framing itself,
     * one transport multiplexing every port. A socket to `usbmuxd` speaks a plist
     * handshake instead, and needs a fresh socket per port, which is what
     * @ref reopen provides. The default is false.
     */
    virtual bool is_usbmuxd() const noexcept
    {
        return false;
    }

    /**
     * @brief Opens a fresh transport to the same endpoint, for a new device port.
     *
     * Only a transport that reports @ref is_usbmuxd needs it; the default is an
     * `ErrorCode::Protocol` error.
     */
    virtual Result<std::unique_ptr<Transport>> reopen() const;

protected:
    Transport() = default;

    /// A transport is move-only, so a concrete transport can be returned by
    /// value from its factory. The move is protected because only a derived
    /// class's own move constructor calls it.
    Transport(Transport &&) = default;
    Transport &operator=(Transport &&) = default;
};

} // namespace ioscpp
