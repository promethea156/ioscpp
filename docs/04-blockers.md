# Blockers

Significant blockers hit during development, and how they were solved. Each entry keeps the
symptom, the cause, and the fix, so the same trap is not walked into twice.

The list is empty in the scaffold and fills in as the slices in
[`03-roadmap.md`](03-roadmap.md) are implemented. The entries below are the ones already known
from the reference implementations and are expected to be hit.

## Hit blockers

### The device starts in a configuration without the mux interface

**Symptom.** With an iPhone attached and its USB ids present in Device Manager,
`ioscpp_usb_example` printed `no device attached`. libusb enumerated the
composite device but its active configuration (value 1) held only the PTP
interface (class `0x06`, subclass `0x01`, protocol `0x01`). The mux interface
(`0xff`/`0xfe`/`0x02`) exists only in configurations 3 and 4, and `list` read
the active configuration alone, so it found no mux and skipped the device.

**Cause.** A device in its initial USB mode does not sit in the configuration that
carries the mux interface; a host has to select it. `usbmuxd` does this in
`set_valid_configuration` (`src/usb.c`): it walks the configurations, finds the
one with the mux interface, detaches the kernel drivers on Linux, and calls
`libusb_set_configuration`. The library never selected a configuration.

**Fix.** `find_mux_interface` searches every configuration and prefers the active
one, and `open` calls `libusb_set_configuration` when the active configuration
differs, detaching kernel drivers on Linux first
(`src/usb/usb_transport.cpp`). A device that is already in the right
configuration is left alone.

### The mux interface is bound to Apple's driver on Windows

**Symptom.** On Windows, `ioscpp_usb_example` printed `no device attached`, or
`libusb_claim_interface` failed with `LIBUSB_ERROR_NOT_SUPPORTED` or
`LIBUSB_ERROR_BUSY`, while the device was present in Device Manager under Apple's
driver.

**Cause.** On Windows the device is freed differently than on Linux, where `usbmuxd`
is the only holder: the *Apple Mobile Device Service* claims the device, and the mux
interface is bound to Apple's `usbaapl64` driver, which libusb cannot open.

**Fix.** Stop *Apple Mobile Device Service*, bind a libusb-compatible driver to the
mux interface alone with Zadig, and replug. Bind the interface, not the composite
device, so the rest of the device keeps Apple's driver. Use `libusb-win32`, not
WinUSB: its libusb0 backend sends `SET_CONFIGURATION`, which the device's initial USB
mode needs (`docs/09-platform-setup.md`).

### A v2 data frame is `ACK` alone, not `PSH|ACK`

**Symptom.** With the configuration selected and the mux handshake done, the
device answered the first lockdownd request with an immediate RST whose payload read
`handleMuxTCPInput th.th_flags = 0x18, not TH_ACK(0x10)`.

**Cause.** `usbmuxd`'s `send_tcp` always sets `th_flags = TH_ACK` for a data
frame; the `PSH` bit is never set. The device's `handleMuxTCPInput` resets a
connection whose data frame carries any other flag. `Stream::write` sent `PSH|ACK`.

**Fix.** `Stream::write` sets `TcpAck` alone (`src/stream.cpp`).

### The v2 mux sequence numbers must advance for every frame

**Symptom.** The device answered the version request and the setup, then reset the
connection with a control frame reading `detected duplicate packet. Expected N
received N-1`. The setup, SYN, and data frames all carried the same mux `tx_seq`.

**Cause.** `usbmuxd`'s `send_packet` increments `dev->tx_seq` for every v2 frame,
including the setup, the SYN, and the ACK. The library's `Session::send` did not
advance `tx_seq_` across the setup and the SYN, so the device saw a repeat.

**Fix.** `Session::send` assigns `header.tx_seq = tx_seq_++` for every frame, so the
setup, the SYN, and the data carry consecutive numbers (`src/session.cpp`).

### The v2 magic is the device's own, so it is not validated

**Symptom.** The device's v2 frames carried `magic = 0xfaceface`, while the host
sent `0xfeedface`. A strict receive check would reject every device frame.

**Cause.** `usbmuxd` sets the host's magic but never checks the device's. The
device uses a different value of its own.

**Fix.** `Session::receive` reads the magic but does not compare it, matching
`usbmuxd` (`src/session.cpp`).

### The device's userspace session answers late

**Symptom.** After the mux handshake, the first lockdownd request is acknowledged
and then the device resets the connection after roughly 60 to 150 seconds with
`sessionUpcall connection closed`. No lockdownd reply arrives.

**Cause.** The device's kernel mux delivers the data to a userspace session
(`sessionUpcall`); when that session is not answered, the connection is closed after
a long timeout. This is the state the device is in on this host, and it is still
open.

### The hand-built TLS session poisons the ClientHello

**Symptom.** After pairing, the TLS handshake sent a ClientHello whose record
version read `0x0000` and whose cipher suite list held only the `00ff` SCSV, and
the device reset the connection. mbedTLS logged
`client hello, add ciphersuite` for no suite but the SCSV.

**Cause.** `TlsSession::start` built a throwaway `mbedtls_ssl_session` that only
carried the `StartSession` `SessionID` and passed it to `mbedtls_ssl_set_session`.
`mbedtls_ssl_set_session` copies the session into `session_negotiate` and sets
`handshake->resume`, and a session whose `tls_version` is still zero makes
`ssl_write_client_hello` take the resume branch and set `ssl->tls_version` to zero.
The ClientHello record header is written from `ssl->tls_version`
(`ssl_msg.c:2955`), and every suite is filtered against it
(`ssl_client.c:356`), so only the SCSV survives.

