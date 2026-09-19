# Learning iOS Device Protocols

A module-by-module curriculum that uses this repository as the worked example. Each
module is one layer of the library, from raw bytes to a full file transfer, and each
points at the header that implements it and at the reference implementation it follows.

The tour in [`examples/demo/main.cpp`](examples/demo/main.cpp) is the hands-on
counterpart: run it next to the modules below.

## Module 0: The shape of the problem

An iOS device is not a filesystem with a shell. It is a set of services behind one
gatekeeper (`lockdownd`), reached over one multiplexed link (usbmux). Nothing works
until the host and the device share a pairing record.

Read: [`docs/01-objective.md`](docs/01-objective.md), [`docs/02-references.md`](docs/02-references.md).

## Module 1: Byte channels

The library never lets a protocol layer see USB. `Transport` is `read` and `write`,
nothing else, so a device, a mock, and a socket are interchangeable.

Read: [`include/ioscpp/transport.hpp`](include/ioscpp/transport.hpp),
[`include/ioscpp/testing/mock_transport.hpp`](include/ioscpp/testing/mock_transport.hpp).

## Module 2: Framing

A byte channel has no message boundaries, so a layer above it frames them. The mux
header is a 16-byte big-endian record with a magic; `Session` writes the header and its
payload as one write, and reads the header before the payload. The magic is read but not
checked, matching `usbmuxd`, because the device's own v2 value differs.

Read: [`include/ioscpp/protocol/usbmux.hpp`](include/ioscpp/protocol/usbmux.hpp),
[`include/ioscpp/session.hpp`](include/ioscpp/session.hpp).

## Module 3: Property lists

Every control message on the device is a property list. A plist is a small recursive
variant: null, boolean, integer, real, string, data, date, array, dictionary. The codec
parses the XML form the device sends and the binary form some services use.

Read: [`include/ioscpp/protocol/plist.hpp`](include/ioscpp/protocol/plist.hpp).

## Module 4: The mux protocol

The device multiplexes TCP-like connections over one USB interface. The host negotiates
a version, optionally sends a setup packet, then opens a port with SYN, SYN|ACK, ACK.
Data frames carry a sequence number and are acknowledged.

Read: [`include/ioscpp/connection.hpp`](include/ioscpp/connection.hpp),
[`include/ioscpp/stream.hpp`](include/ioscpp/stream.hpp),
[`docs/04-blockers.md`](docs/04-blockers.md).

## Module 5: Pairing and `lockdownd`

`lockdownd` on port `62078` is the gatekeeper. The host generates a key pair and a
self-signed certificate, exchanges them in a `Pair` request, derives a session key, and
wraps every later message in TLS. Only then does `StartService` hand out a port.

Read: [`include/ioscpp/crypto/pairing.hpp`](include/ioscpp/crypto/pairing.hpp),
[`include/ioscpp/lockdown.hpp`](include/ioscpp/lockdown.hpp).

## Module 6: `AFC`

`AFC` is the file service. A 40-byte header (`CFA6LPAA` and four little-endian 64-bit
fields) prefixes every request and reply. `list`, `stat`, `pull`, and `push` are all
built on it.

Read: [`include/ioscpp/afc.hpp`](include/ioscpp/afc.hpp),
[`docs/06-afc-protocol.md`](docs/06-afc-protocol.md).

## Module 7: Applications

`installation_proxy` installs and removes apps; process control launches and closes
them. Both are plist services, so they reuse Module 3 and Module 5.

Read: [`include/ioscpp/app.hpp`](include/ioscpp/app.hpp).

## Module 8: Putting it together

`Device` composes the modules: a transport, a mux connection, a pairing, a `lockdownd`
client, and the device's identity. The demo walks the implemented features once; walking
every feature once is the goal of Slice 8 (`03-roadmap.md`).

Read: [`include/ioscpp/device.hpp`](include/ioscpp/device.hpp),
[`examples/demo/main.cpp`](examples/demo/main.cpp).
