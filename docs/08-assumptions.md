# Assumptions

Ideas the implementation depends on that are **not yet proven on a real device**. An
assumption is not a bug: it is a belief taken from the reference implementations, from the
protocol notes, or from a device-free test, and it holds only until a device run confirms or
refutes it. When an assumption is tested, its entry moves to
[`04-blockers.md`](04-blockers.md) (if it was wrong) or is marked **proven** here (if it held).

Each entry says what is assumed, why we believe it, how far the code goes, and how to prove it.

## How to read an entry

- **Assumption** — the thing believed.
- **Why we believe it** — the evidence, which is never this repository's own device run yet.
- **Status** — the furthest the code or the tests reach today.
- **Proof** — the device run that would settle it.

## USB

### A single process can own the device without `usbmuxd`

**Assumption.** A host process can claim the device's mux interface directly with libusb and
exchange mux frames, with `usbmuxd` stopped and no daemon or socket in between.

**Why we believe it.** `usbmuxd` itself is just such a process; its `src/usb.c` is the same
claim on the same interface. go-ios and pymobiledevice3 do it without `usbmuxd`
(`02-references.md`).

**Status.** Proven. `UsbTransport::open` claims the interface (`src/usb/usb_transport.cpp:462`),
selects the configuration that carries it first, and the device answers the mux version
request (`tests/device_test.cpp`).

**Proof.** `ioscpp_usb_example` with `usbmuxd` stopped answers the mux version request.

### The mux interface is present and stable

**Assumption.** Every supported device and iOS version exposes the interface with class `0xff`,
subclass `0xfe`, protocol `0x02`, and two bulk endpoints, and it is not necessarily interface 0
or in the configuration the device starts in.

**Why we believe it.** `usbmuxd`'s `src/usb.h` fixes that triple, and
`docs/04-blockers.md` records the "not the first interface" trap. A device in its initial mode
carries the interface only in a later configuration, so `usbmuxd` selects it.

**Status.** Proven. `find_mux_interface` walks every configuration for the triple and `open` selects
the one that carries it before claiming the interface (`src/usb/usb_transport.cpp`), and the device
answers (`tests/device_test.cpp`). On this device the interface is not index 0.

**Proof.** A device whose mux interface is not index 0, and not in the active configuration,
still connects and answers.

### libusb behaves the same on all three platforms

**Assumption.** Claiming the interface, detaching the kernel driver on Linux, and reading the
serial descriptor work on Windows (WinUSB), macOS, and Linux alike.

**Why we believe it.** libusb is the documented cross-platform path, and the code has a Linux
detach (`src/usb/usb_transport.cpp:441`).

**Status.** Proven on Windows: the device test claims the interface and connects
(`tests/device_test.cpp`). macOS and Linux are still only compiled.

**Proof.** The device test passes on all three CI platforms.

### A short bulk transfer is not an error

**Assumption.** libusb can return a transfer shorter than requested without the device having
stalled, and the transport must loop or buffer rather than treat it as end of stream.

**Why we believe it.** `docs/04-blockers.md` records this as a known trap, and `read` buffers a
partial transfer for later reads (`src/usb/usb_transport.cpp:505`).

**Status.** Proven. The TLS handshake records arrive as several short transfers and reassemble
(`tests/device_test.cpp`).

**Proof.** A large transfer over the real link reassembles without a desync.

## Mux protocol

### The version negotiation and v2 setup are as described

**Assumption.** The device answers `MUX_PROTO_VERSION` with a version, a v2 device needs the
`MUX_PROTO_SETUP` packet with payload `\x07`, and the header is 8 bytes on v1 and 16 on v2.

**Why we believe it.** `usbmuxd`'s `src/device.c` and `docs/04-blockers.md` describe it.

**Status.** Proven. A real device completes the v2 negotiation and a port connect
(`tests/device_test.cpp`).

**Proof.** A real device completes the negotiation and a port connect.

## Pairing and `lockdownd`

### Pairing works, including the trust prompt and TLS

**Assumption.** The pairing exchange completes (with the user tapping *Trust* when asked), the
pairing record is saved and reused, and after `StartSession` the `lockdownd` link is TLS using the
session key.

