# Blockers

Significant blockers hit during development, and how they were solved. Each entry keeps the
symptom, the cause, and the fix, so the same trap is not walked into twice.

It fills in as the slices in [`03-roadmap.md`](03-roadmap.md) are implemented: the entries
below are the ones hit so far, and the ones known from the reference implementations and
expected.

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

### The USB serial descriptor carries its trailing NUL padding

**Symptom.** The pairing record was written as `%USERPROFILE%\.ioscpp\<serial>`
with no `.plist` suffix, so the next run did not find it and paired again. The serial
printed with 20 trailing spaces, and the path held 20 NUL bytes before `.plist`.

**Cause.** The device's `iSerial` string descriptor is a fixed 44-byte field whose tail
is NUL padded, and `libusb_get_string_descriptor_ascii` returns that field's length
rather than the string's, so `device_serial` built a 44-byte `std::string` of which 20
bytes are NUL. `std::filesystem` keeps the NULs, but `fopen` is a C string and stops
at the first one, so `...<serial>.plist` was written as `...<serial>`
(`src/usb/usb_transport.cpp`).

**Fix.** `device_serial` trims the trailing NULs, so the serial is the 24 characters
`idevice_id -l` prints and the record is `<serial>.plist`.

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

**Reproduce.** Delete `%USERPROFILE%\.ioscpp\<serial>.plist` so the next run re-pairs, then
run `ioscpp_usb_example` and answer the trust prompt. With the zero-length serial the
device resets after the ClientHello; with the serial fixed it answers and the run completes.

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


### The device answers `READ_DIR` with one `DATA` and no `STATUS`

**Symptom.** After the mux fix, the device test hung in `Afc::list`: the trace showed
the device sent one `DATA` packet holding the whole root listing (`entire=151`) and then
nothing more. `ctest -R "^device$"` never returned.

**Cause.** `Afc::list` sent `READ_DIR` and looped until a `STATUS`, but the device ends a
directory listing with the single `DATA` packet. `libimobiledevice`'s `afc_read_directory`
calls `afc_receive_data` once and returns, and `pymobiledevice3`'s `listdir` waits for one
response; neither reads a trailing `STATUS`. The mock test and `docs/06-afc-protocol.md`
encoded the wrong belief, so the mock confirmed it.

**Fix.** `Afc::list` sends `READ_DIR` with `transact` and parses the one `DATA` answer
(`src/afc.cpp`). The mock no longer feeds a `STATUS` after the listing
(`tests/afc_test.cpp`).

### The device's listing carries names alone

**Symptom.** With the hang fixed, the device test failed with `the media root listed no
directory`. The raw answer was `.`, `..`, `Downloads`, `Books`, and so on, with no `st_*`
keys.

**Cause.** The device's `READ_DIR` answer is the entry names alone, each NUL-terminated;
the stat keys the mock fed are not part of it. `DirEntry::is_directory` was therefore
always false.

**Fix.** The device test lists the root for a non-empty answer and stats `/` for the
directory check (`tests/device_test.cpp`). The parser still reads stat keys when a device
sends them.

### `FILE_OPEN` carries the mode first, and answers `FILE_OPEN_RES`

**Symptom.** The device test failed with `push: the device sent an unexpected AFC
operation`. The device answered `FILE_OPEN` with opcode `0x0E`, which `transact` rejected.

**Cause.** Two format errors: `Afc::open_file` sent the path and then the mode, while both
`libimobiledevice`'s `afc_file_open` and `pymobiledevice3`'s `FopenRequest` send the
8-byte mode first and then the path; and the answer's opcode is `FILE_OPEN_RES` (`0x0E`),
which `afc_receive_data` accepts but `transact` did not.

**Fix.** `Afc::open_file` puts the mode first (`src/afc.cpp`), and `transact` accepts
`kOpFileOpenRes`.

### The device caps one message at 65535 bytes

**Symptom.** With list, stat, and open fixed, the device answered a `FILE_WRITE` with a
control frame reading `asyncReadComplete, message was too large (65536 bytes, max = 65535)`,
and the test hung.

