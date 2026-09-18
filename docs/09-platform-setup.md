# Platform Setup

How to build `ioscpp` and reach a device on Linux, macOS, and Windows. The build is
the same everywhere; only the toolchain, the USB access, and the daemon to stop differ.

This is a setup document, not a feature document. The library's behavior is the same on
every platform; what differs is what the host has to grant it. Until a device is
reachable, the device-free tests still build and pass.

## Common

- A C++20 compiler, CMake 3.24 or newer, and Git.
- Dependencies are fetched at configure time: `tl::expected`, mbedTLS, and, with
  `IOSCPP_BUILD_USB=ON` (the default at top level), libusb.
- `IOSCPP_BUILD_USB=OFF` builds the core with no USB dependency at all, for a consumer
  that supplies its own `Transport`.

## One owner per device

The device's mux interface is claimed by one process at a time. Stop any `usbmuxd` before
running `ioscpp`, exactly as `adb kill-server` is needed for Android. With `usbmuxd` running,
`ioscpp` still enumerates the device but cannot claim the interface.

The device also asks for trust the first time this host pairs: tap *Trust This Computer?* on
the device, or the pairing exchange blocks until it is answered.

## Linux

### Build

```
sudo apt-get update
sudo apt-get install -y build-essential cmake git pkg-config
```

The libusb build is configured with its udev backend off (`src/usb/CMakeLists.txt`), so no
`libudev` package is needed; enumeration uses the netlink backend.

### USB access

Opening the device needs permission. Either run as root, or grant the user access with a udev
rule for Apple's vendor id. Create `/etc/udev/rules.d/39-ioscpp.rules`:

```
SUBSYSTEM=="usb", ATTR{idVendor}=="05ac", MODE="0666", GROUP="plugdev"
```

Then reload:

```
sudo udevadm control --reload-rules
sudo udevadm trigger
```

Replug the device, and add the user to `plugdev` if it is not a member. The library detaches
the kernel driver itself when it claims the interface (`src/usb/usb_transport.cpp`).

### Stop the daemon

```
sudo systemctl stop usbmuxd
```

## macOS

### Build

```
xcode-select --install
brew install cmake
```

No driver and no udev rule are needed: libusb reaches the device through IOKit.

### Stop the daemon

macOS ships a system `usbmuxd`; a brew `libimobiledevice` install adds its own. Stop
whichever is running:

```
# The brew service, if libimobiledevice was installed through brew
brew services stop usbmuxd
# The system daemon, if it is the one holding the device
sudo launchctl stop com.apple.usbmuxd
```

The system daemon may be respawned by `launchd`; if the interface is still claimed, unload
its plist instead of stopping it.

## Windows

### Build

Install **Visual Studio 2022** with the *Desktop development with C++* workload, and CMake
3.24 or newer. Run the build from a **Developer PowerShell for VS 2022**. Winsock is linked
automatically (`CMakeLists.txt`).

### USB driver

libusb on Windows cannot open the device through Apple's `usbaapl64` driver; the mux
interface needs a libusb-compatible driver bound to it. Bind one to the interface only, so the
rest of the device keeps Apple's driver.

1. Install [Zadig](https://zadig.akeo.ie/).
2. Put the device in the normal (unlocked) mode and plug it in.
3. In Zadig, enable *List All Devices*, pick the Apple device, and install a driver.
4. Replug the device.

Replacing the whole composite device's driver instead of one interface can make iTunes
re-pair it; binding the single interface avoids that.

**Use libusb-win32, not WinUSB.** libusb's WinUSB backend never sends
`SET_CONFIGURATION` to the device; its libusb0 backend does. A device in its initial
USB mode does not sit in the configuration that carries the mux interface, so a WinUSB
driver leaves the mux interface unreachable, and `list` reports `no device attached`.
libusb-win32 (`libusb0.sys`) is needed for `ioscpp` to select the configuration
itself, as `usbmuxd` does. A device that is already in the right configuration, for
example because Apple Mobile Device Support selected it, works with either.

The libusb0 driver also needs `libusbK.dll` in `System32`; the libusbK setup
provides it.

### Stop the daemon

Stop the `usbmuxd` service, or stop Apple Mobile Device Support, so it releases the
interface.

## Developer tooling

- Format with **clang-format 19.1.1**; CI pins that version. Check it with
  `cmake --build build --target format-check`.
- Optional API docs need [Doxygen](https://www.doxygen.nl/), then
  `cmake --build build --target ioscpp_docs`.

## Comparing against a reference

`libimobiledevice`, `pymobiledevice3`, and `go-ios` reach the same protocols, so
one of them is a second opinion when `ioscpp` disagrees with a device. Only one
process can own the mux interface at a time, so a reference and `ioscpp` cannot talk
to the same device together on the host where `usbmuxd` lives; the options below
work around that.

- **On a host with a `usbmuxd`.** `libimobiledevice` and `pymobiledevice3` reach
  the device through it, so run `usbmuxd`, point them at it, and capture the USB
  traffic with **USBPcap** (Wireshark) instead of `ioscpp` to compare the bytes.
  `USBPcap` captures below the driver, so it sees the traffic whichever driver
  owns the interface. `USBPcap` also captures an `ioscpp` run for the other side of
  the comparison.
- **No device at all.** `tools/compare-clienthello.py ref` builds the
  `pymobiledevice3` OpenSSL context and captures its ClientHello through
  `ssl.MemoryBIO`, and `diff` names the differences against a file that
  `IOSCPP_TRACE=1 IOSCPP_DUMP=<file> ioscpp_usb_example` writes. It separates a
  stack default from a fault without a device or a second host.
- **A `usbmuxd` of your own.** The `libimobiledevice-win32` release ships a
  `libusb`-backed `usbmuxd` and the `idevice*` tools. On Windows it needs the
  mux interface on a `libusb`-compatible driver, which is what `ioscpp` already
  needs, so it is a reference without a driver change. `pymobiledevice3` can then
  reach that `usbmuxd` over TCP by setting `USBMUXD_SOCKET_ADDRESS`.
- **A second host or `WSL`.** `usbipd` attaches the device to `WSL`, where
  `libimobiledevice` runs with `usbmuxd` stopped and `tcpdump` captures the
  link. The device belongs to one host at a time, so this and a native `ioscpp`
  run are exclusive.

## Verify

With a device attached and trusted:

```
cmake -S . -B build -DIOSCPP_BUILD_TESTS=ON -DIOSCPP_BUILD_EXAMPLES=ON
cmake --build build --config Release
ctest --test-dir build -C Release -E "^device$" --output-on-failure
build/examples/Release/ioscpp_usb_example
```

`ioscpp_device_tests` exits `77` when no device is attached, so the same commands
pass on a machine without one. See [`08-assumptions.md`](08-assumptions.md) for the
parts of this path that are not yet proven on a device.
