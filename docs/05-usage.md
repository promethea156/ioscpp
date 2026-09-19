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

```cpp
auto afc = device.open_afc().value();
for (const auto &entry : afc.list("/DCIM").value())
{
    std::cout << (entry.is_directory() ? "d " : "- ") << entry.name << "\n";
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

## Install and uninstall an app

`install` uploads the IPA into `/PublicStaging` over `AFC` and then asks
`installation_proxy` to install it. `uninstall` names the bundle id.

```cpp
auto result = ioscpp::install(device, "app.ipa").value();
if (!result.success)
{
    std::cout << "install failed: " << result.failure_reason() << "\n";
}

ioscpp::uninstall(device, "com.example.app").value();
```

## Launch, check, and close an app

```cpp
ioscpp::launch(device, "com.example.app").value();

if (ioscpp::is_running(device, "com.example.app").value())
{
    std::cout << "running\n";
}

ioscpp::close(device, "com.example.app").value();
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

## Trace a run

`IOSCPP_TRACE` prints the protocol steps to `stderr`, and `IOSCPP_DUMP` appends the
TLS records the host sends to a file. Both are unset by default, and they are the only
environment variables the library reads.

```powershell
$env:IOSCPP_TRACE = "1"
$env:IOSCPP_DUMP = "clienthello.bin"
```
