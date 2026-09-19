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

### The generated certificates carry an empty serial number

**Symptom.** OpenSSL 3.0 rejects every certificate in the pair record with
`[SSL] PEM lib`. Python's `ssl.SSLContext.load_cert_chain` fails, and the Rust
`cryptography` parser reports `TbsCertificate::serial` as `InvalidValue`. mbedTLS
accepts them. The DER `tbsCertificate` holds `02 00`, an `INTEGER` with a
zero-length value.

**Cause.** `generate_certificate` sets the serial with `mbedtls_mpi_lset(&serial, 0)`,
and mbedTLS writes a zero MPI as a zero-length `INTEGER` (`02 00`). RFC 5280
requires a positive serial, and OpenSSL 3.0 enforces it while mbedTLS does not
(`src/crypto/pairing.cpp`). The reference `pymobiledevice3` sets the serial to one
(`_SERIAL` in `ca.py`), so the value is not the trap and the zero-length encoding is.

**Impact.** A tool that reads the pair record with OpenSSL, such as a
`pymobiledevice3` comparison or `libimobiledevice`, cannot load the host
certificate. The device itself also rejects the handshake (see the entry below).

**Fix.** `generate_certificate` sets the serial to one, so mbedTLS writes `02 01 01`
(`src/crypto/pairing.cpp`). The device then answers the ClientHello and the handshake
completes.

### The device resets the TLS handshake after the ClientHello

**Symptom.** With a well-formed ClientHello (record version `0x0303`, the full
suite list, and a valid `key_share`), the device acknowledges it and then sends a
mux control frame `type=3 socketIsClosed sock_receive returned errno 54` and a
reset whose reason is `sessionUpcall connection closed`. No ServerHello arrives,
and `mbedtls_ssl_handshake` returns `MBEDTLS_ERR_SSL_CONN_EOF`.

**Cause.** The host certificate's serial number. The pairing chain wrote the serial as
a zero-length `INTEGER` (`02 00`), which the device's TLS rejects as the client
identity, so it closes the connection before the handshake finishes. The device is on
iOS 18.7.8.

The ClientHello itself is not the cause: replaying the captured reference
ClientHello byte for byte over the same link still resets. Nor is the mux framing or
the version (v1 and v2 both reset), the TLS version (TLS 1.2 and 1.3 both reset), the
pre-TLS delay, the verify mode, or the session id echo. Bisecting the named ClientHello
differences (`IOSCPP_REFERENCE_*`) also left the reset in place.

**Fix.** `generate_certificate` writes a non-zero serial (`src/crypto/pairing.cpp`,
see the entry above). With it, the device answers with a ServerHello, the handshake
completes, and `ioscpp_usb_example` prints the device's `ProductType` and
`ProductVersion`. The pair record must be regenerated, because it carries the old
certificate.

