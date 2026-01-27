// https://vkguide.dev/docs/gpudriven
// https://github.com/zeux/niagara
#include <Havk/Havk.h>
#include <Havx/MainWindow.h>
#include <Havx/Camera.h>
#include <Havx/ShaderDebugTools.h>

#define HAVK_PERFMON_OVERRIDE_TRACY_MACROS
#include <Havx/PerfMonitor.h>

#include "Model.h"

using namespace havk::vectors;

struct MaterialKey {
    bool DoubleSided;
    bool AlphaTest;

    MaterialKey() {}
    MaterialKey(const Material& mat) : DoubleSided(mat.DoubleSided), AlphaTest(mat.AlphaCutoff > 0.0f) {}

    bool operator==(const MaterialKey&) const = default;
};
static constexpr auto HashMaterialKey = [](const MaterialKey& key) { return key.DoubleSided + key.AlphaTest * 2; };

struct ModelDrawBatch {
    havk::GraphicsPipeline* Pipeline;
    MaterialKey Material;
    havk::BufferSpan<shader::MeshDrawCommand> DrawCmds;
    havk::BufferSpan<uint32_t> DrawCount;
    havk::BufferSpan<float3x4> Transforms;
};
struct ModelRenderData {
    Model* Model;
    havk::BufferPtr ScratchBuffer;
    std::vector<ModelDrawBatch> DrawBatches;
    havk::BufferSpan<float3x4> Transforms;

    Animation* ActiveAnim = nullptr;
};
struct AppWindow {
    havx::MainWindow _window;
    havk::DeviceContextPtr _device;
    havk::SwapchainPtr _swapchain;

    // Scene
    havx::Camera _camera = { .FieldOfView = 80.0f, .MoveSpeed = 5.0 };
    std::unique_ptr<Model> _model;

    // Renderer
    std::vector<ModelRenderData> _activeModels;
    havk::GraphicsPipelinePtr _deferredPipeline;
    havk::AccelStructPoolPtr _accels;

    havk::ImagePtr _gbuffer, _depthBuffer, _depthPyramid;

