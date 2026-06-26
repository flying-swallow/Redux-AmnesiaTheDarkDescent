-- premake/external.lua -- SDL2 and openal-soft built by driving their own CMake.
--
-- These two libraries ship large, feature-probing CMake build systems that are
-- impractical (and brittle) to reimplement in premake. Each is wrapped in a
-- premake kind "Makefile" project whose buildcommands run the library's own
-- CMake (configure + build) into build-premake/external/<name>/<config>, then
-- copy the resulting shared library next to the game. Consumers pull in the
-- headers/links via link_sdl2() / link_openal().
--
-- Both are built SHARED so the artifacts are self-contained (no transitive
-- static-link ordering against openal-soft's bundled fmt, etc.).

EXT_ROOT = ROOT .. "/build-premake/external"
local RUNTIME_LIBS = ROOT .. "/build-premake/amnesia/%{cfg.buildcfg}/libs"

local function cmake_makefile(name, srcdir, extra_args, copy_glob)
    local bdir = EXT_ROOT .. "/" .. name .. "/%{cfg.buildcfg}"
    project(name)
        kind "Makefile"
        location (ROOT .. "/build-premake/projects")

        buildcommands {
            string.format('"%s" -S "%s" -B "%s" -DCMAKE_BUILD_TYPE=%%{cfg.buildcfg} %s',
                CMAKE, srcdir, bdir, extra_args),
            string.format('"%s" --build "%s" --config %%{cfg.buildcfg} -j', CMAKE, bdir),
        }
        rebuildcommands {
            string.format('"%s" --build "%s" --config %%{cfg.buildcfg} -j', CMAKE, bdir),
        }
        cleancommands {
            string.format('{RMDIR} "%s"', bdir),
        }
        filter "system:not windows"
            buildcommands {
                string.format('mkdir -p "%s"', RUNTIME_LIBS),
                string.format('cp -P %s "%s"/ 2>/dev/null || true', bdir .. "/" .. copy_glob, RUNTIME_LIBS),
            }
        filter {}
end

-- ---- SDL2 -----------------------------------------------------------------
cmake_makefile("sdl2_ext",
    DEPS_EXTERN .. "/SDL",
    "-DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST=OFF -DSDL2_DISABLE_INSTALL=ON -DSDL_RPATH=OFF",
    "libSDL2*.so*")

function link_sdl2()
    dependson { "sdl2_ext" }
    -- ORDER MATTERS: the generated SDL_config.h (which #defines the X11/Wayland
    -- video drivers) must be found before the submodule's committed
    -- include/SDL_config.h, whose Linux fallback is SDL_config_minimal.h (no
    -- video drivers). With the wrong order the engine compiles SDL_syswm with no
    -- SDL_VIDEO_DRIVER_* defined and GetWindowHandle() hits assert(false).
    includedirs {
        -- generated SDL_config.h candidates (standalone build tree) -- FIRST
        EXT_ROOT .. "/sdl2_ext/%{cfg.buildcfg}/include-config-release/SDL2",
        EXT_ROOT .. "/sdl2_ext/%{cfg.buildcfg}/include-config-debug/SDL2",
        EXT_ROOT .. "/sdl2_ext/%{cfg.buildcfg}/include",
        EXT_ROOT .. "/sdl2_ext/%{cfg.buildcfg}/include/SDL2",
        -- submodule public headers (committed SDL_config.h resolves to minimal)
        DEPS_EXTERN .. "/SDL/include",
        DEPS_EXTERN .. "/SDL/include/SDL2",
    }
    filter "system:linux"
        includedirs { CONFIG_DIR .. "/linux/SDL2" }   -- harvested fallback
        libdirs { EXT_ROOT .. "/sdl2_ext/%{cfg.buildcfg}" }
    filter { "system:linux", "configurations:Release" }
        links { "SDL2-2.0" }
    filter { "system:linux", "configurations:Debug" }
        links { "SDL2-2.0d" }
    filter "system:windows"
        libdirs { EXT_ROOT .. "/sdl2_ext/%{cfg.buildcfg}/%{cfg.buildcfg}" }
        links { "SDL2" }
    filter {}
end

-- ---- openal-soft -----------------------------------------------------------
cmake_makefile("openal_ext",
    DEPS_EXTERN .. "/openal-soft",
    "-DLIBTYPE=SHARED -DALSOFT_UTILS=OFF -DALSOFT_EXAMPLES=OFF -DALSOFT_INSTALL=OFF "
        .. "-DALSOFT_INSTALL_CONFIG=OFF -DALSOFT_INSTALL_HRTF_DATA=OFF -DALSOFT_INSTALL_AMBDEC_PRESETS=OFF "
        .. "-DALSOFT_INSTALL_EXAMPLES=OFF -DALSOFT_INSTALL_UTILS=OFF -DALSOFT_UPDATE_BUILD_VERSION=OFF",
    "libopenal.so*")

function link_openal()
    dependson { "openal_ext" }
    includedirs {
        DEPS_EXTERN .. "/openal-soft/include",
        DEPS_EXTERN .. "/openal-soft/include/AL",
    }
    filter "system:linux"
        libdirs { EXT_ROOT .. "/openal_ext/%{cfg.buildcfg}" }
        links { "openal" }
    filter "system:windows"
        libdirs { EXT_ROOT .. "/openal_ext/%{cfg.buildcfg}/%{cfg.buildcfg}" }
        links { "OpenAL32" }
    filter {}
end
