cmake_minimum_required(VERSION 3.24)

# `project(VERSION)` takes numbers only, so a pre-release keeps the numeric base
# here and the `-<pre-release>` suffix lives in the tag and the changelog.
project(
  ioscpp
  VERSION 0.1.0
  DESCRIPTION "A simplified, dependency-free iOS device client library in C++"
  HOMEPAGE_URL "https://github.com/promethea156/ioscpp"
  LANGUAGES CXX
)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(IOSCPP_IS_TOP_LEVEL ON)
else()
  set(IOSCPP_IS_TOP_LEVEL OFF)
endif()

option(IOSCPP_BUILD_TESTS "Build ioscpp tests" ${IOSCPP_IS_TOP_LEVEL})
option(IOSCPP_BUILD_EXAMPLES "Build ioscpp examples" ${IOSCPP_IS_TOP_LEVEL})
option(IOSCPP_BUILD_DOCS "Build ioscpp documentation" OFF)
option(IOSCPP_BUILD_USB "Build the ioscpp USB backend (libusb)" ${IOSCPP_IS_TOP_LEVEL})
option(IOSCPP_INSTALL "Generate ioscpp install rules" ${IOSCPP_IS_TOP_LEVEL})

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

include(GNUInstallDirs)

# `tl::expected` provides the `Result<T>` that every fallible operation returns
# (see `docs/07-error-model.md`). It is header-only and licensed CC0-1.0, so it
# carries no license obligations, but it does have to be told not to build its own
# tests or install rules: its tests fetch their own Catch2 v2, which would collide
# with the v3 this project uses, and its install rules would install a second copy of
# a dependency.
include(FetchContent)
set(EXPECTED_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(EXPECTED_BUILD_PACKAGE OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  tl_expected
  GIT_REPOSITORY https://github.com/TartanLlama/expected.git
  GIT_TAG v1.3.1
)
FetchContent_MakeAvailable(tl_expected)

# Pairing (the host key pair, the lockdownd pairing exchange, and the TLS session
# that follows it) uses mbedTLS (Apache-2.0). It is a private dependency of the
# core: the public headers keep mbedTLS behind a pimpl, so a consumer never sees
# it. mbedTLS is acquired with FetchContent and linked statically into `ioscpp`.
include(FetchContent)
set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
# mbedTLS turns on -Werror by default, and a newer AppleClang warns about the
# non-NUL-terminated TLS 1.3 labels in ssl_tls13_keys.c, which would fail the
# build. The warning is a false positive in mbedTLS, so its warnings are not made
# fatal.
set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  mbedtls
  GIT_REPOSITORY https://github.com/Mbed-TLS/mbedtls.git
  GIT_TAG v3.6.2
)
FetchContent_MakeAvailable(mbedtls)

# HTTP/2 framing, HPACK, and flow control for the CoreDevice tunnel's RemoteXPC
# layer use nghttp2 (MIT). It is a private dependency of the core, kept behind a
# pimpl, and only its library is built, statically. `BUILD_SHARED_LIBS` is scoped
# so the choice does not leak into `ioscpp` itself, which decides its own type
# from the caller's `BUILD_SHARED_LIBS`.
include(FetchContent)
set(ENABLE_LIB_ONLY ON CACHE BOOL "" FORCE)
set(ENABLE_DOC OFF CACHE BOOL "" FORCE)
set(ENABLE_WERROR OFF CACHE BOOL "" FORCE)
set(BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
if(DEFINED BUILD_SHARED_LIBS)
  set(IOSCPP_BUILD_SHARED_SAVED ${BUILD_SHARED_LIBS})
endif()
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  nghttp2
  GIT_REPOSITORY https://github.com/nghttp2/nghttp2.git
  GIT_TAG v1.70.0
)
FetchContent_MakeAvailable(nghttp2)
if(DEFINED IOSCPP_BUILD_SHARED_SAVED)
  set(BUILD_SHARED_LIBS ${IOSCPP_BUILD_SHARED_SAVED} CACHE BOOL "" FORCE)
