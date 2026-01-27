[vk::binding(0, 0)] __DynamicResource havk__TextureHeap[];
[vk::binding(1, 0)] __DynamicResource havk__RWTextureHeap[];
[vk::binding(2, 0)] SamplerState havk__ImutSamplerHeap[];
[vk::binding(3, 0)] SamplerState havk__DynSamplerHeap[];

interface IImageDescriptor {
    static const bool isWriteable;
};

__generic<E : ITexelElement, Shape : __ITextureShape, int isArray, int isMS, int sampleCount, int access, int isShadow, int format>
extension _Texture<E, Shape, isArray, isMS, sampleCount, access, isShadow, 0, format> : IImageDescriptor {
    static const bool isWriteable = access == 1;
};

public struct ImageHandle<T : IImageDescriptor & __IDynamicResourceCastable> {
    uint DescriptorIndex;

    public property T Instance {
        [ForceInline]
        get { return (T.isWriteable ? havk__RWTextureHeap : havk__TextureHeap)[DescriptorIndex].as<T>(); }
    }
    public property T NonUniformInstance {
        [ForceInline]
        get { return (T.isWriteable ? havk__RWTextureHeap : havk__TextureHeap)[NonUniformResourceIndex(DescriptorIndex)].as<T>(); }
    }
}

public struct SamplerHandle {
    uint DescriptorIndex;

    public property SamplerState Instance {
        [ForceInline]
        get { return havk__DynSamplerHeap[DescriptorIndex]; }
    }
    public property SamplerState NonUniformInstance {
        [ForceInline]
        get { return havk__DynSamplerHeap[NonUniformResourceIndex(DescriptorIndex)]; }
    }
}
public extension SamplerState {
    [ForceInline]
    __implicit_conversion(1)
    public __init(SamplerHandle handle) { return handle.Instance; }
}
public extension SamplerComparisonState {
    [ForceInline]
    __implicit_conversion(1)
    public __init(SamplerHandle handle) { return reinterpret<SamplerComparisonState>(handle.Instance); }
}

// Slang's DescriptorHandle<AccelStruct> is 8 bytes, but 4 aligned.
// This is probably a bit wasteful and we could patch up in the generator, but oh well.
public struct AccelStructHandle {
    public uint64_t Address;
}
public extension RaytracingAccelerationStructure {
    [ForceInline]
    __implicit_conversion(1)
    public __init(AccelStructHandle handle) { return RaytracingAccelerationStructure(handle.Address); }
}

public extension bool {
    __generic<T : IImageDescriptor & __IDynamicResourceCastable>
    public __implicit_conversion(400) __init(ImageHandle<T> handle) { return handle.DescriptorIndex != 0; }
    public __implicit_conversion(400) __init(AccelStructHandle handle) { return handle.Address != 0; }
};

__generic<E : ITexelElement, Shape : __ITextureShape, int format>
public extension ImageHandle<_Texture<E, Shape, 0, 0, 0, 1, 0, 0, format>> {
    [ForceInline]
    public E Load(vector<uint, Shape.dimensions> pos) {
        return Instance[pos];
    }
    [ForceInline]
    public void Store(vector<uint, Shape.dimensions> pos, E value) {
        Instance[pos] = value;
    }
}

__generic<E : ITexelElement, Shape : __ITextureShape, int format>
public extension ImageHandle<_Texture<E, Shape, 0, 0, 0, 0, 0, 0, format>> {
    [ForceInline]
    public E Load(vector<int, Shape.dimensions> pos, int mipLevel = 0) {
        vector<int, Shape.dimensions + 1> mpos;
        [ForceUnroll] for (int i = 0; i < Shape.dimensions; i++) mpos[i] = pos[i];
        mpos[Shape.dimensions] = mipLevel;
        return Instance.Load(mpos);
    }
    [ForceInline]
    public E Sample(SamplerState sampler, vector<float, Shape.dimensions> pos) {
        return Instance.Sample(sampler, pos);
    }
    [ForceInline]
    public E SampleLevel(SamplerState sampler, vector<float, Shape.dimensions> pos, float level) {
        return Instance.SampleLevel(sampler, pos, level);
    }
    [ForceInline]
    public E SampleBias(SamplerState sampler, vector<float, Shape.dimensions> pos, float bias) {
        return Instance.SampleBias(sampler, pos, bias);
    }
    [ForceInline]
    public float SampleCmp(SamplerComparisonState sampler, vector<float, Shape.dimensions> pos, float refValue) {
        return Instance.SampleCmp(sampler, pos, refValue);
    }
}

