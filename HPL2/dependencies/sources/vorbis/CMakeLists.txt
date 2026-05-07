cmake_minimum_required(VERSION 3.15...3.31)
project(vorbis C)

# Snapshot is libvorbis with vorbisfile bundled into one static lib (matches
# the .vcxproj). Source list mirrors vorbis.vcxproj.filters; headers live in
# HPL2/dependencies/include/vorbis/.
set(VORBIS_SRCS
    analysis.c barkmel.c bitrate.c block.c codebook.c envelope.c floor0.c
    floor1.c info.c lookup.c lpc.c lsp.c mapping0.c mdct.c psy.c registry.c
    res0.c sharedbook.c smallft.c synthesis.c vorbisenc.c vorbisfile.c
    window.c
)

add_library(vorbis STATIC ${VORBIS_SRCS})
target_include_directories(vorbis PUBLIC
    ${CMAKE_SOURCE_DIR}/HPL2/dependencies/include
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(vorbis PUBLIC ogg)
set_target_properties(vorbis PROPERTIES POSITION_INDEPENDENT_CODE ON)

# OALWrapper consumes Vorbis::vorbis + Vorbis::vorbisfile; both alias to the
# combined static lib here.
if(NOT TARGET Vorbis::vorbis)
    add_library(Vorbis::vorbis ALIAS vorbis)
endif()
if(NOT TARGET Vorbis::vorbisfile)
    add_library(Vorbis::vorbisfile ALIAS vorbis)
endif()

if(MSVC)
    target_compile_definitions(vorbis PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(vorbis PRIVATE /wd4267 /wd4244 /wd4305 /wd4101)
endif()