    AppWindow() : _window("GPU Driven Rendering") {
        auto devicePars = havk::DeviceCreateParams { .EnableDebugExtensions = true };
        _window.GetDeviceCreationParams(devicePars);
        _device = havk::CreateContext(devicePars);

        _swapchain = _window.CreateSwapchain(*_device);
        _swapchain->SetPreferredFormats({ { VK_FORMAT_B8G8R8A8_UNORM } },
                                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
        _window.SetFrameRateLimit(havx::MainWindow::kFrameLimitNone, _swapchain.get());
        _window.CreateOverlay(*_swapchain);

        auto rasterState = havk::GraphicsPipelineState {
            .Raster = { .FrontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .CullFace = havk::dynamic_state },
            .Depth = { .TestOp = VK_COMPARE_OP_GREATER, .Write = true },
        };
        auto gbufferLayout = havk::AttachmentLayout {
            .Formats = { VK_FORMAT_R32G32_UINT },
            .DepthFormat = VK_FORMAT_D32_SFLOAT,
        };
        _deferredPipeline = _device->CreateGraphicsPipeline({ shader::VS_ModelIndirect::Module, shader::FS_ModelDeferred::Module },
                                                            rasterState, gbufferLayout);
    }
    ~AppWindow() {
        havx::Shadebug::Shutdown();
        havx::PerfMon::Shutdown();
    }

    void RunLoop() {
        _window.RunLoop(*_swapchain, [&](auto&&... args) { RenderFrame(args...); });
    }

    void RenderFrame(havk::Image& frame, havk::CommandList& cmds) {
        havx::PerfMon::NewFrame(cmds);
        havx::Shadebug::NewFrame(cmds);
        ZoneScoped;

        // Toggle camera modes
        if (ImGui::IsKeyPressed(ImGuiKey_F2)) {
            _camera.Mode = _camera.Mode == havx::Camera::Arcball ? havx::Camera::FirstPerson : havx::Camera::Arcball;
        }
        _camera.Update(havx::Camera::GetInputsFromImGui());
        float4x4 projMat = _camera.GetProjMatrix();
        float4x4 viewMat = _camera.GetViewMatrix(true);
        float4x4 projViewMat = projMat * viewMat;
        float4x4 projOrientMat = projMat * _camera.GetViewMatrix(false);

        if (_gbuffer == nullptr || _gbuffer->Size != frame.Size) {
            _gbuffer = _device->CreateImage({
                .Format = VK_FORMAT_R32G32_UINT,
                .Usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                .Size = frame.Size,
            });
            _depthBuffer = _device->CreateImage({
                .Format = VK_FORMAT_D32_SFLOAT,
                .Usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                .Size = frame.Size,
            });
            _depthPyramid = _device->CreateImage({
                .Format = VK_FORMAT_R32_SFLOAT,
                .Usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
                // Round down to nearest pow2 to avoid issues with odd mip sizes
                // findMSB(255) = 7, findMSB(256) = 8, ...
                .Size = uint3(1 << glm::findMSB(uint2(frame.Size)), 1),
                .MipLevels = VK_REMAINING_MIP_LEVELS,
            });
        }

        std::vector<std::tuple<Model*, ModelDrawBatch>> drawBatches;
        havk::BufferSpan<shader::Light> lights;

        auto scratchBuffer = _device->CreateBuffer(UINT_MAX, havk::BufferFlags::DeferredAlloc);
        {
            ZoneScopedN("Cull and Update");

            // Downsample depth buffer
            VkSamplerReductionModeCreateInfo reductionMode = {
                .sType = VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO,
                .reductionMode = VK_SAMPLER_REDUCTION_MODE_MIN,
            };
            havk::SamplerHandle depthReductionSampler = _device->DescriptorHeap->GetSampler({
                .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                .pNext = &reductionMode,
                .magFilter = VK_FILTER_LINEAR,
                .minFilter = VK_FILTER_LINEAR,
                .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                .maxLod = VK_LOD_CLAMP_NONE,
            });
            havk::ImageView srcDepthMip = *_depthBuffer;

            for (uint32_t i = 0; i < _depthPyramid->MipLevels; i++) {
                uint3 mipSize = glm::max(_depthPyramid->Size >> i, 1u);
                auto dstDepthMip = _depthPyramid->GetSubView({
                    .MipOffset = (uint8_t)i,
                    .MipLevels = 1,
                    .ShaderUsage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
                });
                cmds.Dispatch<shader::CS_DownsampleDepth>(mipSize, {
                    .srcImage = srcDepthMip,
                    .dstImage = dstDepthMip,
                    .sampler = depthReductionSampler,
                    .srcScale = 1.0f / float2(mipSize),
                });
                cmds.Barrier({ .SrcStages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, .DstStages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT });
                srcDepthMip = dstDepthMip;
            }

            // First pass to compute scratch size
            auto span = scratchBuffer->Slice<uint8_t>();
            uint32_t numLights = 0, lightIdx = 0;
            for (ModelRenderData& renderData : _activeModels) {
                Model* model = renderData.Model;

                if (renderData.ActiveAnim != nullptr) {
                    span.bump_slice<float3x4>(model->NumLeafNodes + model->MaxJointMatrices);
                }
                span.bump_slice<uint32_t>(renderData.DrawBatches.size());
                span.bump_slice<shader::MeshDrawCommand>(model->MaxDrawCommands);
                numLights += model->Lights.size();
            }
            lights = span.bump_slice<shader::Light>(numLights, 16);

            span.commit_bump_alloc(havk::BufferFlags::MapSeqWrite);

            // Record draw batches
            for (ModelRenderData& renderData : _activeModels) {
                Model* model = renderData.Model;
                auto nodeTransforms = renderData.Transforms;

                if (renderData.ActiveAnim != nullptr) {
                    nodeTransforms = span.bump_slice<float3x4>(model->NumLeafNodes);
                    auto jointMatrices = span.bump_slice<float3x4>(model->MaxJointMatrices);
                    model->UpdatePose(renderData.ActiveAnim, ImGui::GetTime(), nodeTransforms, jointMatrices);
                }
                auto counters = span.bump_slice<uint32_t>(renderData.DrawBatches.size());
                memset(counters.data(), 0, counters.size_bytes());

                for (ModelDrawBatch& batch : renderData.DrawBatches) {
                    ModelDrawBatch culledBatch = {
                        .Pipeline = batch.Pipeline,
                        .Material = batch.Material,
                        .DrawCmds = span.bump_slice<shader::MeshDrawCommand>(batch.DrawCmds.size()),
                        .DrawCount = counters.bump_slice(1),
                        .Transforms = nodeTransforms,
                    };
                    cmds.Dispatch<shader::CS_ModelCullDraws>({ batch.DrawCmds.size(), 1, 1 }, {
                        .srcDraws = batch.DrawCmds,
                        .dstDraws = culledBatch.DrawCmds,
                        .sourceCount = (uint32_t)batch.DrawCmds.size(),
                        .culledCount = culledBatch.DrawCount,
                        .meshBoundSpheres = model->GpuBoundSpheres,
                        .transforms = nodeTransforms,
                        .depthPyramid = *_depthPyramid,
                        .depthSampler = depthReductionSampler,
                        .viewMat = viewMat,
                        .viewProjMat = projViewMat,
                        .projFactors = float3(_camera.NearZ, projMat[0][0], projMat[1][1]),
                    });
                    drawBatches.push_back({ model, culledBatch });
                }
                for (Light& light : model->Lights) {
                    lights[lightIdx++] = light;
                }
            }
        }

        {
            ZoneScopedN("Draw Scene");

            cmds.BeginRendering({
                .Attachments = { havk::RenderAttachment::Cleared(*_gbuffer, { 0, 0, 0, 0 }) },
                .DepthAttachment = havk::RenderAttachment::Cleared(*_depthBuffer, { 0 }),
                .SrcStages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                .DstStages = VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
            });
            for (auto& [model, batch] : drawBatches) {
                cmds.BindIndexBuffer(*model->StorageBuffer, 0, VK_INDEX_TYPE_UINT32);
                vkCmdSetCullMode(cmds.Handle, batch.Material.DoubleSided ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT);

                cmds.DrawIndexedIndirectCount(*batch.Pipeline, batch.DrawCmds, batch.DrawCount, shader::ModelDrawParams {
                    .Draws = batch.DrawCmds,
                    .Meshes = model->GpuMeshes,
                    .Transforms = batch.Transforms,
                    .Materials = model->GpuMaterials,
                    .ViewPos = float3(_camera.ViewPosition),
                    .ViewProj = projViewMat,
                });
            }
            cmds.EndRendering();
        }

        {
            ZoneScopedN("GBuffer Resolve");

            cmds.Barrier({ .SrcStages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, .DstStages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT });
            cmds.Dispatch<shader::CS_GBufferResolve>(frame.Size, {
                .outputImage = frame,
                .gbuffer = *_gbuffer,
                .depthBuffer = *_depthBuffer,
                .sceneTlas = _accels ? _accels->GetAddress(0) : havk::AccelStructHandle(),
                .lights = lights,
                .numLights = (uint32_t)lights.size(),
                .viewPos = float3(_camera.ViewPosition),
                .invProj = havx::GetInverseScreenProjMatrix(projOrientMat, frame.Size),
            });
        }

        {
            ZoneScopedN("Shadebug");
            havx::Shadebug::DrawFrame(cmds, {
                .ProjMat = projOrientMat,
                .ViewOrigin = _camera.ViewPosition,
                .ColorBuffer = &frame,
                .DepthBuffer = _depthBuffer.get(),
                .DepthCompareOp = VK_COMPARE_OP_GREATER,
                .PickImage = &frame,
            });
        }
        havx::PerfMon::DrawFrame();

        ImGui::ShowDemoWindow();
    }

    void LoadModel(const std::string& path) {
        _model = std::make_unique<Model>(_device.get(), path);

        if (_model->Lights.size() == 0) {
            Light& light = _model->Lights.emplace_back();
            light.Name = "Sun";
            light.Direction = glm::normalize(float3(0.4, -1, 0.2));
            light.PackedColor = 0xFFFFFFFF;
            light.Intensity = 2000.0;
            light.UpdateCachedValues();
        }

        ModelRenderData& renderData = _activeModels.emplace_back();
        renderData.Model = _model.get();

        if (_model->Animations.size() > 0) {
            renderData.ActiveAnim = &_model->Animations[0];
        }
        renderData.ScratchBuffer = _device->CreateBuffer(UINT_MAX, havk::BufferFlags::DeferredAlloc);

        auto templateData = renderData.ScratchBuffer->Slice<uint8_t>();
        auto drawCmds = templateData.bump_slice<shader::MeshDrawCommand>(_model->MaxDrawCommands);
        renderData.Transforms = templateData.bump_slice<float3x4>(_model->NumLeafNodes, 16);

        templateData.commit_bump_alloc(havk::BufferFlags::MapSeqWrite);

        // Count material batch sizes
        std::unordered_map<MaterialKey, uint32_t, decltype(HashMaterialKey)> drawBatchOffsets;

        for (uint32_t nodeIdx : _model->NodeIndicesPreDFS) {
            ModelNode& node = _model->Nodes[nodeIdx];

            for (uint32_t i : node.MeshIndices) {
                ModelMesh& mesh = _model->Meshes[i];
                Material& material = _model->Materials[mesh.MaterialId];
                drawBatchOffsets[material]++;
            }
        }
        // Prefix sum, emit batches
        uint32_t runningOffset = 0;
        for (auto& [key, offset] : drawBatchOffsets) {
            uint32_t size = offset;
            offset = runningOffset;
            runningOffset += size;

            renderData.DrawBatches.push_back({
                .Pipeline = _deferredPipeline.get(),
                .Material = key,
                .DrawCmds = drawCmds.subspan(offset, size),
                .Transforms = renderData.Transforms,
            });
        }

        // Generate base draw commands
        uint32_t transformIdx = 0, jointsOffset = _model->NumLeafNodes;

        for (uint32_t nodeIdx : _model->NodeIndicesPreDFS) {
            ModelNode& node = _model->Nodes[nodeIdx];
            if (node.MeshIndices.empty()) continue;

            for (uint32_t i : node.MeshIndices) {
                ModelMesh& mesh = _model->Meshes[i];
                Material& material = _model->Materials[mesh.MaterialId];
                uint32_t& cmdOffset = drawBatchOffsets[material];

                drawCmds[cmdOffset++] = {
                    .Cmd = { .NumIndices = mesh.NumIndices, .IndexOffset = mesh.IndexOffset },
                    .MeshIdx = i,
                    .TransformIdx = transformIdx,
                    .JointsOffset = jointsOffset,
                };
            }
            renderData.Transforms[transformIdx++] = Model::TruncateMatrixCM34(node.GlobalTransform);
            jointsOffset += node.Joints.size();
        }

        // Create ray-tracing acceleration structures if the device supports it
        if (_device->PhysicalDevice.Features.RayQuery) {
            BuildAccelStruct(*_model);
        }
    }

    void BuildAccelStruct(Model& model) {
        auto blasDesc = havk::AccelStructBuildDesc();
        auto transformMatrixBuffer = _device->CreateBuffer(sizeof(float3x4) * model.NumLeafNodes, havk::BufferFlags::HostMem_SeqWrite);
        auto transformMatrices = transformMatrixBuffer->Slice<float3x4>();

        for (uint32_t nodeIdx : model.NodeIndicesPreDFS) {
            ModelNode& node = model.Nodes[nodeIdx];
            if (node.MeshIndices.empty()) continue;

            for (uint32_t i : node.MeshIndices) {
                ModelMesh& mesh = model.Meshes[i];

                auto vertices = model.StorageBuffer->Slice<float3>(mesh.Positions.addr - model.StorageBuffer->DeviceAddress, mesh.NumVertices);
                auto indices = model.StorageBuffer->Slice<uint32_t>(mesh.IndexOffset * sizeof(uint32_t), mesh.NumIndices);
                blasDesc.AddTriangles(vertices, indices, VK_FORMAT_R32G32B32_SFLOAT, 0, transformMatrices);
            }
            transformMatrices.bump_slice(1)[0] = glm::transpose(node.GlobalTransform);
        }

        const auto buildFlags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        blasDesc.CalculateSizesForBLAS(_device.get(), buildFlags);

        // Calculate TLAS size early, so we can tightly allocate storage.
        // For more dynamic cases, the pool could be allocated to an arbitrarily large size.
        // Havk will maybe handle paging/resizing automatically in the future.
        auto tlasDesc = havk::AccelStructBuildDesc();
        tlasDesc.CalculateUpperBoundSizesForTLAS(_device.get(), 1, buildFlags);

        _accels = _device->CreateAccelStructPool();
        HAVK_CHECK(_accels->CreateStorage(blasDesc.AccelStructSize + tlasDesc.AccelStructSize));

        size_t scratchSize = std::max(blasDesc.BuildScratchSize, tlasDesc.BuildScratchSize);
        auto scratchBuffer = _device->CreateBuffer(scratchSize, havk::BufferFlags::DeviceMem);

        auto cmdList = _device->CreateCommandList();
        HAVK_CHECK(_accels->Build(*cmdList, 1, blasDesc, *scratchBuffer));

        // Can only fill instances after BLAS storage has been assigned by Build() or Reserve().
        auto instanceBuffer = _device->CreateBuffer(sizeof(VkAccelerationStructureInstanceKHR), havk::BufferFlags::HostMem_SeqWrite);
        auto instances = instanceBuffer->Slice<VkAccelerationStructureInstanceKHR>();
        instances[0] = _accels->GetInstanceDesc(1, glm::transpose(float4x4(1)));
        tlasDesc.AddInstances(instances);

        // TLAS build depends on built BLAS
        cmdList->Barrier({
            .SrcStages = VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
            .DstStages = VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
        });
        HAVK_CHECK(_accels->Build(*cmdList, 0, tlasDesc, *scratchBuffer));

        cmdList->Submit().Wait();
    }
};

int main(int argc, const char** args) {
    if (argc < 2) {
        printf("Usage: ModelViewer <path to gltf model> [--enable-shadebug]\n");
        return 1;
    }
    AppWindow app;
    if (argc >= 3 && strcmp(args[2], "--enable-shadebug") == 0) {
        havx::Shadebug::Initialize(app._device.get());
    }
    app.LoadModel(args[1]);
    app.RunLoop();
    return 0;
}