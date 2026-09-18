# The Error Model

Nothing in `ioscpp` throws. Every fallible operation returns a `Result<T>`, which is
either a value or an `Error`. This document explains why, and what an `Error` carries.

## Why not exceptions

The library is embeddable. A caller that wants to link it into a program with
exceptions disabled (`-fno-exceptions`), or that wants to keep its own exception
boundary, should not have to catch anything to use it. A `Result<T>` is also visible in
the type, so a caller that ignores an error has to do so deliberately.

The one place the library could throw is allocation. A `std::bad_alloc` from the standard
library is not caught and turned into an `Error`; that is a deliberate choice, because a
program that is out of memory has few good options anyway.

## `Result<T>` is `tl::expected`

`Result<T>` is an alias for `tl::expected<T, Error>`:

```cpp
template <typename T>
using Result = tl::expected<T, Error>;

using Status = tl::expected<void, Error>;
```

`tl::expected` is used rather than a new type so its whole interface is available:
`has_value()`, `operator*`, `operator->`, `value_or`, `and_then`, `map`, and `or_else`.
The alias exists so the public API does not depend on the spelling of the library that
provides it, which makes moving to `std::expected` in C++23 a change here alone.

`Status` is the outcome of an operation with nothing to report. It is the same type with
`void` as the value, so `Status` is either success or an `Error`.

## `Error` carries a code and a message

```cpp
struct Error
{
    ErrorCode code = ErrorCode::Protocol;
    std::string message;
};
```

The message does not repeat the library's name, because the type already carries it: a caller
prints `error: <message>`.

The code is what a caller branches on, without matching on the message. The distinction
between the codes is not how severe the problem is but who is being reported on:

- `InvalidArgument` and `Io` are the caller's or the host's side.
- `Transport` is the link.
- `Protocol` and `Device` are the device's.
- `Crypto` is the pairing key or the TLS session.

## Where the boundaries are

An error is returned, rather than an exception thrown, at every boundary:

- A local file that cannot be read or written is `ErrorCode::Io`.
- A transport that fails, or a transfer that is short, is `ErrorCode::Transport`.
- A frame the device did not send as the protocol requires is `ErrorCode::Protocol`.
- A device that refused a request and gave a reason is `ErrorCode::Device`, with the
  reason as the message. An AFC `STATUS` and a `lockdownd` error are both this.
- A key that cannot be found, generated, or used to sign is `ErrorCode::Crypto`.

## A refused request is not an error

Some outcomes are normal and are not an `Error`:

- A path that does not exist is an empty `optional` from `stat`.
- An install the device refuses is `PackageResult::success == false` with the reason,
  not an `Error`.
- An app that is not running is `is_running == false`, not an `Error`.

An `Error` means the operation could not be completed, not that the device said no.

## The caller's side

A caller checks the result before using the value:

```cpp
auto entries = ioscpp::list(afc, "/DCIM");
if (!entries)
{
    std::cerr << "error: " << entries.error().message << "\n";
    return;
}
for (const auto &entry : *entries) { /* ... */ }
```

`value()` and `error()` assert when they are called on the wrong alternative. The library
never calls them without checking, and neither should a caller.
