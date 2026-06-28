-- libjpeg (9d, jmemnobs) -- explicit list mirrors sources/jpeg/CMakeLists.txt.
project "jpeg"
    kind "StaticLib"
    language "C"
    set_output("static")

    local jpeg_dir = DEPS_SOURCES .. "/jpeg/"
    local names = {
        "jaricom", "jcapimin", "jcapistd", "jcarith", "jccoefct", "jccolor",
        "jcdctmgr", "jchuff", "jcinit", "jcmainct", "jcmarker", "jcmaster",
        "jcomapi", "jcparam", "jcprepct", "jcsample", "jctrans",
        "jdapimin", "jdapistd", "jdarith", "jdatadst", "jdatasrc", "jdcoefct",
        "jdcolor", "jddctmgr", "jdhuff", "jdinput", "jdmainct", "jdmarker",
        "jdmaster", "jdmerge", "jdpostct", "jdsample", "jdtrans",
        "jerror", "jfdctflt", "jfdctfst", "jfdctint", "jidctflt", "jidctfst",
        "jidctint", "jmemmgr", "jmemnobs", "jquant1", "jquant2", "jutils",
    }
    for _, n in ipairs(names) do files { jpeg_dir .. n .. ".c" } end
    includedirs { DEPS_SOURCES .. "/jpeg" }

    filter "system:windows"
        disablewarnings { "4267", "4244", "4018", "4996" }
    filter {}
