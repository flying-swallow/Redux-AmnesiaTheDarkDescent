# DevIL (image library) — adapted from upstream DevIL/DevIL/src-IL/CMakeLists.txt
# for the flattened in-tree layout used by HPL2:
#
#   HPL2/dependencies/sources/DevIL/
#     ├── CMakeLists.txt             <-- this file
#     ├── src/                       <-- .cpp sources (was src-IL/src)
#     ├── include/                   <-- internal il_*.h headers (was src-IL/include)
#     └── DevIL/
#         └── src-IL/include/config.h <-- the configured (non-template) config.h
#
# Public IL/* headers live alongside the other vendored deps at
# HPL2/dependencies/include/IL/, not under sources/.
#
# The parent (HPL2/dependencies/sources/CMakeLists.txt) wires png/jpeg via the
# IL_*_LIB and {PNG,JPEG}_{INCLUDE_DIRS,LIBRARIES} cache vars and disables
# tiff/jasper via IL_NO_TIF/IL_NO_JP2 in config.h, so we only consume what's
# already been set up there — no find_package calls needed.

cmake_minimum_required(VERSION 3.5)
project(DevIL)

include(TestBigEndian)
test_big_endian(WORDS_BIGENDIAN)

set(DEVIL_SRC_DIR  "${CMAKE_CURRENT_SOURCE_DIR}/src")
set(DEVIL_INC_DIR  "${CMAKE_CURRENT_SOURCE_DIR}/include")
# DevIL/src-IL/include holds the *configured* config.h (with IL_NO_TIF=1 etc).
# This must come before DEVIL_INC_DIR on the include path so the configured
# header wins over the template-style copy that lives in DEVIL_INC_DIR.
set(DEVIL_CFG_DIR  "${CMAKE_CURRENT_SOURCE_DIR}/DevIL/src-IL/include")
# Public IL/il.h, IL/devil_internal_exports.h, etc. live alongside DevIL's
# internal headers under sources/DevIL/include/IL.
set(DEVIL_PUBLIC_INC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include")

file(GLOB DevIL_SRCS "${DEVIL_SRC_DIR}/*.cpp")
file(GLOB DevIL_INC
    "${DEVIL_INC_DIR}/*.h"
    "${DEVIL_CFG_DIR}/config.h"
    "${DEVIL_PUBLIC_INC_DIR}/IL/devil_internal_exports.h"
    "${DEVIL_PUBLIC_INC_DIR}/IL/il.h"
)

source_group("Source Files" FILES ${DevIL_SRCS})
source_group("Header Files" FILES ${DevIL_INC})

if(WIN32)
    if(BUILD_SHARED_LIBS)
        add_definitions(-DIL_DLL -DJPEGSTATIC)
    else()
        add_definitions(-DIL_STATIC_LIB -DJPEGSTATIC)
    endif()
endif()

if(BUILD_SHARED_LIBS)
    add_library(IL SHARED ${DevIL_SRCS} ${DevIL_INC})
    set_target_properties(IL PROPERTIES SOVERSION 1)
else()
    add_library(IL STATIC ${DevIL_SRCS} ${DevIL_INC})
endif()

if(MSVC)
    set_target_properties(IL PROPERTIES OUTPUT_NAME "DevIL")
endif()

# il_jpeg.cpp passes IL_TRUE/IL_FALSE (ints) to libjpeg APIs that take its
# `boolean` typedef. GCC/Clang reject the implicit narrowing in C++ mode;
# upstream DevIL relies on -fpermissive here. Apply only to the IL target.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(IL PRIVATE -fpermissive)
endif()

target_include_directories(IL
    PRIVATE
        ${DEVIL_CFG_DIR}        # configured config.h must resolve first
        ${DEVIL_INC_DIR}        # internal il_*.h
    PUBLIC
        ${DEVIL_PUBLIC_INC_DIR} # IL/il.h for downstream consumers
)

# Optional codecs the parent has wired up (IL_PNG_LIB / IL_JPEG_LIB ON).
# Tiff/Jasper/EXR/MNG are disabled in config.h, so they're not linked here.
set(_il_libs "")
set(_il_incs "")

if(IL_PNG_LIB)
    list(APPEND _il_incs ${PNG_INCLUDE_DIRS})
    list(APPEND _il_libs ${PNG_LIBRARIES})
endif()

if(IL_JPEG_LIB)
    list(APPEND _il_incs ${JPEG_INCLUDE_DIRS})
    list(APPEND _il_libs ${JPEG_LIBRARIES})
endif()

if(_il_incs)
    target_include_directories(IL PRIVATE ${_il_incs})
endif()
if(_il_libs)
    target_link_libraries(IL PRIVATE ${_il_libs})
endif()
