add_rules("mode.debug", "mode.release")
add_rules("plugin.compile_commands.autoupdate", {outputdir = "."})

add_requires("bitsery")
add_requires("catch2 3.x")
add_requires("immer")

-- Common flags shared by all targets
local common_flags = {
    "-Weverything", "-Wno-c++98-compat", "-Wno-c++98-compat-pedantic",
    "-Wno-pre-c++20-compat-pedantic", "-Wno-padded",
    "-Wno-reserved-macro-identifier", "-Wno-undef", "-Wno-reserved-identifier",
    -- export keyword is the declaration for module symbols; these are false positives
    "-Wno-missing-variable-declarations", "-Wno-missing-prototypes",
    -- raw pointer indexing over a known-bounded span is intentional (CRC inner loop)
    "-Wno-unsafe-buffer-usage",
}

target("bytecask")
    set_toolchains("clang")
    set_kind("binary")
    add_files("src/*.cpp", "src/engine/*.cppm")
    set_languages("c++23")
    set_policy("build.c++.modules", true)
    add_cxflags(table.unpack(common_flags))
    add_packages("bitsery", "immer")
    -- Patch compile_commands.json so clangd can resolve all module imports.
    -- xmake's compile_commands generator omits some -fmodule-file= flags;
    -- the script adds any that are missing within each target group.
    after_build(function(target)
        os.exec("python3 scripts/fix_compile_commands.py")
    end)

target("bytecask_tests")
    set_toolchains("clang")
    set_kind("binary")
    add_files("tests/*.cpp", "src/engine/*.cppm")
    set_languages("c++23")
    set_policy("build.c++.modules", true)
    add_cxflags(table.unpack(common_flags))
    add_packages("bitsery", "catch2", "immer")

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

