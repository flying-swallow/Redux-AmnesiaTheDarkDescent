macro(set_output_dir name dir)
    foreach (OUTPUTCONFIG ${CMAKE_CONFIGURATION_TYPES})
        string(TOUPPER ${OUTPUTCONFIG} OUTPUTCONFIGUPPERCASE)
        set_target_properties(${name} PROPERTIES RUNTIME_OUTPUT_DIRECTORY_${OUTPUTCONFIGUPPERCASE} ${CMAKE_BINARY_DIR}/amnesia/${OUTPUTCONFIG}/${dir})
        set_target_properties(${name} PROPERTIES LIBRARY_OUTPUT_DIRECTORY_${OUTPUTCONFIGUPPERCASE} ${CMAKE_BINARY_DIR}/amnesia/${OUTPUTCONFIG}/${dir})
        set_target_properties(${name} PROPERTIES ARCHIVE_OUTPUT_DIRECTORY_${OUTPUTCONFIGUPPERCASE} ${CMAKE_BINARY_DIR}/amnesia/${OUTPUTCONFIG}/${dir})
    endforeach()

    set_target_properties(${name} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/amnesia/${dir})
    set_target_properties(${name} PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/amnesia/${dir})
    set_target_properties(${name} PROPERTIES ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/amnesia/${dir})

    set_property(TARGET ${name} PROPERTY IMPORTED_NO_SONAME TRUE)
endmacro()

# Apply set_output_dir to a target and each of its direct LINK_LIBRARIES that
# resolve to actual targets (mirrors warfork's qf_set_output_dir_dep). Useful
# when you don't want to manually mark every transitive runtime artifact.
macro(set_output_dir_dep name dir)
    set_output_dir(${name} "${dir}")
    get_target_property(_dependencies ${name} LINK_LIBRARIES)
    if (_dependencies)
        foreach(_dep ${_dependencies})
            if (TARGET ${_dep})
                get_target_property(_aliased ${_dep} ALIASED_TARGET)
                if (_aliased)
                    set(_dep ${_aliased})
                endif()
                get_target_property(_imported ${_dep} IMPORTED)
                if (NOT _imported)
                    set_output_dir(${_dep} "${dir}")
                endif()
            endif()
        endforeach()
    endif()
endmacro()


