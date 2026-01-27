import Havk.Core;
import Havk.Math;
import Havk.DataPacking;
import Havk.ColorUtils;
import Havk.DebugTools;

#define DEBUG_CULLING 1

#ifndef ENABLE_RAY_TRACING
    #define ENABLE_RAY_TRACING 1
#endif

struct Material {
    TextureHandle2D<float4> AlbedoTex;
    TextureHandle2D<float4> NormalTex;
    TextureHandle2D<float4> MetallicRoughnessTex;

    float4 BaseColorFactor;
    float MetallicFactor;
    float RoughnessFactor;

    float AlphaCutoff;
    bool DoubleSided;
}
struct Mesh {
    uint32_t MaterialId;
    uint32_t NumVertices;
    float3* Positions;
    uint4* Attributes;  // TexCoords (2x F32), Normals+Tangents (2x RGB10A2)
    uint4* JointAndWeights;
}
enum LightType : uint32_t { Directional, Point, Spot };
struct Light {
    uint32_t Type;
    float3 Position;
    float3 Direction;
    uint PackedColor;
    float Intensity; // cd or lux
    float Radius;
    float SpotInnerAngle, SpotOuterAngle;

    // Cached
    float InvRadiusSq_, SpotScale_, SpotOffset_;
    uint _padding[1]; // 64 bytes

    property float3 Color {
        get { return unpackUnorm4x8ToFloat(PackedColor).rgb; }
    }

    float GetAttenuation(float3 surfacePosWS) {
        if (Type == LightType::Directional) return 1.0;

        // from Filament, equivalent to GLTF spec: `attenuation = max( min( 1.0 - ( dist / range )^4, 1 ), 0 ) / dist^2`
        // https://github.com/KhronosGroup/glTF/blob/main/extensions/2.0/Khronos/KHR_lights_punctual/README.md
        float3 posToLight = Position - surfacePosWS;
        float distanceSquare = dot(posToLight, posToLight);
        float factor = distanceSquare * InvRadiusSq_;
        float smoothFactor = max(1.0 - factor * factor, 0.0);
        float attenuation = (smoothFactor * smoothFactor) / max(distanceSquare, 1e-4);

        if (Type == LightType::Spot) {
            // float spotScale = 1.0 / max(cos(SpotInnerAngle) - cos(SpotOuterAngle), 1e-4);
            // float spotOffset = -cos(SpotOuterAngle) * spotScale;
            float cd = dot(-Direction, normalize(posToLight));
            float spotAttenuation = saturate(cd * SpotScale_ + SpotOffset_);
            attenuation *= spotAttenuation * spotAttenuation;
        }
        return attenuation;
    }
};
struct MeshDrawCommand {
    havk::DrawIndexedCommand Cmd;
    uint32_t MeshIdx;
    uint32_t TransformIdx;
    uint32_t JointsOffset;
};
struct ModelDrawParams {
    MeshDrawCommand* Draws;
    Mesh* Meshes;
    float3x4* Transforms;
    Material* Materials;
    float3 ViewPos;
    float4x4 ViewProj;
};

struct MeshShadedVertex {
    float3 Position;
    float2 TexCoord;
    float3 Normal;
    float3 Tangent;

    nointerpolation uint _extra;

    property uint MaterialId {
        get { return _extra & 0xFFFFFF; }
    }
    property float TangentW {
        get { return (int)_extra < 0 ? -1.0f : +1.0f; }
    }
}

