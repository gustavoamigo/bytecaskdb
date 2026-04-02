add_rules("mode.debug", "mode.release")

add_requires("bitsery")
add_requires("catch2 3.x")
add_requires("benchmark", {optional = true})
add_requires("immer")
add_requires("jemalloc")
add_requires("leveldb", {optional = true})

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
    -- raw pointer indexing over a known-bounded span is intentional (CRC inner loop)
    "-Wno-unsafe-buffer-usage",
    -- SSE4.2 required for hardware CRC-32C (_mm_crc32_u64 / _mm_crc32_u8)
    "-msse4.2",
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

target("bytecask")
    set_toolchains("clang")
    set_kind("binary")
    add_files("src/*.cpp", "src/engine/*.cppm")
    set_languages("c++23")
    set_policy("build.c++.modules", true)
    add_cxflags(table.unpack(common_flags))
    add_packages("bitsery", "immer", "jemalloc")
    on_load(apply_sanitizer)
    -- For VS Code / clangd support, run: scripts/gen_compile_commands.sh

target("bytecask_tests")
    set_toolchains("clang")
    set_kind("binary")
    add_files("tests/*.cpp", "src/engine/*.cppm")
    set_languages("c++23")
    set_policy("build.c++.modules", true)
    add_cxflags(table.unpack(common_flags))
    add_packages("bitsery", "catch2", "immer", "jemalloc")
    on_load(apply_sanitizer)

target("bytecask_bench")
    set_toolchains("clang")
    set_kind("binary")
    set_default(false)
    add_files("benchmarks/map_bench.cpp", "src/engine/*.cppm")
    set_languages("c++23")
    set_policy("build.c++.modules", true)
    add_cxflags(table.unpack(common_flags))
    add_cxflags("-Wno-global-constructors")
    add_packages("bitsery", "benchmark", "immer", "jemalloc")
    -- map_bench.cpp defines operator new/delete for allocation tracking.
    -- jemalloc's static archive (jemalloc_cpp.o) also defines them.
    -- --allow-multiple-definition lets the linker keep the first definition
    -- encountered (the .o file, which comes before the archive), so the
    -- custom allocator tracker in map_bench.cpp wins as intended.
    add_ldflags("-Wl,--allow-multiple-definition", {force = true})
    on_load(apply_sanitizer)

target("engine_bench")
    set_toolchains("clang")
    set_kind("binary")
    set_default(false)
    add_files("benchmarks/engine_bench.cpp", "src/engine/*.cppm")
    set_languages("c++23")
    set_policy("build.c++.modules", true)
    add_cxflags(table.unpack(common_flags))
    add_cxflags("-Wno-global-constructors")
    add_packages("bitsery", "benchmark", "immer", "jemalloc", "leveldb")
    on_load(apply_sanitizer)

--
-- If you want to known more usage about xmake, please see https://xmake.io
--
-- ## FAQ
--
-- You can enter the project directory firstly before building project.
--
--   $ cd projectdir
--
-- 1. How to build project?
--
--   $ xmake
--
-- 2. How to configure project?
--
--   $ xmake f -p [macosx|linux|iphoneos ..] -a [x86_64|i386|arm64 ..] -m [debug|release]
--
-- 3. Where is the build output directory?
--
--   The default output directory is `./build` and you can configure the output directory.
--
--   $ xmake f -o outputdir
--   $ xmake
--
-- 4. How to run and debug target after building project?
--
--   $ xmake run [targetname]
--   $ xmake run -d [targetname]
--
-- 5. How to install target to the system directory or other output directory?
--xm
--   $ xmake install
--   $ xmake install -o installdir
--
-- 6. Add some frequently-used compilation flags in xmake.lua
--
-- @code
--    -- add debug and release modes
--    add_rules("mode.debug", "mode.release")
--
--    -- add macro definition
--    add_defines("NDEBUG", "_GNU_SOURCE=1")
--
--    -- set warning all as error
--    set_warnings("all", "error")
--
--    -- set language: c99, c++11
--    set_languages("c99", "c++11")
--
--    -- set optimization: none, faster, fastest, smallest
--    set_optimize("fastest")
--
--    -- add include search directories
--    add_includedirs("/usr/include", "/usr/local/include")
--
--    -- add link libraries and search directories
--    add_links("tbox")
--    add_linkdirs("/usr/local/lib", "/usr/lib")
--
--    -- add system link libraries
--    add_syslinks("z", "pthread")
--
--    -- add compilation and link flags
--    add_cxflags("-stdnolib", "-fno-strict-aliasing")
--    add_ldflags("-L/usr/local/lib", "-lpthread", {force = true})
--
-- @endcode
--

