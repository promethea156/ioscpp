# The AFC Protocol

`AFC` (Apple File Conduit) is the file service on an iOS device. It is reached by
asking `lockdownd` to start `com.apple.afc`, and it carries binary packets rather
than plists. This document is the wire format that `list`, `stat`, `pull`, and `push`
are built on.

The reference implementation is `libimobiledevice`'s `src/afc.c` and `src/afc.h`.

## The packet

Every packet starts with a 40-byte header, followed by an optional payload:

| Offset | Size | Field | Encoding | Meaning |
| --- | --- | --- | --- | --- |
| 0 | 8 | `magic` | ASCII | Always `CFA6LPAA`. |
| 8 | 8 | `entire_length` | little-endian `uint64` | The header plus the whole payload. |
| 16 | 8 | `this_length` | little-endian `uint64` | The header plus the part of the payload in this packet. |
| 24 | 8 | `packet_num` | little-endian `uint64` | A counter the device echoes back. |
| 32 | 8 | `operation` | little-endian `uint64` | The operation, from the table below. |
| 40 | … | `payload` | operation-specific | The path, length, or data. |

Unlike the mux header, every numeric field is **little-endian**, and `packet_num`
is checked: the device answers with the same number, and a reply that does not match
means the stream is desynchronized.

## Operations

| Value | Name | Direction | Payload |
| --- | --- | --- | --- |
| `0x01` | `STATUS` | device → host | An 8-byte little-endian error code. |
| `0x02` | `DATA` | device → host | The requested bytes. |
| `0x03` | `READ_DIR` | host → device | The directory path, NUL-terminated. |
| `0x04` | `READ_FILE` | host → device | The file path, NUL-terminated. |
| `0x05` | `WRITE_FILE` | host → device | The file path, NUL-terminated. |
| `0x06` | `WRITE_PART` | host → device | The bytes to append. |
| `0x07` | `TRUNCATE` | host → device | An 8-byte length. |
| `0x08` | `REMOVE_PATH` | host → device | The path, NUL-terminated. |
| `0x09` | `MAKE_DIR` | host → device | The path, NUL-terminated. |
| `0x0A` | `GET_FILE_INFO` | host → device | The path, NUL-terminated. |
| `0x0B` | `GET_DEVINFO` | host → device | Nothing. |
| `0x0D` | `FILE_OPEN` | host → device | The path, NUL-terminated, then an 8-byte mode. |
| `0x0F` | `FILE_READ` | host → device | An 8-byte length. |
| `0x10` | `FILE_WRITE` | host → device | The bytes to write. |
| `0x11` | `FILE_SEEK` | host → device | A file handle and an 8-byte offset. |
| `0x14` | `FILE_CLOSE` | host → device | A file handle. |

The full operation set, including the v2 additions, is in `libimobiledevice`'s
`src/afc.h`.

## A reply is `STATUS` or `DATA`

Every request is answered with either a `STATUS` packet carrying an error code, or a
`DATA` packet carrying the reply. A `STATUS` with a non-zero code is the device's
reason, and is returned as an `ErrorCode::Device` error. A request that succeeds but
has nothing to return is a `STATUS` with code `0`.

## Listing a directory

`list` is a `READ_DIR` request followed by a loop:

1. The host sends `READ_DIR` with the directory path.
2. The device answers with a `DATA` packet whose payload is the entry name,
   NUL-terminated, followed by the entry's 8-byte little-endian `stat` fields:
   `mtime`, `size`, and `mode`, in that order.
3. Steps 2 repeats until the device sends a `STATUS` with code `0`, which ends the
   listing.

The `mode` is a POSIX `st_mode`, so `S_ISDIR` and `S_ISREG` work on it unchanged.

## Statting a path

`stat` is a `GET_FILE_INFO` request. The device answers with a `DATA` packet of
eight little-endian 64-bit fields; the first three are `mtime`, `size`, and `mode`.
A path that does not exist is a `STATUS` with a non-zero code, which `stat` returns
as an empty `optional`, not as an error.

## Pulling a file

`pull` is a `FILE_OPEN` (read mode), a loop of `FILE_READ`, and a `FILE_CLOSE`:

1. The host sends `FILE_OPEN` with the path and mode `0`.
2. The device answers with a `DATA` packet carrying a 4-byte file handle.
3. The host sends `FILE_READ` with the handle and the number of bytes it wants.
4. The device answers with a `DATA` packet of the bytes, or a zero-length `DATA`
   when the file is exhausted.
5. Steps 3 and 4 repeat until the short read.
6. The host sends `FILE_CLOSE` with the handle.

Each chunk is written to the local file as it arrives, so the file is never held in
memory.

## Pushing a file

`push` is a `FILE_OPEN` (write mode), a loop of `FILE_WRITE`, and a `FILE_CLOSE`:

1. The host sends `FILE_OPEN` with the path and mode `2`.
2. The device answers with a `DATA` packet carrying a 4-byte file handle.
3. The host sends `FILE_WRITE` with the handle and a chunk of the file.
4. The device answers with a `STATUS` with code `0`.
5. Steps 3 and 4 repeat until the file is exhausted.
6. The host sends `FILE_CLOSE` with the handle.

The device creates the file, or truncates it if it already exists. A path whose parent
does not exist is a `STATUS` with a non-zero code, which `push` returns as an
`ErrorCode::Device` error.
