cmake_minimum_required (VERSION 3.5)
project(Lux)
set(VERSION "1.3.1")

option(USERDIR_RESOURCES "Allow using local userdirectory resources" ON)
option(SYSTEMWIDE_RESOURCES "Allow system-wide installation of game resources" OFF)
SET(AMNESIA_TDD_VERSION "V0000" CACHE STRING "Amnesia Release Version")

if (${CMAKE_SYSTEM_NAME} MATCHES "Linux" OR ${CMAKE_SYSTEM_NAME} MATCHES "FreeBSD")
  set(LINUX ON)
endif()


if(LINUX)
    # Well, first of all, disable some harmless warnings, which are really
    # hard to get rid off (they are overall in the code).
    set(CMAKE_CXX_FLAGS
      "${CMAKE_CXX_FLAGS} -Wno-switch -Wno-undefined-var-template -Wno-extern-c-compat")
    set(CMAKE_CXX_FLAGS_RELEASE "-fno-strict-aliasing")

    set(AMNESIA_GAME_DIRECTORY "$ENV{HOME}/.local/share/Steam/steamapps/common/Amnesia The Dark Descent" CACHE PATH "game path")
else()
    set(AMNESIA_GAME_DIRECTORY "" CACHE PATH "")
endif()


set(MACOSX_BUNDLE_COPYRIGHT "Copyright © 2010-2013 Frictional Games")

if (NOT DEFINED SYSTEMWIDE_RESOURCES_LOCATION)
  set (SYSTEMWIDE_RESOURCES_LOCATION "/usr/local/share/amnesia-tdd")
endif (NOT DEFINED SYSTEMWIDE_RESOURCES_LOCATION)


if(USERDIR_RESOURCES)
    add_definitions(
        -DUSERDIR_RESOURCES
    )
endif()

set(compile_defs "")
set(demo_compile_defs "LUX_DEMO_VERSION")

option(USE_GAMEPAD "Enable gamepad support" ON)
if(USE_GAMEPAD)
	list(APPEND compile_defs USE_GAMEPAD)
	list(APPEND demo_compile_defs USE_GAMEPAD)
endif()

