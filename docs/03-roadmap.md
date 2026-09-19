# Roadmap

The implementation is built in **vertical slices**: each slice runs end to end, from the
public API down to the device, and is finished, tested, and documented before the next one
starts. A slice that only builds half a layer is not done until a test exercises it against a
mock transport, and, where a device is needed, against a real device.

This keeps the repository in a working state after every slice, and it makes each layer's
protocol assumptions explicit while they are still small enough to get right.

## Status

Slices 0 to 5 are done. Slice 4 is proven on a device: `ioscpp_device_tests` walks the
descriptors, claims the mux interface, connects, and reads the device's identity, and it skips with
code 77 when no matching device is attached. Slice 5 is proven too: the pairing exchange completes, the
record is saved and reused, `StartSession` wraps `lockdownd` in TLS, and the device reports its
`ProductType` and `ProductVersion` through `GetValue`. Slice 6 onward stays open until a real device
passes it.

### Current work

Slices 4 and 5 are done and proven, so the next work is Slice 6: validate `AFC` listing and
transfer against a real device (#5). The reset after the ClientHello that held Slices 4 and 5 back
was the host certificate's zero-length serial (`04-blockers.md`); the ClientHello was ruled out by
replaying the captured reference ClientHello byte for byte, and the framing, version, TLS version,
and pre-TLS state were each ruled out in turn. The temporary experiment knobs that did the ruling
out are retired (issue #20), so the default path reads no experiment env vars.

The open work is ordered P4 to P10, lowest first: validate Slices 6 and 7 on a device
(#5, #6), add the explicit disconnect and reconnect (#24), then the guided tour
(#7), the CoreDevice tunnel plan and implementation (#23, #8), and DTX (#9).

- [x] Slice 0: the project layout, the `Result<T>` error model, the `Transport` interface,
  the mock transport, and the build.
- [x] Slice 1: the plist codec.
- [x] Slice 2: the mux frame codec and session.
- [x] Slice 3: the mux version negotiation and port connect.
- [x] Slice 4: the USB transport.
- [x] Slice 5: pairing and `lockdownd`.
- [ ] Slice 6: `AFC` file listing and transfer.
- [ ] Slice 7: app install, uninstall, and control.
- [ ] Slice 8: the guided tour and the device integration test.
- [ ] Slice 9: the iOS 17+ `RSD` tunnel, so the CoreDevice services are reachable.

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
- `Session`, which reads and writes a complete frame over a `Transport`, tracks the mux
  sequence numbers, and reads the header before its payload. The magic is read but not checked,
  matching `usbmuxd`, because the device's own v2 value differs.
- Tests over `MockTransport` for a short read, a length that does not match, and the v1 and v2
  header sizes.

**Done when:** a hand-written frame decodes to the same fields it encoded, and a truncated
frame is an `ErrorCode::Protocol` error.

## Slice 3: Version negotiation and port connect

- `Connection`, which performs the `MUX_PROTO_VERSION` negotiation, sends the v2 setup packet
  when the device reports version 2, and connects a port with SYN / SYN|ACK / ACK.
- `Stream`, which carries data over a connected port and acknowledges each frame.

**Done when:** the mock device completes a negotiation and a connect, and a refused port is an
`ErrorCode::Device` error. Both are in `tests/stream_test.cpp`.

## Slice 4: The USB transport

- `usb::UsbTransport`, backed by libusb, selecting the configuration that carries the
  vendor-specific interface (class `0xff`, subclass `0xfe`, protocol `0x02`) and claiming it.
- `usb::DeviceId` and `usb::list`, so a device is chosen by its USB serial.
- A device integration test that skips itself when no device is attached.

**Done when:** the guided tour's first step connects to a real device.

## Slice 5: Pairing and `lockdownd`

- `crypto::Pairing`, the host key pair, the self-signed certificate, the pairing exchange,
  and the TLS session that `lockdownd` requires afterwards.
- `Lockdown`, the `lockdownd` client: `query`, `get_value`, and `start_service`.
- `Device::disconnect`, an idempotent, innermost-first teardown (TLS, streams, mux, transport),
  so the connection closes in order and the destructor shares it.

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
- A reconnect for a dropped link: a reset or replug re-enumerates the device, so the test
  rediscovers it by serial and connects again rather than reusing a stale handle.

**Done when:** the demo and the device test pass against a real device, including a replug
between steps.

## Slice 9: The iOS 17+ `RSD` tunnel

iOS 17 moved the developer services off `lockdownd` and onto **CoreDevice** over
**RemoteXPC**, and a CoreDevice service is only reachable over an **RSD** (Remote Service
Discovery) tunnel to the device. On a device of 17.4 or later, `lockdownd` exposes the
`com.apple.internal.devicecompute.CoreDeviceProxy` service, whose `CDTunnel`-framed JSON
handshake returns the tunnel interface's address, MTU, and RSD port and then carries the
tunnel's IPv6 packets as data. On 17.0–17.3.1 the same tunnel is reached over the Wi-Fi
**RemotePairing** route instead.

The tunnel is what every later CoreDevice feature needs, so it is scheduled after the
`lockdownd`, AFC, app install, and guided-tour work that runs over the plain mux (P8), and
it is built the same way the rest of the library is: no `usbmuxd`, no `tunneld`
daemon, and no TUN interface. The device's own tunnel address is only reachable from this
process, which is the userspace model; a kernel-routable tunnel is a later improvement.

The hard part is the link: `CoreDeviceProxy` hands over a **raw IPv6 packet stream with no
packet boundaries**, so the library must re-frame it by the IPv6 payload-length field and run a
**userspace TCP/IP stack** over it, so that an ordinary socket can reach the RSD port. Only
outbound TCP to a few ports is needed, so `lwIP` behind a custom `netif` (or a minimal IPv6 +
TCP client) is enough; no ARP, DHCP, routing, or ICMP.

- `Connection`'s `CoreDeviceProxy` handshake, which returns the `RSD` address and port.
- The `CDTunnel` frame and the raw-IPv6 re-framer, from the handshake to a stream of whole
  IPv6 packets.
- The userspace TCP/IP link, so `connect` reaches the RSD port with no root and no driver.
- `protocol::RemoteXpc`, the 16-byte frame header and the `xpc` dictionary codec, which is
  the CoreDevice counterpart of the mux frame and the plist codec.
- `Rsd`, the RSD connection: `GetService` and the service dictionary, and a `Stream` to a
  named service on the tunnel.
- A device integration test that skips itself on a pre-17 device.

go-ios already does all of this in pure Go, and is the reference for every step:
`ios/tunnel/tunnel_lockdown.go` for the handshake, `ios/tunnel/framing.go` for the
re-framer, and gVisor `netstack` for the userspace stack. pymobiledevice3 does the same
with PyTCP, and its RemoteXPC notes and tunnel guide describe the layers.

**Done when:** a device of iOS 17.4 or later lists the RSD services and reaches one over
the tunnel.

## Slice 10: CoreDevice and `DTX` developer services

The services Slice 9 opens are the CoreDevice ones (`com.apple.dvt.*`), which speak `DTX`
rather than the `lockdownd` plists. `dvt` and `fetch-symbols` are the first two.

- `protocol::Dtx`, the `DTX` message codec.
- `dvt` and `fetch-symbols` over the RSD `Stream` from Slice 9.

**Done when:** the tour lists the DVT services and runs one of them.

## Non-goals (for now)

- The privileged `tunneld` daemon and the kernel `utun` interface; the userspace tunnel is
  the first target, because it needs no root and no extra driver.
- The device-initiated AV/HID paths, and WebDriverAgent.
- iOS 17.0–17.3.1 over Wi-Fi, which needs the RemotePairing route rather than
  `CoreDeviceProxy`.
