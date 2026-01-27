// Slightly fancier hello triangle using havk extension libraries.
// This adds an ImGui overlay, arcball control, and MSAA.
#include <Havk/Havk.h>
#include <Havx/MainWindow.h>
#include <Havx/Camera.h>

#include "Shaders/HelloTriangle.h"
using namespace havk::vectors;

int main(int argc, const char** args) {
    // Create GLFW window
    auto window = havx::MainWindow("Hello Triangle");

    // Create Vulkan device context with WSI extensions as requested by GLFW
    // By default, this will pick the first device with support for a minimum set of features.
    auto devicePars = havk::DeviceCreateParams { .EnableDebugExtensions = true };
    window.GetDeviceCreationParams(devicePars);
    auto device = havk::CreateContext(devicePars);

    // Create swapchain
    auto swapchain = window.CreateSwapchain(*device);
    swapchain->SetPreferredFormats({ { VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR } }, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    window.SetFrameRateLimit(havx::MainWindow::kFrameLimitMonitor, swapchain.get());

    // Create ImGui context and rendering backend
    window.CreateOverlay(*swapchain);

    // Create render pipeline
    auto depthlessState = havk::GraphicsPipelineState {
        .Raster = { .FrontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .CullFace = VK_CULL_MODE_NONE },
        .Depth = { .TestOp = VK_COMPARE_OP_ALWAYS },
        .MSAA = { .NumSamples = 4 },
    };
    auto attachmentLayout = havk::AttachmentLayout { .Formats = { swapchain->GetFormat() } };
    auto pipeline = device->CreateGraphicsPipeline({ VS_Triangle::Module, FS_Triangle::Module }, depthlessState, attachmentLayout);

    // Upload vertices
    // Larger buffers will typically need staging for optimal performance,
    // since DEVICE_LOCAL memory may not be mappeable without ReBAR or UMA.
    const Vertex vertices[] = {
        { float2(-0.5f, +0.5f), 0xFF0000 },
        { float2(+0.0f, -0.5f), 0x00FF00 },
        { float2(+0.5f, +0.5f), 0x0000FF },
    };
    auto vertexBuffer = device->CreateBuffer(sizeof(vertices), havk::BufferFlags::DeviceMem | havk::BufferFlags::MapSeqWrite);
    vertexBuffer->Write(0, vertices, 3);

    // Create renderer stuff
    auto camera = havx::Camera { .ArcDistance = 1.0, .Mode = havx::Camera::Arcball, .MoveSpeed = 1.0 };
    havk::ImagePtr msaaColorBuffer;

    // Main loop
    window.RunLoop(*swapchain, [&](havk::Image& frame, havk::CommandList& cmds) {
        if (msaaColorBuffer == nullptr || msaaColorBuffer->Size != frame.Size) {
            msaaColorBuffer = device->CreateImage({
                .Format = swapchain->GetFormat(),
                .Usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                .Size = frame.Size,
                .NumSamples = 4,
            });
        }
        camera.Update(havx::Camera::GetInputsFromImGui());

        // Swapchain synchronization is implicit, so we don't need a barrier here.
        cmds.BeginRendering({
            .Attachments = { {
                .Target = *msaaColorBuffer,
                .LoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                .StoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                .ClearValue = { 0.5, 0.5, 0.5, 1 },
                .ResolveTarget = &frame,
            } },
        });
        cmds.Draw(*pipeline, { .NumVertices = 3 }, VS_Triangle::Params {
            .vertices = *vertexBuffer,
            .projMat = camera.GetProjMatrix() * camera.GetViewMatrix(true),
        });
        cmds.EndRendering();

        ImGui::SetNextWindowCollapsed(true, ImGuiCond_FirstUseEver);
        ImGui::ShowDemoWindow();
    });
    return 0;
}
