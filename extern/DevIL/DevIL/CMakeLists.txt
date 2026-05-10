cmake_minimum_required(VERSION 2.6)

project(ImageLib)
# include our custom modules
set (CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Modules")

option(IL_BUILD_ILUT "Build the ILUT helper library (requires GLUT)" ON)
add_subdirectory(src-IL)
add_subdirectory(src-ILU)
if(IL_BUILD_ILUT)
    add_subdirectory(src-ILUT)
endif()
option(IL_TESTS "build DevIL tests" ON)
if (IL_TESTS)
    ENABLE_TESTING()
    Add_Subdirectory(test)
endif()