**Why we believe it.** `docs/04-blockers.md` and the `lockdown.c` reference describe the flow.

**Status.** Proven. The pairing exchange completes (with the trust prompt when the device is
untrusted), the record is saved to `%USERPROFILE%\.ioscpp\<serial>.plist`, and a later run loads it
and passes `StartSession` with `EnableSessionSSL=true`. The TLS handshake that follows completes, so
the device reports its `ProductType` and `ProductVersion` (`tests/device_test.cpp`). The reset after the
ClientHello was the host certificate's zero-length serial, and the record path was the USB serial's
trailing NUL padding (`docs/04-blockers.md`).

The client identity is settled as the host leaf certificate, which is what `lockdownd` paired
against, and the auth mode as `REQUIRED` with a callback that accepts the device certificate
whatever its chain says, matching `idevice_connection_enable_ssl` (`src/crypto/pairing.cpp`).

**Proof.** A device that is already trusted completes the tour's query step.

### A pairing record saved once is reusable

**Assumption.** A pairing record written by one run is accepted by `lockdownd` on the next, with no
re-pair and no trust tap.

**Status.** Proven. The device test connects, then loads the saved record again and finds it already
paired, so a second run needs no trust tap (`tests/device_test.cpp`).

**Proof.** Two consecutive device test runs, the second with the device already trusted.

## Connection lifecycle

### A dropped link is recoverable by re-discovery, not by the old handle

**Assumption.** After the device resets the mux link, or is unplugged and replugged, a reconnect
rediscovers the device by its USB serial and runs `Device::connect` again, because the device
re-enumerates and its USB address changes.

**Why we believe it.** The wire capture in `04-blockers.md` shows the device reset the mux connection, and
`usb::DeviceId` already selects a device by serial, so discovery is the same call the first connect uses.

**Status.** Implemented. The choice is the caller-owned transport: `Device::connect` keeps taking a
`Transport&`, `Device` stays tied to that one transport, and a reconnect is `disconnect`, destroy the
`Device`, re-discover by serial, open a fresh transport, and `connect` again. `ioscpp::connect_with_retry`
owns that loop with a bounded exponential backoff, so a caller no longer hand-rolls it. `Transport::reopen`
still means "a fresh `usbmuxd` socket per port" and is not the reconnect.

**Proof.** `tests/device_test.cpp` disconnects and reconnects through `connect_with_retry`, which
re-discovers the device by serial and opens a fresh transport; with `IOSCPP_TEST_REPLUG=1` it waits for a
physical unplug and replug first, so the re-enumeration and the new USB address are exercised.
`tests/connect_test.cpp` covers the retry policy device-free over a fake transport and connect.

### `disconnect` is idempotent and ordered

**Assumption.** `Device::disconnect` closes innermost first (TLS `close_notify`, then streams, mux, and
transport) and a second call is a no-op.

**Status.** Proven. `Device::disconnect` closes the `Lockdown` session (`TlsSession::close` sends the
`close_notify`, then `Stream::close` sends the reset) and then `Connection::close` closes the transport.
Each of `Stream::close`, `Connection::close`, `Lockdown::close`, and `Device::disconnect` is idempotent,
and the destructors route through them. A service stream the caller opened, such as an `Afc`, must be
destroyed first, because `Device` does not own it.

**Proof.** Two `disconnect` calls leave nothing open, and `tests/stream_test.cpp` drives the same order over
the mock transport: the stream reset is written before the transport close, and neither repeats.

## AFC

### The AFC wire format is as implemented

**Assumption.** The `CFA6LPAA` packet format, the operation set, and the relative UTF-8 NUL
terminated paths (`docs/06-afc-protocol.md`) match what a device expects.

**Status.** Proven. The device test lists the media root, stats it and a missing path, and
round-trips a 200 KiB file through `/PublicStaging` with the bytes compared
(`tests/device_test.cpp`). The device corrected four beliefs along the way: `READ_DIR` ends
with its single `DATA` and no `STATUS`, the listing carries names alone, `FILE_OPEN` sends the
mode before the path and answers `FILE_OPEN_RES`, and a message is capped at 65535 bytes
(`docs/04-blockers.md`).

