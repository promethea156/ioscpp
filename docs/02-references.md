# References

The following external resources serve as references for understanding the iOS device protocols and their implementation. They are essential for building a client that talks to a device directly, without `usbmuxd` or `libimobiledevice`.

## The Mux Protocol

The device exposes a vendor-specific USB interface that multiplexes TCP-like connections. Apple never documented it, but `usbmuxd` implements the host side and its source is the de facto specification.

### libimobiledevice/usbmuxd

- **URL**: https://github.com/libimobiledevice/usbmuxd
- **What it is**: The reference daemon that talks to iOS devices over USB.
- **Why it matters**:
  - `src/device.c` defines the wire format: a 16-byte `mux_header` (`protocol`, `length`, `magic` `0xfeedface`, `tx_seq`, `rx_seq`), a `version_header`, and a BSD TCP header, all big-endian.
  - It documents the version negotiation (`MUX_PROTO_VERSION`), the v2 setup packet, and the TCP state machine (SYN, SYN|ACK, ACK, RST, PSH|ACK) used to open a port on the device.
  - `src/usb.c` documents the USB side: the interface is class `0xff`, subclass `0xfe`, protocol `0x02`, with a 16 KiB max read unit and a 3 × 16 KiB max transfer unit.

### libimobiledevice/libusbmuxd

- **URL**: https://github.com/libimobiledevice/libusbmuxd
- **What it is**: The client library for the `usbmuxd` daemon, and the source of the socket protocol.
- **Why it matters**: `include/usbmuxd-proto.h` defines the daemon-side plist protocol (`MESSAGE_CONNECT`, `MESSAGE_LISTEN`, `MESSAGE_PLIST`, `usbmuxd_connect_request`), which a host that talks to a running `usbmuxd` instead of the device directly would use.

## Lockdown and Pairing

### libimobiledevice/libimobiledevice

- **URL**: https://github.com/libimobiledevice/libimobiledevice
- **What it is**: A cross-platform library that speaks `lockdownd`, `AFC`, `installation_proxy`, and the other device services, through `usbmuxd`.
- **Why it matters**:
  - `src/lockdown.c` documents the pairing exchange: `QueryType`, `Pair`, `StartSession`, and `StartService`, all carried as plists with a 4-byte big-endian length prefix.
  - `src/afc.c` and `src/afc.h` document the `AFC` wire format (`CFA6LPAA` magic and the operation set) that file listing and transfer are built on.
  - `src/installation_proxy.c` and `src/process_control.c` document app installation and process control.
  - It is the closest analogue to what this library is, but it requires the `usbmuxd` daemon, while this library does not.

### libimobiledevice/libplist

- **URL**: https://github.com/libimobiledevice/libplist
- **What it is**: The property list parser and serializer that every iOS protocol uses.
- **Why it matters**: It is the reference for the XML and binary plist formats that `lockdownd`, `installation_proxy`, and process control all exchange. This library implements its own codec so it does not have to link libplist.

### libimobiledevice/libimobiledevice-glue

- **URL**: https://github.com/libimobiledevice/libimobiledevice-glue
- **What it is**: Shared helpers (base64, hex, endianness, and so on) used by the other libimobiledevice projects.
- **Why it matters**: Its base64 and endianness helpers are the reference for the plist and pairing codecs.

## Cross-platform implementations

### danielpaulus/go-ios

- **URL**: https://github.com/danielpaulus/go-ios
- **What it is**: A production-grade, cross-platform implementation of the iOS device protocols in Go, shipped as the `ios` CLI and used by Appium, headspin.io, and Sauce Labs. It has been verified against real devices, including on Windows and Linux.
- **Why it matters**:
  - It is a working, tested implementation to compare against when a call does not behave as the device expects, which is exactly the kind of second opinion a protocol reimplementation needs.
  - Its `ios` service list is the broadest map of what the device offers (`installation_proxy`, `process_control`, `AFC`, `diagnostics_relay`, `screenshotr`, `os_trace_relay`, and so on), which is what the roadmap is drawn from.
  - It documents the iOS 17+ paths this library has not reached yet: the Remote Service Discovery (`RSD`) tunnel, the `CoreDevice` services, and the userspace `tunnel` needed before any of them work.
  - It pairs without the manual trust tap, which is the behavior the pairing slice targets.
  - It is MIT-licensed and compiles statically for every host, the same self-contained goal this library has.

## Protocol Documentation

### pymobiledevice3

- **URL**: https://github.com/doronz88/pymobiledevice3
- **What it is**: A modern, pure-Python implementation of the device protocols, with clear `construct` definitions of each wire format.
- **Why it matters**:
  - It is easier to read than the C sources for the `lockdownd`, `AFC`, and `installation_proxy` message layouts.
  - It documents the pairing record fields and the service names (`com.apple.afc`, `com.apple.mobile.installation_proxy`, and so on).

### The Apple Device Protocol notes

- **URL**: https://www.theiphonewiki.com/wiki/Usbmux
- **What it is**: A community write-up of the mux protocol and the `lockdownd` service.
- **Why it matters**: It is a compact overview of the ports (`62078` for `lockdownd`), the pairing flow, and the service-start flow.

## Related Sources

For completeness, the upstream sources referenced by the resources above:

- usbmuxd USB wire format (`device.c`): https://github.com/libimobiledevice/usbmuxd/blob/master/src/device.c
- usbmuxd USB constants (`usb.h`): https://github.com/libimobiledevice/usbmuxd/blob/master/src/usb.h
- libimobiledevice lockdown client: https://github.com/libimobiledevice/libimobiledevice/blob/master/src/lockdown.c
- libimobiledevice AFC client: https://github.com/libimobiledevice/libimobiledevice/blob/master/src/afc.c
- libimobiledevice installation proxy: https://github.com/libimobiledevice/libimobiledevice/blob/master/src/installation_proxy.c
