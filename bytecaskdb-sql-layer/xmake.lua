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

target("sql_layer")
    set_kind("static")
    set_default(false)
    add_deps("bytecask")
    add_includedirs("..", {public = true})
    add_files("codec/*.cpp", "catalog/*.cpp", "algebra/*.cpp", "sql/*.cpp")
    add_packages("libpg_query", {public = true})

target("sql_layer_tests")
    set_kind("binary")
    set_default(false)
    add_deps("sql_layer")
    add_packages("catch2")
    set_languages("c17", "c++23")
    add_files("tests/*.cpp")
    -- The hand-written plans, compiled by clang as C. Every plan defines
    -- plan(); each is renamed so several link into one binary.
    add_includedirs("algebra")
    for _, name in ipairs({"insert", "select_by_id", "select_name_range", "update_qty", "delete_by_id"}) do
        add_files("plans/" .. name .. ".c", {defines = "plan=plan_" .. name,
                  cflags = "-Wno-declaration-after-statement"})
    end
    on_config(function(t)
        t:add("syslinks", "pthread")
    end)
