struct Vertex {
    float2 pos;
    uint32_t color;
};
[shader("vertex")]
void VS_Triangle(uniform Vertex* vertices, uniform float4x4 projMat,
                 uint vertexId: SV_VertexID,
                 out float4 clipPos: SV_Position, out float3 color) {
    clipPos = mul(projMat, float4(vertices[vertexId].pos, 0, 1));
    color = unpackUnorm4x8ToFloat(vertices[vertexId].color).bgr; // 0xAARRGGBB in little-endian becomes BGRA
}

[shader("fragment")]
float4 FS_Triangle(float3 color, bool frontFace: SV_IsFrontFace) {
    return float4(frontFace ? color : 1 - color, 1.0);
}
