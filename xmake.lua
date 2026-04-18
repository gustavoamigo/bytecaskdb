add_rules("mode.debug", "mode.release", "mode.releasedbg")

add_requires("crc32c")
-- Test dependency — optional so `xmake build` (default targets)
-- doesn't download/build it unless the consuming target is explicitly built.
add_requires("catch2 3.x", {optional = true})

-- Benchmark option: `xmake f --enable-benchmarks=true`
-- Downloads benchmark, leveldb, and rocksdb only when enabled.
option("enable-benchmarks")
    set_default(false)
    set_showmenu(true)
    set_description("Download and build benchmark dependencies (benchmark, leveldb, rocksdb)")
option_end()

if has_config("enable-benchmarks") then
    add_requires("benchmark")
    add_requires("leveldb")
    add_requires("rocksdb")
end

-- Sanitizer option: `xmake f --sanitizer=address` or `--sanitizer=thread`
option("sanitizer")
    set_default("")
    set_showmenu(true)
    set_description("Enable sanitizer (address, thread, or empty to disable)")
option_end()

-- Coverage option: `xmake f --coverage=true`
option("coverage")
    set_default("")
    set_showmenu(true)
    set_description("Enable LLVM source-based code coverage (true or empty to disable)")
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
    -- fault_injector.h inline functions are included in multiple module units;
    -- Clang flags duplicate definitions but they are harmless ODR-compliant inlines.
    "-Wno-decls-in-multiple-modules",
}

-- Apply sanitizer flags to a target if the option is set.
-- When the CLANG_TARGET_TRIPLE environment variable is set (e.g. by
-- scripts/run_sanitizer.sh), --target= is passed so the linker finds
-- platform-specific sanitizer runtime libraries. Without it, Clang may
-- default to a generic triple that doesn't match the OS's library layout
-- (e.g. x86_64-unknown-linux-gnu vs x86_64-redhat-linux-gnu on Fedora).
local function apply_sanitizer(t)
    local san = get_config("sanitizer")
    if san and san ~= "" then
        local triple = os.getenv("CLANG_TARGET_TRIPLE")
        local target_flag = triple and ("--target=" .. triple) or nil
        t:add("cxflags", "-fsanitize=" .. san, {force = true})
        t:add("ldflags", "-fsanitize=" .. san, {force = true})
        if target_flag then
            t:add("cxflags", target_flag, {force = true})
            t:add("ldflags", target_flag, {force = true})
        end
        if san == "address" then
            t:add("cxflags", "-fno-omit-frame-pointer", {force = true})
        end
    end
end

-- Apply LLVM source-based coverage flags to a target if the option is set.
local function apply_coverage(t)
    local cov = get_config("coverage")
    if cov and cov ~= "" then
        local triple = os.getenv("CLANG_TARGET_TRIPLE")
        local target_flag = triple and ("--target=" .. triple) or nil
        t:add("cxflags", "-fprofile-instr-generate", "-fcoverage-mapping", {force = true})
        t:add("ldflags", "-fprofile-instr-generate", {force = true})
        if target_flag then
            t:add("cxflags", target_flag, {force = true})
            t:add("ldflags", target_flag, {force = true})
        end
    end
end

-- Global defaults applied to all targets
set_toolchains("clang")
add_syslinks("pthread")
set_languages("c++23")
set_policy("build.c++.modules", true)
add_cxflags(table.unpack(common_flags))

if is_mode("release") or is_mode("releasedbg") then
    add_cxflags("-O3", "-fomit-frame-pointer")
end

-- LTO and -march=native applied per-target to avoid polluting dependency package builds.
local function add_release_opts(t)
    if is_mode("release") or is_mode("releasedbg") then
        t:set("policy", "build.optimization.lto", true)
        t:add("cxflags", "-march=native", {force = true})
    end
end

target("bytecask_tests")
    set_kind("binary")
    set_default(false)
    -- For VS Code / clangd support, run: scripts/gen_compile_commands.sh
    add_files("tests/*.cpp", "tests/proof/generated/*.cpp", "bytecaskdb/*.cppm", "bytecaskdb/bytecask.cpp")
    add_includedirs("bytecaskdb", "tests")
    add_packages("catch2", "crc32c")
    add_defines("BYTECASK_TESTING")
    on_config(function(t)
        apply_sanitizer(t)
        apply_coverage(t)
        add_release_opts(t)
    end)

target("map_bench")
    set_kind("binary")
    set_default(false)
    add_files("benchmarks/map_bench.cpp", "bytecaskdb/*.cppm", "bytecaskdb/bytecask.cpp")
    add_cxflags("-Wno-global-constructors")
    add_packages("benchmark", "crc32c")
    add_defines("BYTECASK_TESTING")
    on_config(function(t)
        apply_sanitizer(t)
        add_release_opts(t)
    end)

target("engine_bench")
    set_kind("binary")
    set_default(false)
    add_files("benchmarks/engine_bench.cpp", "bytecaskdb/*.cppm", "bytecaskdb/bytecask.cpp")
    add_cxflags("-Wno-global-constructors")
    add_packages("benchmark", "crc32c", "leveldb", "rocksdb")
    on_config(function(t)
        apply_sanitizer(t)
        add_release_opts(t)
    end)

-- Static library target for out-of-tree consumers (e.g. the MariaDB plugin).
-- Compiles all C++23 module sources and exposes them via libbytecask.a.
-- Note: C++23 module BMIs are not portable across translation units that
-- import them without the matching toolchain; the MariaDB plugin instead
-- uses the stable header-based C API in include/bytecask_c.h.
--
-- src/bytecask_c.cpp is compiled here (not in mariadb/CMakeLists.txt) because
-- it imports the C++23 bytecask module and must be built with the same
-- toolchain that produced the BMIs.
target("bytecask")
    set_kind("static")
    set_default(false)
    add_cxxflags("-fPIC", {force = true})  -- required when linking into a shared object (e.g. MariaDB plugin)
    add_files("bytecaskdb/*.cppm", "bytecaskdb/bytecask.cpp", "src/bytecask_c.cpp")
    add_packages("crc32c")
    on_config(apply_sanitizer)

