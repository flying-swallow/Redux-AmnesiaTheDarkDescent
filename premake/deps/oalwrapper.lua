-- OALWrapper -- mirrors extern/CMakeLists.txt. Wraps OpenAL + ogg/vorbis + SDL2.
project "OALWrapper"
    kind "StaticLib"
    language "C++"
    set_output("static")

    files { DEPS_SOURCES .. "/OpenAL/*.cpp" }
    defines { "USE_SDL2" }
    includedirs {
        CONFIG_DIR .. "/common",            -- generated <ogg/config_types.h>
        DEPS_EXTERN .. "/ogg/include",      -- <ogg/*.h>
        DEPS_EXTERN .. "/vorbis/include",   -- <vorbis/*.h>
        DEPS_SOURCES .. "/OpenAL",          -- sources + public "OpenAL/OAL_*.h"
        DEPS_SOURCES .. "/freealut",        -- <AL/EFX-Util.h>, <AL/xram.h>, <AL/alut.h>
        ROOT .. "/HPL2/core/include",
    }
    links { "ogg", "vorbisfile", "vorbis", "freealut" }

    -- SDL2 + OpenAL headers/links (built via their own CMake).
    link_sdl2()
    link_openal()