**Proof.** The tour lists a directory and round-trips a file against a device.

## Apps

### Install needs the package staged, then installed by device path

**Assumption.** An IPA is uploaded over `AFC` into `/PublicStaging` and then installed over
`installation_proxy` from that device-side path. On iOS 17.4 and later both run over the RSD shims
(`com.apple.afc.shim.remote` and `com.apple.mobile.installation_proxy.shim.remote`), and
launch/close/is_running run over the RSD `com.apple.instruments.dtservicehub` service.

**Why we believe it.** `docs/04-blockers.md` and the `installation_proxy.c` reference describe the
staging and install. pymobiledevice3's `ProcessControl` implements launch/close/is_running as `DTX`
method calls (`launchSuspendedProcessWithDevicePath:...`, `sendSignal:toPid:`,
`processIdentifierForBundleIdentifier:`) over the `com.apple.instruments.server.services.processcontrol`
channel, not as the plist service the code first used.

**Status.** `App` is written. On iOS 17.4 and later `install(Rsd&)` and `uninstall(Rsd&)` ride the RSD
shims, and the device test's opt-in round trip is verified on an iOS 18.7.8 device: a development-signed
IPA installs and uninstalls. `launch(Rsd&)`, `is_running(Rsd&)`, and `close(Rsd&)` ride the RSD
`dtservicehub` service over `DTX`, and the same round trip launches the installed app, finds it running by
its bundle id, and kills it by pid.

**Proof.** The demo installs, launches, checks, and closes an app on a device.

### `house_arrest` vends an app's container over `AFC`

**Assumption.** `com.apple.mobile.house_arrest` is started like any other service, and the
bundle id then travels in a `VendContainer` (whole container) or `VendDocuments` (`Documents`
subtree) command; the device answers with the container vended over the same `AFC` protocol. On
iOS 17.4 and later the RSD tunnel carries the same service as
`com.apple.mobile.house_arrest.shim.remote`.

**Why we believe it.** pymobiledevice3's `HouseArrestService` sends
`{"Command": "VendContainer", "Identifier": bundle_id}` after starting the service and names the
RSD shim `com.apple.mobile.house_arrest.shim.remote`; `libimobiledevice`'s
`house_arrest_send_command` sends the same command. Neither passes the bundle id in
`StartService`, so the vend command is the reference behavior, not the issue's `ApplicationID`.

**Status.** Proven on a device. The device test's opt-in round trip installed a
development-signed IPA on an iOS 18.7.8 device and the container vended over the RSD shim
(`container: vended over the RSD shim`); it listed the container and round-tripped a file in its
`Documents`. The mux-link fallback was not reached, because the RSD shim answered first, so whether
`com.apple.mobile.house_arrest` still answers on the mux link on 17.4+ is not settled.

**Proof.** A device of iOS 17.4 or later lists an installed app's `Documents` and round-trips a
file in it (`tests/device_test.cpp`).

## iOS 17+ CoreDevice

### The RSD tunnel is reachable in userspace, with no daemon or TUN

**Assumption.** The `CoreDeviceProxy` handshake returns an RSD address and port, and the device's
IPv6 packets can be carried as data over a `Stream` with no `tunneld` and no `utun` interface.

**Why we believe it.** go-ios's `ios tunnel start --userspace` does exactly this in pure Go: it starts
the `com.apple.internal.devicecompute.CoreDeviceProxy` lockdown service, exchanges the `CDTunnel`-framed
JSON handshake for the address, MTU, and RSD port, re-frames the raw byte stream into IPv6 packets
(`ios/tunnel/framing.go`), and runs a userspace TCP/IP stack over that link. pymobiledevice3 does the
same with PyTCP. Both then speak RemoteXPC/RSD over a normal socket, so the tunnel is proven reachable
with no root.

