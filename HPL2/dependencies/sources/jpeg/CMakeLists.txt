cmake_minimum_required(VERSION 3.15...3.31)
project(jpeg C)

# Mirrors the source list in jpeg.vcxproj.filters (libjpeg 9d, jmemnobs).
set(JPEG_SRCS
    jaricom.c jcapimin.c jcapistd.c jcarith.c jccoefct.c jccolor.c
    jcdctmgr.c jchuff.c jcinit.c jcmainct.c jcmarker.c jcmaster.c
    jcomapi.c jcparam.c jcprepct.c jcsample.c jctrans.c
    jdapimin.c jdapistd.c jdarith.c jdatadst.c jdatasrc.c jdcoefct.c
    jdcolor.c jddctmgr.c jdhuff.c jdinput.c jdmainct.c jdmarker.c
    jdmaster.c jdmerge.c jdpostct.c jdsample.c jdtrans.c
    jerror.c jfdctflt.c jfdctfst.c jfdctint.c jidctflt.c jidctfst.c
    jidctint.c jmemmgr.c jmemnobs.c jquant1.c jquant2.c jutils.c
)

add_library(jpeg STATIC ${JPEG_SRCS})
target_include_directories(jpeg PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(jpeg PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(MSVC)
    target_compile_definitions(jpeg PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(jpeg PRIVATE /wd4267 /wd4244 /wd4018 /wd4996)
endif()