**State.** `TlsSession::start` presents the host leaf certificate and key (the
`pymobiledevice3` choice) with `config_defaults`, the `libimobiledevice` auth mode
and verify callback, no session, and no hostname. The `IOSCPP_TLS12`, `IOSCPP_NO_VERIFY`,
and `IOSCPP_SESSION` probes still reset with the old certificate and are not needed for the
fix; the `IOSCPP_REPLAY` probe sends a captured ClientHello in place of the built one, and
`IOSCPP_MUX_V1` pins the mux to v1; both reset, which rules out the ClientHello and the
framing as the cause. All of them are retired (issue #20), so only `IOSCPP_TRACE` and
`IOSCPP_DUMP` remain.

**Reproduce.** Delete `%USERPROFILE%\.ioscpp\<udid>` so the next run re-pairs, then run
`ioscpp_usb_example` and answer the trust prompt. With the zero-length serial the device
resets after the ClientHello; with the serial fixed it answers and the run completes.

**Wire capture.** A USBPcap capture of a failing run (device on `USBPcap1`, root hub
`USBROOT(0)#USB(7)`, address `1.35.0`) shows the sequence at the wire. The ClientHello
is one bulk `OUT` transfer (frame 1267, record version `0x0303`, 432 bytes), byte
identical to `IOSCPP_DUMP` except the 32-byte random. The device acknowledges it
(frame 1268), and 6 ms later the device sends a mux control frame `type=3` reading
`socketIsClosed sock_receive returned errno 54` (frame 1270) and a data frame reading
`sessionUpcall connection closed` (frame 1272), then sends nothing more. No ServerHello
and no TLS alert arrive, so the device's `lockdownd` takes the record layer and closes
before it answers.

**Reference capture.** Reached through Apple's own stack. Installing the *Apple Mobile
Device Support* package and putting the device back on Apple's driver starts the
*Apple Mobile Device Service*, and `pymobiledevice3` then reaches the device through
Apple's `usbmuxd` (`docs/09-platform-setup.md`). `USBPcap` captures below the driver, so
the reference is captured on the same device and the same host as the failing run.

The `libimobiledevice` `usbmuxd` v1.1.1 was the earlier route and did not get there:
it finds the device and claims the mux interface, but then dies with a heap corruption
(`0xc0000374`) on its bundled libusb 1.0.24, an access violation (`0xc0000005`) with
libusb 1.0.30 once the first client connects, and `Mux error (-8)` on the first
`lockdownd` connect.

### The device answers the reference ClientHello with a ServerHello

**Result.** The decisive capture for the reset above. With the device on Apple's driver and
`pymobiledevice3` driving it through Apple's `usbmuxd`, the device **does** answer. The
host's ClientHello is frame 531 (record version `0x0301`, 512 bytes), and the device answers
with a ServerHello in frame 537 (handshake type `0x02`, server version `0x0303`, a 32-byte
session id, suite `0xc030`, and the `renegotiation_info` and `extended_master_secret`
extensions), then its Certificate in frame 541. No reset follows.

**Conclusion.** The device accepts an OpenSSL ClientHello and resets mbedTLS's, so the fault is
on `ioscpp`'s side and not in the device, the pairing, the session, or the mux state. The named
differences in the entry below are the candidate causes, and the certificate serial behind them is
the actual one.

**Artifacts.** The reference run, the failing run, both ClientHellos, and the ServerHello
are in [`captures/`](captures/).

**Reproduce.** Capture while `pymobiledevice3 lockdown info` runs, then read the two frames:

```powershell
tshark -i '\\.\USBPcap1' -a duration:30 -w reference-run.pcapng
tshark -r reference-run.pcapng -Y 'usb.src == "host" && usb.data_len > 200'
```

The ClientHello is the frame whose payload reads `16 03 01` and handshake type `01`;
`tools/compare-clienthello.py parse` decodes it and `diff` names the differences from
`IOSCPP_DUMP`.

### The ClientHello differs from the OpenSSL reference

**Symptom.** A reference ClientHello, built with the exact `pymobiledevice3` context
(`PROTOCOL_TLS_CLIENT`, min TLS 1.2, max TLS 1.3, `ALL:!aNULL:!eNULL:@SECLEVEL=0`,
`OP_LEGACY_SERVER_CONNECT`, `CERT_NONE`, no hostname) and captured through
`ssl.MemoryBIO`, is 517 bytes; `ioscpp`'s is 432 bytes. Both are well formed and the
device acks both, so this is the closest named lead, not a proven cause.

**Named differences.**

| Item | Reference (OpenSSL) | `ioscpp` (mbedTLS) |
| --- | --- | --- |
| record version | `0x0301` | `0x0303` |
| cipher suites | 75, `0x1302` first | 109, `0x1303` first |
| `supported_groups` | no brainpool, `x25519` first | adds `0x001a`/`0x001b`/`0x001c` |
| `signature_algorithms` | 22, includes `ed25519`, `ed448`, `rsa_pss_pss_*`, SHA1 | 9, `rsa_pss_rsae_*` and `rsa_pkcs1_*` only |
| `ec_point_formats` | `uncompressed` + both compressed | `uncompressed` only |
| `psk_key_exchange_modes` | `psk_dhe_ke` | `psk_dhe_ke` + `psk_ke` |
| `padding` | 126 bytes, record padded to 512 | absent |
| `key_share`, `supported_versions`, `session_ticket` | same | same |

The reference is OpenSSL, so the differences are mostly stack defaults. The capture in the
entry above shows the device answers the reference ClientHello, so the reset looked like one of
these differences. Bisecting them with the `IOSCPP_REFERENCE_*` knobs (since retired, issue #20)
then showed that none is the cause on its own: even a ClientHello matching the reference on the record version, the
extension set, and `ec_point_formats` is reset, and so is a byte-for-byte replay of the reference
ClientHello. The reset was the certificate serial in the entry above, and these differences are the
mbedTLS stack defaults `ioscpp` keeps.


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
