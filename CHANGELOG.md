# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- `Device::disconnect`, an idempotent, innermost-first teardown (TLS `close_notify`,
  then the streams, the mux, and the transport), with the destructor routed through it.
- `Stream::close`, `Lockdown::close`, and `Connection::close`, the idempotent pieces
  `Device::disconnect` is built from.
- The device test disconnects, re-discovers the device by serial, and reconnects on a
  fresh transport; `IOSCPP_TEST_REPLUG` waits for a physical unplug and replug first.
- The iOS 17+ CoreDevice tunnel plan (`docs/10-coredevice-tunnel.md`): the four layers, the
  `CDTunnel` handshake and raw-IPv6 re-framer formats, the RSD/RemoteXPC surface, the testing
  plan, and the choice of a hand-rolled minimal IPv6 + TCP client over `lwIP`.
- `protocol::Json`, a minimal JSON codec for the CoreDevice tunnel handshake, with an integral
  number serialized without a fraction.
- `protocol::Cdtunnel`, the `CDTunnel` frame codec and the handshake request and response,
  tested device-free over the mock.
- `protocol::Ipv6` and `Ipv6Framer`, the fixed IPv6 header and the re-framer that turns
  the tunnel's boundary-less byte stream into whole packets, tested device-free over the mock.
- `docs/00-start-here.md`, a plain-language tour of the project with a glossary, and a
  plain-language introduction to the CoreDevice tunnel plan.

### Fixed

- `Afc::list` now reads the single `DATA` packet the device sends for `READ_DIR`,
  instead of waiting for a `STATUS` it never sends, so listing no longer hangs.
- `Afc::open_file` now sends the mode before the path and accepts the device's
  `FILE_OPEN_RES` answer, matching `libimobiledevice` and `pymobiledevice3`.
- `Afc` now chunks `FILE_READ` and `FILE_WRITE` at 32 KiB, under the device's
  65535-byte message cap, and sends the handle as the packet data for `FILE_WRITE`.
- `install` now closes its `AFC` stream before it opens `installation_proxy`, because a
  stream left open on the shared mux connection discards the other's frames.
- `install` now sends the staged package path alone, without an empty
  `ApplicationIdentifier`, and sets `PackageType` to `Developer`, matching
  `pymobiledevice3 apps install --developer`.

### Changed

- The mbedTLS TLS debug callback is now behind `IOSCPP_TRACE`, so a normal run no
  longer prints the handshake.
- The `app` process-control functions are documented as unvalidated and blocked on the
  `RSD` tunnel: they used a plist service that does not exist, and the real service is
  `DTX` (`docs/04-blockers.md`).

## [0.1.0-rc.1] - 2026-09-19

The first release. It connects to a device over USB, pairs with it, and reads its
identity; `AFC` and the app functions ship unvalidated on hardware.

### Added

- The project layout, mirroring [`adbcpp`](https://github.com/promethea156/adbcpp): `include/`,
  `src/`, `tests/`, `examples/`, `tools/`, `docs/`, and `cmake/`.
- The `Result<T>` error model, so nothing in the library throws.
- The `Transport` interface and the in-memory `testing::MockTransport`.
- The `protocol::Plist` codec, the `protocol::MuxHeader`/`protocol::TcpHeader` codec,
  and the `Session` that frames them over a transport.
- The `Connection` (mux negotiation and port connect) and the `Stream` on a port.
- The `Lockdown` client, the `Afc` client, and the `app` functions.
- `crypto::Pairing`, backed by mbedTLS.
- `usb::UsbTransport`, backed by libusb, and `tcp::TcpTransport`.
- `TcpTransport`, a socket transport to a running `usbmuxd`, with no third-party
  dependency.
- `examples/`, and the design documents under `docs/`.
- `tests/device_test.cpp`, the device integration test that skips with code `77`
  when no matching device is attached.

### Changed

- The TLS experiment knobs are retired, so the default path reads no experiment
  environment variables; `IOSCPP_TRACE` and `IOSCPP_DUMP` remain.
- The client identity is settled as the host leaf certificate, and the auth mode as
  `MBEDTLS_SSL_VERIFY_REQUIRED` with a callback that accepts the device
  certificate, matching `idevice_connection_enable_ssl`.

### Removed

- `crypto::Pairing::root_private_key()`; the root key is still in the pairing
  record.

### Fixed

- `usb::UsbTransport` now searches every USB configuration, selects the one that
  carries the mux interface, and detaches kernel drivers on Linux, matching
  `usbmuxd`, so a device in its initial USB mode is found.
- The v2 mux framing now matches `usbmuxd`: data frames are `ACK` alone, `tx_seq`
  advances for every frame, and the device's own v2 magic is not checked.
- The pairing chain writes a non-zero certificate serial, so the device stops
  resetting the TLS handshake after the ClientHello and answers with a ServerHello.
- `usb::UsbTransport` trims the serial descriptor's trailing NUL padding, so the
  pairing record is written as `<serial>.plist` and is found again on the next run.

[Unreleased]: https://github.com/promethea156/ioscpp/compare/v0.1.0-rc.1...HEAD
[0.1.0-rc.1]: https://github.com/promethea156/ioscpp/releases/tag/v0.1.0-rc.1
