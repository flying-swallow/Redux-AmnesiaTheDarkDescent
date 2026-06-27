-- premake/helpers.lua -- shared paths, glob, output-dir and prebuild helpers.
-- All helpers are global so the dofile'd sub-scripts can use them directly.

DEPS_EXTERN  = ROOT .. "/HPL2/extern"
DEPS_SOURCES = DEPS_EXTERN   -- merged: submodules + vendored sources share one dir
CONFIG_DIR   = ROOT .. "/premake/config"   -- pre-generated, per-platform config headers
BUILD_OUT    = "%{wks.location}"           -- build-premake/

-- Public headers for the in-tree dependencies. Replaces the old aggregated
-- HPL2/dependencies/include tree (removed): submodule libs (ogg, vorbis) expose
-- their own include/, vendored libs (DevIL, Newton, OALWrapper, freealut) expose
-- headers next to their sources. openal-soft and SDL headers come separately via
-- link_openal() / link_sdl2().
function deps_public_includes()
    includedirs {
        CONFIG_DIR .. "/common",            -- generated <ogg/config_types.h>
        DEPS_EXTERN .. "/ogg/include",      -- <ogg/*.h>
        DEPS_EXTERN .. "/vorbis/include",   -- <vorbis/*.h>
        DEPS_SOURCES .. "/DevIL/include",   -- <IL/il.h>
        DEPS_SOURCES .. "/Newton",          -- <Newton.h>
        DEPS_SOURCES .. "/OpenAL",          -- "OpenAL/OAL_*.h" (OALWrapper public)
        DEPS_SOURCES .. "/freealut",        -- <AL/EFX-Util.h>, <AL/xram.h>
        DEPS_SOURCES .. "/freealut/AL",     -- <alut.h>
    }
end

-- Mirror CMake's set_output_dir(): runtime artifacts + shared libs go to
-- build-premake/amnesia/<config>/ alongside the game; the optional subdir
-- (e.g. "libs") nests under it.
function runtime_dir(subdir)
    local d = BUILD_OUT .. "/amnesia/%{cfg.buildcfg}"
    if subdir and subdir ~= "" then d = d .. "/" .. subdir end
    return d
end

-- Standard output layout for a target. kind: "runtime" (exe / shared lib that
-- must sit next to Amnesia) or "static" (intermediate static lib).
function set_output(kind)
    objdir (BUILD_OUT .. "/obj/%{prj.name}/%{cfg.buildcfg}")
    if kind == "runtime" then
        targetdir (runtime_dir(""))
        -- The engine resolves config/resources relative to the working dir, and
        -- the Windows WinMain entry (unlike the Linux main) does not chdir to the
        -- data dir. Make F5/Debug from VS run inside the deployed game folder so
        -- it finds resources.cfg / config/*.cfg next to the exe.
        debugdir (runtime_dir(""))
    else
        targetdir (BUILD_OUT .. "/lib/%{cfg.buildcfg}")
    end
end

-- glob(patterns) -> expands a list of ROOT-relative or absolute matchfiles
-- patterns into a flat list (mirrors CMake file(GLOB ...)).
function glob(patterns)
    local out = {}
    for _, p in ipairs(patterns) do
        for _, f in ipairs(os.matchfiles(p)) do
            table.insert(out, f)
        end
    end
    return out
end

-- MathLib is header-only. Consumers call mathlib_use() to pull in its include
-- dir and the SSE flag GCC/Clang need (MSVC enables SSE implicitly on x64).
function mathlib_use()
    includedirs { DEPS_EXTERN .. "/MathLib" }
    filter { "toolset:gcc or clang" }
        buildoptions { "-msse4.2" }
    filter {}
end

-- Vulkan headers + VMA are header-only. Also emit the platform surface defines:
-- CMake sets these PUBLIC on the volk target (extern/volk/CMakeLists.txt), so
-- every volk consumer inherits them. premake has no such propagation, and the
-- engine's surface-creation code (RISwapchain.cpp) is #ifdef-gated on
-- VK_USE_PLATFORM_*_KHR -- without the define here the surface is never created
-- and the first vkGetPhysicalDeviceSurfaceSupportKHR call dereferences garbage.
function vulkan_includes()
    includedirs {
        DEPS_EXTERN .. "/Vulkan-Headers/include",
        DEPS_EXTERN .. "/VulkanMemoryAllocator/include",
        DEPS_EXTERN .. "/volk",
    }
    vulkan_platform_defines()
end

-- Per-platform Vulkan surface defines, mirroring extern/CMakeLists.txt VOLK_STATIC_DEFINES.
function vulkan_platform_defines()
    filter "system:windows"
        defines { "VK_USE_PLATFORM_WIN32_KHR" }
    filter "system:linux"
        if _OPTIONS["graphics-x11"] ~= "off" then
            defines { "VK_USE_PLATFORM_XLIB_KHR" }
        end
        if _OPTIONS["graphics-wayland"] ~= "off" then
            defines { "VK_USE_PLATFORM_WAYLAND_KHR" }
        end
    filter {}
end

-- BuildID source/header. These files are checked into the tree (the CMake
-- GenerateBuildID custom command is dormant -- a version_source/version_sources
-- typo means it never runs -- so the committed copies are what actually build).
-- We just add the committed per-platform source + header to the file list.
function buildid(idname, dir)
    local suffix = os.target() == "windows" and "Win32" or "Linux"
    files {
        dir .. "/BuildID_" .. idname .. "_" .. suffix .. ".cpp",
        dir .. "/BuildID_" .. idname .. ".h",
    }
end

-- Pinned Slang release for the premake auto-download. Keep in sync with
-- SLANG_VERSION in cmake/slang.cmake.
SLANG_VERSION  = "2026.11"
SLANG_PREBUILT = ROOT .. "/build-premake/_deps/slang-prebuilt"

local function slangc_exe()
    return os.target() == "windows" and "slangc.exe" or "slangc"
end

-- Host CPU -> Slang asset arch token. The workspace pins x86_64, but detect so an
-- aarch64 host still resolves the right asset (mirrors cmake/slang.cmake).
local function slang_host_arch()
    if os.target() == "windows" then
        local pa = (os.getenv("PROCESSOR_ARCHITECTURE") or ""):lower()
        return pa:find("arm") and "aarch64" or "x86_64"
    end
    local m = (os.outputof("uname -m") or "x86_64"):lower():gsub("%s+", "")
    return (m == "aarch64" or m == "arm64") and "aarch64" or "x86_64"
end

-- Download + extract the pinned prebuilt slangc at configure time (when premake5
-- runs), mirroring cmake/slang.cmake. Pure Lua via premake's http.download +
-- zip.extract -- no python, no external tar: we fetch the .zip asset, which Slang
-- publishes for every platform. Idempotent (skips if slangc is already present).
local function download_slangc()
    local root   = SLANG_PREBUILT .. "/slang-" .. SLANG_VERSION
    local slangc = root .. "/bin/" .. slangc_exe()

    -- Tolerate both archive layouts: bin/slangc directly, or wrapped */bin/slangc.
    local function find_extracted()
        if os.isfile(slangc) then return slangc end
        local m = os.matchfiles(root .. "/**/bin/" .. slangc_exe())
        return m[1]
    end

    local found = find_extracted()
    if found then return found end

    local os_name = os.target()
    if os_name == "macosx" then os_name = "macos" end
    local asset = string.format("slang-%s-%s-%s.zip", SLANG_VERSION, os_name, slang_host_arch())
    local url   = "https://github.com/shader-slang/slang/releases/download/v"
        .. SLANG_VERSION .. "/" .. asset
    local archive = SLANG_PREBUILT .. "/" .. asset

    os.mkdir(root)
    print("Slang: downloading " .. url)
    local res, code = http.download(url, archive, {})
    if res ~= "OK" then
        os.remove(archive)
        error(string.format("Slang: download failed (%s, code %s). URL: %s", tostring(res), tostring(code), url))
    end
    zip.extract(archive, root)
    os.remove(archive)

    found = find_extracted()
    if not found then
        error("Slang: could not locate slangc inside extracted archive at " .. root)
    end
    if os.target() ~= "windows" then
        os.execute(string.format('chmod +x "%s"', found))  -- zip extraction drops the exec bit
    end
    return found
end

-- Resolve a slangc executable for shader compilation.
--  1. --slangc=<path>
--  2. a copy already extracted under build/_deps (CMake) or build-premake/_deps
--  3. the pinned prebuilt release, downloaded now (configure time) if absent
function resolve_slangc()
    if _OPTIONS["slangc"] then return _OPTIONS["slangc"] end
    local matches = os.matchfiles(ROOT .. "/build/_deps/slang-prebuilt/**/bin/" .. slangc_exe())
    if #matches > 0 then return matches[1] end
    return download_slangc()
end

-- Slang entry-point stage suffixes -- a .slang file is a shader to compile only if
-- its name ends in one of these (mirrors STAGE_SUFFIXES/is_entry_shader in the old
-- scripts/compile_slang_shaders.py and the explicit SHADERS list in
-- amnesia/CMakeLists.txt). The remaining .slang files are include-only headers,
-- pulled in via the -I path rather than compiled.
local SLANG_STAGE_SUFFIXES = {
    ".vert.slang", ".frag.slang", ".comp.slang", ".cs.slang", ".geom.slang",
    ".tesc.slang", ".tese.slang", ".rgen.slang", ".rchit.slang", ".rmiss.slang",
    ".rahit.slang", ".rint.slang", ".rcall.slang", ".rt.slang", ".3d.slang",
}

local function is_entry_shader(file)
    for _, suffix in ipairs(SLANG_STAGE_SUFFIXES) do
        if file:sub(-#suffix) == suffix then return true end
    end
    return false
end

-- Compile every entry-point .slang shader under amnesia/slang into
-- <runtime>/compiled_shaders. Native port of cmake/shaders.cmake's
-- _target_shaders_compile_slang: one per-file custom build rule per shader (the
-- premake analogue of add_custom_command), so only changed shaders recompile.
-- Must be called inside the consuming project so files{}/filter{} apply to it.
function slang_prebuild()
    -- slangc is resolved (and auto-downloaded if needed) here at configure time.
    local slangc = resolve_slangc()
    local src = ROOT .. "/amnesia/slang"
    local out = runtime_dir("") .. "/compiled_shaders"

    -- Add only the entry shaders to the file list; the per-file rule below matches
    -- them via "files:**.slang". Include-only headers stay off the list (uncompiled)
    -- but remain reachable through the -I path.
    local shaders = {}
    for _, f in ipairs(os.matchfiles(src .. "/**.slang")) do
        if is_entry_shader(f) then table.insert(shaders, f) end
    end
    files(shaders)

    -- Same flag set as cmake/shaders.cmake (incl. -emit-spirv-directly). %{file.*}
    -- tokens are expanded per shader at build time; %{file.basename} strips only the
    -- trailing .slang (foo.vert.slang -> foo.vert), matching CMake's STEM LAST_ONLY.
    local flags = "-target spirv -profile sm_6_6 -emit-spirv-directly "
        .. "-fvk-use-entrypoint-name -matrix-layout-column-major -fvk-use-scalar-layout"
    local compile = string.format(
        '"%s" "%%{file.abspath}" %s -I"%%{file.directory}" -I"%s" -o "%s/%%{file.basename}.spv"',
        slangc, flags, src, out)

    filter "files:**.slang"
        buildmessage "Slang %{file.relpath}"
        buildinputs { slangc }
        buildoutputs { out .. "/%{file.basename}.spv" }
    filter { "files:**.slang", "system:not windows" }
        buildcommands {
            string.format('mkdir -p "%s"', out),
            compile,
        }
    filter { "files:**.slang", "system:windows" }
        buildcommands {
            string.format('if not exist "%s" mkdir "%s"', out, out),
            compile,
        }
    filter {}
end

CMAKE = _OPTIONS["cmake"] or "cmake"

-- Link the engine + its full dependency set into a final executable. premake
-- does NOT transitively propagate a static library's links to consumers, so
-- every executable (Amnesia, editors, MshConverter) must link the whole set.
-- linkgroups handles the circular static-lib references (vorbis<->ogg, etc.).
function link_engine()
    links {
        "HPL2", "OALWrapper", "AngelScript", "Newton", "tinyxml2",
        "vorbisfile", "vorbis", "ogg", "freealut",
        "zlib", "volk", "IL", "png", "jpeg",
    }
    link_sdl2()
    link_openal()
    filter "system:linux"
        links { "pthread", "dl" }
    filter "toolset:gcc or clang"
        linkgroups "On"
    filter {}
end
