cmake_minimum_required(VERSION 3.15...3.31)
project(glew C)

# Snapshot at HPL2/dependencies/sources/GLEW. Headers live in the
# central include mirror at HPL2/dependencies/include/GL/.
add_library(glew STATIC glew.c)
target_include_directories(glew PUBLIC ${CMAKE_SOURCE_DIR}/HPL2/dependencies/include)
target_compile_definitions(glew PUBLIC GLEW_STATIC)
set_target_properties(glew PROPERTIES POSITION_INDEPENDENT_CODE ON)

find_package(OpenGL REQUIRED)
target_link_libraries(glew PUBLIC OpenGL::GL)

if(NOT TARGET GLEW::GLEW)
    add_library(GLEW::GLEW ALIAS glew)
endif()

if(MSVC)
    target_compile_definitions(glew PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
