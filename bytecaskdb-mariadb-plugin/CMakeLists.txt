# bytecaskdb-mariadb-plugin/CMakeLists.txt
#
# Out-of-tree CMake build for the ByteCaskDB MariaDB storage plugin.
#
# Prerequisites
# -------------
# 1. MariaDB development package containing handler.h and related headers:
#      Fedora / RHEL:  dnf install mariadb-devel
#      Debian / Ubuntu: apt install libmariadb-dev
#    The headers live under $(mariadb_config --include-dir) or
#    /usr/include/mysql on many distributions.
#
# 2. ByteCaskDB static archive (libbytecask.a) and its C++ header:
#    Build the static target first:
#      cd <bytecask-root> && xmake build bytecask
#    The archive will be at:
#      <bytecask-root>/build/linux/x86_64/release/libbytecask.a
#
# Usage
# -----
#   cmake -S bytecaskdb-mariadb-plugin -B bytecaskdb-mariadb-plugin/build \
#         -DBYTECASK_ROOT=<bytecask-root> \
#         -DMARIADB_INCLUDE_DIR=/usr/include/mysql
#   cmake --build bytecaskdb-mariadb-plugin/build
#
# Then load in MariaDB:
#   INSTALL PLUGIN bytecaskdb SONAME 'ha_bytecaskdb.so';

