# The codec fuzzers.
#
# The codecs parse bytes that come from a device, and malformed input must return
# an error rather than crash. Every codec that parses device bytes has one harness
# here, and each harness is built one of two ways:
#
#   * with libFuzzer (`IOSCPP_USE_LIBFUZZER=ON`, Clang only), which supplies
#     `main` and the coverage-guided loop;
#   * with the in-repo `standalone_main.cpp`, which supplies `main` and feeds a
#     deterministic pseudo-random stream, so the harnesses still run on a compiler
#     without libFuzzer (MSVC, or an AppleClang without it).
#
# `IOSCPP_BUILD_FUZZERS` is off by default: a fuzzer is a development tool
# rather than part of the library, and it is wired into CI on its own.

# libFuzzer ships with upstream Clang. AppleClang reports its own compiler id and
# does not ship it, and MSVC does not ship it at all, so the default is on only
# for upstream Clang.
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  set(IOSCPP_USE_LIBFUZZER_DEFAULT ON)
else()
  set(IOSCPP_USE_LIBFUZZER_DEFAULT OFF)
endif()
option(IOSCPP_USE_LIBFUZZER "Use libFuzzer for the codec fuzzers" ${IOSCPP_USE_LIBFUZZER_DEFAULT})

if(IOSCPP_USE_LIBFUZZER AND NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  message(FATAL_ERROR
    "IOSCPP_USE_LIBFUZZER=ON needs upstream Clang; build the standalone "
    "driver by leaving it off.")
endif()

# One harness per codec, each named `ioscpp_fuzz_<codec>`.
set(IOSCPP_FUZZ_HARNESSES
  afc
  cdtunnel
  dtx
  http2
  ipv6
  plist
  xpc
)

foreach(harness IN LISTS IOSCPP_FUZZ_HARNESSES)
  add_executable(ioscpp_fuzz_${harness} ${harness}_fuzz.cpp)
  target_link_libraries(ioscpp_fuzz_${harness} PRIVATE ioscpp::ioscpp)

  if(IOSCPP_USE_LIBFUZZER)
    target_compile_options(ioscpp_fuzz_${harness} PRIVATE
      -fsanitize=fuzzer,address,undefined
      -fno-omit-frame-pointer
    )
    target_link_options(ioscpp_fuzz_${harness} PRIVATE
      -fsanitize=fuzzer,address,undefined
    )
  else()
    target_sources(ioscpp_fuzz_${harness} PRIVATE standalone_main.cpp)
  endif()

  if(MSVC)
    target_compile_options(ioscpp_fuzz_${harness} PRIVATE /W4 /permissive-)
  else()
    target_compile_options(ioscpp_fuzz_${harness} PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endforeach()
