namespace math {

public static const float 
    Pi     = 3.14159265358979323846,
    Tau    = 6.28318530717958647692,
    HalfPi = 1.57079632679489661923,
    InvPi  = 0.31830988618379067153,
    InvTau = 0.15915494309189533576,
    E      = 2.71828182845904523536;
    
//////////////////////////////////////// Pseudo Random ////////////////////////////////////////

static uint _randomState;

// Init thread's random state based on a 96-bit seed (e.g. screen pos + frame no. See also, bit_cast() and clockARB()).
// This must be called at least once prior to sampling calls.
//
// Note the underlying algorithm may change in the future, and produce different values for any particular seed.
// It is currently based on LCG-XS: https://vectrx.substack.com/p/lcg-xs-fast-gpu-rng
public void RandomSeed(uint3 seed) {
    uint hash = 0u;
    for (int i = 0; i < 3; i++) {
        hash ^= seed[i];
        hash = hash * 2654435761u + 1692572869u;
        hash = hash ^ (hash >> 18);
    }
    _randomState = hash;
}
public uint RandomU32() {
    uint r = _randomState * 747796405u + 2891336453u;
    _randomState = r ^ (r >> 14);
    return _randomState;
}

// Returns a random float, uniformly distributed in [0,1) range.
public float RandomFloat() {
    const float scale = 1.0 / (1 << 24);
    return (RandomU32() >> 8) * scale;
}

// Returns a vector of random floats, uniformly distributed in [0,1) range.
// This function is more efficient than consecutive calls to RandomFloat(),
// but returned values have lower mantissa resolution/period (16 bits).
public float2 RandomFloat2() {
    uint2 bits = RandomU32() >> uint2(16, 0) & 65535;
    return bits * (1.0 / 65536);
}

// Returns a vector of random floats, uniformly distributed in [0,1) range.
// This function is more efficient than consecutive calls to RandomFloat(),
// but returned values have lower mantissa resolution/period (10 bits).
public float3 RandomFloat3() {
    uint3 bits = RandomU32() >> uint3(22, 12, 2) & 1023;
    return bits * (1.0 / 1024);
}

#define XXH_PRIME32_1 0x9E3779B1U
#define XXH_PRIME32_2 0x85EBCA77U
#define XXH_PRIME32_3 0xC2B2AE3DU
#define XXH_PRIME32_4 0x27D4EB2FU
#define XXH_PRIME32_5 0x165667B1U

// Hash a set of values using the xxHash32 algorithm.
// This function only accepts 32-bit arguments!
public uint HashXXH32<each T>(expand each T values) where T : IArithmetic {
    uint hash = XXH_PRIME32_5;
    expand HashXXH32_Round(hash, bit_cast<uint>(each values));
    return HashXXH32_Avalanche(hash);
}
public void HashXXH32_Round(inout uint hash, uint data) {
    hash += data * XXH_PRIME32_3;
    hash = ((hash << 17) | (hash >> 15)) * XXH_PRIME32_4;
}
public uint HashXXH32_Avalanche(uint hash) {
    hash ^= hash >> 15;
    hash *= XXH_PRIME32_2;
    hash ^= hash >> 13;
    hash *= XXH_PRIME32_3;
    hash ^= hash >> 16;
    return hash;
}

// Martin Roberts quasirandom sequence
// http://extremelearning.com.au/unreasonable-effectiveness-of-quasirandom-sequences
public float2 QuasirandRoberts2(uint index) {
    return fract(index * float2(0.75487766624669276005, 0.56984029099805326591) + 0.5);
}
public float3 QuasirandRoberts3(uint index) {
    return fract(index * float3(0.81917251339616443970, 0.67104360670378920841, 0.54970047790197026694) + 0.5);
}

public float2 QuasirandHammersley2(uint index, uint count) {
    return float2(index / (float)count, reversebits(index) * 0x1p-32);
}

//////////////////////////////////////// Projections ////////////////////////////////////////

// Find UV on octahedron. Result ranges [-1..1].
//
// https://knarkowicz.wordpress.com/2014/04/16/octahedron-normal-vector-encoding
// https://xcancel.com/Stubbesaurus/status/937994790553227264
public float2 MapOctahedron(float3 norm) {
    // norm /= abs(norm.x) + abs(norm.y) + abs(norm.z);
    // float t = max(-norm.z, 0.0);
    // norm.x += (norm.x < 0.0) ? -t : t;
    // norm.y += (norm.y < 0.0) ? -t : t;
    // return norm.xy;
    float w = rcp(abs(norm.x) + abs(norm.y) + abs(norm.z));
    float t = max(-norm.z * w, 0.0);
    return fma(norm.xy, w, copysign(t, norm.xy));
}
public float3 UnmapOctahedron(float2 uv) {
    float3 norm = float3(uv, 1.0 - abs(uv.x) - abs(uv.y));
    float t = max(-norm.z, 0.0);
    // norm.x += (norm.x < 0.0) ? t : -t;
    // norm.y += (norm.y < 0.0) ? t : -t;
    norm.xy -= copysign(t, norm.xy);
    return normalize(norm);
}

// Find UV on sphere / equirectangular projection. Result ranges [0,1].
// This is the projection most typically used in HDRI environment maps.
public float2 MapSphere(float3 dir) {
    return float2(atan2(dir.z, dir.x) * InvTau, asin(dir.y) * -InvPi) + 0.5;
}
public float3 UnmapSphere(float2 uv) {
    float u = (uv.x - 0.5) * Tau;
    float v = (uv.y - 0.5) * -Pi;
    float x = cos(u) * cos(v);
    float y = sin(v);
    float z = sin(u) * cos(v);
    return float3(x, y, z);
}

// Find UV on cube face. Result ranges [0,1].
// https://en.wikipedia.org/wiki/Cube_mapping
// https://wikis.khronos.org/opengl/Cubemap_Texture
public void MapCube(float3 dir, out float2 uv, out int faceIdx) {
    float maxAxis = max(max(abs(dir.y), abs(dir.z)), abs(dir.x));

    if (maxAxis == abs(dir.x)) {
        uv = float2(dir.x < 0 ? dir.z : -dir.z, -dir.y);
        faceIdx = dir.x < 0 ? 1 : 0;
    } else if (maxAxis == abs(dir.y)) {
        uv = float2(dir.x, dir.y < 0 ? -dir.z : dir.z);
        faceIdx = dir.y < 0 ? 3 : 2;
    } else if (maxAxis == abs(dir.z)) {
        uv = float2(dir.z < 0 ? -dir.x : dir.x, -dir.y);
        faceIdx = dir.z < 0 ? 5 : 4;
    }
    uv = uv * (rcp(maxAxis) * 0.5) + 0.5;
}
public float3 UnmapCube(float2 uv, uint faceIdx) {
    float u = uv.x * 2 - 1;
    float v = 1 - uv.y * 2;
    float3 dir;

    switch (faceIdx) {
        default:
        case 0: dir = float3(+1,  v, -u); break; // X+
        case 1: dir = float3(-1,  v,  u); break; // X-
        case 2: dir = float3( u, +1, -v); break; // Y+
        case 3: dir = float3( u, -1,  v); break; // Y-
        case 4: dir = float3( u,  v, +1); break; // Z+
        case 5: dir = float3(-u,  v, -1); break; // Z-
    }
    return normalize(dir);
}

//////////////////////////////////////// Geometry ////////////////////////////////////////

// Building an Orthonormal Basis, Revisited
// http://jcgt.org/published/0006/01/01/
public float3x3 BuildOrthonormalBasis(float3 n) {
    float sign = n.z < 0.0f ? -1.0f : 1.0f;
    float a = -1.0f / (sign + n.z);
    float b = n.x * n.y * a;
    float3 b1 = float3(1.0f + sign * n.x * n.x * a, sign * b, -sign * n.x);
    float3 b2 = float3(b, sign + n.y * n.y * a, -n.y);
    return float3x3(b1, b2, n);
}

};

// TODO: consider moving BSDF stuff here