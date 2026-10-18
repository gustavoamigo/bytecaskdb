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
