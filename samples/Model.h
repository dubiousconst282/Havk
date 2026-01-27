#pragma once

#include <Havk/Havk.h>
#include <glm/ext/quaternion_float.hpp>

#include <string>

#include "Shaders/ModelRender.h"

using namespace havk::vectors;

struct Material : shader::Material {
    std::string Name;
};
struct ModelMesh : shader::Mesh {
    uint32_t NumVertices = 0, NumIndices = 0;
    uint32_t IndexOffset = 0;
    float4 BoundSphere;
};
struct Light : shader::Light {
    std::string Name;
    uint32_t ParentNodeIdx;

    void UpdateCachedValues() {
        InvRadiusSq_ = 1.0f / (Radius * Radius);
        SpotScale_ = 1.0f / std::max(glm::cos(SpotInnerAngle) - glm::cos(SpotOuterAngle), 1e-4f);
        SpotOffset_ = -glm::cos(SpotOuterAngle) * SpotScale_;
    }
};


struct Animation;
struct TransformTRS {
    float3 Translation = {};
    float3 Scale = {};
    glm::quat Rotation = {};

    bool HasValue() const { return Scale.x != 0; }
    float4x4 ToMatrix() const;
};

struct ModelNode {
    std::vector<uint32_t> MeshIndices;
    uint32_t ParentIdx;

    std::vector<uint32_t> Joints;
    std::vector<glm::mat4x3> InverseBindMatrices;
    TransformTRS LocalTRS;
    glm::mat4x3 LocalTransform;
    glm::mat4x3 GlobalTransform;
};
struct Model {
    std::vector<havk::ImagePtr> Images;
    std::vector<Material> Materials;
    std::vector<Light> Lights;
    std::vector<Animation> Animations;

    std::vector<ModelNode> Nodes;
    std::vector<ModelMesh> Meshes;
    std::vector<uint32_t> NodeIndicesPreDFS;

    havk::BufferPtr StorageBuffer;
    havk::BufferSpan<shader::Material> GpuMaterials;
    havk::BufferSpan<shader::Mesh> GpuMeshes;
    havk::BufferSpan<float4> GpuBoundSpheres;
    uint32_t NumLeafNodes = 0, MaxDrawCommands = 0, MaxJointMatrices = 0;

    Model(havk::DeviceContext* device, const std::string& path);

    // Update node transforms from animation, copying resulting matrices and joints into given spans.
    void UpdatePose(Animation* anim, double timestamp, havk::BufferSpan<float3x4> leafGlobalTransforms,
                    havk::BufferSpan<float3x4> dfsJointMatrices);

    static float4x4 InverseAffine(const float4x4& matrix) {
        float3x3 inv = glm::inverse(float3x3(matrix));
        float3 invOfs = -inv * float3(matrix[3]);
        return {
            inv[0].x, inv[0].y, inv[0].z, 0.0f,
            inv[1].x, inv[1].y, inv[1].z, 0.0f,
            inv[2].x, inv[2].y, inv[2].z, 0.0f,
            invOfs.x, invOfs.y, invOfs.z, 1.0f
        };
    }
    // glm::mat4x3 = float3x4
    static float3x4 TruncateMatrixCM34(const float4x4& matrix) {
        return {
            matrix[0][0], matrix[0][1], matrix[0][2],
            matrix[1][0], matrix[1][1], matrix[1][2],
            matrix[2][0], matrix[2][1], matrix[2][2],
            matrix[3][0], matrix[3][1], matrix[3][2],
        };
    }
};

struct Animation {
    enum LerpMode : uint8_t { kLerpNearest, kLerpLinear, kLerpSlerp, kLerpCubic };
    struct Sampler {
        uint32_t LastFrameIndex;
        uint32_t FrameCount;
        uint32_t DataOffset;
        LerpMode LerpMode;
    };
    struct Channel {
        Sampler SamplerTRS[3] = {};  // one per T/R/S
    };
    std::vector<Channel> Channels;
    std::vector<uint32_t> NodeToChannelMap; // UINT_MAX if empty
    std::vector<float> KeyframeData;
    float Duration = 0;

    void Interpolate(float timestamp, uint32_t channelIdx, TransformTRS& transform);
};