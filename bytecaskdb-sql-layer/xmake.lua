-- bytecaskdb-sql-layer: SQL on the engine's public API (#487). MIT, like the
-- core. Nothing here includes a MariaDB header or depends on the plugin.
-- prototype: the end-to-end prototype of #497.

-- Third-party dependencies, fetched by git at a pinned tag or commit and
-- built from source (neither is a known-good xmake-repo package). The root
-- xmake.lua includes this file only with `xmake f --sql_layer=y`, which is
-- what keeps them from being fetched otherwise; both are required, so a
-- failed fetch or build fails configure at the package, not a later compile.

-- libpg_query: PostgreSQL 17's parser as a library, with its protobuf-c
-- output and the bundled protobuf-c runtime (no system protobuf). Its own
-- code is BSD-3-Clause; the PostgreSQL sources are under the PostgreSQL
-- licence.
package("libpg_query")
    set_homepage("https://github.com/pganalyze/libpg_query")
    set_urls("https://github.com/pganalyze/libpg_query.git")
    add_versions("6.2.5", "17-6.2.5")
    on_install("linux", "macosx", function (package)
        local cc = package:build_getenv("cc") or "cc"
        os.vrunv("make", {"build", "-j4", "CC=" .. cc})
        os.cp("libpg_query.a", package:installdir("lib") .. "/")
        os.cp("pg_query.h", package:installdir("include") .. "/")
        os.cp("postgres_deparse.h", package:installdir("include") .. "/")
        os.cp("protobuf/pg_query.pb-c.h", package:installdir("include", "protobuf") .. "/")
        os.cp("vendor/protobuf-c/protobuf-c.h", package:installdir("include", "protobuf-c") .. "/")
    end)
    on_test(function (package)
        assert(package:has_cfuncs("pg_query_parse_protobuf", {includes = "pg_query.h"}))
    end)
package_end()
add_requires("libpg_query 6.2.5")

