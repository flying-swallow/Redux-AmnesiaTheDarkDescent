-- ogg (Xiph container) -- mirrors the 2-file build in extern/CMakeLists.txt.
project "ogg"
    kind "StaticLib"
    language "C"
    set_output("static")

    files {
        DEPS_EXTERN .. "/ogg/src/bitwise.c",
        DEPS_EXTERN .. "/ogg/src/framing.c",
    }
    includedirs {
        CONFIG_DIR .. "/common",         -- generated <ogg/config_types.h>
        DEPS_EXTERN .. "/ogg/include",   -- public ogg/ headers
    }
