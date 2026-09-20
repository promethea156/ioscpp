# The iOS 17+ CoreDevice tunnel

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

## Layer 4: RSD and RemoteXPC

An ordinary TCP connection to `[serverAddress]:serverRSDPort` is the RSD. It speaks RemoteXPC, the
CoreDevice counterpart of the mux framing and the plist codec:

- The RSD handshake is a plist handshake over a 16-byte multiplexed RemoteXPC frame whose **flags word
  must be set exactly**; a wrong flags word makes the device drop the connection.
- The body is an `xpc` dictionary, not a plist, so it needs its own codec.
- `Rsd::get_service` sends `GetService` for a service name and returns a `Stream` to the named service's
  port; the RSD service dictionary lists the `com.apple.dt.*` services.

The exact RemoteXPC header and `xpc` codec are pinned against go-ios's RSD/RemoteXPC packages and
pymobiledevice3's `remote/remote_service_discovery.py`, `remote/remote_service.py`, and
`remote/xpc_message.py` during implementation, the same way the mux and plist codecs were pinned.

## API surface

- `protocol::Cdtunnel`: encode and decode a `CDTunnel` frame, and the handshake request and response
  JSON. A codec, tested device-free.
- `protocol::Ipv6`: parse and build the fixed IPv6 header and re-frame a byte stream into whole packets.
  A codec, tested device-free.
- `RemoteXpc` and `Rsd`: the frame header, the `xpc` dictionary, `GetService`, and a `Stream` to a
  named service.
- `Device::tunnel()` (name pinned during implementation): starts `CoreDeviceProxy`, runs the handshake, and
  returns a `Tunnel` with `address`, `port`, and `mtu`. The `Tunnel` owns the userspace link and the RSD
  connection, so the caller reaches services by name; `Device::disconnect` tears it down innermost first
  (see `docs/08-assumptions.md`).

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

The hand-rolled link adds no dependency, so the build is unchanged. `lwIP` would add a vendored
dependency and a `netif` port, and would be a `FetchContent` entry next to mbedTLS if the fallback is taken.
The new sources join the `ioscpp` target in `CMakeLists.txt`; the codecs and the link are transport-agnostic,
so they belong to the core and not to `ioscpp-usb`.

## Increments

Each increment is end to end and leaves the repository working:

1. `protocol::Cdtunnel` and the raw-IPv6 re-framer, device-free over the mock. No device and no new
   dependency.
2. The `CoreDeviceProxy` handshake on `Device`, returning the RSD address and port, with a device test that
   skips on a pre-17.4 device.
3. The userspace IPv6 + TCP link, device-free against the scripted peer, then reaching the RSD port on a device.
4. `protocol::RemoteXpc`, `Rsd`, and `GetService`, with a device test that lists the RSD services and reaches
   one over the tunnel. This is Slice 9's done-when.

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
