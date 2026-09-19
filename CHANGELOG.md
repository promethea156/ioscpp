# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

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

[Unreleased]: https://github.com/promethea156/ioscpp/commits/main
