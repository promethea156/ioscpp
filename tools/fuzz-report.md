# Fuzz report

Evidence that the codec fuzzers under [`../fuzz/`](../fuzz) build and run. This snapshot was
captured on Windows with MSVC 17.13.15, which has no libFuzzer, so it covers the in-repo
standalone driver. The libFuzzer branch is CI-only (`.github/workflows/ci.yml`, the `fuzz` job,
Ubuntu with upstream Clang). Re-run the commands below to refresh it.

## Build the fuzz targets

```
cmake -S . -B build-fuzz -DIOSCPP_BUILD_TESTS=OFF -DIOSCPP_BUILD_EXAMPLES=OFF -DIOSCPP_BUILD_FUZZERS=ON
cmake --build build-fuzz --config Release
```

All seven targets link against `ioscpp` and produce an executable:

```
ioscpp_fuzz_plist.vcxproj    -> build-fuzz\fuzz\Release\ioscpp_fuzz_plist.exe
ioscpp_fuzz_xpc.vcxproj      -> ...\ioscpp_fuzz_xpc.exe
ioscpp_fuzz_dtx.vcxproj      -> ...\ioscpp_fuzz_dtx.exe
ioscpp_fuzz_cdtunnel.vcxproj -> ...\ioscpp_fuzz_cdtunnel.exe
ioscpp_fuzz_ipv6.vcxproj     -> ...\ioscpp_fuzz_ipv6.exe
ioscpp_fuzz_afc.vcxproj      -> ...\ioscpp_fuzz_afc.exe
ioscpp_fuzz_http2.vcxproj    -> ...\ioscpp_fuzz_http2.exe
```

## Run the standalone driver

```
build-fuzz\fuzz\Release\ioscpp_fuzz_plist.exe -runs=200000
```

Each harness fed 200000 deterministic pseudo-random inputs and returned 0:

```
ioscpp_fuzz_plist    -> exit 0 : ran 200000 inputs
ioscpp_fuzz_xpc      -> exit 0 : ran 200000 inputs
ioscpp_fuzz_dtx      -> exit 0 : ran 200000 inputs
ioscpp_fuzz_cdtunnel -> exit 0 : ran 200000 inputs
ioscpp_fuzz_ipv6     -> exit 0 : ran 200000 inputs
ioscpp_fuzz_afc      -> exit 0 : ran 200000 inputs
ioscpp_fuzz_http2    -> exit 0 : ran 200000 inputs
```

The fuzzers call the same `parse`/`decode` entry points the device-free tests exercise, and
those tests pass, so the parsers are reached rather than skipped.

## Findings

The first libFuzzer run in CI (`.github/workflows/ci.yml`, the `fuzz` job) found two crashes, both
an unbounded allocation from a crafted length. Both are fixed, and each crashing input is pinned as a
regression case:

- `ioscpp_fuzz_xpc` aborted with an ASan out-of-memory on an array whose element count claimed far
  more elements than the bytes that remain, so `decode_object` reserved for the count before it was
  bounded. `src/protocol/remotexpc.cpp` now rejects a count larger than the bytes that remain, and
  `tests/remotexpc_test.cpp` pins the crashing input.
- `ioscpp_fuzz_plist` aborted on a binary plist whose trailer object count, multiplied by the offset
  size, overflowed the table-bounds check and then drove a huge allocation. The bound in
  `src/protocol/plist.cpp` is now formed as a division, and the object-level reads and count-based
  allocations are bounds-checked; `tests/plist_test.cpp` pins the crafted trailer.

## Guards

`IOSCPP_BUILD_FUZZERS` is off by default, so a normal build has no fuzz targets. Requesting
libFuzzer on a compiler without it is a configuration error rather than a broken build:

```
=== Default (IOSCPP_BUILD_FUZZERS unset): no fuzz targets ===
OK: no fuzz targets in the default build

=== MSVC + IOSCPP_USE_LIBFUZZER=ON: configure must fail ===
CMake Error at fuzz/CMakeLists.txt:27 (message):
  IOSCPP_USE_LIBFUZZER=ON needs upstream Clang; build the standalone driver
-- Configuring incomplete, errors occurred!
```

## Formatting and the device-free suite

The sources are formatted and the suite passes, including the two regression cases:

```
=== format-check ===
  Checking ioscpp formatting with clang-format      (no violations)

=== ctest (device-free) ===
100% tests passed, 0 tests failed out of 99
```

## Limits

This snapshot covers the standalone driver only, because no upstream Clang is installed on the
Windows host. The libFuzzer branch, which is the coverage-guided one, is exercised by the CI `fuzz`
job, and the CI `fuzz-standalone` job runs the driver above on Windows. A crash found by either
becomes a regression case under [`../tests/`](../tests).
