-- premake/external.lua -- SDL2 and openal-soft built by driving their own CMake.
--
-- These two libraries ship large, feature-probing CMake build systems that are
-- impractical (and brittle) to reimplement in premake. Each is wrapped in a
-- premake kind "Makefile" project whose buildcommands run the library's own
-- CMake (configure + build) into build-premake/external/<name>/<config>.
-- Consumers pull in the headers/links via link_sdl2() / link_openal().
--
-- Linux keeps the shared-library deployment flow and copies the resulting .so
-- files next to the game. Windows builds static SDL2/openal-soft libraries, so
-- no SDL2/OpenAL DLLs need to be deployed next to the executable.

EXT_ROOT = ROOT .. "/build-premake/external"
local RUNTIME_LIBS = ROOT .. "/build-premake/amnesia/%{cfg.buildcfg}/libs"
-- Kept for any future Windows shared-library externals; SDL2/openal-soft are
-- static on Windows and therefore do not use this path.
local RUNTIME_EXE  = ROOT .. "/build-premake/amnesia/%{cfg.buildcfg}"
local function winpath(p) return (p:gsub("/", "\\")) end
local SDL2_PROJECT = "SDL2"
local OPENAL_PROJECT = "OpenALSoft"

-- unix_args / win_args: platform-specific CMake configure arguments.
-- copy_glob: optional posix shared-lib glob copied next to the game (Linux, into libs/).
-- win_glob: optional Windows DLL glob copied next to the .exe.
local function cmake_makefile(name, srcdir, unix_args, win_args, copy_glob, win_glob)
    local bdir = EXT_ROOT .. "/" .. name .. "/%{cfg.buildcfg}"
    project(name)
        kind "Makefile"
        location (ROOT .. "/build-premake/projects")

        filter "system:not windows"
            buildcommands {
                string.format('"%s" -Wno-deprecated -S "%s" -B "%s" -DCMAKE_BUILD_TYPE=%%{cfg.buildcfg} %s',
                    CMAKE, srcdir, bdir, unix_args),
                string.format('"%s" --build "%s" --config %%{cfg.buildcfg} -j', CMAKE, bdir),
            }
        filter "system:windows"
            buildcommands {
                string.format('"%s" -Wno-deprecated -S "%s" -B "%s" -DCMAKE_BUILD_TYPE=%%{cfg.buildcfg} %s',
                    CMAKE, srcdir, bdir, win_args),
                string.format('"%s" --build "%s" --config %%{cfg.buildcfg} -j', CMAKE, bdir),
            }
        filter {}
        rebuildcommands {
            string.format('"%s" --build "%s" --config %%{cfg.buildcfg} -j', CMAKE, bdir),
        }
        cleancommands {
            string.format('{RMDIR} "%s"', bdir),
        }
        if copy_glob then
            filter "system:not windows"
                buildcommands {
                    string.format('mkdir -p "%s"', RUNTIME_LIBS),
                    string.format('cp -P %s "%s"/ 2>/dev/null || true', bdir .. "/" .. copy_glob, RUNTIME_LIBS),
                }
        end
        if win_glob then
            filter "system:windows"
                buildcommands {
                    string.format('if not exist "%s" mkdir "%s"', winpath(RUNTIME_EXE), winpath(RUNTIME_EXE)),
                    string.format('copy /Y "%s" "%s\\"', winpath(bdir .. "/%{cfg.buildcfg}/" .. win_glob), winpath(RUNTIME_EXE)),
                }
        end
        filter {}
end

-- ---- SDL2 -----------------------------------------------------------------
cmake_makefile(SDL2_PROJECT,
    DEPS_EXTERN .. "/SDL",
    "-DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST=OFF -DSDL2_DISABLE_INSTALL=ON -DSDL_RPATH=OFF",
    "-DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST=OFF -DSDL2_DISABLE_INSTALL=ON -DSDL_RPATH=OFF",
    "libSDL2*.so*", nil)