file(GLOB AMENISA_GAME_SOURCES RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
    src/game/*.cpp
    src/game/*.h
)

# Flat executable on every platform: set_output_dir() + the deploy target stage
# the binary next to the game assets and the engine resolves compiled_shaders/
# relative to cwd, so a macOS .app bundle is intentionally not used here.
add_executable(Amnesia WIN32 ${AMENISA_GAME_SOURCES} ${AMNESIA_SLANG_INCLUDES})
target_link_libraries(Amnesia HPL2)
set_target_properties(Amnesia PROPERTIES
  COMPILE_DEFINITIONS "${compile_defs}"
  MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Game-Info.plist"
  MACOSX_BUNDLE_ICON_FILE "Lux.icns"
  MACOSX_BUNDLE_BUNDLE_VERSION ${VERSION}
  MACOSX_BUNDLE_SHORT_VERSION_STRING ${VERSION}
  MACOSX_BUNDLE_GUI_IDENTIFIER "com.frictionalgames.Amnesia"
  MACOSX_BUNDLE_DISPLAY_NAME "Amnesia"
  MACOSX_BUNDLE_BUNDLE_NAME "Amnesia: The Dark Descent"
)

IF(APPLE)
    add_definitions(
        -DMAC_OS
    )
ELSEIF(LINUX)
    add_definitions(
        -DLINUX
    )
ENDIF()


add_custom_target(GameRelease DEPENDS Amnesia)
set(AMNESIA_EXECUTABLE_OUTPUT_PATH  ${INSTALL_OUTPUT_PATH}/Amnesia)
install(DIRECTORY DESTINATION ${AMNESIA_EXECUTABLE_OUTPUT_PATH})

# Tool binaries are emitted directly into the Amnesia runtime directory via
# set_output_dir() in AddToolTarget (HPL2/tools/CMakeLists.txt) — no
# post-build copy needed.

target_compile_definitions(Amnesia PUBLIC AMNESIA_TDD_VERSION="${AMNESIA_TDD_VERSION}")
target_compile_definitions(Amnesia PUBLIC AMNESIA_TDD_TAG="${AMNESIA_TDD_TAG}")
set_output_dir (Amnesia "") 
    

add_custom_target(deploy
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} 
    COMMAND ${CMAKE_COMMAND} 
    -DAMNESIA_GAME_DIRECTORY=${AMNESIA_GAME_DIRECTORY}
    -DAMNESIA_EXECUTABLE_OUTPUT_PATH="$<TARGET_FILE_DIR:Amnesia>"
    -P ${CMAKE_CURRENT_SOURCE_DIR}/copy_game_assets.cmake
    COMMENT "copying assets ...")



# file(GENERATE OUTPUT AMNESIA_SHADER_DIR CONTENT $<TARGET_FILE_DIR:Amnesia>/compiled_shaders)

# file(MAKE_DIRECTORY ${AMNESIA_SHADER_DIR})
target_shaders(
    Amnesia
SHADERS
    # Slang port of the Falcor SurfelGI render passes. Each file below has at
    # least one shader entry point; the headers/utility .slang files in
    # slang/ and slang/SurfelGI/ are pulled in via the INCLUDE_DIRS path
    # below rather than listed here.
    ./slang/Gui/gui.vert.slang
    ./slang/Gui/gui.frag.slang
    ./slang/WireFrame/wireframe.vert.slang
    ./slang/WireFrame/wireframe.frag.slang
    ./slang/DebugDraw/debug.vert.slang
    ./slang/DebugDraw/debug_2d.vert.slang
    ./slang/DebugDraw/debug.frag.slang
    ./slang/DebugDraw/debug_uv.vert.slang
    ./slang/DebugDraw/debug_uv.frag.slang
    ./slang/InventoryPost/inventory_post.vert.slang
    ./slang/InventoryPost/inventory_post.frag.slang
    ./slang/ParticlePass/Particle.vert.slang
    ./slang/ParticlePass/Particle.frag.slang
    ./slang/TranslucentPass/Translucent.vert.slang
    ./slang/TranslucentPass/Translucent.frag.slang
    ./slang/WaterPass/Water.vert.slang
    ./slang/WaterPass/Water.frag.slang
    ./slang/DecalPass/Decal.vert.slang
    ./slang/DecalPass/Decal.frag.slang
    ./slang/SimplePass/Simple.vert.slang
    ./slang/SimplePass/Simple.frag.slang
    ./slang/SurfelGBuffer/SurfelGBuffer.3d.slang
    ./slang/SurfelGI/SurfelPreparePass.cs.slang
    ./slang/SurfelGI/SurfelUpdatePass.cs.slang
    ./slang/SurfelGI/SurfelGenerationPass.cs.slang
    ./slang/SurfelGI/SurfelIntegratePass.cs.slang
    ./slang/SurfelGI/SurfelEvaluationPass.cs.slang
    ./slang/SurfelGI/SurfelRayTrace.rt.slang
    ./slang/SurfelGI/LightGridBuildPass.cs.slang
    ./slang/MainCompositePass/MainCompositePass.cs.slang
    ./slang/DirectLightingPass/DirectLightingPass.cs.slang
    ./slang/DirectLightingPass/DirectSpatialReusePass.cs.slang
    ./slang/DirectLightingPass/DirectAtrousPass.cs.slang
    ./slang/SurfelVBuffer/SurfelVBuffer.rt.slang
    ./slang/PostEffect/posteffect_fullscreen.vert.slang
    ./slang/PostEffect/posteffect_blit.frag.slang
    ./slang/PostEffect_ToneMap/posteffect_tonemap.frag.slang
    ./slang/PostEffect_Bloom/posteffect_bloom_blur.frag.slang
    ./slang/PostEffect_Bloom/posteffect_bloom_add.frag.slang
    ./slang/PostEffect_ColorConvTex/posteffect_color_conv.frag.slang
    ./slang/PostEffect_ImageTrail/posteffect_image_trail.frag.slang
    ./slang/PostEffect_RadialBlur/posteffect_radial_blur.frag.slang
    ./slang/PostEffect_Insanity/posteffect_insanity.frag.slang
    ./slang/Outline/outline_geom.vert.slang
    ./slang/Outline/outline_geom.frag.slang
    ./slang/Outline/outline_alpha.vert.slang
    ./slang/Outline/outline_alpha.frag.slang
    ./slang/Outline/glow_object.vert.slang
    ./slang/Outline/glow_object.frag.slang
    ./slang/Outline/outline_composite.frag.slang
INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/slang
METAL_EXCLUDE
    # The DXR ray-tracing passes use the metal::raytracing intersection-function
    # model (a real port). The SurfelGI compute passes are also hand-ported:
    # Slang's Metal output is unusable for them (illegal `export`/`uint16_t`,
    # >31 direct buffer bindings, and bare-float3 structs that don't match the
    # scalar device layout), so their hand-written .metal in METAL_SOURCES is
    # staged instead of the Slang-compiled output.
    ./slang/SurfelGI/SurfelRayTrace.rt.slang
    ./slang/SurfelVBuffer/SurfelVBuffer.rt.slang
    ./slang/SurfelGI/SurfelPreparePass.cs.slang
    ./slang/SurfelGI/LightGridBuildPass.cs.slang
    ./slang/SurfelGI/SurfelIntegratePass.cs.slang
    ./slang/SurfelGI/SurfelEvaluationPass.cs.slang
    ./slang/SurfelGI/SurfelUpdatePass.cs.slang
    ./slang/SurfelGI/SurfelGenerationPass.cs.slang
    # Full slang->metal hand-port: every remaining raster/post/compute pass is now
    # staged from its hand-written .metal in METAL_SOURCES instead of slangc's Metal
    # output. The Metal RIProgram derives binding layout from explicit C++
    # RIProgramBinding tables (mtl.index), not reflection, so the dropped reflection
    # .spv is harmless.
    ./slang/Gui/gui.vert.slang
    ./slang/Gui/gui.frag.slang
    ./slang/WireFrame/wireframe.vert.slang
    ./slang/WireFrame/wireframe.frag.slang
    ./slang/DebugDraw/debug.vert.slang
    ./slang/DebugDraw/debug_2d.vert.slang
    ./slang/DebugDraw/debug.frag.slang
    ./slang/DebugDraw/debug_uv.vert.slang
    ./slang/DebugDraw/debug_uv.frag.slang
    ./slang/InventoryPost/inventory_post.vert.slang
    ./slang/InventoryPost/inventory_post.frag.slang
    ./slang/ParticlePass/Particle.vert.slang
    ./slang/ParticlePass/Particle.frag.slang
    ./slang/TranslucentPass/Translucent.vert.slang
    ./slang/TranslucentPass/Translucent.frag.slang
    ./slang/WaterPass/Water.vert.slang
    ./slang/WaterPass/Water.frag.slang
    ./slang/DecalPass/Decal.vert.slang
    ./slang/DecalPass/Decal.frag.slang
    ./slang/SimplePass/Simple.vert.slang
    ./slang/SimplePass/Simple.frag.slang
    ./slang/SurfelGBuffer/SurfelGBuffer.3d.slang
    ./slang/MainCompositePass/MainCompositePass.cs.slang
    ./slang/DirectLightingPass/DirectLightingPass.cs.slang
    ./slang/DirectLightingPass/DirectSpatialReusePass.cs.slang
    ./slang/DirectLightingPass/DirectAtrousPass.cs.slang
    ./slang/PostEffect/posteffect_fullscreen.vert.slang
    ./slang/PostEffect/posteffect_blit.frag.slang
    ./slang/PostEffect_ToneMap/posteffect_tonemap.frag.slang
    ./slang/PostEffect_Bloom/posteffect_bloom_blur.frag.slang
    ./slang/PostEffect_Bloom/posteffect_bloom_add.frag.slang
    ./slang/PostEffect_ColorConvTex/posteffect_color_conv.frag.slang
    ./slang/PostEffect_ImageTrail/posteffect_image_trail.frag.slang
    ./slang/PostEffect_RadialBlur/posteffect_radial_blur.frag.slang
    ./slang/PostEffect_Insanity/posteffect_insanity.frag.slang
    ./slang/Outline/outline_geom.vert.slang
    ./slang/Outline/outline_geom.frag.slang
    ./slang/Outline/outline_alpha.vert.slang
    ./slang/Outline/outline_alpha.frag.slang
    ./slang/Outline/glow_object.vert.slang
    ./slang/Outline/glow_object.frag.slang
    ./slang/Outline/outline_composite.frag.slang
METAL_SOURCES
    # Hand-written Metal ports (under amnesia/metal/, mirroring the slang/ tree).
    # Staged verbatim into compiled_shaders/ on a Metal build.
    ./metal/SurfelGI/SurfelRayTrace.rt.metal
    ./metal/SurfelVBuffer/SurfelVBuffer.rt.metal
    ./metal/SurfelGI/SurfelPreparePass.cs.metal
    ./metal/SurfelGI/LightGridBuildPass.cs.metal
    ./metal/SurfelGI/SurfelIntegratePass.cs.metal
    ./metal/SurfelGI/SurfelEvaluationPass.cs.metal
    ./metal/SurfelGI/SurfelUpdatePass.cs.metal
    ./metal/SurfelGI/SurfelGenerationPass.cs.metal
    # Full slang->metal hand-port (mirrors the METAL_EXCLUDE additions above).
    # Note: ./metal/Outline/enemy_glow.frag.metal is intentionally NOT staged — it
    # has no .slang twin in SHADERS and no program loads it.
    ./metal/Gui/gui.vert.metal
    ./metal/Gui/gui.frag.metal
    ./metal/WireFrame/wireframe.vert.metal
    ./metal/WireFrame/wireframe.frag.metal
    ./metal/DebugDraw/debug.vert.metal
    ./metal/DebugDraw/debug_2d.vert.metal
    ./metal/DebugDraw/debug.frag.metal
    ./metal/DebugDraw/debug_uv.vert.metal
    ./metal/DebugDraw/debug_uv.frag.metal
    ./metal/InventoryPost/inventory_post.vert.metal
    ./metal/InventoryPost/inventory_post.frag.metal
    ./metal/ParticlePass/Particle.vert.metal
    ./metal/ParticlePass/Particle.frag.metal
    ./metal/TranslucentPass/Translucent.vert.metal
    ./metal/TranslucentPass/Translucent.frag.metal
    ./metal/WaterPass/Water.vert.metal
    ./metal/WaterPass/Water.frag.metal
    ./metal/DecalPass/Decal.vert.metal
    ./metal/DecalPass/Decal.frag.metal
    ./metal/SimplePass/Simple.vert.metal
    ./metal/SimplePass/Simple.frag.metal
    ./metal/SurfelGBuffer/SurfelGBuffer.3d.metal
    ./metal/MainCompositePass/MainCompositePass.cs.metal
    ./metal/DirectLightingPass/DirectLightingPass.cs.metal
    ./metal/DirectLightingPass/DirectSpatialReusePass.cs.metal
    ./metal/DirectLightingPass/DirectAtrousPass.cs.metal
    ./metal/PostEffect/posteffect_fullscreen.vert.metal
    ./metal/PostEffect/posteffect_blit.frag.metal
    ./metal/PostEffect_ToneMap/posteffect_tonemap.frag.metal
    ./metal/PostEffect_Bloom/posteffect_bloom_blur.frag.metal
    ./metal/PostEffect_Bloom/posteffect_bloom_add.frag.metal
    ./metal/PostEffect_ColorConvTex/posteffect_color_conv.frag.metal
    ./metal/PostEffect_ImageTrail/posteffect_image_trail.frag.metal
    ./metal/PostEffect_RadialBlur/posteffect_radial_blur.frag.metal
    ./metal/PostEffect_Insanity/posteffect_insanity.frag.metal
    ./metal/Outline/outline_geom.vert.metal
    ./metal/Outline/outline_geom.frag.metal
    ./metal/Outline/outline_alpha.vert.metal
    ./metal/Outline/outline_alpha.frag.metal
    ./metal/Outline/glow_object.vert.metal
    ./metal/Outline/glow_object.frag.metal
    ./metal/Outline/outline_composite.frag.metal
OUTPUT_FOLDER
${AMNESIA_SHADER_DIR}
)

#add_custom_target(shaders
#    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} 
#    COMMAND ${CMAKE_COMMAND} 
#    -DAMNESIA_GAME_DIRECTORY=${AMNESIA_GAME_DIRECTORY}
#    -DAMNESIA_EXECUTABLE_OUTPUT_PATH="$<TARGET_FILE_DIR:Amnesia>"
#    -P ${CMAKE_CURRENT_SOURCE_DIR}/build_shaders.cmake
#    COMMENT "building shaders ...")

