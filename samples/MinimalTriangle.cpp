// Minimal hello triangle using only GLFW and plain Havk.
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <Havk/Havk.h>

#include "Shaders/HelloTriangle.h"
using namespace havk::vectors;

int main(int argc, const char** args) {
    // Create GLFW window
    glfwInit();
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    auto window = glfwCreateWindow(1280, 720, "Minimal Vulkan Triangle", NULL, NULL);

    // Create Vulkan device context with WSI extensions as requested by GLFW
    uint32_t numGlfwReqExts;
    auto glfwReqExts = glfwGetRequiredInstanceExtensions(&numGlfwReqExts);

    auto device = havk::CreateContext({
        .EnableDebugExtensions = true,
        .EnableSwapchainExtension = true,
        .RequiredInstanceExtensions = std::vector(glfwReqExts, glfwReqExts + numGlfwReqExts),
        .IsSuitableMainQueue =
            [](const havk::PhysicalDeviceInfo& device, const VkQueueFamilyProperties& props, uint32_t index) {
                return (props.queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0 &&
                       glfwGetPhysicalDevicePresentationSupport(device.Instance, device.Handle, index) == GLFW_TRUE;
            },
    });

    // Create surface and swapchain
    VkSurfaceKHR surface;
    HAVK_CHECK(glfwCreateWindowSurface(device->Instance, window, nullptr, &surface));

    auto swapchain = device->CreateSwapchain(surface);
    swapchain->SetPreferredFormats({ { VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR } }, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    swapchain->SetPreferredPresentModes({ VK_PRESENT_MODE_FIFO_KHR });

    // Create render pipeline
    auto depthlessState = havk::GraphicsPipelineState {
        .Raster = { .FrontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .CullFace = VK_CULL_MODE_NONE },
        .Depth = { .TestOp = VK_COMPARE_OP_ALWAYS },
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

    // Main loop
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        int width, height;
        glfwGetFramebufferSize(window, &width, &height);

        // We can't request a zero size image, wait until window is visible.
        if (width == 0 || height == 0) {
            glfwWaitEvents();
            continue;
        }
        auto [frame, cmds] = swapchain->AcquireImage({ width, height });

        // Swapchain synchronization is implicit, so we don't need a barrier here.
        cmds->BeginRendering({
            .Attachments = { {
                .Target = *frame,
                .LoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                .ClearValue = { 0.5, 0.5, 0.5, 1 },
            } },
        });
        cmds->Draw(*pipeline, { .NumVertices = 3 }, VS_Triangle::Params {
            .vertices = *vertexBuffer,
            .projMat = {
                1,  0,  0,  0,
                0, -1,  0,  0, // flip to Y+ up
                0,  0,  1,  0,
                0,  0,  0,  1,
            },
        });
        cmds->EndRendering();

        swapchain->Present();
    }
    // Swapchain and associated surface *must* be destroyed before glfwTerminate().
    swapchain.reset();
    glfwTerminate();
}