cmake_minimum_required(VERSION 3.20)
project(ha_bytecaskdb VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# The plugin uses C++20 (not C++23 modules) — it consumes ByteCaskDB through
# the PIMPL C++ header in include/bytecask.hpp, whose implementation is
# compiled into libbytecask.a by xmake. The header relies on std::span and
# std::ranges::subrange, which require C++20.

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------

# Root of the bytecask workspace.  Can be overridden at configure time.
if(NOT DEFINED BYTECASK_ROOT)
  set(BYTECASK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")
endif()

# Location of the prebuilt static archive.
if(NOT DEFINED BYTECASK_LIB)
  set(BYTECASK_LIB
      "${BYTECASK_ROOT}/build/linux/x86_64/release/libbytecask.a")
endif()

if(NOT EXISTS "${BYTECASK_LIB}")
  message(FATAL_ERROR
    "libbytecask.a not found at ${BYTECASK_LIB}.\n"
    "Build it first: cd ${BYTECASK_ROOT} && xmake build bytecask")
endif()

# Include directory for the stable C API header.
set(BYTECASK_INCLUDE_DIR "${BYTECASK_ROOT}/include")

# MariaDB include directory.  Try to find it automatically or accept override.
if(NOT DEFINED MARIADB_INCLUDE_DIR)
  # Try mariadb_config / mysql_config
  find_program(MARIADB_CONFIG NAMES mariadb_config mysql_config)
  if(MARIADB_CONFIG)
    execute_process(
      COMMAND "${MARIADB_CONFIG}" --variable=pkgincludedir
      OUTPUT_VARIABLE MARIADB_INCLUDE_DIR
      OUTPUT_STRIP_TRAILING_WHITESPACE)
  else()
    # Common fallback paths
    foreach(_dir /usr/include/mysql /usr/include/mariadb
                 /usr/local/include/mysql /usr/local/include/mariadb)
      if(EXISTS "${_dir}/handler.h")
        set(MARIADB_INCLUDE_DIR "${_dir}")
        break()
      endif()
    endforeach()
  endif()
endif()

if(NOT MARIADB_INCLUDE_DIR OR NOT EXISTS "${MARIADB_INCLUDE_DIR}")
  message(FATAL_ERROR
    "MariaDB development headers not found.\n"
    "Install mariadb-devel (Fedora) or libmariadb-dev (Debian), or pass:\n"
    "  -DMARIADB_INCLUDE_DIR=/path/to/mysql/include")
endif()

message(STATUS "MariaDB include dir : ${MARIADB_INCLUDE_DIR}")
message(STATUS "ByteCaskDB include  : ${BYTECASK_INCLUDE_DIR}")
message(STATUS "ByteCaskDB library  : ${BYTECASK_LIB}")

# ---------------------------------------------------------------------------
# Import the static library as a CMake target
# ---------------------------------------------------------------------------

add_library(bytecask_static STATIC IMPORTED)
set_target_properties(bytecask_static PROPERTIES
  IMPORTED_LOCATION "${BYTECASK_LIB}")

# ByteCaskDB uses crc32c internally; link it too.
find_library(CRC32C_LIB   NAMES crc32c HINTS /usr/lib64 /usr/lib /usr/local/lib /usr/local/lib64)
if(NOT CRC32C_LIB)
  message(FATAL_ERROR "crc32c not found.  Install google-crc32c-devel (Fedora) or libcrc32c-dev (Debian).")
endif()

# When ON, embed libstdc++ and libgcc statically so the .so has no C++ runtime
# version dependency at all — only glibc symbols remain.  Use for portable builds
# (e.g. Docker) where the runtime image may ship an older libstdc++.
option(BYTECASKDB_STATIC_LIBSTDCXX "Statically link libstdc++ and libgcc into the plugin" OFF)

# ---------------------------------------------------------------------------
# Plugin shared library
# ---------------------------------------------------------------------------

add_library(ha_bytecaskdb MODULE
  ha_bytecaskdb.cc
  bytecaskdb_plugin.cc
  bytecaskdb_txn.cc
  catalog.cc
  key_encoding.cc
  row_encoding.cc
  # bytecask_hpp.cpp lives in bytecaskdb/ and is compiled by xmake into
  # libbytecask.a (it imports the C++23 bytecask module and must be built
  # with the same toolchain that produced the BMIs).  The PIMPL symbols
  # exposed by include/bytecask.hpp are linked in via bytecask_static.
)

if(BYTECASKDB_STATIC_LIBSTDCXX)
  target_link_options(ha_bytecaskdb PRIVATE -static-libstdc++ -static-libgcc)
endif()

target_include_directories(ha_bytecaskdb PRIVATE
  # server/private must precede server which must precede the client include
  # root so that the server-side my_global.h (which defines MY_GLOBAL_INCLUDED)
  # is found before the client-side stub.
  "${MARIADB_INCLUDE_DIR}/server/private"
  "${MARIADB_INCLUDE_DIR}/server"
  "${MARIADB_INCLUDE_DIR}"
  "${BYTECASK_INCLUDE_DIR}"
  "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_compile_options(ha_bytecaskdb PRIVATE
  -Wall -Wextra
  # MYSQL_SERVER activates the server-side handler API.
  -DMYSQL_SERVER
  # Required for dynamically loaded plugins — changes symbol names from
  # builtin_<name>_plugin_interface_version to _mysql_plugin_interface_version_.
  -DMYSQL_DYNAMIC_PLUGIN
  # Release-build MariaDB servers are compiled with DBUG_OFF; the DBUG
  # symbols (_db_my_assert etc.) are absent from the server binary.  Compile
  # the plugin with the same flag so the macros collapse to no-ops.
  -DDBUG_OFF
  -DDISABLE_DTRACE
  # MariaDB headers trigger some pedantic warnings; suppress known noise.
  -Wno-unused-parameter
  -Wno-missing-field-initializers
  -Wno-unused-variable
  -Wno-unused-function
)

# MariaDB plugins must not link the server symbols — they are dlopen'd into
# the server at runtime.  Link only the runtime and our static archive.
# stdc++ is omitted here: the C++ compiler driver adds it implicitly, and
# BYTECASKDB_STATIC_LIBSTDCXX controls whether that implicit link is static.
target_link_libraries(ha_bytecaskdb PRIVATE
  bytecask_static
  "${CRC32C_LIB}"
  m
  dl
)

# Plugin shared objects conventionally have no "lib" prefix on Linux.
set_target_properties(ha_bytecaskdb PROPERTIES
  PREFIX ""
  OUTPUT_NAME "ha_bytecaskdb"
)

message(STATUS "Plugin target: ha_bytecaskdb.so")

# ---------------------------------------------------------------------------
# Unit tests for the encoding / catalog layer
# ---------------------------------------------------------------------------
# These compile catalog.cc, key_encoding.cc, row_encoding.cc against minimal
# type stubs (no MariaDB server headers, no libbytecask).  Only pure encoding
# logic is tested.

# ---------------------------------------------------------------------------
# Integration tests
# ---------------------------------------------------------------------------
# Full end-to-end tests that provision MariaDB, load the plugin, and run SQL tests.

add_executable(integration_tests
  tests/unit/integration_test.cpp
)

target_include_directories(integration_tests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_compile_options(integration_tests PRIVATE
  -Wall -Wextra
  -DBYTECASKDB_TESTS  # Enable test mode guards
)

target_link_libraries(integration_tests PRIVATE Catch2::Catch2WithMain)

# ---------------------------------------------------------------------------
# Unit tests for the encoding / catalog layer
# ---------------------------------------------------------------------------
# These compile catalog.cc, key_encoding.cc, row_encoding.cc against minimal
# type stubs (no MariaDB server headers, no libbytecask).  Only pure encoding
# logic is tested.

include(FetchContent)
FetchContent_Declare(
  Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG        v3.7.1
)
FetchContent_MakeAvailable(Catch2)

add_executable(mariadb_plugin_tests
  tests/unit/mariadb_encoding_test.cpp
  tests/unit/mariadb_txn_simple_test.cpp
  catalog.cc
  key_encoding.cc
  row_encoding.cc
)

target_include_directories(mariadb_plugin_tests PRIVATE
  # Stubs must come first so tests/unit/my_global.h etc. win over MariaDB headers.
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit"
  "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_compile_options(mariadb_plugin_tests PRIVATE
  -Wall -Wextra
  -DBYTECASKDB_TESTS  # Enable test mode guards
)

target_link_libraries(mariadb_plugin_tests PRIVATE Catch2::Catch2WithMain)

include(CTest)
list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)
catch_discover_tests(mariadb_plugin_tests)

message(STATUS "Test target: mariadb_plugin_tests")
