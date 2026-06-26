-- premake5.lua -- alternative build system for HPL2 / Amnesia (Surfel-GI fork)
--
-- This is *additive*: the CMake build (CMakeLists.txt) and the Visual Studio
-- solution (Amnesia.sln) are untouched and remain the primary build paths.
-- This file lets you generate a Visual Studio solution or gmake2 Makefiles via:
--
--     premake5 vs2022       (Windows)
--     premake5 gmake2       (Linux)
--     make -C build-premake config=release -j$(nproc)
--
-- Most in-tree third-party libs are built from source as hand-written premake
-- projects (premake/deps/*.lua). The two libraries that ship their own large,
-- feature-probing CMake build systems -- SDL2 and openal-soft -- are built by
-- driving their own CMake as a prebuild step and then linked (premake/external.lua).
-- See BUILD.md "Building with premake" for details.

-- Repo root (directory containing this file). Every path below is built off it
-- so the included sub-scripts don't depend on their own location.
ROOT = _MAIN_SCRIPT_DIR

dofile "premake/options.lua"
dofile "premake/helpers.lua"

workspace "Amnesia"
    location (ROOT .. "/build-premake")
    configurations { "Debug", "Release" }
    architecture "x86_64"
    cppdialect "C++20"
    cdialect "gnu11"   -- match CMake's default (GNU extensions: alloca, etc.)
    startproject "Amnesia"

    -- Match CMP0091 / CMAKE_MSVC_RUNTIME_LIBRARY = MultiThreaded$<Debug>DLL
    staticruntime "off"

    filter "configurations:Debug"
        defines { "_DEBUG" }
        symbols "On"
        runtime "Debug"
    filter "configurations:Release"
        defines { "NDEBUG" }
        optimize "On"
        runtime "Release"
    filter {}

    filter "system:windows"
        systemversion "latest"
        defines { "NOMINMAX=1", "ML_INTRINSIC_LEVEL=1", "_CRT_SECURE_NO_WARNINGS" }
        flags { "MultiProcessorCompile" }
    filter {}

-- External CMake-driven dependencies (SDL2 + openal-soft).
dofile "premake/external.lua"

-- Hand-written from-source third-party dependency projects.
dofile "premake/deps/zlib.lua"
dofile "premake/deps/ogg.lua"
dofile "premake/deps/vorbis.lua"
dofile "premake/deps/jpeg.lua"
dofile "premake/deps/png.lua"
dofile "premake/deps/tinyxml2.lua"
dofile "premake/deps/freealut.lua"
dofile "premake/deps/newton.lua"
dofile "premake/deps/angelscript.lua"
dofile "premake/deps/devil.lua"
dofile "premake/deps/volk.lua"
dofile "premake/deps/oalwrapper.lua"
-- MathLib is header-only (interface): no project, consumed via mathlib_use() in helpers.

-- Engine, game and tools.
dofile "premake/hpl2.lua"
dofile "premake/amnesia.lua"
if _OPTIONS["with-tools"] ~= "no" then
    dofile "premake/tools.lua"
end
