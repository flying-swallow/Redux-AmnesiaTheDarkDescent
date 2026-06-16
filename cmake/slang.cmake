# Slang shader compiler — prebuilt acquisition.
#
# Downloads the official slangc release for the host platform at configure
# time and exposes it as an IMPORTED executable target `slang::slangc`, so
# shaders.cmake can dispatch `.slang` shader compiles to it.
#
# Override with `-DSLANGC_EXECUTABLE=/path/to/slangc` to use a local install
# (e.g. the one bundled in the Vulkan SDK) and skip the download entirely.

set(SLANG_VERSION "2026.11" CACHE STRING
    "Slang release tag to download (without leading 'v'). Asset filename must exist at https://github.com/shader-slang/slang/releases.")
set(SLANGC_EXECUTABLE "" CACHE FILEPATH
    "Override: path to a slangc executable. Empty => download the pinned prebuilt release.")

# Resolve into a local var so changing SLANG_VERSION takes effect on reconfigure
# without the user having to clear the cache.
if(SLANGC_EXECUTABLE AND EXISTS "${SLANGC_EXECUTABLE}")
    set(_slang_slangc "${SLANGC_EXECUTABLE}")
    message(STATUS "Slang: using user-provided slangc at ${_slang_slangc}")
else()
    if(WIN32)
        set(_slang_os "windows")
        set(_slang_ext "zip")
    elseif(APPLE)
        set(_slang_os "macos")
        set(_slang_ext "zip")
    elseif(UNIX)
        set(_slang_os "linux")
        set(_slang_ext "tar.gz")
    else()
        message(FATAL_ERROR "Slang: unsupported host OS '${CMAKE_HOST_SYSTEM_NAME}'")
    endif()

    string(TOLOWER "${CMAKE_HOST_SYSTEM_PROCESSOR}" _slang_arch_raw)
    if(_slang_arch_raw MATCHES "amd64|x86_64|x64")
        set(_slang_arch "x86_64")
    elseif(_slang_arch_raw MATCHES "aarch64|arm64")
        set(_slang_arch "aarch64")
    else()
        message(FATAL_ERROR "Slang: unsupported host arch '${CMAKE_HOST_SYSTEM_PROCESSOR}'")
    endif()

    set(_slang_asset "slang-${SLANG_VERSION}-${_slang_os}-${_slang_arch}.${_slang_ext}")
    set(_slang_url "https://github.com/shader-slang/slang/releases/download/v${SLANG_VERSION}/${_slang_asset}")
    set(_slang_root "${CMAKE_BINARY_DIR}/_deps/slang-prebuilt")
    set(_slang_extracted "${_slang_root}/slang-${SLANG_VERSION}")
    set(_slang_archive "${_slang_root}/${_slang_asset}")

    if(NOT EXISTS "${_slang_extracted}/bin/slangc${CMAKE_EXECUTABLE_SUFFIX}")
        message(STATUS "Slang: downloading ${_slang_url}")
        file(MAKE_DIRECTORY "${_slang_extracted}")
        file(DOWNLOAD
            "${_slang_url}"
            "${_slang_archive}"
            SHOW_PROGRESS
            TLS_VERIFY ON
            STATUS _slang_dl_status)
        list(GET _slang_dl_status 0 _slang_dl_code)
        if(NOT _slang_dl_code EQUAL 0)
            file(REMOVE "${_slang_archive}")
            message(FATAL_ERROR "Slang: download failed (${_slang_dl_status}). URL: ${_slang_url}")
        endif()
        file(ARCHIVE_EXTRACT INPUT "${_slang_archive}" DESTINATION "${_slang_extracted}")
    endif()

    # Slang archives sometimes extract directly into bin/lib/include and
    # sometimes wrap in a top-level slang-<ver>/ dir; tolerate both.
    if(EXISTS "${_slang_extracted}/bin/slangc${CMAKE_EXECUTABLE_SUFFIX}")
        set(_slang_slangc "${_slang_extracted}/bin/slangc${CMAKE_EXECUTABLE_SUFFIX}")
    else()
        file(GLOB _slang_found "${_slang_extracted}/*/bin/slangc${CMAKE_EXECUTABLE_SUFFIX}")
        if(_slang_found)
            list(GET _slang_found 0 _slang_slangc)
        else()
            message(FATAL_ERROR "Slang: could not locate slangc inside extracted archive at ${_slang_extracted}")
        endif()
    endif()

    if(UNIX)
        # Some archives drop the exec bit during extraction.
        execute_process(COMMAND chmod +x "${_slang_slangc}")
    endif()

    message(STATUS "Slang: slangc ready at ${_slang_slangc}")
endif()

add_executable(slang::slangc IMPORTED GLOBAL)
set_target_properties(slang::slangc PROPERTIES
    IMPORTED_LOCATION "${_slang_slangc}")
