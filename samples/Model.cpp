#include "Model.h"

#define CGLTF_IMPLEMENTATION
#include <cgltf.h>
#include <stb_image.h>

#include <glm/gtc/type_ptr.hpp>
#include <stdexcept>

using namespace havk::vectors;

struct CopyQueue {
    havk::BufferPtr Buffer;
    havk::CommandListPtr CmdList;
    size_t NextOffset = 0;

    CopyQueue(havk::DeviceContext* ctx, size_t bufferCap) {
        Buffer = ctx->CreateBuffer(bufferCap, havk::BufferFlags::HostMem_Cached);
        CmdList = ctx->CreateCommandList();
    }

    template<typename T>
    T* WriteOrStage(havk::BufferSpan<T> dest) {
        if (dest.is_host_visible()) return dest.data();

        auto tempSpan = Alloc(dest.size_bytes());
        CmdList->CopyBuffer(tempSpan.source_buffer(), dest.source_buffer(), tempSpan.offset_bytes(),
                            dest.offset_bytes(), dest.size_bytes());
        return (T*)tempSpan.data();
    }

    havk::BufferSpan<uint8_t> Alloc(size_t numBytes) {
        HAVK_ASSERT(numBytes <= Buffer->Size);

        if (NextOffset + numBytes > Buffer->Size) Flush();
        
        size_t destOffset = NextOffset;
        NextOffset += numBytes;
        return Buffer->Slice<uint8_t>(destOffset, numBytes);
    }

    void Flush() {
        if (NextOffset == 0) return;
        CmdList->Barrier();
        CmdList->Submit().Wait();
        CmdList->Context->GarbageCollect();
        CmdList->Begin();
        NextOffset = 0;
    }
};

static uint32_t PackNormal(float3 value) {
    uint3 q = uint3(glm::clamp(value, -1.0f, 1.0f) * 511.0f + 511.5f);
    return q.x | (q.y << 10) | (q.z << 20);
}

static havk::ImageHandle LoadTexture(cgltf_data* gltf, const cgltf_texture* texInfo, const std::string& baseDir,
                                     std::vector<havk::ImagePtr>& cache, CopyQueue& copyQueue, VkFormat format) {
    if (!texInfo || !texInfo->image) return {};
    cgltf_image* imgInfo = texInfo->image;

    size_t imageIdx = cgltf_image_index(gltf, imgInfo);
    if (cache[imageIdx]) return *cache[imageIdx];

    int width, height;
    uint8_t* pixels = nullptr;

    if (imgInfo->uri && strncmp(imgInfo->uri, "data:", 5) != 0) {
        std::string path = baseDir + "/" + imgInfo->uri;
        pixels = stbi_load(path.c_str(), &width, &height, nullptr, 4);
    } else if (imgInfo->buffer_view) {
        auto* data = (const uint8_t*)imgInfo->buffer_view->buffer->data;
        pixels = stbi_load_from_memory(&data[imgInfo->buffer_view->offset], imgInfo->buffer_view->size, &width, &height, nullptr, 4);
    }
    if (pixels == nullptr) {
        throw std::runtime_error(std::string("Failed to load GLTF image: ") + stbi_failure_reason());
    }
    // stbi can't take a buffer directly, so we need a redundant copy.
    auto stagingBuffer = copyQueue.Alloc((size_t)(width * height * 4));
    memcpy(stagingBuffer.data(), pixels, stagingBuffer.size_bytes());
    stbi_image_free(pixels);

    auto label = havk::DebugLabel("gltf_%d-%s", imageIdx, imgInfo->name ? imgInfo->name : (imgInfo->uri ? imgInfo->uri : "unnamed"));
    auto image = copyQueue.CmdList->Context->CreateImage({
        .Format = format,
        .Usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .Size = { width, height, 1 },
        .MipLevels = VK_REMAINING_MIP_LEVELS,
    }, label);

    copyQueue.CmdList->CopyBufferToImage({ .SrcData = stagingBuffer, .DstImage = *image, .GenerateMips = true });

    cache[imageIdx] = std::move(image);
    return *cache[imageIdx];
}

