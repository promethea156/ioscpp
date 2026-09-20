include(FetchContent)

FetchContent_Declare(
  Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG v3.7.1
)
FetchContent_MakeAvailable(Catch2)

add_executable(ioscpp_tests
  afc_test.cpp
  cdtunnel_test.cpp
  ipv6_test.cpp
  json_test.cpp
  plist_test.cpp
  remotexpc_test.cpp
  session_test.cpp
  stream_test.cpp
  tcp_link_test.cpp
  tls_client_hello_test.cpp
  transport_test.cpp
  usbmux_test.cpp
)
target_link_libraries(ioscpp_tests
  PRIVATE ioscpp::ioscpp
    Catch2::Catch2WithMain
    # `tls_client_hello_test.cpp` inspects the first flight directly, so it
    # reaches mbedTLS, which is otherwise private to `ioscpp`.
    MbedTLS::mbedtls
)

list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
include(Catch)
catch_discover_tests(ioscpp_tests)

if(IOSCPP_BUILD_USB)
  add_executable(ioscpp_device_tests device_test.cpp)
  target_link_libraries(ioscpp_device_tests
    PRIVATE ioscpp::usb
  )

  # `$<TARGET_RUNTIME_DLLS:...>` is always empty on non-DLL platforms, and
  # `cmake -E copy_if_different` then fails because it is left with a single
  # argument, so the copy is Windows-only.
  if(WIN32)
    add_custom_command(TARGET ioscpp_device_tests POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              $<TARGET_RUNTIME_DLLS:ioscpp_device_tests>
              $<TARGET_FILE_DIR:ioscpp_device_tests>
      COMMAND_EXPAND_LISTS
    )
  endif()

  add_test(NAME device COMMAND ioscpp_device_tests)
  set_tests_properties(device PROPERTIES SKIP_RETURN_CODE 77)
endif()
