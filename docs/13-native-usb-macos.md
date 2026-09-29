# Replacing libusb with the native macOS USB API

## In plain terms

The USB transport is one small file behind a stable seam, so replacing libusb is mostly
mechanical. macOS can select the configuration through IOKit, so the Windows trap does not
exist here either. What macOS has instead is a capture problem: taking the interface from the
kernel driver needs an entitlement or root, and doing so re-enumerates the device. macOS is
also the least-proven platform today, so the first step is to confirm the libusb path works on a
real Mac before porting anything. This document is the macOS-only roadmap.

It is the third of the trio, after [`11-native-usb-windows.md`](11-native-usb-windows.md) and
[`12-native-usb-linux.md`](12-native-usb-linux.md), and records the plan for the future
improvement named in [`01-objective.md`](01-objective.md#usb-backend): replace libusb with
platform-native USB APIs (WinUSB on Windows, IOKit on macOS, `usbfs` on Linux) to remove the
third-party dependency and its license obligations.

## Why the port is small

libusb is confined to a single 600-line translation unit, `src/usb/usb_transport.cpp`, behind a
pimpl (`UsbTransport::Impl`, `src/usb/usb_transport.cpp:322`). The public header
`include/ioscpp/usb/usb_transport.hpp` never includes `<libusb.h>`, and `Transport`
(`include/ioscpp/transport.hpp:25`) is the seam the rest of the library uses. The whole surface
that any backend must provide is:

- enumerate by VID/PID, read device, configuration, interface, and endpoint descriptors, read the
  `iSerial` string;
- open and close the device, select the configuration, claim and release the interface, clear a halt;
- **bulk read and write only** — no asynchronous transfers, no hotplug, no control transfers, no
  isochronous (`src/usb/usb_transport.cpp:343`, `:520`, `:553`).

The whole USB layer has no `#if defined(__APPLE__)` branch at all today
([`08-assumptions.md`](08-assumptions.md#libusb-behaves-the-same-on-all-three-platforms)), so the
native backend is new code with no existing Apple block to preserve.

## macOS is the least-proven platform

[`08-assumptions.md`](08-assumptions.md#libusb-behaves-the-same-on-all-three-platforms) records
that the libusb path is proven on Windows and Linux, and macOS is **still only compiled**. So the
first increment is not a port at all: it is a device run that confirms the current libusb backend
reaches a device on macOS, which becomes the baseline the native backend must match.

## The two things to get right

1. **Capturing the interface from the kernel driver.** libusb's `darwin_detach_kernel_driver` needs
   either the `com.apple.vm.device-access` entitlement or root, and detaching calls
   `USBDeviceReEnumerate` (`darwin_usb.c:2902-2944`, `:2188`). The native backend reproduces
   that capture path, and [`09-platform-setup.md`](09-platform-setup.md#macos) already requires
   stopping the system `usbmuxd` first. The entitlement is the macOS analogue of the Linux driver
   detach, and unlike Linux it is not a plain ioctl.

2. **`SetConfiguration` invalidates the interface.** libusb releases every claimed interface before
   `SetConfiguration` and re-claims them after (`darwin_usb.c:1700-1713`). The native backend keeps
   that order: release, set the configuration, re-claim. Skipping it leaves stale interface handles.

A useful simplification: libusb runs a hotplug runloop and uses the asynchronous
`ReadPipeAsyncTO`/`WritePipeAsyncTO` (`darwin_usb.c:2382-2386`). Because the transport is
synchronous and ignores hotplug, the native backend can use the synchronous `ReadPipeTO`/`WritePipeTO`
and skip the runloop and the `CreateInterfaceAsyncEventSource` entirely.

## The macOS API mapping

| libusb today | Native macOS |
| --- | --- |
| `libusb_get_device_list` | `IOServiceMatching("IOUSBDevice")` / `IOServiceGetMatchingServices` / `IOCreatePlugInInterfaceForService` |
| `libusb_get_device_descriptor` | `DeviceRequestTO` `GET_DESCRIPTOR`, or the registry `idVendor`/`idProduct` |
| `libusb_get_config_descriptor` | `GetConfigurationDescriptorPtr` |
| `libusb_get_string_descriptor_ascii` | `IORegistryEntryCreateCFProperty(kUSBSerialNumberString)`, then CFString to UTF-8 |
| `libusb_open` / `libusb_close` | `USBDeviceOpenSeize` / `USBDeviceClose` |
| `libusb_get_configuration` | `GetConfiguration` |
| `libusb_set_configuration` | `SetConfiguration`, releasing then re-claiming interfaces |
| `libusb_kernel_driver_active` | registry child check on the interface service (`IORegistryEntryGetChildEntry`) |
| `libusb_detach_kernel_driver` | `IOServiceAuthorize(kIOServiceInteractionAllowed)` + `USBDeviceReEnumerate` |
| `libusb_claim_interface` | `USBInterfaceOpen` (or `USBInterfaceOpenSeize`) |
| `libusb_release_interface` | `USBInterfaceClose` |
| `libusb_clear_halt` | `ClearPipeStallBothEnds` |
| `libusb_bulk_transfer` | `ReadPipeTO` / `WritePipeTO` (timeout) |
| `LIBUSB_ERROR_TIMEOUT` | `kIOUSBTransactionTimeout`; a stalled pipe is `kIOUSBPipeStalled` |

The backend links `-framework IOKit -framework CoreFoundation`, so no library is fetched.

## Roadmap

Each increment is verifiable against a real Mac and a device, reusing the existing
`tests/device_test.cpp` and `tests/multi_device_test.cpp` as the acceptance test.

| # | Increment | Verify |
| --- | --- | --- |
| 0 | Baseline: run the current libusb backend on a real Mac against a device. | Device test passes on macOS on libusb |
| 1 | Split `src/usb/usb_transport.cpp` into a common core plus a backend interface (`usb_backend.hpp`); keep libusb as one backend behind `IOSCPP_USB_BACKEND`. | macOS build and the existing device test still pass on libusb |
| 2 | Enumeration: `IOServiceMatching` walk, filter VID/PID, locate the mux interface in any configuration, read the serial registry property. | `list` and `is_present` return the same serial as libusb |
| 3 | Open, descriptors, configuration: `USBDeviceOpenSeize`, `DeviceRequestTO` and `GetConfigurationDescriptorPtr`, `SetConfiguration`. | `open` succeeds with the system `usbmuxd` stopped |
| 4 | Driver capture: `IOServiceAuthorize` plus `USBDeviceReEnumerate` behind the entitlement, release/re-claim around `SetConfiguration`, `USBInterfaceOpen`, `USBInterfaceClose`, `ClearPipeStallBothEnds`. | Device test claims the interface with Apple's driver attached |
| 5 | Transfers: `ReadPipeTO` / `WritePipeTO`, map `kIOUSBTransactionTimeout` into the existing retry and budget loop. | Device test read and write, including the TLS handshake's short reads |
| 6 | Wire the factory, default macOS to the native backend, drop the libusb `FetchContent`; keep the `usb-1.0` export while Windows is still on libusb. | Full device and multi-device tests on macOS, and the `macos-latest` CI job |
| 7 | Docs: update the macOS section of [`09-platform-setup.md`](09-platform-setup.md); note the `launchctl` step is unchanged and the entitlement is new. | — |

Increment 4 is the only step with real uncertainty, and increment 0 is the only step that
can fail for reasons unrelated to the port. Its fallbacks, in order:

1. **`USBInterfaceOpenSeize`** — libusb uses plain `USBInterfaceOpen` (`darwin_usb.c:1911`);
   the seize variant forces the claim when another client holds it.
2. **Run with `sudo`** — the `IOServiceAuthorize` path falls back to root when the
   `com.apple.vm.device-access` entitlement is absent (`darwin_usb.c:2930-2933`).

## What it buys, and what it does not

- **Buys:** no LGPL dynamic-link obligation on macOS, no libusb `FetchContent`, static
  linking becomes possible, and no shared library beside the binaries.
- **Does not buy:** the system `usbmuxd` must still be stopped
  ([`09-platform-setup.md`](09-platform-setup.md#macos)), and the new capture path may need
  either the `com.apple.vm.device-access` entitlement in a signed binary or root. Neither the
  daemon step nor the signing story is removed.

## Estimate

Roughly **one to two weeks** on macOS with a device, the most of the three because it starts
from unproven: increment 0 is a device run, increment 4 needs the capture path and possibly an
entitlement and a signed test binary, and the rest is mechanical IOKit code. It is longer than the
Linux plan ([`12-native-usb-linux.md`](12-native-usb-linux.md#estimate)) and comparable to the
Windows plan ([`11-native-usb-windows.md`](11-native-usb-windows.md#estimate)).

## Interaction with the other plans

- **Increment 1 is shared with both other plans.** Whichever platform is done first defines
  `usb_backend.hpp`; the others adopt it. Do not build three backend interfaces.
- **The `usb-1.0` export and the `FetchContent` stay** until every supported platform is
  native. `IOSCPP_USB_BACKEND` should default per platform, not globally.
- **CI is the safety net.** The `macos-latest` matrix job (`.github/workflows/ci.yml:30`) builds
  and tests macOS, so a leak in the shared increment fails there rather than at a user's site.

## Where to read more

- [`11-native-usb-windows.md`](11-native-usb-windows.md) — the Windows plan, and the harder
  configuration-selection problem.
- [`12-native-usb-linux.md`](12-native-usb-linux.md) — the Linux plan, which has no
  configuration-selection trap.
- [`08-assumptions.md`](08-assumptions.md#libusb-behaves-the-same-on-all-three-platforms) — why
  macOS is the least-proven platform.
- [`09-platform-setup.md`](09-platform-setup.md) — the current macOS daemon step.
