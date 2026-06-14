# Directory of this list file + the Metal include-flattener, captured at include
# time so the staging step can locate the script regardless of caller scope.
set(_SHADERS_CMAKE_DIR "${CMAKE_CURRENT_LIST_DIR}")
find_package(Python3 COMPONENTS Interpreter REQUIRED)

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

function(_target_shaders_compile_slang_metal target shader_file include_dirs out_var)
    # Slang -> textual Metal Shading Language (.metal). Mirrors the SPIR-V path
    # but drops the Vulkan-only flags (-emit-spirv-directly / -fvk-*) and emits
    # for Apple's metal target. Output basename matches the .spv convention.
    #
    # We deliberately emit TEXTUAL MSL rather than `-target metallib` (Metal
    # Library Bytecode): producing a .metallib requires Apple's offline Metal
    # toolchain (`xcrun metal`/`metallib`), which is a separately-installed
    # component ("xcodebuild -downloadComponent MetalToolchain") and is NOT
    # guaranteed present (and slang's metallib path shells out to it). Instead
    # the Metal RIProgram loads the .metal text at runtime via
    # MTL::Device::newLibrary(NS::String source, ...), which uses the Metal
    # *runtime* compiler shipped with the framework — no offline toolchain
    # needed. Verified: all 48 non-RT Slang shaders in the corpus lower to MSL
    # cleanly; only the 2 DXR .rt.slang passes need a real port (METAL_EXCLUDE).
    # NOTE: these .metal files stage next to the executable and now build with it
    # (target_shaders wires ${target}_metal_shaders as an executable dependency).
    cmake_path(GET shader_file STEM LAST_ONLY shader_basename)
    get_filename_component(shader_dir "${shader_file}" DIRECTORY)
    # Concrete (non-generator-expression) path so it is valid in the custom
    # command OUTPUT and the aggregate custom target's DEPENDS. Nothing consumes
    # these at runtime yet (the Metal RIProgram loader is a later phase).
    set(out_dir "${CMAKE_CURRENT_BINARY_DIR}/compiled_shaders")
    set(out_file "${out_dir}/${shader_basename}.metal")
    # Dual-emit: a SPIR-V sibling is produced alongside the MSL purely so the
    # Metal RIProgram::initialize() can run the SAME spirv_reflect path the
    # Vulkan backend uses to derive descriptor-set / argument-buffer layout +
    # vertex-input + push-constant reflection (Metal has no equivalent of the
    # SPIR-V reflection lib in-tree). The .spv is reflection-only; execution
    # uses the .metal via runtime newLibrary().
    set(refl_file "${out_dir}/${shader_basename}.spv")

    set(_inc_flags "-I${shader_dir}")
    foreach(_d IN LISTS include_dirs)
        list(APPEND _inc_flags "-I${_d}")
    endforeach()

    add_custom_command(
        COMMENT
            "Build Slang->Metal (+reflection .spv) shader ${shader_file}"
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
                -target metal
                -profile sm_6_6
                -matrix-layout-column-major
                -DSLANG_METAL=1
                ${_inc_flags}
                -o "${out_file}"
        COMMAND
            $<TARGET_FILE:slang::slangc>
                "${shader_file}"
                -target spirv
                -profile sm_6_6
                -emit-spirv-directly
                -fvk-use-entrypoint-name
                -matrix-layout-column-major
                -fvk-use-scalar-layout
                -DSLANG_METAL=1
                ${_inc_flags}
                -o "${refl_file}"
        OUTPUT
            "${out_file}"
            "${refl_file}"
    )
    set(${out_var} "${out_file}" PARENT_SCOPE)
endfunction()