static Animation ParseAnimation(const cgltf_data* gltf, const cgltf_animation& srcAnim) {
    Animation anim;
    anim.NodeToChannelMap = std::vector<uint32_t>(gltf->nodes_count, UINT_MAX);

    for (uint32_t i = 0; i < srcAnim.channels_count; i++) {
        auto& channel = srcAnim.channels[i];
        auto& sampler = *channel.sampler;
        if (!channel.target_node) continue;

        int prop = channel.target_path == cgltf_animation_path_type_translation ? 0 :
                   channel.target_path == cgltf_animation_path_type_rotation    ? 1 :
                   channel.target_path == cgltf_animation_path_type_scale       ? 2 :
                                                                                  -1;
        if (prop < 0) continue;

        size_t offset = anim.KeyframeData.size();
        anim.KeyframeData.resize(offset + sampler.input->count + sampler.output->count * 4);

        float* timestamps = &anim.KeyframeData[offset];
        float* controlPoints = &anim.KeyframeData[offset + sampler.input->count];

        cgltf_accessor_unpack_floats(sampler.input, timestamps, sampler.input->count);

        if (sampler.output->type == cgltf_type_vec4) {
            cgltf_accessor_unpack_floats(sampler.output, controlPoints, sampler.output->count * 4);
        } else {
            assert(sampler.output->type == cgltf_type_vec3);
            for (uint32_t i = 0; i < sampler.output->count; i++) {
                cgltf_accessor_read_float(sampler.output, i, &controlPoints[i * 4], 3);
            }
        }

        anim.Duration = glm::max(anim.Duration, timestamps[sampler.input->count - 1]);

        uint32_t nodeIdx = cgltf_node_index(gltf, channel.target_node);
        uint32_t& channelIdx = anim.NodeToChannelMap[nodeIdx];
        if (channelIdx == UINT_MAX) {
            channelIdx = anim.Channels.size();
            anim.Channels.push_back({});
        }
        anim.Channels[channelIdx].SamplerTRS[prop] = {
            .FrameCount = (uint32_t)sampler.input->count,
            .DataOffset = (uint32_t)offset,
            .LerpMode = sampler.interpolation == cgltf_interpolation_type_linear ?
                            (channel.target_path == cgltf_animation_path_type_rotation ? Animation::kLerpSlerp : Animation::kLerpLinear) :
                        sampler.interpolation == cgltf_interpolation_type_cubic_spline ? Animation::kLerpCubic :
                                                                                         Animation::kLerpNearest,
        };
    }
    return anim;
}