function link_sdl2()
    dependson { SDL2_PROJECT }
    -- ORDER MATTERS: the generated SDL_config.h (which #defines the X11/Wayland
    -- video drivers) must be found before the submodule's committed
    -- include/SDL_config.h, whose Linux fallback is SDL_config_minimal.h (no
    -- video drivers). With the wrong order the engine compiles SDL_syswm with no
    -- SDL_VIDEO_DRIVER_* defined and GetWindowHandle() hits assert(false).
    includedirs {
        -- generated SDL_config.h / SDL_revision.h from the SDL2 build tree --
        -- FIRST so they win over the submodule's committed SDL_config.h (whose
        -- Linux fallback is SDL_config_minimal.h). The generated config also matches
        -- exactly what the SDL library was built with (X11/Wayland drivers, etc.).
        EXT_ROOT .. "/" .. SDL2_PROJECT .. "/%{cfg.buildcfg}/include-config-release/SDL2",
        EXT_ROOT .. "/" .. SDL2_PROJECT .. "/%{cfg.buildcfg}/include-config-debug/SDL2",
        EXT_ROOT .. "/" .. SDL2_PROJECT .. "/%{cfg.buildcfg}/include",
        EXT_ROOT .. "/" .. SDL2_PROJECT .. "/%{cfg.buildcfg}/include/SDL2",
        -- submodule public headers (committed SDL_config.h resolves to minimal)
        DEPS_EXTERN .. "/SDL/include",
        DEPS_EXTERN .. "/SDL/include/SDL2",
    }
    filter "system:linux"
        libdirs { EXT_ROOT .. "/" .. SDL2_PROJECT .. "/%{cfg.buildcfg}" }
    filter { "system:linux", "configurations:Release" }
        links { "SDL2-2.0" }
    filter { "system:linux", "configurations:Debug" }
        links { "SDL2-2.0d" }
    filter "system:windows"
        libdirs { EXT_ROOT .. "/" .. SDL2_PROJECT .. "/%{cfg.buildcfg}/%{cfg.buildcfg}" }
    -- SDL2's CMake build applies the debug postfix 'd', so the Debug static
    -- library on disk is SDL2-staticd.lib.
    filter { "system:windows", "configurations:Debug" }
        links { "SDL2-staticd" }
    filter { "system:windows", "configurations:Release" }
        links { "SDL2-static" }
    filter {}
end

-- ---- openal-soft -----------------------------------------------------------
-- ALSOFT_ENABLE_MODULES=OFF: openal-soft auto-enables C++20 modules with the
-- "Visual Studio 17 2022" generator on MSVC 14.34+ (and Ninja on GCC 15+/Clang 17+).
-- Its sources then `import alc.context;` / `import gsl;`, which the MSBuild module
-- build fails to resolve here (C2230 could not find module 'alc.context'). The
-- non-module #include path is the supported fallback, so force it everywhere.
cmake_makefile(OPENAL_PROJECT,
    DEPS_EXTERN .. "/openal-soft",
    "-DLIBTYPE=SHARED -DALSOFT_ENABLE_MODULES=OFF -DALSOFT_UTILS=OFF -DALSOFT_EXAMPLES=OFF -DALSOFT_INSTALL=OFF "
        .. "-DALSOFT_INSTALL_CONFIG=OFF -DALSOFT_INSTALL_HRTF_DATA=OFF -DALSOFT_INSTALL_AMBDEC_PRESETS=OFF "
        .. "-DALSOFT_INSTALL_EXAMPLES=OFF -DALSOFT_INSTALL_UTILS=OFF -DALSOFT_UPDATE_BUILD_VERSION=OFF",
    "-DLIBTYPE=STATIC -DALSOFT_ENABLE_MODULES=OFF -DALSOFT_UTILS=OFF -DALSOFT_EXAMPLES=OFF -DALSOFT_INSTALL=OFF "
        .. "-DALSOFT_INSTALL_CONFIG=OFF -DALSOFT_INSTALL_HRTF_DATA=OFF -DALSOFT_INSTALL_AMBDEC_PRESETS=OFF "
        .. "-DALSOFT_INSTALL_EXAMPLES=OFF -DALSOFT_INSTALL_UTILS=OFF -DALSOFT_UPDATE_BUILD_VERSION=OFF "
        .. "-DCMAKE_CXX_FLAGS=\"/EHsc /wd4267 /wd4875\"",
    "libopenal.so*", nil)

function link_openal()
    dependson { OPENAL_PROJECT }
    includedirs {
        DEPS_EXTERN .. "/openal-soft/include",
        DEPS_EXTERN .. "/openal-soft/include/AL",
    }
    filter "system:linux"
        libdirs { EXT_ROOT .. "/" .. OPENAL_PROJECT .. "/%{cfg.buildcfg}" }
        links { "openal" }
    filter "system:windows"
        libdirs { EXT_ROOT .. "/" .. OPENAL_PROJECT .. "/%{cfg.buildcfg}/%{cfg.buildcfg}" }
        links { "OpenAL32" }
    filter {}
end