[shader("vertex")]
void VS_ModelIndirect(
    uniform ModelDrawParams pc,
    uint vertexId: SV_VertexID, uint drawId: SV_DrawIndex,
    out float4 clipPos: SV_Position, out MeshShadedVertex vtx)
{
    MeshDrawCommand draw = pc.Draws[drawId];
    Mesh mesh = pc.Meshes[draw.MeshIdx];
    float3x4 transform = pc.Transforms[draw.TransformIdx];

    if (mesh.JointAndWeights != nullptr && draw.JointsOffset != uint.maxValue) {
        float3x4* jointMatrices = &pc.Transforms[draw.JointsOffset];
        uint4 packedJoints = loadAligned<16>(&mesh.JointAndWeights[vertexId]);
        uint4 joints = bit_cast<uint16_t4>(packedJoints.xy);
        float4 weights = bit_cast<uint16_t4>(packedJoints.zw) / 65535.0f;

        float3x4 skinMatrix =
            weights.x * jointMatrices[joints.x] +
            weights.y * jointMatrices[joints.y] +
            weights.z * jointMatrices[joints.z] +
            weights.w * jointMatrices[joints.w];

        transform = float3x4(mul(float4x4(transform, float4(0, 0, 0, 1)), float4x4(skinMatrix, float4(0, 0, 0, 1))));
    }
    vtx.Position = mul(transform, float4(mesh.Positions[vertexId], 1));

    uint4 packedAttribs = loadAligned<16>(&mesh.Attributes[vertexId]);
    vtx.TexCoord = asfloat(packedAttribs.xy);

    float3 normal = havk::UnpackRGB10A2(packedAttribs.z).xyz * 2 - 1;
    float3 tangent = havk::UnpackRGB10A2(packedAttribs.w).xyz * 2 - 1;
    vtx.Normal = normalize(mul(float3x3(transform), normal));
    vtx.Tangent = normalize(mul(float3x3(transform), tangent));
    vtx._extra = mesh.MaterialId | (packedAttribs.w & (1 << 31));

    clipPos = mul(pc.ViewProj, float4(vtx.Position, 1.0));
}

[shader("fragment")]
void FS_ModelDeferred(uniform ModelDrawParams pc, MeshShadedVertex vtx, out uint2 data, float2 fragCoord: SV_Position) {
    Material mat = pc.Materials[vtx.MaterialId];
    float4 baseColor = mat.BaseColorFactor;

    // According to spec, we don't actually need NonUniformInstance here because we have exactly one material per draw command.
    if (mat.AlbedoTex) {
        let colorSampler = havk::GetSampler(havk::FilterMode::Anisotropic, havk::WrapMode::Repeat);
        baseColor *= mat.AlbedoTex.Sample(colorSampler, vtx.TexCoord);
    }
    // TODO-PERF: specialize for alpha tests, `discard` disables early depth tests and has a slight performance impact, 10~15%.
    if (baseColor.a < mat.AlphaCutoff) discard;

    float3 normalWS = vtx.Normal;
    float metallic = mat.MetallicFactor;
    float roughness = mat.RoughnessFactor;

    let propSampler = havk::GetSampler(havk::FilterMode::Linear, havk::WrapMode::Repeat);

    if (mat.NormalTex) {
        float3 normalTS = mat.NormalTex.Sample(propSampler, vtx.TexCoord).xyz * 2 - 1;

        float3 tangent = vtx.Tangent.xyz;
        float3 bitangent = cross(normalWS, tangent) * vtx.TangentW;
        normalWS = normalize(mul(normalTS, float3x3(tangent, bitangent, normalWS)));
    }
    if (mat.MetallicRoughnessTex) {
        float4 mr = mat.MetallicRoughnessTex.Sample(propSampler, vtx.TexCoord);
        metallic *= mr.b;
        roughness *= mr.g;
    }
    data.x = packUnorm4x8(float4(baseColor.rgb, metallic));
    data.y = havk::PackOctahedron<24>(normalWS) | uint(roughness * 255.0 + 0.5) << 24;
}

