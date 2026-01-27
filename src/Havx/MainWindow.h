#pragma once

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#define IMGUI_DISABLE_SSE  // minimize include bloat
#include <imgui_internal.h>
#include <backends/imgui_impl_glfw.h>
#include <implot.h>

#include <Havk/Havk.h>
#include "SystemUtils.h"
#include "ImGuiRenderer.h"

#include <stdexcept>

namespace havx {

struct MainWindow {
    GLFWwindow* Handle;
    double FrameRateLimit = 0;
    std::unique_ptr<ImGuiRenderer> OverlayRenderer = nullptr;

private:
    double _framePrevEndTime = 0;
    uint32_t _lastRescaledMouseEventId = 0;  // ImGui scaling hack
    int _lastWinRect[4] = {};                // Window coords before switching to fullscreen

public:
    MainWindow(const char* title, int width = 0, int height = 0) {
        // Note that ImGui viewports are not supported on the native Wayland backend, due to... whatever
        // https://gitlab.freedesktop.org/wayland/wayland-protocols/-/merge_requests/264
        //
        // Can force X11/XWayland as workaround, but comes with a few other issues (somewhat buggy, no HDR, harsh scrolling).
        // set environment var: XDG_SESSION_TYPE=x11
        // or call:             glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_X11);
        if (glfwInit()) {
            glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
            if (width == 0 || height == 0) {
                auto* vidmode = glfwGetVideoMode(glfwGetPrimaryMonitor());
                width = vidmode->width * 4 / 6;
                height = vidmode->height * 4 / 6;
            }
            Handle = glfwCreateWindow(width, height, title, NULL, NULL);
        }
        if (!Handle) {
            const char* desc;
            glfwGetError(&desc);
            glfwTerminate();
            throw std::runtime_error(desc);
        }
    }
    MainWindow(GLFWwindow* window) : Handle(window) {}

    ~MainWindow() {
        if (OverlayRenderer != nullptr) {
            OverlayRenderer = nullptr;
            ImGui_ImplGlfw_Shutdown();
            ImPlot::DestroyContext();
            ImGui::DestroyContext();
        }
        glfwTerminate();
    }

