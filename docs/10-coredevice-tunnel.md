# The iOS 17+ CoreDevice tunnel

## In plain terms

On iOS 17 and later the phone's app installer and developer tools sit behind a
**private tunnel**, so the library asks the phone for a tunnel and the phone answers
with an address, a port, and a size limit. The connection then becomes a pipe of
**network packets**, which the library re-frames into whole packets and carries over a
small network stack of its own, with no helper program and no special permission.
Reaching a service through that tunnel is what unblocks app install, app uninstall,
and app control. The plain-language tour is in [`00-start-here.md`](00-start-here.md);
the rest of this document is the same plan in technical terms.

This is the plan for Slice 9 ([#8](https://github.com/promethea156/ioscpp/issues/8), the
plan itself tracked as [#23](https://github.com/promethea156/ioscpp/issues/23)). It records the
layers, the wire formats, the API surface, the testing plan, and the one real decision: the
userspace TCP/IP stack. It is written before any of it is implemented.

## Why the tunnel exists

iOS 17 moved the developer services off `lockdownd` and onto **CoreDevice** over
**RemoteXPC**. A `com.apple.dt.*` service is no longer on a `lockdownd` port: it is behind a
`CoreDeviceProxy` handshake that hands out an IPv6 **RSD** (Remote Service Discovery) address
and port, and the tunnel carries the device's IPv6 packets as data. On iOS 17.4 and later
`lockdownd` exposes `com.apple.internal.devicecompute.CoreDeviceProxy` for this; on 17.0–17.3.1
the same tunnel is reached over the Wi-Fi **RemotePairing** route instead, which is out of scope.

The mux-link `installation_proxy` accepts a connection but does not answer on iOS 17+
(`docs/04-blockers.md`), so app install, uninstall, and control, and every CoreDevice feature,
are blocked on this tunnel.

## The four layers

In order, from `lockdownd` down to RSD:

1. The `CoreDeviceProxy` `StartService` and its `CDTunnel`-framed JSON handshake, which returns
   the RSD address and port.
2. The raw-IPv6 re-framer, which turns the handshake's boundary-less byte stream into a stream of
   whole IPv6 packets.
3. A userspace TCP/IP stack over that link, so an ordinary socket reaches the RSD port.
4. RSD/RemoteXPC, the plist handshake and the 16-byte multiplexed frame, and a `Stream` to a
   named service on the tunnel. `DTX` is Slice 10 ([#9](https://github.com/promethea156/ioscpp/issues/9)).

Only layer 3 is genuinely new to this repository. go-ios builds all four in pure Go, with no
`usbmuxd`, no `tunneld`, and no TUN device, and is the reference for each; pymobiledevice3 does
the same with PyTCP. This library keeps the userspace model, so it needs no root and no kernel
interface either.

## Layer 1: `CoreDeviceProxy` and the `CDTunnel` handshake

`Device` starts the `com.apple.internal.devicecompute.CoreDeviceProxy` service over the plain mux,
the same `Lockdown::start_service` path the other services use. Over that stream the device and the
host exchange one `CDTunnel` frame:

- The frame is the 8-byte ASCII magic `CDTunnel`, a **16-bit big-endian** body length, then the JSON
  body.
- The host sends `{"type":"clientHandshakeRequest","mtu":<n>}`.
- The device answers with JSON carrying `clientParameters.address`, `clientParameters.mtu`,
  `serverAddress`, and `serverRSDPort`. `serverAddress` and `serverRSDPort` are the RSD endpoint;
  `clientParameters.address` is the tunnel-local IPv6 address and `clientParameters.mtu` sizes the
  re-framer's buffer.

The `StartService` answer sets `EnableServiceSSL`, so the service requires TLS before it exchanges
any data: the stream is wrapped in a `TlsSession` with the pairing record, and the frame then goes
over TLS. A plaintext frame makes the device reset the port with `sessionUpcall connection closed`
(`docs/04-blockers.md`).

After the frame the **same stream** is a raw byte stream of back-to-back IPv6 packets with no packet
boundaries; the handshake bytes must be consumed exactly so the re-framer starts on a packet boundary.

The request MTU differs between the references: go-ios asks for `1280` (the IPv6 minimum) and
pymobiledevice3's TCP tunnel for `16000`. The device's answer's `clientParameters.mtu` is what the
host uses, so the requested value is pinned against a reference and a device during implementation.

References: go-ios `ios/tunnel/tunnel_lockdown.go` (`connectToTunnelLockdown`,
`exchangeCoreTunnelParameters`) and `ios/tunnel/tunnel.go` (`connectToTunnel`); pymobiledevice3
`remote/tunnel_service.py` (`RemotePairingTcpTunnel.request_tunnel_establish`, `CDTunnelPacket`).

## Layer 2: the raw-IPv6 re-framer

The tunnel is a TCP byte stream, so one `read` is not one packet: a read can return a partial packet
or several coalesced ones. The re-framer reads exactly the 40-byte fixed IPv6 header, checks the version
nibble is 6, reads the 16-bit big-endian payload length at header offset 4, reads that many bytes, and
emits exactly one whole packet. A frame larger than the buffer fails loudly rather than truncating, because
a truncation would desync every packet after it. Writes are already packet-aligned: each outbound packet is
written in one call and the device reframes on its side.

Reference: go-ios `ios/tunnel/framing.go` (`framedIPv6Reader`), which documents the same reasoning.

## Layer 3: the userspace TCP/IP link (the new piece)

Only **outbound TCP** to a few RSD ports is needed, so the stack is small: no ARP, DHCP, routing, or
ICMP. Two options:

| | A minimal IPv6 + TCP client (chosen) | `lwIP` behind a custom `netif` |
| --- | --- | --- |
| Size | A few hundred lines: IPv6 header, TCP state machine | A large C library and its port |
| Dependencies | None, keeping the dependency-free goal | A new vendored dependency and build integration |
| Fit | Only a TCP client, which is all the tunnel needs | A full stack, most of which is unused |
| Risk | Retransmission, window, and MSS are ours to get right | Mature and device-proven |
| Testing | Device-free against a scripted IPv6/TCP peer | Device-free against its own tests, then a device |

**Decision.** The minimal IPv6 + TCP client, because only a few outbound connections are needed, the
repository's goal is to stay dependency-free, and the whole surface is small enough to test device-free
against a scripted peer. `lwIP` is the fallback if the hand-rolled stack proves too fragile on a device;
that is a build-integration change and does not change the layer above.

The client needs: build and parse the IPv6 header; a TCP state machine (SYN, SYN|ACK, ACK, data, FIN,
RST); sequence and acknowledgement handling with a send window; MSS-sized segments and the negotiated MTU;
and a single retransmission timer. The link sits below the re-framer: it reads whole IPv6 packets, writes
whole IPv6 packets, and exposes a blocking `connect` to `[address]:port` and a byte-stream socket to the
RSD port.

**Implemented.** `TcpLink` is that client: it re-frames the tunnel with `protocol::Ipv6Framer`, runs the
SYN / SYN|ACK / ACK exchange in `connect`, and moves the connection's bytes in `read` and `write`, with the
segments sized by the MTU's MSS. It does not retransmit, because the reliable mux below the tunnel already
carries every byte; the TCP checksum it sends is over the IPv6 pseudo-header, and the one it receives is not
verified, for the same reason.

## Layer 4: HTTP/2, RemoteXPC, and RSD

An ordinary TCP connection to `[serverAddress]:serverRSDPort` is the RSD. It is the CoreDevice
counterpart of the mux, and it has three layers of its own:

- **HTTP/2.** `nghttp2` (MIT) drives this layer, a private dependency of the core behind a pimpl. It
  is I/O-free (`session_mem_recv` in, `session_mem_send` out), so it fits the blocking `TcpLink`.
  The connection opens with the HTTP/2 preface `PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n`, then a
  `SETTINGS` frame, a connection `WINDOW_UPDATE`, and a `HEADERS` frame for the control stream. Every
  RemoteXPC message travels in a `DATA` frame on stream 1 (the reply channel is stream 3), and the first
  requests have a **fixed frame order** the device validates: a `HEADERS` for stream 1, the init `DATA`, a
  `HEADERS` for stream 3, then the term `DATA` for stream 1 and the init-handshake `DATA` for stream 3.
  The device answers `GOAWAY` on a wrong stream id and `FLOW_CONTROL_ERROR` when a large payload ignores
  the granted window, so `SETTINGS` and `WINDOW_UPDATE` must be tracked for a payload over 64 KiB.
- **RemoteXPC.** Inside a `DATA` frame, each message is a 16-byte `XpcWrapper` whose **flags word must
  be set exactly**; a wrong flags word makes the device drop the connection. The flags carry `ALWAYS_SET`,
  `DATA_PRESENT` when a payload follows, and `WANTING_REPLY` for a request. The body is a `XpcPayload`
  (magic `0x42133742`, protocol version 5) and then an `xpc` object, not a plist, so it needs its own codec.
- **RSD.** The first RemoteXPC request is the device handshake (a dictionary with the host `UUID`), whose
  answer carries `Properties` and `Services`, the service dictionary that lists the `com.apple.dt.*` services and
  their ports. A service is then started by opening a connection to its port and exchanging the `RSDCheckin`
  plist (a `Label`, `ProtocolVersion` 2, and the `Request`), which answers with `RSDCheckin` and then a
  `StartService` message the host ignores.

The exact HTTP/2 framing, RemoteXPC header, and `xpc` codec are pinned against pymobiledevice3's
`remote/remotexpc.py`, `remote/xpc_message.py`, and `remote/remote_service_discovery.py`, and go-ios's
`ios/rsd.go`, the same way the mux and plist codecs were pinned. The XPC codec is the same shape as the
plist codec (a type word, then a length-prefixed value, with an aligned string), so `protocol::Json` and
`protocol::Plist` are the models it follows.

## API surface

- `protocol::Cdtunnel`: encode and decode a `CDTunnel` frame, and the handshake request and response
  JSON. A codec, tested device-free.
- `protocol::Ipv6`: parse and build the fixed IPv6 header and re-frame a byte stream into whole packets.
  A codec, tested device-free.
- `TcpLink`: re-frame the tunnel's IPv6 packets and run a small TCP client over them, with a blocking
  `connect` to `[address]:port`, `read`, `write`, and `close`. Tested device-free against a scripted peer.
- `protocol::RemoteXpc`: the `XpcWrapper` 16-byte frame, the `XpcPayload`, and the `xpc` object codec.
  A codec, tested device-free.
- `protocol::Http2`: the preface, `SETTINGS`, `HEADERS`, `DATA`, and `WINDOW_UPDATE`, with flow control
  for a payload over 64 KiB. `nghttp2` underneath, behind a pimpl, tested device-free against a scripted peer.
- `Rsd`: the device handshake, the service dictionary, `start_service` with its `RSDCheckin`, and a
  `Stream` to a named service.
- `Device::tunnel()`: starts `CoreDeviceProxy`, runs the handshake, and returns a `Tunnel` with
  `client_address`, `address`, `port`, and `mtu`. The `Tunnel` is a `Transport`, so `TcpLink` reads its
  packets directly, and the RSD connection follows. `Device::disconnect` tears it down innermost first (see
  `docs/08-assumptions.md`).

## Testing

- The codecs and the re-framer are device-free over `testing::MockTransport`: a scripted `CDTunnel`
  frame, a scripted raw stream of partial and coalesced IPv6 packets, and a scripted `xpc` dictionary.
- The userspace link is device-free against a scripted IPv6/TCP peer: a mock that answers SYN with
  SYN|ACK, acknowledges data, and resets, so the state machine, the sequence numbers, and the re-framer are
  all covered without a device.
- The device test is opt-in and **skips with code 77 on a device older than iOS 17.4**, because
  `CoreDeviceProxy` does not exist before then. On 17.4+ it lists the RSD services and reaches one over
  the tunnel.
- A device test that installs over the tunnel, replacing the mux-link `installation_proxy` path, is Slice 7
  ([#6](https://github.com/promethea156/ioscpp/issues/6)).

## Build integration

The hand-rolled link adds no dependency, so it is unchanged. `nghttp2` is a third `FetchContent`
dependency next to mbedTLS and libusb, built library-only and static, and linked privately into `ioscpp`;
it never appears in a public header, so the core stays free of it. `lwIP` remains the fallback for the link if
the hand-rolled stack proves too fragile, and would add a vendored dependency and a `netif` port. The new sources
join the `ioscpp` target in `CMakeLists.txt`; the codecs and the link are transport-agnostic, so they belong to
the core and not to `ioscpp-usb`.

## Increments

Each increment is end to end and leaves the repository working:

1. `protocol::Cdtunnel` and the raw-IPv6 re-framer, device-free over the mock. No device and no new
   dependency. **Done.**
2. The `CoreDeviceProxy` handshake on `Device`, returning the RSD address and port, with a device test that
   skips on a pre-17.4 device. **Done and proven on an iOS 18.7.8 device**: the handshake answers over TLS
   and reports the RSD address, port, and MTU.
3. The userspace IPv6 + TCP link, device-free against the scripted peer, then reaching the RSD port on a device.
   **Done.** `TcpLink` re-frames the tunnel with `protocol::Ipv6Framer`, runs the handshake, and the device
   test reaches the RSD port on an iOS 18.7.8 device.
4. `protocol::RemoteXpc`, the `XpcWrapper` 16-byte frame, the `XpcPayload`, and the `xpc` object codec,
   device-free over the mock. No new dependency.
5. `protocol::Http2`, the preface, `SETTINGS`, `HEADERS`, `DATA`, and `WINDOW_UPDATE`, with flow control
   for a payload over 64 KiB, over `nghttp2` behind a pimpl, device-free against a scripted peer.
6. `Rsd`, the device handshake, the service dictionary, `start_service` with its `RSDCheckin`, and a
   `Stream` to a named service, with a device test that lists the RSD services and reaches one over the
   tunnel. This is Slice 9's done-when.

Increments 4 to 6 were one increment in the first plan; the references show that RemoteXPC runs over
HTTP/2, so it is three, each end to end.

## Risks and blockers

- **The device must be 17.4 or later.** The device test skips on anything older, and 17.0–17.3.1 needs the
  Wi-Fi RemotePairing route, which is a non-goal for now.
- **The RSD address is IPv6 and route-less** (`docs/04-blockers.md`). It is not reachable from the host's routing
  table, so every packet goes over the tunnel by hand and never through the host stack.
- **The RemoteXPC flags word must be exact** (`docs/04-blockers.md`). A wrong value makes the device drop the
  connection.
- **A CoreDevice service speaks `DTX`, not a plist** (`docs/04-blockers.md`). That codec is Slice 10
  ([#9](https://github.com/promethea156/ioscpp/issues/9)).
- **The requested MTU differs between the references** (go-ios `1280`, pymobiledevice3 `16000`). The answer's
  `clientParameters.mtu` is what the re-framer uses, so the request is pinned against a reference and a device.

## References

- [`03-roadmap.md`](03-roadmap.md), Slice 9, and [`08-assumptions.md`](08-assumptions.md), "The RSD tunnel is
  reachable in userspace, with no daemon or TUN", and [`04-blockers.md`](04-blockers.md), the CoreDevice and
  RemoteXPC entries.
- go-ios `ios/tunnel/tunnel_lockdown.go`, `framing.go`, `rwcendpoint.go`, and `tunnel.go`.
- pymobiledevice3 `remote/tunnel_service.py`, `remote/userspace_tunnel.py`, and the RemoteXPC internals.
