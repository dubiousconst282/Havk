namespace havk::colorutils {

public float3 sRGB_to_Linear(float3 value) {
    float3 L = value / 12.92;
    float3 G = pow(value * 0.9478672 + 0.0521327, 2.4);
    return select(value < 0.04045, L, G);
}
public float3 sRGB_from_Linear(float3 value) {
    float3 L = value * 12.92;
    float3 G = pow(value, 1.0 / 2.4) * 1.055 - 0.055;
    return select(value < 0.0031308, L, G);
}

public float3 HSV_to_sRGB(float3 value) {
    float R = abs(value.x * 6 - 3) - 1;
    float G = 2 - abs(value.x * 6 - 2);
    float B = 2 - abs(value.x * 6 - 4);
    float3 RGB = saturate(float3(R, G, B));
    return ((RGB - 1) * value.y + 1) * value.z;
}
public float3 HSV_from_sRGB(float3 value) {
    // Based on work by Sam Hocevar and Emil Persson
    // https://chilliant.com/rgb2hsv.html
    const float Epsilon = 1e-10;
    float4 P = (value.g < value.b) ? float4(value.bg, -1.0, 2.0 / 3.0) : float4(value.gb, 0.0, -1.0 / 3.0);
    float4 Q = (value.r < P.x) ? float4(P.xyw, value.r) : float4(value.r, P.yzx);
    float C = Q.x - min(Q.w, Q.y);
    float H = abs((Q.w - Q.y) / (6 * C + Epsilon) + Q.z);
    float S = C / (Q.x + Epsilon);
    return float3(H, S, Q.x);
}

public float DitherBayer8x8(int2 pos) {
    int i = pos.x, j = pos.y, k = (i ^ j) << 1;
    int m = 0;
    m |= ((j & 1) | (k & 2)) << 4;
    m |= ((j & 2) | (k & 4)) << 1;
    m |= ((j & 4) | (k & 8)) >> 2;
    return m / 64.0;
}
// Interleaved Gradient Noise
// https://www.iryoku.com/next-generation-post-processing-in-call-of-duty-advanced-warfare
// https://blog.demofox.org/2022/01/01/interleaved-gradient-noise-a-different-kind-of-low-discrepancy-sequence/
public float DitherJimenez(int2 pos, uint frame = 0) {
    float2 fpos = pos + (frame & 63) * 5.588238;
    return fract(52.9829189 * fract(0.06711056 * fpos.x + 0.00583715 * fpos.y));
}
// https://extremelearning.com.au/unreasonable-effectiveness-of-quasirandom-sequences
public float DitherRoberts(int2 pos, uint frame = 0) {
    float z = (frame & 63) * 0.412;
    float r = fract(pos.x * 0.7548776662 + pos.y * 0.56984029 + z);
    return abs(r * 2.0 - 1.0);
}

};

