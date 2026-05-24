
option(BUILD_EXTERNAL OFF)
option(ENABLE_HLSL OFF)
option(ENABLE_OPT OFF)
option(ENABLE_PCH OFF)
option(ENABLE_CTEST OFF)

# OALWrapper references ${SDL2_INCLUDE_DIR} directly. The bundled SDL2 build
# doesn't export this var (only the SDL2::SDL2 target), and openal-soft's
# optional find_package(SDL2) caches it as NOTFOUND. Set it explicitly to the
# bundled SDL headers before any consumer reads it.
if(NOT USE_SYSTEM_SDL2)
  set(SDL2_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/SDL/include" CACHE PATH "" FORCE)
endif()

add_subdirectory(OALWrapper)
add_subdirectory(Newton)
add_subdirectory(AngelScript)
add_subdirectory(glslang)
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

if(USE_SYSTEM_ZLIB)
  find_package(ZLIB REQUIRED GLOBAL)
  add_library(zlib ALIAS ZLIB::ZLIB)
  target_link_libraries(ZLIB::ZLIB INTERFACE ZLIB::ZLIB)
else()
  option(ZLIB_BUILD_EXAMPLES "" OFF)
  add_subdirectory(zlib)
  set_output_dir(zlib "")
endif()

if (USE_SYSTEM_SDL2)
  find_package(SDL2 REQUIRED GLOBAL)
else()
  option(SDL_RPATH "" OFF)
  option(SDL_SHARED "" ON)
  option(SDL_STATIC "" OFF)
  option(SDL_SHARED_ENABLED_BY_DEFAULT"" OFF)
  option(SDL_TEST "" OFF)
  option(SDL2_DISABLE_INSTALL "" ON)
  
  add_subdirectory(SDL)
  set_output_dir (SDL2 "")
  set(SDL2_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/SDL/include" CACHE PATH "" FORCE)
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

if(USE_SYSTEM_DEVIL)
  find_package(DevIL REQUIRED GLOBAL)
else()
  # libjpeg / libpng are vendored under HPL2/dependencies/sources for the VS
  # build; reuse the same sources from CMake so DevIL can be built with
  # JPEG/PNG support on platforms that don't ship system codecs (notably
  # Windows). Without these, DevIL's find_package(JPEG/PNG) fails and
  # IL_NO_JPG / IL_NO_PNG get baked into config.h, causing JPG/PNG asset
  # loads to silently return false.
  add_subdirectory(
    ${CMAKE_SOURCE_DIR}/HPL2/dependencies/sources/jpeg
    ${CMAKE_BINARY_DIR}/HPL2/dependencies/jpeg
  )
  set_target_properties(jpeg PROPERTIES FOLDER extern)
  add_subdirectory(
    ${CMAKE_SOURCE_DIR}/HPL2/dependencies/sources/png
    ${CMAKE_BINARY_DIR}/HPL2/dependencies/png
  )
  set_target_properties(png PROPERTIES FOLDER extern)

  # Steer DevIL's optional-codec discovery: IL_*_LIB=ON skips its
  # find_package() calls; the {JPEG,PNG}_INCLUDE_DIRS / *_LIBRARIES vars are
  # then consumed directly by DevIL/src-IL/CMakeLists.txt.
  set(IL_JPEG_LIB ON CACHE BOOL "" FORCE)
  set(JPEG_INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/HPL2/dependencies/sources/jpeg" CACHE PATH "" FORCE)
  set(JPEG_LIBRARIES jpeg CACHE STRING "" FORCE)
  set(IL_PNG_LIB ON CACHE BOOL "" FORCE)
  set(PNG_INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/HPL2/dependencies/sources/png" CACHE PATH "" FORCE)
  set(PNG_LIBRARIES png CACHE STRING "" FORCE)
  # FindTIFF and FindJasper transitively call find_package(JPEG); without these
  # IL_*_LIB skips, that secondary search overwrites JPEG_LIBRARY in the cache
  # with NOTFOUND and breaks the IL link. Short-circuit the codecs we don't
  # bundle so DevIL relies entirely on the libs we wired above.
  set(IL_TIFF_LIB ON CACHE BOOL "" FORCE)
  set(IL_NO_TIF 1 CACHE STRING "" FORCE)
  set(IL_JASPER_LIB ON CACHE BOOL "" FORCE)
  set(IL_NO_JP2 1 CACHE STRING "" FORCE)

  # ILUT is the GL/GLUT/D3D helper layer; HPL2 only uses IL/ILU. Skip it so
  # CI doesn't need freeglut on Windows or libglut3-dev on Linux.
  set(IL_BUILD_ILUT OFF CACHE BOOL "" FORCE)

  option(IL_TESTS "" OFF)
  option(BUILD_SHARED_LIBS "" OFF)
  add_subdirectory(DevIL/DevIL)
  # Public IL/*.h headers live next to the other vendored deps' includes,
  # not inside the DevIL source tree.
  set(IL_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/DevIL/DevIL/include" CACHE PATH "" FORCE)
endif()

if(USE_SYSTEM_GLEW)
  find_package(GLEW REQUIRED GLOBAL)
else()
  add_subdirectory(glew)
endif()

add_subdirectory(MathLib)

# MathLib uses SSE3+ intrinsics that GCC/Clang gate behind -msse* flags (MSVC
# enables them implicitly on x64).
if(NOT MSVC AND NOT (CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|ARM64|arm"))
    target_compile_options(MathLib INTERFACE -msse4.2)
endif()

