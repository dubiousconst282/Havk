# Havk
Havk is a minimalistic abstraction layer and framework for Vulkan. It is primarily intended for smaller applications and prototypes, where quick setup and simplicity are desired - hence "HAcKable Vulkan".

## Main features
- First class support for Slang shaders: CMake integration, binding generation, hot-reloading
- Vulkan 1.3 baseline, with optional support for extensions such as ray tracing and mesh shaders
- Bindless resources with no descriptor set management
- Automatic resource disposal with RAII and deletion queues
- Headless device contexts, portable windowing integration

### Utility extensions
These are exposed in CMake target `havk::extensions` when `set(HAVK_ENABLE_GFX_EXTENSIONS TRUE)`.

- [Havx/MainWindow.h](./src/Havx/MainWindow.h): Windowing helpers for GLFW and ImGui
- [Havx/ImGuiRenderer.h](./src/Havx/ImGuiRenderer.h): ImGui rendering backend
- [Havx/Camera.h](./src/Havx/Camera.h): Standalone first-person/arcball camera
- [Havx/ShaderDebugTools.h](./src/Havx/ShaderDebugTools.h): Immediate mode shape drawing and input widgets for shaders
- [Havx/PerfMonitor.h](./src/Havx/PerfMonitor.h): Embedded CPU/GPU profiler for manually instrumented scopes

## Usage guide
The following is a minimal example showing how to create a window and write pixels to it via a compute shader. Build requires the Vulkan SDK, CMake, and a C++20 compiler (tested with Clang and MSVC, Windows and Linux).

```cpp
// Main.cpp
#include <Havk/Havk.h>
#include <Havx/MainWindow.h>
#include "Shaders/Main.h"

int main(int argc, const char** args) {
    auto window = havx::MainWindow("Minimal Demo");

    auto devicePars = havk::DeviceCreateParams { .EnableDebugExtensions = true };
    window.GetDeviceCreationParams(devicePars);
    auto device = havk::CreateContext(devicePars);

    auto swapchain = window.CreateSwapchain(*device);
    swapchain->SetPreferredFormats({ { VK_FORMAT_R8G8B8A8_UNORM } }, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    window.CreateOverlay(*swapchain);

    window.RunLoop(*swapchain, [&](havk::Image& backbuffer, havk::CommandList& cmds) {
        cmds.Dispatch<CS_Mandelbrot>(backbuffer.Size, {
            .image = backbuffer,
            .region = { -0.76731801, -0.09786693, -0.764549314, -0.0956518277 },
        });
        ImGui::ShowMetricsWindow();
    });
    return 0;
}

// Shaders/Main.slang
import Havk.Core;

[numthreads(8, 8)]
void CS_Mandelbrot(uniform ImageHandle2D<float4> image, uniform float4 region, uint2 tid: SV_DispatchThreadID) {
    float2 c = lerp(region.xy, region.zw, tid / float2(image.Size));
    float2 z = 0;
    int i = 0;

    while (i++ < 600 && dot(z, z) < 64) {
        z = float2(z.x * z.x - z.y * z.y, 2 * z.x * z.y) + c;
    }
    float s = i < 600 ? i - log2(log2(dot(z, z))) : 0;
    float3 color = abs(sin(s * float3(0.02, 0.04, 0.08)));
    image.Store(tid, float4(color, 1));
}

// CMakeLists.txt
cmake_minimum_required(VERSION 3.28)

set(HAVK_ENABLE_GFX_EXTENSIONS TRUE)

include(FetchContent)
FetchContent_Declare(
    havk
    GIT_REPOSITORY "https://github.com/dubiousconst282/Havk"
    GIT_TAG        "805443363e1c5cd3a4067831f4c1141bca2c0780"
)
FetchContent_MakeAvailable(havk)

add_executable(SampleApp Main.cpp)
target_link_libraries(SampleApp PRIVATE havk::havk havk::extensions)
target_shader_sources(SampleApp PRIVATE Shaders/Main.slang)

// Build
cmake -S . -B ./build -G Ninja -DCMAKE_CXX_COMPILER=clang++
cmake --build ./build
```

### Shader compilation and bindings
Shader source files can be specified in CMake through `target_shader_sources()`. This will create a pairing target like `SampleApp-shaders` that invokes a custom command to rebuild shaders as necessary. The following parameters are currently supported:

```cmake
set(SHADER_BUILD_DEBUG_INFO TRUE)   # Emit non-semantic debug info (= slangc -g2)
set(SHADER_BUILD_OPTIMIZE TRUE)     # Pass generated modules through spirv-opt (= slangc -O2)

target_shader_sources(SampleApp
    NAMESPACE shader                # [optional] Prefix namespace for all generated definitions
    INCLUDE_DIRS ${pathA} ${pathB}  # [optional] Additional module/include search paths
    COMPILE_DEFS ENABLE_FOO=1       # [optional] Additional preprocessor definitions
    PUBLIC|PRIVATE                  # Enable/disable exporting of generated shader headers to linking targets

    Shaders/Mesh.slang
    Shaders/Lighting.slang
)
# Export additional include directories to linking targets
set_target_properties(SampleApp PROPERTIES SHADER_PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/Shaders")
```

