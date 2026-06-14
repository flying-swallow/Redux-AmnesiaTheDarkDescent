cmake_minimum_required (VERSION 3.5)
project(Tools)

# VERSION is only set in amnesia/CMakeLists.txt (directory-scoped), so the
# bundle properties in AddToolTarget need a local fallback.
if(NOT DEFINED VERSION)
    set(VERSION "1.3.1")
endif()

find_file(TOOLINFO_PLIST Tool-Info.plist
    PATHS ${CMAKE_CURRENT_SOURCE_DIR}
    NO_DEFAULT_PATH
)
mark_as_advanced(TOOLINFO_PLIST)

function(AddToolTarget target)
    # Flat executable (no macOS .app bundle): staged next to the Amnesia binary
    # via set_output_dir() below, matching the game's flat deploy model.
    add_executable(${target}
        ${ARGN}
    )
    target_link_libraries(${target} HPL2)

    # Publish next to the Amnesia binary — the engine resolves
    # compiled_shaders/ and resource dirs relative to cwd, so the tools
    # belong in the same runtime directory as the game.
    set_output_dir(${target} "")

    set_target_properties(${target} PROPERTIES
        MACOSX_BUNDLE_INFO_PLIST "${TOOLINFO_PLIST}"
        MACOSX_BUNDLE_BUNDLE_VERSION ${VERSION}
        MACOSX_BUNDLE_SHORT_VERSION_STRING ${VERSION}
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.frictionalgames.HPL2.${target}"
        MACOSX_BUNDLE_DISPLAY_NAME "${target}"
        MACOSX_BUNDLE_BUNDLE_NAME "HPL2: ${target}"
    )

    # No post-build bundle copy: tools are staged into the Amnesia runtime
    # directory via set_output_dir() above, matching how amnesia/CMakeLists.txt
    # deploys the game binary (legacy PostBuildMacBundle relied on the removed
    # target LOCATION property and is not used by this build).

endfunction()

if(USERDIR_RESOURCES)
    add_definitions(
        -DUSERDIR_RESOURCES
    )
endif()

add_subdirectory(editors)

file(GLOB common_sources RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
    ../tests/Common/*.*
)

### MapViewer
# Disabled: legacy viewer never ported off the removed OpenGL APIs
# (cRenderList / CreateFrameBuffer / SetVsyncActive). Use the LevelEditor instead.

#AddToolTarget(MapViewer
#    mapview/MapView.cpp
#    ${common_sources}
#)

### ModelViewer
# Disabled: legacy viewer not ported to the RI backend. Use the ModelEditor instead.

#AddToolTarget(ModelViewer
#    modelview/ModelView.cpp
#    ${common_sources}
#)

### ParticleViewer

#AddToolTarget(ParticleViewer
#    particleview/ParticleView.cpp
#    ${common_sources}
#)

##  Mesh Converter

add_executable(MshConverter
    mshconverter/MshConverter.cpp
)
target_link_libraries(MshConverter HPL2)
set_output_dir(MshConverter "")

