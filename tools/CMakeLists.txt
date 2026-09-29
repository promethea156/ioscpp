# The driver helper is Windows-only and sits on the USB backend, which is the
# only target that links the Win32 driver APIs.
add_executable(ioscpp_windows_driver windows-driver/main.cpp)
target_link_libraries(ioscpp_windows_driver PRIVATE ioscpp::usb)

# `$<TARGET_RUNTIME_DLLS:...>` is empty on non-DLL platforms, so the copy is
# Windows-only, matching the examples.
if(WIN32)
  add_custom_command(TARGET ioscpp_windows_driver POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:ioscpp_windows_driver>
            $<TARGET_FILE_DIR:ioscpp_windows_driver>
    COMMAND_EXPAND_LISTS
  )
endif()
