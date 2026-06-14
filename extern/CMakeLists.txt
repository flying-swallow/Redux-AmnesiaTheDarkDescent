
# OALWrapper references ${SDL3_INCLUDE_DIR} directly. The bundled SDL build
# doesn't export this var (only the SDL3::SDL3 target). Set it explicitly to the
# bundled SDL headers before any consumer reads it.
if(NOT USE_SYSTEM_SDL3)
  set(SDL3_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/SDL/include" CACHE PATH "" FORCE)
endif()

add_subdirectory(OALWrapper)
add_subdirectory(Newton)
add_subdirectory(AngelScript)
add_subdirectory(VulkanMemoryAllocator)

set(VOLK_STATIC_DEFINES "")
if(CMAKE_SYSTEM_NAME STREQUAL Linux)
    if (USE_GRAPHICS_X11)
        find_path (XLIB_HEADERS NAMES "X11/Xlib.h")
        if (NOT XLIB_HEADERS)
            message (FATAL_ERROR "Can't find 'X11/xlib.h'.(libx11-dev is not installed?)")
        endif ()
        list(APPEND VOLK_STATIC_DEFINES  VK_USE_PLATFORM_XLIB_KHR)
    endif ()
    if (USE_GRAPHICS_WAYLAND)
        find_path (WAYLAND_HEADERS NAMES "wayland-client.h")
        if (NOT WAYLAND_HEADERS)
            message (FATAL_ERROR "Can't find 'wayland-client.h'.(libwayland-dev is not installed?)")
        endif ()
        list(APPEND VOLK_STATIC_DEFINES VK_USE_PLATFORM_WAYLAND_KHR)
    endif ()
elseif (${CMAKE_SYSTEM_NAME} MATCHES "Windows")
  list(APPEND VOLK_STATIC_DEFINES VK_USE_PLATFORM_WIN32_KHR)
endif()

set(VULKAN_HEADERS_INSTALL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/Vulkan-Headers" )
option(VOLK_INSTALL "" OFF)
add_subdirectory(volk)

# metal-cpp: Apple's header-only C++ bindings for Metal/Foundation/QuartzCore.
# Header-only submodule (no CMakeLists.txt of its own), macOS-only. Exposed as an
# INTERFACE target so consumers get the include path + the Apple frameworks.
if(APPLE)
    add_library(metal-cpp INTERFACE)
    target_include_directories(metal-cpp INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/metal-cpp)
    target_compile_features(metal-cpp INTERFACE cxx_std_20)

    find_library(METAL_FRAMEWORK Metal REQUIRED)
    find_library(FOUNDATION_FRAMEWORK Foundation REQUIRED)
    find_library(QUARTZCORE_FRAMEWORK QuartzCore REQUIRED)
    target_link_libraries(metal-cpp INTERFACE
        ${METAL_FRAMEWORK}
        ${FOUNDATION_FRAMEWORK}
        ${QUARTZCORE_FRAMEWORK})
endif()

if(USE_SYSTEM_ZLIB)
  find_package(ZLIB REQUIRED GLOBAL)
  add_library(zlib ALIAS ZLIB::ZLIB)
  target_link_libraries(ZLIB::ZLIB INTERFACE ZLIB::ZLIB)
else()
  option(ZLIB_BUILD_EXAMPLES "" OFF)
  add_subdirectory(zlib)
  set_output_dir(zlib "")
endif()

if (USE_SYSTEM_SDL3)
  find_package(SDL3 REQUIRED GLOBAL)
else()
  option(SDL_RPATH "" OFF)
  option(SDL_SHARED "" ON)
  option(SDL_STATIC "" OFF)
  option(SDL_SHARED_ENABLED_BY_DEFAULT"" OFF)
  option(SDL_TEST "" OFF)
  option(SDL_DISABLE_INSTALL "" ON)

  add_subdirectory(SDL)
  set_output_dir (SDL3-shared "")
  set(SDL3_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/SDL/include" CACHE PATH "" FORCE)
endif()

if(USE_SYSTEM_OGG)
  find_package(Ogg REQUIRED GLOBAL)
else()
  add_subdirectory(ogg)
  set_target_properties(ogg PROPERTIES FOLDER extern)
endif()

if(USE_SYSTEM_VORBIS)
  find_package(Vorbis REQUIRED GLOBAL)
else()
  if(NOT USE_SYSTEM_OGG)
    set(OGG_LIBRARY ogg)
    set(OGG_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/ogg/include)
  endif()
  add_subdirectory(vorbis)
endif()

if (USE_SYSTEM_TINYXML)
  find_package(tinyxml2 CONFIG REQUIRED GLOBAL)
else()
  option(BUILD_TESTING "" OFF)
  option(tinyxml2_INSTALL_PKGCONFIG "" OFF)
  add_subdirectory(tinyxml2)
endif()

if (USE_SYSTEM_OPENAL)
  find_package(OpenAL REQUIRED GLOBAL)
else()
  set(LIBTYPE STATIC)
  set(FORCE_STATIC_VCRT OFF CACHE BOOL "openal-soft must not rewrite /MD->/MT" FORCE)
  option(ALSOFT_UTILS "" OFF)
  option(ALSOFT_EXAMPLES "" OFF)
  option(ALSOFT_UPDATE_BUILD_VERSION "" OFF)
  option(ALSOFT_INSTALL "" OFF)
  option(ALSOFT_INSTALL_CONFIG "" OFF)
  option(ALSOFT_INSTALL_HRTF_DATA "" OFF)
  option(ALSOFT_INSTALL_AMBDEC_PRESETS "" OFF)
  option(ALSOFT_INSTALL_EXAMPLES "" OFF)
  option(ALSOFT_INSTALL_UTILS "" OFF)
  add_subdirectory(openal-soft)
  set_target_properties(OpenAL PROPERTIES FOLDER extern/openal-soft)
  set_target_properties(alsoft.common PROPERTIES FOLDER extern/openal-soft)
  set_target_properties(alsoft.excommon PROPERTIES FOLDER extern/openal-soft)
  target_compile_definitions(OpenAL PUBLIC AL_LIBTYPE_STATIC)
endif()

# Image codecs (replaces DevIL): libpng + libjpeg-turbo decode PNG/JPEG, the
# header-only tinyddsloader parses DDS, and an in-tree decoder handles TGA (see
# HPL2/core/sources/impl/BitmapLoader{DDS,Image}.cpp). Drops DevIL/ILU and the
# vendored HPL2/dependencies/sources/{jpeg,png} entirely.

# libpng (static). find_package(ZLIB) is unconditional in libpng; it resolves
# the system zlib on macOS/Linux. (Windows: point ZLIB_ROOT at extern/zlib.)
set(PNG_SHARED OFF CACHE BOOL "" FORCE)
set(PNG_STATIC ON CACHE BOOL "" FORCE)
set(PNG_FRAMEWORK OFF CACHE BOOL "" FORCE)
set(PNG_TESTS OFF CACHE BOOL "" FORCE)
set(PNG_TOOLS OFF CACHE BOOL "" FORCE)
set(SKIP_INSTALL_ALL ON CACHE BOOL "" FORCE)
add_subdirectory(libpng)
if(TARGET png_static)
  set_target_properties(png_static PROPERTIES FOLDER extern)
endif()

# libjpeg-turbo forbids add_subdirectory() integration (hard error in its
# CMakeLists), so build it as an ExternalProject (its own CMake) and consume the
# installed static lib + headers through an IMPORTED target. The real consumer
# must depend on `libjpeg_turbo_ep` so the headers/lib exist before it builds
# (see HPL2/CMakeLists.txt).
include(ExternalProject)
set(_jt_install "${CMAKE_CURRENT_BINARY_DIR}/libjpeg-turbo-ep/install")
set(_jt_lib "${_jt_install}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}jpeg${CMAKE_STATIC_LIBRARY_SUFFIX}")
ExternalProject_Add(libjpeg_turbo_ep
  SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/libjpeg-turbo"
  PREFIX "${CMAKE_CURRENT_BINARY_DIR}/libjpeg-turbo-ep"
  CMAKE_ARGS
    -DCMAKE_INSTALL_PREFIX=${_jt_install}
    -DCMAKE_INSTALL_LIBDIR=lib
    -DCMAKE_BUILD_TYPE=$<CONFIG>
    -DENABLE_SHARED=OFF
    -DENABLE_STATIC=ON
    -DWITH_TURBOJPEG=OFF
    -DREQUIRE_SIMD=OFF
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5
  BUILD_BYPRODUCTS "${_jt_lib}"
)
file(MAKE_DIRECTORY "${_jt_install}/include")
add_library(jpeg-static STATIC IMPORTED GLOBAL)
set_target_properties(jpeg-static PROPERTIES
  IMPORTED_LOCATION "${_jt_lib}"
  INTERFACE_INCLUDE_DIRECTORIES "${_jt_install}/include")
set(JPEG_INCLUDE_DIRS "${_jt_install}/include" CACHE STRING "" FORCE)
# png.h lives in the source dir; pnglibconf.h is generated into the build dir.
set(PNG_INCLUDE_DIRS
    "${CMAKE_CURRENT_SOURCE_DIR}/libpng"
    "${CMAKE_CURRENT_BINARY_DIR}/libpng" CACHE STRING "" FORCE)

# tinyddsloader: header-only DDS parser exposed as an INTERFACE target.
add_library(tinyddsloader INTERFACE)
target_include_directories(tinyddsloader
    INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/tinyddsloader)

add_subdirectory(MathLib)

# MathLib uses SSE3+ intrinsics that GCC/Clang gate behind -msse* flags (MSVC
# enables them implicitly on x64).
if(NOT MSVC AND NOT (CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|ARM64|arm"))
    target_compile_options(MathLib INTERFACE -msse4.2)
endif()

