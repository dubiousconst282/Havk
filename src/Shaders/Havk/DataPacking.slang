import Math;

// Data packing and miscellaneous conversions
namespace havk {
    
// Project normal to octahedron and quantize into lower `bits`
public uint PackOctahedron<int Bits = 32>(float3 norm) {
    static_assert(Bits > 0 && Bits % 2 == 0, "Number of bits must be a multiple of two");

    const int cbits = Bits / 2;
    const int mask = (1 << cbits) - 1;
    const float scale = mask / 2.0; // e.g. for 16 = 32767.5

    uint2 d = uint2(math::MapOctahedron(norm) * scale + scale);
    return (d.y << cbits) | d.x;
}
public float3 UnpackOctahedron<int Bits = 32>(uint packed) {
    static_assert(Bits > 0 && Bits % 2 == 0, "Number of bits must be a multiple of two");

    const int cbits = Bits / 2;
    const int mask = (1 << cbits) - 1;
    const float scale = mask / 2.0; // e.g. for 16 = 32767.5

    uint2 uv_i = packed >> uint2(0, cbits) & mask;
    return math::UnmapOctahedron(uv_i * (1.0 / scale) - 1.0);
}

static const uint4 kMaskRGB10A2 = uint4(1023, 1023, 1023, 3);

public uint PackRGB10A2(float4 value) {
    uint4 q = uint4(saturate(value) * kMaskRGB10A2 + 0.5);
    return q.x | (q.y << 10) | (q.z << 20) | (q.w << 30);
}
public float4 UnpackRGB10A2(uint packed) {
    uint4 q = packed >> uint4(0, 10, 20, 30) & kMaskRGB10A2;
    return q * (1.0 / kMaskRGB10A2);
}

};