-- tinyxml2 -- single translation unit (xmltest.cpp is the test program, excluded).
project "tinyxml2"
    kind "StaticLib"
    language "C++"
    set_output("static")

    files { DEPS_EXTERN .. "/tinyxml2/tinyxml2.cpp" }
    includedirs { DEPS_EXTERN .. "/tinyxml2" }
    defines { "_FILE_OFFSET_BITS=64" }

    filter "configurations:Debug"
        defines { "TINYXML2_DEBUG" }
    filter {}
