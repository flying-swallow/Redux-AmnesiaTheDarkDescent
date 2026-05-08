set(GLEW_SOURCE_ROOT "${CMAKE_SOURCE_DIR}/HPL2/dependencies")

add_library(glew STATIC
    "${GLEW_SOURCE_ROOT}/sources/GLEW/glew.c"
)

target_include_directories(glew PUBLIC
    "${GLEW_SOURCE_ROOT}/include"
)

target_compile_definitions(glew PUBLIC GLEW_STATIC)

find_package(OpenGL REQUIRED)
target_link_libraries(glew PUBLIC OpenGL::GL)

if(NOT TARGET GLEW::GLEW)
    add_library(GLEW::GLEW ALIAS glew)
endif()

set_target_properties(glew PROPERTIES FOLDER extern)