-- TinyCC: compiles plans in process (the run-time tier, #493). LGPL-2.1;
-- linked statically here, which is acceptable for a prototype that does not
-- ship. Pinned to a mob-branch commit: the last release, 0.9.27, is from 2017.
--
-- It maps a plan's code writable, then mprotects it executable, which SELinux
-- in enforcing mode refuses ("mprotect failed"). Built with --with-selinux it
-- maps the code through a file instead, which works in every mode. That is
-- chosen here whenever SELinux is enabled on the build machine, as a package
-- config so that xmake rebuilds the package when that changes.
local function selinux_enabled()
    return os.exists("/sys/fs/selinux/enforce")
end

package("tinycc")
    set_homepage("https://bellard.org/tcc/")
    set_urls("https://github.com/TinyCC/tinycc.git")
    add_versions("2026.09.26", "9db1105c32afd3dcf0c28b8186f08e63c761b2b5")
    add_links("tcc")
    add_syslinks("dl", "pthread")
    add_configs("selinux", {description = "Map plan code through a file, for SELinux enforcing.", default = false, type = "boolean"})
    on_install("linux", function (package)
        local cc = package:build_getenv("cc") or "cc"
        local args = {"--prefix=" .. package:installdir(), "--cc=" .. cc}
        if package:config("selinux") then table.insert(args, "--with-selinux") end
        os.vrunv("./configure", args)
        os.vrunv("make", {"-j4"})
        os.vrunv("make", {"install"})
    end)
    on_test(function (package)
        assert(package:has_cfuncs("tcc_new", {includes = "libtcc.h"}))
    end)
package_end()
add_requires("tinycc 2026.09.26", {configs = {selinux = selinux_enabled()}})

-- libuv: non-blocking event loop for sql_layer_server (docs/session_design.md).
-- MIT-licensed C; linked only into the server binary.
add_requires("libuv")

-- C++23 modules, as the engine (the root sets the modules policy for every
-- target): one interface unit per module (bytecask.sql.<name>), holding its
-- declarations and definitions, public so sql_layer_tests imports them. The
-- one header is algebra/bc_plan.h, the C contract; algebra/plan_api.cpp, its
-- extern "C" definitions and the struct bc_ctx behind them, is a plain
-- translation unit.
--
-- Built twice, as the engine is (bytecask, bytecask_testing): sql_layer, and
-- sql_layer_testing with BYTECASK_SQL_TESTING, which adds the test-only calls
-- of the embedded API (Database::table_rows). The tests and the bytecask_sql
-- module link the testing build; nothing else does.
local function sql_layer_library(name, testing)
    target(name)
        set_kind("static")
        set_default(false)
        add_deps("bytecask")
        add_rules("bytecask.native_flags")
        add_includedirs("..", {public = true})
        add_files("codec/*.cppm", "catalog/*.cppm", "algebra/*.cppm", "sql/*.cppm", "plan/*.cppm", "api/*.cppm",
                  {public = true})
        add_files("algebra/plan_api.cpp")
        add_packages("libpg_query", "tinycc", {public = true})
        if testing then
            -- Public: a dependent target may build the module files with its own flags.
            add_defines("BYTECASK_SQL_TESTING", {public = true})
            -- Linked into the bytecask_sql Python module, a shared object, as the
            -- engine's library is into bytecaskdb_python. The production build
            -- links into no shared object.
            add_cxxflags("-fPIC", {force = true})
        end
        if (get_config("sanitizer") or ""):find("memory", 1, true) then
            -- No std-module probe under -nostdinc++; see bytecask.catch2 in the
            -- root xmake.lua, which does the same for the test target.
            set_policy("build.c++.modules.std", false)
        end
        -- Public: tcc_loader.cppm, a public interface unit, uses both, and a
        -- dependent target may build its module file with its own flags.
        add_defines("SQL_LAYER_ALGEBRA_DIR=\"" .. path.join(os.scriptdir(), "algebra") .. "\"", {public = true})
        on_config(function (t)
            local tcc = t:pkg("tinycc")
            if tcc then
                t:add("defines", "SQL_LAYER_TCC_LIB_PATH=\"" .. path.join(tcc:installdir(), "lib", "tcc") .. "\"",
                      {public = true})
            end
        end)
    target_end()
end

sql_layer_library("sql_layer", false)
sql_layer_library("sql_layer_testing", true)

target("sql_layer_tests")
    set_kind("binary")
    set_default(false)
    add_deps("sql_layer_testing")
    add_rules("bytecask.native_flags")
    add_rules("bytecask.catch2")
    set_languages("c17", "c++23")
    -- The tests, and the fixtures they share as modules (bytecask.sql.tests.*).
    add_files("tests/*.cpp", "tests/*.cppm")
    -- The TPROC-C harness's module, for its smoke test.
    add_files("harness/tprocc.cppm", "server/pg_text.cppm", "server/pg_wire.cppm", "server/pg_session.cppm")
    -- The engine's counted allocation-failure seam (tests/alloc_faults.h):
    -- it replaces the global operator new in this binary.
    add_files("../tests/alloc_faults.cpp", {defines = "BYTECASK_TESTING"})
    add_includedirs("../bytecaskdb")
    -- The hand-written plans, compiled by clang as C. Every plan defines
    -- plan(); each is renamed so several link into one binary.
    add_includedirs("algebra")
    add_defines("SQL_LAYER_PLANS_DIR=\"" .. path.join(os.scriptdir(), "plans") .. "\"")
    add_defines("SQL_LAYER_GOLDEN_DIR=\"" .. path.join(os.scriptdir(), "tests", "golden") .. "\"")
    for _, name in ipairs({"insert", "select_by_id", "select_name_range", "update_qty", "delete_by_id"}) do
        add_files("plans/" .. name .. ".c", {defines = "plan=plan_" .. name,
                  cflags = "-Wno-declaration-after-statement"})
    end

-- Micro-benchmarks of the plan API's fixed costs (bench/sql_layer_bench.cpp).
-- Not built by default and not run in CI. Build in releasedbg, as the
-- published numbers were: release's LTO link fails where llvm-ar is older
-- than clang.
target("sql_layer_bench")
    set_kind("binary")
    set_default(false)
    add_deps("sql_layer")
    add_rules("bytecask.native_flags")
    add_files("bench/*.cpp")

-- The TPROC-C harness (docs/testing_design.md, *The TPROC-C harness*):
-- TPROC-C through the embedded API, and Elle histories through SQL. Not
-- built by default and not run in CI; its smoke test is in sql_layer_tests.
-- Linked against the testing build for the engine's stats().
-- Build: xmake f -m releasedbg --sql_layer=y && xmake build sql_layer_tprocc
target("sql_layer_tprocc")
    set_kind("binary")
    set_default(false)
    add_deps("sql_layer_testing")
    add_rules("bytecask.native_flags")
    -- pg_text: load --sql-out writes values in PostgreSQL's text format.
    add_files("harness/*.cpp", "harness/*.cppm", "tests/tprocc_schema.cppm", "server/pg_text.cppm")

-- The PostgreSQL wire-protocol server (docs/server_design.md): the layer for
-- HammerDB and any libpq client. Not built by default. Linked against the
-- production build. The wire tests (tests/server/) and the proof tests'
-- wire target start it.
-- Build: xmake f --sql_layer=y && xmake build sql_layer_server
target("sql_layer_server")
    set_kind("binary")
    set_default(false)
    add_deps("sql_layer")
    add_rules("bytecask.native_flags")
    add_files("server/*.cpp", "server/*.cppm")
    add_packages("libuv")

-- The Python module the proof tests drive (tests/proof/, #515): open,
-- execute, close, the catalog, and a table's rows. Not built by default.
-- Build: xmake f --sql_layer=y && xmake build bytecask_sql
-- Usage: PYTHONPATH=bytecaskdb-sql-layer/python python3 -m pytest bytecaskdb-sql-layer/tests/proof
target("bytecask_sql")
    set_kind("shared")
    set_default(false)
    add_deps("sql_layer_testing")
    add_rules("bytecask.native_flags")
    add_rules("bytecask.nanobind")
    set_values("nanobind.module", "bytecask_sql")
    set_values("nanobind.targetdir", "bytecaskdb-sql-layer/python")
    add_files("python/bytecask_sql_module.cpp")
