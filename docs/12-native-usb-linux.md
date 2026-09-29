# Replacing libusb with the native Linux USB API

## In plain terms

The USB transport is one small file behind a stable seam, so replacing libusb is mostly
mechanical. Linux is easier than Windows: `usbfs` **can** send `SET_CONFIGURATION`, so the
one trap that dominates the Windows plan does not exist here. The two things to get right are
the kernel-driver handover and keeping the dependency-free enumeration. This document is the
Linux-only roadmap for that work.

It is the Linux counterpart to [`11-native-usb-windows.md`](11-native-usb-windows.md) and records
the plan for the future improvement named in [`01-objective.md`](01-objective.md#usb-backend):
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

Only two blocks are Linux-specific today: the kernel-driver detach before `set_configuration`
(`src/usb/usb_transport.cpp:449-466`) and `libusb_set_auto_detach_kernel_driver`
(`:476-478`), both under `#if defined(__linux__)`. Both have direct `usbfs` ioctl equivalents.

## Why Linux is easier than Windows

The Windows plan's whole difficulty is that WinUSB cannot send `SET_CONFIGURATION`
([`11-native-usb-windows.md`](11-native-usb-windows.md#the-one-hard-problem-selecting-the-configuration)).
`usbfs` can: `IOCTL_USBFS_SETCONFIGURATION` sets it directly, exactly as libusb's own Linux
backend does (`linux_usbfs.c`, `op_set_configuration`). So the mux configuration is selected the
same way the current code already selects it, and no fallback or reopen dance is needed.

The one thing to get right instead is the **kernel-driver handover**. libusb checks the bound driver
with `IOCTL_USBFS_GETDRIVER` and, with auto-detach set, claims through
`IOCTL_USBFS_DISCONNECT_CLAIM` with the `USBFS_DISCONNECT_CLAIM_EXCEPT_DRIVER` flag and driver
`"usbfs"`; on a kernel that lacks that ioctl it falls back to `IOCTL_USBFS_DISCONNECT` then
`IOCTL_USBFS_CLAIMINTERFACE` (`linux_usbfs.c`, `detach_kernel_driver_and_claim`). The native
backend reproduces exactly that, and releases with `IOCTL_USBFS_RELEASEINTERFACE`.

The second thing is to keep enumeration **dependency-free**. The libusb build deliberately turns its
udev backend off so no `libudev` package is needed (`src/usb/CMakeLists.txt:5-8`). Native `sysfs`
enumeration keeps that property: read `/sys/bus/usb/devices`, and fall back to reading `/dev/bus/usb`
with `IOCTL_USBFS_CONTROL` only if `sysfs` is absent. No `libudev`, no new package.

## The Linux API mapping

| libusb today | Native Linux |
| --- | --- |
| `libusb_get_device_list` | walk `/sys/bus/usb/devices` (`busnum`, `devnum`, `idVendor`, `idProduct`); fallback `readdir` `/dev/bus/usb` |
| `libusb_get_device_descriptor` | parse the sysfs `descriptors` blob (bus-endian), or `IOCTL_USBFS_CONTROL` `GET_DESCRIPTOR` |
| `libusb_get_config_descriptor` | parse the same sysfs `descriptors` blob |
| `libusb_get_string_descriptor_ascii` | sysfs `serial`, or `IOCTL_USBFS_CONTROL` `GET_DESCRIPTOR` `STRING`, then UTF-16LE to UTF-8 |
| `libusb_open` / `libusb_close` | `open` / `close` `/dev/bus/usb/BBB/DDD` |
| `libusb_get_configuration` | sysfs `bConfigurationValue`, or `IOCTL_USBFS_CONTROL` `GET_CONFIGURATION` |
| `libusb_set_configuration` | `ioctl(USBDEVFS_SETCONFIGURATION)` |
| `libusb_kernel_driver_active` | `ioctl(USBDEVFS_GETDRIVER)` (driver != `"usbfs"`) |
| `libusb_detach_kernel_driver` | `ioctl(USBDEVFS_DISCONNECT)` |
| `libusb_claim_interface` | `ioctl(USBDEVFS_DISCONNECT_CLAIM)`; fallback `DISCONNECT` + `CLAIMINTERFACE` |
| `libusb_release_interface` | `ioctl(USBDEVFS_RELEASEINTERFACE)` |
| `libusb_clear_halt` | `ioctl(USBDEVFS_CLEAR_HALT)` |
| `libusb_bulk_transfer` | `ioctl(USBDEVFS_BULK)` (`struct usbdevfs_bulktransfer`, timeout in ms) |
| `LIBUSB_ERROR_TIMEOUT` | `ETIMEDOUT`; a stalled endpoint is `EPIPE` |

The backend includes `<linux/usbdevice_fs.h>` and `<sys/ioctl.h>`, so no library is linked at
all. Device nodes are `/dev/bus/usb/BBB/DDD` from the sysfs `busnum` and `devnum`.

## Roadmap

Each increment is verifiable against a real device, reusing the existing
`tests/device_test.cpp` and `tests/multi_device_test.cpp` as the acceptance test.

| # | Increment | Verify |
| --- | --- | --- |
| 0 | Split `src/usb/usb_transport.cpp` into a common core plus a backend interface (`usb_backend.hpp`); keep libusb as one backend behind `IOSCPP_USB_BACKEND`. | Linux build and the existing device test still pass on libusb |
| 1 | Enumeration: walk `/sys/bus/usb/devices`, filter VID/PID, locate the mux interface in any configuration, read `serial`. | `list` and `is_present` return the same serial as libusb |
| 2 | Open, descriptors, configuration: `open` the `/dev/bus/usb` node, read the `descriptors` blob, `ioctl(USBDEVFS_SETCONFIGURATION)`. | `open` succeeds with `usbmuxd` stopped |
| 3 | Driver handover: `USBDEVFS_GETDRIVER` check, `USBDEVFS_DISCONNECT_CLAIM` with the `DISCONNECT` + `CLAIMINTERFACE` fallback, `USBDEVFS_RELEASEINTERFACE`, `USBDEVFS_CLEAR_HALT`. | Device test claims the interface with a kernel driver bound |
| 4 | Transfers: `USBDEVFS_BULK` for read and write, map `ETIMEDOUT` into the existing retry and budget loop, `EPIPE` to a clear-halt. | Device test read and write, including the TLS handshake's short reads |
| 5 | Wire the factory, default Linux to the native backend, drop the libusb `FetchContent` and DLL copy; keep the `usb-1.0` export while Windows or macOS is still on libusb. | Full device and multi-device tests on Linux, and the Ubuntu CI job |
| 6 | Docs: update the Linux section of [`09-platform-setup.md`](09-platform-setup.md); note the udev rule is unchanged and no package is added. | — |

Increment 3 is the only step with real nuance. Its fallback, in order:

1. **`USBDEVFS_DISCONNECT` then `USBDEVFS_CLAIMINTERFACE`** — for kernels without
   `USBDEVFS_DISCONNECT_CLAIM` (before Linux 3.5), matching libusb's own fallback.
2. **Skip the detach entirely** — when `USBDEVFS_GETDRIVER` reports no driver or `"usbfs"`,
   claim directly, as the current code does when `kernel_driver_active` is false.

## What it buys, and what it does not

- **Buys:** no LGPL dynamic-link obligation on Linux, no libusb `FetchContent`, no `libudev`,
  static linking becomes possible, and no extra DLL or shared object beside the binaries.
- **Does not buy:** the udev rule that grants access to `/dev/bus/usb`
  ([`09-platform-setup.md`](09-platform-setup.md#usb-access)) is still required, and
  `usbmuxd` must still be stopped. Neither changes.

## Estimate

Roughly **three to five days** on Linux with a device in hand: increments 1, 2, and 4 are
mechanical sysfs and ioctl code (about 400–600 lines), and increment 3 is the only part needing
care. Linux has no configuration-selection trap, so it is shorter than the Windows plan
([`11-native-usb-windows.md`](11-native-usb-windows.md#estimate)).

## Interaction with the Windows plan

- **Increment 0 is shared.** Whichever platform is done first defines `usb_backend.hpp`; the other
  adopts it. Do not build two backend interfaces.
- **The `usb-1.0` export and the `FetchContent` stay** until every supported platform is native.
  Dropping them on Linux alone would break a `find_package(ioscpp)` consumer whose target set still
  expects `usb-1.0`. `IOSCPP_USB_BACKEND` should default per platform, not globally.
- **CI is the safety net.** The `ubuntu-latest` matrix job (`.github/workflows/ci.yml:30`) builds and
  tests Linux, so a leak in the shared increment fails there rather than at a user's site.

## Benefits and cost justification

The benefit is not a new feature: libusb already works, so the justification is the removal
of a recurring tax that [`01-objective.md`](01-objective.md#usb-backend) already names as the
goal, not a bug fix. The cost has to be argued against that tax.

### The benefit is concrete

1. **License and static linking (the strongest).** libusb is LGPL-2.1-or-later, so the static
   `ioscpp-usb` cannot statically link it and every consumer must ship `libusb-1.0.so` and set its
   search path — the shared-object copy in the build and the consumer `LD_LIBRARY_PATH` in
   `.github/workflows/ci.yml:78-83`. A native backend is a static object with no runtime
   dependency, which is what "Embeddable" ([`01-objective.md`](01-objective.md)) needs.

2. **No library at all.** `usbfs` is syscalls and ioctls, so the shared object disappears
   entirely, the deliberate no-`libudev` property is kept (`src/usb/CMakeLists.txt:5-8`), and the
   per-enumeration libusb event-monitor thread is gone.

3. **Supply chain and build.** No `FetchContent` clone of `libusb-cmake` at configure time, and one
   fewer component with a CVE history to track.

4. **Direct control.** Timeout, retry, budget, and endpoint recovery are built on libusb's semantics
   today; native maps them exactly, with no abstraction impedance.

### Where the benefit is weak

- **The benefit is mostly non-functional.** libusb on Linux already needs no driver install, so nothing
  in the setup gets easier — only a shared object and a thread are removed.
- **It re-earns solved bugs.** libusb encodes years of edge cases (atomic detach-and-claim, short
  reads, stalls); the device test is the only proof the native code matches.

### Why the estimate is what it is

| Phase | Days | Basis |
| --- | --- | --- |
| Shared backend split (increment 0) | 3-5 | one 600-line file, pimpl seam already exists |
| Mechanical sysfs and ioctl code | 2-3 | about 400-600 lines, the mapping table is known |
| Driver-handover nuance | 1 | one ioctl with a kernel-version fallback |
| Device verification | 2-3 | each increment needs a device run |

This is a one-time cost against a permanent per-consumer tax; the estimate above is the same
breakdown.

### The decision test

Worth it if `ioscpp` is distributed to third parties, static self-contained binaries matter, or a
device and CI already exist. Not worth it if it is used only in-tree, or if macOS keeps libusb and the
dependency therefore remains for one platform anyway.

## Where to read more

- [`11-native-usb-windows.md`](11-native-usb-windows.md) — the Windows plan, and the harder
  configuration-selection problem.
- [`13-native-usb-macos.md`](13-native-usb-macos.md) — the macOS plan, and its kernel-capture
  problem.
- [`01-objective.md`](01-objective.md#usb-backend) — the libusb decision and the native-API future
  improvement.
- [`09-platform-setup.md`](09-platform-setup.md) — the current Linux USB access and driver steps.
