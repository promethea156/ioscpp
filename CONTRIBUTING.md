# Contributing to ioscpp

Thanks for helping. `ioscpp` is a small project, and the most valuable contributions are
often the ones that need no new code at all: confirming that it works on your machine,
reporting where it does not, and sharpening the documents.

There is no CLA and no sign-up. Open an issue or a pull request and say what you found.

## Ways to help

### Verify a platform

**Windows is the only platform this project has been built and run on.** Linux and macOS
are expected to work but are [marked unverified](README.md#platform-support). If you have
either, build the project, run the suite, and
[open a verification report](https://github.com/promethea156/ioscpp/issues/new?template=platform_verification.yml)
with the result, green or red. This is the single most useful thing you can do, and it needs no
knowledge of the code.

The tests that need a device skip themselves, so the suite runs on any machine:

```sh
cmake -S . -B build -DIOSCPP_BUILD_TESTS=ON -DIOSCPP_BUILD_EXAMPLES=ON
cmake --build build --config Release
ctest --test-dir build -C Release -E "^device$" --output-on-failure
```

`-E "^device$"` is anchored on purpose: an unanchored `-E device` also skips every test
whose name merely contains `device`, and the run still reports success. Drop the flag to run
the device test too (see [The device test](#the-device-test)).

If the build or a test fails, a report with the exact compiler, CMake version, OS, and
output is already a contribution — [open a bug](https://github.com/promethea156/ioscpp/issues/new?template=bug_report.yml).

### Pick up a roadmap item

[`docs/03-roadmap.md`](docs/03-roadmap.md) is the plan. The remaining slices each have an
issue, and most of them are blocked on one thing: **a run against a real device**. The iOS 17+ `RSD`
tunnel (Slice 9) is done, so app install and uninstall now run over its shims; the remaining work is
the guided tour and `DTX`, and the device runs that report what happens. Items labelled
[`good first issue`](https://github.com/promethea156/ioscpp/labels/good%20first%20issue)
need little context; those labelled
[`help wanted`](https://github.com/promethea156/ioscpp/labels/help%20wanted) are broader.
Reasonable starting points today:

- **Build the guided tour and the device integration test** ([#7](https://github.com/promethea156/ioscpp/issues/7)) — walk every implemented feature against one device.
- **Build `DTX` and the `dvt` services** ([#9](https://github.com/promethea156/ioscpp/issues/9)) — the last layer, and it unblocks app process control.
- **Validate app install, uninstall, and control** ([#6](https://github.com/promethea156/ioscpp/issues/6)) — install and uninstall are proven; process control needs `DTX`.

If an issue looks stale or already done, say so in it rather than guessing.

### Improve the documentation

The [guided tour](examples/demo/main.cpp), [`LEARNING.md`](LEARNING.md), and the
[design documents](docs/) are how the library is understood. If a step was unclear, that is
a bug in the docs — please fix it or open an issue. [`docs/04-blockers.md`](docs/04-blockers.md)
exists precisely so a later reader can find a better solution than the one recorded, and
[`docs/08-assumptions.md`](docs/08-assumptions.md) lists what no device run has confirmed
yet; turning an assumption into a proven result is a contribution in itself.

### Send code

Read [`docs/01-objective.md`](docs/01-objective.md) for the architecture and the
[error model](docs/07-error-model.md) before writing any. In short:

- The library never throws. Every fallible operation returns a `Result<T>`.
- Public declarations carry a Doxygen comment.
- The core adds no third-party dependency beyond `tl::expected`; `crypto` (mbedTLS) and `usb` (libusb) are optional backends.
- Comments explain *why*, matching the surrounding code.

## Set up

You need a C++20 compiler, CMake 3.24 or newer, and Git. The build is the same on every
platform; the [README](README.md#build-it) has the toolchain notes, and
[`docs/09-platform-setup.md`](docs/09-platform-setup.md) has the per-platform USB driver,
device permission, and daemon steps. Every dependency is fetched by CMake. Configure once and keep
the tests running as you go:

```sh
cmake -S . -B build -DIOSCPP_BUILD_TESTS=ON -DIOSCPP_BUILD_EXAMPLES=ON
cmake --build build --config Release
ctest --test-dir build -C Release -E "^device$" --output-on-failure
```

Check the formatting with clang-format 19.1.1 before committing:

```sh
cmake --build build --target format-check
```

`format-check` is only defined when `clang-format` is found; set
`-DIOSCPP_CLANG_FORMAT=<path>` to point at a specific binary. The same check runs in CI,
so an unformatted file fails the build there.

## The device test

`ioscpp_device_tests` is the one test that needs hardware. It exits with code 77 (a CTest
skip) when no matching device is attached, so it never fails a machine without one;
`IOSCPP_TEST_SERIAL` names the device to match when several are attached, and the first is used
otherwise. Today it connects, reads the device's identity, exercises AFC listing, stat, and a push/pull round
trip, opens the RSD tunnel, and lists the RSD services. The opt-in install and uninstall round trip runs
only when `IOSCPP_TEST_IPA` and `IOSCPP_TEST_BUNDLE` name a development-signed app: it stages the IPA
over the RSD `AFC` shim, installs it over the `installation_proxy` shim, and uninstalls the bundle. It
uninstalls and reinstalls the bundle and loses its data, so only set those variables for an app you have
agreed to replace.

A device that has not been trusted by this host shows the *Trust This Computer?* prompt, and the pairing
exchange blocks until it is answered, so run the device test from an interactive terminal.

## Commits and branches

The project follows [Conventional Commits 1.0.0](https://www.conventionalcommits.org/en/v1.0.0/#specification)
and a four-branch model. The full rules are in
[`docs/01-objective.md`](docs/01-objective.md#commit-messages).

- Branch from `development` and name it `<type>/<slug>`, e.g. `fix/usb-short-transfer`.
- Target `development` in the pull request. `main` is release-only.
- Use a Conventional Commit type (`feat`, `fix`, `docs`, `test`, `ci`, ...). Version bumps
  are derived from the types, so an accurate type matters.

## Before you open a pull request

- [ ] The suite passes: `ctest --test-dir build -C Release -E "^device$" --output-on-failure`
- [ ] New behaviour has a unit test, driven by the mock transport where possible
- [ ] `cmake --build build --target format-check` is clean
- [ ] Public declarations have Doxygen comments
- [ ] The device test is run, or the reason it was not is stated
- [ ] Docs are updated if a command, an API, or a convention changed

The [pull request template](.github/PULL_REQUEST_TEMPLATE.md) asks for the same, so filling
it in is the checklist.

## Reviewing

Reviews are welcome too, and a review that only asks a question is useful. A reviewer is
looking for correctness, a test for the new path, the error-model shape, and a comment where
the reason for a decision is not obvious. Be kind and assume good faith; the
[blockers document](docs/04-blockers.md) shows how many sharp edges this protocol has.

## Getting help

Open an [issue](https://github.com/promethea156/ioscpp/issues/new/choose) and ask. A
question is a valid issue.
