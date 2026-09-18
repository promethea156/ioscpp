# Objective

## Purpose

This repository aims to provide a **simplified, customized reimplementation of the iOS device protocols as a C++ library**. The library is designed to be embedded directly into C++ applications, giving developers programmatic control over iOS devices from a desktop host.

## Key Constraint

The library must operate **without depending on `usbmuxd`, `libimobiledevice`, or Apple's `idevice*` command-line tools**. There is no daemon to start, no socket to a running `usbmuxd`, and no external binary required. The library opens the device's USB interface itself and speaks the mux, `lockdownd`, `AFC`, and service protocols directly.

## Initial Scope

The first iteration targets a minimal but practical feature set, mirroring the `adbcpp` initial scope:

- **File transfer**: pull and push files to and from a device over `AFC`.
- **Package management**: install and uninstall applications.
- **File listing**: enumerate files and directories on a device.
- **Connection management**: discover, connect to, pair with, and disconnect from devices.
- **App control**: launch and close applications.
- **App status**: check whether an application is running.

## Non-Goals (for now)

- Full parity with every `idevice*` tool or every Apple service.
- Support for every iOS version, device family, or carrier variant.
- Jailbreak-only services.
- Wireless (Wi-Fi sync) discovery; the first iteration is USB-only.

## Technical Requirements

- **Cross-platform**: Linux, Windows, and macOS.
- **Language standard**: C++20.
- **Build system**: CMake.
- **Documentation**: Doxygen.

## USB Backend

Device communication goes through a `Transport` abstraction, which keeps the core library free of any USB dependency.

**Decision:** use **libusb** for the USB transport, linked **dynamically** as an optional backend target. libusb is licensed under LGPL-2.1-or-later, so it must **not** be statically linked into `ioscpp`. Consumers that do not need USB do not pull libusb in. libusb is acquired with CMake **FetchContent** and built as a **shared** library (`LIBUSB_BUILD_SHARED_LIBS=ON`).

The device presents a vendor-specific USB interface (class `0xff`, subclass `0xfe`, protocol `0x02`); the library claims it and exchanges the usbmux frames described in [`06-afc-protocol.md`](06-afc-protocol.md) and the design documents.

**Future improvement:** once most of the implementation is complete, replace libusb with platform-native USB APIs (WinUSB on Windows, IOKit on macOS, `usbfs` on Linux) to remove the third-party dependency and its license obligations entirely.

## Crypto

The host key pair, the `lockdownd` pairing exchange, and the TLS session that follows pairing use **mbedTLS** (Apache-2.0), acquired with CMake **FetchContent**. This is required for every device connection: `lockdownd` refuses to start a service for an unpaired host. The public headers keep mbedTLS behind a pimpl, so it is a private dependency of the core.

## Guiding Principles

- **Self-contained**: no reliance on `usbmuxd`, `libimobiledevice`, or external binaries.
- **Embeddable**: usable as a library from other C++ projects.
- **Focused**: implement only what is needed, cleanly.
- **Portable**: consistent behavior and API across all supported platforms.

## Versioning

The project follows [Semantic Versioning 2.0.0](https://semver.org/) (SemVer).

Versions take the form `MAJOR.MINOR.PATCH`:

- **MAJOR** is incremented for incompatible API changes.
- **MINOR** is incremented when functionality is added in a backwards-compatible manner.
- **PATCH** is incremented for backwards-compatible bug fixes.

Pre-release and build metadata **MAY** be appended as `-<pre-release>` and `+<build>` respectively (e.g. `0.1.0-alpha.1`, `0.1.0+build.5`).

Version bumps are derived from commit types, tying SemVer to the [commit message convention](#commit-messages) below:

- `fix` maps to a **PATCH** release.
- `feat` maps to a **MINOR** release.
- A `BREAKING CHANGE` (or `!`), regardless of type, maps to a **MAJOR** release.

## Branching

The repository keeps four long-lived branches, so a change is always made somewhere that says
what it is for:

- `main` is stable. Every commit on it is a release or a change about to be tagged, and a tag
  is only made here. Nothing lands on `main` without passing the whole CI matrix.
- `development` is the integration branch. Feature work merges here first, and it is where the
  slices in [`03-roadmap.md`](03-roadmap.md) are built.
- `release_candidate` is the stabilization branch. It is cut from `development` when a release
  is being prepared, and only fixes for that release land on it until it is merged to `main`.
- `hotfix` is for an urgent fix to a tagged release. It is cut from `main`, fixed, and merged
  back to both `main` and `development`, so the fix is not lost by the next release.

A short-lived branch is named `<type>/<slug>`, using the same types as the commit convention
below, for example `feat/afc-push` or `fix/usb-short-transfer`. It is deleted once it merges.

### Merging

How a branch is merged depends on whether it is long-lived, because a squash gives the target a
**new commit** rather than the source's history:

- A short-lived branch **SHOULD** be squash-merged into its target. The branch is deleted, so the
  rewritten history costs nothing and the target keeps one clean commit per change.
- A merge between two long-lived branches (`development` → `release_candidate` → `main`, and `hotfix`
  back to both) **MUST** be a **merge commit**, not a squash. A squash leaves the source branch
  holding commits that the target does not have, so the two diverge, the next pull request shows
  commits that are already released, and a conflict is guaranteed on the next touch of a shared file.
- If a long-lived branch is nonetheless squash-merged, the target **MUST** be merged back into the
  source immediately afterwards, so the source regains the target's history and the divergence is closed.

The rule in one line: never squash a long-lived branch into another long-lived branch without a
back-merge.

## Releasing

A release is a tag, and the tag is the release: nothing is published that CI has not built and tested on every platform first. The release is prepared on `release_candidate`, merged to `main`, and tagged on `main`.

1. Bump `project(VERSION)` in `CMakeLists.txt`.
2. Add the release's section to [`CHANGELOG.md`](../CHANGELOG.md), newest first.
3. Commit with `chore(release): <version>`.
4. Tag it `v<version>` and push the tag. The tag runs the whole CI matrix (`.github/workflows/ci.yml`), and the `release` job then publishes the tag's changelog section as the GitHub release.

A release is only tagged once the tests pass on all three platforms, and a breaking change is only released on a **MAJOR** bump, so a tag's version and the changelog section it publishes never disagree.

## Commit Messages

Commit messages follow the [Conventional Commits 1.0.0](https://www.conventionalcommits.org/en/v1.0.0/#specification) specification.

### Format

```
<type>[optional scope]: <description>

[optional body]

[optional footer(s)]
```

### Types

- `feat`: a new feature.
- `fix`: a bug fix.
- `docs`: documentation only changes.
- `refactor`: a code change that neither fixes a bug nor adds a feature.
- `perf`: a performance improvement.
- `test`: adding or correcting tests.
- `build`: changes to the build system or dependencies (e.g. CMake).
- `ci`: changes to CI configuration.
- `chore`: other changes that don't modify source or test files.
- `style`: formatting changes that don't affect meaning.

### Rules

- A scope **MAY** be provided in parentheses, e.g. `feat(afc): ...`.
- The description follows the colon and a space and is a short summary.
- A body **MAY** follow after one blank line for additional context.
- Breaking changes **MUST** be indicated with a `!` after the type/scope, and/or a `BREAKING CHANGE:` footer.
- Types are case-insensitive in practice; `BREAKING CHANGE` **MUST** be uppercase.

### Examples

```
docs: add commit message standard
feat(afc): implement push command
fix(usb): handle short USB transfers
feat(api)!: rename connect to open

BREAKING CHANGE: `connect` is now named `open`.
```
