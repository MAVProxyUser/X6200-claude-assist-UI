cmake_minimum_required(VERSION 3.21)

project(libg90
  VERSION 0.1.0
  DESCRIPTION "Xiegu G90 Body Link (DE-9) codec and wire library"
  LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(LIBG90_BUILD_TESTS "Build unit tests" ON)
option(LIBG90_BUILD_CLI "Build g90ctl CLI" ON)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

add_library(libg90 STATIC
  lib/src/error.cpp
  lib/src/types.cpp
  lib/src/crc.cpp
  lib/src/head.cpp
  lib/src/body.cpp
  lib/src/defaults.cpp
  lib/src/de9.cpp
  lib/src/log.cpp
)
add_library(libg90::libg90 ALIAS libg90)

target_include_directories(libg90
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/lib/include>
    $<INSTALL_INTERFACE:include>
)

target_compile_features(libg90 PUBLIC cxx_std_20)

target_compile_options(libg90 PRIVATE
  -Wall -Wextra -Wpedantic -Werror
)

set_target_properties(libg90 PROPERTIES
  OUTPUT_NAME g90
  EXPORT_NAME libg90
)

if(LIBG90_BUILD_CLI)
  add_subdirectory(cli)
endif()

if(LIBG90_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

install(TARGETS libg90
  EXPORT libg90Targets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(DIRECTORY lib/include/g90
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(EXPORT libg90Targets
  FILE libg90Targets.cmake
  NAMESPACE libg90::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/libg90
)

configure_package_config_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/libg90Config.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/libg90Config.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/libg90
)

# ExactVersion at 0.1.0 for the first public release.
write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/libg90ConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY ExactVersion
)

install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/libg90Config.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/libg90ConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/libg90
)
