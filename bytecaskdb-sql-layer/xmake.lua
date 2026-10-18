-- bytecaskdb-sql-layer: SQL on the engine's public API (#487). MIT, like the
-- core. Nothing here includes a MariaDB header or depends on the plugin.
-- prototype: the end-to-end prototype of #497.

-- Third-party dependencies, fetched by git at a pinned tag or commit and
-- built from source (neither is a known-good xmake-repo package). Optional,
-- so nothing is fetched unless a target that uses them is built.

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
add_requires("libpg_query 6.2.5", {optional = true})

-- TinyCC: compiles plans in process (the run-time tier, #493). LGPL-2.1;
-- linked statically here, which is acceptable for a prototype that does not
-- ship. Pinned to a mob-branch commit: the last release, 0.9.27, is from 2017.
package("tinycc")
    set_homepage("https://bellard.org/tcc/")
    set_urls("https://github.com/TinyCC/tinycc.git")
    add_versions("2026.09.26", "9db1105c32afd3dcf0c28b8186f08e63c761b2b5")
    add_links("tcc")
    add_syslinks("dl", "pthread")
    on_install("linux", function (package)
        local cc = package:build_getenv("cc") or "cc"
        os.vrunv("./configure", {"--prefix=" .. package:installdir(), "--cc=" .. cc})
        os.vrunv("make", {"-j4"})
        os.vrunv("make", {"install"})
    end)
    on_test(function (package)
        assert(package:has_cfuncs("tcc_new", {includes = "libtcc.h"}))
    end)
package_end()
add_requires("tinycc 2026.09.26", {optional = true})

target("sql_layer")
    set_kind("static")
    set_default(false)
    add_deps("bytecask")
    add_rules("bytecask.native_flags")
    add_includedirs("..", {public = true})
    add_files("codec/*.cpp", "catalog/*.cpp", "algebra/*.cpp", "sql/*.cpp", "plan/*.cpp", "api/*.cpp")
    add_packages("libpg_query", "tinycc", {public = true})
    add_defines("SQL_LAYER_ALGEBRA_DIR=\"" .. path.join(os.scriptdir(), "algebra") .. "\"")
    on_config(function (t)
        local tcc = t:pkg("tinycc")
        if tcc then
            t:add("defines", "SQL_LAYER_TCC_LIB_PATH=\"" .. path.join(tcc:installdir(), "lib", "tcc") .. "\"")
        end
    end)

target("sql_layer_tests")
    set_kind("binary")
    set_default(false)
    add_deps("sql_layer")
    add_rules("bytecask.native_flags")
    add_packages("catch2")
    set_languages("c17", "c++23")
    add_files("tests/*.cpp")
    -- The hand-written plans, compiled by clang as C. Every plan defines
    -- plan(); each is renamed so several link into one binary.
    add_includedirs("algebra")
    add_defines("SQL_LAYER_PLANS_DIR=\"" .. path.join(os.scriptdir(), "plans") .. "\"")
    for _, name in ipairs({"insert", "select_by_id", "select_name_range", "update_qty", "delete_by_id"}) do
        add_files("plans/" .. name .. ".c", {defines = "plan=plan_" .. name,
                  cflags = "-Wno-declaration-after-statement"})
    end
