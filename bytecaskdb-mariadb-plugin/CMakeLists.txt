# bytecaskdb-mariadb-plugin/CMakeLists.txt
#
# Out-of-tree CMake build for the ByteCaskDB MariaDB storage plugin.
#
# Prerequisites
# -------------
# 1. MariaDB development package containing handler.h and related headers:
#      Fedora / RHEL:  dnf install mariadb-devel
#    Debian/Ubuntu's mariadb dev packages (libmariadb-dev, libmariadbd-dev)
#    never ship handler.h and the rest of the server-internal sql/ headers —
#    only Fedora's mariadb-devel bundles them. On Ubuntu, run
#    scripts/setup_ubuntu_host.sh, which fetches and builds a matching
#    MariaDB server source tree under .mariadb_server_src/.mariadb_server_build
#    for this purpose; this file falls back to that tree automatically when
#    the Fedora-style layout isn't found.
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

# xmake names its output arch dir "x86_64" or "arm64", matching uname -m
# except aarch64 -> arm64.
if(NOT DEFINED BYTECASK_ARCH)
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
    set(BYTECASK_ARCH "arm64")
  else()
    set(BYTECASK_ARCH "x86_64")
  endif()
endif()

# Location of the prebuilt static archive.
if(NOT DEFINED BYTECASK_LIB)
  set(BYTECASK_LIB
      "${BYTECASK_ROOT}/build/linux/${BYTECASK_ARCH}/release/libbytecask.a")
endif()

if(NOT EXISTS "${BYTECASK_LIB}")
  message(FATAL_ERROR
    "libbytecask.a not found at ${BYTECASK_LIB}.\n"
    "Build it first: cd ${BYTECASK_ROOT} && xmake build bytecask")
endif()

# Include directory for the stable C API header.
set(BYTECASK_INCLUDE_DIR "${BYTECASK_ROOT}/include")

# Location of the testing archive (with BYTECASK_TESTING defined).
set(BYTECASK_TESTING_LIB
    "${BYTECASK_ROOT}/build/linux/${BYTECASK_ARCH}/release/libbytecask_testing.a")

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

# The plugin needs the full server-internal API (handler.h, sql_class.h, and
# everything they transitively include) — Fedora's mariadb-devel bundles it
# under <MARIADB_INCLUDE_DIR>/server(/private), but Debian/Ubuntu's mariadb
# dev packages never ship it at all. When that Fedora-style layout isn't
# present, fall back to a MariaDB server source + build tree (as produced by
# scripts/setup_ubuntu_host.sh), which supplies the same headers straight
# from sql/, mysys/, storage/innobase/include, etc.
set(MARIADB_SERVER_INCLUDE_DIRS
  # server/private must precede server which must precede the client include
  # root so that the server-side my_global.h (which defines MY_GLOBAL_INCLUDED)
  # is found before the client-side stub.
  "${MARIADB_INCLUDE_DIR}/server/private"
  "${MARIADB_INCLUDE_DIR}/server"
  "${MARIADB_INCLUDE_DIR}")

