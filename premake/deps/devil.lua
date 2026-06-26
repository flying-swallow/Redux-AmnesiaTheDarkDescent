-- DevIL (image library, IL) -- mirrors sources/DevIL/CMakeLists.txt.
-- The configured config.h (with IL_NO_TIF / IL_NO_JP2) lives in
-- DevIL/DevIL/src-IL/include and MUST resolve before the template copy in include/.
project "IL"
    kind "StaticLib"
    language "C++"
    set_output("static")

    files { DEPS_SOURCES .. "/DevIL/src/*.cpp" }
    includedirs {
        DEPS_SOURCES .. "/DevIL/DevIL/src-IL/include", -- configured config.h first
        DEPS_SOURCES .. "/DevIL/include",              -- internal il_*.h + public IL/il.h
        -- codecs wired up by the parent (IL_PNG_LIB / IL_JPEG_LIB ON)
        DEPS_SOURCES .. "/png",
        DEPS_SOURCES .. "/jpeg",
        DEPS_EXTERN .. "/zlib",
    }
    links { "png", "jpeg" }

    filter "system:linux"
        includedirs { CONFIG_DIR .. "/linux/zlib" }
    filter "system:windows"
        includedirs { CONFIG_DIR .. "/windows/zlib" }
        defines { "IL_STATIC_LIB", "JPEGSTATIC" }
    filter "toolset:gcc or clang"
        buildoptions { "-fpermissive" }
    filter {}
