# Roadmap

The implementation is built in **vertical slices**: each slice runs end to end, from the
public API down to the device, and is finished, tested, and documented before the next one
starts. A slice that only builds half a layer is not done until a test exercises it against a
mock transport, and, where a device is needed, against a real device.

This keeps the repository in a working state after every slice, and it makes each layer's
protocol assumptions explicit while they are still small enough to get right.

## Status

The repository is a **scaffold**: the whole project layout, the public headers, the error
model, and the device-free test suite exist, and the library builds. The slices below are the
work that turns the scaffold into a working client.

- [x] Slice 0: the project layout, the `Result<T>` error model, the `Transport` interface,
  the mock transport, and the build.
- [ ] Slice 1: the plist codec.
- [ ] Slice 2: the mux frame codec and session.
- [ ] Slice 3: the mux version negotiation and port connect.
- [ ] Slice 4: the USB transport.
- [ ] Slice 5: pairing and `lockdownd`.
- [ ] Slice 6: `AFC` file listing and transfer.
- [ ] Slice 7: app install, uninstall, and control.
- [ ] Slice 8: the guided tour and the device integration test.

## Slice 0: Layout, error model, and transport

The foundation the rest is built on.

- `error.hpp`, `export.hpp`, and `transport.hpp`.
- The `testing::MockTransport`, so every later slice can be tested without a device.
- `CMakeLists.txt`, the tests, and the examples.

**Done when:** the library builds on all three platforms and the device-free tests pass.

## Slice 1: The plist codec

Every iOS protocol exchanges property lists, so this comes first.

- `protocol::Plist`, a variant over null, boolean, integer, real, string, data, date, array,
  and dictionary.
- `Plist::parse` and `Plist::to_xml`, covering the XML form the device sends, and the
  binary form (`bplist00`) some services use.
- A round-trip test for every type, and a corpus test against captured payloads.

**Done when:** `parse` then `to_xml` round-trips every type, and the malformed-input tests
return an `ErrorCode::Protocol` instead of crashing.

## Slice 2: The mux frame codec and session

- `protocol::MuxHeader` and `protocol::TcpHeader`, with encode and decode, big-endian, and
  the `0xfeedface` magic.
- `Session`, which reads and writes a complete frame over a `Transport` and validates the
  header before reading its payload.
- Tests over `MockTransport` for a short read, a bad magic, and a length that does not match.

**Done when:** a hand-written frame decodes to the same fields it encoded, and a truncated
frame is an `ErrorCode::Protocol` error.

## Slice 3: Version negotiation and port connect

- `Connection`, which performs the `MUX_PROTO_VERSION` negotiation, sends the v2 setup packet
  when the device reports version 2, and connects a port with SYN / SYN|ACK / ACK.
- `Stream`, which carries data over a connected port and acknowledges each frame.

**Done when:** the mock device completes a negotiation and a connect, and a refused port is an
`ErrorCode::Protocol` error with the device's reason.

## Slice 4: The USB transport

- `usb::UsbTransport`, backed by libusb, claiming the vendor-specific interface
  (class `0xff`, subclass `0xfe`, protocol `0x02`).
- `usb::DeviceId` and `usb::list`, so a device is chosen by its USB serial.
- A device integration test that skips itself when no device is attached.

**Done when:** the guided tour's first step connects to a real device.

## Slice 5: Pairing and `lockdownd`

- `crypto::Pairing`, the host key pair, the self-signed certificate, the pairing exchange,
  and the TLS session that `lockdownd` requires afterwards.
- `Lockdown`, the `lockdownd` client: `query`, `get_value`, and `start_service`.

**Done when:** the tour queries the device's `ProductType` and `ProductVersion`.

## Slice 6: AFC file listing and transfer

- `Afc`, the `com.apple.afc` client, and `list`, `stat`, `pull`, and `push` over it, using
  the `CFA6LPAA` packet format in [`06-afc-protocol.md`](06-afc-protocol.md).

**Done when:** the tour lists a directory and round-trips a file.

## Slice 7: App install, uninstall, and control

- `app.hpp`: `install` and `uninstall` over `installation_proxy`, and `launch`, `close`, and
  `is_running` over process control.

**Done when:** the tour installs, launches, checks, and closes an app.

## Slice 8: The guided tour and the device integration test

- `examples/demo/main.cpp`, the commented walkthrough.
- `tests/device_test.cpp`, which runs every feature against one device and skips itself
  with code 77 when none is attached.

**Done when:** the demo and the device test pass against a real device.
