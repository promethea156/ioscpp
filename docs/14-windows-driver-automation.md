# Automating the Windows driver step

The Windows setup needs one manual step: binding a libusb-compatible driver to the
device. This document is the plan to do that step in code instead, and it decides
how the driver package is signed.

The step is implemented in the library, in `ioscpp::usb`:

- `src/usb/windows_driver.cpp` discovers the node, generates the INF, signs the
  package, and installs it;
- `include/ioscpp/usb/windows_driver.hpp` is the public surface;
- `tools/windows-driver/main.cpp` is a small command-line front end;
- `tools/install-windows-driver.ps1` builds the tool and elevates.

It is a companion to [`09-platform-setup.md`](09-platform-setup.md#usb-driver),
which describes the manual version, and to
[`11-native-usb-windows.md`](11-native-usb-windows.md), which is the separate
plan to replace libusb. Automating this step does **not** remove that plan's driver
step; it removes only the GUI, and it is cheaper than the native rewrite.

## What the manual step is

On Windows the mux interface is bound to Apple's `usbaapl64` driver, which libusb
cannot open, and libusb's WinUSB backend cannot send the `SET_CONFIGURATION` the
device's initial USB mode needs (`09-platform-setup.md:104-111`). The user has to
bind `libusb-win32` to the interface with Zadig
(`09-platform-setup.md:113-124`). It is one step per device, survives replugs, and
needs a human to pick the right interface.

## What Zadig does, and what the helper does instead

Zadig is a GUI over `libwdi`, which is a wrapper over `SetupAPI` and `pnputil`:

1. find the target device node;
2. generate an INF from a template whose `DeviceID` is the node's hardware id;
3. sign the INF's catalog with a bundled certificate;
4. install the package and bind it to that node alone.

The helper does the same four steps with the Win32 API:

- `SetupDiGetClassDevs` / `SetupDiEnumDeviceInfo` find the node and read its
  hardware id and current service;
- the INF is written from the template a working package uses;
- `makecat` and `signtool` (Windows SDK) build and sign the catalog, with a
  self-signed certificate that is trusted on the machine;
- `SetupCopyOEMInf` installs the package, and
  `UpdateDriverForPlugAndPlayDevices` binds it to the node.

The bind is scoped by the INF's `[Devices]` model, which is `USB\<hardware id>`,
so only the target is rebound and the rest of the device keeps Apple's driver.

## Which node is the target

The interface node `USB\VID_05AC&PID_xxxx&MI_01` is preferred, because binding it
leaves the rest of the device on Apple's driver. But a fresh device sits in
configuration 1, which carries only the PTP interface, so there is no `MI_01` node
yet (`11-native-usb-windows.md:32-46`). The helper therefore falls back to the
composite node `USB\VID_05AC&PID_xxxx`, and the generated INF sets
`InitialConfigValue` to the configuration that carries the mux interface, so the
driver selects it on start and libusb does not have to.

## The signing route

This is the only genuinely uncertain part. Editing an INF invalidates the catalog of
the package it came from, and a machine with driver signature enforcement on refuses
an unsigned package (`09-platform-setup.md:113-116`, `:132-135`). There are two
routes.

### Route 1 (recommended for distribution): ship a pre-signed package

Generate the INF once, at release time, sign its catalog once, and ship the package
with the binaries. The user's machine needs no Windows SDK and no per-device
generation; it installs the shipped package. The certificate is still needed on the
user's machine, because a self-signed catalog is trusted only where its cert is
trusted; the package carries the `.cer` and the helper adds it to *Trusted Root* and
*Trusted Publishers* once. A real code-signing certificate later replaces the
self-signed one and the `.cer` step disappears.

### Route 2 (implemented): generate and sign on the machine

The helper writes the INF for the attached device, builds the catalog with `makecat`
from the Windows SDK, signs it with a self-signed cert, and trusts the cert. It needs
the Windows SDK but not the WDK; when the SDK is absent it installs the package
unsigned and warns. This is the general route, so it covers a product id a shipped
package does not.

### The decision

Implement route 2 now, because it needs only the Windows SDK and it works on any
device, and ship route 1 when the release process is set up. The helper prefers
`inf2cat` (WDK) when present and falls back to `makecat` (SDK).

### When the package is not signed

`UpdateDriverForPlugAndPlayDevices` refuses it under enforcement. Two stopgaps, in order: run
`bcdedit /set testsigning on` and reboot, which accepts any test-signed package, or
install Apple's *desktop* iTunes support package, whose `usbaapl64` driver already
selects a configuration, so the device reaches the mux interface without any libusb
driver (`09-platform-setup.md:212-244`).

## Using it

```powershell
tools/install-windows-driver.ps1              # bind the attached device
tools/install-windows-driver.ps1 -List           # report, change nothing
tools/install-windows-driver.ps1 -Force             # rebind even if already libusb0
tools/install-windows-driver.ps1 -Package C:\libusb-win32
tools/install-windows-driver.ps1 -Uninstall
```

The wrapper builds `ioscpp_windows_driver` and elevates it, because installing a
kernel driver needs an elevated process. The tool can also be called directly, once
built, from an already-elevated shell:

```powershell
build/tools/Release/ioscpp_windows_driver.exe --force
```

## Limits

- It still needs one elevation, exactly as Zadig does, because a kernel driver is
  installed.
- It replaces only the target node's driver; when the target is the interface, iTunes
  and photo import keep working, the same as the manual step.
- It binds by hardware id, so two attached devices with the same product id are both
  rebound by one run, which is the intended behavior.

## Where to read more

- [`09-platform-setup.md`](09-platform-setup.md#usb-driver) — the manual driver step.
- [`11-native-usb-windows.md`](11-native-usb-windows.md) — the native-API plan,
  which this step does not replace.
- [`src/usb/windows_driver.cpp`](../src/usb/windows_driver.cpp) — the implementation.
- [`tools/install-windows-driver.ps1`](../tools/install-windows-driver.ps1) — the wrapper.