namespace havk {

// Tone-mapping function.
// Input and outputs are both in the linear BT.709 gamut, output is in [0,1] range.
public enum Tonemap {
    Clamped,
    Reinhard,
    // Approximation of ACES Filmic curve
    // [Krzysztof Narkowicz. 2016] https://knarkowicz.wordpress.com/2016/01/06/aces-filmic-tone-mapping-curve
    Filmic,
    // Approximation of AgX display transform
    // [Benjamin Wrensch. 2023] https://iolite-engine.com/blog_posts/minimal_agx_implementation
    AgX,
    // Khronos PBR Neutral Image Encoding
    // [Emmett Lalish/The Khronos Group. 2024] https://modelviewer.dev/examples/tone-mapping
    PBRNeutral
};
public extension Tonemap {
    public float3 operator()(float3 color) {
        switch (this) {
            default:
            case Clamped:       return saturate(color);
            case Reinhard:      return color / (color + 1.0);
            case Filmic:        return Tonemap_Filmic(color);
            case AgX:           return Tonemap_AgX(color);
            case PBRNeutral:    return Tonemap_PBRNeutral(color);
        }
    }
};

float3 Tonemap_Filmic(float3 x) {
    // Narkowicz 2015, "ACES Filmic Tone Mapping Curve"
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

float3 Tonemap_AgX(float3 x) {
    // Note these are column major, matching GLSL code!
    const float3x3 agx_mat = float3x3(
        0.842479062253094, 0.042328242261012, 0.042375654905705,
        0.078433599999999, 0.878468636469772, 0.0784336,
        0.079223745147764, 0.079166127460543, 0.879142973793104);

    const float3x3 agx_mat_inv = float3x3(
         1.19687900512017, -0.05289685175745, -0.05297163551444,
        -0.09802088114013,  1.15190312990417, -0.09804345011712,
        -0.09902974407972, -0.09896117684484,  1.15107367264116);

    const float min_ev = -12.47393f;
    const float max_ev = 4.026069f;

    // Input transform (inset)
    x = mul(x, agx_mat);

    // Log2 space encoding
    x = clamp(log2(x), min_ev, max_ev);
    x = (x - min_ev) / (max_ev - min_ev);

    // Apply sigmoid function approximation
    x = (((((15.5 * x - 40.14) * x + 31.96) * x - 6.868) * x + 0.4298) * x + 0.1191) * x - 0.00232;

    // Inverse input transform (outset)
    x = mul(x, agx_mat_inv);

    // sRGB IEC 61966-2-1 2.2 Exponent Reference EOTF Display
    // NOTE: We're linearizing the output here. Comment/adjust when
    // *not* using a sRGB render target
    x = pow(x, float3(2.2));

    return x;
}

float3 Tonemap_PBRNeutral(float3 color) {
    const float startCompression = 0.8 - 0.04;
    const float desaturation = 0.15;

    float x = min(color.r, min(color.g, color.b));
    float offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
    color -= offset;

    float peak = max(color.r, max(color.g, color.b));
    if (peak < startCompression) return color;

    const float d = 1.0 - startCompression;
    float newPeak = 1.0 - d * d / (peak + d - startCompression);
    color *= newPeak / peak;

    float g = 1.0 - 1.0 / (desaturation * (peak - newPeak) + 1.0);
    return lerp(color, newPeak, g);
}

// Maps a linear range of values into a color gradient. Outputs gamma sRGB.
// https://matplotlib.org/stable/users/explain/colors/colormaps.html
public enum Colormap {
    // Viridis colormaps
    // [Stéfan van der Walt, Nathaniel Smith. 2015] https://bids.github.io/colormap
    // [Jerome Liard. 2016] https://www.shadertoy.com/view/XtGGzG
    Viridis, Inferno, Magma, Plasma,
    // Google's Turbo rainbow colormap
    // [Anton Mikhailov, Ruofei Du. 2019] https://research.google/blog/turbo-an-improved-rainbow-colormap-for-visualization
    Turbo,
    Grayscale,
};
public extension Colormap {
    public float3 operator()(float x, float2 range) {
        // Curve-fitted RGB coefficients
        static const float coeffs[5][18] = {
            // Viridis
            {
                +0.280268003, -0.143510503, +2.225793877, -14.815088879, +25.212752309, -11.772589584,
                -0.002117546, +1.617109353, -1.909305070, +2.701152864, -1.685288385, +0.178738871,
                +0.300805501, +2.614650302, -12.019139090, +28.933559110, -33.491294770, +13.762053843,
            },
            // Inferno
            {
                -0.027780558, +1.228188385, +0.278906882, +3.892783760, -8.490712758, +4.069046086,
                +0.014065206, +0.015360518, +1.605395918, -4.821108251, +8.389314011, -4.193858954,
                -0.019628385, +3.122510347, -5.893222355, +2.798380308, -3.608884658, +4.324996022,
            },
            // Magma
            {
                -0.023226960, 1.087154378, -0.109964741, +6.333665763, -11.640596589, 5.337625354,
                +0.010680993, 0.176613780, +1.638227448, -6.743522237, +11.426396979, -5.523236379,
                -0.008260782, 2.244286052, +3.005587601, -24.27976981, +32.484310068, -12.688259703,
            },
            // Plasma
            {
                +0.063861086, +1.992659096, -1.023901152, -0.490832805, +1.308442123, -0.914547012,
                +0.049718590, -0.791144343, +2.892305078, +0.811726816, -4.686502417, +2.717794514,
                +0.513275779, +1.580255060, -5.164414457, +4.559573646, -1.916810682, +0.570638854,
            },
            // Turbo
            {
                0.13572138, 4.61539260, -42.66032258, 132.13108234, -152.94239396, 59.28637943,
                0.09140261, 2.19418839, 4.84296658, -14.18503333, 4.27729857, 2.82956604,
                0.10667330, 12.64194608, -60.58204836, 110.36276771, -89.90310912, 27.34824973,
            }
        };

        x = saturate((x - range.x) / (range.y - range.x));
        if (this == Grayscale) return x;

        let c = coeffs[(int)this];
        return float3(
            c[0] + x * (c[1] + x * (c[2] + x * (c[3] + x * (c[4] + x * c[5])))),
            c[6] + x * (c[7] + x * (c[8] + x * (c[9] + x * (c[10] + x * c[11])))),
            c[12] + x * (c[13] + x * (c[14] + x * (c[15] + x * (c[16] + x * c[17]))))
        );
    }
};

};
