# mariadb/CMakeLists.txt
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
# 2. ByteCaskDB static archive (libbytecask.a) and its C API header:
#    Build the static target first:
#      cd <bytecask-root> && xmake build bytecask
#    The archive will be at:
#      <bytecask-root>/build/linux/x86_64/release/libbytecask.a
#
# Usage
# -----
#   cmake -S mariadb -B mariadb/build \
#         -DBYTECASK_ROOT=<bytecask-root> \
#         -DMARIADB_INCLUDE_DIR=/usr/include/mysql
#   cmake --build mariadb/build
#
# Then load in MariaDB:
#   INSTALL PLUGIN bytecaskdb SONAME 'ha_bytecaskdb.so';

cmake_minimum_required(VERSION 3.20)
project(ha_bytecaskdb VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# The plugin uses C++17 (not C++23 modules) — it communicates with the engine
# only through the stable C API in bytecask_c.h.

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
find_library(CRC32C_LIB   NAMES crc32c HINTS /usr/lib64 /usr/lib)
if(NOT CRC32C_LIB)
  message(FATAL_ERROR "crc32c not found.  Install google-crc32c-devel (Fedora) or libcrc32c-dev (Debian).")
endif()

# ---------------------------------------------------------------------------
# Plugin shared library
# ---------------------------------------------------------------------------

add_library(ha_bytecaskdb MODULE
  ha_bytecaskdb.cc
  bytecaskdb_plugin.cc
  key_encoding.cc
  row_encoding.cc
)

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
  # MariaDB headers trigger some pedantic warnings; suppress known noise.
  -Wno-unused-parameter
  -Wno-missing-field-initializers
  -Wno-unused-variable
  -Wno-unused-function
)

# MariaDB plugins must not link the server symbols — they are dlopen'd into
# the server at runtime.  Link only the runtime and our static archive.
target_link_libraries(ha_bytecaskdb PRIVATE
  bytecask_static
  "${CRC32C_LIB}"
  stdc++
  m
  dl
)

# Plugin shared objects conventionally have no "lib" prefix on Linux.
set_target_properties(ha_bytecaskdb PROPERTIES
  PREFIX ""
  OUTPUT_NAME "ha_bytecaskdb"
)

message(STATUS "Plugin target: ha_bytecaskdb.so")
