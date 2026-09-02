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

    -- Per-shader Slang -> SPIR-V build rules next to the executable (native port of
    -- cmake/shaders.cmake; incremental, no python).
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
--
-- Implemented as a portable Lua walk (os.matchfiles + os.copyfile) so it works
-- on both Linux and Windows; the previous `find ... cp --parents` pipeline was
-- Unix-only.
newaction {
    trigger = "deploy",
    description = "Copy Amnesia game assets next to the built executable",
    execute = function()
        local gamedir = _OPTIONS["game-dir"]
        if not gamedir then
            print("error: --game-dir=<path to Amnesia: The Dark Descent> is required")
            return
        end
        -- Same exclusion set as the CMake copy_game_assets.cmake / the old shell
        -- pipeline: skip the prebuilt game binaries, DLLs/EXEs and bulky archives.
        local function excluded(name)
            return name:match("^Amnesia")
                or name:match("%.rar$")
                or name:match("%.pdf$")
                or name:match("%.dll$")
                or name:match("%.exe$")
        end
        -- Mirror one source tree into dest, skipping names the filter rejects.
        local function mirror(srcroot, dest, reject)
            for _, src in ipairs(os.matchfiles(srcroot .. "/**")) do
                local rel = path.getrelative(srcroot, src)
                if not (reject and reject(path.getname(rel))) then
                    local target = dest .. "/" .. rel
                    os.mkdir(path.getdirectory(target))
                    os.copyfile(src, target)
                end
            end
        end

        local gd = path.getabsolute(gamedir)
        -- In-tree editor/viewer runtime data (HPL2/tools/resources -> editor/,
        -- viewer/). Copied AFTER the game assets so our copy wins over whatever
        -- an HPL2-tools-era install happens to carry. The editor targets also
        -- drop this in as a postbuild (premake/tools.lua), so deploying is not
        -- required to get it -- this keeps `premake5 deploy` self-sufficient for
        -- anyone populating a build dir without rebuilding the editors.
        local res = ROOT .. "/HPL2/tools/resources"
        for _, cfg in ipairs({ "Debug", "Release" }) do
            local dest = ROOT .. "/build-premake/amnesia/" .. cfg
            if os.isdir(dest) then
                print("Deploying assets to " .. dest)
                mirror(gd, dest, excluded)
                if os.isdir(res) then
                    print("Deploying editor resources to " .. dest)
                    mirror(res, dest, nil)
                end
            end
        end
    end
}
