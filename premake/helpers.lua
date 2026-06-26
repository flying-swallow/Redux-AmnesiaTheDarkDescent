-- premake/helpers.lua -- shared paths, glob, output-dir and prebuild helpers.
-- All helpers are global so the dofile'd sub-scripts can use them directly.

DEPS_EXTERN  = ROOT .. "/HPL2/dependencies/extern"
DEPS_SOURCES = ROOT .. "/HPL2/dependencies/sources"
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

-- Vulkan headers + VMA are header-only.
function vulkan_includes()
    includedirs {
        DEPS_EXTERN .. "/Vulkan-Headers/include",
        DEPS_EXTERN .. "/VulkanMemoryAllocator/include",
        DEPS_EXTERN .. "/volk",
    }
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

-- Resolve a slangc executable for shader compilation.
--  1. --slangc=<path>
--  2. the copy the CMake build downloaded under build/_deps/slang-prebuilt/
--  3. "slangc" on PATH (e.g. from the Vulkan SDK)
function resolve_slangc()
    if _OPTIONS["slangc"] then return _OPTIONS["slangc"] end
    local exe = os.target() == "windows" and "slangc.exe" or "slangc"
    local matches = os.matchfiles(ROOT .. "/build/_deps/slang-prebuilt/**/bin/" .. exe)
    if #matches > 0 then return matches[1] end
    return exe  -- assume on PATH
end

-- Compile every .slang shader under amnesia/slang into <runtime>/compiled_shaders
-- as a prebuild step, reusing the standalone scripts/compile_slang_shaders.py
-- (same flags as cmake/shaders.cmake).
function slang_prebuild()
    local slangc = resolve_slangc()
    local py = ROOT .. "/scripts/compile_slang_shaders.py"
    local src = ROOT .. "/amnesia/slang"
    local out = runtime_dir("") .. "/compiled_shaders"
    filter "system:not windows"
        prebuildcommands {
            string.format('mkdir -p "%s"', out),
            string.format('SLANGC="%s" python3 "%s" "%s" "%s" "%s"', slangc, py, src, out, src),
        }
    filter "system:windows"
        prebuildcommands {
            string.format('if not exist "%s" mkdir "%s"', out, out),
            string.format('set "SLANGC=%s" && python "%s" "%s" "%s" "%s"', slangc, py, src, out, src),
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