**The one new piece.** The tunnel's link is a raw IPv6 packet stream, so a userspace TCP/IP stack is
required. In C++ that is `lwIP` behind a custom `netif`, or a minimal IPv6 + TCP client: the tunnel needs
only outbound TCP connections to a few RSD ports, so no ARP, DHCP, routing, or ICMP is needed. `TcpLink` is
the minimal client, with `protocol::Ipv6Framer` below it.

**Status.** Implemented and proven on a device. The `CoreDeviceProxy` handshake runs over
TLS and returns the RSD address, port, and MTU, and `TcpLink` re-frames the tunnel's IPv6 packets and
opens a TCP connection to the RSD port, all on an iOS 18.7.8 device (`docs/10-coredevice-tunnel.md`,
increments 2 and 3). The `protocol::RemoteXpc` codec and the `protocol::Http2` layer the RSD
connection rides on are done device-free (increments 4 and 5), and the `Rsd` connection over the link,
which lists the services and reaches one, is done and proven on an iOS 18.7.8 device (increment 6).
The layers, the wire formats, the API surface, the testing plan, and the choice of a
hand-rolled minimal IPv6 + TCP client over `lwIP` are in
[`10-coredevice-tunnel.md`](10-coredevice-tunnel.md).

**Proof.** A device of iOS 17.4 or later lists the RSD services over the tunnel.

### The RSD shim services are lockdown-style plist services

**Assumption.** The iOS 17.4+ `AFC` and installer shims (`com.apple.afc.shim.remote` and
`com.apple.mobile.installation_proxy.shim.remote`) are not RemoteXPC services: after the
`RSDCheckin`, each speaks the same length-prefixed plist framing `lockdownd` does.

**Why we believe it.** pymobiledevice3's `RSD_SERVICE_NAME` reaches both over the RSD tunnel with
an `RSDCheckin` and then the same messages as the mux-link services.

**Status.** Proven on a device. The device test stages an IPA over the `AFC` shim and the installer
shim answers a status, and `uninstall(Rsd&)` removes an app over the installer shim
(`docs/04-blockers.md`).

**Proof.** A device of iOS 17.4 or later installs a development-signed app over the RSD shims.

## Device power and input

### iOS exposes a sleep request but no wake or input channel

**Assumption.** `com.apple.mobile.diagnostics_relay` exposes `Sleep`, `Restart`, and `Shutdown` as
plist requests, but no `Wake`, and a paired host has no general input-injection channel. So a host can
turn the screen off but has no documented way to turn it back on.

**Why we believe it.** This is where iOS differs from Android. `adb shell input keyevent
KEYCODE_SLEEP` / `KEYCODE_WAKEUP` works because adb runs a **shell on the device** that injects input
events, so sleep and wake are just two key events. iOS gives a paired host no shell and no input
service: `lockdownd` exposes a fixed service set, and `com.apple.mobile.diagnostics_relay`'s power
requests are `Sleep`, `Restart`, and `Shutdown`, with no `Wake` (libimobiledevice's `idevicediagnostics`
and pymobiledevice3's `diagnostics` expose the same three and no wake). The nearest analogue,
WebDriverAgent's `/wda/lock` and `/wda/unlock`, is a developer/XCTest app and a non-goal
([`01-objective.md`](01-objective.md#non-goals-for-now)).

**Status.** Not implemented. There is no diagnostics client. `Sleep` (screen off), `Restart`, and
`Shutdown` are planned in #89, and whether the screen can be turned back on is the spike in #90.

**Proof.** #90 settles it: either a mechanism turns the screen back on against a real device, or the
finding is recorded in [`04-blockers.md`](04-blockers.md) so #89 can say the screen-off direction has
no counterpart.

## Test fidelity

### The mock transport models the device

**Assumption.** A device-free test over `MockTransport` that passes means the codec and the session
logic are correct, so a later device failure is a protocol assumption, not a codec bug.

**Why we believe it.** It is the only way to test without a device, and the slices are built on it.

**Risk.** A mock encodes the same misunderstanding the code does, so it can confirm a wrong frame.
The device tests are what break the tie. The AFC mock did exactly this: it fed a `STATUS` after a
`READ_DIR` listing, so it confirmed the wrong belief until the device hung
(`docs/04-blockers.md`).
