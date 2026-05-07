cmake_minimum_required(VERSION 3.15...3.31)
project(ogg C)

# Snapshot is libogg ~1.3.x; sources live in this dir, headers in
# HPL2/dependencies/include/ogg/. Source list mirrors ogg.vcxproj.filters.
add_library(ogg STATIC bitwise.c framing.c)
target_include_directories(ogg PUBLIC ${CMAKE_SOURCE_DIR}/HPL2/dependencies/include)
set_target_properties(ogg PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(NOT TARGET Ogg::ogg)
    add_library(Ogg::ogg ALIAS ogg)
endif()

if(MSVC)
    target_compile_definitions(ogg PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(ogg PRIVATE /wd4267 /wd4244)
endif()