if(NOT EXISTS "${MARIADB_INCLUDE_DIR}/server/handler.h" AND
   NOT EXISTS "${MARIADB_INCLUDE_DIR}/server/private/handler.h")
  if(NOT DEFINED MARIADB_SERVER_SRC_DIR)
    set(MARIADB_SERVER_SRC_DIR "${BYTECASK_ROOT}/.mariadb_server_src")
  endif()
  if(NOT DEFINED MARIADB_SERVER_BUILD_DIR)
    set(MARIADB_SERVER_BUILD_DIR "${BYTECASK_ROOT}/.mariadb_server_build")
  endif()

  if(EXISTS "${MARIADB_SERVER_SRC_DIR}/sql/handler.h")
    message(STATUS "MariaDB server headers : ${MARIADB_SERVER_SRC_DIR} (source tree)")

    # ha_bytecaskdb.cc includes <mysql/server/private/sql_class.h> — the exact
    # layout Fedora's mariadb-devel installs into /usr/include/mysql/server/
    # private, resolved there via the compiler's default /usr/include search
    # path rather than any -I flag. Our source tree doesn't reproduce that
    # layout, so recreate it with a symlink onto sql/ directly.
    set(_mariadb_server_private_dir "${MARIADB_SERVER_SRC_DIR}/include/mysql/server")
    if(NOT EXISTS "${_mariadb_server_private_dir}/private")
      file(MAKE_DIRECTORY "${_mariadb_server_private_dir}")
      file(CREATE_LINK "${MARIADB_SERVER_SRC_DIR}/sql" "${_mariadb_server_private_dir}/private" SYMBOLIC)
    endif()

    # ha_bytecaskdb.cc also includes <private/service_versions.h> — same
    # Fedora-layout assumption (that file lives at
    # <pkgincludedir>/mysql/private/service_versions.h there) for a header
    # that in our tree sits directly under include/. Self-alias so
    # "private/X" resolves back to include/X.
    if(NOT EXISTS "${MARIADB_SERVER_SRC_DIR}/include/private")
      file(CREATE_LINK "." "${MARIADB_SERVER_SRC_DIR}/include/private" SYMBOLIC)
    endif()

    set(MARIADB_SERVER_INCLUDE_DIRS
      "${MARIADB_SERVER_BUILD_DIR}/include"
      "${MARIADB_SERVER_BUILD_DIR}/sql"
      "${MARIADB_SERVER_BUILD_DIR}/strings"
      "${MARIADB_SERVER_SRC_DIR}/include"
      "${MARIADB_SERVER_SRC_DIR}/include/providers"
      "${MARIADB_SERVER_SRC_DIR}/sql"
      "${MARIADB_SERVER_SRC_DIR}/dbug"
      "${MARIADB_SERVER_SRC_DIR}/mysys"
      "${MARIADB_SERVER_SRC_DIR}/storage/innobase/include"
      "${MARIADB_SERVER_SRC_DIR}/tpool"
      "${MARIADB_SERVER_SRC_DIR}/wsrep-lib/include"
      "${MARIADB_SERVER_SRC_DIR}/wsrep-lib/wsrep-API/v26")
  else()
    message(FATAL_ERROR
      "MariaDB server-internal headers (handler.h) not found under "
      "${MARIADB_INCLUDE_DIR}/server, nor in a built MariaDB source tree at "
      "${MARIADB_SERVER_SRC_DIR}.\n"
      "Debian/Ubuntu's MariaDB dev packages don't ship these headers at all — "
      "run scripts/setup_ubuntu_host.sh to fetch and build a matching MariaDB "
      "server source tree, or install mariadb-devel (Fedora).")
  endif()
endif()

message(STATUS "MariaDB include dir : ${MARIADB_INCLUDE_DIR}")
message(STATUS "ByteCaskDB include  : ${BYTECASK_INCLUDE_DIR}")
message(STATUS "ByteCaskDB library  : ${BYTECASK_LIB}")

