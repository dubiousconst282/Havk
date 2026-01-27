[shader("vertex")]
void VS_FullScreenTriangleCCW(uint vertexId: SV_VertexID, out float4 pos: SV_Position, out float2 screenUV: TEXCOORD0) {
    // Drawing a single triangle instead of a quad avoids helper fragment invocations
    // around the diagonal edges by taking advantage of guard-band clipping.
    // - https://stackoverflow.com/a/59739538
    // - https://wallisc.github.io/rendering/2021/04/18/Fullscreen-Pass.html
    const float2 vertices[3] = { float2(-1, 3), float2(3, -1), float2(-1, -1) };

    pos = float4(vertices[vertexId], 0, 1);
    screenUV = vertices[vertexId] * 0.5 + 0.5;
}