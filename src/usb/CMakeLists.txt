include(FetchContent)

set(LIBUSB_INSTALL_TARGETS OFF)
set(LIBUSB_BUILD_SHARED_LIBS ON)
# libusb's udev backend includes <libudev.h>, which is not part of a default Linux
# toolchain, so it is turned off: the netlink backend enumerates and watches devices
# without any system package.
set(LIBUSB_ENABLE_UDEV OFF)
FetchContent_Declare(
  libusb_cmake
  GIT_REPOSITORY https://github.com/libusb/libusb-cmake.git
  GIT_TAG v1.0.30-0
)
FetchContent_MakeAvailable(libusb_cmake)

add_library(ioscpp-usb usb_transport.cpp)
add_library(ioscpp::usb ALIAS ioscpp-usb)

# Export the target as `ioscpp::usb`, matching the in-tree alias.
set_target_properties(ioscpp-usb PROPERTIES EXPORT_NAME usb)

target_compile_features(ioscpp-usb PUBLIC cxx_std_20)
target_compile_definitions(ioscpp-usb PRIVATE IOSCPP_EXPORTS)
target_link_libraries(ioscpp-usb
  PUBLIC ioscpp::ioscpp
  PRIVATE usb-1.0
)

if(MSVC)
  target_compile_options(ioscpp-usb PRIVATE /W4 /permissive-)
else()
  target_compile_options(ioscpp-usb PRIVATE -Wall -Wextra -Wpedantic)
endif()