[numthreads(8, 8)]
void CS_GBufferResolve(
    uniform ImageHandle2D<float4> outputImage,
    uniform TextureHandle2D<uint2> gbuffer,
    uniform TextureHandle2D<float> depthBuffer,
    uniform AccelStructHandle sceneTlas,
    uniform Light* lights,
    uniform uint numLights,
    uniform float3 viewPos,
    uniform float4x4 invProj,
    uint2 screenPos: SV_DispatchThreadID)
{
    if (any(screenPos >= gbuffer.Size)) return;
    float depth = depthBuffer.Load(screenPos);

    if (depth <= 0.0) {  // "sky"
        outputImage.Store(screenPos, float4(0.5, 0.6, 0.8, 1));
        return;
    }
    float4 unprojPos = mul(invProj, float4(screenPos, depth, 1));
    float3 worldPos = viewPos + (unprojPos.xyz / unprojPos.w);
    uint2 gdata = gbuffer.Load(screenPos);
    float4 gdata0 = unpackUnorm4x8ToFloat(gdata.x);

    float3 baseColor = gdata0.rgb;
    float3 normal = havk::UnpackOctahedron<24>(gdata.y);
    float metallic = gdata0.a;
    float roughness = (gdata.y >> 24) / 255.0;
    float reflectance = 0.5; // parametized IOR

    float3 finalColor = 0.0;
    float exposure = 0.002;

    float alphaRoughness = max(roughness * roughness, 1e-4);
    float3 f0 = lerp(0.16 * reflectance * reflectance, baseColor, metallic);
    float3 diffuseColor = baseColor * (1.0 - metallic);

    float3 viewDir = normalize(viewPos - worldPos);
    float NoV = abs(dot(normal, viewDir)) + 1e-5;

    for (uint i = 0; i < numLights; i++) {
        Light light = loadAligned<16>(&lights[i]);
        float3 lightDir = light.Type == LightType::Directional ? -light.Direction : normalize(light.Position - worldPos);

        float NoL = saturate(dot(normal, lightDir));
        if (NoL < 1e-4) continue;

        float attenuation = light.GetAttenuation(worldPos) * light.Intensity * exposure;
        if (NoL * attenuation < 1e-4) continue;

#if ENABLE_RAY_TRACING
        if (sceneTlas) {
            float lightDist = light.Type == LightType::Directional ? 200 : distance(worldPos, light.Position);

            RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> rq;
            rq.TraceRayInline(sceneTlas, 0, 0xFF, RayDesc(worldPos, 0.001, lightDir, lightDist));
            if (rq.Proceed() && rq.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE) continue;
        }
#endif

        float3 halfwayDir = normalize(viewDir + lightDir);
        float NoH = saturate(dot(normal, halfwayDir));
        float LoH = saturate(dot(lightDir, halfwayDir));

        // BRDF
        float D = D_TrowbridgeReitz(alphaRoughness, NoH);
        float V = V_CorrelatedSmithGGX(alphaRoughness, NoL, NoV);
        float3 F = F_Schlick(LoH, f0);
        float3 Fr = (D * V) * F;
        float3 Fd = diffuseColor * math::InvPi; // Lambert

        finalColor += (Fd + Fr) * light.Color * (NoL * attenuation);
    }
    finalColor += baseColor * 0.05; // ambient

    finalColor = havk::Tonemap::Filmic(finalColor);
    finalColor = havk::colorutils::sRGB_from_Linear(finalColor);
    outputImage.Store(screenPos, float4(finalColor, 1));
}

// Normal distribution factor, aka D_GGX
float D_TrowbridgeReitz(float alphaRoughness, float NoH) {
    float a = NoH * alphaRoughness;
    float k = alphaRoughness / (1.0 - NoH * NoH + a * a);
    return k * k * math::InvPi;
}

// Geometry visibility factor
// V = G / (4 * NoL * NoV)
float V_CorrelatedSmithGGX(float alphaRoughness, float NoL, float NoV) {
    float a2 = alphaRoughness * alphaRoughness;
    float GGXV = NoL * sqrt(NoV * NoV * (1.0 - a2) + a2);
    float GGXL = NoV * sqrt(NoL * NoL * (1.0 - a2) + a2);
    return 0.5 / (GGXV + GGXL);
}

// Fresnel reflection factor
float3 F_Schlick(float VoH, float3 f0) {
    float x = saturate(1.0 - VoH);
    float x2 = x * x;
    float x5 = x2 * x2 * x;
    return f0 + (1.0 - f0) * x5;
}

[numthreads(8, 8)]
void CS_DownsampleDepth(
    uniform TextureHandle2D<float> srcImage,
    uniform ImageHandle2D<float> dstImage,
    uniform SamplerHandle sampler,
    uniform float2 srcScale,
    uint2 tid: SV_DispatchThreadID)
{
    float value = srcImage.SampleLevel(sampler, (tid + 0.5) * srcScale, 0);
    dstImage.Store(tid, value);
}