function(_target_shaders_copy_metal target metal_file out_var)
    # Stage a hand-written .metal (the passes Slang can't lower to Metal — the DXR
    # ray-tracing shaders in METAL_EXCLUDE) into the same compiled_shaders/ output
    # dir as the Slang->MSL path, by file copy (no slangc). Metal compiles the
    # .metal text at runtime via MTL::Device::newLibrary, so no offline toolchain.
    cmake_path(GET metal_file FILENAME _fname)
    set(out_dir "${CMAKE_CURRENT_BINARY_DIR}/compiled_shaders")
    set(out_file "${out_dir}/${_fname}")
    # FLATTEN quoted #includes into one self-contained file (newLibrary has no
    # include support). This lets the hand-written .metal share .metalh headers
    # mirroring the Slang modules + slang/Constants.h instead of re-declaring the
    # shared structs/utilities per file. Search roots mirror the slang import
    # paths: metal/ (the .metalh tree) and slang/ (Constants.h / HostDefinitions.h).
    file(GLOB_RECURSE _metalh CONFIGURE_DEPENDS
         "${CMAKE_CURRENT_SOURCE_DIR}/metal/*.metalh")
    add_custom_command(
        COMMENT
            "Stage + flatten hand-written Metal shader ${metal_file}"
        MAIN_DEPENDENCY
            "${CMAKE_CURRENT_SOURCE_DIR}/${metal_file}"
        DEPENDS
            ${_metalh}
            "${_SHADERS_CMAKE_DIR}/flatten_metal_includes.py"
        VERBATIM
        COMMAND ${CMAKE_COMMAND} -E make_directory "${out_dir}"
        COMMAND ${Python3_EXECUTABLE}
                "${_SHADERS_CMAKE_DIR}/flatten_metal_includes.py"
                "${CMAKE_CURRENT_SOURCE_DIR}/${metal_file}"
                "${out_file}"
                "${CMAKE_CURRENT_SOURCE_DIR}/metal"
                "${CMAKE_CURRENT_SOURCE_DIR}/slang"
        OUTPUT
            "${out_file}"
    )
    set(${out_var} "${out_file}" PARENT_SCOPE)
endfunction()

function(target_shaders target)
    cmake_parse_arguments(PARSE_ARGV 1 target_shaders "" "OUTPUT_FOLDER" "SHADERS;INCLUDE_DIRS;METAL_EXCLUDE;METAL_SOURCES")

    # Metal backend: compile the Slang shaders that Slang's (experimental) Metal
    # target supports to .metal, skipping the ones listed in METAL_EXCLUDE
    # (currently the bindless / DXR ray-tracing passes Slang can't yet lower).
    # The outputs are grouped under a "${target}_metal_shaders" custom target that
    # is wired as a dependency of the executable (below), so the shaders build
    # whenever the executable does — the same effect as the SPIR-V path, which
    # attaches each shader to the target via target_sources. (We use a custom
    # target + add_dependencies rather than target_sources(.metal) because CMake/
    # Xcode would otherwise try to compile the .metal with the offline Metal
    # toolchain; these are loaded at runtime via MTL::Device::newLibrary instead.)
    if(ENABLE_METAL)
        set(_metal_outputs "")
        set(_n_skipped 0)
        foreach(shader_file IN LISTS target_shaders_SHADERS)
            cmake_path(GET shader_file EXTENSION LAST_ONLY _ext)
            string(TOLOWER "${_ext}" _ext)
            if(NOT _ext STREQUAL ".slang")
                continue()
            endif()
            if(shader_file IN_LIST target_shaders_METAL_EXCLUDE)
                math(EXPR _n_skipped "${_n_skipped} + 1")
                continue()
            endif()
            _target_shaders_compile_slang_metal("${target}" "${shader_file}" "${target_shaders_INCLUDE_DIRS}" _out)
            list(APPEND _metal_outputs "${_out}")
        endforeach()
        list(LENGTH _metal_outputs _n_built)
        # Hand-written Metal sources (the METAL_EXCLUDE ray-tracing passes ported by
        # hand) are staged verbatim into compiled_shaders/ alongside the MSL.
        set(_n_handwritten 0)
        foreach(metal_file IN LISTS target_shaders_METAL_SOURCES)
            _target_shaders_copy_metal("${target}" "${metal_file}" _out)
            list(APPEND _metal_outputs "${_out}")
            math(EXPR _n_handwritten "${_n_handwritten} + 1")
        endforeach()
        if(_metal_outputs)
            add_custom_target(${target}_metal_shaders DEPENDS ${_metal_outputs})
            # Metal now links, so stage the shaders next to the executable
            # whenever it builds (mirrors the SPIR-V path attaching shaders to
            # the target). ${target} is the executable created before this call.
            add_dependencies(${target} ${target}_metal_shaders)
        endif()
        message(STATUS "target_shaders(${target}) METAL: compiling ${_n_built} Slang->Metal shader(s) + ${_n_handwritten} hand-written Metal shader(s), skipping ${_n_skipped} listed in METAL_EXCLUDE")
        return()
    endif()

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
            message(FATAL_ERROR "target_shaders(${target}): unsupported shader '${shader_file}' — only .slang is supported (glslang was removed).")
        endif()
    endforeach()
endfunction()
