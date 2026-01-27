import Havk.Core;

struct ImDrawVert {
    float2 pos;
    float2 uv;
    uint32_t col;
};
struct ImGuiDrawParams {
    float2 Scale;
    float2 Offset;
    ImDrawVert* Vertices;
    TextureHandle2D Texture;
};
[vk::push_constant] ImGuiDrawParams pc;

[shader("vertex")]
void VS_ImGuiDrawMain(uint vertexId: SV_VulkanVertexID, out float4 clipPos: SV_Position, out float2 uv, out float4 color) {
    ImDrawVert vert = pc.Vertices[vertexId];
    clipPos = float4(vert.pos * pc.Scale + pc.Offset, 0, 1);
    uv = vert.uv;
    color = unpackUnorm4x8ToFloat(vert.col);
}

[shader("fragment")]
float4 FS_ImGuiDrawMain(float2 uv, float4 color) {
    let sampler = havk::GetSampler(havk::FilterMode::Linear, havk::WrapMode::ClampToEdge);
    color *= pc.Texture.Sample(sampler, uv);
    return color;
}