Model::Model(havk::DeviceContext* device, const std::string& path) {
    cgltf_options gltfOptions = { };
    cgltf_data* gltf = NULL;
    cgltf_result parseResult = cgltf_parse_file(&gltfOptions, path.c_str(), &gltf);
    if (parseResult != cgltf_result_success) {
        throw std::runtime_error("Failed to parse GLTF");
    }

    parseResult = cgltf_load_buffers(&gltfOptions, gltf, path.c_str());

    if (parseResult != cgltf_result_success) {
        throw std::runtime_error("Failed to load associated GLTF data");
    }
    std::string baseDir = path.substr(0, path.find_last_of("/\\"));

    size_t dataSize = 0;
    uint32_t numIndices = 0;
    uint32_t numPrimitives = 0;
    uint32_t maxVerticesPerMesh = 0;

    // Calculate required storage size
    for (uint32_t meshIdx = 0; meshIdx < gltf->meshes_count; meshIdx++) {
        auto* mesh = &gltf->meshes[meshIdx];

        for (uint32_t primIdx = 0; primIdx < mesh->primitives_count; primIdx++) {
            auto* prim = &mesh->primitives[primIdx];

            numIndices += prim->indices->count;
            uint32_t numVertices = cgltf_find_accessor(prim, cgltf_attribute_type_position, 0)->count;

            maxVerticesPerMesh = std::max(maxVerticesPerMesh, numVertices);
            dataSize += (numVertices * sizeof(float3) + 15) & ~15;

            if (cgltf_find_accessor(prim, cgltf_attribute_type_texcoord, 0) || cgltf_find_accessor(prim, cgltf_attribute_type_normal, 0)) {
                dataSize += numVertices * sizeof(uint4); // packed attribs
            }
            if (cgltf_find_accessor(prim, cgltf_attribute_type_joints, 0)) {
                dataSize += numVertices * sizeof(uint4); // packed joints + weights
            }
        }
        numPrimitives += mesh->primitives_count;
    }

    // Allocate storage buffer
    StorageBuffer = device->CreateBuffer(UINT_MAX, havk::BufferFlags::DeferredAlloc);
    auto bufferSpan = StorageBuffer->Slice<uint8_t>();

    auto indexData = bufferSpan.bump_slice<uint32_t>(numIndices);
    auto vertexData = bufferSpan.bump_slice(dataSize, 16);
    GpuMeshes = bufferSpan.bump_slice<shader::Mesh>(numPrimitives);
    GpuMaterials = bufferSpan.bump_slice<shader::Material>(gltf->materials_count);
    GpuBoundSpheres = bufferSpan.bump_slice<float4>(numPrimitives);

    bufferSpan.commit_bump_alloc(havk::BufferFlags::DeviceMem_MappedIfOptimal);

    auto copyQueue = CopyQueue(device, 1024 * 1024 * 128);
    auto meshPrimStartIdx = std::vector<uint32_t>(gltf->meshes_count);
    auto tempPositions = std::vector<float3>(maxVerticesPerMesh);

    // Copy to storage buffers
    for (uint32_t meshIdx = 0; meshIdx < gltf->meshes_count; meshIdx++) {
        auto* mesh = &gltf->meshes[meshIdx];
        meshPrimStartIdx[meshIdx] = Meshes.size();

        for (uint32_t primIdx = 0; primIdx < mesh->primitives_count; primIdx++) {
            auto* prim = &mesh->primitives[primIdx];

            ModelMesh& mesh = Meshes.emplace_back();
            mesh.MaterialId = cgltf_material_index(gltf, prim->material);

            auto dstIndices = indexData.bump_slice(prim->indices->count);
            cgltf_accessor_unpack_indices(prim->indices, copyQueue.WriteOrStage(dstIndices), 4, prim->indices->count);
            mesh.IndexOffset = dstIndices.offset();
            mesh.NumIndices = dstIndices.size();

            // Positions
            auto* srcPositions = cgltf_find_accessor(prim, cgltf_attribute_type_position, 0);
            uint32_t numVertices = srcPositions->count;
            assert(cgltf_num_components(srcPositions->type) == 3);
            cgltf_accessor_unpack_floats(srcPositions, (float*)tempPositions.data(), numVertices * 3);

            auto dstPositions = vertexData.bump_slice<float>(numVertices * 3);
            memcpy(copyQueue.WriteOrStage(dstPositions), tempPositions.data(), dstPositions.size_bytes());
            mesh.Positions = dstPositions.device_addr();
            mesh.NumVertices = numVertices;

            // Bound sphere
            glm::dvec3 centerPosSum = {};
            for (uint32_t i = 0; i < numVertices; i++) {
                centerPosSum += tempPositions[i];
            }
            float3 centerPos = float3(centerPosSum / (double)numVertices);

            float radius = 0;
            for (uint32_t i = 0; i < numVertices; i++) {
                radius = std::max(radius, glm::distance(tempPositions[i], centerPos));
            }
            mesh.BoundSphere = float4(centerPos, radius);

            // Normals
            auto* srcTexcoords = cgltf_find_accessor(prim, cgltf_attribute_type_texcoord, 0);
            auto* srcNormals = cgltf_find_accessor(prim, cgltf_attribute_type_normal, 0);
            auto* srcTangents = cgltf_find_accessor(prim, cgltf_attribute_type_tangent, 0);

            if (srcTexcoords || srcNormals) {
                auto dstAttribs = vertexData.bump_slice<uint4>(numVertices, 16);
                mesh.Attributes = dstAttribs;

                auto packedData = copyQueue.WriteOrStage(dstAttribs);

                for (uint32_t i = 0; i < numVertices; i++) {
                    uint4 packed = uint4(0);

                    if (srcTexcoords) {
                        cgltf_accessor_read_float(srcTexcoords, i, (float*)&packed.x, 2);
                    }
                    if (srcNormals) {
                        float3 normal;
                        float4 tangent = float4(0);
                        cgltf_accessor_read_float(srcNormals, i, &normal.x, 3);
                        if (srcTangents) cgltf_accessor_read_float(srcTangents, i, &tangent.x, 4);

                        packed.z = PackNormal({ normal.x, normal.y, normal.z });
                        packed.w = PackNormal({ tangent.x, tangent.y, tangent.z });
                        packed.w |= tangent.w < 0 ? (1 << 31) : 0;
                    }
                    packedData[i] = packed;
                }
            }

            // Skin joints
            auto srcJoints = cgltf_find_accessor(prim, cgltf_attribute_type_joints, 0);
            auto srcWeights = cgltf_find_accessor(prim, cgltf_attribute_type_weights, 0);

            if (srcJoints && srcWeights) {
                auto dstJoints = vertexData.bump_slice<uint4>(numVertices, 16);
                mesh.JointAndWeights = dstJoints;

                auto packedData = copyQueue.WriteOrStage(dstJoints);

                for (uint32_t i = 0; i < numVertices; i++) {
                    uint4 jointIds;
                    float4 weights;
                    cgltf_accessor_read_uint(srcJoints, i, &jointIds.x, 4);
                    cgltf_accessor_read_float(srcWeights, i, &weights.x, 4);

                    uint16_t4 quantJointIds = uint16_t4(jointIds);
                    uint16_t4 quantWeights = glm::clamp(weights, 0.0f, 1.0f) * 65535.0f + 0.5f;

                    memcpy(&packedData[i].x, &quantJointIds, 8);
                    memcpy(&packedData[i].z, &quantWeights, 8);
                }
            }
        }
    }
    
    // Load materials and textures
    Images.resize(gltf->images_count);
    Materials.resize(gltf->materials_count);

    for (uint32_t matIdx = 0; matIdx < gltf->materials_count; matIdx++) {
        cgltf_material& srcMat = gltf->materials[matIdx];
        cgltf_pbr_metallic_roughness& pbrInfo = srcMat.pbr_metallic_roughness;
        Material& mat = Materials[matIdx];

        mat.Name = srcMat.name ? srcMat.name : "";
        mat.DoubleSided = srcMat.double_sided;
        mat.AlphaCutoff = srcMat.alpha_mode == cgltf_alpha_mode_opaque ? 0.0f : srcMat.alpha_cutoff;

        mat.BaseColorFactor = glm::make_vec4(pbrInfo.base_color_factor);
        mat.MetallicFactor = pbrInfo.metallic_factor;
        mat.RoughnessFactor = pbrInfo.roughness_factor;

        mat.AlbedoTex = LoadTexture(
            gltf, pbrInfo.base_color_texture.texture,
            baseDir, Images, copyQueue, VK_FORMAT_R8G8B8A8_SRGB);

        mat.MetallicRoughnessTex = LoadTexture(
            gltf, pbrInfo.metallic_roughness_texture.texture,
            baseDir, Images, copyQueue, VK_FORMAT_R8G8B8A8_UNORM);

        mat.NormalTex = LoadTexture(
            gltf, srcMat.normal_texture.texture,
            baseDir, Images, copyQueue, VK_FORMAT_R8G8B8A8_UNORM);
    }

    // Copy metadata to GPU all at once
    auto destMeshes = copyQueue.WriteOrStage(GpuMeshes);
    for (uint32_t i = 0; i < Meshes.size(); i++) {
        destMeshes[i] = Meshes[i];
    }
    auto destBounds = copyQueue.WriteOrStage(GpuBoundSpheres);
    for (uint32_t i = 0; i < Meshes.size(); i++) {
        destBounds[i] = Meshes[i].BoundSphere;
    }
    auto destMaterials = copyQueue.WriteOrStage(GpuMaterials);
    for (uint32_t i = 0; i < Materials.size(); i++) {
        destMaterials[i] = Materials[i];
    }
    copyQueue.Flush();

    // Convert nodes
    auto recurseNode = [&](auto& recurseNode, cgltf_node* srcNode, const float4x4& parentTransform) -> void {
        uint32_t nodeIdx = cgltf_node_index(gltf, srcNode);
        ModelNode& node = Nodes[nodeIdx];
        node.ParentIdx = srcNode->parent ? cgltf_node_index(gltf, srcNode->parent) : UINT_MAX;

        NodeIndicesPreDFS.push_back(nodeIdx);

        if (!srcNode->has_matrix) {
            node.LocalTRS = {
                .Translation = glm::make_vec3(srcNode->translation),
                .Scale = glm::make_vec3(srcNode->scale),
                .Rotation = glm::quat::wxyz(srcNode->rotation[3], srcNode->rotation[0], srcNode->rotation[1], srcNode->rotation[2]),
            };
        }
        float4x4 localTransform;
        cgltf_node_transform_local(srcNode, &localTransform[0][0]);
        node.LocalTransform = localTransform;
        node.GlobalTransform = parentTransform * localTransform;

        if (srcNode->mesh) {
            MaxDrawCommands += srcNode->mesh->primitives_count;
            NumLeafNodes++;

            uint32_t meshStartIdx = meshPrimStartIdx[cgltf_mesh_index(gltf, srcNode->mesh)];
            for (uint32_t i = 0; i < srcNode->mesh->primitives_count; i++) {
                node.MeshIndices.push_back(meshStartIdx + i);
            }
        }
        if (srcNode->skin) {
            MaxJointMatrices += srcNode->skin->joints_count;
            
            node.Joints.resize(srcNode->skin->joints_count);
            for (uint32_t i = 0; i < srcNode->skin->joints_count; i++) {
                node.Joints[i] = cgltf_node_index(gltf, srcNode->skin->joints[i]);
            }

            if (auto* ibms = srcNode->skin->inverse_bind_matrices) {
                node.InverseBindMatrices.resize(ibms->count);

                for (uint32_t i = 0; i < ibms->count; i++) {
                    float4x4 m;
                    cgltf_accessor_read_float(ibms, i, &m[0][0], 16);
                    node.InverseBindMatrices[i] = glm::mat4x3(m);
                }
            }
        }
        if (srcNode->light) {
            Light& light = Lights.emplace_back();
            light.Name = srcNode->light->name;
            light.Type = srcNode->light->type == cgltf_light_type_directional ? 0 : srcNode->light->type == cgltf_light_type_spot ? 2 : 1;
            light.ParentNodeIdx = nodeIdx;
            light.Position = node.GlobalTransform[3];
            light.Direction = glm::normalize(-node.GlobalTransform[2]); // T * float3(0, 0, -1)

            light.PackedColor = glm::packUnorm4x8(float4(glm::make_vec3(srcNode->light->color), 0));
            light.Intensity = srcNode->light->intensity;
            light.SpotInnerAngle = srcNode->light->spot_inner_cone_angle;
            light.SpotOuterAngle = srcNode->light->spot_outer_cone_angle;
            light.Radius = srcNode->light->range > 0 ? srcNode->light->range : 1e+6;
            light.UpdateCachedValues();
        }

        for (uint32_t i = 0; i < srcNode->children_count; i++) {
            recurseNode(recurseNode, srcNode->children[i], node.GlobalTransform);
        }
    };

    Nodes.resize(gltf->nodes_count);
    NodeIndicesPreDFS.reserve(gltf->nodes_count);

    auto& scene = gltf->scenes[0];
    for (uint32_t i = 0; i < scene.nodes_count; i++) {
        recurseNode(recurseNode, scene.nodes[i], float4x4(1));
    }

    for (uint32_t i = 0; i < gltf->animations_count; i++) {
        Animations.push_back(ParseAnimation(gltf, gltf->animations[i]));
    }
}
void Model::UpdatePose(Animation* anim, double timestamp, havk::BufferSpan<float3x4> leafGlobalTransforms,
                       havk::BufferSpan<float3x4> dfsJointMatrices) {
    float cyclingTimestamp = fmod(timestamp, anim->Duration);

    for (uint32_t nodeIdx : NodeIndicesPreDFS) {
        ModelNode& node = Nodes[nodeIdx];
        float4x4 localTransform = float4x4(node.LocalTransform);

        if (anim->NodeToChannelMap[nodeIdx] != UINT_MAX) {
            TransformTRS localTRS = node.LocalTRS; // copy
            anim->Interpolate(cyclingTimestamp, anim->NodeToChannelMap[nodeIdx], localTRS);
            localTransform = localTRS.ToMatrix();
        }
        float4x4 parentTransform = node.ParentIdx != UINT_MAX ? float4x4(Nodes[node.ParentIdx].GlobalTransform) : float4x4(1);
        node.GlobalTransform = parentTransform * localTransform;

        if (node.MeshIndices.size() > 0 && leafGlobalTransforms.size() > 0) {
            leafGlobalTransforms[0] = TruncateMatrixCM34(node.GlobalTransform);
            leafGlobalTransforms.bump_slice(1);
        }
        if (node.Joints.size() > 0 && dfsJointMatrices.size() >= node.Joints.size()) {
            float4x4 inverseTransform = InverseAffine(node.GlobalTransform);

            for (uint32_t i = 0; i < node.Joints.size(); i++) {
                float4x4 jointMatrix = inverseTransform * float4x4(Nodes[node.Joints[i]].GlobalTransform);

                if (i < node.InverseBindMatrices.size()) {
                    jointMatrix *= float4x4(node.InverseBindMatrices[i]);
                }
                dfsJointMatrices[i] = TruncateMatrixCM34(jointMatrix);
            }
            dfsJointMatrices.bump_slice(node.Joints.size());
        }
    }
    for (Light& light : Lights) {
        ModelNode& node = Nodes[light.ParentNodeIdx];
        light.Position = node.GlobalTransform[3];
        light.Direction = glm::normalize(-node.GlobalTransform[2]);  // T * float3(0, 0, -1)
    }
}

