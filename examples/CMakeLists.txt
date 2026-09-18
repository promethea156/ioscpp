add_executable(ioscpp_sample sample/main.cpp)
target_link_libraries(ioscpp_sample PRIVATE ioscpp::ioscpp)

# The sample is device-free, so CTest runs it as part of the normal suite.
if(IOSCPP_BUILD_TESTS)
  add_test(NAME example_sample COMMAND ioscpp_sample)
endif()

if(IOSCPP_BUILD_USB)
  add_executable(ioscpp_usb_example usb/main.cpp)
  target_link_libraries(ioscpp_usb_example
    PRIVATE ioscpp::usb
  )

  # Walks every feature once against the first attached device.
  add_executable(ioscpp_demo_example demo/main.cpp)
  target_link_libraries(ioscpp_demo_example
    PRIVATE ioscpp::usb
  )

  # Pushes and pulls one file, timing each, for a transfer benchmark.
  add_executable(ioscpp_transfer_example transfer/main.cpp)
  target_link_libraries(ioscpp_transfer_example
    PRIVATE ioscpp::usb
  )

  # `$<TARGET_RUNTIME_DLLS:...>` is always empty on non-DLL platforms, and
  # `cmake -E copy_if_different` then fails because it is left with a single
  # argument, so the copy is Windows-only.
  if(WIN32)
    add_custom_command(TARGET ioscpp_usb_example POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              $<TARGET_RUNTIME_DLLS:ioscpp_usb_example>
              $<TARGET_FILE_DIR:ioscpp_usb_example>
      COMMAND_EXPAND_LISTS
    )
    add_custom_command(TARGET ioscpp_demo_example POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              $<TARGET_RUNTIME_DLLS:ioscpp_demo_example>
              $<TARGET_FILE_DIR:ioscpp_demo_example>
      COMMAND_EXPAND_LISTS
    )
    add_custom_command(TARGET ioscpp_transfer_example POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              $<TARGET_RUNTIME_DLLS:ioscpp_transfer_example>
              $<TARGET_FILE_DIR:ioscpp_transfer_example>
      COMMAND_EXPAND_LISTS
    )
  endif()
endif()
