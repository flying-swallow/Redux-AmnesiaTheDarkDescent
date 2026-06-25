option(BUILD_EXTERNAL OFF)
option(ENABLE_HLSL OFF)
option(ENABLE_OPT OFF)
option(ENABLE_PCH OFF)
option(ENABLE_CTEST OFF)

get_filename_component(HPL2_DEPS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
set(HPL2_DEPS_INCLUDE_DIR "${HPL2_DEPS_DIR}/include")
set(HPL2_DEPS_SOURCE_DIR "${HPL2_DEPS_DIR}/sources")

set(VOLK_STATIC_DEFINES "")
if(CMAKE_SYSTEM_NAME STREQUAL Linux)
  if(USE_GRAPHICS_X11)
    find_path(XLIB_HEADERS NAMES "X11/Xlib.h")
    if(NOT XLIB_HEADERS)
      message(FATAL_ERROR "Can't find 'X11/xlib.h'.(libx11-dev is not installed?)")
    endif()
    list(APPEND VOLK_STATIC_DEFINES VK_USE_PLATFORM_XLIB_KHR)
  endif()
  if(USE_GRAPHICS_WAYLAND)
    find_path(WAYLAND_HEADERS NAMES "wayland-client.h")
    if(NOT WAYLAND_HEADERS)
      message(FATAL_ERROR "Can't find 'wayland-client.h'.(libwayland-dev is not installed?)")
    endif()
    list(APPEND VOLK_STATIC_DEFINES VK_USE_PLATFORM_WAYLAND_KHR)
  endif()
elseif(CMAKE_SYSTEM_NAME MATCHES "Windows")
  list(APPEND VOLK_STATIC_DEFINES VK_USE_PLATFORM_WIN32_KHR)
endif()

add_subdirectory(Vulkan-Headers)

set(VULKAN_HEADERS_INSTALL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/Vulkan-Headers")
option(VOLK_INSTALL "" OFF)
add_subdirectory(volk)

option(VMA_ENABLE_INSTALL "" OFF)
add_subdirectory(VulkanMemoryAllocator)

if(USE_SYSTEM_ZLIB)
  find_package(ZLIB REQUIRED GLOBAL)
  if(NOT TARGET zlib)
    add_library(zlib ALIAS ZLIB::ZLIB)
  endif()
else()
  file(GLOB ZLIB_SOURCES CONFIGURE_DEPENDS "${HPL2_DEPS_SOURCE_DIR}/zlib/*.c")
  add_library(zlib STATIC ${ZLIB_SOURCES})
  add_library(ZLIB::ZLIB ALIAS zlib)
  target_include_directories(zlib PUBLIC "${HPL2_DEPS_SOURCE_DIR}/zlib")
  set_target_properties(zlib PROPERTIES POSITION_INDEPENDENT_CODE ON)
  set(ZLIB_INCLUDE_DIR "${HPL2_DEPS_SOURCE_DIR}/zlib" CACHE PATH "" FORCE)
  set(ZLIB_INCLUDE_DIRS "${HPL2_DEPS_SOURCE_DIR}/zlib" CACHE PATH "" FORCE)
  set(ZLIB_LIBRARY zlib CACHE STRING "" FORCE)
  set(ZLIB_LIBRARIES zlib CACHE STRING "" FORCE)
endif()

if(USE_SYSTEM_OGG)
  find_package(Ogg REQUIRED GLOBAL)
else()
  add_library(ogg STATIC
    "${HPL2_DEPS_SOURCE_DIR}/ogg/bitwise.c"
    "${HPL2_DEPS_SOURCE_DIR}/ogg/framing.c"
  )
  add_library(Ogg::ogg ALIAS ogg)
  target_include_directories(ogg
    PUBLIC "${HPL2_DEPS_INCLUDE_DIR}"
    PRIVATE "${HPL2_DEPS_SOURCE_DIR}/ogg"
  )
  set_target_properties(ogg PROPERTIES POSITION_INDEPENDENT_CODE ON)
  set(OGG_INCLUDE_DIR "${HPL2_DEPS_INCLUDE_DIR}" CACHE PATH "" FORCE)
  set(OGG_LIBRARY ogg CACHE STRING "" FORCE)
endif()

if(USE_SYSTEM_VORBIS)
  find_package(Vorbis REQUIRED GLOBAL)
else()
  file(GLOB VORBIS_SOURCES CONFIGURE_DEPENDS "${HPL2_DEPS_SOURCE_DIR}/vorbis/*.c")
  list(REMOVE_ITEM VORBIS_SOURCES
    "${HPL2_DEPS_SOURCE_DIR}/vorbis/vorbisenc.c"
    "${HPL2_DEPS_SOURCE_DIR}/vorbis/vorbisfile.c"
  )

  add_library(vorbis STATIC ${VORBIS_SOURCES})
  add_library(Vorbis::vorbis ALIAS vorbis)
  target_include_directories(vorbis
    PUBLIC "${HPL2_DEPS_INCLUDE_DIR}"
    PRIVATE "${HPL2_DEPS_SOURCE_DIR}/vorbis"
  )
  target_link_libraries(vorbis PUBLIC Ogg::ogg PRIVATE $<$<BOOL:${UNIX}>:m>)
  set_target_properties(vorbis PROPERTIES POSITION_INDEPENDENT_CODE ON)

  add_library(vorbisfile STATIC "${HPL2_DEPS_SOURCE_DIR}/vorbis/vorbisfile.c")
  add_library(Vorbis::vorbisfile ALIAS vorbisfile)
  target_include_directories(vorbisfile
    PUBLIC "${HPL2_DEPS_INCLUDE_DIR}"
    PRIVATE "${HPL2_DEPS_SOURCE_DIR}/vorbis"
  )
  target_link_libraries(vorbisfile PUBLIC Vorbis::vorbis Ogg::ogg)
  set_target_properties(vorbisfile PROPERTIES POSITION_INDEPENDENT_CODE ON)

  add_library(vorbisenc STATIC "${HPL2_DEPS_SOURCE_DIR}/vorbis/vorbisenc.c")
  add_library(Vorbis::vorbisenc ALIAS vorbisenc)
  target_include_directories(vorbisenc
    PUBLIC "${HPL2_DEPS_INCLUDE_DIR}"
    PRIVATE "${HPL2_DEPS_SOURCE_DIR}/vorbis"
  )
  target_link_libraries(vorbisenc PUBLIC Vorbis::vorbis Ogg::ogg)
  set_target_properties(vorbisenc PROPERTIES POSITION_INDEPENDENT_CODE ON)

  set(VORBIS_INCLUDE_DIR "${HPL2_DEPS_INCLUDE_DIR}" CACHE PATH "" FORCE)
  set(VORBISFILE_INCLUDE_DIR "${HPL2_DEPS_INCLUDE_DIR}" CACHE PATH "" FORCE)
endif()

if(USE_SYSTEM_SDL2)
  find_package(SDL2 REQUIRED GLOBAL)
elseif(EXISTS "${HPL2_DEPS_SOURCE_DIR}/SDL2/CMakeLists.txt")
  option(SDL_RPATH "" OFF)
  option(SDL_SHARED "" ON)
  option(SDL_STATIC "" OFF)
  option(SDL_SHARED_ENABLED_BY_DEFAULT "" OFF)
  option(SDL_TEST "" OFF)
  option(SDL2_DISABLE_INSTALL "" ON)
  add_subdirectory("${HPL2_DEPS_SOURCE_DIR}/SDL2" "${CMAKE_CURRENT_BINARY_DIR}/SDL2")
  set_output_dir(SDL2 "")
  set(SDL2_INCLUDE_DIR "${HPL2_DEPS_INCLUDE_DIR}/SDL2" CACHE PATH "" FORCE)
else()
  message(STATUS "Bundled SDL2 source has no CMakeLists.txt; using system SDL2.")
  find_package(SDL2 REQUIRED GLOBAL)
endif()

if(NOT TARGET SDL2::SDL2)
  if(DEFINED SDL2_LIBRARIES)
    set(_SDL2_IMPORTED_INCLUDE_DIRS "${SDL2_INCLUDE_DIRS}")
    if(NOT _SDL2_IMPORTED_INCLUDE_DIRS AND SDL2_INCLUDE_DIR)
      set(_SDL2_IMPORTED_INCLUDE_DIRS "${SDL2_INCLUDE_DIR}")
    endif()
    add_library(SDL2::SDL2 INTERFACE IMPORTED GLOBAL)
    set_target_properties(SDL2::SDL2 PROPERTIES
      INTERFACE_INCLUDE_DIRECTORIES "${_SDL2_IMPORTED_INCLUDE_DIRS}"
      INTERFACE_LINK_LIBRARIES "${SDL2_LIBRARIES}"
    )
  else()
    message(FATAL_ERROR "SDL2 was found, but no SDL2::SDL2 target or SDL2_LIBRARIES variable was provided.")
  endif()
endif()

if(NOT SDL2_INCLUDE_DIR)
  if(SDL2_INCLUDE_DIRS)
    set(SDL2_INCLUDE_DIR "${SDL2_INCLUDE_DIRS}" CACHE PATH "" FORCE)
  else()
    set(SDL2_INCLUDE_DIR "${HPL2_DEPS_INCLUDE_DIR}/SDL2" CACHE PATH "" FORCE)
  endif()
endif()

if(USE_SYSTEM_OPENAL)
  find_package(OpenAL REQUIRED GLOBAL)
  if(NOT TARGET OpenAL::OpenAL)
    add_library(OpenAL::OpenAL INTERFACE IMPORTED GLOBAL)
    set_target_properties(OpenAL::OpenAL PROPERTIES
      INTERFACE_LINK_LIBRARIES "${OPENAL_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${OPENAL_INCLUDE_DIR}"
    )
  endif()
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
  if(TARGET OpenAL)
    set_target_properties(OpenAL PROPERTIES FOLDER extern/openal-soft)
    target_compile_definitions(OpenAL PUBLIC AL_LIBTYPE_STATIC)
  endif()
  if(TARGET alsoft.common)
    set_target_properties(alsoft.common PROPERTIES FOLDER extern/openal-soft)
  endif()
  if(TARGET alsoft.excommon)
    set_target_properties(alsoft.excommon PROPERTIES FOLDER extern/openal-soft)
  endif()
endif()

file(GLOB FREEALUT_SOURCES CONFIGURE_DEPENDS "${HPL2_DEPS_SOURCE_DIR}/freealut/*.c")
add_library(freealut STATIC ${FREEALUT_SOURCES})
target_include_directories(freealut
  PUBLIC
    "${HPL2_DEPS_INCLUDE_DIR}"
    "${HPL2_DEPS_INCLUDE_DIR}/AL"
  PRIVATE
    "${HPL2_DEPS_SOURCE_DIR}/freealut"
)
target_link_libraries(freealut PUBLIC OpenAL::OpenAL)
target_compile_definitions(freealut PRIVATE HAVE_STDINT_H=1)
if(WIN32)
  target_compile_definitions(freealut PRIVATE HAVE__STAT=1 HAVE_SLEEP=1 HAVE_WINDOWS_H=1)
else()
  target_compile_definitions(freealut PRIVATE HAVE_STAT=1 HAVE_UNISTD_H=1 HAVE_USLEEP=1)
endif()
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_definitions(freealut PRIVATE HAVE___ATTRIBUTE__=1)
endif()
set_target_properties(freealut PROPERTIES POSITION_INDEPENDENT_CODE ON)

file(GLOB OALWRAPPER_SOURCES CONFIGURE_DEPENDS "${HPL2_DEPS_SOURCE_DIR}/OpenAL/*.cpp")
file(GLOB OALWRAPPER_HEADERS CONFIGURE_DEPENDS "${HPL2_DEPS_INCLUDE_DIR}/OpenAL/*.h")
add_library(OALWrapper STATIC ${OALWRAPPER_SOURCES} ${OALWRAPPER_HEADERS})
target_compile_definitions(OALWrapper PUBLIC USE_SDL2)
target_include_directories(OALWrapper
  PUBLIC "${HPL2_DEPS_INCLUDE_DIR}"
  PRIVATE
    "${HPL2_DEPS_SOURCE_DIR}/OpenAL"
    "${CMAKE_SOURCE_DIR}/HPL2/core/include"
)
target_link_libraries(OALWrapper
  PUBLIC
    Ogg::ogg
    Vorbis::vorbisfile
    Vorbis::vorbis
    OpenAL::OpenAL
    SDL2::SDL2
  PRIVATE
    freealut
)
set_target_properties(OALWrapper PROPERTIES POSITION_INDEPENDENT_CODE ON)
set(OALWRAPPER_INCLUDE_DIR "${HPL2_DEPS_INCLUDE_DIR}" CACHE PATH "" FORCE)

if(MSVC)
  enable_language(ASM_MASM)
endif()

set(ANGELSCRIPT_SOURCE_DIR "${HPL2_DEPS_SOURCE_DIR}/AngelScript")
set(ANGELSCRIPT_ASM_SOURCES)
if(MSVC)
  set(ANGELSCRIPT_ASM_SOURCES "${ANGELSCRIPT_SOURCE_DIR}/sources/as_callfunc_x64_msvc_asm.asm")
  set_source_files_properties(${ANGELSCRIPT_ASM_SOURCES} PROPERTIES LANGUAGE ASM_MASM)
  set_source_files_properties(${ANGELSCRIPT_ASM_SOURCES} PROPERTIES COMPILE_DEFINITIONS "_M_X64")
endif()

add_library(AngelScript STATIC
  ${ANGELSCRIPT_ASM_SOURCES}
  "${ANGELSCRIPT_SOURCE_DIR}/sources/as_arrayobject.cpp"
  "${ANGELSCRIPT_SOURCE_DIR}/sources/as_atomic.cpp"
  "${ANGELSCRIPT_SOURCE_DIR}/sources/as_builder.cpp"
  "${ANGELSCRIPT_SOURCE_DIR}/sources/as_bytecode.cpp"
  "${ANGELSCRIPT_SOURCE_DIR}/sources/as_callfunc_arm.cpp"
  "${ANGELSCRIPT_SOURCE_DIR}/sources/as_callfunc_mips.cpp"
  "${ANGELSCRIPT_SOURCE_DIR}/sources/as_callfunc_ppc_64.cpp"
  "${ANGELSCRIPT_SOURCE_DIR}/sources/as_callfunc_ppc.cpp"
  "${ANGELSCRIPT_SOURCE_DIR}/sources/as_callfunc_sh4.cpp"
  "${ANGELSCRIPT_SOURCE_DIR}/sources/as_callfunc_x64_gcc.cpp"
  "${ANGELSCRIPT_SOURCE_DIR}/sources/as_callfunc_x64_msvc.cpp"
  "${ANGELSCRIPT_SOURCE_DIR}/sources/as_callfunc_x86.cpp"
  "${ANGELSCRIPT_SOURCE_DIR}/sources/as_callfunc_xenon.cpp"
  "${ANGELSCRIPT_SOURCE_DIR}/sources/as_callfunc.cpp"
  "${ANGELSCRIPT_SOURCE_DIR}/sources/as_compiler.cpp"
  "${ANGELSCRIPT_SOURCE_DIR}/sources/as_configgroup.cpp"
  "${ANGELSCRIPT_SOURCE_DIR}/sources/as_context.cpp"
  "${ANGELSCRIPT_SOURCE_DIR}/sources/as_datatype.cpp"
  "${ANGELSCRIPT_SOURCE_DIR}/sources/as_gc.cpp"
  "${ANGELSCRIPT_SOURCE_DIR}/sources/as_generic.cpp"
  "${ANGELSCRIPT_SOURCE_DIR}/sources/as_globalproperty.cpp"
  "${ANGELSCRIPT_SOURCE_DIR}/sources/as_memory.cpp"
  "${ANGELSCRIPT_SOURCE_DIR}/sources/as_module.cpp"
  "${ANGELSCRIPT_SOURCE_DIR}/sources/as_objecttype.cpp"
  "${ANGELSCRIPT_SOURCE_DIR}/sources/as_outputbuffer.cpp"
  "${ANGELSCRIPT_SOURCE_DIR}/sources/as_parser.cpp"
  "${ANGELSCRIPT_SOURCE_DIR}/sources/as_restore.cpp"
  "${ANGELSCRIPT_SOURCE_DIR}/sources/as_scriptcode.cpp"
  "${ANGELSCRIPT_SOURCE_DIR}/sources/as_scriptengine.cpp"
  "${ANGELSCRIPT_SOURCE_DIR}/sources/as_scriptfunction.cpp"
  "${ANGELSCRIPT_SOURCE_DIR}/sources/as_scriptnode.cpp"
  "${ANGELSCRIPT_SOURCE_DIR}/sources/as_scriptobject.cpp"
  "${ANGELSCRIPT_SOURCE_DIR}/sources/as_string_util.cpp"
  "${ANGELSCRIPT_SOURCE_DIR}/sources/as_string.cpp"
  "${ANGELSCRIPT_SOURCE_DIR}/sources/as_thread.cpp"
  "${ANGELSCRIPT_SOURCE_DIR}/sources/as_tokenizer.cpp"
  "${ANGELSCRIPT_SOURCE_DIR}/sources/as_typeinfo.cpp"
  "${ANGELSCRIPT_SOURCE_DIR}/sources/as_variablescope.cpp"
)
target_include_directories(AngelScript PUBLIC "${ANGELSCRIPT_SOURCE_DIR}/include")
set_target_properties(AngelScript PROPERTIES POSITION_INDEPENDENT_CODE ON)

set(NEWTON_SOURCE_DIR "${HPL2_DEPS_SOURCE_DIR}/Newton")
add_library(Newton STATIC
  "${NEWTON_SOURCE_DIR}/core/dg.cpp"
  "${NEWTON_SOURCE_DIR}/core/dgAABBPolygonSoup.cpp"
  "${NEWTON_SOURCE_DIR}/core/dgConvexHull3d.cpp"
  "${NEWTON_SOURCE_DIR}/core/dgConvexHull4d.cpp"
  "${NEWTON_SOURCE_DIR}/core/dgCRC.cpp"
  "${NEWTON_SOURCE_DIR}/core/dgDebug.cpp"
  "${NEWTON_SOURCE_DIR}/core/dgDelaunayTetrahedralization.cpp"
  "${NEWTON_SOURCE_DIR}/core/dgGeneralMatrix.cpp"
  "${NEWTON_SOURCE_DIR}/core/dgGeneralVector.cpp"
  "${NEWTON_SOURCE_DIR}/core/dgGoogol.cpp"
  "${NEWTON_SOURCE_DIR}/core/dgIntersections.cpp"
  "${NEWTON_SOURCE_DIR}/core/dgMatrix.cpp"
  "${NEWTON_SOURCE_DIR}/core/dgMemory.cpp"
  "${NEWTON_SOURCE_DIR}/core/dgNode.cpp"
  "${NEWTON_SOURCE_DIR}/core/dgPolygonSoupBuilder.cpp"
  "${NEWTON_SOURCE_DIR}/core/dgPolyhedra.cpp"
  "${NEWTON_SOURCE_DIR}/core/dgPolyhedraMassProperties.cpp"
  "${NEWTON_SOURCE_DIR}/core/dgQuaternion.cpp"
  "${NEWTON_SOURCE_DIR}/core/dgRandom.cpp"
  "${NEWTON_SOURCE_DIR}/core/dgRef.cpp"
  "${NEWTON_SOURCE_DIR}/core/dgRefCounter.cpp"
  "${NEWTON_SOURCE_DIR}/core/dgSmallDeterminant.cpp"
  "${NEWTON_SOURCE_DIR}/core/dgSPDMatrix.cpp"
  "${NEWTON_SOURCE_DIR}/core/dgSphere.cpp"
  "${NEWTON_SOURCE_DIR}/core/dgThreads.cpp"
  "${NEWTON_SOURCE_DIR}/core/dgTree.cpp"
  "${NEWTON_SOURCE_DIR}/core/dgTypes.cpp"
  "${NEWTON_SOURCE_DIR}/physics/dgBallConstraint.cpp"
  "${NEWTON_SOURCE_DIR}/physics/dgBilateralConstraint.cpp"
  "${NEWTON_SOURCE_DIR}/physics/dgBody.cpp"
  "${NEWTON_SOURCE_DIR}/physics/dgBodyMasterList.cpp"
  "${NEWTON_SOURCE_DIR}/physics/dgBroadPhaseCollision.cpp"
  "${NEWTON_SOURCE_DIR}/physics/dgCollision.cpp"
  "${NEWTON_SOURCE_DIR}/physics/dgCollisionBox.cpp"
  "${NEWTON_SOURCE_DIR}/physics/dgCollisionBVH.cpp"
  "${NEWTON_SOURCE_DIR}/physics/dgCollisionCapsule.cpp"
  "${NEWTON_SOURCE_DIR}/physics/dgCollisionChamferCylinder.cpp"
  "${NEWTON_SOURCE_DIR}/physics/dgCollisionCompound.cpp"
  "${NEWTON_SOURCE_DIR}/physics/dgCollisionCompoundBreakable.cpp"
  "${NEWTON_SOURCE_DIR}/physics/dgCollisionCone.cpp"
  "${NEWTON_SOURCE_DIR}/physics/dgCollisionConvex.cpp"
  "${NEWTON_SOURCE_DIR}/physics/dgCollisionConvexHull.cpp"
  "${NEWTON_SOURCE_DIR}/physics/dgCollisionConvexModifier.cpp"
  "${NEWTON_SOURCE_DIR}/physics/dgCollisionCylinder.cpp"
  "${NEWTON_SOURCE_DIR}/physics/dgCollisionEllipse.cpp"
  "${NEWTON_SOURCE_DIR}/physics/dgCollisionHeightField.cpp"
  "${NEWTON_SOURCE_DIR}/physics/dgCollisionMesh.cpp"
  "${NEWTON_SOURCE_DIR}/physics/dgCollisionNull.cpp"
  "${NEWTON_SOURCE_DIR}/physics/dgCollisionScene.cpp"
  "${NEWTON_SOURCE_DIR}/physics/dgCollisionSphere.cpp"
  "${NEWTON_SOURCE_DIR}/physics/dgCollisionUserMesh.cpp"
  "${NEWTON_SOURCE_DIR}/physics/dgConnectorConstraint.cpp"
  "${NEWTON_SOURCE_DIR}/physics/dgConstraint.cpp"
  "${NEWTON_SOURCE_DIR}/physics/dgContact.cpp"
  "${NEWTON_SOURCE_DIR}/physics/dgCorkscrewConstraint.cpp"
  "${NEWTON_SOURCE_DIR}/physics/dgHingeConstraint.cpp"
  "${NEWTON_SOURCE_DIR}/physics/dgMeshEffect.cpp"
  "${NEWTON_SOURCE_DIR}/physics/dgMeshEffect2.cpp"
  "${NEWTON_SOURCE_DIR}/physics/dgMinkowskiConv.cpp"
  "${NEWTON_SOURCE_DIR}/physics/dgNarrowPhaseCollision.cpp"
  "${NEWTON_SOURCE_DIR}/physics/dgPointToCurveConstraint.cpp"
  "${NEWTON_SOURCE_DIR}/physics/dgSlidingConstraint.cpp"
  "${NEWTON_SOURCE_DIR}/physics/dgUniversalConstraint.cpp"
  "${NEWTON_SOURCE_DIR}/physics/dgUpVectorConstraint.cpp"
  "${NEWTON_SOURCE_DIR}/physics/dgUserConstraint.cpp"
  "${NEWTON_SOURCE_DIR}/physics/dgWorld.cpp"
  "${NEWTON_SOURCE_DIR}/physics/dgWorldDynamicUpdate.cpp"
  "${NEWTON_SOURCE_DIR}/newton/Newton.cpp"
  "${NEWTON_SOURCE_DIR}/newton/NewtonClass.cpp"
)
target_include_directories(Newton
  PUBLIC "${HPL2_DEPS_INCLUDE_DIR}"
  PRIVATE
    "${NEWTON_SOURCE_DIR}/core"
    "${NEWTON_SOURCE_DIR}/physics"
    "${NEWTON_SOURCE_DIR}/newton"
)
set_target_properties(Newton PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(UNIX)
  target_compile_options(Newton PRIVATE -Wno-extern-c-compat)
endif()

if(WIN32)
  target_compile_definitions(Newton PUBLIC "_NEWTON_USE_LIB=1")
  if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    target_compile_definitions(Newton PRIVATE _WIN_64_VER)
  else()
    target_compile_definitions(Newton PRIVATE _WIN_32_VER)
  endif()
elseif(APPLE)
  target_compile_definitions(Newton PRIVATE _MAC_VER)
  if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    target_compile_definitions(Newton PRIVATE _MAC_VER_64)
  endif()
elseif(UNIX)
  target_compile_definitions(Newton PRIVATE _LINUX_VER)
  if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    target_compile_definitions(Newton PRIVATE _LINUX_VER_64)
  endif()
endif()

if(USE_SYSTEM_TINYXML)
  find_package(tinyxml2 CONFIG REQUIRED GLOBAL)
else()
  option(BUILD_TESTING "" OFF)
  option(tinyxml2_INSTALL "" OFF)
  option(tinyxml2_INSTALL_PKGCONFIG "" OFF)
  add_subdirectory(tinyxml2)
endif()

if(USE_SYSTEM_DEVIL)
  find_package(DevIL REQUIRED GLOBAL)
  if(NOT TARGET IL)
    add_library(IL INTERFACE IMPORTED GLOBAL)
    set_target_properties(IL PROPERTIES
      INTERFACE_LINK_LIBRARIES "${IL_LIBRARIES}"
      INTERFACE_INCLUDE_DIRECTORIES "${IL_INCLUDE_DIR}"
    )
  endif()
else()
  add_subdirectory("${HPL2_DEPS_SOURCE_DIR}/jpeg" "${CMAKE_CURRENT_BINARY_DIR}/jpeg")
  set_target_properties(jpeg PROPERTIES FOLDER extern)
  add_subdirectory("${HPL2_DEPS_SOURCE_DIR}/png" "${CMAKE_CURRENT_BINARY_DIR}/png")
  set_target_properties(png PROPERTIES FOLDER extern)

  set(IL_JPEG_LIB ON CACHE BOOL "" FORCE)
  set(JPEG_INCLUDE_DIRS "${HPL2_DEPS_SOURCE_DIR}/jpeg" CACHE PATH "" FORCE)
  set(JPEG_LIBRARIES jpeg CACHE STRING "" FORCE)
  set(IL_PNG_LIB ON CACHE BOOL "" FORCE)
  set(PNG_INCLUDE_DIRS "${HPL2_DEPS_SOURCE_DIR}/png" CACHE PATH "" FORCE)
  set(PNG_LIBRARIES png CACHE STRING "" FORCE)
  set(IL_TIFF_LIB ON CACHE BOOL "" FORCE)
  set(IL_NO_TIF 1 CACHE STRING "" FORCE)
  set(IL_JASPER_LIB ON CACHE BOOL "" FORCE)
  set(IL_NO_JP2 1 CACHE STRING "" FORCE)
  set(IL_BUILD_ILUT OFF CACHE BOOL "" FORCE)
  option(IL_TESTS "" OFF)
  option(BUILD_SHARED_LIBS "" OFF)
  add_subdirectory("${HPL2_DEPS_SOURCE_DIR}/DevIL" "${CMAKE_CURRENT_BINARY_DIR}/DevIL")
  set(IL_INCLUDE_DIR "${HPL2_DEPS_INCLUDE_DIR}" CACHE PATH "" FORCE)
endif()

add_subdirectory(MathLib)

# MathLib uses SSE3+ intrinsics that GCC/Clang gate behind -msse* flags (MSVC
# enables them implicitly on x64).
if(NOT MSVC AND NOT (CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|ARM64|arm"))
  target_compile_options(MathLib INTERFACE -msse4.2)
endif()