void Animation::Interpolate(float timestamp, uint32_t channelIdx, TransformTRS& transform) {
    Channel& ch = Channels[channelIdx];

    for (uint32_t j = 0; j < 3; j++) {
        Sampler& sampler = ch.SamplerTRS[j];
        if (sampler.FrameCount == 0) continue;

        const float* keyTimestamps = &KeyframeData[sampler.DataOffset];
        const float4* keyPoints = (float4*)&KeyframeData[sampler.DataOffset + sampler.FrameCount];

        // Advance or reset frame index based on current time
        uint32_t& currIdx = sampler.LastFrameIndex;
        if (timestamp < keyTimestamps[currIdx + 1]) currIdx = 0;
        while (currIdx + 1 < sampler.FrameCount - 1 && timestamp > keyTimestamps[currIdx + 1]) currIdx++;

        float prevTime = keyTimestamps[currIdx], nextTime = keyTimestamps[currIdx + 1];
        float td = nextTime - prevTime;
        float t = glm::clamp((timestamp - prevTime) / td, 0.0f, 1.0f);

        // Interpolate
        float4 res;

        if (sampler.LerpMode == kLerpLinear) {
            float4 p0 = keyPoints[currIdx], p1 = keyPoints[currIdx + 1];
            res = glm::mix(p0, p1, t);
        } else if (sampler.LerpMode == kLerpSlerp) {
            float4 p0 = keyPoints[currIdx], p1 = keyPoints[currIdx + 1];
            auto qr = glm::slerp(glm::quat::wxyz(p0.w, p0.x, p0.y, p0.z), glm::quat::wxyz(p1.w, p1.x, p1.y, p1.z), t);
            res = float4(qr.x, qr.y, qr.z, qr.w);
        } else if (sampler.LerpMode == kLerpNearest) {
            res = keyPoints[currIdx];
        } else if (sampler.LerpMode == kLerpCubic) {
            float4 v0 = keyPoints[(currIdx + 0) * 3 + 1];
            float4 b0 = keyPoints[(currIdx + 0) * 3 + 2];
            float4 a1 = keyPoints[(currIdx + 1) * 3 + 0];
            float4 v1 = keyPoints[(currIdx + 1) * 3 + 1];
            float t2 = t * t;
            float t3 = t2 * t;

            // clang-format off
            res = ( 2 * t3 - 3 * t2 + 1) * v0 + 
                  (     t3 - 2 * t2 + t) * (b0 * td) +
                  (-2 * t3 + 3 * t2    ) * v1 +
                  (     t3 -     t2    ) * (a1 * td);
            // clang-format on
        }

        // Store result into target node's transform
        if (j == 0) transform.Translation = float3(res);
        if (j == 1) transform.Rotation = glm::normalize(glm::quat::wxyz(res.w, res.x, res.y, res.z));
        if (j == 2) transform.Scale = float3(res);
    }
}

float4x4 TransformTRS::ToMatrix() const {
    float4x4 M = glm::scale(glm::mat4_cast(Rotation), Scale);
    M[3] = float4(Translation, 1);
    return M;
}