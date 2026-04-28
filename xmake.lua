add_rules("mode.debug", "mode.release", "mode.releasedbg")

add_requires("crc32c")
add_requires("jemalloc", {optional = true, configs = {prof = true}})
-- Test dependency — optional so `xmake build` (default targets)
-- doesn't download/build it unless the consuming target is explicitly built.
add_requires("catch2 3.x", {optional = true})

-- Benchmark dependencies — optional so `xmake build` (default targets)
-- doesn't download/build them unless a consuming target is explicitly built.
add_requires("benchmark", {optional = true})
-- add_requires("leveldb", {optional = true})
add_requires("rocksdb", {system = true, optional = true})

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
set_languages("c++23")
set_policy("build.c++.modules", true)
add_cxflags(table.unpack(common_flags))

-- Native-only helper: adds pthread to the current target.
-- Called from each native target's on_config instead of globally,
-- because WASM targets run single-threaded and have no pthread.
local function add_native_syslinks(t)
    t:add("syslinks", "pthread")
end

if is_mode("release") or is_mode("releasedbg") then
    add_cxflags("-O3", "-fomit-frame-pointer")
end

-- LTO and target CPU applied per-target to avoid polluting dependency package builds.
-- Set BYTECASK_MARCH to override (e.g. "x86-64-v3" for portable wheels).
-- Defaults to "native" for local development.
local march = os.getenv("BYTECASK_MARCH") or "native"
local function add_release_opts(t)
    if is_mode("release") then
        t:set("policy", "build.optimization.lto", true)
        t:add("cxflags", "-march=" .. march, {force = true})
    end
    if is_mode("releasedbg") then
        -- No LTO in releasedbg so perf/gdb can resolve symbols.
        t:add("cxflags", "-march=" .. march, {force = true})
        t:add("cxflags", "-fno-omit-frame-pointer", {force = true})
    end
end

target("bytecask_tests")
    set_kind("binary")
    set_default(false)
    -- For VS Code / clangd support, run: scripts/gen_compile_commands.sh
    add_files("tests/*.cpp", "tests/proof/generated/*.cpp", "bytecaskdb/*.cppm", "bytecaskdb/bytecask.cpp")
    remove_files("tests/radix_tree_memory_test.cpp")
    add_includedirs("bytecaskdb", "tests")
    add_packages("catch2", "crc32c")
    add_defines("BYTECASK_TESTING")
    on_config(function(t)
        add_native_syslinks(t)
        apply_sanitizer(t)
        apply_coverage(t)
        add_release_opts(t)
    end)

target("radix_tree_memory_tests")
    set_kind("binary")
    set_default(false)
    add_files("tests/radix_tree_memory_test.cpp", "bytecaskdb/*.cppm", "bytecaskdb/bytecask.cpp")
    add_includedirs("bytecaskdb", "tests")
    add_cxflags("-Wno-global-constructors")
    add_packages("catch2", "crc32c")
    add_defines("BYTECASK_TESTING")
    on_config(function(t)
        apply_sanitizer(t)
        apply_coverage(t)
        add_release_opts(t)
    end)

target("unordered_view_tests")
    set_kind("binary")
    set_default(false)
    add_files("benchmarks/unordered_view_test.cpp", "bytecaskdb/*.cppm", "bytecaskdb/bytecask.cpp")
    add_includedirs("bytecaskdb", "benchmarks", "tests")
    add_cxflags("-Wno-global-constructors")
    add_packages("catch2", "crc32c")
    add_defines("BYTECASK_TESTING")
    on_config(function(t)
        add_native_syslinks(t)
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
        add_native_syslinks(t)
        apply_sanitizer(t)
        add_release_opts(t)
    end)

target("engine_bench")
    set_kind("binary")
    set_default(false)
    add_files("benchmarks/engine_bench.cpp", "bytecaskdb/*.cppm", "bytecaskdb/bytecask.cpp")
    add_cxflags("-Wno-global-constructors")
    add_packages("benchmark", "crc32c", "rocksdb")
    add_defines("BENCH_NO_LEVELDB")
    on_config(function(t)
        add_native_syslinks(t)
        apply_sanitizer(t)
        add_release_opts(t)
    end)

target("memory_profile")
    set_kind("binary")
    set_default(false)
    add_files("benchmarks/memory_profile.cpp", "bytecaskdb/*.cppm", "bytecaskdb/bytecask.cpp")
    add_packages("crc32c", "jemalloc")
    on_config(function(t)
        add_native_syslinks(t)
        apply_sanitizer(t)
        add_release_opts(t)
    end)

