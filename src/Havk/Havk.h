#pragma once

#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>
#include <functional>

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include "ShaderBridge.h"

#if defined(NDEBUG)
    #define HAVK_ASSERT(expr)
#elif defined(_MSC_VER)
    // On Windows, assert() failures show an annoying message box instead of
    // breaking to debugger straightaway. This works for clang as well.
    #define HAVK_ASSERT(expr) (void)((expr) || (__debugbreak(), 0))
#else
    #include <cassert>
    #define HAVK_ASSERT(cond) assert(cond)
#endif

#define HAVK_CHECK(callExpr) \
    if (auto vkr__ = (callExpr); vkr__ != VK_SUCCESS) ::havk::Panic(vkr__, #callExpr);

#define HAVK_NON_COPYABLE(name)            \
    name(const name&) = delete;            \
    name& operator=(const name&) = delete; \
    name(name&&) = default;                \
    name& operator=(name&&) = default;

namespace havk {

void Panic(VkResult result, const char* msg);

struct DeviceContext;
using DeviceContextPtr = std::unique_ptr<DeviceContext>;

// Managed resource with queued deletion.
struct Resource {
    HAVK_NON_COPYABLE(Resource);

    DeviceContext* Context;

    Resource() = default;
    virtual ~Resource() {}

    struct QueuedDeleter {
        void operator()(Resource* res);
    };
};
// Destroy managed resource immediately, bypassing deletion queue.
template<typename T>
inline void Destroy(std::unique_ptr<T, Resource::QueuedDeleter>& obj) { delete obj.release(); }

#define HAVK_RESOURCE_FWD_DECL(name) \
    struct name;                     \
    using name##Ptr = std::unique_ptr<name, Resource::QueuedDeleter>;

HAVK_RESOURCE_FWD_DECL(CommandList);
HAVK_RESOURCE_FWD_DECL(Pipeline);
HAVK_RESOURCE_FWD_DECL(ComputePipeline);
HAVK_RESOURCE_FWD_DECL(GraphicsPipeline);
HAVK_RESOURCE_FWD_DECL(Buffer);
HAVK_RESOURCE_FWD_DECL(Image);
HAVK_RESOURCE_FWD_DECL(AccelStructPool);

#undef HAVK_RESOURCE_FWD_DECL

struct Swapchain;
using SwapchainPtr = std::unique_ptr<Swapchain>;

struct ImageDesc;
struct ImageView;
struct ImageViewDesc;
enum class BufferFlags;
template<typename T> struct BufferSpan;

struct DescriptorHeap;
struct GraphicsPipelineState;
struct AttachmentLayout;

struct ReloadWatcher; // Internal

template<typename T>
concept ProgramShape = requires {
    std::is_same_v<decltype(T::Module), ModuleDesc>;
    std::is_trivial_v<typename T::Params>;
};
template<typename T>
concept ComputeProgramShape = requires {
    requires ProgramShape<T>;
    std::is_same_v<decltype(T::GroupSize), vectors::uint3>;
};

// Debug text for Vulkan object handles.
// Defaults to caller's source location, but can be a custom
// printf-style format string (only `%s` and `%d` args are supported).
struct DebugLabel {
    const char* Format = nullptr;
    union {
        struct { const char* Arg0; uint64_t Arg1; };
        uint64_t Args[2];
    };

    constexpr DebugLabel() = default;
    constexpr DebugLabel(const char* text) : Format("%s"), Arg0(text), Arg1(0) {}

    template<typename T0, typename T1 = uint64_t>  // `auto` types can't be deduced from param default value.
    DebugLabel(const char* fmt, T0 arg0, T1 arg1 = 0) : Format(fmt), Args { uint64_t(arg0), uint64_t(arg1) } {}

#if __clang__
    static consteval DebugLabel ForCurrentSourceLoc(const char* fileName = __builtin_FILE_NAME(), uint32_t lineNo = __builtin_LINE()) {
#else
    static consteval DebugLabel ForCurrentSourceLoc(const char* fileName = __builtin_FILE(), uint32_t lineNo = __builtin_LINE()) {
        for (const char* ptr = fileName; *ptr != '\0'; ptr++) {
            if (*ptr == '/' || *ptr == '\\') fileName = ptr + 1;
        }
#endif
        DebugLabel lbl;
        lbl.Format = "%s:%d";
        lbl.Arg0 = fileName;
        lbl.Arg1 = lineNo;
        return lbl;
    }

    void AssignToObject(DeviceContext* ctx, VkObjectType objType, void* objHandle) const;
};

template<typename T>
struct Span {
    constexpr Span() = default;
    constexpr Span(T* ptr, size_t size) : _ptr(ptr), _size(size) {}
    constexpr Span(std::vector<T>& vec) : _ptr(vec.data()), _size(vec.size()) {}
    constexpr Span(const std::vector<std::remove_cv_t<T>>& vec) requires std::is_const_v<T> : _ptr(vec.data()), _size(vec.size()) {}
    constexpr Span(std::initializer_list<std::remove_cv_t<T>> list) requires std::is_const_v<T> : _ptr(list.begin()), _size(list.size()) {}
    template<size_t N> constexpr Span(T (&values)[N]) : _ptr(values), _size(N) {}

    T& operator[](size_t idx) const { HAVK_ASSERT(idx < _size); return _ptr[idx]; }

    size_t size() const { return _size; }
    T* data() const { return _ptr; }

    T* begin() const { return _ptr; }
    T* end() const { return _ptr + _size; }

private:
    T* _ptr = nullptr;
    size_t _size = 0;
};

struct DispatchTable {
#define PFN_FOR_EACH(visit_dev, visit_ins)                     \
    /* VK_KHR_acceleration_structure */                        \
    visit_dev(CreateAccelerationStructureKHR);                 \
    visit_dev(DestroyAccelerationStructureKHR);                \
    visit_dev(GetAccelerationStructureBuildSizesKHR);          \
    visit_dev(GetAccelerationStructureDeviceAddressKHR);       \
    visit_dev(GetDeviceAccelerationStructureCompatibilityKHR); \
    visit_dev(CmdBuildAccelerationStructuresKHR);              \
    visit_dev(CmdWriteAccelerationStructuresPropertiesKHR);    \
    visit_dev(CmdCopyAccelerationStructureKHR);                \
    visit_dev(CmdCopyAccelerationStructureToMemoryKHR);        \
    visit_dev(CmdCopyMemoryToAccelerationStructureKHR);        \
    /* VK_EXT_mesh_shader */                                   \
    visit_dev(CmdDrawMeshTasksEXT);                            \
    visit_dev(CmdDrawMeshTasksIndirectEXT);                    \
    visit_dev(CmdDrawMeshTasksIndirectCountEXT);               \
    /* VK_EXT_debug_utils */                                   \
    visit_ins(SetDebugUtilsObjectNameEXT);                     \
    visit_ins(CmdBeginDebugUtilsLabelEXT);                     \
    visit_ins(CmdEndDebugUtilsLabelEXT);                       \
    visit_ins(CmdInsertDebugUtilsLabelEXT);                    \
    ;

#define PFN_DECL(name) PFN_vk##name name
#define PFN_LOAD_DEV(name) name = (PFN_vk##name)vkGetDeviceProcAddr(device, "vk" #name)
#define PFN_LOAD_INS(name) name = (PFN_vk##name)vkGetInstanceProcAddr(instance, "vk" #name)

    PFN_FOR_EACH(PFN_DECL, PFN_DECL);

    DispatchTable() = default;
    DispatchTable(VkDevice device, VkInstance instance) { PFN_FOR_EACH(PFN_LOAD_DEV, PFN_LOAD_INS); }

#undef PFN_FOR_EACH
#undef PFN_DECL
#undef PFN_LOAD_DEV
#undef PFN_LOAD_INS
};

struct DeviceFeatures {
    bool RayQuery;             // VK_KHR_ray_query
    bool RayTracingPipeline;   // VK_KHR_ray_tracing_pipeline
    bool MeshShader;           // VK_EXT_mesh_shader
    bool FragShaderInterlock;  // VK_EXT_fragment_shader_interlock    (pixel + sample)
    bool ConservativeRaster;   // VK_EXT_conservative_rasterization
    bool ShaderClock;          // VK_KHR_shader_clock
    bool PerformanceQuery;     // VK_KHR_performance_query
};
struct PhysicalDeviceInfo {
    VkPhysicalDevice Handle = nullptr;
    VkInstance Instance;
    uint32_t QueueIndices[3] = { ~0u, ~0u, ~0u };  // QueueDomain -> native device queue index

    DeviceFeatures Features = {};
    VkPhysicalDeviceProperties Props;
};

enum class QueueDomain { Main, AsyncCompute, AsyncTransfer, Count_ };
struct DeviceQueue {
    VkQueue Handle = nullptr;
    VkCommandPool CmdPool = nullptr;
    VkSemaphore SubmitSemaphore = nullptr;
    uint32_t FamilyIndex = 0;
    uint64_t NextSubmitTimestamp = 1;
};

enum class LogLevel { Trace, Debug, Info, Warn, Error };
using LoggerCallback = std::function<void(DeviceContext* ctx, LogLevel level, const char* fmt, va_list)>;

struct DeviceCreateParams {
    LoggerCallback LoggerSink = nullptr;

    bool EnableDebugExtensions = false;                   // Enable shader reload, EXT_debug_utils, KHR_performance_query, etc.
    bool EnableSwapchainExtension = false;                // Require `VK_KHR_swapchain`.
    std::vector<const char*> RequiredInstanceExtensions;  // Required extensions for vkCreateInstance().

    std::function<void(std::vector<VkPhysicalDevice>&)> SortCandidateDevices;
    std::function<bool(const PhysicalDeviceInfo& device, const VkQueueFamilyProperties& props, uint32_t index)> IsSuitableMainQueue;

    // Can be used to override vkCreateDevice() and enable extra features in an improvised way.
    std::function<VkResult(const PhysicalDeviceInfo& device, VkDeviceCreateInfo* deviceCI, VkDevice* outDevice)> CreateLogicalDevice;
};
DeviceContextPtr CreateContext(const DeviceCreateParams& pars);

struct DeviceContext {
    HAVK_NON_COPYABLE(DeviceContext);

    VkInstance Instance = nullptr;
    VkDevice Device = nullptr;
    VmaAllocator Allocator = nullptr;

    std::unique_ptr<struct DescriptorHeap> DescriptorHeap;
    VkPipelineCache PipelineCache = nullptr;

    DispatchTable Pfn;
    PhysicalDeviceInfo PhysicalDevice;

    LoggerCallback LoggerSink;

    // Hook points (adhoc APIs, will change in the future!).
    std::function<void(CommandList&, VkQueue, VkSubmitInfo&, VkFence)> SubmitHook_;

    std::function<VkResult(Span<const ModuleDesc> mods, VkBaseInStructure* createInfo,
                           VkPipelineShaderStageCreateInfo* stages, VkPipeline* pipeline)> OnCreatePipelineHook_;
    std::function<void(Pipeline&)> OnDestroyPipelineHook_;

    DeviceContext() = default;
    ~DeviceContext();

    ComputePipelinePtr CreateComputePipeline(const ModuleDesc& mod, const SpecConstMap& specMap = {});

    GraphicsPipelinePtr CreateGraphicsPipeline(Span<const ModuleDesc> mods, const GraphicsPipelineState& state,
                                               const AttachmentLayout& outputs, const SpecConstMap& specMap = {});

    // TODO: RT pipes

    // Create buffer suitable for general purpose usage, backed by memory matching given flags.
    BufferPtr CreateBuffer(size_t sizeInBytes, BufferFlags flags, VkBufferUsageFlags usage = 0,
                           DebugLabel label = DebugLabel::ForCurrentSourceLoc());
    ImagePtr CreateImage(const ImageDesc& desc, DebugLabel label = DebugLabel::ForCurrentSourceLoc());

    AccelStructPoolPtr CreateAccelStructPool();

    // TODO: investigate use of aliased resources https://gpuopen-librariesandsdks.github.io/VulkanMemoryAllocator/html/resource_aliasing.html

    template<ComputeProgramShape TProgram>
    ComputePipeline* GetProgram() {
        static uint32_t id = ++s_nextStaticProgramId;

        if (_staticPrograms.size() <= id || _staticPrograms[id] == nullptr) [[unlikely]] {
            return CreateStaticComputeProgram(id, TProgram::Module);
        }
        return _staticPrograms[id].get();
    }

    // Create one-time submit command list.
    CommandListPtr CreateCommandList(QueueDomain queue = QueueDomain::Main, bool beginRecording = true);

    SwapchainPtr CreateSwapchain(VkSurfaceKHR surface);

    // NOTE: This is a placeholder, only a single queue is currently supported.
    DeviceQueue* GetQueue(QueueDomain domain) {
        HAVK_ASSERT(domain == QueueDomain::Main);
        return &_queues[(int)domain];
    }

    // Wait until all pending submissions have completed.
    void WaitIdle() { vkDeviceWaitIdle(Device); };

    // Flush deletion queue and refresh shaders.
    void GarbageCollect();

    [[gnu::format(printf, 3, 4)]]
    void Log(LogLevel level, const char* message, ...);

private:
    friend DeviceContextPtr CreateContext(const DeviceCreateParams& pars);

    friend struct Resource::QueuedDeleter;
    friend struct CommandList;
    friend struct Swapchain;
    friend struct Pipeline;
    friend struct AccelStructPool;

    // Recyclers are deletion queues (different term to avoid confusion with device queues).
    // They are flushed according to submission completion timestamps.
    // Resources are keept alive for the lifetime of any pending command list, starting at
    // the time they go out of scope.
    struct Recycler {
        std::vector<Resource*> Entries;
        uint64_t FlushTimestamp = 0;  // Time at which all submitted CmdLists will have been completed.
        uint32_t RefCount = 0;        // Number of pending CmdLists still referencing this recycler.
        std::unique_ptr<Recycler> Next;
    };

    std::unique_ptr<Recycler> _currRecycler = std::make_unique<Recycler>();
    std::vector<ComputePipelinePtr> _staticPrograms;

    CommandListPtr _prologueCmds;

    DeviceQueue _queues[(int)QueueDomain::Count_];
    VkQueryPool _accelsSizeQueryPools[2] = {};

    VkDebugUtilsMessengerEXT _debugMessenger = nullptr;
    std::unique_ptr<ReloadWatcher> _reloadWatcher;

    static uint32_t s_nextStaticProgramId;
    ComputePipeline* CreateStaticComputeProgram(uint32_t id, const ModuleDesc& mod);

    template<typename R, typename... Args>
    auto MakeUniqueResource(Args&&... args) {
        R* ptr = new R(std::forward<Args>(args)...);
        ptr->Context = this;
        return std::unique_ptr<R, Resource::QueuedDeleter>(ptr);
    }
};

// Manages the global image descriptor heap.
//   (set = 0, binding = 0) SampledImages[];
//   (set = 0, binding = 1) StorageImages[];
//   (set = 0, binding = 2) ImmutableSamplers[];  // predefined for common filter and wrap modes.
//   (set = 0, binding = 3) DynamicSamplers[];    // created at runtime with DescriptorHeap->GetSampler().
struct DescriptorHeap {
    HAVK_NON_COPYABLE(DescriptorHeap);

    DeviceContext* Context;
    VkDescriptorPool Pool = nullptr;
    VkDescriptorSetLayout SetLayout = nullptr;
    VkDescriptorSet Set = nullptr;
    VkPipelineLayout BindlessPipelineLayout = nullptr;

    DescriptorHeap(DeviceContext* ctx);
    ~DescriptorHeap();

    ImageHandle CreateHandle(VkImageView viewHandle, VkImageUsageFlags usage);
    void DestroyHandle(ImageHandle handle);

    // Returns cached sampler descriptor for given parameters. Once created, cannot be destroyed.
    SamplerHandle GetSampler(const VkSamplerCreateInfo& desc);

private:
    // MagFilter    = Nearest | Linear
    // MinFilter    = Nearest | Linear | Anisotropic
    // WrapMode     = Repeat | MirroredRepeat | ClampToEdge | MirrorClampToEdge
    static constexpr uint32_t kMaxSamplers = 256, kNumImutSamplers = 2 * 3 * 4;
    static constexpr uint32_t kMaxImages = 1024 * 64;

    struct HandleAllocator {
        uint32_t NextFreeWordIdxHint = 1;
        uint64_t UsedMap[(kMaxImages + 63) / 64] = { 1 };  // Slot 0 is reserved for null handle.

        uint32_t Alloc();
        void Free(uint32_t id);
    };
    HandleAllocator _allocator;
    VkSampler _imutSamplers[kNumImutSamplers];
    std::vector<std::tuple<VkSamplerCreateInfo, VkSamplerReductionMode, VkSampler, SamplerHandle>> _samplerMap;
};

struct Pipeline : Resource {
    VkPipeline Handle = nullptr;
    ~Pipeline() override;
};
struct GraphicsPipeline final : Pipeline {};
struct ComputePipeline final : Pipeline {};

enum class BufferFlags {
    DeviceMem = 1 << 0,     // Prefer allocation over device memory (VRAM).
    HostMem = 1 << 1,       // Prefer allocation over host memory (SysRAM).

    MapSeqWrite = 1 << 2,   // Require memory that is host-visible and write-combined.  VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
    MapCached = 1 << 3,     // Require memory that is host-visible and cached.          VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT

    AllowNonHostCoherent = 1 << 5,  // Allow memory that is not HOST_COHERENT (requires manual flush/invalidate calls after/before access
                                    // to memory mappings). On common desktop GPUs, all HOST_VISIBLE memory types are coherent.

    AllowNonHostVisible = 1 << 6,   // Allow memory that is not HOST_VISIBLE despite mapping request, if it may provide better performance.
                                    // (use in combination of DeviceMem, fallback writes to a staging buffer if `MappedData == nullptr`).
                                    // VMA_ALLOCATION_CREATE_HOST_ACCESS_ALLOW_TRANSFER_INSTEAD_BIT

    DeferredAlloc = 1 << 7,         // Requests that buffer memory should not be allocated at `CreateBuffer()`,
                                    // but deferred until a call to `Realloc()` or `commit_bump_alloc()`.

    // Convenience combos
    HostMem_SeqWrite = HostMem | MapSeqWrite,  // Good for uploads, sequential CPU write -> GPU read.
    HostMem_Cached = HostMem | MapCached,      // Good for readbacks, random CPU read/writes <-> sequential GPU read/write.
    DeviceMem_MappedIfOptimal = DeviceMem | MapSeqWrite | AllowNonHostVisible,
};
constexpr BufferFlags operator|(BufferFlags a, BufferFlags b) { return BufferFlags((int)a | (int)b); }
constexpr int operator&(BufferFlags a, BufferFlags b) { return (int)a & (int)b; }

struct Buffer final : Resource {
    VkBuffer Handle = nullptr;
    VmaAllocation Allocation = nullptr;

    uint64_t Size = 0;
    uint8_t* MappedData = nullptr;
    VkDeviceAddress DeviceAddress = 0;

    ~Buffer() override { vmaDestroyBuffer(Context->Allocator, Handle, Allocation); }

    template<typename T>
    operator DevicePtr<T>() { return DeviceAddress; }

    // Write and flush data to mapped range.
    template<typename T>
    void Write(size_t destByteOffset, const T* src, size_t elemCount) {
        size_t byteCount = elemCount * sizeof(T);
        HAVK_ASSERT(destByteOffset + byteCount <= Size);

        memcpy(MappedData + destByteOffset, src, byteCount);
        vmaFlushAllocation(Context->Allocator, Allocation, destByteOffset, byteCount);
    }
    // Needs to be called after writing to mapped memory for memory types that are not HOST_COHERENT.
    void Flush(size_t destByteOffset, size_t byteCount) {
        vmaFlushAllocation(Context->Allocator, Allocation, destByteOffset, byteCount);
    }
    // Needs to be called before reading from mapped memory for memory types that are not HOST_COHERENT.
    void Invalidate(size_t destByteOffset, size_t byteCount) {
        vmaInvalidateAllocation(Context->Allocator, Allocation, destByteOffset, byteCount);
    }

    // Despite how this function is named, it can currently only be called once on deferred buffers.
    VkResult Realloc(size_t sizeInBytes, BufferFlags flags, VkBufferUsageFlags usage = 0, DebugLabel label = DebugLabel::ForCurrentSourceLoc());

    // Creates a reference to a portion of this buffer, starting at given byte offset.
    template<typename T>
    BufferSpan<T> Slice(size_t byteOffset = 0, size_t elemCount = VK_WHOLE_SIZE);
};

template<typename T>
struct BufferSpan {
    // Wrapper struct intended to help avoid accidental reads from uncached mappings.
    struct [[gnu::may_alias]] Elem {
        T value;
        void operator=(const T& value_) { value = value_; }
    };

    BufferSpan() : _buffer(nullptr), _offset(0), _count(0) {}
    BufferSpan(Buffer& buffer) : _buffer(&buffer), _offset(0), _count(buffer.Size / sizeof(T)) {}

    operator BufferSpan<const T>() { return { _buffer, _offset, _count }; }

    Elem& operator[](size_t index) {
        HAVK_ASSERT(_buffer->MappedData && index < _count);
        return ((Elem*)(_buffer->MappedData + _offset))[index];
    }

    T* data() {
        HAVK_ASSERT(_buffer->MappedData && "Buffer must be host-visible!");
        return (T*)(_buffer->MappedData + _offset);
    }

    size_t size() const { return _count; }
    size_t size_bytes() const { return _count * sizeof(T); }
    bool is_host_visible() const { return _buffer && _buffer->MappedData; }

    size_t offset() const { return _offset / sizeof(T); }
    size_t offset_bytes() const { return _offset; }

    VkDeviceAddress device_addr() const { return _buffer ? _buffer->DeviceAddress + _offset : 0; }
    VkDeviceAddress device_addr(size_t elemOffset) const {
        HAVK_ASSERT(elemOffset < _count);
        return device_addr() + elemOffset * sizeof(T);
    }
    operator DevicePtr<T>() const { return device_addr(); }

    // Buffer in which this span refers to.
    Buffer& source_buffer() const {
        HAVK_ASSERT(_buffer && "Empty buffer span!");
        return *_buffer;
    }

    BufferSpan subspan(size_t elemOffset, size_t elemCount = VK_WHOLE_SIZE) const {
        if (elemCount == VK_WHOLE_SIZE) elemCount = _count - elemOffset;
        HAVK_ASSERT(elemOffset + elemCount <= _count);
        return { _buffer, _offset + elemOffset * sizeof(T), elemCount };
    }

    // Reinterpret to span of given type, adjusting sizes accordingly.
    template<typename R>
    BufferSpan<R> as() const {
        HAVK_ASSERT(_offset % alignof(R) == 0);
        return { _buffer, _offset, size_bytes() / sizeof(R) };
    }

    // Creates subslice with `elemCount` elements of type `R`, and advances current span to the end of it.
    template<typename R = T>
    BufferSpan<R> bump_slice(size_t elemCount, size_t align = alignof(R)) {
        size_t startOffset = align_up(_offset, align);
        size_t maxOffset = _offset + size_bytes();
        HAVK_ASSERT(startOffset + elemCount * sizeof(R) <= maxOffset && "Slice out of range!");
        _offset = align_up(startOffset + elemCount * sizeof(R), alignof(T));
        _count = _offset < maxOffset ? (maxOffset - _offset) / sizeof(T) : 0;
        return { _buffer, startOffset, elemCount };
    }

    template<typename R>
    BufferSpan<R> bump_write(const R* src, size_t elemCount, size_t align = alignof(R)) {
        auto slice = bump_slice<R>(elemCount, align);
        memcpy(slice.data(), src, elemCount * sizeof(R));
        return slice;
    }

    // Reallocates source buffer handle and memory with size matching the start offset of this span.
    // Span is reset back to start of buffer.
    void commit_bump_alloc(BufferFlags flags, DebugLabel label = DebugLabel::ForCurrentSourceLoc()) {
        HAVK_CHECK(source_buffer().Realloc(_offset, flags, 0, label));
        _count = _offset;
        _offset = 0;
    }

private:
    Buffer* _buffer;
    size_t _offset, _count;

    friend struct Buffer;
    template<typename E> friend struct BufferSpan;

    BufferSpan(Buffer* buffer, size_t offset, size_t count)
        : _buffer(buffer), _offset(offset), _count(count) {}

    static size_t align_up(size_t value, size_t amount) {
        HAVK_ASSERT((amount & -amount) == amount && "Alignment value must be a power of two!");
        return (value + (amount - 1)) & ~(amount - 1);
    }
};
template<typename T>
BufferSpan<T> Buffer::Slice(size_t byteOffset, size_t elemCount) {
    HAVK_ASSERT(byteOffset <= Size && byteOffset % alignof(T) == 0);

    size_t maxElems = (Size - byteOffset) / sizeof(T);
    if (elemCount == VK_WHOLE_SIZE) elemCount = maxElems;
    else HAVK_ASSERT(elemCount <= maxElems);

    return BufferSpan<T>(this, byteOffset, elemCount);
}

struct ImageDesc {
    VkImageViewType Type = VK_IMAGE_VIEW_TYPE_2D;
    VkFormat Format;
    VkImageUsageFlags Usage;

    vectors::uint3 Size;                // Width, Height, Depth|NumLayers
    uint32_t MipLevels = 1;             // Can be any high value or `VK_REMAINING_MIP_LEVELS`.
    uint8_t NumSamples = 1;             // MSAA sample count. Must be a power of two, supported by relevant `PhysicalDeviceLimits.__SampleCounts`.
    char ChannelSwizzle[5] = "RGBA";    // Channel swizzle string, composed of four of any `RGBA`, `XYZW`, or `01` characters.
                                        // `0` and `1` will map to the respective constants, e.g. `1.0f` or `int(1)` depending on format.

    bool IsLayered() const {
        return Type == VK_IMAGE_VIEW_TYPE_2D_ARRAY ||
               Type == VK_IMAGE_VIEW_TYPE_CUBE ||
               Type == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
    }
    uint32_t GetNumLayers() const { return IsLayered() ? Size.z : 1; }
};

struct Image final : Resource {
    VkImage Handle;
    VkImageView ViewHandle;
    VmaAllocation Allocation = nullptr; // Null if this is a swapchain image.
    ImageHandle DescriptorHandle = {};

    vectors::uint3 Size;                // Width, Height, Depth|NumLayers
    uint8_t MipLevels = 1;
    uint8_t NumSamples = 1;
    bool IsLayered;

    VkFormat Format;
    VkImageUsageFlags Usage;

    ~Image() override;
    operator ImageHandle() { HAVK_ASSERT(DescriptorHandle); return DescriptorHandle; }

    // Returns the number of layers, or 1 if this is not a layered (array) image.
    uint32_t GetNumLayers() const { return IsLayered ? Size.z : 1; }

    // Get or create cached sub-view to this image.
    ImageView GetSubView(const ImageViewDesc& desc);

    VkImageSubresourceRange GetEntireRange() const {
        return { .aspectMask = GetAspectMask(Format), .levelCount = MipLevels, .layerCount = GetNumLayers() };
    }

    static VkImageAspectFlags GetAspectMask(VkFormat format);

private:
    // Internal
    struct CustomViewMap;
    CustomViewMap* _customViews = nullptr;
};
struct ImageViewDesc {
    uint16_t LayerOffset = 0, NumLayers = 0;
    uint8_t MipOffset = 0, MipLevels = 0;
    char ChannelSwizzle[5] = "RGBA";
    VkFormat Format = VK_FORMAT_UNDEFINED;
    VkImageUsageFlags ShaderUsage = 0;
    VkImageViewType Type = VK_IMAGE_VIEW_TYPE_2D;

    bool operator==(const ImageViewDesc&) const = default;
};
struct ImageView {
    Image* SourceImage = nullptr;
    VkImageView Handle = nullptr;
    ImageHandle DescriptorHandle = {};

    ImageView() = default;
    ImageView(Image& image) : SourceImage(&image), Handle(image.ViewHandle), DescriptorHandle(image.DescriptorHandle) {}

    operator ImageHandle() { HAVK_ASSERT(DescriptorHandle); return DescriptorHandle; }
    explicit operator bool() const { return SourceImage != nullptr; }
};

// List of geometry descriptors used for building acceleration structures.
struct AccelStructBuildDesc {
    std::vector<VkAccelerationStructureGeometryKHR> Geometries;
    std::vector<uint32_t> PrimitiveCounts;

    // Available after CalculateSizes(). These are pre-aligned to whatever necessary amount, and can be summed directly.
    size_t AccelStructSize = 0, BuildScratchSize = 0, UpdateScratchSize = 0;
    VkBuildAccelerationStructureFlagsKHR Flags = 0;
    VkAccelerationStructureTypeKHR Type = VK_ACCELERATION_STRUCTURE_TYPE_MAX_ENUM_KHR;

    // For non-indexed triangles, set `indices = {}`.
    template<typename TVertex, typename TIndex = uint32_t>
    void AddTriangles(BufferSpan<TVertex> vertices, BufferSpan<TIndex> indices, VkFormat positionFormat, int positionFieldOffset = 0,
                      DevicePtr<vectors::float3x4> transform = nullptr) {
        static_assert(std::is_same_v<TIndex, uint32_t> || std::is_same_v<TIndex, uint16_t>);

        uint32_t triCount = (indices.size() > 0 ? indices.size() : vertices.size()) / 3;
        AddTriangles(triCount, {
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR,
            .vertexFormat = positionFormat,
            .vertexData = { vertices.device_addr() + positionFieldOffset },
            .vertexStride = sizeof(TVertex),
            .maxVertex = (uint32_t)(vertices.size() - 1),
            .indexType = indices.size() == 0 ? VK_INDEX_TYPE_NONE_KHR : 
                            std::is_same_v<TIndex, uint32_t> ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16,
            .indexData = { indices.device_addr() },
            .transformData = { transform.addr },
        });
    }
    void AddTriangles(uint32_t count, const VkAccelerationStructureGeometryTrianglesDataKHR& desc) {
        Geometries.push_back({
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
            .geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR,
            .geometry = { .triangles = desc },
        });
        PrimitiveCounts.push_back(count);
    }
    void AddBoxes(BufferSpan<const VkAabbPositionsKHR> data) {
        Geometries.push_back({
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
            .geometryType = VK_GEOMETRY_TYPE_AABBS_KHR,
            .geometry = { .aabbs = {
                .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_AABBS_DATA_KHR,
                .data = { data.device_addr() },
                .stride = sizeof(VkAabbPositionsKHR),
            } },
        });
        PrimitiveCounts.push_back((uint32_t)data.size());
    }
    void AddInstances(BufferSpan<const VkAccelerationStructureInstanceKHR> data) {
        Geometries.push_back({
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
            .geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR,
            .geometry = { .instances = {
                .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR,
                .arrayOfPointers = false,
                .data = { data.device_addr() },
            } },
        });
        PrimitiveCounts.push_back((uint32_t)data.size());
    }
    uint32_t NumGeometries() const { return (uint32_t)Geometries.size(); }

    void CalculateSizesForBLAS(DeviceContext* ctx, VkBuildAccelerationStructureFlagsKHR buildFlags) {
        CalculateSizes(ctx, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, buildFlags);
    }
    void CalculateSizesForTLAS(DeviceContext* ctx, VkBuildAccelerationStructureFlagsKHR buildFlags) {
        CalculateSizes(ctx, VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, buildFlags);
    }
    void CalculateSizes(DeviceContext* ctx, VkAccelerationStructureTypeKHR type, VkBuildAccelerationStructureFlagsKHR buildFlags);
    void CalculateUpperBoundSizesForTLAS(DeviceContext* ctx, uint32_t maxInstanceCount, VkBuildAccelerationStructureFlagsKHR buildFlags);
};
// A pool of one or more acceleration structures. "Node" in this context means one BLAS/TLAS instance.
struct AccelStructPool final : Resource {
    struct Node {
        VkAccelerationStructureKHR Handle = nullptr;
        VmaVirtualAllocation Range = nullptr;
    };
    std::vector<Node> Nodes;
    // Internal
    VkBuffer StorageBuffer_ = nullptr;
    VmaAllocation StorageAllocation_ = nullptr;
    VmaVirtualBlock RangeAllocator_ = nullptr;

    ~AccelStructPool() override;

    // Reset and reallocates pool storage. Any existing node must not be in use by the GPU.
    VkResult CreateStorage(size_t capacity, bool useLinearSubAllocator = false);

    // Calling this is redundant unless node address is needed before build.
    VkResult Reserve(uint32_t nodeIdx, size_t storageSize);

    // Create and build accel struct node. Any existing node must not be in use by the GPU.
    // Returns `VK_ERROR_OUT_OF_DEVICE_MEMORY` if out of pool storage.
    VkResult Build(CommandList& cmdList, uint32_t nodeIdx, const AccelStructBuildDesc& buildDesc, BufferSpan<uint8_t> scratchBuffer);

    // TODO: support AS updates

    // Get post-build properties for a list of nodes.
    // To avoid stalling device, completion status can be polled by clearing result buffer beforehand and checking for non-zero results.
    void GetProps(CommandList& cmdList, Span<const uint32_t> nodeIds, BufferSpan<uint64_t> results, VkQueryType prop);

    void GetCompactedSizes(CommandList& cmdList, Span<const uint32_t> nodeIds, BufferSpan<uint64_t> results) {
        GetProps(cmdList, nodeIds, results, VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR);
    }
    void GetSerializedSizes(CommandList& cmdList, Span<const uint32_t> nodeIds, BufferSpan<uint64_t> results) {
        GetProps(cmdList, nodeIds, results, VK_QUERY_TYPE_ACCELERATION_STRUCTURE_SERIALIZATION_SIZE_KHR);
    }

    // Compact node to a new location. Source and destination pool can be the same, but node index must be different.
    VkResult Compact(CommandList& cmdList, uint32_t nodeIdx, AccelStructPool& destPool, uint32_t destNodeIdx, uint64_t compactedSize);

    // Serialize node into a driver-specific format that can be quickly reloaded by `Deserialize()`.
    // The destination buffer must be big enough to hold the size returned by `GetSerializedSizes()`.
    void Serialize(CommandList& cmdList, uint32_t nodeIdx, BufferSpan<uint8_t> destData);

    // Reload node from previously serialized data. `deserializedSize` is the value returned by `GetDeserializedSize()`, which can be zero.
    // On failure, returns one of: `VK_ERROR_INCOMPATIBLE_DRIVER`, `VK_ERROR_OUT_OF_DEVICE_MEMORY`.
    VkResult Deserialize(CommandList& cmdList, uint32_t nodeIdx, BufferSpan<const uint8_t> srcData, uint64_t deserializedSize);

    // Get required storage size for a deserialized node, or zero if the serialized data is incompatible with current device or driver.
    // This function requires only the first 56 bytes of the serialized data.
    // See https://docs.vulkan.org/spec/latest/chapters/accelstructures.html#serialized-as-header
    uint64_t GetDeserializedSize(const uint8_t* headerData, size_t dataSize);

    AccelStructHandle GetAddress(uint32_t nodeIdx) const {
        VkAccelerationStructureDeviceAddressInfoKHR addrInfo = {
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR,
            .accelerationStructure = Nodes[nodeIdx].Handle,
        };
        return { .Address = Context->Pfn.GetAccelerationStructureDeviceAddressKHR(Context->Device, &addrInfo) };
    }

    // Return BLAS instance descriptor for a given node and parameters.
    VkAccelerationStructureInstanceKHR GetInstanceDesc(uint32_t nodeIdx, vectors::float3x4 transform,
                                                       VkGeometryInstanceFlagsKHR flags = 0) const {
        return {
            .transform = {
                transform[0][0], transform[0][1], transform[0][2], transform[0][3],
                transform[1][0], transform[1][1], transform[1][2], transform[1][3],
                transform[2][0], transform[2][1], transform[2][2], transform[2][3],
            },
            .mask = 0xFF,
            .flags = flags,
            .accelerationStructureReference = GetAddress(nodeIdx).Address,
        };
    }
};

struct RenderAttachment {
    ImageView Target = {};
    VkAttachmentLoadOp LoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    VkAttachmentStoreOp StoreOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkClearValue ClearValue = {};
    Image* ResolveTarget = nullptr;  // MSAA output
    VkResolveModeFlagBits ResolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;

    static RenderAttachment Cleared(const ImageView& target, VkClearValue value = {}, VkAttachmentStoreOp storeOp = VK_ATTACHMENT_STORE_OP_STORE) {
        return { .Target = target, .LoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .StoreOp = storeOp, .ClearValue = value };
    }
    static RenderAttachment Overlay(const ImageView& target) {
        return { .Target = target, .LoadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .StoreOp = VK_ATTACHMENT_STORE_OP_STORE };
    }
};
struct RenderingDesc {
    RenderAttachment Attachments[8];
    RenderAttachment DepthAttachment;
    RenderAttachment StencilAttachment;
    vectors::uint2 RegionOffset = { 0, 0 };
    vectors::uint2 RegionSize = { 0, 0 };
    // If set, will emit appropriate barrier and transitions for target attachments. Must still emit barrier before reading outputs:
    // - DstStages: COLOR_ATTACHMENT_OUTPUT and EARLY_FRAGMENT_TESTS|LATE_FRAGMENT_TESTS for depth/stencil images.
    // - DstLayout: ATTACHMENT_OPTIMAL
    VkPipelineStageFlagBits2 SrcStages = VK_PIPELINE_STAGE_FLAG_BITS_MAX_ENUM;
    VkPipelineStageFlagBits2 DstStages = VK_PIPELINE_STAGE_NONE; // Additional stages to stall/flush, e.g. INDIRECT_DRAW_BIT.
};

// Pixels are indexed in the usual way: `x + y*PixelsPerRow + z*PixelsPerRow*RowsPerLayer`
// See https://docs.vulkan.org/spec/latest/chapters/copies.html#copies-buffers-images-addressing
struct CopyBufferToImageParams {
    BufferSpan<const uint8_t> SrcData;
    uint32_t PixelsPerRow = 0;  // Default: DstExtent.x
    uint32_t RowsPerLayer = 0;  // Default: DstExtent.y
    Image& DstImage;
    vectors::uint3 DstOffset = { 0, 0, 0 };      // If layered image, Z = layer offset
    vectors::uint3 DstExtent = { ~0u, ~0u, 1 };  // Default: {Image.Size.xy, 1}
    uint32_t DstMipLevel = 0;
    bool GenerateMips = false;  // Generate mips by sucessive downsampling

    // If set, will emit appropriate barriers before and after copy, from/to these values.
    VkPipelineStageFlagBits2 SrcStages = VK_PIPELINE_STAGE_FLAG_BITS_MAX_ENUM;
    VkPipelineStageFlagBits2 DstStages = VK_PIPELINE_STAGE_FLAG_BITS_MAX_ENUM;
};

struct PushConstantData {
    const void* Ptr = nullptr;
    uint32_t DstOffset = 0;
    uint32_t Size = 0;

    PushConstantData() = default;
    PushConstantData(const void* ptr, uint32_t destOffset, uint32_t size) : Ptr(ptr), DstOffset(destOffset), Size(size) {}

    template<typename T>
    PushConstantData(const T& ref) : Ptr(&ref), Size(sizeof(T)) {}
};

// Barriers default to being over-defined, forcing complete pipeline stall and cache flush/inval. Refine as necessary.
struct BarrierDesc {
    VkPipelineStageFlagBits2 SrcStages = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT; // Stages to wait before `DstStages` can execute
    VkPipelineStageFlagBits2 DstStages = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT; // Stages to stall until `SrcStages` have completed
    // Intended memory access, controls cache flushing (src) and invalidation (dst).
    VkAccessFlagBits2 SrcAccess = VK_ACCESS_MEMORY_WRITE_BIT;
    VkAccessFlagBits2 DstAccess = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
};

// A token marking the asynchronous completion of a command list submission.
struct Future {
    DeviceContext* Context;
    uint64_t Timestamp;
    DeviceQueue* Queue;

    Future(DeviceContext* ctx, uint64_t ts, DeviceQueue* queue) : Context(ctx), Timestamp(ts), Queue(queue) {}

    void Wait(uint64_t timeoutNs = UINT64_MAX) const;
    bool IsComplete() const;
};

struct CommandList final : Resource {
    VkCommandBuffer Handle;
    DeviceQueue* Queue;

    // Internal
    VkPipeline BoundPipeline_ = nullptr;
    DeviceContext::Recycler* Recycler_ = nullptr;

    ~CommandList() override {
        vkFreeCommandBuffers(Context->Device, Queue->CmdPool, 1, &Handle);
        if (Recycler_ != nullptr) Recycler_->RefCount--;
    }

    void Begin() {
        VkCommandBufferBeginInfo beginInfo = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        };
        HAVK_CHECK(vkBeginCommandBuffer(Handle, &beginInfo));

        BoundPipeline_ = nullptr;

        HAVK_ASSERT(Recycler_ == nullptr);
        Recycler_ = Context->_currRecycler.get();
        Recycler_->RefCount++;
    }

    // Submit command list for execution.
    Future Submit(VkSemaphore waitSemaphore = nullptr, VkSemaphore signalSemaphore = nullptr, VkFence fence = nullptr);

    void BindPipeline(const Pipeline& pipeline, PushConstantData pc) {
        if (BoundPipeline_ != pipeline.Handle) {
            BoundPipeline_ = pipeline.Handle;

            auto bindpoint = dynamic_cast<const GraphicsPipeline*>(&pipeline) ? VK_PIPELINE_BIND_POINT_GRAPHICS : VK_PIPELINE_BIND_POINT_COMPUTE;
            vkCmdBindPipeline(Handle, bindpoint, pipeline.Handle);
            vkCmdBindDescriptorSets(Handle, bindpoint, Context->DescriptorHeap->BindlessPipelineLayout, 0, 1, &Context->DescriptorHeap->Set, 0, nullptr);
        }
        if (pc.Size > 0) {
            vkCmdPushConstants(Handle, Context->DescriptorHeap->BindlessPipelineLayout, VK_SHADER_STAGE_ALL, pc.DstOffset, pc.Size, pc.Ptr);
        }
    }
    void PushConstants(PushConstantData pc) {
        if (pc.Size > 0) {
            vkCmdPushConstants(Handle, Context->DescriptorHeap->BindlessPipelineLayout, VK_SHADER_STAGE_ALL, pc.DstOffset, pc.Size, pc.Ptr);
        }
    }

    // Dispatches the minimum number of groups covering the given invocation count: `numGroups = ceil(numInvocs / GroupSize)`.
    template<ComputeProgramShape TCompute>
    void Dispatch(vectors::uint3 numInvocs, const TCompute::Params& pc) {
        auto numGroups = (numInvocs + TCompute::GroupSize - 1u) / TCompute::GroupSize;
        DispatchGroups(*Context->GetProgram<TCompute>(), numGroups, pc);
    }
    void DispatchGroups(const ComputePipeline& pipeline, vectors::uint3 numGroups, PushConstantData pc = {}) {
        BindPipeline(pipeline, pc);
        vkCmdDispatch(Handle, numGroups.x, numGroups.y, numGroups.z);
    }

    void Draw(const GraphicsPipeline& pipeline, const DrawCommand& cmd, PushConstantData pc = {}) {
        BindPipeline(pipeline, pc);
        vkCmdDraw(Handle, cmd.NumVertices, cmd.NumInstances, cmd.VertexOffset, cmd.NumInstances);
    }
    void DrawIndexed(const GraphicsPipeline& pipeline, const DrawIndexedCommand& cmd, PushConstantData pc = {}) {
        BindPipeline(pipeline, pc);
        vkCmdDrawIndexed(Handle, cmd.NumIndices, cmd.NumInstances, cmd.IndexOffset, (int32_t)cmd.VertexOffset, cmd.InstanceOffset);
    }
    template<typename TCmd>
    void DrawIndexedIndirect(const GraphicsPipeline& pipeline, BufferSpan<TCmd> cmds, PushConstantData pc = {}) {
        BindPipeline(pipeline, pc);
        vkCmdDrawIndexedIndirect(Handle, cmds.source_buffer().Handle, cmds.offset_bytes(), cmds.size(), sizeof(TCmd));
    }
    template<typename TCmd>
    void DrawIndexedIndirectCount(const GraphicsPipeline& pipeline, BufferSpan<TCmd> cmds, BufferSpan<uint32_t> count,
                                  PushConstantData pc = {}) {
        BindPipeline(pipeline, pc);
        vkCmdDrawIndexedIndirectCount(Handle, cmds.source_buffer().Handle, cmds.offset_bytes(), count.source_buffer().Handle,
                                      count.offset_bytes(), cmds.size(), sizeof(TCmd));
    }

    void DrawMeshTasks(const GraphicsPipeline& pipeline, vectors::uint3 numGroups, PushConstantData pc = {}) {
        BindPipeline(pipeline, pc);
        Context->Pfn.CmdDrawMeshTasksEXT(Handle, numGroups.x, numGroups.y, numGroups.z);
    }
    template<typename TCmd>
    void DrawMeshTasksIndirect(const GraphicsPipeline& pipeline, BufferSpan<TCmd> cmds, PushConstantData pc = {}) {
        BindPipeline(pipeline, pc);
        Context->Pfn.CmdDrawMeshTasksIndirectEXT(Handle, cmds.source_buffer().Handle, cmds.offset_bytes(), cmds.size(),
                                                 sizeof(TCmd));
    }
    template<typename TCmd>
    void DrawMeshTasksIndirectCount(const GraphicsPipeline& pipeline, BufferSpan<TCmd> cmds, BufferSpan<uint32_t> count,
                                    PushConstantData pc = {}) {
        BindPipeline(pipeline, pc);
        Context->Pfn.CmdDrawMeshTasksIndirectCountEXT(Handle, cmds.source_buffer().Handle, cmds.offset_bytes(),
                                                      count.source_buffer().Handle, count.offset_bytes(), cmds.size(),
                                                      sizeof(TCmd));
    }

    void BeginRendering(const RenderingDesc& desc, bool setViewport = true);
    void EndRendering() { vkCmdEndRendering(Handle); }

    void SetViewport(VkViewport vp) { vkCmdSetViewport(Handle, 0, 1, &vp); }
    void SetScissor(VkRect2D rect) { vkCmdSetScissor(Handle, 0, 1, &rect); }

    void BindIndexBuffer(Buffer& buffer, size_t offset, VkIndexType indexType) {
        vkCmdBindIndexBuffer(Handle, buffer.Handle, offset, indexType);
    }

    [[gnu::format(printf, 2, 4)]]
    void BeginDebugLabel(const char* fmt, uint32_t color = 0, ...);
    void EndDebugLabel();

    // Splats a 32-bit value to the given buffer range (must be 4-byte aligned).
    void FillBuffer(BufferSpan<uint32_t> buffer, uint32_t value) {
        vkCmdFillBuffer(Handle, buffer.source_buffer().Handle, buffer.offset_bytes(), buffer.size_bytes(), value);
    }

    // Copies host data to buffer in device timeline. This is limited to 64KB per call and larger copies
    // should be avoided, see docs for `vkCmdUpdateBuffer`.
    void UpdateBuffer(Buffer& buffer, size_t destOffset, uint32_t dataSize, const void* data) {
        vkCmdUpdateBuffer(Handle, buffer.Handle, destOffset, dataSize, data);
    }
    void CopyBuffer(Buffer& source, Buffer& dest, size_t srcOffset = 0, size_t destOffset = 0, size_t size = VK_WHOLE_SIZE) {
        if (size == VK_WHOLE_SIZE) size = std::min(source.Size - srcOffset, dest.Size - destOffset);
        HAVK_ASSERT(srcOffset + size <= source.Size);
        HAVK_ASSERT(destOffset + size <= dest.Size);

        VkBufferCopy region = { srcOffset, destOffset, size };
        vkCmdCopyBuffer(Handle, source.Handle, dest.Handle, 1, &region);
    }

    void CopyBufferToImage(const CopyBufferToImageParams& pars);

    void ClearColorImage(Image& image, VkClearColorValue value) {
        VkImageSubresourceRange range = image.GetEntireRange();
        vkCmdClearColorImage(Handle, image.Handle, VK_IMAGE_LAYOUT_GENERAL, &value, 1, &range);
    }
    void ClearDepthImage(Image& image, VkClearDepthStencilValue value) {
        VkImageSubresourceRange range = image.GetEntireRange();
        vkCmdClearDepthStencilImage(Handle, image.Handle, VK_IMAGE_LAYOUT_GENERAL, &value, 1, &range);
    }

    // Global memory barrier
    // - Prevents previous commands of `SrcStages` from being re-ordered after future commands of `DstStages`.
    // - Flush and/or invalidate memory caches.
    // https://docs.vulkan.org/spec/latest/chapters/synchronization.html
    // https://themaister.net/blog/2019/08/14/yet-another-blog-explaining-vulkan-synchronization
    void Barrier(const BarrierDesc& barrier = {}) {
        VkMemoryBarrier2 memBarrier = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
            .srcStageMask = barrier.SrcStages,
            .srcAccessMask = barrier.SrcAccess,
            .dstStageMask = barrier.DstStages,
            .dstAccessMask = barrier.DstAccess,
        };
        VkDependencyInfo dep = {
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .memoryBarrierCount = 1,
            .pMemoryBarriers = &memBarrier
        };
        vkCmdPipelineBarrier2(Handle, &dep);
    }
};

struct Swapchain {
    HAVK_NON_COPYABLE(Swapchain);

    DeviceContext* Context;
    VkSurfaceKHR Surface = nullptr;
    VkSwapchainKHR Handle = nullptr;

    VkSurfaceCapabilitiesKHR SurfaceCaps = {};

    Swapchain(DeviceContext* ctx, VkSurfaceKHR surface);
    ~Swapchain();

    // Returned image is in GENERAL layout and contents are undefined. It can be written to immediately without barriers.
    std::pair<Image*, CommandList*> AcquireImage(vectors::uint2 surfaceSize);

    // Submit command list and present image previously returned from `AcquireImage()`.
    void Present();

    // Re-creates swapchain and initialize properties if necessary.
    // Automatically called by AcquireImage().
    void Initialize(vectors::uint2 surfaceSize);

    vectors::uint2 GetSurfaceSize() const {
        if (_images.empty()) return { 0, 0 };
        auto size = _images[0].Target->Size;
        return { size.x, size.y };
    }

    // Order implies priority.
    bool SetPreferredPresentModes(Span<const VkPresentModeKHR> modes);
    bool SetPreferredFormats(Span<const VkSurfaceFormatKHR> formats, VkImageUsageFlags requiredUsages);

    VkFormat GetFormat() const { return _selectedFormat.format; }
    VkColorSpaceKHR GetColorSpace() const { return _selectedFormat.colorSpace; }
    VkPresentModeKHR GetPresentMode() const { return _selectedPresentMode; }

    Span<const VkPresentModeKHR> GetAvailPresentModes() const { return _availPresentModes; }
    Span<const VkSurfaceFormatKHR> GetAvailFormats() const { return _availFormats; }

    // TODO: maybe allow setting number of frames in flight

    // Index of current frame in flight (incremented per present, wraps around by `GetNumFramesInFlight()`)
    uint32_t GetFlightIndex() const { return _currFrameIdx; }
    uint32_t GetNumFramesInFlight() const { return 2; }

private:
    struct FrameSyncInfo {
        CommandListPtr CmdList;

        VkSemaphore AvailableSemaphore;
        VkFence InFlightFence;
    };
    struct SwcImageInfo {
        ImagePtr Target;
        VkSemaphore RenderFinishedSemaphore;
    };
    std::vector<SwcImageInfo> _images;
    std::vector<FrameSyncInfo> _frameSync;
    uint32_t _currFrameIdx = 0, _currImageIdx = 0;  // not necessarily the same!
    bool _invalid = false;  // true if swapchain needs to be recreated

    std::vector<VkSurfaceFormatKHR> _availFormats;
    std::vector<VkPresentModeKHR> _availPresentModes;
    VkPresentModeKHR _selectedPresentMode;
    VkSurfaceFormatKHR _selectedFormat;
    VkImageUsageFlags _selectedUsages;

    void DestroyCurrentSwapchain();
};

constexpr struct {} dynamic_state = {};

// Pipeline state parameter that may be changed dynamically, when set to `dynamic_state`.
// https://registry.khronos.org/vulkan/specs/latest/man/html/VkDynamicState.html
template<typename T>
struct MaybeDynamicState {
    T Value;
    bool IsDynamic;

    constexpr MaybeDynamicState(decltype(dynamic_state)) : Value(), IsDynamic(true) {}
    constexpr MaybeDynamicState(T value_) : Value(value_), IsDynamic(false) {}
};

// https://docs.vulkan.org/refpages/latest/refpages/source/VkPipelineRasterizationStateCreateInfo.html
// https://docs.vulkan.org/spec/latest/chapters/primsrast.html
struct RasterizerState {
    VkPrimitiveTopology Topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    bool PrimitiveRestart = false;

    MaybeDynamicState<VkPolygonMode> PolygonMode = VK_POLYGON_MODE_FILL;
    MaybeDynamicState<VkFrontFace> FrontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    MaybeDynamicState<VkCullModeFlagBits> CullFace = VK_CULL_MODE_BACK_BIT;

    MaybeDynamicState<float> LineWidth = 1.0f;
    bool ConservativeRaster = false;
};
// https://docs.vulkan.org/refpages/latest/refpages/source/VkPipelineDepthStencilStateCreateInfo.html
// https://docs.vulkan.org/guide/latest/depth.html
struct DepthState {
    // Depth test predicate. `VK_COMPARE_OP_ALWAYS` to disable testing.
    VkCompareOp TestOp = VK_COMPARE_OP_ALWAYS;
    bool Write = true;
    bool Clamp = false;
    bool BoundsTest = false;  // See vkCmdSetDepthBounds()
};

// https://docs.vulkan.org/spec/latest/chapters/framebuffer.html#framebuffer-blending
struct ColorBlendingState {
    VkLogicOp LogicOp = VK_LOGIC_OP_COPY;
    vectors::float4 Constants = { 0, 0, 0, 0 };
    VkPipelineColorBlendAttachmentState AttachmentStates[8] = { kSrcCopy, kSrcCopy, kSrcCopy, kSrcCopy,
                                                                kSrcCopy, kSrcCopy, kSrcCopy, kSrcCopy };

    // Common blending presets
    // https://www.w3.org/TR/compositing-1/#porterduffcompositingoperators

    // out.rgba = src.rgba
    static constexpr VkPipelineColorBlendAttachmentState kSrcCopy = { .blendEnable = VK_FALSE, .colorWriteMask = 15 };

    // `out.rgb = src.rgb * src.a + dst.rgb * (1 - src.a)`
    // `out.a   =           src.a + dst.a   * (1 - src.a)`
    static constexpr VkPipelineColorBlendAttachmentState kSrcOver = {
        .blendEnable = VK_TRUE,
        .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .colorBlendOp = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .alphaBlendOp = VK_BLEND_OP_ADD,
        .colorWriteMask = 15,
    };
};
// https://docs.vulkan.org/refpages/latest/refpages/source/VkPipelineMultisampleStateCreateInfo.html
struct MultisamplingState {
    uint8_t NumSamples = 1;
    float MinSampleShading = 0.0f;
    bool AlphaToCoverage = false;
    bool AlphaToOne = false;
};

struct GraphicsPipelineState {
    RasterizerState Raster = {};
    DepthState Depth = {};
    ColorBlendingState Blend = {};
    MultisamplingState MSAA = {};
};

struct AttachmentLayout {
    VkFormat Formats[8] = {};
    VkFormat DepthFormat = VK_FORMAT_UNDEFINED;
    VkFormat StencilFormat = VK_FORMAT_UNDEFINED;

    bool operator==(const AttachmentLayout& other) const = default;
};

};  // namespace havk