[numthreads(64)]
void CS_ModelCullDraws(
    uniform MeshDrawCommand* srcDraws,
    uniform MeshDrawCommand* dstDraws,
    uniform uint sourceCount,
    uniform uint* culledCount,
    uniform float4* meshBoundSpheres,
    uniform float3x4* transforms,
    uniform TextureHandle2D<float> depthPyramid,
    uniform SamplerHandle depthSampler,
    uniform float4x4 viewMat,
    uniform float4x4 viewProjMat,
    uniform float3 projFactors,
    uint tid: SV_DispatchThreadID)
{
    if (tid >= sourceCount) return;
    MeshDrawCommand draw = srcDraws[tid];
    float3x4 transform = transforms[draw.TransformIdx];

    float4 sphere = meshBoundSpheres[draw.MeshIdx];
    sphere.xyz = mul(transform, float4(sphere.xyz, 1)).xyz;
    sphere.w = mul(transform, float4(sphere.www, 0)).x;  // assume uniform scale

    if (!FrustumContainsSphere(viewProjMat, sphere.xyz, sphere.w)) return;

#if DEBUG_CULLING
    dbg::EnableOnce();
    bool dbgMeshBounds = dbg::Checkbox("Show Mesh Bounds", false);
    bool dbgOcclusion = dbg::Checkbox("Show Occlusion Bounds", false);
    int2 dbgDrawRange = (int2)dbg::Drag("Draw Range", float2(0, 100000), 0, 100000, "%.0f");
    if (tid < dbgDrawRange.x || tid > dbgDrawRange.y) return;
#endif

    float3 centerVS = mul(viewMat, float4(sphere.xyz, 1)).xyz;

    float4 screenBB;
    if (ProjectSphere(centerVS, sphere.w, projFactors, screenBB)) {
        float2 bbSize = (screenBB.zw - screenBB.xy) * depthPyramid.Size;
        float pyramidLevel = floor(log2(max(bbSize.x, bbSize.y)));
        float2 centerSS = (screenBB.xy + screenBB.zw) * 0.5;

        float depthVisible = depthPyramid.SampleLevel(depthSampler, centerSS, pyramidLevel);
        float depthSphere = projFactors.x / (-centerVS.z - sphere.w);

#if DEBUG_CULLING
        if (dbgOcclusion) {
            float2 screenSize = dbg::GetDisplaySize();
            dbg::IsEnabled = true;
            dbg::SetStroke(depthVisible > depthSphere ? float3(1, 0, 0) : float3(1, 1, 1));
            dbg::DrawRect2D(screenBB.xy * screenSize, screenBB.zw * screenSize);
            //dbg::DrawCircle2D(centerSS * screenSize, exp2(pyramidLevel));

            dbg::SetFill(1, 1, 1);
            dbg::DrawText3D(sphere.xyz, "#%d D=%.2f V=%.2f L=%f", tid, depthSphere * 1000, depthVisible * 1000, pyramidLevel);
        }
#endif

        if (depthVisible > depthSphere) return;
    }

    draw.Cmd.InstanceOffset = 1;  // this is a nop, workaround for https://github.com/shader-slang/slang/issues/9614

    uint dstIndex = 0;
    InterlockedAdd(*culledCount, 1, dstIndex);
    dstDraws[dstIndex] = draw;

#if DEBUG_CULLING
    if (dbgMeshBounds) {
        dbg::IsEnabled = true;
        dbg::NoFill();
        dbg::SetStroke(float3(1, 1, 0));
        dbg::DrawSphere3D(sphere.xyz, sphere.w);
    }
#endif
}

// 2D Polyhedral Bounds of a Clipped, Perspective - Projected 3D Sphere. Michael Mara, Morgan McGuire. 2013
bool ProjectSphere(float3 c, float r, float3 proj, inout float4 aabb) {
    // proj = znear, P00, P11
    c.z *= -1;
    if (c.z < r + proj.x) return false;

    float3 cr = c * r;
    float2 v = sqrt(c.xy * c.xy + c.z * c.z - r * r);
    aabb.xy = (v * c.xy - cr.z) / (v * c.z + cr.xy) * proj.yz;
    aabb.zw = (v * c.xy + cr.z) / (v * c.z - cr.xy) * proj.yz;
    aabb = aabb * 0.5 + 0.5;  // clip space -> uv space
    return true;
}

bool FrustumContainsSphere(float4x4 projView, float3 center, float radius) {
    // Gribb & Hartmann
    for (uint i = 0; i < 3; i++) {
        float4 planeA = projView[3] + projView[i];  // Left,  Bottom, Near
        float4 planeB = projView[3] - projView[i];  // Right, Top,    Far
        planeA /= length(planeA.xyz);
        planeB /= length(planeB.xyz);
        float distA = dot(center, planeA.xyz) + planeA.w;
        float distB = dot(center, planeB.xyz) + planeB.w;
        if (distA < -radius || distB < -radius) return false;
    }
    return true;
}

