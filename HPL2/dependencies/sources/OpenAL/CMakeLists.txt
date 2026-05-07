cmake_minimum_required(VERSION 3.15...3.31)
project(OpenALWrapper CXX)

# Frictional Games' OpenAL wrapper — the snapshot uses a flat `OpenAL/` source
# layout (matching HPL2/dependencies/include/OpenAL/), distinct from the
# upstream OALWrapper layout (which exposes headers under `OALWrapper/...`).
# HPL2 picks the right include style via the USE_OALWRAPPER preprocessor flag;
# this snapshot expects USE_OALWRAPPER to be undefined.

set(OAL_SRCS
    OAL_AudioData.cpp OAL_Buffer.cpp OAL_Device.cpp OAL_EFX.cpp
    OAL_EFXManager.cpp OAL_Effect.cpp OAL_Effect_Reverb.cpp OAL_EffectSlot.cpp
    OAL_Filter.cpp OAL_Helper.cpp OAL_Init.cpp OAL_Loaders.cpp
    OAL_LoggerObject.cpp OAL_OggHelper.cpp OAL_OggSample.cpp OAL_OggStream.cpp
    OAL_Playback.cpp OAL_Sample.cpp OAL_Source.cpp OAL_SourceManager.cpp
    OAL_Stream.cpp OAL_Types.cpp OAL_WAVSample.cpp
)

add_library(OALWrapper STATIC ${OAL_SRCS})
set_target_properties(OALWrapper PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_include_directories(OALWrapper PUBLIC
    ${CMAKE_SOURCE_DIR}/HPL2/dependencies/include
    # The Frictional OAL_Wrapper snapshot consumes HPL2's own system/* headers
    # (LowLevelSystem, MemoryManager, Platform, String). Logically this lib is
    # part of HPL2, not a third-party dep — wire its include path so it can
    # reach those headers without HPL2 having to repeat it for every consumer.
    ${CMAKE_SOURCE_DIR}/HPL2/core/include
)

target_compile_definitions(OALWrapper PRIVATE USE_SDL2)

target_link_libraries(OALWrapper PUBLIC
    OpenAL
    alut
    Vorbis::vorbisfile
    Vorbis::vorbis
    Ogg::ogg
    SDL2::SDL2
)

if(MSVC)
    target_compile_definitions(OALWrapper PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
