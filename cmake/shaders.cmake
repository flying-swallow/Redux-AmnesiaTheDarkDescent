function(target_shaders target)
    cmake_parse_arguments(PARSE_ARGV 1 target_shaders "" "OUTPUT_FOLDER" "SHADERS")
    foreach(shader_file IN LISTS target_shaders_SHADERS)
        target_sources(
            "${target}"
            PRIVATE
                "${shader_file}")
        get_filename_component(shader_output_name ${shader_file} NAME)
        cmake_path(
            REPLACE_EXTENSION
                shader_file
            LAST_ONLY
            ".spv"
            OUTPUT_VARIABLE
                output_file)

        add_custom_command(
            COMMENT
                "Build shader file ${shader_file}"
            MAIN_DEPENDENCY
                "${shader_file}"
            DEPENDS
                glslang-standalone
            VERBATIM
            WORKING_DIRECTORY
                "${CMAKE_CURRENT_SOURCE_DIR}"
            COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:${target}>/compiled_shaders
            COMMAND
                $<TARGET_FILE:glslang-standalone> -V ${shader_file} -o $<TARGET_FILE_DIR:${target}>/compiled_shaders/${shader_output_name}.spv
            OUTPUT
                ${shader_output_name}.spv
        )
    endforeach()
endfunction()