-- Python bindings via nanobind.
-- Wraps the C++23 module interface directly (not the C API).
-- Prerequisites: pip install nanobind
-- Build: xmake build bytecaskdb_python
-- Usage: PYTHONPATH=bytecask-python python3 your_script.py
target("bytecaskdb_python")
    set_kind("shared")
    set_default(false)
    add_files("bytecask-python/src/bytecaskdb_module.cpp")
    add_files("bytecaskdb/*.cppm", "bytecaskdb/bytecask.cpp")
    add_packages("crc32c")
    -- nanobind requires compiling nb_combined.cpp from the nanobind package.
    on_load(function(t)
        local python = "python3"
        -- Python include directory
        local py_inc = os.iorunv(python, {"-c", "import sysconfig; print(sysconfig.get_path('include'))"})
        t:add("includedirs", py_inc:trim())
        -- nanobind include directory
        local nb_inc = os.iorunv(python, {"-c", "import nanobind; print(nanobind.include_dir())"})
        t:add("includedirs", nb_inc:trim())
        -- nanobind bundled dependencies (robin-map)
        local nb_pkg = os.iorunv(python, {"-c", "import os, nanobind; print(os.path.dirname(nanobind.__file__))"})
        t:add("includedirs", path.join(nb_pkg:trim(), "ext", "robin_map", "include"))
        -- nanobind source (nb_combined.cpp)
        local nb_src = os.iorunv(python, {"-c", "import nanobind; print(nanobind.source_dir())"})
        t:add("files", path.join(nb_src:trim(), "nb_combined.cpp"))
        -- Extension suffix and output naming
        local ext = os.iorunv(python, {"-c", "import sysconfig; print(sysconfig.get_config_var('EXT_SUFFIX'))"})
        ext = ext:trim()  -- e.g. ".cpython-314-x86_64-linux-gnu.so"
        -- Strip leading dot and .so suffix to get the tag
        local tag = ext:match("^%.(.+)%.so$") or ext:match("^%.(.+)%.pyd$") or ""
        t:set("basename", "_bytecaskdb." .. tag)
        t:set("prefixname", "")  -- no "lib" prefix
        t:set("extension", ".so")
        t:set("targetdir", path.join(os.projectdir(), "bytecask-python", "bytecaskdb"))
    end)
    -- Suppress warnings from nanobind headers (third-party code).
    add_cxxflags("-Wno-old-style-cast", "-Wno-extra-semi-stmt", "-Wno-shadow",
                 "-Wno-covered-switch-default", "-Wno-cast-function-type-strict",
                 "-Wno-sign-conversion", "-Wno-double-promotion", "-Wno-shadow-field",
                 "-Wno-cast-qual", "-Wno-zero-as-null-pointer-constant",
                 "-Wno-missing-field-initializers", "-Wno-float-equal",
                 "-Wno-deprecated-declarations", "-Wno-nested-anon-types",
                 "-Wno-gnu-anonymous-struct", "-Wno-unused-function",
                 {force = true})
    add_cxxflags("-fPIC", {force = true})
    on_config(function(t)
        -- On Linux, reject undefined symbols at link time so missing deps fail early.
        -- On macOS, Python C API symbols are resolved at runtime when the interpreter
        -- loads the extension, so use -undefined dynamic_lookup instead.
        if os.host() == "linux" then
            t:add("ldflags", "-Wl,--no-undefined", {force = true})
        else
            t:add("ldflags", "-undefined", "dynamic_lookup", {force = true})
        end
        apply_sanitizer(t)
        add_release_opts(t)
    end)

-- Fuzz targets — buffer-level parser harnesses using libFuzzer + ASan.
-- Build: CLANG_TARGET_TRIPLE=$(clang --print-target-triple) xmake f --sanitizer=fuzzer,address -m debug -y
--        xmake build fuzz_data_entry   (or fuzz_hint_entry)
-- Run:   ./build/.../fuzz_data_entry tests/fuzz/corpus/data_entry/ -max_total_time=60

target("fuzz_data_entry")
    set_kind("binary")
    set_default(false)
    add_files("tests/fuzz/fuzz_data_entry.cpp", "bytecaskdb/*.cppm", "bytecaskdb/bytecask.cpp")
    add_includedirs("bytecaskdb")
    add_packages("crc32c")
    add_defines("BYTECASK_TESTING")
    on_config(apply_sanitizer)

target("fuzz_hint_entry")
    set_kind("binary")
    set_default(false)
    add_files("tests/fuzz/fuzz_hint_entry.cpp", "bytecaskdb/*.cppm", "bytecaskdb/bytecask.cpp")
    add_includedirs("bytecaskdb")
    add_packages("crc32c")
    add_defines("BYTECASK_TESTING")
    on_config(apply_sanitizer)

-- Seed corpus generator for fuzz targets.
-- Build: xmake build gen_fuzz_corpus
-- Run:   ./build/.../gen_fuzz_corpus   (writes to tests/fuzz/corpus/)
target("gen_fuzz_corpus")
    set_kind("binary")
    set_default(false)
    add_files("tests/fuzz/gen_corpus.cpp", "bytecaskdb/*.cppm", "bytecaskdb/bytecask.cpp")
    add_includedirs("bytecaskdb")
    add_packages("crc32c")