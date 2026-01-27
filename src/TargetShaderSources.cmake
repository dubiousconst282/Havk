function(target_shader_sources targetName)
    # Multi value arguments must be either followed by another option,
    # or appear after the source list to break ambiguity.
    set(options PUBLIC PRIVATE)
    set(oneValueArgs BASE_DIR NAMESPACE)
    set(multiValueArgs COMPILE_DEFS INCLUDE_DIRS EXTRA_ARGS)
    cmake_parse_arguments(arg "${options}" "${oneValueArgs}" "${multiValueArgs}" "${ARGN}")

    if (NOT TARGET ${targetName})
        message(FATAL_ERROR "Unknown target ${targetName}")
    endif()

    if (NOT DEFINED ${arg_BASE_DIR})
        set(arg_BASE_DIR ${CMAKE_CURRENT_SOURCE_DIR})
    endif()

    # Gather exported shader include paths from dependencies, recursively
    set(includeDirs ${arg_INCLUDE_DIRS})
    get_property(targetDeps TARGET ${targetName} PROPERTY LINK_LIBRARIES)

    while (NOT "${targetDeps}" STREQUAL "")
        list(POP_BACK targetDeps depName)

        if (TARGET ${depName})
            get_property(publicShaders TARGET ${depName} PROPERTY SHADER_PUBLIC_INCLUDE_DIRS)
            if (DEFINED publicShaders)
                list(APPEND includeDirs "${publicShaders}")
            endif()

            get_property(indirectDeps TARGET ${depName} PROPERTY LINK_LIBRARIES)
            list(APPEND targetDeps ${indirectDeps})
        endif()
    endwhile()
    
    list(REMOVE_DUPLICATES includeDirs)
    list(TRANSFORM includeDirs PREPEND "-I")

    set(outputDir ${CMAKE_CURRENT_BINARY_DIR}/shader-build/)
    set(outputCppFiles ${arg_UNPARSED_ARGUMENTS})
    list(TRANSFORM outputCppFiles REPLACE "^(.+)\.slang$" "${outputDir}\\1.cpp")

    # Generate utility shader target
    set(shaderTargetName "${targetName}-shaders")
    
    if (NOT TARGET ${shaderTargetName})
        add_custom_target(${shaderTargetName})
        add_dependencies(${shaderTargetName} ShaderBuildTool)
        set_target_properties(${shaderTargetName} PROPERTIES ADDITIONAL_CLEAN_FILES ${outputDir})

        add_dependencies(${targetName} ${shaderTargetName})
        if (arg_PUBLIC)
            target_include_directories(${targetName} PUBLIC ${outputDir})
        else()
            target_include_directories(${targetName} PRIVATE ${outputDir})
        endif()
    endif()

    if (DEFINED arg_NAMESPACE)
        set(arg_EXTRA_ARGS "${arg_EXTRA_ARGS};--base-ns;${arg_NAMESPACE}")
    endif()

    if (SHADER_BUILD_DEBUG_INFO)
        set(arg_EXTRA_ARGS "${arg_EXTRA_ARGS};-g2")
    endif()
    if (SHADER_BUILD_OPTIMIZE)
        set(arg_EXTRA_ARGS "${arg_EXTRA_ARGS};-O2")
    endif()
    
    list(TRANSFORM arg_COMPILE_DEFS PREPEND "-D")

    set(shaderBuildArgs 
        --skip-unchanged
        --output-dir ${outputDir}
        ${includeDirs}
        --base-dir ${arg_BASE_DIR}
        ${arg_COMPILE_DEFS}
        # Could also forward global compile defs, but should we?
        # $<LIST:TRANSFORM,$<TARGET_PROPERTY:${targetName},COMPILE_DEFINITIONS>,PREPEND,-D>
        ${arg_EXTRA_ARGS}
        ${arg_UNPARSED_ARGUMENTS}
    )

    # Clean output if arguments have changed
    get_property(havkSourceDir TARGET ShaderBuildTool PROPERTY SOURCE_DIR)
    file(TIMESTAMP "${havkSourceDir}/ShaderBuildTool.cpp" hashKeys)

    string(SHA1 hashKeys "${shaderBuildArgs}-${hashKeys}")
    if (NOT "${SHADER_BRIDGE_${targetName}_CLEAN_HASH}" STREQUAL ${hashKeys})
        file(REMOVE_RECURSE ${outputDir})
        set("SHADER_BRIDGE_${targetName}_CLEAN_HASH" ${hashKeys} CACHE INTERNAL "" FORCE)
        message("Invalidating shader binaries for ${targetName}")
    endif()
    
    # TODO: quote strings with spaces and escape shit
    # TODO: find a better way to save build metadata
    string(JOIN " " shaderReloadArgs ${shaderBuildArgs})
    set(shaderReloadArgs 
        "base_dir: ${arg_BASE_DIR}\n"
        "output_dir: ${outputDir}\n"
        "sources: ${arg_UNPARSED_ARGUMENTS}\n"
        "watcher_cmd: $<TARGET_FILE:ShaderBuildTool> --watch $<JOIN:${shaderReloadArgs}, >\n")
    string(JOIN "" shaderReloadArgs ${shaderReloadArgs})

    file(GENERATE OUTPUT "$<TARGET_FILE:${targetName}>.shaderwatch" 
         CONTENT "${shaderReloadArgs}"
         NEWLINE_STYLE LF)

    add_custom_command(
        TARGET ${shaderTargetName} PRE_BUILD
        COMMAND $<TARGET_FILE:ShaderBuildTool> ${shaderBuildArgs}
        COMMAND_EXPAND_LISTS
        BYPRODUCTS ${outputCppFiles}
    )
    target_sources(${targetName} PRIVATE ${outputCppFiles})
endfunction()
