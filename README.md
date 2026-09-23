# ioscpp

A small, self-contained **iOS device client, as a C++20 library**.

`ioscpp` talks to an iPhone or iPad directly, over USB, with **no `usbmuxd`, no `libimobiledevice`, and no external binary**. Embed it in a C++ program and it lists and transfers files, and installs and removes apps.

> **Status: 2.1.0.** Connect, Info, Files, app install/uninstall, app process control, and app container access are proven on devices, including two driven at once, on iOS 17.5.1 and 18.7.8. The protocol slices are tracked in [`docs/03-roadmap.md`](docs/03-roadmap.md). Every fallible operation returns a `Result<T>` instead of throwing.

## How this was built

This project is written **with AI assistance**. I am teaching myself how iOS
device protocols work from the inside, so the AI keeps the code and the prose small,
plain, and easy to follow — a tutor as much as a typist.

Everything that is not the typing, I do by hand, because that is where the learning
is:

- choosing what to build, the [key constraint](docs/01-objective.md#key-constraint), and the [non-goals](docs/01-objective.md#non-goals-for-now);
- planning the work as [vertical slices](docs/03-roadmap.md) and writing the [roadmap](docs/03-roadmap.md);
- designing the [lesson plan](LEARNING.md) and keeping the code documented, so it can be read as a lesson;
- setting up the machine, the compiler, and the [Apple USB driver](docs/09-platform-setup.md);
- attaching the iPhones and iPads, tapping *Trust*, and pairing each host;
- running every example and the device test against real hardware;
- finding and diagnosing what only breaks on a device, then directing the fix;
- reviewing every change, running the suite, and checking the formatting.

The AI writes most of the implementation; I direct it, review it, and am
responsible for what ships.

## Platform support

| Platform | Status | Notes |
|----------|--------|-------|
| Windows  | ![Windows: tested](https://img.shields.io/badge/Windows-tested-brightgreen) | Built, tested, and exercised against real devices, including USB. |
| Linux    | ![Linux: tested](https://img.shields.io/badge/Linux-tested-brightgreen) | Built, tested, and exercised against real devices, including USB. |
| macOS    | ![macOS: CI tested](https://img.shields.io/badge/macOS-CI%20tested-yellow) | CI builds it and runs the device-free suite; the USB path is unverified. |

CI builds and runs the device-free test suite on **all three** platforms, so all three are
known to compile and pass it. **Windows and Linux are exercised by hand against real
devices**, including the USB transport, so the macOS USB path is unverified and may have
rough edges. The platform steps below are written from the toolchain and standard-library
APIs the code targets. If you have a device on macOS, please build it and
[report the result](https://github.com/promethea156/ioscpp/issues/new?template=platform_verification.yml);
a green run is just as useful as a red one, and see [Contributing](#contributing).

## Device and iOS support

`ioscpp` reaches any iPhone or iPad that presents the mux interface. What it can do
depends on the device's iOS version, because iOS 17.4 moved the app installer and the
developer tools behind the `CoreDevice` tunnel:

| iOS version | What works |
|-------------|------------|
| Any | Connect, Info, and Files, over the mux link. |
| 17.4 or later | The app steps too: install, launch, the running check, close, and uninstall, over the tunnel. |
| 17.0–17.3.1 | Connect, Info, and Files; the app steps need the Wi-Fi `RemotePairing` route, which is not implemented ([`docs/10-coredevice-tunnel.md`](docs/10-coredevice-tunnel.md#the-wi-fi-remotepairing-route-ios-1701731)). |

Exercised on **iOS 17.5.1** (iPhone15,4) and **iOS 18.7.8** (iPhone17,4), including
both devices driven at once; **17.5.1 is the oldest iOS version tested so far**.

## What it can do

- **Connect**: discover a device over USB, pair with it, and reach any `lockdownd` service.
- **Info**: query the device's model, iOS version, and unique id.
- **Files**: list a directory, `stat` a path, and pull or push a file over `AFC`.
- **Apps**: install and uninstall an app over the RSD `AFC` and `installation_proxy` shims on iOS 17.4+, or the mux link below; launch it, check whether it is running, and close it.
- **App files**: list, `stat`, pull, and push inside one installed app's own container over `house_arrest`, on the mux link or the iOS 17.4+ RSD shim.
- **Log**: opt in to a process-wide logger, configurable per level, that reports frames, retries, and state changes and never logs key material or a payload.

Connect, Info, Files, app install/uninstall, app process control, and app container
access are proven on a device; the container vends over the RSD shim on iOS 17.4+. Process
control rides the RSD `com.apple.instruments.dtservicehub` service over
`DTX`, Slice 10 in [`docs/03-roadmap.md`](docs/03-roadmap.md). Several devices are
independent, so a program can drive every attached device at once, one thread per device;
[`examples/multi`](examples/multi/main.cpp) does exactly that.

## Build it

You need a **C++20 compiler**, **CMake 3.24 or newer**, and **Git** (the test framework and the dependencies are fetched automatically at configure time). The build is the same everywhere; only the toolchain setup differs. **Windows and Linux** are known to work so far — see [Platform support](#platform-support).

Every dependency is fetched by CMake, so there is nothing else to install. The compiler, USB driver, device permission, and daemon steps for each platform are in [`docs/09-platform-setup.md`](docs/09-platform-setup.md).

One process owns the device's mux interface at a time, so stop any `usbmuxd` before running `ioscpp` against a device, exactly as `adb kill-server` is needed for Android. On Linux that is `sudo systemctl stop usbmuxd`; the other platforms and the first-run *Trust* prompt are in [`docs/09-platform-setup.md`](docs/09-platform-setup.md).

From the repository root:

```
cmake -S . -B build -DIOSCPP_BUILD_TESTS=ON -DIOSCPP_BUILD_EXAMPLES=ON
cmake --build build --config Release
ctest --test-dir build --output-on-failure -C Release
```

The `--config Release` flag is used by multi-config generators (Visual Studio, Xcode) and ignored by single-config generators (Makefiles, Ninja). The tests that need a device skip themselves when none is attached, so the build and the suite pass on any machine.

### Fuzz the codecs

The codecs parse bytes that come from a device, and malformed input must return an error rather than crash. Every codec has a harness under `fuzz/`, built one of two ways. With upstream Clang, build them against libFuzzer and run one for a short budget:

```
cmake -S . -B build -DIOSCPP_BUILD_FUZZERS=ON -DIOSCPP_USE_LIBFUZZER=ON
cmake --build build --config Release
build/fuzz/ioscpp_fuzz_plist -max_total_time=30
```

Without libFuzzer (MSVC, or an AppleClang without it), the same harnesses build against an in-repo driver that feeds a deterministic pseudo-random stream, so they still run:

```
cmake -S . -B build -DIOSCPP_BUILD_FUZZERS=ON -DIOSCPP_USE_LIBFUZZER=OFF
cmake --build build --config Release
build/fuzz/Release/ioscpp_fuzz_plist.exe -runs=20000
```

<details>
<summary><b>Linux</b> — tested</summary>

Install the toolchain and libusb's build dependency:

```
sudo apt-get update
sudo apt-get install -y build-essential cmake git pkg-config
```

Stop `usbmuxd` before running against a device, exactly as `adb kill-server` is needed for Android:

```
sudo systemctl stop usbmuxd
```

Then run the three commands above.
</details>

<details>
<summary><b>macOS</b> — CI tested</summary>

Install the Xcode Command Line Tools (Clang) and CMake:

```
xcode-select --install
brew install cmake
```

Then run the three commands above.
</details>

<details>
<summary><b>Windows</b> — tested</summary>

Install **Visual Studio 2022** with the *Desktop development with C++* workload, and CMake 3.24 or newer. Then run the three commands above from a **Developer PowerShell for VS 2022**.

Before a device is reachable, the mux interface needs the libusb-win32 driver bound to it with [Zadig](https://zadig.akeo.ie/), and Apple Mobile Device Service stopped; the steps are in [`docs/09-platform-setup.md`](docs/09-platform-setup.md).
</details>

Optional API documentation, if [Doxygen](https://www.doxygen.nl/) is installed:

```
cmake -S . -B build -DIOSCPP_BUILD_DOCS=ON
cmake --build build --target ioscpp_docs
```

## Take the guided tour

The fastest way to learn the library is to run [`examples/demo/main.cpp`](examples/demo/main.cpp) and read it as it runs. It is one file, and every step is commented with what the call does on the device and which protocol it speaks, so the source is the walkthrough. It:

1. finds an attached device and opens its USB interface;
2. negotiates the mux, pairs with it if it is not paired, starts the `lockdownd` session, and reads the identity;
3. opens `AFC` and lists the media root;
4. pushes a file into `/PublicStaging`, stats it, and pulls it back;
5. opens the CoreDevice tunnel and the RSD connection, reporting how many services it lists;
6. installs an app from an IPA, replacing an existing copy;
7. launches it, checks it is running, and closes it;
8. pushes a file into the app's own container over `house_arrest`, stats it, and pulls it back;
9. uninstalls it.

Steps 5-9 need iOS 17.4 or later, because the installer and the developer tools moved behind the tunnel there.

To run it, you need a device with **a trusted host** (tap *Trust* on the device when asked) and an IPA to install. The install replaces that bundle, so it loses the bundle's data.

```
build/examples/Release/ioscpp_demo_example <bundle-id> <app.ipa>
```

The binary is under `build/examples/Release/` for a multi-config generator (Visual Studio, Xcode) and `build/examples/` for a single-config one (Makefiles, Ninja).

It uses the first attached device. When it finishes, open the source and read it next to the output: each `step(...)` in the source is one of the nine steps above.

## Run it against every device

[`examples/multi/main.cpp`](examples/multi/main.cpp) is the same tour run against every attached device at once, one thread per device. A `Device`, a `Stream`, and a `Connection` are not thread-safe, but different devices are independent, so one thread per device is how several are driven together. Every line is prefixed with the device's USB serial, so two devices are told apart.

```
build/examples/Release/ioscpp_multi_example <bundle-id> <app.ipa>
```

Every device needs the host setup in [`docs/09-platform-setup.md`](docs/09-platform-setup.md), including its own USB driver binding on Windows. Each device's install replaces that bundle on that device, so it loses the bundle's data there.

## Project Layout

```
include/ioscpp/         Public headers (transport, protocol, session, stream,
                         device, lockdown, log, afc, house_arrest, app, rsd)
include/ioscpp/crypto/   The pairing record, backed by mbedTLS
include/ioscpp/tcp/       The TCP transport, over the platform's sockets
include/ioscpp/usb/       The USB transport, backed by libusb
include/ioscpp/testing/  The in-memory transport used by the tests
src/                    Library sources, mirroring the public headers
tests/                  Catch2 unit tests and the device integration test
examples/               Runnable examples: the guided tour in demo/ and the parallel tour in multi/
fuzz/                   One libFuzzer harness per wire codec, plus a standalone driver
tools/                  Developer scripts and reports (device lister, transfer benchmark, fuzz report)
docs/                   Design documents and Doxygen configuration
cmake/                  CMake package configuration
```

## Where to go next

- [`docs/00-start-here.md`](docs/00-start-here.md) — a plain-language tour of the project, with no prior knowledge assumed.
- [`examples/demo/main.cpp`](examples/demo/main.cpp) — the guided tour, step by step in its comments.
- [`examples/multi/main.cpp`](examples/multi/main.cpp) — the same tour against every attached device, one thread per device.
- [`docs/05-usage.md`](docs/05-usage.md) — copy-pasteable snippets for one feature at a time.
- [`docs/06-afc-protocol.md`](docs/06-afc-protocol.md) — how `AFC` and file transfer work, byte by byte.
- [`docs/03-roadmap.md`](docs/03-roadmap.md) and the [open issues](https://github.com/promethea156/ioscpp/issues) — what is planned next.

## Design documents

- [`docs/00-start-here.md`](docs/00-start-here.md) — a plain-language tour of the project, and a glossary of the terms it uses
- [`docs/01-objective.md`](docs/01-objective.md) — objective, technical requirements, versioning, and commit conventions
- [`docs/02-references.md`](docs/02-references.md) — reference material on the iOS device protocols
- [`docs/03-roadmap.md`](docs/03-roadmap.md) — vertical-slice implementation roadmap
- [`docs/04-blockers.md`](docs/04-blockers.md) — significant blockers and how they were solved
- [`docs/05-usage.md`](docs/05-usage.md) — usage guide with copy-pasteable code examples
- [`docs/06-afc-protocol.md`](docs/06-afc-protocol.md) — the `AFC` wire format and how `list` is built on it
- [`docs/07-error-model.md`](docs/07-error-model.md) — why nothing throws, and what `Result<T>` carries instead
- [`docs/08-assumptions.md`](docs/08-assumptions.md) — what the implementation assumes but has not yet proven on a device
- [`docs/09-platform-setup.md`](docs/09-platform-setup.md) — what to install and grant per platform to build and reach a device
- [`docs/10-coredevice-tunnel.md`](docs/10-coredevice-tunnel.md) — the iOS 17+ CoreDevice tunnel plan: the layers, the wire formats, and the testing plan

## Contributing

Contributions are welcome, and the most useful ones often need no new code: verifying a
platform, reporting a failure, or fixing a document. See [`CONTRIBUTING.md`](CONTRIBUTING.md)
for how to build, test, and open a pull request, and the
[roadmap](docs/03-roadmap.md) for what is planned. Issues labelled
[`good first issue`](https://github.com/promethea156/ioscpp/labels/good%20first%20issue)
are a good place to start.

## References

- [libimobiledevice.org](https://libimobiledevice.org/) — the project home, its API docs, and the device/firmware status list
- [usbmuxd](https://github.com/libimobiledevice/usbmuxd) — the mux protocol over USB
- [libimobiledevice](https://github.com/libimobiledevice/libimobiledevice) — `lockdownd`, `AFC`, and the service clients
- [go-ios](https://github.com/danielpaulus/go-ios) — a working, tested implementation of the protocols in Go
- [pymobiledevice3](https://github.com/doronz88/pymobiledevice3) — the protocols, in readable Python

## License

Boost Software License 1.0. See [`LICENSE`](LICENSE).