else()
  unset(BUILD_SHARED_LIBS CACHE)
endif()

add_library(ioscpp
  src/afc.cpp
  src/app.cpp
  src/connection.cpp
  src/crypto/pairing.cpp
  src/device.cpp
  src/lockdown.cpp
  src/protocol/cdtunnel.cpp
  src/protocol/ipv6.cpp
  src/protocol/json.cpp
  src/protocol/plist.cpp
  src/protocol/remotexpc.cpp
  src/protocol/usbmux.cpp
  src/session.cpp
  src/stream.cpp
  src/tcp/tcp_transport.cpp
  src/tcp_link.cpp
  src/tunnel.cpp
)
add_library(ioscpp::ioscpp ALIAS ioscpp)

target_compile_features(ioscpp PUBLIC cxx_std_20)

target_include_directories(ioscpp PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
  "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)

# `<tl/expected.hpp>` appears in the public `error.hpp`, so `tl::expected` is a
# public dependency of the core target.
target_link_libraries(ioscpp PUBLIC tl::expected)

# mbedTLS is only reached through the pimpl in `crypto/pairing.cpp`, so it is a
# private dependency of the core and never appears in a public header.
target_link_libraries(ioscpp PRIVATE MbedTLS::mbedcrypto MbedTLS::mbedtls MbedTLS::mbedx509)

# nghttp2 is only reached through the pimpl in the RemoteXPC layer, so it is a
# private dependency of the core and never appears in a public header.
target_link_libraries(ioscpp PRIVATE nghttp2)

if(WIN32)
  target_link_libraries(ioscpp PRIVATE ws2_32)
endif()

set_target_properties(ioscpp PROPERTIES
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR}
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
)

if(BUILD_SHARED_LIBS)
  target_compile_definitions(ioscpp
    PRIVATE IOSCPP_EXPORTS
    INTERFACE IOSCPP_SHARED
  )
endif()

if(MSVC)
  target_compile_options(ioscpp PRIVATE /W4 /permissive-)
else()
  target_compile_options(ioscpp PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(IOSCPP_BUILD_USB)
  add_subdirectory(src/usb)
endif()

if(IOSCPP_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(IOSCPP_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

if(IOSCPP_BUILD_DOCS)
  add_subdirectory(docs)
endif()

if(IOSCPP_IS_TOP_LEVEL)
  include(cmake/Format.cmake)
endif()

if(IOSCPP_INSTALL)
  include(CMakePackageConfigHelpers)

  install(TARGETS ioscpp
    EXPORT ioscppTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  )

  install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

  # mbedTLS is a private dependency of the core, but a static library propagates
  # its private dependencies to the final link, so those targets have to be in the
  # export set too.
  install(TARGETS mbedcrypto mbedtls mbedx509 everest p256m
    EXPORT ioscppTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  )

  # `tl::expected` is header-only, and `EXPECTED_BUILD_PACKAGE OFF` keeps it from
  # installing itself, so its target and its headers are installed here. It is
  # exported under the project's own namespace.
  install(TARGETS expected
    EXPORT ioscppTargets
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  )
  install(DIRECTORY "${tl_expected_SOURCE_DIR}/include/" DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

  # The USB backend is exported as `ioscpp::usb`, so a `find_package(ioscpp)`
  # consumer gets the same targets as a FetchContent consumer. It is a static
  # library with a private dependency (libusb), and a static library propagates its
  # private dependencies to the final link, so those dependencies have to be in the
  # export set too.
  if(IOSCPP_BUILD_USB)
    install(TARGETS ioscpp-usb usb-1.0
      EXPORT ioscppTargets
      RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
      LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
      ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
      INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )
  endif()

  install(EXPORT ioscppTargets
    FILE ioscppTargets.cmake
    NAMESPACE ioscpp::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ioscpp
  )

  configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ioscppConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/ioscppConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ioscpp
  )

  write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/ioscppConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
  )

  install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/ioscppConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/ioscppConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ioscpp
  )
endif()
