# Unified third-party orchestrator. Replaces extern/CMakeLists.txt.
# Both the CMake build and (eventually) the Visual Studio solution consume
# everything from HPL2/dependencies/sources/.

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

# --- Core deps: ordering matters (ogg before vorbis, zlib before png/DevIL) ---

if (USE_SYSTEM_SDL2)
  find_package(SDL2 REQUIRED GLOBAL)
else()
  option(SDL_RPATH "" OFF)
  option(SDL_SHARED "" ON)
  option(SDL_STATIC "" OFF)
  option(SDL_SHARED_ENABLED_BY_DEFAULT "" OFF)
  option(SDL_TEST "" OFF)
  option(SDL2_DISABLE_INSTALL "" ON)

  add_subdirectory(SDL)
  set_output_dir(SDL2 "")
  set(SDL2_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/SDL/include" CACHE PATH "" FORCE)
endif()

if(USE_SYSTEM_ZLIB)
  find_package(ZLIB REQUIRED GLOBAL)
  add_library(zlib ALIAS ZLIB::ZLIB)
  target_link_libraries(ZLIB::ZLIB INTERFACE ZLIB::ZLIB)
else()
  add_subdirectory(zlib)
  set_output_dir(zlib "")
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
    set(OGG_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/HPL2/dependencies/include)
  endif()
  add_subdirectory(vorbis)
  set_target_properties(vorbis PROPERTIES FOLDER extern)
endif()

if (USE_SYSTEM_TINYXML)
  find_package(tinyxml2 CONFIG REQUIRED GLOBAL)
else()
  option(BUILD_TESTING "" OFF)
  option(tinyxml2_INSTALL_PKGCONFIG "" OFF)
  add_subdirectory(tinyxml2)
endif()

# OpenAL the library: still openal-soft (kept as submodule per session
# decision). The Frictional OAL_Wrapper layer below consumes the resulting
# OpenAL target.
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
  set_target_properties(common PROPERTIES FOLDER extern/openal-soft)
  set_target_properties(ex-common PROPERTIES FOLDER extern/openal-soft)
  target_compile_definitions(OpenAL PUBLIC AL_LIBTYPE_STATIC)
endif()

# freealut comes before OpenAL because the FG OAL wrapper consumes
# alutGetError/alutLoadWAVFile from this lib.
add_subdirectory(freealut)
set_target_properties(alut PROPERTIES FOLDER extern)

# Frictional Games' OpenAL wrapper (formerly OALWrapper). Snapshot lives at
# HPL2/dependencies/sources/OpenAL/ matching the VS solution layout; produces
# the OALWrapper target HPL2 links against.
add_subdirectory(OpenAL)
set_target_properties(OALWrapper PROPERTIES FOLDER extern)

# DevIL + its bundled codec wiring. libjpeg and libpng are vendored siblings
# we author CMakeLists for; DevIL discovers them via cache vars.
if(USE_SYSTEM_DEVIL)
  find_package(DevIL REQUIRED GLOBAL)
else()
  add_subdirectory(jpeg)
  set_target_properties(jpeg PROPERTIES FOLDER extern)
  add_subdirectory(png)
  set_target_properties(png PROPERTIES FOLDER extern)

  # Steer DevIL's optional-codec discovery: IL_*_LIB=ON skips its
  # find_package() calls; the {JPEG,PNG}_INCLUDE_DIRS / *_LIBRARIES vars are
  # then consumed directly by DevIL/src-IL/CMakeLists.txt.
  set(IL_JPEG_LIB ON CACHE BOOL "" FORCE)
  set(JPEG_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/jpeg" CACHE PATH "" FORCE)
  set(JPEG_LIBRARIES jpeg CACHE STRING "" FORCE)
  set(IL_PNG_LIB ON CACHE BOOL "" FORCE)
  set(PNG_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/png" CACHE PATH "" FORCE)
  set(PNG_LIBRARIES png CACHE STRING "" FORCE)
  # FindTIFF and FindJasper transitively call find_package(JPEG); without these
  # IL_*_LIB skips, that secondary search overwrites JPEG_LIBRARY in the cache
  # with NOTFOUND and breaks the IL link. Short-circuit the codecs we don't
  # bundle so DevIL relies entirely on the libs we wired above.
  set(IL_TIFF_LIB ON CACHE BOOL "" FORCE)
  set(IL_NO_TIF 1 CACHE STRING "" FORCE)
  set(IL_JASPER_LIB ON CACHE BOOL "" FORCE)
  set(IL_NO_JP2 1 CACHE STRING "" FORCE)

  option(IL_TESTS "" OFF)
  option(BUILD_SHARED_LIBS "" OFF)
  add_subdirectory(DevIL/DevIL)
  set(IL_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/DevIL/DevIL/include" CACHE PATH "" FORCE)
endif()

# Pure VS-snapshot deps with new CMakeLists in this session.
add_subdirectory(AngelScript)
set_target_properties(AngelScript PROPERTIES FOLDER extern)
add_subdirectory(Newton)
set_target_properties(Newton PROPERTIES FOLDER extern)

if(USE_SYSTEM_GLEW)
  find_package(GLEW REQUIRED GLOBAL)
else()
  add_subdirectory(GLEW)
endif()

# --- Vulkan stack and math library ---

set(VOLK_STATIC_DEFINES "")
if(CMAKE_SYSTEM_NAME STREQUAL Linux)
    if (USE_GRAPHICS_X11)
        find_path(XLIB_HEADERS NAMES "X11/Xlib.h")
        if (NOT XLIB_HEADERS)
            message(FATAL_ERROR "Can't find 'X11/xlib.h'.(libx11-dev is not installed?)")
        endif()
        list(APPEND VOLK_STATIC_DEFINES VK_USE_PLATFORM_XLIB_KHR)
    endif()
    if (USE_GRAPHICS_WAYLAND)
        find_path(WAYLAND_HEADERS NAMES "wayland-client.h")
        if (NOT WAYLAND_HEADERS)
            message(FATAL_ERROR "Can't find 'wayland-client.h'.(libwayland-dev is not installed?)")
        endif()
        list(APPEND VOLK_STATIC_DEFINES VK_USE_PLATFORM_WAYLAND_KHR)
    endif()
elseif (${CMAKE_SYSTEM_NAME} MATCHES "Windows")
  list(APPEND VOLK_STATIC_DEFINES VK_USE_PLATFORM_WIN32_KHR)
endif()

set(VULKAN_HEADERS_INSTALL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/Vulkan-Headers")
option(VOLK_INSTALL "" OFF)
add_subdirectory(volk)

add_subdirectory(glslang)
add_subdirectory(VulkanMemoryAllocator)
add_subdirectory(MathLib)
