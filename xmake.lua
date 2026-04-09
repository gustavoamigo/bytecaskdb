add_rules("mode.debug", "mode.release", "mode.releasedbg")

add_requires("catch2 3.x")
add_requires("crc32c")
add_requires("benchmark", {optional = true})
add_requires("leveldb", {optional = true})
add_requires("rocksdb", {optional = true})

-- Sanitizer option: `xmake f --sanitizer=address` or `--sanitizer=thread`
option("sanitizer")
    set_default("")
    set_showmenu(true)
    set_description("Enable sanitizer (address, thread, or empty to disable)")
option_end()

-- Common flags shared by all targets
local common_flags = {
    "-Weverything", "-Wno-c++98-compat", "-Wno-c++98-compat-pedantic",
    "-Wno-pre-c++20-compat-pedantic", "-Wno-padded",
    "-Wno-reserved-macro-identifier", "-Wno-undef", "-Wno-reserved-identifier",
    -- export keyword is the declaration for module symbols; these are false positives
    "-Wno-missing-variable-declarations", "-Wno-missing-prototypes",
    -- raw pointer indexing over a known-bounded span is intentional in I/O paths
    "-Wno-unsafe-buffer-usage",
    -- -Wswitch-enum catches unhandled enum values; -Wswitch-default conflicts with
    -- exhaustive switches that list all enum cases and need no default.
    "-Wno-switch-default",
}

-- Apply sanitizer flags to a target if the option is set.
local function apply_sanitizer(t)
    local san = get_config("sanitizer")
    if san and san ~= "" then
        -- Fedora ships compiler-rt under x86_64-redhat-linux-gnu; Clang's
        -- default triple (x86_64-unknown-linux-gnu) doesn't match, so we
        -- override it so the linker finds the sanitizer runtime libs.
        t:add("cxflags", "--target=x86_64-redhat-linux-gnu", {force = true})
        t:add("ldflags", "--target=x86_64-redhat-linux-gnu", {force = true})
        t:add("cxflags", "-fsanitize=" .. san, {force = true})
        t:add("ldflags", "-fsanitize=" .. san, {force = true})
        if san == "address" then
            t:add("cxflags", "-fno-omit-frame-pointer", {force = true})
        end
    end
end

-- Global defaults applied to all targets
set_toolchains("clang")
set_languages("c++23")
set_policy("build.c++.modules", true)
add_cxflags(table.unpack(common_flags))

target("bytecask_tests")
    set_kind("binary")
    -- For VS Code / clangd support, run: scripts/gen_compile_commands.sh
    add_files("tests/*.cpp", "src/*.cppm", "src/bytecask.cpp")
    add_packages("catch2", "crc32c")
    add_defines("BYTECASK_TESTING")
    on_load(apply_sanitizer)

target("bytecask_bench")
    set_kind("binary")
    set_default(false)
    add_files("benchmarks/map_bench.cpp", "src/*.cppm", "src/bytecask.cpp")
    add_cxflags("-Wno-global-constructors")
    add_packages("benchmark", "crc32c")
    add_defines("BYTECASK_TESTING")
    on_load(apply_sanitizer)

target("engine_bench")
    set_kind("binary")
    set_default(false)
    add_files("benchmarks/engine_bench.cpp", "src/*.cppm", "src/bytecask.cpp")
    add_cxflags("-Wno-global-constructors")
    add_packages("benchmark", "crc32c", "leveldb", "rocksdb")
    on_load(apply_sanitizer)

-- Static library target for out-of-tree consumers (e.g. the MariaDB plugin).
-- Compiles all C++23 module sources and exposes them via libbytecask.a.
-- Note: C++23 module BMIs are not portable across translation units that
-- import them without the matching toolchain; the MariaDB plugin instead
-- uses the stable header-based C API in include/bytecask_c.h.
target("bytecask")
    set_kind("static")
    set_default(false)
    add_cxxflags("-fPIC", {force = true})  -- required when linking into a shared object (e.g. MariaDB plugin)
    add_files("src/*.cppm", "src/bytecask.cpp", "src/bytecask_c.cpp")
    add_packages("crc32c")
    on_load(apply_sanitizer)