    void GetDeviceCreationParams(havk::DeviceCreateParams& pars) {
        uint32_t numReqExts;
        auto reqExts = glfwGetRequiredInstanceExtensions(&numReqExts);

        for (uint32_t i = 0; i < numReqExts; i++) {
            pars.RequiredInstanceExtensions.push_back(reqExts[i]);
        }
        pars.EnableSwapchainExtension = true;
        pars.IsSuitableMainQueue = [&](const havk::PhysicalDeviceInfo& dev, const VkQueueFamilyProperties& props, uint32_t index) {
            return (props.queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0 &&
                   glfwGetPhysicalDevicePresentationSupport(dev.Instance, dev.Handle, index) == GLFW_TRUE;
        };
    }
    havk::SwapchainPtr CreateSwapchain(havk::DeviceContext& ctx) {
        VkSurfaceKHR surface;
        HAVK_CHECK(glfwCreateWindowSurface(ctx.Instance, Handle, nullptr, &surface));
        return ctx.CreateSwapchain(surface);
    }

    void CreateOverlay(havk::Swapchain& swapchain) {
        if (ImGui::GetCurrentContext() == nullptr) {
            ImGui::CreateContext();
            ImPlot::CreateContext();
            ImGui_ImplGlfw_InitForVulkan(Handle, true);
        }
        OverlayRenderer = std::make_unique<ImGuiRenderer>(swapchain.Context, swapchain.GetFormat());
        OverlayRenderer->LoadDefaultStyle();

        float scale;
        glfwGetWindowContentScale(Handle, &scale, nullptr);

        if (scale != (int)scale) {
            scale = 1.0f + (scale - (int)scale);
            ImGuiStyle& style = ImGui::GetStyle();
            style.FontScaleDpi = scale;
            style.ScaleAllSizes(scale);
        }
    }

    void NewFrame() {
        if (OverlayRenderer != nullptr) {
            ImGui_ImplGlfw_NewFrame();

            ImGuiIO& io = ImGui::GetIO();

            // On platforms like Wayland, fractional FB scaling makes text and lines blurry and shimmery.
            // We'll do a little hacking to force ImGui render everything at int scale, and adjust style at startup.
            ImVec2 fbScale = io.DisplayFramebufferScale;
            ImVec2 fbScaleI = ImVec2((int)fbScale.x, (int)fbScale.y);
            if (fbScale.x != fbScaleI.x || fbScale.y != fbScaleI.y) {
                fbScale.x = 1.0f + (fbScale.x - fbScaleI.x);
                fbScale.y = 1.0f + (fbScale.y - fbScaleI.y);

                if (io.WantSetMousePos) {
                    glfwSetCursorPos(Handle, io.MousePos.x / fbScale.x, io.MousePos.y / fbScale.x);
                }
                io.DisplaySize.x *= fbScale.x;
                io.DisplaySize.y *= fbScale.y;
                io.DisplayFramebufferScale = fbScaleI;

                for (auto& event : ImGui::GetCurrentContext()->InputEventsQueue) {
                    if (event.Type == ImGuiInputEventType_MousePos && event.EventId > _lastRescaledMouseEventId) {
                        event.MousePos.PosX *= fbScale.x;
                        event.MousePos.PosY *= fbScale.y;
                        // ImGui will occasionally defer events due to the trickling rule (see ImGui::UpdateInputEvents()).
                        // This ensures we only rewrite the same event once.
                        _lastRescaledMouseEventId = event.EventId;
                    }
                }
            }
            ImGui::NewFrame();
        }
    }

    void WaitFrameInterval() {
        // TODO: implement something better because this kinda sucks (...but all the alternatives seem painful)
        // - VK_EXT_present_timing
        // - https://raphlinus.github.io/ui/graphics/gpu/2021/10/22/swapchain-frame-pacing.html
        // - https://github.com/gfx-rs/wgpu/issues/6932
        // - https://github.com/glfw/glfw/issues/1157
        if (FrameRateLimit > 0 && FrameRateLimit <= 10000) {
            double targetDur = 1.0 / FrameRateLimit;
            double elapsed = glfwGetTime() - _framePrevEndTime;
            double delay = targetDur - elapsed;

            if (delay > 0) {
                PreciseSleep(delay);
            }
            _framePrevEndTime = glfwGetTime();
        }
    }

    // Convenience render loop.
    void RunLoop(havk::Swapchain& swapchain, std::invocable<havk::Image&, havk::CommandList&> auto renderFrame) {
        while (!glfwWindowShouldClose(Handle)) {
            glfwPollEvents();

            int width, height;
            glfwGetFramebufferSize(Handle, &width, &height);

            if (width == 0 || height == 0) {
                // This happens on Windows when minimized, but swapchain will throw
                // when requesting image with zero size.
                // TODO: should probably try update ImGui viewports when minimized.
                glfwWaitEvents();
                continue;
            }
            auto [frame, cmds] = swapchain.AcquireImage({ width, height });
            // GC after all waiting to maybe catch up with async GPU work
            swapchain.Context->GarbageCollect();

            NewFrame();

            renderFrame(*frame, *cmds);

            if (OverlayRenderer != nullptr) {
                OverlayRenderer->Render(*frame, *cmds);

                if (ImGui::IsKeyPressed(ImGuiKey_F11)) {
                    ToggleFullScreen();
                }
            }
            swapchain.Present();
            WaitFrameInterval();
        }
        // Must release ImGui renderer before device is destroyed
        OverlayRenderer.reset();
    }

    static constexpr double kFrameLimitMonitor = -1, kFrameLimitNone = DBL_MAX;

    void SetFrameRateLimit(double maxFramesPerSec, havk::Swapchain* swapchain) {
        double refreshRate = GetMonitorRefreshRate();
        if (maxFramesPerSec == kFrameLimitMonitor) {
            maxFramesPerSec = refreshRate;
        }
        if (swapchain != nullptr) {
            // When present mode = FIFO, setting a limit higher than monitor's
            // refresh rate will increase latency.
            // In windowed mode, immediate mode is the same as mailbox (drops frames).
            if (maxFramesPerSec <= 0 || maxFramesPerSec > refreshRate) {
                swapchain->SetPreferredPresentModes({ VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR });
            } else {
                swapchain->SetPreferredPresentModes({ VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_FIFO_KHR });
            }
        }
        FrameRateLimit = maxFramesPerSec;
    }
    double GetMonitorRefreshRate() {
        // No API to query which monitor window is currently in, so just get the primary.
        // https://github.com/glfw/glfw/issues/1699
        auto mode = glfwGetVideoMode(glfwGetPrimaryMonitor());
        return mode->refreshRate;
    }

    void ToggleFullScreen() {
        bool isFullscreen = glfwGetWindowMonitor(Handle) != nullptr;

        if (!isFullscreen) {
            // Backup window position and window size
            glfwGetWindowPos(Handle, &_lastWinRect[0], &_lastWinRect[1]);
            glfwGetWindowSize(Handle, &_lastWinRect[2], &_lastWinRect[3]);

            auto monitor = glfwGetPrimaryMonitor();
            auto mode = glfwGetVideoMode(monitor);

            glfwSetWindowMonitor(Handle, monitor, 0, 0, mode->width, mode->height, mode->refreshRate);
        } else {
            // Restore last window size and position
            glfwSetWindowMonitor(Handle, nullptr, _lastWinRect[0], _lastWinRect[1], _lastWinRect[2], _lastWinRect[3], 0);
        }
    }
};

};  // namespace havx