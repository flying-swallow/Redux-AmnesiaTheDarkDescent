-- volk (Vulkan loader) -- single source file; Vulkan headers are header-only.
project "volk"
    kind "StaticLib"
    language "C"
    set_output("static")

    files { DEPS_EXTERN .. "/volk/volk.c" }
    includedirs {
        DEPS_EXTERN .. "/volk",
        DEPS_EXTERN .. "/Vulkan-Headers/include",
    }
    vulkan_platform_defines()

    filter "system:linux"
        links { "dl" }
    filter {}
