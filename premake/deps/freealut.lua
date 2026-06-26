-- freealut (ALUT) -- mirrors extern/CMakeLists.txt. Core AL headers come from the
-- openal-soft submodule; alut.h (freealut's own public header) sits under
-- sources/freealut/AL.
project "freealut"
    kind "StaticLib"
    language "C"
    set_output("static")

    files { DEPS_SOURCES .. "/freealut/*.c" }
    includedirs {
        DEPS_EXTERN .. "/openal-soft/include",      -- <AL/al.h>, <AL/alc.h>, <AL/alext.h>
        DEPS_EXTERN .. "/openal-soft/include/AL",    -- <al.h>, <alc.h>
        DEPS_SOURCES .. "/freealut",
        DEPS_SOURCES .. "/freealut/AL",              -- <alut.h>
    }
    defines { "HAVE_STDINT_H=1" }

    filter "system:windows"
        defines { "HAVE__STAT=1", "HAVE_SLEEP=1", "HAVE_WINDOWS_H=1" }
    filter "system:not windows"
        defines { "HAVE_STAT=1", "HAVE_UNISTD_H=1", "HAVE_USLEEP=1" }
    filter "toolset:gcc or clang"
        defines { "HAVE___ATTRIBUTE__=1" }
    filter {}
