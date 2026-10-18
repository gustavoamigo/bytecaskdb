-- bytecaskdb-sql-layer: SQL on the engine's public API (#487). MIT, like the
-- core. Nothing here includes a MariaDB header or depends on the plugin.
-- prototype: the end-to-end prototype of #497.

target("sql_layer")
    set_kind("static")
    set_default(false)
    add_deps("bytecask")
    add_includedirs("..", {public = true})
    add_files("codec/*.cpp", "catalog/*.cpp", "algebra/*.cpp")

target("sql_layer_tests")
    set_kind("binary")
    set_default(false)
    add_deps("sql_layer")
    add_packages("catch2")
    add_files("tests/*.cpp")
    on_config(function(t)
        t:add("syslinks", "pthread")
    end)