**Fix.** `TlsSession::start` no longer passes a session, matching
`idevice_connection_enable_ssl`, which never calls `mbedtls_ssl_set_session`
(`src/crypto/pairing.cpp`). A device-free test pins the trap and the escape in
`tests/tls_client_hello_test.cpp`.

### The device resets the TLS handshake after the ClientHello

**Symptom.** With a well-formed ClientHello (record version `0x0303`, the full
suite list, and a valid `key_share`), the device acknowledges it and then sends a
mux control frame `type=3 socketIsClosed sock_receive returned errno 54` and a
reset whose reason is `sessionUpcall connection closed`. No ServerHello arrives,
and `mbedtls_ssl_handshake` returns `MBEDTLS_ERR_SSL_CONN_EOF`.

**Cause.** Still open. It is not the TLS version (it happens pinned to TLS 1.2 too),
not the client identity (the host leaf and the root both fail), not the pre-TLS delay,
and not the verify mode. The device is on iOS 18.7.8.

**State.** `TlsSession::start` presents the host leaf certificate and key (the
`pymobiledevice3` choice) with `config_defaults`, the `libimobiledevice` auth mode
and verify callback, no session, and no hostname. The device is still reset with
`IOSCPP_TLS12` pinning TLS 1.2, `IOSCPP_NO_VERIFY` disabling the peer verify,
and `IOSCPP_SESSION` echoing the `SessionID` with a valid session version and
suite, so none of those is the cause on its own.

## Expected blockers

The entries here are the ones already known from the reference implementations. Some have
since been hit and are recorded under *Hit blockers* above; the rest are still expected.

### The device interface is not the first one

An iOS device in the normal (non-restore) mode exposes several USB configurations and
interfaces. The mux protocol lives on the vendor-specific interface with class `0xff`, subclass
`0xfe`, and protocol `0x02`. Claiming interface 0 instead of the matching one, or using the
default configuration, leaves the reads empty. The fix is to walk the descriptors and claim the
interface that matches the triple.

### A read shorter than a frame is not an error

libusb may return a transfer shorter than the requested length; it is not a short read in the
sense of a POSIX `read`. The transport must loop until it has the requested number of bytes or
the device stalls, rather than treating a partial transfer as end of stream.

### The mux header is big-endian and length includes the header

Every field in the mux header is network byte order, and `length` covers the whole frame,
header included. Reading `length` as the payload size over-reads by 16 bytes and desynchronizes
the stream. The payload length is `length - kMuxHeaderSize`.

### The v2 magic and sequence numbers are only for v2

`magic` (`0xfeedface`) and `tx_seq`/`rx_seq` exist only once the device negotiated mux
version 2. A v1 device has an 8-byte header, not 16. Sending a v2 header to a v1 device
desynchronizes it, so the header size depends on the negotiated version.

### The setup packet is required for v2

After the device answers the version request with major version 2, the host must send a
`MUX_PROTO_SETUP` packet with payload `\x07` before any connect. Skipping it makes the
device ignore the connect.

### `lockdownd` closes an unpaired session

`StartSession` on an unpaired host fails, and the device then drops the connection. Pairing must
complete, and the pairing record must be saved and reused, before any service is started.

### `lockdownd` speaks TLS after the session starts

After `StartSession`, every `lockdownd` message is wrapped in TLS, using the session key from
pairing. Sending a plaintext plist after the session starts is read as a TLS record and the
handshake fails.

### AFC paths are relative to the service root, and are UTF-8

`AFC` paths are relative to the service's root (`/` is the device's media root for
`com.apple.afc`), and are sent as UTF-8 bytes with a terminating NUL. A Windows path or a
non-UTF-8 path fails with an opaque `AFC_E_*` status.

### An install needs the package staged in `/PublicStaging`

`installation_proxy` does not take a local path. The IPA is first uploaded over `AFC` into the
staging directory and then installed from the device-side path. Installing without the upload, or
from a path the host can see but the device cannot, fails.

### A CoreDevice service is not on the mux link, but on the RSD tunnel

On iOS 17.4 and later, a `com.apple.dvt.*` service is not on a `lockdownd` port at all: it is on
an **RSD** address and port that a `CoreDeviceProxy` handshake hands out, and the tunnel carries
the device's IPv6 packets as data. Connecting to the service over the mux link, or reusing the
`lockdownd` port, fails.

### The RSD tunnel address is IPv6 and route-less

The address `CoreDeviceProxy` returns is a device-local IPv6 address that no host route covers, so a
socket to it is bound to the tunnel and sent over it by hand. Letting the host's routing table carry
the packets, or assuming the tunnel address is reachable, fails.

### RemoteXPC frames are not plists, and the flags word must be exact

RemoteXPC is the CoreDevice counterpart of the mux framing, not of the plist codec: a 16-byte header
whose flags word must be set exactly, then an `xpc` dictionary. Treating a RemoteXPC frame as a plist,
or mis-setting the flags word, makes the device drop the connection.

### A CoreDevice service speaks `DTX`, not a plist

A `com.apple.dvt.*` service does not exchange plists. It exchanges `DTX` messages, which have their
own header and payload types. Sending a plist to a `dvt` service, or reading its first reply as one,
fails.
