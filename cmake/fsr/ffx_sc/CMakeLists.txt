cmake_minimum_required(VERSION 3.17)

project(FidelityFX_SC LANGUAGES CXX)

set(FSR_SDK_ROOT "" CACHE PATH "FidelityFX-SDK root directory (the directory containing sdk/)")
if(NOT FSR_SDK_ROOT)
    message(FATAL_ERROR "FSR_SDK_ROOT is not set. Point it at the FidelityFX-SDK-1.1.4 directory containing sdk/.")
endif()

get_filename_component(FSR_SDK_ROOT "${FSR_SDK_ROOT}" ABSOLUTE)
set(_FFX_SC_SDK_TOOL_ROOT "${FSR_SDK_ROOT}/sdk/tools/ffx_shader_compiler")
set(_FFX_SC_SDK_SRC_ROOT "${_FFX_SC_SDK_TOOL_ROOT}/src")
set(_FFX_SC_SDK_LIB_ROOT "${_FFX_SC_SDK_TOOL_ROOT}/libs")

foreach(_required_path
        "${FSR_SDK_ROOT}/sdk"
        "${_FFX_SC_SDK_SRC_ROOT}/ffx_sc.cpp"
        "${_FFX_SC_SDK_SRC_ROOT}/glsl_compiler.cpp"
        "${_FFX_SC_SDK_SRC_ROOT}/compiler.h"
        "${_FFX_SC_SDK_LIB_ROOT}/SPIRV-Reflect/spirv_reflect.c"
        "${_FFX_SC_SDK_LIB_ROOT}/SPIRV-Reflect/include/spirv/unified1/spirv.h")
    if(NOT EXISTS "${_required_path}")
        message(FATAL_ERROR "FSR_SDK_ROOT does not look like FidelityFX-SDK-1.1.4: missing ${_required_path}")
    endif()
endforeach()

# Do not edit the SDK. The source files are copied to a private build-tree
# staging area and patched there so an SDK update cannot modify the checkout.
set(_FFX_SC_REAL_STAGE_DIR "${CMAKE_CURRENT_BINARY_DIR}/ffx_sc_staged")
set(_FFX_SC_TEMP_STAGE_DIR "${CMAKE_CURRENT_BINARY_DIR}/ffx_sc_staged.tmp")
file(REMOVE_RECURSE "${_FFX_SC_TEMP_STAGE_DIR}")
file(MAKE_DIRECTORY "${_FFX_SC_TEMP_STAGE_DIR}/src")
file(MAKE_DIRECTORY "${_FFX_SC_TEMP_STAGE_DIR}/libs/MD5")
file(MAKE_DIRECTORY "${_FFX_SC_TEMP_STAGE_DIR}/libs/SPIRV-Reflect")
file(MAKE_DIRECTORY "${_FFX_SC_TEMP_STAGE_DIR}/libs/tiny-process-library")

# Patch a disposable copy. The final configure_file(COPYONLY) pass below
# publishes only changed files, preserving the staged source mtimes across a
# no-op reconfigure while still making a real patch change visible to the
# compiler.
set(_FFX_SC_STAGE_DIR "${_FFX_SC_TEMP_STAGE_DIR}")

function(ffx_sc_copy_writable source destination)
    configure_file("${source}" "${destination}" COPYONLY)
    # configure_file(COPYONLY) preserves the SDK's read-only mode, while the
    # patcher needs to rewrite these disposable copies.
    file(CHMOD "${destination}"
        PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ)
endfunction()

foreach(_source_file
        ffx_sc.cpp
        glsl_compiler.cpp
        glsl_compiler.h
        compiler.h
        utils.cpp
        utils.h
        pch.cpp
        pch.hpp)
    ffx_sc_copy_writable("${_FFX_SC_SDK_SRC_ROOT}/${_source_file}"
                         "${_FFX_SC_STAGE_DIR}/src/${_source_file}")
endforeach()

foreach(_source_file md5.cpp md5.h md5_loc.h)
    ffx_sc_copy_writable("${_FFX_SC_SDK_LIB_ROOT}/MD5/${_source_file}"
                         "${_FFX_SC_STAGE_DIR}/libs/MD5/${_source_file}")
endforeach()

foreach(_source_file spirv_reflect.c spirv_reflect.h)
    ffx_sc_copy_writable("${_FFX_SC_SDK_LIB_ROOT}/SPIRV-Reflect/${_source_file}"
                         "${_FFX_SC_STAGE_DIR}/libs/SPIRV-Reflect/${_source_file}")
endforeach()
file(MAKE_DIRECTORY "${_FFX_SC_STAGE_DIR}/libs/SPIRV-Reflect/include/spirv/unified1")
ffx_sc_copy_writable(
    "${_FFX_SC_SDK_LIB_ROOT}/SPIRV-Reflect/include/spirv/unified1/spirv.h"
    "${_FFX_SC_STAGE_DIR}/libs/SPIRV-Reflect/include/spirv/unified1/spirv.h")

foreach(_source_file process.cpp process.hpp process_unix.cpp process_win.cpp)
    ffx_sc_copy_writable("${_FFX_SC_SDK_LIB_ROOT}/tiny-process-library/${_source_file}"
                         "${_FFX_SC_STAGE_DIR}/libs/tiny-process-library/${_source_file}")
endforeach()

include("${CMAKE_CURRENT_LIST_DIR}/../patches/portable_glsl.cmake")

