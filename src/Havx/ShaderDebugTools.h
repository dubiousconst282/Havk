#pragma once
#include <Havk/Havk.h>

// Immediate-mode debug functions for shaders.
namespace havx::Shadebug {

struct DrawFrameParams {
    // G-Buffer parameters for rendering of debug shapes
    havk::vectors::float4x4 ProjMat;      // View projection matrix (with no translation)
    havk::vectors::float3 ViewOrigin;     // Camera position
    havk::Image* ColorBuffer = nullptr;   // RGBA.
    havk::Image* NormalBuffer = nullptr;  // RGBA. Normals stored in `0..1` range, `A` set to 1.
    havk::Image* DepthBuffer = nullptr;
    VkCompareOp DepthCompareOp = VK_COMPARE_OP_ALWAYS;

    // Reference image used for zooming and pixel picking.
    // We currently assume it covers the entire screen.
    // Must have SAMPLED usage.
    havk::Image* PickImage = nullptr;
};

// Create debug context and install hooks to device.
void Initialize(havk::DeviceContext* ctx);

// Call this before destroying any previously bound DeviceContext.
void Shutdown();

// Bind DeviceContext and record commands for new frame.
void NewFrame(havk::CommandList& cmds);

// Draw debug geometry and immediate-mode widgets (using ImGui).
void DrawFrame(havk::CommandList& cmds, const DrawFrameParams& pars);

};