__generic<E : ITexelElement, int access, int format>
public extension ImageHandle<_Texture<E, __Shape2D, 0, 0, 0, access, 0, 0, format>> {
    public property uint2 Size {
        [ForceInline]
        get {
            uint2 size;
            Instance.GetDimensions(size.x, size.y);
            return size;
        }
    }
}

public typealias TextureHandle2D<F: ITexelElement = float4> = ImageHandle<Texture2D<F>>;
public typealias TextureHandle3D<F: ITexelElement = float4> = ImageHandle<Texture3D<F>>;
public typealias ImageHandle2D<F: ITexelElement = float4> = ImageHandle<RWTexture2D<F>>;
public typealias ImageHandle3D<F: ITexelElement = float4> = ImageHandle<RWTexture3D<F>>;

public typealias RWTextureCube<F: ITexelElement = float4> = _Texture<F, __ShapeCube, 0, 0, 0, 1, 0, 0, 0>;

public typealias TextureHandleCube<F: ITexelElement = float4> = ImageHandle<TextureCube<F>>;
public typealias ImageHandleCube<F: ITexelElement = float4> = ImageHandle<RWTextureCube<F>>;

public typealias ImageHandleArray2D<F: ITexelElement = float4> = ImageHandle<RWTexture2DArray<F>>;

#define DESTRUCT_VEC1(var) var.x
#define DESTRUCT_VEC2(var) var.x, var.y
#define DESTRUCT_VEC3(var) var.x, var.y, var.z

#define DEF_TEXTURE_SIZE_EXT(shape, isArray, dim) \
    __generic<T: ITexelElement, let access : int, let isShadow : int, let isCombined : int, let format : int> \
    public extension _Texture<T, shape, isArray, 0, 0, access, isShadow, isCombined, format> { \
        public property uint##dim Size { \
            get { \
                uint##dim res; \
                GetDimensions(DESTRUCT_VEC##dim(res)); \
                return res; \
            } \
        } \
    }

DEF_TEXTURE_SIZE_EXT(__Shape2D, 0, 2);
DEF_TEXTURE_SIZE_EXT(__Shape2D, 1, 3);
DEF_TEXTURE_SIZE_EXT(__Shape3D, 0, 3);

DEF_TEXTURE_SIZE_EXT(__ShapeCube, 0, 2);

namespace havk {

public enum FilterMode { Nearest, Linear, Anisotropic };
public enum WrapMode { Repeat, MirroredRepeat, ClampToEdge, MirrorClampToEdge };

// Gets a pre-defined sampler descriptor with the given parameters.
[ForceInline]
public SamplerState GetSampler(FilterMode filter, WrapMode wrap = WrapMode.Repeat) {
    return GetSampler(filter, filter, wrap);
}

[ForceInline]
public SamplerState GetSampler(FilterMode magFilter, FilterMode minFilter, WrapMode wrap) {
    if (magFilter == FilterMode.Anisotropic) magFilter = FilterMode.Linear;
    // This indexing logic should match sampler generation in DescriptorHeap constructor.
    return havk__ImutSamplerHeap[(int)magFilter + (int)minFilter * 2 + (int)wrap * 6];
}

// Same as VkDrawIndirectCommand.
public struct DrawCommand {
    public uint32_t NumVertices = 0;
    public uint32_t NumInstances = 1;
    public uint32_t VertexOffset = 0;
    public uint32_t InstanceOffset = 0;
};
// Same as VkDrawIndexedIndirectCommand.
public struct DrawIndexedCommand {
    public uint32_t NumIndices = 0;
    public uint32_t NumInstances = 1;
    public uint32_t IndexOffset = 0;
    public uint32_t VertexOffset = 0;
    public uint32_t InstanceOffset = 0;
};

};
