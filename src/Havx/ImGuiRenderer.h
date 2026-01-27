#pragma once

#include <imgui.h>

#include <Havk/Havk.h>
#include <Shaders/Havk/DrawImGui.h>

namespace havx {

struct ImGuiRenderer {
private:
    havk::DeviceContext* _device;
    havk::GraphicsPipelinePtr _pipeline;
    VkFormat _renderFormat;

public:
    ImGuiRenderer(havk::DeviceContext* device, VkFormat outputFormat) {
        _device = device;
        _renderFormat = GetUNormFormat(outputFormat);

        havk::GraphicsPipelineState state = {
            .Raster = { .FrontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .CullFace = VK_CULL_MODE_NONE },
            .Depth = { .TestOp = VK_COMPARE_OP_ALWAYS },
            .Blend = { .AttachmentStates = { havk::ColorBlendingState::kSrcOver } },
        };
        havk::AttachmentLayout layout = { .Formats = { _renderFormat } };
        _pipeline = _device->CreateGraphicsPipeline({ shader::VS_ImGuiDrawMain::Module, shader::FS_ImGuiDrawMain::Module }, state, layout);

        ImGuiIO& io = ImGui::GetIO();

        IM_ASSERT(io.BackendRendererUserData == nullptr && "Already initialized a renderer backend!");
        io.BackendRendererUserData = this;
        io.BackendRendererName = "havk";
        io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;  // We can honor the ImDrawCmd::VtxOffset field, allowing for large meshes.
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;   // We can honor ImGuiPlatformIO::Textures[] requests during render.
        io.BackendFlags |= ImGuiBackendFlags_RendererHasViewports;  // We can create multi-viewports on the Renderer side (optional)
        io.Fonts->TexDesiredFormat = ImTextureFormat_Alpha8;

        if ((io.BackendFlags & ImGuiBackendFlags_PlatformHasViewports) == 0) return;

        ImGuiPlatformIO& platform_io = ImGui::GetPlatformIO();
        IM_ASSERT(platform_io.Platform_CreateVkSurface != nullptr && "Platform needs to setup the CreateVkSurface handler.");

        platform_io.Renderer_CreateWindow = [](ImGuiViewport* vp) {
            ImGuiPlatformIO& platform_io = ImGui::GetPlatformIO();
            auto backend = (ImGuiRenderer*)ImGui::GetIO().BackendRendererUserData;

            VkSurfaceKHR surface;
            HAVK_CHECK((VkResult)platform_io.Platform_CreateVkSurface(vp, (ImU64)backend->_device->Instance, nullptr, (ImU64*)&surface));

            auto swapchain = backend->_device->CreateSwapchain(surface).release();
            swapchain->SetPreferredPresentModes({ VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_FIFO_KHR });
            swapchain->SetPreferredFormats({ { backend->_renderFormat } }, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
            vp->RendererUserData = swapchain;
        };
        platform_io.Renderer_DestroyWindow = [](ImGuiViewport* vp) {
            // Swapchain dtor will wait on device idle so this should be fine.
            delete (havk::Swapchain*)vp->RendererUserData;
            vp->RendererUserData = nullptr;
        };
        platform_io.Renderer_SetWindowSize = [](ImGuiViewport* vp, ImVec2 size) {
            auto swapchain = (havk::Swapchain*)vp->RendererUserData;
            swapchain->Initialize({ (uint32_t)size.x, (uint32_t)size.y });
        };
        platform_io.Renderer_RenderWindow = [](ImGuiViewport* vp, void* render_arg) {
            auto swapchain = (havk::Swapchain*)vp->RendererUserData;
            auto backend = (ImGuiRenderer*)ImGui::GetIO().BackendRendererUserData;

            auto [image, cmdList] = swapchain->AcquireImage({ (uint32_t)vp->Size.x, (uint32_t)vp->Size.y });

            cmdList->BeginRendering({ .Attachments = { havk::RenderAttachment::Cleared(*image) }  });
            backend->RenderDrawLists(vp->DrawData, *cmdList);
            cmdList->EndRendering();

            swapchain->Present();
        };
    }

    ~ImGuiRenderer() {
        ImGuiIO& io = ImGui::GetIO();
        ImGuiPlatformIO& platform_io = ImGui::GetPlatformIO();
        
        io.BackendRendererName = nullptr;
        io.BackendRendererUserData = nullptr;
        io.BackendFlags &= ~(ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasViewports);

        for (ImTextureData* tex : platform_io.Textures) {
            if (tex->RefCount == 1) {
                tex->SetStatus(ImTextureStatus_WantDestroy);
                UpdateTexture(tex);
            }
        }
        for (ImGuiViewport* viewport : platform_io.Viewports) {
            if (auto swapchain = (havk::Swapchain*)viewport->RendererUserData) {
                delete swapchain;
            }
        }
    }

    void LoadDefaultStyle() {
        ImGuiIO& io = ImGui::GetIO();

        io.IniFilename = "priv/imgui.ini";
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
        io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;

        ImGuiStyle& style = ImGui::GetStyle();
        ImGui::StyleColorsLight(&style);
        style.FrameRounding = 3;
        style.GrabRounding = 3;
        style.FrameBorderSize = 1.0f;
        style.Colors[ImGuiCol_Border] = ImVec4(0.00f, 0.00f, 0.00f, 0.12f);

        // Try load a better font from system files
        const std::pair<const char*, float> trialFontFiles[] = {
            { "assets/fonts/Roboto-Medium.ttf", 14.0f },
#if _WIN32
            { "C:/Windows/Fonts/seguisb.ttf", 16.0f },
#else
            { "/usr/share/fonts/noto/NotoSans-Medium.ttf", 15.0f },
            { "/usr/share/fonts/TTF/DejaVuSans.ttf", 13.0f },
#endif
        };
        for (auto [filename, size] : trialFontFiles) {
            // Check if file exists
#if _WIN32
            if (FILE* fs; fopen_s(&fs, filename, "rb") == 0) fclose(fs);
            else continue;
#else
            if (access(filename, R_OK) != 0) continue;
#endif
            io.Fonts->AddFontFromFileTTF(filename, size);
            break;
        }
    }

    void Render(havk::Image& image, havk::CommandList& cmdList) {
        ImGui::Render();

        VkFormat unormFormat = GetUNormFormat(image.Format);
        auto unormView = image.Format != unormFormat ? image.GetSubView({ .Format = unormFormat }) : image;
        cmdList.BeginRendering({
            .Attachments = { havk::RenderAttachment::Overlay(unormView) },
            .SrcStages = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        });
        RenderDrawLists(ImGui::GetDrawData(), cmdList);
        cmdList.EndRendering();

        if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
            ImGui::UpdatePlatformWindows();
            ImGui::RenderPlatformWindowsDefault();
        }
    }

