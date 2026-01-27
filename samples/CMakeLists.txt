CPMAddPackage("gh:nothings/stb#master")
CPMAddPackage("gh:jkuhlmann/cgltf#v1.15")

set(SHADER_BUILD_DEBUG_INFO TRUE)

add_executable(Sample_HelloCompute HelloCompute.cpp)
target_link_libraries(Sample_HelloCompute PRIVATE havk)
target_shader_sources(Sample_HelloCompute PRIVATE Shaders/HelloCompute.slang)

add_executable(Sample_HelloTriangle HelloTriangle.cpp)
target_link_libraries(Sample_HelloTriangle PRIVATE havk havk::extensions)
target_shader_sources(Sample_HelloTriangle PRIVATE Shaders/HelloTriangle.slang)

add_executable(Sample_ShadebugDemo ShadebugDemo.cpp)
target_link_libraries(Sample_ShadebugDemo PRIVATE havk havk::extensions)
target_shader_sources(Sample_ShadebugDemo PRIVATE Shaders/ShadebugDemo.slang)

add_executable(Sample_GpuDrivenRendering GpuDrivenRendering.cpp Model.cpp StbImage.cpp)
target_link_libraries(Sample_GpuDrivenRendering PRIVATE havk havk::extensions)
target_include_directories(Sample_GpuDrivenRendering PRIVATE ${stb_SOURCE_DIR} ${cgltf_SOURCE_DIR})
target_shader_sources(Sample_GpuDrivenRendering
    COMPILE_DEFS ENABLE_RAY_TRACING=1
    NAMESPACE shader
    PRIVATE Shaders/ModelRender.slang
)