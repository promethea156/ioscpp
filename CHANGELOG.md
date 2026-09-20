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
- `Device::tunnel`, the iOS 17.4+ `CoreDeviceProxy` handshake, returning the RSD address,
  port, and MTU, with `protocol::Cdtunnel`'s header-length helper; the device test opens
  the tunnel and is skipped on an older device.
- `TcpLink`, a userspace TCP client over the tunnel: it re-frames the tunnel's IPv6 packets
  with `protocol::Ipv6Framer`, runs the SYN / SYN|ACK / ACK exchange to an RSD port, and
  moves the connection's bytes. Tested device-free against a scripted IPv6/TCP peer, and the
  device test reaches the RSD port on a real device.
- `protocol::RemoteXpc`, the RemoteXPC codec the RSD layer rides on: `XpcWrapper` (the magic,
  the exact flags word, the body length, and the message id), `XpcPayload` (the payload magic and
  version), and `Xpc`, the `xpc` object codec over eleven kinds. Tested device-free, with a
  pinned byte vector for the dictionary's field order and padding.
- `protocol::Http2`, the HTTP/2 framing the RemoteXPC messages ride on: the connection preface,
  `SETTINGS`, `WINDOW_UPDATE`, `HEADERS`, `DATA`, `PING`, and `GOAWAY`, with the peer's window
  tracked so a payload over 64 KiB is split across `DATA` frames. Hand-rolled, tested device-free
  against a scripted peer.
- `Rsd`, the Remote Service Discovery connection over the tunnel: the device handshake, the service
  dictionary, `start_service` with its `RSDCheckin`, and a `TcpLink` to a named service. The device
  test lists the RSD services and reaches one, on an iOS 18.7.8 device.
- `ByteStream`, the byte-level seam a service codec rides: `Stream` (the mux link) and `TcpLink` (the
  RSD tunnel) both implement it, so `AFC` and the plist service run over either link unchanged.
- `PlistService`, the length-prefixed plist service that `lockdownd`, `installation_proxy`, process
  control, and the RSD checkin share.
- `install(Rsd&, ipa)` and `uninstall(Rsd&, bundle_id)`, the iOS 17.4+ app functions over the RSD
  tunnel: the IPA is staged in `/PublicStaging` over the `AFC` shim, then installed over the
  `installation_proxy` shim. Verified on an iOS 18.7.8 device: a development-signed IPA installs
  and uninstalls, and an unsigned IPA is refused with `ApplicationVerificationFailed`.

### Fixed

- `Device::tunnel` now wraps the `CoreDeviceProxy` stream in TLS when the `StartService`
  answer sets `EnableServiceSSL`, so the `CDTunnel` handshake is no longer sent in plaintext
  and the device no longer resets the port. `Lockdown::start_service` returns the flag
  alongside the port, and `Tunnel` is a pimpl so the TLS session's stream pointer stays
  valid when the tunnel moves.
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
- `PlistService` now writes the length prefix as the plist size alone rather than the
  size plus the prefix, so the device no longer reads past the message and resets the
  connection; this is `internal_plist_send`'s framing.

### Changed

- The CoreDevice tunnel plan now records that RemoteXPC runs over HTTP/2, so the last
  increment is three (`protocol::RemoteXpc`, `protocol::Http2`, and `Rsd`) rather than one.
- The tunnel's HTTP/2 layer is hand-rolled, with no dependency: nghttp2's session enforces
  HTTP semantics and drops the `DATA` of an empty-`HEADERS` stream, so the layer implements
  the framing itself, as go-ios and pymobiledevice3 do.
- `TcpLink` is now a `Transport`, so the HTTP/2 layer reads and writes it directly.
- `XpcWrapper::encode` now writes the flags word exactly as it is set rather than
  normalizing `AlwaysSet` and `DataPresent`, so the RSD setup frames match the reference.
- The mbedTLS TLS debug callback is now behind `IOSCPP_TRACE`, so a normal run no
  longer prints the handshake.
- The `app` process-control functions are documented as unvalidated and blocked on the
  `RSD` tunnel: they used a plist service that does not exist, and the real service is
  `DTX` (`docs/04-blockers.md`).
- The RSD checkin now reads its plists through `PlistService`, so the length-prefixed
  framing has one implementation rather than one per service.

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
