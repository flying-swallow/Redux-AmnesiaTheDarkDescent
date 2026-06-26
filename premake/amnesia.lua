-- Amnesia game executable -- mirrors amnesia/CMakeLists.txt.
local AMN = ROOT .. "/amnesia"

-- Short git hash for AMNESIA_TDD_TAG (mirrors root CMakeLists `git log -1 --format=%h`).
local git_tag = os.outputof('git -C "' .. ROOT .. '" log -1 --format=%h') or "unknown"
git_tag = git_tag:gsub("%s+", "")

project "Amnesia"
    language "C++"
    set_output("runtime")
    filter "system:windows"
        kind "WindowedApp"
    filter "system:not windows"
        kind "ConsoleApp"
    filter {}

    files { AMN .. "/src/game/*.cpp", AMN .. "/src/game/*.h" }

    includedirs {
        ROOT .. "/HPL2/core/include",
        AMN .. "/slang",
        AMN .. "/glsl",
        DEPS_SOURCES .. "/AngelScript/include",
        DEPS_EXTERN .. "/tinyxml2",
    }
    deps_public_includes()   -- ogg/vorbis/IL/Newton/OALWrapper public headers
    vulkan_includes()
    mathlib_use()

    defines {
        "USERDIR_RESOURCES",
        "USE_GAMEPAD",
        'AMNESIA_TDD_VERSION="V0000"',
        'AMNESIA_TDD_TAG="' .. git_tag .. '"',
    }

    link_engine()      -- HPL2 + full dependency set (no transitive propagation in premake)

    -- Compile shaders next to the executable (reuses scripts/compile_slang_shaders.py).
    slang_prebuild()

    -- RPATH so the colocated SDL2/OpenAL shared libs in ./libs are found.
    filter "system:linux"
        defines { "LINUX" }
        buildoptions { "-Wno-switch", "-Wno-undefined-var-template", "-Wno-extern-c-compat" }
        linkoptions { "-Wl,-rpath,'$$ORIGIN/libs'", "-Wl,-rpath,'$$ORIGIN'" }
        linkgroups "On"
    filter { "system:linux", "configurations:Release" }
        buildoptions { "-fno-strict-aliasing" }
    filter {}

-- ---- deploy action --------------------------------------------------------
-- Mirrors the CMake `deploy` target: copy the installed game assets next to the
-- built executable. Run with:  premake5 deploy --game-dir="/path/to/Amnesia TDD"
newaction {
    trigger = "deploy",
    description = "Copy Amnesia game assets next to the built executable",
    execute = function()
        local gamedir = _OPTIONS["game-dir"]
        if not gamedir then
            print("error: --game-dir=<path to Amnesia: The Dark Descent> is required")
            return
        end
        for _, cfg in ipairs({ "Debug", "Release" }) do
            local dest = ROOT .. "/build-premake/amnesia/" .. cfg
            if os.isdir(dest) then
                print("Deploying assets to " .. dest)
                -- exclude the binaries/archives the CMake copy_game_assets.cmake skips
                os.execute(string.format(
                    'cd "%s" && find . -type f ! -name "Amnesia*" ! -name "*.rar" ! -name "*.pdf" '
                    .. '! -name "*.dll" ! -name "*.exe" -exec cp --parents {} "%s"/ \\;',
                    gamedir, dest))
            end
        end
    end
}
