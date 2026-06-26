-- libpng (1.6.37) -- explicit list mirrors sources/png/CMakeLists.txt.
-- pnglibconf.h is checked in alongside the sources.
project "png"
    kind "StaticLib"
    language "C"
    set_output("static")

    local png_dir = DEPS_SOURCES .. "/png/"
    local names = {
        "png", "pngerror", "pngget", "pngmem", "pngpread", "pngread", "pngrio",
        "pngrtran", "pngrutil", "pngset", "pngtrans", "pngwio", "pngwrite",
        "pngwtran", "pngwutil",
    }
    for _, n in ipairs(names) do files { png_dir .. n .. ".c" } end

    includedirs { DEPS_SOURCES .. "/png", DEPS_EXTERN .. "/zlib" }
    links { "zlib" }

    filter "system:linux"
        includedirs { CONFIG_DIR .. "/linux/zlib" }
    filter "system:windows"
        includedirs { CONFIG_DIR .. "/windows/zlib" }
        disablewarnings { "4267", "4244", "4018", "4996" }
    filter {}
