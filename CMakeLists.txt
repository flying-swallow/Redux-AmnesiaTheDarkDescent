cmake_minimum_required(VERSION 3.18...3.31)
cmake_policy(SET CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
message(STATUS "CMAKE_SYSTEM: " ${CMAKE_SYSTEM})
message(STATUS "CMAKE_SYSTEM_NAME: " ${CMAKE_SYSTEM_NAME})
message(STATUS "CMAKE_SYSTEM_VERSION: " ${CMAKE_SYSTEM_VERSION})
message(STATUS "CMAKE_SYSTEM_PROCESSOR: " ${CMAKE_SYSTEM_PROCESSOR})
set(CMAKE_CXX_STANDARD 20)
set(INSTALL_OUTPUT_PATH ${CMAKE_CURRENT_BINARY_DIR}/bin)
set(CMAKE_INSTALL_PREFIX ${INSTALL_OUTPUT_PATH} CACHE PATH "Install path prefix, prepended onto install directories." FORCE)

set(CMAKE_MODULE_PATH ${CMAKE_HOME_DIRECTORY}/cmake)
include(utils)
include(slang)
include(shaders)

set(CMAKE_SKIP_BUILD_RPATH FALSE)
 
# when building, don't use the install RPATH already
# (but later on when installing)
set(CMAKE_BUILD_WITH_INSTALL_RPATH FALSE)
set(CMAKE_INSTALL_RPATH $ORIGIN/libs)
set(CMAKE_BUILD_RPATH $ORIGIN/libs:$ORIGIN)

enable_language(ASM)

set(AMNESIA_GAME_DIRECTORY "Amnesia game assets" CACHE PATH "")
option(USE_GRAPHICS_X11 "Build using X11" ON)
option(USE_GRAPHICS_WAYLAND "Build using wayland" ON)

OPTION(USE_SYSTEM_ZLIB "Prefer system ZLIB instead of the bundled one" OFF)
OPTION(USE_SYSTEM_OPENAL "Prefer system OpenAL Soft instead of the bundled one" OFF)
OPTION(USE_SYSTEM_TINYXML "Prefer system TinyXMl2 instead of the bundled one" OFF)
OPTION(USE_SYSTEM_SDL2 "Prefer system SDL2 instead of the bundled one" OFF)
OPTION(USE_SYSTEM_OGG "Prefer system OGG instead of the bundled one" OFF)
OPTION(USE_SYSTEM_VORBIS "Prefer system vorbis instead of the bundled one" OFF)
OPTION(USE_SYSTEM_DEVIL "Prefer system devil instead of the bundled one" OFF)

if(MSVC)
    add_compile_options(/MP)
    add_compile_definitions("NOMINMAX=1" "ML_INTRINSIC_LEVEL=1")
endif()

# hack to get to build on windwos
# HACK to correctly set Linux property for newer cmake
if(NOT APPLE AND UNIX)
    set(LINUX TRUE)
ENDIF()

execute_process(
        COMMAND git log -1 --format=%h
        WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
        OUTPUT_VARIABLE AMNESIA_TDD_TAG
        OUTPUT_STRIP_TRAILING_WHITESPACE
        )

# option(WITH_TOOLS "build HPL2 with tools" ON)

add_subdirectory(extern)
add_subdirectory(HPL2)
add_subdirectory(amnesia)

