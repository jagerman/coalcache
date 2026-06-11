
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
  foreach(lang C CXX)
    if(NOT DEFINED CMAKE_${lang}_COMPILER_LAUNCHER AND NOT CMAKE_${lang}_COMPILER MATCHES ".*/ccache")
      message(STATUS "Enabling ccache for ${lang}")
      set(CMAKE_${lang}_COMPILER_LAUNCHER ${CCACHE_PROGRAM} CACHE STRING "")
    endif()
  endforeach()
endif()


cmake_minimum_required(VERSION 3.13...3.31 FATAL_ERROR)
project(solanacache
    VERSION 0.0.1
    LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20 CACHE STRING "The C++ standard whose features are requested.")
if(CMAKE_CXX_STANDARD LESS 20)
    set(CMAKE_CXX_STANDARD 20 CACHE STRING "" FORCE)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)

find_package(CURL REQUIRED COMPONENTS HTTP HTTPS SSL)

# uSockets doesn't really have a proper build system (just a very simple Makefile) so build it
# ourselves.
if (NOT CMAKE_VERSION VERSION_LESS 3.12)
    set(conf_depends "CONFIGURE_DEPENDS")
else()
    set(conf_depends "")
endif()

file(GLOB usockets_src ${conf_depends} uWebSockets/uSockets/src/*.c uWebSockets/uSockets/src/eventing/*.c)
file(COPY uWebSockets/uSockets/src/libusockets.h DESTINATION uWebSockets)
add_library(uSockets STATIC EXCLUDE_FROM_ALL ${usockets_src})
target_compile_definitions(uSockets PRIVATE LIBUS_NO_SSL=1)
target_include_directories(uSockets PRIVATE uWebSockets/uSockets/src)

# The uWebSockets C++ layer is header-only but isn't actually prefixed in the repository itself, but
# rather only on install (which, as above, is just a very simple Makefile).  This is unfortunate
# because it means that we can't use `#include <uWebSockets/App.h>` directly with the repo; so
# instead we emulate the installation process into the build directory and include it (with the
# prefix) from there.
file(COPY uWebSockets/src/ DESTINATION uWebSockets/uWebSockets FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp")
add_library(uWebSockets INTERFACE)
target_include_directories(uWebSockets INTERFACE ${CMAKE_CURRENT_BINARY_DIR}/uWebSockets)
target_link_libraries(uWebSockets INTERFACE uSockets)
target_compile_definitions(uWebSockets INTERFACE UWS_HTTPRESPONSE_NO_WRITEMARK)
if(TARGET zlib)
    target_link_libraries(uWebSockets INTERFACE zlib)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(ZLIB zlib REQUIRED IMPORTED_TARGET)
    target_link_libraries(uWebSockets INTERFACE PkgConfig::ZLIB)
endif()

find_package(nlohmann_json REQUIRED)
add_subdirectory(CLI11)

set(FORCE_OXENC_SUBMODULE ON CACHE BOOL "")
add_subdirectory(libquic)

pkg_check_modules(SODIUM REQUIRED IMPORTED_TARGET libsodium)

add_executable(coalcache src/main.cpp src/client.cpp src/server.cpp src/coalcache.cpp src/reqcache.cpp src/proxycache.cpp src/routes.cpp src/config.cpp)
target_link_libraries(coalcache PRIVATE
    CLI11::CLI11 CURL::libcurl uWebSockets nlohmann_json::nlohmann_json fmt::fmt quic PkgConfig::SODIUM)
