# ioscpp

A small, self-contained **iOS device client, as a C++20 library**.

`ioscpp` talks to an iPhone or iPad directly, over USB, with **no `usbmuxd`, no `libimobiledevice`, and no external binary**. Embed it in a C++ program and it lists and transfers files, installs and removes apps, and starts and stops them.

> **Status: 0.1.0 (scaffold).** The project layout, the public headers, the `Result<T>` error model, and the device-free test suite exist and build. The protocol slices are tracked in [`docs/03-roadmap.md`](docs/03-roadmap.md). Every fallible operation returns a `Result<T>` instead of throwing.

## What it can do

- **Files**: list a directory, `stat` a path, and pull or push a file over `AFC`.
- **Apps**: install and uninstall an app, launch it, check whether it is running, and close it.
- **Connect**: discover a device over USB, pair with it, and reach any `lockdownd` service.
- **Info**: query the device's model, iOS version, and unique id.

## Build it

You need a **C++20 compiler**, **CMake 3.24 or newer**, and **Git** (the test framework and the dependencies are fetched automatically at configure time). The build is the same everywhere; only the toolchain setup differs.

From the repository root:

```
cmake -S . -B build -DIOSCPP_BUILD_TESTS=ON -DIOSCPP_BUILD_EXAMPLES=ON
cmake --build build --config Release
ctest --test-dir build --output-on-failure -C Release
```

The `--config Release` flag is used by multi-config generators (Visual Studio, Xcode) and ignored by single-config generators (Makefiles, Ninja). The tests that need a device skip themselves when none is attached, so the build and the suite pass on any machine.

<details>
<summary><b>Linux</b></summary>

Install the toolchain and libusb's build dependency:

```
sudo apt-get update
sudo apt-get install -y build-essential cmake git pkg-config
```

Then run the three commands above.
</details>

<details>
<summary><b>macOS</b></summary>

Install the Xcode Command Line Tools (Clang) and CMake:

```
xcode-select --install
brew install cmake
```

Then run the three commands above.
</details>

<details>
<summary><b>Windows</b></summary>

Install **Visual Studio 2022** with the *Desktop development with C++* workload, and CMake 3.24 or newer. Then run the three commands above from a **Developer PowerShell for VS 2022**.
</details>

Optional API documentation, if [Doxygen](https://www.doxygen.nl/) is installed:

```
cmake -S . -B build -DIOSCPP_BUILD_DOCS=ON
cmake --build build --target ioscpp_docs
```

## Take the guided tour

The fastest way to learn the library is to run [`examples/demo/main.cpp`](examples/demo/main.cpp) and read it as it runs. It is one file, and every step is commented with what the call does on the device and which protocol it speaks, so the source is the walkthrough. It:

1. finds an attached device and connects to it, with the mux negotiation;
2. pairs with it if it is not paired, and opens `lockdownd`;
3. queries the device's model and iOS version;
4. lists a directory over `AFC`;
5. pushes a file, stats it, and pulls it back;
6. installs an app, uninstalling an old copy first;
7. launches the app;
8. checks that it stays running for ten seconds;
9. closes it.

To run it, you need a device with **a trusted host** (tap *Trust* on the device when asked) and an IPA to install. It uninstalls the bundle first, so it loses that bundle's data.

```
build/examples/Release/ioscpp_demo_example <bundle-id> <app.ipa> [--serial <serial>]
```

The binary is under `build/examples/Release/` for a multi-config generator (Visual Studio, Xcode) and `build/examples/` for a single-config one (Makefiles, Ninja).

With no `--serial`, it prints the attached devices and uses the first. When it finishes, open the source and read it next to the output: each `step(...)` in the source is one of the nine steps above.

## Project Layout

```
include/ioscpp/         Public headers (transport, protocol, session, stream,
                         device, lockdown, afc, app)
include/ioscpp/crypto/   The pairing record, backed by mbedTLS
include/ioscpp/tcp/       The TCP transport, over the platform's sockets
include/ioscpp/usb/       The USB transport, backed by libusb
include/ioscpp/testing/  The in-memory transport used by the tests
src/                    Library sources, mirroring the public headers
tests/                  Catch2 unit tests and the device integration test
examples/               Runnable examples, including the guided tour in demo/
tools/                  Developer scripts (device lister, transfer benchmark)
docs/                   Design documents and Doxygen configuration
cmake/                  CMake package configuration
```

## Where to go next

- [`examples/demo/main.cpp`](examples/demo/main.cpp) — the guided tour, step by step in its comments.
- [`docs/05-usage.md`](docs/05-usage.md) — copy-pasteable snippets for one feature at a time.
- [`docs/06-afc-protocol.md`](docs/06-afc-protocol.md) — how `AFC` and file transfer work, byte by byte.
- [`docs/03-roadmap.md`](docs/03-roadmap.md) and the [open issues](https://github.com/promethea156/ioscpp/issues) — what is planned next.

## Design documents

- [`docs/01-objective.md`](docs/01-objective.md) — objective, technical requirements, versioning, and commit conventions
- [`docs/02-references.md`](docs/02-references.md) — reference material on the iOS device protocols
- [`docs/03-roadmap.md`](docs/03-roadmap.md) — vertical-slice implementation roadmap
- [`docs/04-blockers.md`](docs/04-blockers.md) — significant blockers and how they were solved
- [`docs/05-usage.md`](docs/05-usage.md) — usage guide with copy-pasteable code examples
- [`docs/06-afc-protocol.md`](docs/06-afc-protocol.md) — the `AFC` wire format and how `list` is built on it
- [`docs/07-error-model.md`](docs/07-error-model.md) — why nothing throws, and what `Result<T>` carries instead

## References

- [usbmuxd](https://github.com/libimobiledevice/usbmuxd) — the mux protocol over USB
- [libimobiledevice](https://github.com/libimobiledevice/libimobiledevice) — `lockdownd`, `AFC`, and the service clients
- [go-ios](https://github.com/danielpaulus/go-ios) — a working, tested implementation of the protocols in Go
- [pymobiledevice3](https://github.com/doronz88/pymobiledevice3) — the protocols, in readable Python

## License

Boost Software License 1.0. See [`LICENSE`](LICENSE).
