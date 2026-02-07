#include <Havk/Havk.h>
#include <Havx/MainWindow.h>
#include <Havx/Camera.h>
#include <Havx/ShaderDebugTools.h>

#include "Shaders/ShadebugDemo.h"
using namespace havk::vectors;

int main(int argc, const char** args) {
    // Create GLFW window
    auto window = havx::MainWindow("Shadebug Demo");

    // Create Vulkan device context with WSI extensions as requested by GLFW
    auto devicePars = havk::DeviceCreateParams { .EnableDebugExtensions = true };
    window.GetDeviceCreationParams(devicePars);
    auto device = havk::CreateContext(devicePars);

    // Create swapchain
    auto swapchain = window.CreateSwapchain(*device);
    swapchain->SetPreferredFormats({ { VK_FORMAT_B8G8R8A8_UNORM } },
                                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    window.SetFrameRateLimit(havx::MainWindow::kFrameLimitMonitor, swapchain.get());

    // Create ImGui context and rendering backend
    window.CreateOverlay(*swapchain);

    havx::Shadebug::Initialize(device.get());

    // Create renderer stuff
    auto camera = havx::Camera { .Position = { 0, 1, 5 }, .MoveSpeed = 10.0 };
    havk::ImagePtr colorBuffer, depthBuffer, normalBuffer;

    auto storageBuffer = device->CreateBuffer(1024 * 1024, havk::BufferFlags::DeviceMem);

    // Main loop
    window.RunLoop(*swapchain, [&](havk::Image& frame, havk::CommandList& cmds) {
        if (depthBuffer == nullptr || depthBuffer->Size != frame.Size) {
            colorBuffer = device->CreateImage({
                .Format = VK_FORMAT_R8G8B8A8_UNORM,
                .Usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                         VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                .Size = frame.Size,
            });
            depthBuffer = device->CreateImage({
                .Format = VK_FORMAT_D32_SFLOAT,
                .Usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                .Size = frame.Size,
            });
            normalBuffer = device->CreateImage({
                .Format = VK_FORMAT_A2R10G10B10_UNORM_PACK32,
                .Usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                .Size = frame.Size,
            });
        }
        havx::Shadebug::NewFrame(cmds);
        camera.Update(havx::Camera::GetInputsFromImGui());
        float4x4 projMat = camera.GetProjMatrix() * camera.GetViewMatrix(false);
        float4x4 invProjMat = havx::GetInverseScreenProjMatrix(projMat, frame.Size);

        cmds.Dispatch<CS_DebugToolsDemo>({ 1000000, 1, 1 }, {
            .storage = *storageBuffer,
            .colorBuffer = *colorBuffer,
            .normalBuffer = *normalBuffer,
            .depthBuffer = *depthBuffer,
            .viewOrigin = camera.ViewPosition,
            .invProj = invProjMat,
        });

        // Draw debug frame
        cmds.Barrier({ .SrcStages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, .DstStages = VK_PIPELINE_STAGE_2_CLEAR_BIT });
        cmds.ClearColorImage(*normalBuffer, { 0, 0, 0, 0 });
        cmds.ClearDepthImage(*depthBuffer, { 0 });

        havx::Shadebug::DrawFrame(cmds, {
            .ProjMat = projMat,
            .ViewOrigin = camera.ViewPosition,
            .ColorBuffer = colorBuffer.get(),
            .NormalBuffer = normalBuffer.get(),
            .DepthBuffer = depthBuffer.get(),
            .DepthCompareOp = VK_COMPARE_OP_GREATER,
            .PickImage = colorBuffer.get(),
        });

        // Compose
        cmds.Barrier({ .SrcStages = VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, .DstStages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT });
        cmds.Dispatch<CS_ComposeDebug>(frame.Size, {
            .colorBuffer = *colorBuffer,
            .normalBuffer = *normalBuffer,
            .depthBuffer = *depthBuffer,
            .viewOrigin = camera.ViewPosition,
            .invProj = invProjMat,
        });

        // Save result for next frame
        VkImageBlit copyRegion = {
            .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .srcOffsets = { { 0, 0, 0 }, { (int)frame.Size.x, (int)frame.Size.y, (int)frame.Size.z } },
            .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .dstOffsets = { { 0, 0, 0 }, { (int)frame.Size.x, (int)frame.Size.y, (int)frame.Size.z } },
        };
        cmds.Barrier({ .SrcStages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, .DstStages = VK_PIPELINE_STAGE_2_BLIT_BIT });
        vkCmdBlitImage(cmds.Handle, colorBuffer->Handle, VK_IMAGE_LAYOUT_GENERAL, frame.Handle, VK_IMAGE_LAYOUT_GENERAL, 1, &copyRegion, VK_FILTER_NEAREST);
    });
    havx::Shadebug::Shutdown();
    return 0;
}