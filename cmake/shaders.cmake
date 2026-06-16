function(_target_shaders_compile_slang target shader_file include_dirs)
    # cmake_path STEM LAST_ONLY strips only the trailing `.slang`, so
    # `foo.vert.slang` → `foo.vert` and the .spv name matches the convention
    # the runtime loader (`RIProgram::loadShaderStage`) expects.
    cmake_path(GET shader_file STEM LAST_ONLY shader_basename)
    get_filename_component(shader_dir "${shader_file}" DIRECTORY)
    set(out_dir "$<TARGET_FILE_DIR:${target}>/compiled_shaders")
    set(out_file "${out_dir}/${shader_basename}.spv")

    set(_inc_flags "-I${shader_dir}")
    foreach(_d IN LISTS include_dirs)
        list(APPEND _inc_flags "-I${_d}")
    endforeach()

    add_custom_command(
        COMMENT
            "Build Slang shader ${shader_file}"
        MAIN_DEPENDENCY
            "${shader_file}"
        DEPENDS
            $<TARGET_FILE:slang::slangc>
        VERBATIM
        WORKING_DIRECTORY
            "${CMAKE_CURRENT_SOURCE_DIR}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${out_dir}"
        COMMAND
            $<TARGET_FILE:slang::slangc>
                "${shader_file}"
                -target spirv
                -profile sm_6_6
                -emit-spirv-directly
                -fvk-use-entrypoint-name
                -matrix-layout-column-major
                -fvk-use-scalar-layout
                ${_inc_flags}
                -o "${out_file}"
        OUTPUT
            ${shader_basename}.spv
    )
endfunction()

function(target_shaders target)
    cmake_parse_arguments(PARSE_ARGV 1 target_shaders "" "OUTPUT_FOLDER" "SHADERS;INCLUDE_DIRS")
    foreach(shader_file IN LISTS target_shaders_SHADERS)
        target_sources(
            "${target}"
            PRIVATE
                "${shader_file}")
        # cmake_path EXTENSION LAST_ONLY yields ".slang" from "foo.comp.slang";
        # get_filename_component(EXT) would return ".comp.slang" and miss the match.
        cmake_path(GET shader_file EXTENSION LAST_ONLY _ext)
        string(TOLOWER "${_ext}" _ext)
        if(_ext STREQUAL ".slang")
            _target_shaders_compile_slang("${target}" "${shader_file}" "${target_shaders_INCLUDE_DIRS}")
        else()
            message(FATAL_ERROR "target_shaders: unsupported shader '${shader_file}' — only .slang is supported (glslang was removed)")
        endif()
    endforeach()
endfunction()
