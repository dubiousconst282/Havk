#pragma once

#include <cstring>
#include <vector>

#include <vulkan/vulkan.h>

namespace havk {

struct ModuleDesc {
    enum Flags {
        kNoReload = 1 << 0,     // Skip hot-reloading
    };
    const uint32_t* Code;       // SPIR-V binary data
    uint32_t CodeSize;          // SPIR-V binary size
    uint32_t Flags = 0;
    const char* EntryPoint;
    const char* SourcePath;     // Optional. For labeling and hot-reload support.

    VkShaderStageFlagBits GetStage() const;
};

struct ImageHandle {
    uint32_t HeapIndex = 0;
    explicit operator bool() const { return HeapIndex != 0; }
};
struct SamplerHandle {
    uint32_t HeapIndex = 0;
    explicit operator bool() const { return HeapIndex != 0; }
};
struct AccelStructHandle {
    uint64_t Address = 0;
    explicit operator bool() const { return Address != 0; }
};

template<typename T>
struct DevicePtr {
    VkDeviceAddress addr;

    constexpr DevicePtr(std::nullptr_t = {}) : addr(0) { }
    DevicePtr(VkDeviceAddress addr_) : addr(addr_) { }
    explicit operator VkDeviceAddress() const { return addr; }

    DevicePtr operator+(intptr_t elemOffset) const { return { addr + elemOffset * sizeof(T) }; }
    DevicePtr operator-(intptr_t elemOffset) const { return *this + -elemOffset; }
};

template<typename T>
struct SpecOpt {
    T value;
    bool present;

    constexpr SpecOpt() : value(), present(false) {}
    constexpr SpecOpt(const T& value) : value(value), present(true) {}
};

struct SpecConstMap {
    std::vector<VkSpecializationMapEntry> Entries;
    std::vector<uint8_t> ConstantData;

    template<typename T>
    void Add(uint32_t constId, const T& value) {
        uint32_t offset = (uint32_t)ConstantData.size();
        Entries.push_back({ .constantID = constId, .offset = offset, .size = sizeof(T) });
        ConstantData.resize(offset + sizeof(T));
        memcpy(&ConstantData[offset], &value, sizeof(T));
    }
    template<>
    void Add(uint32_t constId, const bool& value) {
        Add(constId, value ? VK_TRUE : VK_FALSE);
    }
    VkSpecializationInfo GetSpecInfo() const {
        return {
            .mapEntryCount = (uint32_t)Entries.size(),
            .pMapEntries = Entries.data(),
            .dataSize = ConstantData.size(),
            .pData = ConstantData.data(),
        };
    }
};

// Same as VkDrawIndirectCommand, with defaults.
struct DrawCommand {
    uint32_t NumVertices = 0;
    uint32_t NumInstances = 1;
    uint32_t VertexOffset = 0;
    uint32_t InstanceOffset = 0;
};

// Same as VkDrawIndexedIndirectCommand, with defaults.
struct DrawIndexedCommand {
    uint32_t NumIndices = 0;
    uint32_t NumInstances = 1;
    uint32_t IndexOffset = 0;
    uint32_t VertexOffset = 0;
    uint32_t InstanceOffset = 0;
};

};  // namespace havk


#ifdef HAVK_VECTORS_OVERRIDE_INCLUDE
#include HAVK_VECTORS_OVERRIDE_INCLUDE
#else
#include <glm/glm.hpp>

namespace havk::vectors {
using float2 = glm::vec2;
using float3 = glm::vec3;
using float4 = glm::vec4;

using int2 = glm::ivec2;
using int3 = glm::ivec3;
using int4 = glm::ivec4;

using uint2 = glm::uvec2;
using uint3 = glm::uvec3;
using uint4 = glm::uvec4;

using bool2 = glm::bvec2;
using bool3 = glm::bvec3;
using bool4 = glm::bvec4;

using int8_t2 = glm::i8vec2;
using int8_t3 = glm::i8vec3;
using int8_t4 = glm::i8vec4;

using uint8_t2 = glm::u8vec2;
using uint8_t3 = glm::u8vec3;
using uint8_t4 = glm::u8vec4;

using int16_t2 = glm::i16vec2;
using int16_t3 = glm::i16vec3;
using int16_t4 = glm::i16vec4;

using uint16_t2 = glm::u16vec2;
using uint16_t3 = glm::u16vec3;
using uint16_t4 = glm::u16vec4;

using int64_t2 = glm::i64vec2;
using int64_t3 = glm::i64vec3;
using int64_t4 = glm::i64vec4;

using uint64_t2 = glm::u64vec2;
using uint64_t3 = glm::u64vec3;
using uint64_t4 = glm::u64vec4;

using float2x2 = glm::mat2x2;
using float2x3 = glm::mat2x3;
using float2x4 = glm::mat2x4;
using float3x2 = glm::mat3x2;
using float3x3 = glm::mat3x3;
using float3x4 = glm::mat3x4;
using float4x2 = glm::mat4x2;
using float4x3 = glm::mat4x3;
using float4x4 = glm::mat4x4;

// Clang's _Float16 will randomly cause linking failures on Windows, even with AVX2 support:
// https://github.com/llvm/llvm-project/issues/112870
#if (__clang__ || __GNUC__) && !_WIN32
using float16_t = _Float16;
#else
struct float16_t {
    constexpr float16_t() : bits(0) {}
    float16_t(float value) { bits = glm::detail::toFloat16(value); }
    operator float() const { return glm::detail::toFloat32(bits); }

private:
    glm::detail::hdata bits;
};
#endif

using float16_t2 = glm::vec<2, float16_t>;
using float16_t3 = glm::vec<3, float16_t>;
using float16_t4 = glm::vec<4, float16_t>;

};  // namespace havk::vectors
#endif
