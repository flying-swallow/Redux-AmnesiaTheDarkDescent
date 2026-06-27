-- premake/options.lua -- command-line options, mirroring the CMake -D switches.

newoption {
    trigger = "slangc",
    value = "PATH",
    description = "Path to a slangc executable. If omitted, the script reuses one already "
        .. "extracted under build/_deps/ (CMake), otherwise it downloads the pinned release "
        .. "(SLANG_VERSION in premake/helpers.lua) at configure time."
}

newoption {
    trigger = "game-dir",
    value = "PATH",
    description = "Path to your installed Amnesia: The Dark Descent folder (used by the deploy action)."
}

newoption {
    trigger = "with-tools",
    value = "yes/no",
    description = "Build the HPL2 editors/tools (default yes). Mirrors CMake WITH_TOOLS.",
    allowed = { { "yes", "Build tools" }, { "no", "Skip tools" } },
    default = "yes",
}

newoption {
    trigger = "graphics-x11",
    value = "on/off",
    description = "(Linux) Enable the X11 Vulkan surface backend (default on). Mirrors USE_GRAPHICS_X11.",
    allowed = { { "on", "" }, { "off", "" } },
    default = "on",
}

newoption {
    trigger = "graphics-wayland",
    value = "on/off",
    description = "(Linux) Enable the Wayland Vulkan surface backend (default on). Mirrors USE_GRAPHICS_WAYLAND.",
    allowed = { { "on", "" }, { "off", "" } },
    default = "on",
}

newoption {
    trigger = "cmake",
    value = "PATH",
    description = "Path to the cmake executable used to build SDL2 + openal-soft (default 'cmake' on PATH)."
}
