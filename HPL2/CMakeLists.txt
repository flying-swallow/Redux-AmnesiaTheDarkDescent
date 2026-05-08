cmake_minimum_required (VERSION 3.5)
project(HPL2)

set(CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
set(HPL2_VERSION_MAJOR 2)
set(HPL2_VERSION_MINOR 0)

add_definitions(
  -DUSE_OALWRAPPER
  -DUSE_SDL2
)

# common shared source
file(GLOB common_sources RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
    core/sources/ai/*
    core/sources/engine/*
    core/sources/generate/*
    core/sources/graphics/*
    core/sources/gui/*
    core/sources/haptic/*
    core/sources/input/*
    core/sources/math/*
    core/sources/physics/*
    core/sources/resources/*
    core/sources/scene/*
    core/source/windowing/*
    core/sources/sound/*
    core/sources/system/*
    core/source/platform/sdl2/*

    core/include/ai/*
    core/include/engine/*
    core/include/generate/*
    core/include/graphics/*
    core/include/gui/*
    core/include/haptic/*
    core/include/input/*
    core/include/math/*
    core/include/physics/*
    core/include/resources/*
    core/include/scene/*
    core/include/sound/*
    core/include/system/*
    core/include/*.*
)
list(REMOVE_DUPLICATES common_sources)

#setup various IMPL sources
file(GLOB impl_sources RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
    core/include/impl/tinyXML/*
    core/include/impl/*.*

    # tinyXML
    core/sources/impl/tinyXml/*
    core/sources/impl/XmlDocumentTiny.cpp
    # scripting
    core/sources/impl/SqScript.cpp
    core/sources/impl/scriptarray.cpp
    core/sources/impl/scripthelper.cpp
    core/sources/impl/scriptstring.cpp
    core/sources/impl/scriptstring_utils.cpp
    # Image loading
    core/sources/impl/BitmapLoader*
    # Newton
    core/sources/impl/*Newton.cpp
    core/sources/impl/LegacyVertexBuffer.cpp
    # SDL
    core/sources/impl/GamepadSDL.cpp
    core/sources/impl/GamepadSDL2.cpp
    core/sources/impl/KeyboardSDL.cpp
    core/sources/impl/MouseSDL.cpp
    core/sources/impl/TimerSDL.cpp
    core/sources/impl/LowLevelGraphicsSDL.cpp
    core/sources/impl/LowLevelInputSDL.cpp
    core/sources/impl/LowLevelResourcesSDL.cpp
    core/sources/impl/LowLevelSystemSDL.cpp
    core/sources/impl/SDLEngineSetup.cpp
    core/sources/impl/SDLFontData.cpp
    core/sources/impl/SDLTexture.cpp

    core/sources/platform/sdl2/*.cpp
    # OpenAL
    core/sources/impl/LowLevelSoundOpenAL.cpp
    core/sources/impl/OpenAL*
    # mesh loader
    core/sources/impl/MeshLoaderCollada.cpp
    core/sources/impl/MeshLoaderColladaHelpers.cpp
    core/sources/impl/MeshLoaderColladaLoader.cpp
    core/sources/impl/MeshLoaderMSH.cpp
    core/sources/impl/MeshLoaderFBX.cpp

    core/sources/impl/ThreadSDL.cpp
    core/sources/impl/MutexSDL.cpp
    core/sources/impl/GLSLProgram.cpp
    core/sources/impl/GLSLShader.cpp
    core/sources/impl/OcclusionQueryOGL.cpp
    core/sources/impl/FrameBufferGL.cpp
    core/sources/impl/VertexBuffer_RI.cpp
    core/sources/impl/VertexBufferOGL_VBO.cpp
    core/sources/impl/VertexBufferOpenGL.cpp
    core/sources/impl/VertexBufferOGL_Array.cpp
)

IF(APPLE)
    LIST(APPEND impl_sources
        core/sources/impl/PlatformMacOSX.mm
        core/sources/impl/PlatformUnix.cpp
        core/sources/impl/PlatformSDL.cpp
    )
    IF(NOT USE_SDL2)
        LIST(APPEND impl_sources
            core/SDLMain.m
            core/SDLMain.h
        )
    ENDIF()
ELSEIF(WIN32)
    LIST(APPEND impl_sources
        core/sources/impl/PlatformWin32.cpp
        core/sources/impl/MutexWin32.cpp
        core/sources/impl/ThreadWin32.cpp
    )
ELSEIF(LINUX)
    LIST(APPEND impl_sources
        core/sources/impl/PlatformUnix.cpp
        core/sources/impl/PlatformSDL.cpp
    )
ENDIF()

# Setup buildID source

IF(APPLE)
    SET(BUILDID_SUFFIX MacOSX)
ELSEIF(LINUX)
    SET(BUILDID_SUFFIX Linux)
ELSEIF(WIN32)
    SET(BUILDID_SUFFIX Win32)
ELSE()
    message(FATAL_ERROR "Unknown platform")
ENDIF()
SET(BUILDID_SUFFIX ${BUILDID_SUFFIX} CACHE STRING "Build ID Suffix")
find_file(BUILDID_BIN buildcounter.pl
    PATHS ${CMAKE_CURRENT_SOURCE_DIR}
    NO_DEFAULT_PATH
)
mark_as_advanced(BUILDID_SUFFIX BUILDID_BIN)

function(GenerateBuildID outvar idname includedir sourcedir)
    add_custom_command(
        OUTPUT
            ${sourcedir}/BuildID_${idname}_${BUILDID_SUFFIX}.cpp
            ${includedir}/BuildID_${idname}.h
        COMMAND
            perl ${BUILDID_BIN}
        ARGS
            ${idname} ${includedir} ${sourcedir}
    )
    set(${outvar} ${sourcedir}/BuildID_${idname}_${BUILDID_SUFFIX}.cpp
            ${includedir}/BuildID_${idname}.h PARENT_SCOPE)
endfunction()

GenerateBuildID(version_source
    HPL2_0
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/sources
)

#configure_file(
#  ${CMAKE_CURRENT_SOURCE_DIR}/include/ResourcesLocation.h.in
#  ${CMAKE_CURRENT_SOURCE_DIR}/include/ResourcesLocation.h)

# setup source groups
source_group("AI" REGULAR_EXPRESSION "\\/ai\\/.+")
source_group("Engine" REGULAR_EXPRESSION "\\/engine\\/.+")
source_group("Generate" REGULAR_EXPRESSION "\\/generate\\/.+")
source_group("Graphics" REGULAR_EXPRESSION "\\/graphics\\/.+")
source_group("GUI" REGULAR_EXPRESSION "\\/gui\\/.+")
source_group("Input" REGULAR_EXPRESSION "\\/input\\/.+")
source_group("Math" REGULAR_EXPRESSION "\\/math\\/.+")
source_group("Physics" REGULAR_EXPRESSION "\\/physics\\/.+")
source_group("Resources" REGULAR_EXPRESSION "\\/resources\\/.+")
source_group("Scene" REGULAR_EXPRESSION "\\/scene\\/.+")
source_group("Sound" REGULAR_EXPRESSION "\\/sound\\/.+")
source_group("System" REGULAR_EXPRESSION "\\/system\\/.+")

source_group("Implementation Specific" REGULAR_EXPRESSION "\\/impl\\/.+")
source_group("TinyXML" REGULAR_EXPRESSION "\\/impl\\/tiny.+")
source_group("SDL Specific" REGULAR_EXPRESSION "\\/impl\\/.*SDL.+")
source_group("OpenGL Specific" REGULAR_EXPRESSION "\\/impl\\/.*GL.+")
source_group("HaptX Specific" REGULAR_EXPRESSION "\\/impl\\/.+HaptX.+")
source_group("Newton Specific" REGULAR_EXPRESSION "\\/impl\\/.+Newton.+")
source_group("Mesh Loaders" REGULAR_EXPRESSION "\\/impl\\/Mesh.+")
source_group("OpenAL Specific" REGULAR_EXPRESSION "\\/impl\\/.*OpenAL.+")
source_group("Win32" REGULAR_EXPRESSION "\\/impl\\/.+Win32.+")
source_group("Platform" REGULAR_EXPRESSION "\\/impl\\/Platform.+")


# setup libs

set(OpenGL_GL_PREFERENCE LEGACY)
find_package(OpenGL REQUIRED)
if(USE_SYSTEM_GLEW)
    find_package(GLEW REQUIRED)
endif()

# setup HPL2 compile target

add_library(HPL2 STATIC
    ${common_sources}
    ${version_sources}
    ${impl_sources}
)

set(HPL2_INCLUDES
    PUBLIC core/include
    #Important that this is before dependencies so the correct OpenAL headers are used
    PRIVATE ${OALWRAPPER_INCLUDE_DIR}
    PRIVATE ${IL_INCLUDE_DIR}
)

set(RENDERDOC_PATH "/usr/local" CACHE PATH "Path to RenderDoc.")
set(RENDERDOC_ENABLED OFF CACHE BOOL "Enable RenderDoc integration. Use RENDERDOC_PATH to specific the path to RenderDoc.")

if(RENDERDOC_ENABLED)
    add_definitions(-DHPL2_RENDERDOC_ENABLED)
    set(HPL2_INCLUDES
        ${HPL2_INCLUDES}
        PRIVATE ${RENDERDOC_PATH}
    )
endif()


target_include_directories(HPL2
    ${HPL2_INCLUDES}
)

# Cheat to avoid resolving the SDL2 under dependencies/include/SDL2 instead of the one from find_package. The SDL2::SDL2 target's
target_include_directories(HPL2 BEFORE PRIVATE
    "$<TARGET_PROPERTY:SDL2::SDL2,INTERFACE_INCLUDE_DIRECTORIES>"
)

if(LINUX)
  set(PLATFORM_LIBS
    pthread
    dl
    )
endif()

# Don't use exceptions
if(WIN32)
  add_compile_options(/EHs-c-)
  target_compile_definitions(HPL2 PRIVATE WIN32_LEAN_AND_MEAN SDL_MAIN_HANDLED IL_STATIC_LIB)
else()
  add_compile_options(-fno-exceptions)
endif()

target_link_libraries(HPL2
  OALWrapper # also pulls in ogg,vorbis,SDL
  AngelScript
  Newton
  tinyxml2::tinyxml2
  SDL2::SDL2
  OpenGL::GL
  GLEW::GLEW
  OpenGL::GLU
  MathLib
  zlib
  volk
  GPUOpen::VulkanMemoryAllocator
  IL
  ${PLATFORM_LIBS}
  )

# Route HPL2 + every direct link dependency that is a real local target into
# build/amnesia/<config>/libs so runtime DLLs (SDL2d.dll, zlibd.dll, ...) end up
# alongside Amnesia.exe. Mirrors warfork's qf_set_output_dir_dep pattern.
set_output_dir_dep(HPL2 "")
  