**Cause.** `Afc` chunked `FILE_WRITE`/`FILE_READ` at 64 KiB (`kChunkSize`), so the
device's AFC message (`entire_length`) reached 65584 bytes, over the device's 16-bit cap.

**Fix.** `Afc` chunks at 32 KiB (`src/afc.cpp`), so a message stays well under the cap.

### The `CoreDeviceProxy` service resets a plaintext handshake

**Symptom.** The device test connected to `CoreDeviceProxy`, sent the `CDTunnel` handshake
request, and the device answered a mux control frame reading
`socketIsClosed sock_receive returned errno 54` and a reset whose reason is
`sessionUpcall connection closed`, with no handshake answer.

**Cause.** The `StartService` answer for `CoreDeviceProxy` sets `EnableServiceSSL`, so the
service requires TLS before it exchanges any data. The library sent the `CDTunnel` frame in
plaintext, and the service closed the port. The frame bytes themselves match the reference, so
the missing TLS was the whole fault.

**Fix.** `Tunnel::open` wraps the service stream in `TlsSession`, using the pairing record,
when the `StartService` answer sets `EnableServiceSSL`, and the handshake then goes over TLS
(`src/tunnel.cpp`). `Lockdown::start_service` returns the flag alongside the port
(`src/lockdown.cpp`), so a caller can no longer ignore it.

**Note.** The `Tunnel` is a pimpl, like `Lockdown`, because the TLS session binds to the
`Stream`'s address. A `Stream` member moved after the TLS session starts leaves the session's
pointer dangling, and the first write then crashes.

## Expected blockers

The entries here are the ones already known from the reference implementations. Most have
since been hit and are marked **Hit** below; the rest are still expected.

### The device interface is not the first one

An iOS device in the normal (non-restore) mode exposes several USB configurations and
interfaces. The mux protocol lives on the vendor-specific interface with class `0xff`, subclass
`0xfe`, and protocol `0x02`. Claiming interface 0 instead of the matching one, or using the
default configuration, leaves the reads empty. The fix is to walk the descriptors and claim the
interface that matches the triple.

**Hit.** The transport searches every configuration and claims the matching interface
(`src/usb/usb_transport.cpp`), and the device answers on a real device (`tests/device_test.cpp`).

### A read shorter than a frame is not an error

libusb may return a transfer shorter than the requested length; it is not a short read in the
sense of a POSIX `read`. The transport must loop until it has the requested number of bytes or
the device stalls, rather than treating a partial transfer as end of stream.

**Hit.** `UsbTransport::read` buffers a partial transfer, and the TLS records arrive as several
short transfers and reassemble (`src/usb/usb_transport.cpp`).

### The mux header is big-endian and length includes the header

Every field in the mux header is network byte order, and `length` covers the whole frame,
header included. Reading `length` as the payload size over-reads by 16 bytes and desynchronizes
the stream. The payload length is `length - kMuxHeaderSize`.

**Hit.** `protocol::MuxHeader::decode` is big-endian and `Session` reads
`length - kMuxHeaderSize` (`src/session.cpp`), and the device answers.

### The v2 magic and sequence numbers are only for v2

`magic` (`0xfeedface`) and `tx_seq`/`rx_seq` exist only once the device negotiated mux
version 2. A v1 device has an 8-byte header, not 16. Sending a v2 header to a v1 device
desynchronizes it, so the header size depends on the negotiated version.

**Hit.** `Session::send` sets the v2 magic and advances `tx_seq` per frame
(`src/session.cpp`), and the device accepts the frames.

### The setup packet is required for v2

After the device answers the version request with major version 2, the host must send a
`MUX_PROTO_SETUP` packet with payload `\x07` before any connect. Skipping it makes the
device ignore the connect.

**Hit.** `Connection::open` sends the setup packet after a v2 answer
(`src/connection.cpp`), and the device answers the connect.

### `lockdownd` closes an unpaired session

`StartSession` on an unpaired host fails, and the device then drops the connection. Pairing must
complete, and the pairing record must be saved and reused, before any service is started.

**Hit.** `Device::connect` pairs and saves the record before `StartSession`
(`src/device.cpp`), and the device starts the session.

### `lockdownd` speaks TLS after the session starts

