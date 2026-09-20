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
interface needs a libusb-compatible driver bound to it. Bind one to the interface only,
so the rest of the device keeps Apple's driver.

**Use libusb-win32, not WinUSB.** libusb's WinUSB backend never sends
`SET_CONFIGURATION` to the device; its libusb0 backend does. A device in its initial
USB mode does not sit in the configuration that carries the mux interface, so a WinUSB
driver leaves the mux interface unreachable, and `list` reports `no device attached`.
libusb-win32 (`libusb0.sys`) is needed for `ioscpp` to select the configuration
itself, as `usbmuxd` does. A device that is already in the right configuration, for
example because Apple Mobile Device Support selected it, works with either. `libusbK`
cannot select a configuration either, so it has the same limit as WinUSB.

Install the driver with **Device Manager**, so no installer is needed:

1. Download the libusb-win32 binary package (`libusb-win32-bin-1.4.0.2.zip`) from
   [SourceForge](https://sourceforge.net/projects/libusb-win32/files/libusb-win32-release/)
   and extract it.
2. The package's `bin\libusb0.inf` ships another device's hardware id, in the `DeviceID`
   string of its `[Strings]` section. Set it to the mux interface's hardware id,
   `VID_05AC&PID_12A8&MI_01`; check the interface's *Hardware Ids* in Device Manager if
   the interface number differs.
3. Put the device in the normal (unlocked) mode and plug it in.
4. In **Device Manager**, find the mux interface: *View* -> *Devices by connection*, then
   the Apple composite device and its `Apple Mobile Device USB Device` (interface 1).
5. *Update driver* -> *Browse my computer* -> *Let me pick from a list* -> *Have Disk* ->
   browse to `libusb0.inf` -> **libusb-win32** -> install.
6. Replug the device.

The binding is per device, so every device a host talks to needs the step once; it survives
replugs. With two devices attached and only one bound, the unbound one has no readable serial,
so `list` leaves it out and it is not driven.

Editing the INF leaves its catalog signature stale, so a machine with driver signature
enforcement on can refuse the package; the install then needs enforcement off, or the package
re-signed. [Zadig](https://zadig.akeo.ie/) generates and signs a package for the device
instead, with no INF editing, so it is the shorter route when the manual one is refused.

Replacing the whole composite device's driver instead of one interface can make iTunes
re-pair it; binding the single interface avoids that.

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
- **Capturing the wire.** `dumpcap -D` may not list the USBPcap interfaces; `tshark -D`
  does. The device is on `\\.\USBPcap1` (root hub 1). Capture while an example runs:

  ```powershell
  tshark -i '\\.\USBPcap1' -a duration:30 -w ioscpp_run.pcapng
  ```

  `tshark -r ioscpp_run.pcapng -Y 'usb.device_address == 35 && usb.src == "host" && usb.data_len > 100'`
  then finds the ClientHello, and
  `-Y 'usb.capdata contains 73:65:73:73:69:6f:6e:55:70:63:61:6c:6c'` finds the
  device's `sessionUpcall connection closed` frame.
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
  This `usbmuxd` v1.1.1 crashes after it claims the mux interface. Replace its
  `libusb-1.0.dll` with libusb 1.0.30 and start it with `-p -n`; it then lists
  the device (`idevice_id -l`), but the first `lockdownd` connect still kills it
  with `Mux error (-8)`.
- **A second host or `WSL`.** `usbipd` attaches the device to `WSL`, where
  `libimobiledevice` runs with `usbmuxd` stopped and `tcpdump` captures the
  link. The device belongs to one host at a time, so this and a native `ioscpp`
  run are exclusive.

### Reaching a reference through Apple Mobile Device Support

Apple's own stack is the most faithful reference, and it needs no second host. It
does need the device back on Apple's driver, which the libusb0 package cannot install,
and it is exclusive with `ioscpp` on the device, so capture the reference and switch back.

1. Install the **desktop** iTunes support package, not the Microsoft Store build,
   which is a UWP package and does not register the driver or the service the same
   way. `iTunes64Setup.exe /extract` unpacks it, then install
   `AppleMobileDeviceSupport64.msi`, which registers `Apple Mobile Device Service`
   and the `usbaapl64` driver.
2. Restore the driver in **Device Manager**, since *Have Disk* only installs the
   package it is pointed at: *Update driver* -> *Browse my computer* -> *Let me pick
   from a list* -> *Apple Mobile Device USB Driver*. Do not uninstall with *delete the
   driver software* ticked, or the `libusb0` binding is lost and the driver step is
   redone.
3. Start the service, answer the trust prompt, and confirm the reference reaches the
   device:

   ```powershell
   Start-Service 'Apple Mobile Device Service'
   pymobiledevice3 usbmux list
   pymobiledevice3 lockdown info
   ```

   If the daemon is not found, point `pymobiledevice3` at its TCP socket with
   `USBMUXD_SOCKET_ADDRESS` set to `127.0.0.1:27015`.
4. Capture the reference as above, and read the device's answer with
   `tshark -r ref_run.pcapng -Y 'usb.src == "device" && usb.data_len > 40'`.
   The device answers with a ServerHello, which is the reference the host's own
   handshake was compared against (`04-blockers.md`).
5. Stop the service, rebind the mux interface to `libusb-win32` as above, and run
   `ioscpp` again.

## Verify

With a device attached and trusted:

```
cmake -S . -B build -DIOSCPP_BUILD_TESTS=ON -DIOSCPP_BUILD_EXAMPLES=ON
cmake --build build --config Release
ctest --test-dir build -C Release -E "^device$" --output-on-failure
ctest --test-dir build -C Release -R "^device$" --output-on-failure
build/examples/Release/ioscpp_usb_example
```

The `device` test exits `77` when no matching device is attached, so the whole
suite passes on a machine without one; `IOSCPP_TEST_SERIAL` names the device to
match when several are attached. See [`08-assumptions.md`](08-assumptions.md) for the
parts of this path that are not yet proven on a device.
