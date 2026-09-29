# Replacing libusb with the native Windows USB API

## In plain terms

The USB transport is one small file behind a stable seam, so replacing libusb is mostly
mechanical. Windows, however, is the hard platform of the three: the phone boots in a USB
configuration without the mux interface, the host has to send `SET_CONFIGURATION` to select
the one that carries it, and WinUSB does not send it. The port is therefore a rewrite of one
file plus one genuinely uncertain step. This document is the Windows-only roadmap for that work.

It records the plan for the future improvement named in [`01-objective.md`](01-objective.md#usb-backend):
replace libusb with platform-native USB APIs (WinUSB on Windows, IOKit on macOS, `usbfs` on
Linux) to remove the third-party dependency and its license obligations.

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

So the port is a rewrite of that one file behind the same `UsbTransport` class; the core, the
protocols, the tests, and the examples are untouched.

## The one hard problem: selecting the configuration

A freshly plugged device starts in configuration 1, which carries only the PTP interface (class `0x06`,
subclass `0x01`, protocol `0x01`). The mux interface (`0xff`/`0xfe`/`0x02`) exists only in
configurations 3 and 4, so the host must send `SET_CONFIGURATION` before it can claim the interface
([`04-blockers.md`](04-blockers.md#the-device-starts-in-a-configuration-without-the-mux-interface),
[`09-platform-setup.md`](09-platform-setup.md#usb-driver)).

libusb's WinUSB backend **cannot send `SET_CONFIGURATION`**. In libusb's own
`windows_winusb.c`, the WinUSB sub-API intercepts the request and returns
`LIBUSB_ERROR_NOT_SUPPORTED` unless it equals the active configuration; only the `libusb0` sub-API
lets it through (`windows_winusb.c:3441-3448`). That is exactly why
[`09-platform-setup.md`](09-platform-setup.md#usb-driver) requires `libusb-win32` and not WinUSB
today. A naive native WinUSB port therefore reproduces the project's original `no device attached`
bug.

There is also a chicken-and-egg: a WinUSB handle is per interface, and the `MI_01` node may not
exist until the configuration is already switched.

## The Windows API mapping

| libusb today | Native Windows |
| --- | --- |
| `libusb_get_device_list` | `SetupDiGetClassDevs` / `SetupDiEnumDeviceInterfaces` / `SetupDiGetDeviceInterfaceDetail` on `GUID_DEVINTERFACE_USB_DEVICE` |
| `libusb_get_device_descriptor` | `WinUsb_GetDescriptor` (parse the blob) |
| `libusb_get_config_descriptor` | `WinUsb_GetDescriptor` per configuration index (parse the blob) |
| `libusb_get_string_descriptor_ascii` | `WinUsb_GetDescriptor` `USB_STRING_DESCRIPTOR_TYPE`, then UTF-16LE to UTF-8 |
| `libusb_open` / `libusb_close` | `CreateFile` / `CloseHandle` plus `WinUsb_Initialize` / `WinUsb_Free` |
| `libusb_get_active_config_descriptor` | `WinUsb_QueryInterfaceSettings` (active configuration only) |
| `libusb_set_configuration` | `WinUsb_ControlTransfer` with `SET_CONFIGURATION`, then reopen |
| `libusb_claim_interface` / `libusb_release_interface` | implicit per interface; `WinUsb_Initialize` / `WinUsb_Free` |
| `libusb_clear_halt` | `WinUsb_ResetPipe` (or `WinUsb_AbortPipe`) |
| `libusb_bulk_transfer` | `WinUsb_ReadPipe` / `WinUsb_WritePipe` with `PIPE_TRANSFER_TIMEOUT`, `AUTO_CLEAR_STALL` |
| `LIBUSB_ERROR_TIMEOUT` | `ERROR_SEM_TIMEOUT` from the pipe call |

Enumeration filters the device path on VID/PID and `MI_01`; the interface is opened by its device
path, so a sharing violation when Apple's service holds it maps to the current "busy" error.

## Roadmap

Each increment is verifiable against a real device, reusing the existing
`tests/device_test.cpp` and `tests/multi_device_test.cpp` as the acceptance test.

| # | Increment | Verify |
| --- | --- | --- |
| 0 | Split `src/usb/usb_transport.cpp` into a common core plus a backend interface (`usb_backend.hpp`); keep libusb as one backend behind `IOSCPP_USB_BACKEND`. | Windows build and the existing device test still pass on libusb |
| 1 | Enumeration: SetupAPI device-interface walk, filter VID/PID and `MI_01`, parse descriptors, convert the UTF-16 `iSerial` to UTF-8. | `list` and `is_present` return the same serial as libusb |
| 2 | Open and claim: `CreateFile` the interface path, `WinUsb_Initialize`, `WinUsb_QueryInterfaceSettings`, `WinUsb_QueryPipe` for the two bulk pipes. | `open` succeeds with Apple's service stopped |
| 3 | Transfers: `WinUsb_ReadPipe` / `WinUsb_WritePipe`, `PIPE_TRANSFER_TIMEOUT`, `AUTO_CLEAR_STALL`, `WinUsb_ResetPipe`; map `ERROR_SEM_TIMEOUT` into the existing retry and budget loop. | Device test read and write, including the TLS handshake's short reads |
| 4 | **Configuration-selection spike** (see below): fetch every configuration descriptor, locate the mux configuration, send `SET_CONFIGURATION`, reopen. | `open` on a freshly replugged, untrusted device |
| 5 | Wire the factory, default Windows to the native backend, keep `IOSCPP_USB_BACKEND=libusb` as an escape hatch; drop the libusb `FetchContent` and DLL copy on Windows. | Full device and multi-device tests on Windows |
| 6 | Docs: rewrite the Windows driver section of [`09-platform-setup.md`](09-platform-setup.md) for WinUSB, and note the LGPL obligation is gone. | — |

Increment 4 is the only genuinely uncertain step. Its fallbacks, in order:

1. **`WinUsb_ControlTransfer` plus reopen** — the most likely fix. libusb's "creates issues"
   comment is about its own cached configuration and pipes, which a reopen discards.
2. **Switch through the PTP interface** — bind WinUSB to `MI_00`, open it, send
   `SET_CONFIGURATION`, after which `MI_01` appears.
3. **Keep `libusb0.sys` and drive it directly** — the most reliable, but this is libusb-win32's
   ioctl API rather than WinUSB, and still ships a third-party kernel driver.

## What it buys, and what it does not

- **Buys:** no LGPL dynamic-link obligation on Windows, no libusb `FetchContent`, static linking
  becomes possible, and one fewer DLL beside the binaries.
- **Does not buy:** the interface still has to be bound to a non-Apple driver with Zadig — the step in
  [`09-platform-setup.md`](09-platform-setup.md#usb-driver) stays, only the driver chosen changes
  from `libusb-win32` to **WinUSB**. If fallback 3 is needed, even that stays as it is.

## Estimate

Roughly one and a half to two and a half weeks on Windows with a device in hand: three to five days
for increments 0–3 (mechanical SetupAPI and WinUSB code, about 600–900 lines), and three to five days
of uncertainty for increment 4. Linux (`usbfs`) and macOS (IOKit) are each easier because neither has the
configuration-selection trap, so doing Windows first de-risks the pattern for the others.

## Benefits and cost justification

The benefit is not a new feature: libusb already works, so the justification is the removal
of a recurring tax that [`01-objective.md`](01-objective.md#usb-backend) already names as the
goal, not a bug fix. The cost has to be argued against that tax.

### The benefit is concrete

1. **License and static linking (the strongest).** libusb is LGPL-2.1-or-later, so the static
   `ioscpp-usb` cannot statically link it and every consumer must ship `libusb-1.0.dll` and set its
   search path — the DLL copy steps in `examples/CMakeLists.txt:36-61` and `tests/CMakeLists.txt:54-85`
   and the consumer `PATH`/`LD_LIBRARY_PATH` in `.github/workflows/ci.yml:78-83`. A native backend
   is a static object with no runtime dependency, which is what "Embeddable"
   ([`01-objective.md`](01-objective.md)) needs.

2. **An inbox driver instead of a third-party one.** Today the plan needs `libusb-win32`'s
   `libusb0.sys` ([`09-platform-setup.md`](09-platform-setup.md#usb-driver)), a legacy third-party
   signed kernel driver. If increment 4 works, the driver becomes **WinUSB**, inbox since Windows 8 and
   maintained by Microsoft. That is a maintenance and security upgrade, not a rebrand.

3. **Supply chain and build.** No `FetchContent` clone of `libusb-cmake` at configure time, and one
   fewer component with a CVE history to track.

4. **Direct control.** Timeout, retry, budget, and endpoint recovery are built on libusb's semantics
   today; native maps them exactly, with no abstraction impedance.

### Where the benefit is weak

- **The Windows win can evaporate.** If `WinUsb_ControlTransfer` cannot select the configuration and
  fallback 3 is needed, `libusb0.sys` stays and benefit 2 disappears.
- **It re-earns solved bugs.** libusb encodes years of edge cases (short reads, stalls, configuration
  ordering); the device test is the only proof the native code matches.

### Why the estimate is what it is

| Phase | Days | Basis |
| --- | --- | --- |
| Shared backend split (increment 0) | 3-5 | one 600-line file, pimpl seam already exists |
| Mechanical SetupAPI and WinUSB code | 3-5 | about 600-900 lines, the mapping table is known |
| Configuration-selection spike | 2-4 | one unknown with three fallbacks |
| Device verification | 2-3 | each increment needs a device run and a trust tap |

This is a one-time cost against a permanent per-consumer tax; the estimate above is the same
breakdown.

### The decision test

Worth it if `ioscpp` is distributed to third parties, static self-contained binaries matter, or a
device and CI already exist. Not worth it if it is used only in-tree, or if fallback 3 is needed and the
Windows win shrinks to the license benefit alone.

## Where to read more

- [`01-objective.md`](01-objective.md#usb-backend) — the libusb decision and the native-API future
  improvement.
- [`04-blockers.md`](04-blockers.md) — the two blockers this plan is built around.
- [`09-platform-setup.md`](09-platform-setup.md) — the current per-platform USB access and driver steps.
- [`12-native-usb-linux.md`](12-native-usb-linux.md) — the Linux plan, which has no
  configuration-selection trap.
- [`13-native-usb-macos.md`](13-native-usb-macos.md) — the macOS plan, and its kernel-capture
  problem.
