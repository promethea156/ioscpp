# Blockers

Significant blockers hit during development, and how they were solved. Each entry keeps the
symptom, the cause, and the fix, so the same trap is not walked into twice.

The list is empty in the scaffold and fills in as the slices in
[`03-roadmap.md`](03-roadmap.md) are implemented. The entries below are the ones already known
from the reference implementations and are expected to be hit.

## Expected blockers

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
