cmake_minimum_required(VERSION 3.15...3.31)
project(alut C)

# freealut snapshot. Headers live in HPL2/dependencies/include/AL/.
set(ALUT_SRCS
    alutBufferData.c alutCodec.c alutError.c alutInit.c alutInputStream.c
    alutLoader.c alutOutputStream.c alutUtil.c alutVersion.c alutWaveform.c
)

add_library(alut STATIC ${ALUT_SRCS})
# alutInternal.h does `#include "alut.h"` (no AL/ prefix), so the AL/ subdir
# itself has to be on the include path, not just the parent /include/.
target_include_directories(alut PUBLIC
    ${CMAKE_SOURCE_DIR}/HPL2/dependencies/include
    ${CMAKE_SOURCE_DIR}/HPL2/dependencies/include/AL
)
target_link_libraries(alut PUBLIC OpenAL)
set_target_properties(alut PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(WIN32)
    target_compile_definitions(alut PRIVATE
        HAVE_WINDOWS_H HAVE_SLEEP HAVE__STAT HAVE_BASETSD_H
        HAVE_STDINT_H
    )
else()
    target_compile_definitions(alut PRIVATE
        HAVE_TIME_H HAVE_NANOSLEEP HAVE_UNISTD_H HAVE_STAT
        HAVE_STDINT_H
    )
endif()
if(MSVC)
    target_compile_definitions(alut PRIVATE _CRT_SECURE_NO_WARNINGS ALUT_BUILD_LIBRARY)
endif()