# ha_bytecaskdb.cc's extra() handles ALTER TABLE...ALGORITHM=COPY bulk-copy
# state transitions. Every released MariaDB (10.3 through at least 12.0)
# names these enumerators HA_EXTRA_END_ALTER_COPY / HA_EXTRA_ABORT_ALTER_COPY;
# MariaDB's unreleased main branch dropped the "_ALTER_" infix. Detect which
# spelling the active headers provide instead of hardcoding one, since a
# plain #ifdef can't see enum members (only macros).
include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_INCLUDES ${MARIADB_SERVER_INCLUDE_DIRS})
set(CMAKE_REQUIRED_DEFINITIONS -DMYSQL_SERVER -DDBUG_OFF)
check_cxx_source_compiles("
#include <my_global.h>
#include <my_base.h>
int main() { return HA_EXTRA_END_ALTER_COPY + HA_EXTRA_ABORT_ALTER_COPY; }
" BYTECASKDB_HAVE_ALTER_COPY_NAMES)
unset(CMAKE_REQUIRED_INCLUDES)
unset(CMAKE_REQUIRED_DEFINITIONS)

# ---------------------------------------------------------------------------
# Import the static library as a CMake target
# ---------------------------------------------------------------------------

add_library(bytecask_static STATIC IMPORTED)
set_target_properties(bytecask_static PROPERTIES
  IMPORTED_LOCATION "${BYTECASK_LIB}")

# ByteCaskDB uses crc32c internally; link it too. No libcrc32c-dev package
# exists on Ubuntu (see scripts/setup_ubuntu_host.sh) — xmake.lua already
# fetches its own copy via add_requires("crc32c") to build the core engine,
# so fall back to that cached copy when no system package is installed.
file(GLOB CRC32C_XMAKE_HINTS "$ENV{HOME}/.xmake/packages/c/crc32c/*/*/lib")
find_library(CRC32C_LIB NAMES crc32c
  HINTS /usr/lib64 /usr/lib /usr/local/lib /usr/local/lib64 ${CRC32C_XMAKE_HINTS})
if(NOT CRC32C_LIB)
  message(FATAL_ERROR
    "crc32c not found. Install google-crc32c-devel (Fedora) or libcrc32c-dev "
    "(Debian), or run 'xmake build bytecask' first so its fetched crc32c "
    "copy under ~/.xmake/packages is available.")
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
  ${MARIADB_SERVER_INCLUDE_DIRS}
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
  $<$<BOOL:${BYTECASKDB_HAVE_ALTER_COPY_NAMES}>:-DBYTECASKDB_HAVE_ALTER_COPY_NAMES>
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
  tests/unit/row_count_test.cpp
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

# ---------------------------------------------------------------------------
# Transaction tests (linked against ByteCaskDB for real DB access)
# ---------------------------------------------------------------------------

add_executable(mariadb_txn_tests
  tests/unit/mariadb_txn_test.cpp
  bytecaskdb_txn.cc
  bytecaskdb_plugin.cc
  catalog.cc
  key_encoding.cc
  row_encoding.cc
)

target_include_directories(mariadb_txn_tests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit"
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${BYTECASK_INCLUDE_DIR}"
)

target_compile_options(mariadb_txn_tests PRIVATE
  -Wall -Wextra
  -DBYTECASKDB_TESTS
  -DPLUGIN_TESTING
)

target_link_libraries(mariadb_txn_tests PRIVATE
  Catch2::Catch2WithMain
  bytecask_static
  "${CRC32C_LIB}"
)

# ---------------------------------------------------------------------------
# Proof tests — correctness validation via PluginTestHarness
# ---------------------------------------------------------------------------
# These compile ha_bytecaskdb.cc with PLUGIN_TESTING (stub headers) and
# BYTECASK_TESTING (fault injection checkpoints) against a real bytecask::DB.

file(GLOB PROOF_TEST_SOURCES
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/proof/prove_*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/proof/generated/prove_*.cpp"
)

add_executable(mariadb_proof_tests
  ${PROOF_TEST_SOURCES}
  tests/proof/harness.cpp
  ha_bytecaskdb.cc
  bytecaskdb_plugin.cc
  bytecaskdb_txn.cc
  catalog.cc
  key_encoding.cc
  row_encoding.cc
)

target_include_directories(mariadb_proof_tests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/proof"
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${BYTECASK_INCLUDE_DIR}"
  "${BYTECASK_ROOT}/bytecaskdb"
)

target_compile_options(mariadb_proof_tests PRIVATE
  -Wall -Wextra
  -DPLUGIN_TESTING
  -DBYTECASK_TESTING
  -Wno-unused-parameter
)

target_link_libraries(mariadb_proof_tests PRIVATE
  Catch2::Catch2WithMain
  "${BYTECASK_TESTING_LIB}"
  "${CRC32C_LIB}"
)

include(CTest)
list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)
catch_discover_tests(mariadb_plugin_tests)
catch_discover_tests(mariadb_proof_tests)

message(STATUS "Test target: mariadb_plugin_tests")
message(STATUS "Test target: mariadb_proof_tests")
