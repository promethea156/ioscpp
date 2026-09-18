#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ioscpp/error.hpp"
#include "ioscpp/export.hpp"
#include "ioscpp/transport.hpp"

namespace ioscpp::usb
{

/// Identifies an iOS USB device, by USB serial and optionally by product id.
///
/// A device in normal mode exposes the mux interface with the same USB ids
/// regardless of model, so the USB serial is how two attached devices are told
/// apart. The serial is the USB `iSerial` descriptor string, the same value
/// `idevice_id -l` prints.
///
/// A default-constructed id matches the first device, and one with only `serial`
/// set matches by serial alone, so a caller does not need to know the product id of
/// a device whose serial they have.
struct IOSCPP_API DeviceId
{
    /// The USB vendor id, or zero to match any vendor. Apple's is `0x05ac`.
    std::uint16_t vendor_id = 0x05ac;
    /// The USB product id, or zero to match any product.
    std::uint16_t product_id = 0;
    /// The USB serial number, or empty to match the first matching device.
    std::string serial;

    /// Parses a selector: `VID:PID` (hex) or `serial:<serial>`.
    static Result<DeviceId> parse(std::string_view text);
};

/**
 * @brief A Transport over the device's vendor-specific USB interface.
 *
 * Opens the device matching the given @ref DeviceId, by its serial when one is
 * set, and claims the mux interface: class `0xFF`, subclass `0xFE`, protocol
 * `0x02`. That triple is how `usbmuxd` finds the mux function:
 *
 *   https://github.com/libimobiledevice/usbmuxd/blob/master/src/usb.h
 *
 * A read returns the contents of one USB transfer, which may be a partial frame,
 * and the session reads on until it has a whole frame.
 *
 * @note libusb is linked dynamically. This type is only available when `ioscpp`
 * is built with `IOSCPP_BUILD_USB=ON`.
 */
class IOSCPP_API UsbTransport : public Transport
{
public:
    /// The default timeout for each bulk transfer, in milliseconds.
    static constexpr unsigned int kDefaultTransferTimeoutMs = 5000;

    /// The default total a single transfer waits before it gives up, in milliseconds.
    static constexpr unsigned int kDefaultTransferBudgetMs = 120000;

    /// Whether a USB device matching `id` is currently present.
    static Result<bool> is_present(DeviceId id);

    /**
     * @brief Returns every attached device, each with its USB serial filled in.
     *
     * This is how a caller discovers the serial to pass to @ref open. The list is
     * in libusb's enumeration order, which is stable for one attached set.
     */
    static Result<std::vector<DeviceId>> list();

    /**
     * @brief Opens the USB device matching `id` and claims the mux interface.
     *
     * Opening can fail, and a constructor cannot report that, so this is a named
     * factory and the constructor is private.
     */
    static Result<UsbTransport> open(DeviceId id = {}, unsigned int transfer_timeout_ms = kDefaultTransferTimeoutMs,
                                     unsigned int transfer_budget_ms = kDefaultTransferBudgetMs);

    ~UsbTransport() override;

    UsbTransport(const UsbTransport &) = delete;
    UsbTransport &operator=(const UsbTransport &) = delete;

    /// A transport is returned by value, so it moves.
    UsbTransport(UsbTransport &&) noexcept;
    UsbTransport &operator=(UsbTransport &&) noexcept;

    /// Sets the timeout applied to each later bulk transfer, in milliseconds.
    void set_transfer_timeout(unsigned int milliseconds) noexcept;

    /// The timeout applied to each bulk transfer, in milliseconds.
    unsigned int transfer_timeout() const noexcept;

    /**
     * @brief Sets the total a read waits across retries before it gives up, in
     * milliseconds.
     *
     * A single transfer may time out having moved nothing, which is retried while
     * the budget lasts, so pairing can wait for the trust prompt while a normal
     * operation fails fast.
     */
    void set_transfer_budget(unsigned int milliseconds) noexcept;

    /// The total a read waits across retries before it gives up, in milliseconds.
    unsigned int transfer_budget() const noexcept;

    Result<std::size_t> read(std::span<std::byte> buffer) override;
    Status write(std::span<const std::byte> data) override;
    void close() override;

    /// The opened device's USB `iSerial` descriptor, or empty when it has none.
    std::string_view serial() const noexcept override;

private:
    // Opening is done by `open`, so the constructor is private.
    UsbTransport();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ioscpp::usb
