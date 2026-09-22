# Usage

Copy-pasteable snippets, one feature at a time. Every fallible call returns a
`Result<T>`, so each snippet checks the result with `has_value()` and prints the error
instead of throwing (see [`07-error-model.md`](07-error-model.md)).

The snippets assume the headers are included as one:

```cpp
#include <ioscpp/ioscpp.hpp>
```

## Connect to a device

A `Device` is a connected, paired device. The `Transport` is the link, the `Pairing` is
the host's pairing record, and `connect` performs the mux negotiation, the `lockdownd`
handshake, and the pairing exchange if the host is not paired yet.

```cpp
ioscpp::crypto::Pairing pairing = ioscpp::crypto::Pairing::load("pairing.json").value();

auto transport = ioscpp::usb::UsbTransport::open().value();
auto device = ioscpp::Device::connect(*transport, pairing).value();

std::cout << device.udid() << " " << device.product_type() << " "
          << device.product_version() << "\n";
```

## Disconnect and reconnect

`disconnect` tears the connection down innermost first (TLS, streams, mux, transport) and is
idempotent. The transport is closed but owned by the caller, and a `Device` is tied to one transport,
so a reconnect destroys the `Device`, re-discovers the device by its serial, opens a fresh transport, and
connects again. A service stream the caller opened must be destroyed first, so its reset is sent while the
link is still up.

```cpp
afc.reset();

device->disconnect();
device.reset();
transport.reset();

for (const auto &id : ioscpp::usb::UsbTransport::list().value())
{
    if (id.serial == serial)
    {
        transport = ioscpp::usb::UsbTransport::open(id).value();
        device = ioscpp::Device::connect(*transport, pairing).value();
    }
}
```

## List the attached devices

```cpp
for (const auto &id : ioscpp::usb::UsbTransport::list().value())
{
    std::cout << id.serial << " " << id.product_id << "\n";
}
```

## Run over the mux directly

A `Connection` is the mux layer alone, without `lockdownd`. It is what a caller
uses to speak a raw service on a known port.

```cpp
auto transport = ioscpp::usb::UsbTransport::open(ioscpp::usb::DeviceId{}).value();
auto connection = ioscpp::Connection::open(*transport).value();
auto stream = connection.connect(62078).value(); // lockdownd
```

## List a directory

A `READ_DIR` answer carries the entry names alone, so `list` returns names; the type
of an entry comes from `stat`.

```cpp
auto afc = device.open_afc().value();
for (const auto &entry : afc.list("/DCIM").value())
{
    std::cout << entry.name << "\n";
}
```

## Stat a path

```cpp
auto info = afc.stat("/DCIM/100APPLE").value();
if (info.has_value())
{
    std::cout << info->size << " bytes\n";
}
```

## Pull a file

```cpp
afc.pull("/DCIM/100APPLE/IMG_0001.JPG", "IMG_0001.JPG").value();
```

## Push a file

```cpp
afc.push("local.txt", "/Documents/local.txt").value();
```

## Read and write inside an app's container

`house_arrest` vends one installed app's own sandbox over `AFC`, so the same
`list`, `stat`, `pull`, and `push` act inside the app. The service is
`com.apple.mobile.house_arrest`; on iOS 17.4 and later the RSD tunnel carries it
as `com.apple.mobile.house_arrest.shim.remote`, so `start` has an overload for
each. The bundle id travels in a `VendContainer` (whole container) or
`VendDocuments` (`Documents` only) command after the service starts.

```cpp
auto container = ioscpp::HouseArrest::start(*rsd, "com.example.app").value();

for (const auto &entry : container.afc().list("/Documents").value())
{
    std::cout << entry.name << "\n";
}

container.afc().push("local.txt", "/Documents/local.txt").value();
```

## Install and uninstall an app

On iOS 17.4 and later, `install(Rsd&, ipa)` uploads the IPA into `/PublicStaging` over the RSD
`AFC` shim and then asks the RSD `installation_proxy` shim to install it.
`uninstall(Rsd&, bundle_id)` names the bundle id. The pre-17.4 `install(Device&, ipa)` and
`uninstall(Device&, bundle_id)` over the mux link stay as the fallback.

An install the device refuses is a normal outcome, so it is `success == false` with the reason
rather than an `Error`. A development-signed IPA whose provisioning profile lists the device
installs; an unsigned or App Store IPA is refused with `ApplicationVerificationFailed`.

```cpp
auto result = ioscpp::install(*rsd, "app.ipa").value();
if (!result.success)
{
    std::cout << "install failed: " << result.failure_reason() << "\n";
}

ioscpp::uninstall(*rsd, "com.example.app").value();
```

## Launch, check, and close an app

On iOS 17.4 and later, `launch(Rsd&, bundle_id)` opens the RSD
`com.apple.instruments.dtservicehub` service and launches the app over its `DTX`
process-control channel, returning the process id. `is_running(Rsd&, bundle_id)` resolves
the bundle id to a process id, and `close(Rsd&, pid)` kills that process. The pre-17.4
`launch(Device&, bundle_id)`, `is_running(Device&, bundle_id)`, and `close(Device&, pid)`
over the mux-link `com.apple.instruments.remoteserver` stay as the fallback.

```cpp
auto pid = ioscpp::launch(*rsd, "com.example.app").value();

if (ioscpp::is_running(*rsd, "com.example.app").value())
{
    std::cout << "running\n";
}

ioscpp::close(*rsd, pid).value();
```

## Query device info

```cpp
auto value = device.lockdown().get_value("com.apple.mobile.lockdown", "ProductVersion").value();
std::cout << value.string().value_or("?") << "\n";
```

## Open a raw service

Any `lockdownd` service is reachable by name. `start_service` performs the
`StartService` request and returns a `Stream` on the port the device allocated.

```cpp
auto stream = device.start_service("com.apple.mobile.screenshotr").value();
```

## Drive two devices on two threads

A `Connection`, a `Stream`, and a `Device` are not thread-safe; different devices
are independent, so one thread per device is how several are driven at once.

```cpp
auto a = std::async(std::launch::async, [&] { return run_tour("/dev/one"); });
auto b = std::async(std::launch::async, [&] { return run_tour("/dev/two"); });
```

## Work without a device

The `testing::MockTransport` is an in-memory `Transport`, so a caller can unit-test
the layer above it, and the whole protocol stack can be exercised without hardware.

```cpp
ioscpp::testing::MockTransport transport;
transport.feed(response_bytes);
auto connection = ioscpp::Connection::open(transport).value();
```

## Log what the library is doing

Logging is opt-in and process-wide: nothing is written until `set_logger`
installs a sink, and installing a second sink replaces the first. The level is the
most verbose the sink receives, so `Info` (the default) reports state changes and
`Debug` adds every protocol frame, without its payload. The library never passes
key material or a payload to the sink, so a log is safe to keep.

```cpp
ioscpp::set_logger(
    [](ioscpp::LogLevel level, std::string_view message) { std::cerr << message << "\n"; },
    ioscpp::LogLevel::Debug);
```

`clear_logger()` removes the sink, and `is_logging(level)` lets a caller avoid
building a message the sink would discard.

## Trace a run

`IOSCPP_TRACE` prints the low-level pairing and TLS steps to `stderr`
(certificates, the device key, and each TLS record header), which can include key
material, and `IOSCPP_DUMP` appends the TLS records the host sends to a file. Both
are unset by default, and they are the only environment variables the library
reads.

```powershell
$env:IOSCPP_TRACE = "1"
$env:IOSCPP_DUMP = "clienthello.bin"
```