set(_FFX_SC_STAGE_DIR "${_FFX_SC_REAL_STAGE_DIR}")
file(MAKE_DIRECTORY "${_FFX_SC_STAGE_DIR}/src")
file(MAKE_DIRECTORY "${_FFX_SC_STAGE_DIR}/libs/MD5")
file(MAKE_DIRECTORY "${_FFX_SC_STAGE_DIR}/libs/SPIRV-Reflect")
file(MAKE_DIRECTORY "${_FFX_SC_STAGE_DIR}/libs/SPIRV-Reflect/include/spirv/unified1")
file(MAKE_DIRECTORY "${_FFX_SC_STAGE_DIR}/libs/tiny-process-library")

foreach(_source_file
        ffx_sc.cpp
        glsl_compiler.cpp
        glsl_compiler.h
        compiler.h
        utils.cpp
        utils.h
        pch.cpp
        pch.hpp)
    configure_file("${_FFX_SC_TEMP_STAGE_DIR}/src/${_source_file}"
                   "${_FFX_SC_STAGE_DIR}/src/${_source_file}" COPYONLY)
endforeach()

foreach(_source_file md5.cpp md5.h md5_loc.h)
    configure_file("${_FFX_SC_TEMP_STAGE_DIR}/libs/MD5/${_source_file}"
                   "${_FFX_SC_STAGE_DIR}/libs/MD5/${_source_file}" COPYONLY)
endforeach()

foreach(_source_file spirv_reflect.c spirv_reflect.h)
    configure_file("${_FFX_SC_TEMP_STAGE_DIR}/libs/SPIRV-Reflect/${_source_file}"
                   "${_FFX_SC_STAGE_DIR}/libs/SPIRV-Reflect/${_source_file}" COPYONLY)
endforeach()
configure_file(
    "${_FFX_SC_TEMP_STAGE_DIR}/libs/SPIRV-Reflect/include/spirv/unified1/spirv.h"
    "${_FFX_SC_STAGE_DIR}/libs/SPIRV-Reflect/include/spirv/unified1/spirv.h" COPYONLY)

foreach(_source_file process.cpp process.hpp process_unix.cpp process_win.cpp)
    configure_file("${_FFX_SC_TEMP_STAGE_DIR}/libs/tiny-process-library/${_source_file}"
                   "${_FFX_SC_STAGE_DIR}/libs/tiny-process-library/${_source_file}" COPYONLY)
endforeach()

set(_FFX_SC_REFLECT_SOURCE "${_FFX_SC_STAGE_DIR}/libs/SPIRV-Reflect/spirv_reflect.c")
set_source_files_properties("${_FFX_SC_REFLECT_SOURCE}" PROPERTIES LANGUAGE CXX)
if(WIN32)
    set(_FFX_SC_PROCESS_PLATFORM_SOURCE "${_FFX_SC_STAGE_DIR}/libs/tiny-process-library/process_win.cpp")
else()
    set(_FFX_SC_PROCESS_PLATFORM_SOURCE "${_FFX_SC_STAGE_DIR}/libs/tiny-process-library/process_unix.cpp")
endif()

add_executable(FidelityFX_SC
    "${_FFX_SC_STAGE_DIR}/src/ffx_sc.cpp"
    "${_FFX_SC_STAGE_DIR}/src/glsl_compiler.cpp"
    "${_FFX_SC_STAGE_DIR}/src/glsl_compiler.h"
    "${_FFX_SC_STAGE_DIR}/src/compiler.h"
    "${_FFX_SC_STAGE_DIR}/src/utils.cpp"
    "${_FFX_SC_STAGE_DIR}/src/utils.h"
    "${_FFX_SC_STAGE_DIR}/src/pch.cpp"
    "${_FFX_SC_STAGE_DIR}/src/pch.hpp"
    "${_FFX_SC_STAGE_DIR}/libs/MD5/md5.cpp"
    "${_FFX_SC_STAGE_DIR}/libs/MD5/md5.h"
    "${_FFX_SC_STAGE_DIR}/libs/MD5/md5_loc.h"
    "${_FFX_SC_REFLECT_SOURCE}"
    "${_FFX_SC_STAGE_DIR}/libs/SPIRV-Reflect/spirv_reflect.h"
    "${_FFX_SC_STAGE_DIR}/libs/tiny-process-library/process.cpp"
    "${_FFX_SC_STAGE_DIR}/libs/tiny-process-library/process.hpp"
    "${_FFX_SC_PROCESS_PLATFORM_SOURCE}")

target_compile_features(FidelityFX_SC PRIVATE cxx_std_17)
set_target_properties(FidelityFX_SC PROPERTIES CXX_EXTENSIONS OFF)
target_include_directories(FidelityFX_SC PRIVATE
    "${_FFX_SC_STAGE_DIR}/src"
    "${_FFX_SC_STAGE_DIR}/libs/MD5"
    "${_FFX_SC_STAGE_DIR}/libs/SPIRV-Reflect"
    "${_FFX_SC_STAGE_DIR}/libs/tiny-process-library")

if(MSVC)
    target_compile_options(FidelityFX_SC PRIVATE /W3)
    target_compile_definitions(FidelityFX_SC PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(FidelityFX_SC PRIVATE -Wall -Wextra)
endif()

find_package(Threads REQUIRED)
target_link_libraries(FidelityFX_SC PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
