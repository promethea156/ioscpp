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
userspace TCP/IP stack. It was written before any of it was implemented, and is kept as the
record of the plan; the slices below are now done.

## Why the tunnel exists

iOS 17 moved the developer services off `lockdownd` and onto **CoreDevice** over
**RemoteXPC**. A `com.apple.dt.*` service is no longer on a `lockdownd` port: it is behind a
`CoreDeviceProxy` handshake that hands out an IPv6 **RSD** (Remote Service Discovery) address
and port, and the tunnel carries the device's IPv6 packets as data. On iOS 17.4 and later
`lockdownd` exposes `com.apple.internal.devicecompute.CoreDeviceProxy` for this; on 17.0–17.3.1
the same tunnel is reached over the Wi-Fi **RemotePairing** route instead, which is out of scope
([below](#the-wi-fi-remotepairing-route-ios-1701731)).

The mux-link `installation_proxy` accepts a connection but does not answer on iOS 17+
(`docs/04-blockers.md`), so app install and uninstall use the RSD `AFC` and installer shims on
this tunnel (Slice 7), and app control and every CoreDevice feature run over the `DTX` codec
(Slice 10).

## The four layers

In order, from `lockdownd` down to RSD:

1. The `CoreDeviceProxy` `StartService` and its `CDTunnel`-framed JSON handshake, which returns
   the RSD address and port.
2. The raw-IPv6 re-framer, which turns the handshake's boundary-less byte stream into a stream of
   whole IPv6 packets.
3. A userspace TCP/IP stack over that link, so an ordinary socket reaches the RSD port.
4. RSD/RemoteXPC, the plist handshake and the fixed multiplexed frame, and a `Stream` to a
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

- **HTTP/2.** The layer is hand-rolled, with no dependency. The frames are simple and the `HEADERS`
  frames carry no fields, so no HPACK is needed, which is how go-ios and pymobiledevice3 do it; nghttp2's
  session enforces HTTP semantics and drops the `DATA` of an empty-`HEADERS` stream, so it does not fit.
  The connection opens with the HTTP/2 preface `PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n`, then a
  `SETTINGS` frame, a connection `WINDOW_UPDATE`, and a `HEADERS` frame for the control stream. Every
  RemoteXPC message travels in a `DATA` frame on stream 1 (the reply channel is stream 3), and the first
  requests have a **fixed frame order** the device validates: a `HEADERS` for stream 1, the init `DATA`, a
  `HEADERS` for stream 3, then the term `DATA` for stream 1 and the init-handshake `DATA` for stream 3.
  The device answers `GOAWAY` on a wrong stream id and `FLOW_CONTROL_ERROR` when a large payload ignores
  the granted window, so `SETTINGS` and `WINDOW_UPDATE` must be tracked for a payload over 64 KiB.
- **RemoteXPC.** Inside a `DATA` frame, each message is an `XpcWrapper`: the magic `0x29B00B92`, a **flags
  word that must be set exactly**, the 64-bit body length, and the message id, then the body. A wrong flags word
  makes the device drop the connection. The flags carry `ALWAYS_SET`, `DATA_PRESENT` when a payload follows, and
  `WANTING_REPLY` for a request. The body is a `XpcPayload` (magic `0x42133742`, protocol version 5) and then
  an `xpc` object, not a plist, so it needs its own codec. Every `xpc` field is little-endian, a string is
  NUL-terminated, and a string, data blob, array, and dictionary are padded to a 4-byte boundary.
- **RSD.** The first RemoteXPC request is the device handshake (a dictionary with the host `UUID`), whose
  answer carries `Properties` and `Services`, the service dictionary that lists the `com.apple.dt.*` services and
  their ports. A service is then started by opening a connection to its port and exchanging the `RSDCheckin`
  plist (a `Label`, `ProtocolVersion` 2, and the `Request`), which answers with `RSDCheckin` and then a
  `StartService` message the host ignores. A `com.apple.dt.*` service is a RemoteXPC one, while the `AFC`
  and installer shims (`com.apple.afc.shim.remote` and
  `com.apple.mobile.installation_proxy.shim.remote`) are lockdown-style: after the same `RSDCheckin` they
  speak the length-prefixed plists `lockdownd` does.

The exact HTTP/2 framing, RemoteXPC header, and `xpc` codec are pinned against pymobiledevice3's
`remote/remotexpc.py`, `remote/xpc_message.py`, and `remote/remote_service_discovery.py`, and go-ios's
`ios/rsd.go`, the same way the mux and plist codecs were pinned. The XPC codec is the same shape as the
plist codec (a type word, then a length-prefixed value, with an aligned string), so `protocol::Json` and
`protocol::Plist` are the models it follows.

## The Wi-Fi RemotePairing route (iOS 17.0–17.3.1)

iOS 17.0–17.3.1 has no `CoreDeviceProxy`: those versions reach the same tunnel only
over the Wi-Fi **RemotePairing** route (issue #73). Only the layers above the `CDTunnel`
handshake are reused; everything below it is new.

In order, from the device to `CDTunnel`:

1. **Bonjour discovery.** The device advertises `_remotepairing._tcp`, and each answer is
   matched to a pair record by an `authTag` derived from the record's 16-byte `altIRK`, so
   only a device this host has paired with is contacted.
2. **A separate pairing record.** RemotePairing keeps its own record (an Ed25519 key pair,
   `remote_unlock_host_key`, `peer_alt_irk`), in a different store from the USB
   `lockdownd` record that `crypto::Pairing` holds. A record that predates `altIRK` is
   rejected, so a host paired only over USB has to pair again over RemotePairing (SRP, and the
   *Trust* prompt).
3. **The RemotePairing handshake.** `RemotePairingProtocol` runs pair-verify with X25519 +
   Ed25519 + HKDF-SHA512 + ChaCha20Poly1305, carried in XPC-style messages
   (`message.plain._0`, `originatedBy`, `sequenceNumber`), then derives `ClientEncrypt-main`
   and `ServerEncrypt-main` keys for an encrypted control channel.
4. **A listener and the transport.** An encrypted `createListener` request returns a port, and
   the tunnel then runs over **QUIC on pre-18.2** or **TLS-PSK TCP on 18.2+**
   (`remote/common.py`: `TunnelProtocol.DEFAULT`, and the code raises `iOS 18.2+ removed
   QUIC protocol support`).
5. **Then** `protocol::Cdtunnel`, `protocol::Ipv6Framer`, `TcpLink`, and `Rsd` are reused
   unchanged.

Two blockers keep this a non-goal. First, iOS 17.0–17.3.1 is pre-18.2, so its transport is
QUIC, which this repository cannot reach: mbedTLS 3.6.2 ships no QUIC, so QUIC means a new
dependency (ngtcp2/quiche/msquic) and a TLS 1.3 handshake with datagram-frame handling. The
TLS-PSK TCP path mbedTLS can do is the 18.2+ route, not the one 17.0–17.3.1 needs. Second,
verification needs a device on iOS 17.0–17.3.1, and the devices on hand are iOS 17.5.1 and 18.7.8,
both already on `CoreDeviceProxy`.

Reference: pymobiledevice3 `remote/tunnel_service.py` (`RemotePairingProtocol`,
`RemotePairingTunnelService`, `RemotePairingQuicTunnel`, `RemotePairingTcpTunnel`,
`get_remote_pairing_tunnel_services`) and `remote/common.py` (`TunnelProtocol`).

## API surface

- `protocol::Cdtunnel`: encode and decode a `CDTunnel` frame, and the handshake request and response
  JSON. A codec, tested device-free.
- `protocol::Ipv6`: parse and build the fixed IPv6 header and re-frame a byte stream into whole packets.
  A codec, tested device-free.
- `TcpLink`: re-frame the tunnel's IPv6 packets and run a small TCP client over them, with a blocking
  `connect` to `[address]:port`, `read`, `write`, and `close`. Tested device-free against a scripted peer.
- `protocol::RemoteXpc`: `XpcWrapper`, the wrapper's header and body; `XpcPayload`, the payload magic
  and version; and `Xpc`, the `xpc` object codec (`Null`, `Bool`, `Int64`, `Uint64`, `Double`, `Date`,
  `Data`, `String`, `Uuid`, `Array`, and `Dictionary`). A codec, tested device-free.
- `protocol::Http2`: the preface, `SETTINGS`, `HEADERS`, `DATA`, `PING`, and `WINDOW_UPDATE`, with the
  peer's window tracked so a payload over 64 KiB is split across `DATA` frames. Hand-rolled, tested device-free
  against a scripted peer.
- `Rsd`: the device handshake, the service dictionary, `start_service` with its `RSDCheckin`, and a
  `TcpLink` to a named service.
- `ByteStream`: the byte-level seam a service codec rides, with `Stream` (the mux link) and `TcpLink` (the
  tunnel) as its implementations, so `AFC` and `PlistService` run over either link unchanged.
- `PlistService`: the length-prefixed plist service (`lockdownd` framing), so the RSD checkin, the installer,
  and process control share one framing.
- `install(Rsd&, ipa)` and `uninstall(Rsd&, bundle_id)`: stage the IPA in `/PublicStaging` over the RSD `AFC`
  shim, then install it over the RSD installer shim.
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
- The device test's opt-in round trip stages and installs an IPA over the RSD `AFC` shim and
  uninstalls an app over the RSD installer shim (Slice 7,
  [#6](https://github.com/promethea156/ioscpp/issues/6)). An unsigned IPA is refused with
  `ApplicationVerificationFailed`, so a full install success needs a development-signed IPA
  (`docs/04-blockers.md`).

## Build integration

The hand-rolled link and HTTP/2 layer add no dependency, so the build is unchanged. `lwIP` remains the
fallback for the link if the hand-rolled stack proves too fragile, and would add a vendored dependency and a
`netif` port. The new sources join the `ioscpp` target in `CMakeLists.txt`; the codecs and the link are
transport-agnostic, so they belong to the core and not to `ioscpp-usb`.

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
4. `protocol::RemoteXpc`, the `XpcWrapper` frame, the `XpcPayload`, and the `xpc` object codec,
   device-free. No new dependency. **Done.** `Xpc`, `XpcPayload`, and `XpcWrapper` round-trip the eleven
   object kinds, and a pinned byte vector covers the dictionary's field order and padding.
5. `protocol::Http2`, the preface, `SETTINGS`, `HEADERS`, `DATA`, `PING`, and `WINDOW_UPDATE`, with the
   peer's window tracked so a payload over 64 KiB is split across `DATA` frames, hand-rolled, device-free against a
   scripted peer. **Done.**
6. `Rsd`, the device handshake, the service dictionary, `start_service` with its `RSDCheckin`, and a
   `TcpLink` to a named service, with a device test that lists the RSD services and reaches one over the
   tunnel. This is Slice 9's done-when. **Done and proven on an iOS 18.7.8 device**: the handshake lists 59
   services and a service is reached.
7. App install and uninstall over the RSD shims, with the device test's opt-in round trip. **Done and
   device-verified** (Slice 7) on an iOS 18.7.8 device: the IPA is staged over the `AFC` shim, installed
   over the installer shim, and the bundle is uninstalled. An unsigned IPA is refused with
   `ApplicationVerificationFailed`, which is a device-side policy answer (`docs/04-blockers.md`).

Increments 4 to 6 were one increment in the first plan; the references show that RemoteXPC runs over
HTTP/2, so it is three, each end to end.

## Risks and blockers

- **The device must be 17.4 or later.** The device test skips on anything older, and 17.0–17.3.1 needs the
  Wi-Fi RemotePairing route, which is a non-goal for now: it is not a route swap but a second pairing record,
  a pair-verify handshake, an encrypted control channel, and a QUIC transport mbedTLS cannot provide
  ([above](#the-wi-fi-remotepairing-route-ios-1701731), issue #73).
- **The RSD address is IPv6 and route-less** (`docs/04-blockers.md`). It is not reachable from the host's routing
  table, so every packet goes over the tunnel by hand and never through the host stack.
- **The RemoteXPC flags word must be exact** (`docs/04-blockers.md`). A wrong value makes the device drop the
  connection.
- **A CoreDevice service speaks `DTX`, not a plist** (`docs/04-blockers.md`). That codec is Slice 10
  ([#9](https://github.com/promethea156/ioscpp/issues/9)).
- **The plist length prefix is the plist size alone** (`docs/04-blockers.md`). Writing the size plus the
  prefix makes the device read past the message and reset the connection; the shared `PlistService` writes the
  size alone.
- **The requested MTU differs between the references** (go-ios `1280`, pymobiledevice3 `16000`). The answer's
  `clientParameters.mtu` is what the re-framer uses, so the request is pinned against a reference and a device.

## References

- [`03-roadmap.md`](03-roadmap.md), Slice 9, and [`08-assumptions.md`](08-assumptions.md), "The RSD tunnel is
  reachable in userspace, with no daemon or TUN", and [`04-blockers.md`](04-blockers.md), the CoreDevice and
  RemoteXPC entries.
- go-ios `ios/tunnel/tunnel_lockdown.go`, `framing.go`, `rwcendpoint.go`, `tunnel.go`, and
  `ios/xpc/encoding.go` for the `xpc` object codec.
- pymobiledevice3 `remote/tunnel_service.py`, `remote/userspace_tunnel.py`, `remote/xpc_message.py`,
  and `remote/remotexpc.py`.
