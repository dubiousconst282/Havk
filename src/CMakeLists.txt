add_library(
    havk STATIC
    Havk/Havk.cpp
    Havx/SystemUtils.cpp
)
add_library(havk::havk ALIAS havk)
target_compile_features(havk PUBLIC cxx_std_20)
target_include_directories(havk PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(havk PRIVATE
        "-Wall" "-Wextra" "-Wsign-conversion" "-Wconversion" "-Wno-string-conversion"
        "-Wno-unused-parameter" "-Wno-missing-designated-field-initializers" "-Wno-missing-field-initializers"
        "-Wno-missing-braces" "-Wno-unused-value"
    )
endif()

target_link_libraries(havk PUBLIC
    Vulkan::Vulkan
    GPUOpen::VulkanMemoryAllocator
    glm::glm
)
target_compile_definitions(havk PUBLIC
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_FORCE_XYZW_ONLY
)

set_target_properties(havk PROPERTIES SHADER_PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/Shaders")

add_executable(
    ShaderBuildTool
    Havk/ShaderBuildTool.cpp
)
target_link_libraries(ShaderBuildTool PRIVATE slang::slang havk)

if (WIN32)
    add_custom_command(TARGET ShaderBuildTool POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:ShaderBuildTool> $<TARGET_FILE_DIR:ShaderBuildTool>
        COMMAND_EXPAND_LISTS
    )
endif()

include(TargetShaderSources.cmake)

if (HAVK_ENABLE_GFX_EXTENSIONS)
    add_library(
        havk_extensions STATIC

        Havx/Yson.cpp
        Havx/PerfMonitor.cpp
        Havx/ShaderDebugTools.cpp

        # TODO: downport DataIO to C++20 and re-add to exts
        # Havx/DataIO.cpp
    )
    add_library(havk::extensions ALIAS havk_extensions)
    target_link_libraries(havk_extensions PUBLIC havk imgui)

    target_shader_sources(
        havk_extensions
        NAMESPACE havx::shader PUBLIC
        Shaders/Havk/DrawImGui.slang
        Shaders/Havk/DebugTools.slang
    )
endif()

if (WIN32)
    target_compile_definitions(ShaderBuildTool PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_definitions(havk PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_definitions(havk_extensions PRIVATE _CRT_SECURE_NO_WARNINGS _USE_MATH_DEFINES)
endif()