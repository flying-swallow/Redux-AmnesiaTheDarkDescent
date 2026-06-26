-- HPL2 engine -- static library, mirrors HPL2/CMakeLists.txt.
local CORE = ROOT .. "/HPL2/core"

project "HPL2"
    kind "StaticLib"
    language "C++"
    set_output("static")
    defines { "USE_SDL2" }

    -- All source patterns are run through glob()/os.matchfiles so, exactly like
    -- CMake's file(GLOB ...), patterns matching nothing are silently dropped
    -- (several literal names in the impl list don't exist on disk).
    local IMPL = CORE .. "/sources/impl/"
    local common_dirs = {
        "ai", "engine", "generate", "graphics", "gui", "haptic", "input",
        "math", "physics", "resources", "scene", "sound", "system",
    }
    local patterns = {}
    for _, d in ipairs(common_dirs) do
        table.insert(patterns, CORE .. "/sources/" .. d .. "/*.cpp")
        table.insert(patterns, CORE .. "/sources/" .. d .. "/*.c")
    end
    -- Implementation-specific sources (mirrors impl_sources block).
    local impl_patterns = {
        "SqScript.cpp", "scriptarray.cpp", "scripthelper.cpp",
        "scriptstring.cpp", "scriptstring_utils.cpp",
        "BitmapLoader*", "*Newton.cpp", "LegacyVertexBuffer.cpp",
        "GamepadSDL.cpp", "GamepadSDL2.cpp", "KeyboardSDL.cpp", "MouseSDL.cpp",
        "TimerSDL.cpp", "LowLevelGraphicsSDL.cpp", "LowLevelInputSDL.cpp",
        "LowLevelResourcesSDL.cpp", "LowLevelSystemSDL.cpp", "SDLEngineSetup.cpp",
        "SDLFontData.cpp", "LowLevelSoundOpenAL.cpp", "OpenAL*",
        "MeshLoaderCollada.cpp", "MeshLoaderColladaHelpers.cpp",
        "MeshLoaderColladaLoader.cpp", "MeshLoaderMSH.cpp", "MeshLoaderFBX.cpp",
        "ThreadSDL.cpp", "MutexSDL.cpp", "VertexBuffer.cpp",
    }
    for _, p in ipairs(impl_patterns) do table.insert(patterns, IMPL .. p) end
    table.insert(patterns, CORE .. "/sources/platform/sdl2/*.cpp")
    files (glob(patterns))
    files { CORE .. "/include/**.h" }   -- headers for IDE/source groups

    filter "system:linux"
        files (glob { IMPL .. "PlatformUnix.cpp", IMPL .. "PlatformSDL.cpp" })
    filter "system:windows"
        files (glob { IMPL .. "PlatformWin32.cpp", IMPL .. "MutexWin32.cpp", IMPL .. "ThreadWin32.cpp" })
    filter {}

    -- BuildID_HPL2_0.h is committed under core/include and only referenced from
    -- a commented-out line, so no generated source is needed here.

    -- Include directories. premake does not propagate dependency includes, so
    -- everything HPL2's sources #include must be listed explicitly.
    includedirs {
        CORE .. "/include",
        ROOT .. "/HPL2/include",            -- BuildID_HPL2_0.h
        ROOT .. "/amnesia/glsl",
        ROOT .. "/amnesia/slang",
        DEPS_SOURCES .. "/AngelScript/include",
        DEPS_EXTERN .. "/tinyxml2",
    }
    deps_public_includes()   -- ogg/vorbis/IL/Newton/OALWrapper public headers
    vulkan_includes()
    link_sdl2()      -- SDL2 headers + link + dependson
    link_openal()    -- openal-soft headers + link + dependson
    mathlib_use()

    -- Link the full dependency set; premake propagates these to the final
    -- executables that link HPL2.
    links {
        "OALWrapper", "AngelScript", "Newton", "tinyxml2",
        "vorbisfile", "vorbis", "ogg", "freealut",
        "zlib", "volk", "IL", "png", "jpeg",
    }

    filter "system:linux"
        links { "pthread", "dl" }
    filter "toolset:gcc or clang"
        exceptionhandling "Off"             -- mirrors -fno-exceptions
        linkgroups "On"                     -- resolve circular static-lib deps
    filter "system:windows"
        defines { "WIN32_LEAN_AND_MEAN", "SDL_MAIN_HANDLED", "IL_STATIC_LIB" }
        buildoptions { "/EHs-c-" }
    filter {}