-- Static library target for out-of-tree consumers (e.g. the MariaDB plugin).
-- Compiles all C++23 module sources and exposes them via libbytecask.a.
-- Note: C++23 module BMIs are not portable across translation units that
-- import them without the matching toolchain; the MariaDB plugin instead
-- uses the stable header-based C API in include/bytecask_c.h.
--
-- bytecaskdb/bytecask_c.cpp is compiled here because it imports
-- the C++23 bytecask module and must be built with the same toolchain that
-- produced the BMIs.
target("bytecask")
    set_kind("static")
    set_default(false)
    add_cxxflags("-fPIC", {force = true})  -- required when linking into a shared object (e.g. MariaDB plugin)
    add_files("bytecaskdb/*.cppm", "bytecaskdb/bytecask.cpp", "bytecaskdb/bytecask_c.cpp")
    add_packages("crc32c")
    on_config(function(t)
        add_native_syslinks(t)
        apply_sanitizer(t)
    end)

-- Python bindings via nanobind.
-- Wraps the C++23 module interface directly (not the C API).
-- Prerequisites: pip install nanobind
-- Build: xmake build bytecaskdb_python
-- Usage: PYTHONPATH=bytecaskdb-python python3 your_script.py
target("bytecaskdb_python")
    set_kind("shared")
    set_default(false)
    add_files("bytecaskdb-python/src/bytecaskdb_module.cpp")
    add_files("bytecaskdb/*.cppm", "bytecaskdb/bytecask.cpp")
    add_packages("crc32c")
    -- nanobind requires compiling nb_combined.cpp from the nanobind package.
    on_load(function(t)
        local python = os.getenv("BYTECASK_PYTHON") or "python3"
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
        t:set("targetdir", path.join(os.projectdir(), "bytecaskdb-python", "bytecaskdb"))
        -- Detect free-threaded Python and enable nanobind free-threading support.
        local gil_disabled = os.iorunv(python, {"-c",
            "import sysconfig; print(sysconfig.get_config_var('Py_GIL_DISABLED') or 0)"})
        if gil_disabled:trim() == "1" then
            t:add("cxxflags", "-DNB_FREE_THREADED", {force = true})
        end
    end)
    -- Suppress warnings from nanobind headers (third-party code).
    add_cxxflags("-Wno-old-style-cast", "-Wno-extra-semi-stmt", "-Wno-shadow",
                 "-Wno-covered-switch-default", "-Wno-cast-function-type-strict",
                 "-Wno-sign-conversion", "-Wno-double-promotion", "-Wno-shadow-field",
                 "-Wno-cast-qual", "-Wno-zero-as-null-pointer-constant",
                 "-Wno-missing-field-initializers", "-Wno-float-equal",
                 "-Wno-deprecated-declarations", "-Wno-nested-anon-types",
                 "-Wno-gnu-anonymous-struct", "-Wno-unused-function",
                 "-Wno-disabled-macro-expansion",
                 {force = true})
    add_cxxflags("-fPIC", {force = true})
    -- Resolve Python C API symbols at module load time (provided by the host
    -- interpreter), not at link time -- this is how nanobind and pybind11
    -- build extension modules.
    --   - Linux: shared objects allow undefined symbols by default. Do NOT
    --     pass `-Wl,--no-undefined` here: the Python C API symbols
    --     (PyBytes_*, _Py_Dealloc, ...) are intentionally unresolved and
    --     bound by the dynamic loader when the host interpreter loads the
    --     module.
    --   - macOS: pass `-undefined dynamic_lookup` so the linker tolerates
    --     unresolved Py* symbols; dyld binds them when Python loads the
    --     module. Linking against a framework Python's libpython is
    --     unreliable across runners (the lib dir may not expose a linkable
    --     dylib) and ties the wheel to a specific libpython location.
    if is_host("macosx") then
        add_shflags("-undefined", "dynamic_lookup", {force = true})
    end
    on_config(function(t)
        add_native_syslinks(t)
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
    on_config(function(t)
        add_native_syslinks(t)
        apply_sanitizer(t)
    end)

target("fuzz_hint_entry")
    set_kind("binary")
    set_default(false)
    add_files("tests/fuzz/fuzz_hint_entry.cpp", "bytecaskdb/*.cppm", "bytecaskdb/bytecask.cpp")
    add_includedirs("bytecaskdb")
    add_packages("crc32c")
    add_defines("BYTECASK_TESTING")
    on_config(function(t)
        add_native_syslinks(t)
        apply_sanitizer(t)
    end)

-- Seed corpus generator for fuzz targets.
-- Build: xmake build gen_fuzz_corpus
-- Run:   ./build/.../gen_fuzz_corpus   (writes to tests/fuzz/corpus/)
target("gen_fuzz_corpus")
    set_kind("binary")
    set_default(false)
    add_files("tests/fuzz/gen_corpus.cpp", "bytecaskdb/*.cppm", "bytecaskdb/bytecask.cpp")
    add_includedirs("bytecaskdb")
    add_packages("crc32c")
    on_config(function(t)
        add_native_syslinks(t)
    end)

-- ── WASM / Emscripten targets ───────────────────────────────────────────────
-- Only defined when the Emscripten SDK is activated (EMSDK env var set).
-- CI jobs that don't need WASM (e.g. Python wheel builds) skip these entirely.
if os.getenv("EMSDK") then
--
-- Custom toolchain that uses the EMSDK-bundled Clang for compilation (so
-- xmake's C++ module dependency scanner recognises it as real Clang) and
-- em++ for linking (where it adds JS glue and the WASM runtime).
--
-- Build:  xmake build wasm_smoke_test
-- Run:    node bytecaskdb-node/wasm/build/wasm_smoke_test.js
--
-- Prerequisites: Emscripten SDK activated, and WASM crc32c pre-built:
--   cd bytecaskdb-node/wasm && bash build.sh  (first run builds deps)

toolchain("emcc-clang")
    set_kind("standalone")
    set_description("Emscripten via EMSDK Clang (C++ module compatible)")

    on_check(function(toolchain)
        import("detect.sdks.find_emsdk")
        local emsdk = find_emsdk()
        if emsdk then
            toolchain:config_set("sdkdir", emsdk.sdkdir)
            toolchain:config_set("bindir", path.join(emsdk.sdkdir, "upstream", "bin"))
            toolchain:config_set("emscripten", emsdk.emscripten)
            return true
        end
        return false
    end)

    on_load(function(toolchain)
        local sdkdir = toolchain:config("sdkdir")
        local bindir = toolchain:config("bindir")
        local emscripten = toolchain:config("emscripten")
        local sysroot = path.join(sdkdir, "upstream", "emscripten", "cache", "sysroot")

        -- Real Clang for compilation — passes xmake's tool-name and version checks
        toolchain:set("toolset", "cxx", path.join(bindir, "clang++"))
        toolchain:set("toolset", "cc",  path.join(bindir, "clang"))
        -- em++ for linking — adds JS glue, WASM runtime, Emscripten libraries
        toolchain:set("toolset", "ld", path.join(emscripten, "em++"))
        toolchain:set("toolset", "sh", path.join(emscripten, "em++"))
        toolchain:set("toolset", "ar", path.join(emscripten, "emar"))

        -- Target wasm32-unknown-emscripten with the EMSDK sysroot
        toolchain:add("cxflags", "--target=wasm32-unknown-emscripten")
        toolchain:add("cxflags", "--sysroot=" .. sysroot)
        -- Emscripten compat headers (provides xlocale.h and others)
        toolchain:add("cxflags", "-Xclang", "-iwithsysroot/include/compat")
        -- Prevent the host GCC/libstdc++ headers from leaking into the WASM build
        toolchain:add("cxflags", "-nostdinc++")
        toolchain:add("cxflags", "-isystem" .. path.join(sysroot, "include", "c++", "v1"))
        toolchain:add("cxflags", "-DBYTECASK_SINGLE_THREADED")
        toolchain:add("cxflags", "-fwasm-exceptions")
    end)
toolchain_end()

-- Common WASM link flags applied to each WASM target via add_wasm_ldflags().
local wasm_dir = path.join(os.projectdir(), "bytecaskdb-node", "wasm")
local wasm_crc32c = path.join(wasm_dir, "build", "crc32c-wasm")

-- Common WASM target setup. Each WASM target calls this in on_config.
local function add_wasm_ldflags(t)
    t:add("ldflags",
        "-fwasm-exceptions",
        "-sNODERAWFS=1", "-sENVIRONMENT=node", "-lnoderawfs.js",
        "-sMALLOC=mimalloc", "-sALLOW_MEMORY_GROWTH", "-sEXIT_RUNTIME=1",
        "--pre-js", path.join(wasm_dir, "pre.js"),
        "--js-library", path.join(wasm_dir, "syscall_overrides.js"),
        "-L" .. path.join(wasm_crc32c, "lib"),
        {force = true})
end

-- Common WASM policies shared by all WASM targets.
local function set_wasm_policies()
    set_toolchains("emcc-clang")
    set_languages("c++23")
    set_policy("build.c++.modules", true)
    set_policy("build.c++.modules.clang.fallbackscanner", true)
    set_policy("build.c++.modules.std", false)
end

-- Run WASM targets via node, forwarding extra arguments.
local function wasm_on_run(target)
    import("core.base.option")
    local args = table.join({target:targetfile()}, option.get("arguments") or {})
    os.execv("node", args)
end

-- Common WASM source files and crc32c dependency.
local function add_wasm_sources()
    add_files("bytecaskdb/*.cppm", "bytecaskdb/bytecask.cpp")
    add_includedirs(path.join(wasm_crc32c, "include"))
    add_linkdirs(path.join(wasm_crc32c, "lib"))
    add_links("crc32c")
end

target("wasm_smoke_test")
    set_kind("binary")
    set_default(false)
    set_wasm_policies()
    add_wasm_sources()
    add_files("bytecaskdb-node/wasm/test_node.cpp")
    on_config(add_wasm_ldflags)
    on_run(wasm_on_run)
    set_extension(".js")
    set_targetdir(path.join(wasm_dir, "build"))

target("wasm_embind")
    set_kind("binary")
    set_default(false)
    set_wasm_policies()
    add_wasm_sources()
    add_files("bytecaskdb-node/wasm/bytecask_embind.cpp")
    on_config(function(t)
        t:add("ldflags",
            "-fwasm-exceptions",
            "-lembind",
            "-sNODERAWFS=1", "-sENVIRONMENT=node", "-lnoderawfs.js",
            "-sMALLOC=mimalloc", "-sALLOW_MEMORY_GROWTH",
            "-sMODULARIZE=1", "-sEXPORT_NAME=createByteCask",
            "--pre-js", path.join(wasm_dir, "pre.js"),
            "--js-library", path.join(wasm_dir, "syscall_overrides.js"),
            "-L" .. path.join(wasm_crc32c, "lib"),
            {force = true})
    end)
    set_basename("bytecask")
    set_extension(".mjs")
    set_targetdir(path.join(wasm_dir, "build"))

target("wasm_engine_bench")
    set_kind("binary")
    set_default(false)
    set_wasm_policies()
    add_wasm_sources()
    add_files("benchmarks/engine_bench.cpp")
    add_defines("BENCH_NO_LEVELDB", "BENCH_NO_ROCKSDB", "BENCH_NO_MT")
    add_cxflags("-Wno-global-constructors")
    on_config(function(t)
        local bench_prefix = path.join(wasm_dir, "build", "benchmark-wasm")
        t:add("includedirs", path.join(bench_prefix, "include"))
        t:add("linkdirs", path.join(bench_prefix, "lib"))
        t:add("links", "benchmark", "benchmark_main")
        add_wasm_ldflags(t)
    end)
    set_basename("engine_bench_nodefs")
    set_extension(".js")
    set_targetdir(path.join(wasm_dir, "build"))
    on_run(wasm_on_run)

target("wasm_memory_profile")
    set_kind("binary")
    set_default(false)
    set_wasm_policies()
    add_wasm_sources()
    add_files("benchmarks/memory_profile.cpp")
    on_config(add_wasm_ldflags)
    set_basename("memory_profile")
    set_extension(".js")
    set_targetdir(path.join(wasm_dir, "build"))
    on_run(wasm_on_run)

target("wasm_tests")
    set_kind("binary")
    set_default(false)
    set_wasm_policies()
    add_wasm_sources()
    add_files("tests/*.cpp", "tests/proof/generated/*.cpp")
    add_files("bytecaskdb-node/wasm/catch2_stringmakers.cpp")
    add_includedirs("bytecaskdb", "tests")
    add_defines("BYTECASK_TESTING")
    on_config(function(t)
        local catch2_prefix = path.join(wasm_dir, "build", "catch2-wasm")
        t:add("includedirs", path.join(catch2_prefix, "include"))
        t:add("linkdirs", path.join(catch2_prefix, "lib"))
        t:add("links", "Catch2Main", "Catch2")
        add_wasm_ldflags(t)
        t:add("ldflags", "-sSTACK_SIZE=2097152", {force = true})
    end)
    set_basename("bytecask_tests")
    set_extension(".js")
    set_targetdir(path.join(wasm_dir, "build"))
    -- Single-threaded WASM cannot run threading tests. Automatically exclude
    -- [concurrency] and [lock] tagged tests, matching run_tests.sh behavior.
    on_run(function(target)
        import("core.base.option")
        local user_args = option.get("arguments") or {}
        local args = table.join({target:targetfile()}, user_args)
        if #user_args == 0 then
            table.insert(args, "~[concurrency]")
            table.insert(args, "~[lock]")
        end
        os.execv("node", args)
    end)

end -- if os.getenv("EMSDK")