    // Get ImGui texture ID. The image *must* be visible to STAGE_FRAGMENT and under READ_ONLY or GENERAL layout just before the call to
    // Render().
    ImTextureID GetTextureID(const havk::Image* image, VkFilter filter = VK_FILTER_LINEAR) {
        HAVK_ASSERT(filter == VK_FILTER_LINEAR); // Custom views and filter not supported yet
        return (ImTextureID)image;
    }

    void RenderDrawLists(ImDrawData* draw_data, havk::CommandList& cmdList) {
        // Avoid rendering when minimized, scale coordinates for retina displays (screen coordinates != framebuffer coordinates)
        int fb_width = (int)(draw_data->DisplaySize.x * draw_data->FramebufferScale.x);
        int fb_height = (int)(draw_data->DisplaySize.y * draw_data->FramebufferScale.y);
        if (fb_width <= 0 || fb_height <= 0) return;

        // Catch up with texture updates. Most of the times, the list will have 1 element with an OK status, aka nothing to do.
        // (This almost always points to ImGui::GetPlatformIO().Textures[] but is part of ImDrawData to allow overriding or disabling
        // texture updates).
        if (draw_data->Textures != nullptr) {
            for (ImTextureData* tex : *draw_data->Textures) {
                if (tex->Status != ImTextureStatus_OK) UpdateTexture(tex);
            }
        }

        if (draw_data->TotalVtxCount == 0) return;

        size_t bufferSize = (size_t)draw_data->TotalVtxCount * sizeof(ImDrawVert) +
                            (size_t)draw_data->TotalIdxCount * sizeof(ImDrawIdx);
        auto renderBuffer = _device->CreateBuffer(bufferSize, havk::BufferFlags::MapSeqWrite);

        // Setup desired Vulkan state
        BindRenderState(draw_data, cmdList, fb_width, fb_height, *renderBuffer);

        // Will project scissor/clipping rectangles into framebuffer space
        ImVec2 clip_off = draw_data->DisplayPos;          // (0,0) unless using multi-viewports
        ImVec2 clip_scale = draw_data->FramebufferScale;  // (1,1) unless using retina display which are often (2,2)

        // Upload vertex data and record draw commands
        auto vertexSpan = renderBuffer->Slice<ImDrawVert>(0, (size_t)draw_data->TotalVtxCount);
        auto indexSpan = renderBuffer->Slice<ImDrawIdx>(vertexSpan.size_bytes(), (size_t)draw_data->TotalIdxCount);
        havk::Image* lastTexure = nullptr;

        for (const ImDrawList* draw_list : draw_data->CmdLists) {
            for (const ImDrawCmd& cmd : draw_list->CmdBuffer) {
                if (cmd.UserCallback != nullptr) {
                    // User callback, registered via ImDrawList::AddCallback()
                    // (ImDrawCallback_ResetRenderState is a special callback value used by the user to request the renderer to reset render
                    // state.)
                    if (cmd.UserCallback == ImDrawCallback_ResetRenderState) {
                        BindRenderState(draw_data, cmdList, fb_width, fb_height, *renderBuffer);
                    } else {
                        cmd.UserCallback(draw_list, &cmd);
                    }
                } else {
                    // Project scissor/clipping rectangles into framebuffer space
                    ImVec2 clip_min((cmd.ClipRect.x - clip_off.x) * clip_scale.x, (cmd.ClipRect.y - clip_off.y) * clip_scale.y);
                    ImVec2 clip_max((cmd.ClipRect.z - clip_off.x) * clip_scale.x, (cmd.ClipRect.w - clip_off.y) * clip_scale.y);

                    // Clamp to viewport as vkCmdSetScissor() won't accept values that are off bounds
                    if (clip_min.x < 0.0f) clip_min.x = 0.0f;
                    if (clip_min.y < 0.0f) clip_min.y = 0.0f;
                    if (clip_max.x > fb_width) clip_max.x = (float)fb_width;
                    if (clip_max.y > fb_height) clip_max.y = (float)fb_height;
                    if (clip_max.x <= clip_min.x || clip_max.y <= clip_min.y) continue;

                    // Apply scissor/clipping rectangle
                    cmdList.SetScissor({
                        (int32_t)clip_min.x,
                        (int32_t)clip_min.y,
                        (uint32_t)(clip_max.x - clip_min.x),
                        (uint32_t)(clip_max.y - clip_min.y),
                    });

                    auto texture = (havk::Image*)cmd.GetTexID();
                    if (texture != lastTexure) {
                        cmdList.PushConstants({ &texture->DescriptorHandle, offsetof(shader::ImGuiDrawParams, Texture), sizeof(havk::ImageHandle) });
                        lastTexure = texture;
                    }

                    cmdList.DrawIndexed(*_pipeline, {
                        .NumIndices = cmd.ElemCount,
                        .IndexOffset = (uint32_t)indexSpan.offset() + cmd.IdxOffset,
                        .VertexOffset = (uint32_t)vertexSpan.offset() + cmd.VtxOffset,
                    });
                }
            }
            vertexSpan.bump_write(draw_list->VtxBuffer.Data, (size_t)draw_list->VtxBuffer.Size);
            indexSpan.bump_write(draw_list->IdxBuffer.Data, (size_t)draw_list->IdxBuffer.Size);
        }
        renderBuffer->Flush(0, VK_WHOLE_SIZE);
    }

private:
    void BindRenderState(ImDrawData* draw_data, havk::CommandList& cmdList, int fb_width, int fb_height, havk::Buffer& renderBuffer) {
        cmdList.SetViewport({ 0, 0, (float)fb_width, (float)fb_height, 0.0f, 1.0f });

        // Setup scale and translation:
        // Our visible imgui space lies from DisplayPos (top left) to DisplayPos+DisplaySize (bottom right).
        // DisplayPos is (0,0) for single viewport apps.
        float scale_x = 2.0f / draw_data->DisplaySize.x;
        float scale_y = 2.0f / draw_data->DisplaySize.y;
        float offset_x = -1.0f - draw_data->DisplayPos.x * scale_x;
        float offset_y = -1.0f - draw_data->DisplayPos.y * scale_y;
        cmdList.PushConstants(shader::ImGuiDrawParams {
            .Scale = { scale_x, scale_y },
            .Offset = { offset_x, offset_y },
            .Vertices = renderBuffer,
        });
        cmdList.BindIndexBuffer(renderBuffer, 0, sizeof(ImDrawIdx) == 2 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
    }
    void UpdateTexture(ImTextureData* tex) {
        if (tex->Status == ImTextureStatus_WantCreate) {
            // Create and upload new texture to graphics system
            // IMGUI_DEBUG_LOG("UpdateTexture #%03d: WantCreate %dx%d\n", tex->UniqueID, tex->Width, tex->Height);
            IM_ASSERT(tex->TexID == ImTextureID_Invalid && tex->BackendUserData == nullptr);
            IM_ASSERT(tex->Format == ImTextureFormat_RGBA32 || tex->Format == ImTextureFormat_Alpha8);

            havk::ImageDesc desc = {
                .Format = tex->Format == ImTextureFormat_RGBA32 ? VK_FORMAT_R8G8B8A8_UNORM : VK_FORMAT_R8_UNORM,
                .Usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                .Size = { tex->Width, tex->Height, 1 },
                .MipLevels = 1,
            };
            if (tex->Format == ImTextureFormat_Alpha8) memcpy(desc.ChannelSwizzle, "111R", 4);

            auto backend_tex = _device->CreateImage(desc);
            tex->SetTexID((ImTextureID)backend_tex.release());
        }

        if (tex->Status == ImTextureStatus_WantCreate || tex->Status == ImTextureStatus_WantUpdates) {
            auto backend_tex = (havk::Image*)tex->GetTexID();

            // Update full texture or selected blocks. We only ever write to textures regions which have never been used before!
            // This backend choose to use tex->UpdateRect but you can use tex->Updates[] to upload individual regions.
            // We could use the smaller rect on _WantCreate but using the full rect allows us to clear the texture.
            int upload_x = (tex->Status == ImTextureStatus_WantCreate) ? 0 : tex->UpdateRect.x;
            int upload_y = (tex->Status == ImTextureStatus_WantCreate) ? 0 : tex->UpdateRect.y;
            int upload_w = (tex->Status == ImTextureStatus_WantCreate) ? tex->Width : tex->UpdateRect.w;
            int upload_h = (tex->Status == ImTextureStatus_WantCreate) ? tex->Height : tex->UpdateRect.h;

            // Create the Upload Buffer
            int upload_pitch = upload_w * tex->BytesPerPixel;
            uint32_t upload_size = (uint32_t)(upload_h * upload_pitch);
            auto upload_buffer = _device->CreateBuffer(upload_size, havk::BufferFlags::HostMem_SeqWrite);

            // Upload to Buffer
            for (int y = 0; y < upload_h; y++) {
                memcpy(&upload_buffer->MappedData[upload_pitch * y], tex->GetPixelsAt(upload_x, upload_y + y), (size_t)upload_pitch);
            }

            // Copy to Image
            auto cmdList = _device->CreateCommandList();
            cmdList->CopyBufferToImage({
                .SrcData = *upload_buffer,
                .DstImage = *backend_tex,
                .DstOffset = { upload_x, upload_y, 0 },
                .DstExtent = { upload_w, upload_h, 1 },
                .SrcStages = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                .DstStages = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            });
            cmdList->Submit().Wait();
            havk::Destroy(upload_buffer);

            tex->SetStatus(ImTextureStatus_OK);
        }

        if (tex->Status == ImTextureStatus_WantDestroy) {
            auto backend_tex = (havk::Image*)tex->GetTexID();
            havk::Resource::QueuedDeleter{}(backend_tex);

            // Clear identifiers and mark as destroyed (in order to allow e.g. calling InvalidateDeviceObjects while running)
            tex->SetTexID(ImTextureID_Invalid);
            tex->SetStatus(ImTextureStatus_Destroyed);
        }
    }
    static VkFormat GetUNormFormat(VkFormat format) {
        switch (format) {
            case VK_FORMAT_R8G8B8A8_SRGB: return VK_FORMAT_R8G8B8A8_UNORM;
            case VK_FORMAT_B8G8R8A8_SRGB: return VK_FORMAT_B8G8R8A8_UNORM;
            case VK_FORMAT_A8B8G8R8_SRGB_PACK32: return VK_FORMAT_A8B8G8R8_UNORM_PACK32;
            default: return format;
        }
    }
};

};  // namespace havx