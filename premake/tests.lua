-- Keep the jitter/motion-vector and reactive-mask math contracts covered by
-- headless tests. The math source links nothing, so it stays independent of
-- engine/GPU deps.
-- The tests/graphics/*.cpp glob is intentional: another TU with main() would
-- clash; put new tests in this TU or give them their own project.

-- cmd.exe can parse slash-separated paths as switches, and spaces in an
-- unquoted checkout path split the post-build command. %{cfg.buildtarget.abspath}
-- expands with forward slashes even for vs2022, and path.translate applied to the
-- token expands too late to help, so the Windows branch spells the separators
-- statically. It therefore has to mirror targetdir below -- keep the two in sync.
local function add_test_postbuild()
    filter "system:windows"
        postbuildcommands {
            '"' .. path.translate("tests/%{cfg.buildcfg}/%{prj.name}.exe", "\\") .. '"'
        }
    filter "system:not windows"
        postbuildcommands { '"%{cfg.buildtarget.abspath}"' }
    filter {}
end

-- unittest discovery does not recurse into directories without __init__.py.
-- Keep the search under tests/ and use *_test.py so the argparse-only
-- tests/fsr/check_shader_regeneration.py and
-- tests/fsr/check_patch_restaging.py are not treated as test modules.
-- The runner preserves these rules and maps an empty suite to success so it
-- does not fail the build; real test failures remain fatal.
local function add_python_test_postbuild()
    filter "system:windows"
        postbuildcommands {
            'cd /d "' .. path.translate(ROOT, "\\") .. '" && "'
                .. path.translate(_OPTIONS["python"] or "python", "\\")
                .. '" "'
                .. path.translate(ROOT .. "/scripts/run_python_tests.py", "\\")
                .. '"'
        }
    filter "system:not windows"
        postbuildcommands {
            'cd "' .. ROOT .. '" && "'
                .. (_OPTIONS["python"] or "python3")
                .. '" "' .. ROOT .. '/scripts/run_python_tests.py"'
        }
    filter {}
end

project "TemporalCameraTests"
    kind "ConsoleApp"
    language "C++"
    objdir (BUILD_OUT .. "/obj/%{prj.name}/%{cfg.buildcfg}")
    targetdir (BUILD_OUT .. "/tests/%{cfg.buildcfg}")
    files {
        ROOT .. "/tests/graphics/*.cpp",
        ROOT .. "/HPL2/core/sources/graphics/TemporalCamera.cpp",
        ROOT .. "/HPL2/core/sources/graphics/TemporalUpscalerPolicy.cpp",
        ROOT .. "/HPL2/core/sources/graphics/TemporalReactiveMaskMath.cpp",
        ROOT .. "/HPL2/core/sources/graphics/WaterReflectionJitterMath.cpp",
        ROOT .. "/HPL2/core/sources/graphics/FsrUpscalerParams.cpp",
        ROOT .. "/HPL2/core/sources/graphics/RayConeLod.cpp",
        ROOT .. "/HPL2/core/sources/graphics/CubeMipGen.cpp",
        ROOT .. "/HPL2/core/sources/graphics/BlockCompressionDecode.cpp",
        ROOT .. "/HPL2/core/sources/graphics/RIFormat.c",
    }
    includedirs { ROOT .. "/HPL2/core/include" }
    add_test_postbuild()
    -- gmake2 drops postbuildcommands on kind "Utility" projects, so the python
    -- suite rides on the first test project instead of getting its own.
    if _OPTIONS["with-python-tests"] ~= "no" then
        add_python_test_postbuild()
    end

project "FsrUpscalerParamsTests"
    kind "ConsoleApp"
    language "C++"
    objdir (BUILD_OUT .. "/obj/%{prj.name}/%{cfg.buildcfg}")
    targetdir (BUILD_OUT .. "/tests/%{cfg.buildcfg}")
    files {
        ROOT .. "/tests/graphics/fsr/*.cpp",
        ROOT .. "/HPL2/core/sources/graphics/FsrUpscalerParams.cpp",
    }
    includedirs { ROOT .. "/HPL2/core/include" }
    add_test_postbuild()

-- The bindless slot pools are pure CPU data structures (IndexPool + ObjectPool),
-- so they test without Vulkan or the engine. Own directory because the
-- tests/graphics/*.cpp glob above already owns a main().
project "BindlessPoolTests"
    kind "ConsoleApp"
    language "C++"
    cppdialect "C++17"
    objdir (BUILD_OUT .. "/obj/%{prj.name}/%{cfg.buildcfg}")
    targetdir (BUILD_OUT .. "/tests/%{cfg.buildcfg}")
    files {
        ROOT .. "/tests/graphics/bindless/*.cpp",
        ROOT .. "/HPL2/core/sources/graphics/BindlessPool.cpp",
        ROOT .. "/HPL2/core/sources/graphics/IndexPool.cpp",
    }
    includedirs { ROOT .. "/HPL2/core/include" }
    add_test_postbuild()

-- Keep this one-main-per-project rule for the non-recursive tests/resources/*.cpp
-- glob; this project owns the FileSearcher test executable.
project "FileSearcherTests"
    kind "ConsoleApp"
    language "C++"
    cppdialect "C++17"
    objdir (BUILD_OUT .. "/obj/%{prj.name}/%{cfg.buildcfg}")
    targetdir (BUILD_OUT .. "/tests/%{cfg.buildcfg}")
    files {
        ROOT .. "/tests/resources/*.cpp",
        ROOT .. "/HPL2/core/sources/resources/FileSearcher.cpp",
        ROOT .. "/HPL2/core/sources/system/String.cpp",
        -- String.cpp contains cColor-returning helpers; this keeps the
        -- focused link self-contained without bringing in the engine.
        ROOT .. "/HPL2/core/sources/graphics/Color.cpp",
    }
    includedirs { ROOT .. "/HPL2/core/include" }
    defines { "USE_SDL2" }
    link_sdl2()
    add_test_postbuild()

-- Same one-main-per-project rule as above applies to the tests/fsr/*.cpp glob.
-- The project only exists when FSR is enabled, so --with-fsr=no emits no
-- reference to the SDK includes or the archive.
if _OPTIONS["with-fsr"] ~= "no" then
    project "FsrShaderBlobTests"
        kind "ConsoleApp"
        language "C++"
        cppdialect "C++17"
        objdir (BUILD_OUT .. "/obj/%{prj.name}/%{cfg.buildcfg}")
        targetdir (BUILD_OUT .. "/tests/%{cfg.buildcfg}")
        files {
            ROOT .. "/tests/fsr/*.cpp",
            ROOT .. "/HPL2/core/sources/graphics/spirv_reflect.c",
        }
        includedirs {
            ROOT .. "/HPL2/core/include",
            ROOT .. "/HPL2/core/include/graphics",
        }
        fsr_shader_blob_test_use()
        link_fsr()
        add_test_postbuild()

    project "FsrVulkanLoaderTests"
        kind "ConsoleApp"
        language "C++"
        cppdialect "C++17"
        objdir (BUILD_OUT .. "/obj/%{prj.name}/%{cfg.buildcfg}")
        targetdir (BUILD_OUT .. "/tests/%{cfg.buildcfg}")
        files {
            -- Keep this one-main test explicit; the existing non-recursive globs must not pick it up.
            ROOT .. "/tests/fsr/vulkan_loader/fsr_vulkan_loader_test.cpp",
        }
        vulkan_includes()
        link_fsr()
        links { "volk" }
        filter "system:linux"
            links { "dl", "pthread" }
        filter {}
        add_test_postbuild()
end
