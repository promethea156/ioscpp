# Agent Instructions

## Git

Ask before every commit and before every push. Approval is **per action and per
change**, not a standing permission: a "commit and push" given for one change does
not cover the next one. After the work is done, stop, propose the commit, and wait for
the user to say yes; then, after the commit, propose the push and wait again. Do not
run `git commit`, `git push`, `git tag`, or any other command that changes remote
state until the user has clearly approved that exact action.

When asking for approval, show the proposed commit message and the list of files that
would be included, so the user can review exactly what will be committed. If a push is
next, say which branch and remote it would go to.

The one exception is a chain the user spelled out in the same request, for example
"commit and push": then run the commit, and the push, without stopping in between.

## Branching

`main` is stable, `development` integrates feature work, `release_candidate` stabilizes a
release, and `hotfix` carries an urgent fix from `main` back to `main` and `development`. A
short-lived branch is `<type>/<slug>`. See `docs/01-objective.md` for the full model. Do not
switch the current branch or create a branch without saying so.

Work on an issue happens on a short-lived branch cut from `development`, named `<type>/<slug>`
and named after the issue (for example `feat/explicit-disconnect` for issue #24), one issue per
branch. Commit there, push it, and open a pull request into `development`; do not commit issue
work directly on `development`. The pull request is squash-merged into `development` and the branch
is deleted.

Squash-merge a short-lived branch into its target, but merge two long-lived branches with a
merge commit, not a squash; a squash between long-lived branches makes them diverge, so the target
must be merged back immediately if it happens. See `docs/01-objective.md#merging`.

## Local Tooling

### `usbmuxd` and `libimobiledevice` can hold the device

`usbmuxd` claims the device's USB interface while it runs. Stop it before opening the same device with `ioscpp`, exactly as `adb kill-server` is needed for Android:

```powershell
# Linux
sudo systemctl stop usbmuxd
# macOS (if installed through brew)
brew services stop usbmuxd
```

The `idevice*` tools (`idevice_id`, `ideviceinfo`, `idevicepair`) talk through `usbmuxd`, so they are useful to compare against but cannot run at the same time as `ioscpp` on the same device.

On Windows the same is true of Apple's stack: the *Apple Mobile Device Service* holds the device, and the mux interface is bound to Apple's driver, which libusb cannot open. Stop the service and bind a libusb-compatible driver to the mux interface alone (`docs/09-platform-setup.md`).

Per-platform build, USB access, and driver steps are in `docs/09-platform-setup.md`.

### The device asks for trust

The first time a host pairs with a device, the device shows a *Trust This Computer?* prompt. A test that pairs without a human to tap the prompt hangs. The `ioscpp` pairing exchange waits for that tap, so a device test should either assume the device is already trusted, or expect the prompt.

### The device test is destructive when it is configured to be

`ioscpp_device_tests` exits with code 77 (a CTest skip) when no matching device is attached; `IOSCPP_TEST_SERIAL` names the device to match when several are attached, and the first is used otherwise. Today it connects, reads the device's identity, and exercises AFC listing, stat, and a push/pull round trip in `/PublicStaging`; the opt-in install and uninstall round trip (`IOSCPP_TEST_IPA` and `IOSCPP_TEST_BUNDLE`) stages the IPA over the `RSD` `AFC` shim and installs it over the `RSD` installer shim, then uninstalls the bundle. `ioscpp_multi_device_tests` (CTest test `multi`) is the separate multi-device test: it drives every attached device at once, one thread per device, and skips with code 77 when fewer than two are attached. `IOSCPP_TEST_SERIAL` still selects one device, so setting it narrows the multi-device test to one and it skips. The IPA must be development-signed with the device in its provisioning profile: an unsigned IPA is refused with `ApplicationVerificationFailed`. That round trip uninstalls and reinstalls the bundle and loses its data, so only set those variables for an app the user has agreed to replace.

## Testing

Build and run the suite from the repository root:

```powershell
cmake -S . -B build -DIOSCPP_BUILD_TESTS=ON -DIOSCPP_BUILD_EXAMPLES=ON
cmake --build build --config Release
ctest --test-dir build -C Release -E "^device$" --output-on-failure
```

Check the formatting with clang-format 19.1.1 before committing:

```powershell
cmake --build build --target format-check
```

### `-E` matches the test *name* as a regular expression

`ctest -E` matches the test name as a regular expression, not only the test registered as `device`, so a test whose name merely contains `device` is skipped silently and the run still reports success. Anchor the exclusion to exclude only the device test:

```powershell
ctest --test-dir build -C Release -E "^device$" --output-on-failure
```

### The USB example needs a device and the Trust prompt

`ioscpp_usb_example` detects the first attached device and connects to it. On a device that has not been trusted by this host, the device shows the *Trust This Computer?* prompt, and the pairing exchange blocks until it is answered. Run the example from an interactive terminal so the prompt can be seen.

```powershell
build\examples\Release\ioscpp_usb_example.exe
```

### The demo example walks the implemented features on one device

`ioscpp_demo_example` detects the first attached device, connects, and runs the implemented features once; walking every feature once is the goal of Slice 8. It needs a device and an IPA, and takes the bundle id and the IPA as arguments. It installs over any existing copy, so it loses that bundle's data.

```powershell
build\examples\Release\ioscpp_demo_example.exe <bundle-id> <app.ipa>
```

## Commit Messages

All commits MUST follow the [Conventional Commits 1.0.0](https://www.conventionalcommits.org/en/v1.0.0/#specification) specification.

Format:

```
<type>[optional scope]: <description>

[optional body]

[optional footer(s)]
```

Common types: `feat`, `fix`, `docs`, `refactor`, `perf`, `test`, `build`, `ci`, `chore`, `style`.

- Use a scope in parentheses when useful, e.g. `feat(afc): ...`.
- Mark breaking changes with `!` after the type/scope and/or a `BREAKING CHANGE:` footer.
- Keep the description a short summary.

Examples:

```
docs: add vertical-slice implementation roadmap
feat(afc): implement push command
fix(usb): handle short USB transfers
feat(api)!: rename connect to open
```

See `docs/01-objective.md` for the full versioning and commit message standards.
