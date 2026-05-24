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

add_executable(Amnesia WIN32 MACOSX_BUNDLE ${AMENISA_GAME_SOURCES} ${AMNESIA_SLANG_INCLUDES})
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

#if(WITH_TOOLS)
#    add_custom_command (TARGET Amnesia POST_BUILD
#        COMMAND ${CMAKE_COMMAND} -E copy_if_different
#        $<TARGET_FILE:MapViewer> $<TARGET_FILE_DIR:Amnesia>
#        DEPENDS MapViewer
#    )
#    add_custom_command (TARGET Amnesia POST_BUILD
#        COMMAND ${CMAKE_COMMAND} -E copy_if_different
#        $<TARGET_FILE:ModelViewer> $<TARGET_FILE_DIR:Amnesia>
#        DEPENDS ModelViewer
#    )
#    add_custom_command (TARGET Amnesia POST_BUILD
#        COMMAND ${CMAKE_COMMAND} -E copy_if_different
#        $<TARGET_FILE:MshConverter> $<TARGET_FILE_DIR:Amnesia>
#        DEPENDS MshConverter
#    )
#    add_custom_command (TARGET Amnesia POST_BUILD
#        COMMAND ${CMAKE_COMMAND} -E copy_if_different
#        $<TARGET_FILE:LevelEditor> $<TARGET_FILE_DIR:Amnesia>
#        DEPENDS LevelEditor
#    )
#    add_custom_command (TARGET Amnesia POST_BUILD
#        COMMAND ${CMAKE_COMMAND} -E copy_if_different
#        $<TARGET_FILE:ParticleEditor> $<TARGET_FILE_DIR:Amnesia>
#        DEPENDS ParticleEditor
#    )
#    add_custom_command (TARGET Amnesia POST_BUILD
#        COMMAND ${CMAKE_COMMAND} -E copy_if_different
#        $<TARGET_FILE:ModelEditor> $<TARGET_FILE_DIR:Amnesia>
#        DEPENDS ModelEditor
#    )
#    add_custom_command ( TARGET Amnesia POST_BUILD
#        COMMAND ${CMAKE_COMMAND} ARGS -E copy_directory
#        ${_HPL_TOOL_RESOURCE_PATH_} $<TARGET_FILE_DIR:Amnesia>
#    )
#endif()

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
    ./slang/InventoryPost/inventory_post.vert.slang
    ./slang/InventoryPost/inventory_post.frag.slang
    ./slang/ParticlePass/Particle.vert.slang
    ./slang/ParticlePass/Particle.frag.slang
    ./slang/SurfelGBuffer/SurfelGBuffer.3d.slang
    ./slang/SurfelGI/SurfelPreparePass.cs.slang
    ./slang/SurfelGI/SurfelClearByInstance.cs.slang
    ./slang/SurfelGI/SurfelUpdatePass.cs.slang
    ./slang/SurfelGI/SurfelGenerationPass.cs.slang
    ./slang/SurfelGI/SurfelIntegratePass.cs.slang
    ./slang/SurfelGI/SurfelEvaluationPass.cs.slang
    ./slang/SurfelGI/SurfelRayTrace.rt.slang
    ./slang/SurfelGIRenderPass/SurfelGIRenderPass.frag.slang
    ./slang/SurfelVBuffer/SurfelVBuffer.rt.slang
    ./slang/PostEffect/posteffect_fullscreen.vert.slang
    ./slang/PostEffect/posteffect_blit.frag.slang
    ./slang/PostEffect_Bloom/posteffect_bloom_blur.frag.slang
    ./slang/PostEffect_Bloom/posteffect_bloom_add.frag.slang
    ./slang/PostEffect_ColorConvTex/posteffect_color_conv.frag.slang
    ./slang/PostEffect_ImageTrail/posteffect_image_trail.frag.slang
    ./slang/PostEffect_RadialBlur/posteffect_radial_blur.frag.slang
INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/slang
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