After `StartSession`, every `lockdownd` message is wrapped in TLS, using the session key from
pairing. Sending a plaintext plist after the session starts is read as a TLS record and the
handshake fails.

**Hit.** `StartSession` switches the stream to `TlsSession`, and the handshake completes
(`src/crypto/pairing.cpp`).

### AFC paths are relative to the service root, and are UTF-8

`AFC` paths are relative to the service's root (`/` is the device's media root for
`com.apple.afc`), and are sent as UTF-8 bytes with a terminating NUL. A Windows path or a
non-UTF-8 path fails with an opaque `AFC_E_*` status.

### An install needs the package staged in `/PublicStaging`

`installation_proxy` does not take a local path. The IPA is first uploaded over `AFC` into the
staging directory and then installed from the device-side path. Installing without the upload, or
from a path the host can see but the device cannot, fails.

**Hit.** `install` uploads the IPA into `/PublicStaging` over `AFC` and passes that device-side
path as `PackagePath` (`src/app.cpp`); a `stat` of the staged file reports its full size.

### The mux-link `installation_proxy` accepts a connection but does not answer on iOS 17+

`lockdownd` starts `com.apple.mobile.installation_proxy` over the mux link and the device accepts the
port, but on iOS 17+ it does not answer an `Install` or a `Browse`: the request goes out and the
connection then times out with no reply. The mux-link service is the legacy path, and `pymobiledevice3`
reaches the same service over the **RSD** shim
(`com.apple.mobile.installation_proxy.shim.remote`), which the RSD tunnel carries
(`docs/03-roadmap.md`, Slice 9).

**Hit.** `ioscpp_device_tests` staged an arm64 sample IPA (`MinimumOSVersion` 12.4) into
`/PublicStaging` and sent the `Install` command `pymobiledevice3 apps install` sends, with Developer
Mode on and the device unlocked; the device accepted the `installation_proxy` connection and never replied
(`tests/device_test.cpp`). The `AFC` upload over the same mux link works, so the link is not the problem.

**Fix.** The mux-link path stays as the pre-17.4 fallback, and `install(Rsd&)` and `uninstall(Rsd&)` now
reach the same service over the RSD shim (`src/app.cpp`); the device test's opt-in round trip is verified on an
iOS 18.7.8 device.

### The plist length prefix is the plist size alone, not the size plus the prefix

A service frames a plist the way `lockdownd` does: a 4-byte big-endian length, then the XML plist. The
length is the plist size alone. Writing the size plus the prefix makes the device read four bytes past the
message, wait for them, and reset the connection.

**Hit.** `install(Rsd&, ipa)` staged the IPA over the RSD `AFC` shim and sent the `Install` command, and
the device reset the connection; the staged file stat'd at its full size, so the upload was not the problem.
The private `PlistService` wrote the size plus the prefix (`src/app.cpp`); it now writes the size alone
through the shared `PlistService` (`src/plist_service.cpp`), and the installer shim answers with a status.

### An unsigned package is refused with `ApplicationVerificationFailed`

`installation_proxy` verifies a package's signature before it installs it. A development-signed IPA whose
provisioning profile lists the device installs; an unsigned or App Store IPA is refused with
`ApplicationVerificationFailed`, which is a device-side policy answer, not a protocol error.

**Hit.** `install(Rsd&, ipa)` sent an unsigned IPA and the installer shim answered
`ApplicationVerificationFailed`; `uninstall(Rsd&, bundle_id)` removed the same app over the installer shim,
so the shim's command framing is right and only the signature is missing. A development-signed IPA whose
provisioning profile lists the device then installed and uninstalled cleanly, so the path is verified.

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

RemoteXPC is the CoreDevice counterpart of the mux framing, not of the plist codec: a fixed header
(the magic, the flags word, the body length, the message id) whose flags word must be set exactly, then
an `xpc` object. Treating a RemoteXPC frame as a plist, or mis-setting the flags word, makes the device
drop the connection.

### A CoreDevice service speaks `DTX`, not a plist

A `com.apple.dvt.*` service does not exchange plists. It exchanges `DTX` messages, which have their
own header and payload types. Sending a plist to a `dvt` service, or reading its first reply as one,
fails.
