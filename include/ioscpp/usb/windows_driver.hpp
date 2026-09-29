#pragma once

#include <string>
#include <vector>

#include "ioscpp/error.hpp"
#include "ioscpp/export.hpp"

#if defined(_WIN32)

/**
 * @file
 * @brief Binds a libusb-compatible driver to the device, without Zadig.
 *
 * On Windows a device is bound to Apple's `usbaapl64` driver, which libusb
 * cannot open, and libusb's WinUSB backend cannot send the `SET_CONFIGURATION`
 * the device's initial USB mode needs. The user therefore has to bind a
 * libusb-compatible driver to the device before libusb can reach it.
 *
 * Zadig does that by hand through a GUI. These functions do the same four steps in
 * code, so the step can move into a program:
 *
 *  1. @ref driver_targets finds the device node and the driver already on it;
 *  2. the INF is written from the template a working package uses;
 *  3. `makecat` and `signtool` build and sign the package's catalog;
 *  4. `SetupCopyOEMInf` and `UpdateDriverForPlugAndPlayDevices` install it.
 *
 * Installing a kernel driver needs an elevated process, so a caller that is not
 * elevated gets a failure rather than a silent no-op.
 *
 * @note These functions are Windows-only, so a cross-platform caller guards a call
 * with `#if defined(_WIN32)`.
 */

namespace ioscpp::usb
{

/**
 * @brief A present device node the driver can be bound to.
 *
 * On Windows the mux interface is owned by Apple's `usbaapl64` driver, which
 * libusb cannot open, so a libusb-compatible driver has to be bound to the node.
 * This is the programmatic form of the step Zadig does by hand.
 */
struct IOSCPP_API DriverTarget
{
    /// The device instance id, the node's `InstanceId`.
    std::string instance_id;
    /// The hardware id the generated INF matches, for example
    /// `USB\VID_05AC&PID_12A8&MI_01`.
    std::string hardware_id;
    /// The service name of the driver currently on the node, or empty.
    std::string service;

    /// Whether the node is the mux interface rather than the whole device.
    bool is_interface() const;
};

/// How the driver package is built, signed, and installed.
struct IOSCPP_API DriverOptions
{
    /// The libusb-win32 package directory, or empty to use the copy a previous
    /// install left under `System32`.
    std::string package;
    /// The USB configuration the driver selects on start, which carries the mux
    /// interface. A device starts in a configuration that does not.
    unsigned int config_value = 3;
    /// Whether to rebind a node that is already on libusb-win32.
    bool force = false;
    /// Whether to sign the package with a self-signed certificate.
    bool sign = true;
};

/**
 * @brief Every present Apple device node, the mux interface before the whole
 * device.
 *
 * A device in configuration 1 carries only the PTP interface, so there is no
 * `&MI_01` node and the composite node is returned instead. The mux interface is
 * preferred because binding it leaves the rest of the device on Apple's driver.
 */
IOSCPP_API Result<std::vector<DriverTarget>> driver_targets();

/**
 * @brief Builds, signs, and installs the driver package for `target`.
 *
 * The INF is generated for `target`'s hardware id, so the bind is scoped to that
 * node alone. Installing a kernel driver needs an elevated process; without one
 * the result is a failure with `ErrorCode::Io`.
 */
IOSCPP_API Status install_driver(const DriverTarget &target, const DriverOptions &options = {});

/// Removes every libusb-win32 package from the driver store.
IOSCPP_API Status uninstall_driver();

} // namespace ioscpp::usb

#endif