Binding headers are auto-generated as part of the build, similarly to [sokol-shdc](https://github.com/floooh/sokol-tools/blob/master/docs/sokol-shdc.md). This facilitates sharing of struct definitions, embedding of SPIR-V binaries, and hot-reloading.

```cpp
// Source
[numthreads(64)]
void CS_ComputeHello(uniform float* data, uint tid: SV_DispatchThreadID);

// Auto-generated
struct CS_ComputeHello {
    struct Params {
        havk::DevicePtr<float> data;
    };
    static const havk::ModuleDesc Module; // defined in a separate CU to avoid cascading recompiles
    static constexpr havk::vectors::uint3 GroupSize = { 64, 1, 1 };
    using SpecConst = void;
};

// Library template
template<ComputeProgramShape TCompute>
void Dispatch(vectors::uint3 numCeilInvocs, const TCompute::Params& pc) {
    auto numGroups = (numCeilInvocs + TCompute::GroupSize - 1u) / TCompute::GroupSize;
    // vkCmdBindPipeline + vkCmdPushConstants + vkCmdDispatch
    DispatchGroups(*Context->GetProgram<TCompute>(), numGroups, pc);
}
```

The more usual creation process is required for graphics pipelines or setting extra parameters:

```cpp
auto renderState = havk::GraphicsPipelineState {
    .Raster = { .FrontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .CullFace = havk::dynamic_state /* vkCmdSetCullMode() */ },
    .Depth = { .TestOp = VK_COMPARE_OP_ALWAYS },
    .Blend = { .AttachmentStates = { havk::ColorBlendingState::kSrcOver } },
    .MSAA = { .NumSamples = 4 },
};
auto attachmentLayout = havk::AttachmentLayout { .Formats = { swapchain->GetFormat() } };
auto pipeline = device->CreateGraphicsPipeline({ VS_Main::Module, FS_Main::Module }, renderState, attachmentLayout, specMap);
```

```cpp
auto pipeline = device->CreateComputePipeline(
    CS_ComputeHello::Module,
    CS_ComputeHello::SpecConst { .kEnableFancyMode = true });
```

```cpp
auto spirvModule = havk::ModuleDesc {
    .Code = (uint32_t*)spirvData.data(),
    .CodeSize = (uint32_t)spirvData.size(),
    .EntryPoint = "main"
};
auto specMap = havk::SpecConstMap {};
specMap.Add(targetConstId, 4.0f);
auto pipeline = device->CreateComputePipeline(spirvModule, specMap);
```

> [!NOTE]
> - Proper integration with slangd may require search paths from linked dependencies to be specified manually:
>    ```jsonc
>     // .vscode/settings.json
>     {
>         "slang.additionalSearchPaths": [
>             "./build/deps/havk/src/Shaders/",
>         ],
>         // or: "slang.searchInAllWorkspaceDirectories": true
>     }
>     ```
>
> - Shaders are compiled to read matrices in column-major order by default to more closely match GLM's conventions, but this doesn't fully solve the conflicting definitions between Slang: `matCxR` vs `floatRxC`. Specifically, copying non-square matrices from/to shaders must be done with a memcpy or std::bit_cast, from e.g. `mat4x3` to `mat3x4`, to prevent contents from being garbled.

### Resource ownership and lifetimes
All resources have unique ownership and implicit cleanup through RAII. A deletion queue is used to delay destruction of underlying objects until completion of all command list submissions with overlapping lifetimes, and it needs to be flushed periodically (every frame if not using `MainWindow::RunLoop()`) via a call to `DeviceContext::GarbageCollect()`.

Immediate destruction is supported through `havk::Destroy()`. This is useful when the queue delay is known to be unnecessary or problematic, e.g. when forcing immediate execution via `Submit().Wait()` while some other command list is in recording state.

### Bindless
Shaders can only reference resources through descriptor handles (indices to a global heap) or buffer device pointers, bindings are not supported directly. Descriptors are allocated for any image or sub-view created with SAMPLED and/or STORAGE usages:

```cpp
auto texture = _device->CreateImage({
    .Format = VK_FORMAT_R8G8B8A8_SRGB,
    .Usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
    .Size = { 2048, 2048, 1 },
});
auto stagingBuffer = _device->CreateBuffer(imageDataSize, havk::BufferFlags::HostMem_SeqWrite);
stagingBuffer->Write(0, imageData, imageDataSize);
cmds.CopyBufferToImage({ .SrcData = *stagingBuffer, .DstImage = *texture, .DstStages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT });

cmds.Dispatch<CS_ImageAccess101>(backbuffer->Size, { .sampledImage = *texture, ... });

// Slang
import Havk.Core;

[numthreads(8, 8)]
void CS_ImageAccess101(
    uint2 tid: SV_DispatchThreadID,
    uniform TextureHandle2D<float4> sampledImage,
    uniform ImageHandle2D<float4> storageImage, ...)
{
    if (any(tid >= storageImage.Size)) return;

    let sampler = havk::GetSampler(havk::FilterMode::Linear);
    float4 color = sampledImage.SampleLevel(sampler, texCoord, 0);
    storageImage.Store(tid, color);
}
```

> [!NOTE]
> - Shader texture accesses are assumed to be uniform by default. `.NonUniformInstance` must be used in case textures can vary within invocations of a [single draw command or compute workgroup](https://docs.vulkan.org/samples/latest/samples/extensions/descriptor_indexing/README.html#_when_to_use_non_uniform_indexing_qualifier) (typically when indexing material or texture arrays).
>
> - Shader `GetSampler()` only supports a subset of parameters. Custom descriptor handles can be created through `DeviceContext.DescriptorHeap.GetSampler({ ... })`.

### Synchronization
Havk does not automate synchronization, but makes some guarantees/compromises around image layouts in favor of KHR_unified_image_layouts: images are assumed to always be in GENERAL layout, so only global barriers are necessary for correctness. They are implicitly transitioned from UNDEFINED during creation via a prologue command buffer, and to PRESENT_SRC prior to Present().

Although negligible at smaller scales, this may have a [non-zero cost](https://youtu.be/0rqWe1M2HiE) on current GPU architectures, as global barriers can force [some drivers](https://gitlab.freedesktop.org/mesa/mesa/-/blob/25.3/src/amd/vulkan/radv_cmd_buffer.c?ref_type=heads#L7313) to trigger unnecessary cache flushes due to ambiguity.

### Buffer spans
`BufferSpan<T>` is a tuple of `(buffer, offset, length)` providing type safety over buffer memory ranges, analogous to memory spans/slices in various programming languages. Apart from the more characteristic use patterns, it is mainly useful for bump-allocation of slices from a single buffer:

```cpp
auto scratchBuffer = device->CreateBuffer(UINT32_MAX, havk::BufferFlags::DeferredAlloc);
auto span = scratchBuffer->Slice<uint8_t>();
auto context = span.bump_slice<shader::DispatchContext>(1);  // context = span[0..n*sizeof]; span = span[n*sizeof+align..]
auto instances = span.bump_slice<shader::InstanceData>(maxInstances);
span.commit_bump_alloc(havk::BufferFlags::HostSeqWrite); // alloc buffer with `size = span.offset_bytes()`
```

### Shader Debug Tools / Shadebug
Shadebug is a proxy for emitting ImGui input widgets and simple shapes from shaders. It is occasionally useful for creating live debug visualizations and tweaking one-off parameters. The screenshot below demonstrates some of the available features, [all generated in shader code](./samples/Shaders/ShadebugDemo.slang):

<img width="800" alt="shaderbug_demo1" src="https://github.com/user-attachments/assets/66300428-6470-42c0-84be-4b8865a5abfd" />

Host integration is straightforward. All shader debug functions are guarded by specialization constants, allowing them to be folded off during driver compilation and avoiding runtime performance impact when not in use. Still, a large amount of calls may bloat shader binaries and increase compile times in both Slang frontend and driver, so use of #if macros may be preferred.

```cpp
#include <Havx/ShaderDebugTools.h>

void App::Initialize(...) {
    // Call this before creation of any pipelines using debug tools.
    if (enableShadebug) havx::Shadebug::Initialize(deviceCtx);
}
App::~App() {
    havx::Shadebug::Shutdown();
}
void App::RenderFrame(...) {
    havx::Shadebug::NewFrame(cmdList);

    // TODO: Invoke shaders and stuff...

    havx::Shadebug::DrawFrame(cmdList, {
        // These can all be omitted if only using widgets
        .ProjMat = camera.GetProjMatrix() * camera.GetViewMatrix(false /* no translation */),
        .ViewOrigin = camera.ViewPosition,
        .ColorBuffer = colorBuffer.get(),   // flat albedo output
        .NormalBuffer = normalBuffer.get(), // optional
        .DepthBuffer = depthBuffer.get(),   // optional
        .DepthCompareOp = VK_COMPARE_OP_GREATER,
        .PickImage = &backbuffer,           // optional; must have SAMPLED usage
    });

    // TODO: Optionally shade and compose output buffers on top of final image...
}
```

## Dependencies
The CMake script currently requires the Vulkan headers and loader to be installed at system level (via Vulkan SDK or package manager). All other dependencies are pulled as necessary via CPM.

- [Slang](https://shader-slang.org)
- [VulkanMemoryAllocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator)
- [GLM](https://github.com/g-truc/glm)
- [GLFW](https://www.glfw.org/)*
- [ImGui](https://github.com/ocornut/imgui)*
- [ImPlot](https://github.com/epezent/implot)*

*optional, can be disabled by setting `HAVK_ENABLE_GFX_EXTENSIONS=FALSE`, `HAVK_ENABLE_GLFW=FALSE`.

On Linux, some additional packages may be required to [build GLFW](https://www.glfw.org/docs/latest/compile.html), if it is not already installed.
