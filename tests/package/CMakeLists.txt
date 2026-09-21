cmake_minimum_required(VERSION 3.24)

# A standalone consumer of the installed package, not a subdirectory of the main
# build. The `package` CI job installs `ioscpp` and configures this project with
# `find_package(ioscpp)`, so a broken export set, a missing private dependency,
# or a wrong `INSTALL_INTERFACE` include is caught.
project(ioscpp_package_consumer LANGUAGES CXX)

find_package(ioscpp REQUIRED)

add_executable(ioscpp_package_consumer main.cpp)
target_link_libraries(ioscpp_package_consumer PRIVATE ioscpp::ioscpp)

# The USB backend is exported only when `ioscpp` was built with
# `IOSCPP_BUILD_USB=ON`, so the consumer uses it when it is present and stays
# device-free otherwise.
if(TARGET ioscpp::usb)
  target_link_libraries(ioscpp_package_consumer PRIVATE ioscpp::usb)
  target_compile_definitions(ioscpp_package_consumer PRIVATE IOSCPP_CONSUMER_HAS_USB)
endif()
