cmake_minimum_required(VERSION 3.15...3.31)
project(png C)

# Mirrors the source list in png.vcxproj.filters (libpng 1.6.37). pnglibconf.h
# is checked in alongside the sources so we don't need to run libpng's awk-based
# config generator.
set(PNG_SRCS
    png.c pngerror.c pngget.c pngmem.c pngpread.c pngread.c pngrio.c
    pngrtran.c pngrutil.c pngset.c pngtrans.c pngwio.c pngwrite.c
    pngwtran.c pngwutil.c
)

add_library(png STATIC ${PNG_SRCS})
target_include_directories(png PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(png PRIVATE zlib)
# zlib's directory-level include_directories don't propagate via the target;
# wire the sources/build dirs explicitly so png can find zlib.h / zconf.h.
target_include_directories(png PRIVATE
    ${CMAKE_SOURCE_DIR}/HPL2/dependencies/sources/zlib
    ${CMAKE_BINARY_DIR}/HPL2/dependencies/sources/zlib
)
set_target_properties(png PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(MSVC)
    target_compile_definitions(png PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(png PRIVATE /wd4267 /wd4244 /wd4018 /wd4996)
endif()
