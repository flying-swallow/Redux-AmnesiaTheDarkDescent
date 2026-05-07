cmake_minimum_required(VERSION 3.15...3.31)
project(zlib C)

# Mirrors the source list in zlib.vcxproj.filters (zlib 1.2.11).
set(ZLIB_SRCS
    adler32.c compress.c crc32.c deflate.c gzclose.c gzlib.c gzread.c
    gzwrite.c infback.c inffast.c inflate.c inftrees.c trees.c uncompr.c
    zutil.c
)

add_library(zlib STATIC ${ZLIB_SRCS})
target_include_directories(zlib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(zlib PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(MSVC)
    target_compile_definitions(zlib PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(zlib PRIVATE /wd4267 /wd4244 /wd4127 /wd4996)
endif()
