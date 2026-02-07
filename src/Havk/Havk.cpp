#include <cstdarg>
#include <string>
#include <string_view>
#include <stdexcept>

#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>

#if _WIN32
    #include <io.h>
    #define isatty _isatty
    #define fileno _fileno
#else
    #include <unistd.h>
#endif

// Workaround for VulkanSDK inconsistency
#if __has_include(<spirv-headers/spirv.hpp>)
    #include <spirv-headers/spirv.hpp>
#else
    #include <spirv/unified1/spirv.hpp>
#endif

#include "Havk.h"
#include "ShaderReload.h"

namespace havk {

static bool HasExtension(std::vector<VkExtensionProperties>& availList, const char* name) {
    for (auto& ext : availList) {
        if (strcmp(ext.extensionName, name) == 0) return true;
    }
    return false;
}
static bool CheckDeviceExtensionSupport(PhysicalDeviceInfo& device, const DeviceCreateParams& pars,
                                        const std::vector<const char*>& requiredExtensions) {
    uint32_t extensionCount;
    vkEnumerateDeviceExtensionProperties(device.Handle, nullptr, &extensionCount, nullptr);

    auto availExtensions = std::vector<VkExtensionProperties>(extensionCount);
    vkEnumerateDeviceExtensionProperties(device.Handle, nullptr, &extensionCount, availExtensions.data());

    for (auto requiredName : requiredExtensions) {
        if (!HasExtension(availExtensions, requiredName)) {
            return false;
        }
    }

    device.Features.RayQuery = HasExtension(availExtensions, VK_KHR_RAY_QUERY_EXTENSION_NAME);
    device.Features.RayTracingPipeline = HasExtension(availExtensions, VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME);
    device.Features.MeshShader = HasExtension(availExtensions, VK_EXT_MESH_SHADER_EXTENSION_NAME);
    device.Features.FragShaderInterlock = HasExtension(availExtensions, VK_EXT_FRAGMENT_SHADER_INTERLOCK_EXTENSION_NAME);
    device.Features.ConservativeRaster = HasExtension(availExtensions, VK_EXT_CONSERVATIVE_RASTERIZATION_EXTENSION_NAME);
    device.Features.ShaderClock = HasExtension(availExtensions, VK_KHR_SHADER_CLOCK_EXTENSION_NAME);

    if (pars.EnableDebugExtensions) {
        device.Features.PerformanceQuery = HasExtension(availExtensions, VK_KHR_PERFORMANCE_QUERY_EXTENSION_NAME);
    }
    return true;
}
static bool FindQueueIndices(PhysicalDeviceInfo& dev, const DeviceCreateParams& pars) {
    uint32_t familyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(dev.Handle, &familyCount, nullptr);

    auto queueFamilies = std::vector<VkQueueFamilyProperties>(familyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(dev.Handle, &familyCount, queueFamilies.data());

    for (uint32_t i = 0; i < familyCount; i++) {
        if (pars.IsSuitableMainQueue) {
            if (!pars.IsSuitableMainQueue(dev, queueFamilies[i], i)) continue;
        } else {
            if (!(queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
        }
        dev.QueueIndices[(int)QueueDomain::Main] = i;
        return true;
    }
    return false;
}

static PhysicalDeviceInfo SelectPhysicalDevice(const DeviceCreateParams& pars, VkInstance instance,
                                               const std::vector<const char*>& requiredExtensions) {
    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);

    auto devices = std::vector<VkPhysicalDevice>(deviceCount);
    vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data());

    if (pars.SortCandidateDevices) {
        pars.SortCandidateDevices(devices);
    }

    if (const char* userDeviceId = getenv("HAVK_SELECT_DEVICE")) {
        uint32_t selectedId = (uint32_t)std::stoul(userDeviceId, nullptr, 16);

        std::erase_if(devices, [&](auto& device) {
            VkPhysicalDeviceProperties props;
            vkGetPhysicalDeviceProperties(device, &props);
            printf("[havk] Device %04X: %s%s\n", props.deviceID, props.deviceName, props.deviceID == selectedId ? " *" : "");
            return props.deviceID != selectedId;
        });
        fflush(stdout);
    }

    // Pick first suitable device (this allows OS to override choice depending on e.g. power settings)
    for (auto& device : devices) {
        PhysicalDeviceInfo info = { .Handle = device, .Instance = instance };

        if (!CheckDeviceExtensionSupport(info, pars, requiredExtensions)) continue;
        if (!FindQueueIndices(info, pars)) continue;

        vkGetPhysicalDeviceProperties(device, &info.Props);

        return info;
    }

    throw std::runtime_error("Could not find suitable Vulkan device");
}
static VkDevice CreateLogicalDevice(const DeviceCreateParams& pars, const PhysicalDeviceInfo& devInfo,
                                    std::vector<const char*>& enabledExtensions) {
    VkPhysicalDeviceVulkan12Features availVulkan12Features = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
    };
    VkPhysicalDeviceFeatures2 availFeatures {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext = &availVulkan12Features,
    };
    vkGetPhysicalDeviceFeatures2(devInfo.Handle, &availFeatures);

    // Feature toggles are largely for validation, only a few will actually cause drivers to
    // pre-allocate stuff or switch to a slow mode (robust access), so we should gate those
    // behind opt-in flags in DeviceCreateParams when adding them.
    // (Search for `enabled_features.` in Mesa repo)
    //
    // We want to enforce a reasonable baseline for feature sets, but still allow context creation
    // for devices that don't have some less commonly used features.
    void* featureChain = nullptr;

    VkPhysicalDeviceAccelerationStructureFeaturesKHR accelStructFeatures = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR,
        .accelerationStructure = VK_TRUE,
    };
    VkPhysicalDeviceRayQueryFeaturesKHR rayQueryFeatures = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR,
        .rayQuery = VK_TRUE,
    };
    VkPhysicalDeviceRayTracingPipelineFeaturesKHR rayPipelineFeatures = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR,
        .rayTracingPipeline = VK_TRUE,
    };
    if (devInfo.Features.RayQuery || devInfo.Features.RayTracingPipeline) {
        accelStructFeatures.pNext = featureChain;
        featureChain = &accelStructFeatures;
        enabledExtensions.push_back(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
        enabledExtensions.push_back(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
    }
    if (devInfo.Features.RayQuery) {
        rayQueryFeatures.pNext = featureChain;
        featureChain = &rayQueryFeatures;
        enabledExtensions.push_back(VK_KHR_RAY_QUERY_EXTENSION_NAME);
    }
    if (devInfo.Features.RayTracingPipeline) {
        rayPipelineFeatures.pNext = featureChain;
        featureChain = &rayPipelineFeatures;
        enabledExtensions.push_back(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME);
    }

    VkPhysicalDeviceMeshShaderFeaturesEXT meshShaderFeatures = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT,
        .taskShader = VK_TRUE,
        .meshShader = VK_TRUE,
    };
    if (devInfo.Features.MeshShader) {
        meshShaderFeatures.pNext = featureChain;
        featureChain = &meshShaderFeatures;
        enabledExtensions.push_back(VK_EXT_MESH_SHADER_EXTENSION_NAME);
    }

    VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT fragShaderInterlockFeatures = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT,
        .fragmentShaderSampleInterlock = VK_TRUE,
        .fragmentShaderPixelInterlock = VK_TRUE,
    };
    if (devInfo.Features.FragShaderInterlock) {
        fragShaderInterlockFeatures.pNext = featureChain;
        featureChain = &fragShaderInterlockFeatures;
        enabledExtensions.push_back(VK_EXT_FRAGMENT_SHADER_INTERLOCK_EXTENSION_NAME);
    }
    if (devInfo.Features.ConservativeRaster) {
        enabledExtensions.push_back(VK_EXT_CONSERVATIVE_RASTERIZATION_EXTENSION_NAME);
    }

    VkPhysicalDeviceShaderClockFeaturesKHR shaderClockFeatures = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_CLOCK_FEATURES_KHR,
        .shaderSubgroupClock = VK_TRUE,
    };
    VkPhysicalDevicePerformanceQueryFeaturesKHR perfQueryFeatures = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PERFORMANCE_QUERY_FEATURES_KHR,
        .performanceCounterQueryPools = VK_TRUE,
    };
    if (devInfo.Features.ShaderClock) {
        shaderClockFeatures.pNext = featureChain;
        featureChain = &shaderClockFeatures;
        enabledExtensions.push_back(VK_KHR_SHADER_CLOCK_EXTENSION_NAME);
    }
    if (devInfo.Features.PerformanceQuery) {
        perfQueryFeatures.pNext = featureChain;
        featureChain = &perfQueryFeatures;
        enabledExtensions.push_back(VK_KHR_PERFORMANCE_QUERY_EXTENSION_NAME);
    }

    VkPhysicalDeviceMaintenance5FeaturesKHR maintenance5Features = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR,
        .pNext = featureChain,
        .maintenance5 = VK_TRUE,
    };

    VkPhysicalDeviceVulkan13Features vulkan13Features = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
        .pNext = &maintenance5Features,

        .shaderDemoteToHelperInvocation = VK_TRUE,
        .subgroupSizeControl = VK_TRUE,
        .computeFullSubgroups = VK_TRUE,
        .synchronization2 = VK_TRUE,
        .dynamicRendering = VK_TRUE,
    };
    VkPhysicalDeviceVulkan12Features vulkan12Features = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
        .pNext = &vulkan13Features,

        .samplerMirrorClampToEdge = VK_TRUE,
        .drawIndirectCount = VK_TRUE,

        .storageBuffer8BitAccess = VK_TRUE,
        .storagePushConstant8 = VK_TRUE,
        .shaderFloat16 = VK_TRUE,
        .shaderInt8 = VK_TRUE,

        .descriptorIndexing = VK_TRUE,
        .shaderSampledImageArrayNonUniformIndexing = VK_TRUE,
        .shaderStorageImageArrayNonUniformIndexing = VK_TRUE,
        .descriptorBindingSampledImageUpdateAfterBind = VK_TRUE,
        .descriptorBindingStorageImageUpdateAfterBind = VK_TRUE,
        .descriptorBindingUpdateUnusedWhilePending = VK_TRUE,
        .descriptorBindingPartiallyBound = VK_TRUE,
        .runtimeDescriptorArray = VK_TRUE,

        .samplerFilterMinmax = VK_TRUE,
        .scalarBlockLayout = VK_TRUE,

        .hostQueryReset = VK_TRUE,
        .timelineSemaphore = VK_TRUE,
        .bufferDeviceAddress = VK_TRUE,
        .vulkanMemoryModel = VK_TRUE,
        .vulkanMemoryModelDeviceScope = VK_TRUE,
        .subgroupBroadcastDynamicId = VK_TRUE,
    };
    VkPhysicalDeviceVulkan11Features vulkan11Features = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
        .pNext = &vulkan12Features,

        .storageBuffer16BitAccess = VK_TRUE,
        .storagePushConstant16 = VK_TRUE,
        .variablePointersStorageBuffer = VK_TRUE,
        .variablePointers = VK_TRUE,
        .shaderDrawParameters = VK_TRUE,
    };

    VkPhysicalDeviceFeatures requiredFeatures = {
        .fullDrawIndexUint32 = VK_TRUE,
        .multiDrawIndirect = VK_TRUE,
        .drawIndirectFirstInstance = VK_TRUE,
        .depthClamp = VK_TRUE,
        .wideLines = VK_TRUE,
        .samplerAnisotropy = VK_TRUE,
        .vertexPipelineStoresAndAtomics = VK_TRUE,
        .fragmentStoresAndAtomics = VK_TRUE,
        .shaderInt64 = VK_TRUE,
        .shaderInt16 = VK_TRUE,
    };

    // Untested switches based on GPUInfo reports

    // AMD Windows driver does not support this
    if (!availVulkan12Features.storagePushConstant8) {
        vulkan11Features.storagePushConstant16 = VK_FALSE;
        vulkan12Features.storagePushConstant8 = VK_FALSE;
    }
    // Nvidia GTX10, AMD GCN cards and older do not support this
    if (!availVulkan12Features.shaderFloat16) {
        vulkan12Features.shaderFloat16 = VK_FALSE;
    }

    float queuePriority = 1.0f;
    VkDeviceQueueCreateInfo queueCI = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = devInfo.QueueIndices[(int)QueueDomain::Main],
        .queueCount = 1,
        .pQueuePriorities = &queuePriority,
    };
    VkDeviceCreateInfo deviceCI = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = &vulkan11Features,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queueCI,
        .enabledExtensionCount = (uint32_t)enabledExtensions.size(),
        .ppEnabledExtensionNames = enabledExtensions.data(),
        .pEnabledFeatures = &requiredFeatures,
    };
    VkDevice device;

    if (pars.CreateLogicalDevice) {
        HAVK_CHECK(pars.CreateLogicalDevice(devInfo, &deviceCI, &device));
    } else {
        HAVK_CHECK(vkCreateDevice(devInfo.Handle, &deviceCI, nullptr, &device));
    }
    return device;
}

static VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT type,
                                                    const VkDebugUtilsMessengerCallbackDataEXT* data, void* pUserData) {
    if (severity <= VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT && data->pMessageIdName &&
        strcmp(data->pMessageIdName, "Loader Message") == 0) {
        return VK_FALSE;
    }
    auto ctx = (DeviceContext*)pUserData;

    LogLevel level = (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT) ? LogLevel::Trace :
                     (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT)    ? LogLevel::Info :
                     (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) ? LogLevel::Warn :
                     (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)   ? LogLevel::Error :
                                                                                    LogLevel::Debug;

    ctx->Log(level, "%s", data->pMessage);
#if __GNUC__ && __x86_64
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        asm volatile("nop");  // breakpoint placeholder
    }
#endif
    return VK_FALSE;
}

static void DefaultLoggerSink(DeviceContext* ctx, LogLevel level, const char* format, va_list args) {
    const char* levels[] = { "trc", "dbg", "inf", "wrn", "err" };
    const char* colors[] = { "90", "37", "32", "93", "91" };

    static bool isTerminal = isatty(fileno(stdout));

    char buffer[2048];
    vsnprintf(buffer, sizeof(buffer), format, args);

    if (isTerminal) {
        printf("[havk-\x1B[%sm%s\x1B[0m] %s\n", colors[(int)level], levels[(int)level], buffer);
    } else {
        printf("[havk-%s] %s\n", levels[(int)level], buffer);
    }
    fflush(stdout);
}

static VkSemaphore CreateTimelineSemaphore(VkDevice device) {
    VkSemaphoreTypeCreateInfo timelineCI = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
        .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
        .initialValue = 0,
    };
    VkSemaphoreCreateInfo semaphoreCI = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        .pNext = &timelineCI,
        .flags = 0,
    };
    VkSemaphore semaphore;
    HAVK_CHECK(vkCreateSemaphore(device, &semaphoreCI, NULL, &semaphore));
    return semaphore;
}

DeviceContextPtr CreateContext(const DeviceCreateParams& pars) {
    auto ctx = std::make_unique<DeviceContext>();
    ctx->LoggerSink = pars.LoggerSink ? pars.LoggerSink : DefaultLoggerSink;

    uint32_t extensionCount;
    vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, nullptr);

    auto instanceExtensions = std::vector<VkExtensionProperties>(extensionCount);
    vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, instanceExtensions.data());

    bool enableDebugExtensions = false;
    std::vector<const char*> requiredInstanceExtensions = pars.RequiredInstanceExtensions;

    if (pars.EnableDebugExtensions && HasExtension(instanceExtensions, VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
        enableDebugExtensions = true;
        requiredInstanceExtensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }
    VkDebugUtilsMessengerCreateInfoEXT debugMsgCI = {
        .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
        .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
        .pfnUserCallback = DebugCallback,
        .pUserData = ctx.get(),
    };

    VkApplicationInfo appInfo = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = nullptr,
        .applicationVersion = VK_MAKE_VERSION(1, 0, 0),
        .pEngineName = "havk",
        .engineVersion = VK_MAKE_VERSION(1, 0, 0),
        .apiVersion = VK_API_VERSION_1_3,
    };
    VkInstanceCreateInfo instanceCI = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &appInfo,
        .enabledExtensionCount = (uint32_t)requiredInstanceExtensions.size(),
        .ppEnabledExtensionNames = requiredInstanceExtensions.data(),
    };
    if (enableDebugExtensions) {
        instanceCI.pNext = &debugMsgCI;
    }
    HAVK_CHECK(vkCreateInstance(&instanceCI, nullptr, &ctx->Instance));

    if (enableDebugExtensions) {
        auto func = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(ctx->Instance, "vkCreateDebugUtilsMessengerEXT");
        func(ctx->Instance, &debugMsgCI, nullptr, &ctx->_debugMessenger);
    }

    std::vector<const char*> requiredExtensions = {
        VK_EXT_MEMORY_BUDGET_EXTENSION_NAME,
        VK_KHR_MAINTENANCE_5_EXTENSION_NAME,
    };
    if (pars.EnableSwapchainExtension) {
        requiredExtensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
        requiredExtensions.push_back(VK_KHR_SWAPCHAIN_MUTABLE_FORMAT_EXTENSION_NAME);
    }

    ctx->PhysicalDevice = SelectPhysicalDevice(pars, ctx->Instance, requiredExtensions);
    ctx->Device = CreateLogicalDevice(pars, ctx->PhysicalDevice, requiredExtensions);
    ctx->Pfn = DispatchTable(ctx->Device, ctx->Instance);
    ctx->Log(LogLevel::Info, "Selected device: %s", ctx->PhysicalDevice.Props.deviceName);

    VmaAllocatorCreateInfo allocCI = {
        .flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT | VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT,
        .physicalDevice = ctx->PhysicalDevice.Handle,
        .device = ctx->Device,
        .instance = ctx->Instance,
        .vulkanApiVersion = appInfo.apiVersion,
    };
    HAVK_CHECK(vmaCreateAllocator(&allocCI, &ctx->Allocator));

    for (uint32_t i = 0; i < std::size(ctx->_queues); i++) {
        DeviceQueue& queue = ctx->_queues[i];
        queue.FamilyIndex = ctx->PhysicalDevice.QueueIndices[i];
        if (queue.FamilyIndex == ~0u) continue;

        vkGetDeviceQueue(ctx->Device, queue.FamilyIndex, 0, &queue.Handle);

        VkCommandPoolCreateInfo cmdPoolCI = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
            .queueFamilyIndex = queue.FamilyIndex,
        };
        HAVK_CHECK(vkCreateCommandPool(ctx->Device, &cmdPoolCI, nullptr, &queue.CmdPool));

        queue.SubmitSemaphore = CreateTimelineSemaphore(ctx->Device);
        queue.NextSubmitTimestamp = 1;
    }

    ctx->DescriptorHeap = std::make_unique<DescriptorHeap>(ctx.get());

    if (pars.EnableDebugExtensions) {
        ctx->_reloadWatcher = ReloadWatcher::TryCreateForCurrentApp();

        if (ctx->_reloadWatcher == nullptr) {
            ctx->Log(LogLevel::Warn, "Could not initialize shader hot-reload watcher, maybe missing metadata file?");
        }
    }
    return ctx;
}

uint32_t DeviceContext::s_nextStaticProgramId = 0;

DeviceContext::~DeviceContext() {
    vkDeviceWaitIdle(Device);

    _staticPrograms.clear();

    // Deletion queues will only be marked as ready to retire after a Submit(),
    // so we have to flush any pending recyclers manually.
    for (auto rc = _currRecycler.get(); rc != nullptr; rc = rc->Next.get()) {
        HAVK_ASSERT(rc->RefCount == 0 && "Some CommandLists have not been released or are still in recording state!");

        for (Resource* res : rc->Entries) {
            delete res;
        }
    }

    DescriptorHeap.reset();
    vmaDestroyAllocator(Allocator);

    for (auto& pool : _accelsSizeQueryPools) {
        if (pool == nullptr) continue;

        vkDestroyQueryPool(Device, pool, nullptr);
    }

    if (_debugMessenger != nullptr) {
        auto func = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(Instance, "vkDestroyDebugUtilsMessengerEXT");
        func(Instance, _debugMessenger, nullptr);
    }

    for (auto& queue : _queues) {
        if (queue.Handle == nullptr) continue;

        vkDestroyCommandPool(Device, queue.CmdPool, nullptr);
        vkDestroySemaphore(Device, queue.SubmitSemaphore, nullptr);
    }

    vkDestroyDevice(Device, nullptr);
    vkDestroyInstance(Instance, nullptr);
}

void DeviceContext::Log(LogLevel level, const char* format, ...) {
    if (LoggerSink == nullptr) return;

    va_list args;
    va_start(args, format);
    LoggerSink(this, level, format, args);
    va_end(args);
}

CommandListPtr DeviceContext::CreateCommandList(QueueDomain queueDomain, bool beginRecording) {
    auto cmdList = MakeUniqueResource<CommandList>();
    cmdList->Queue = GetQueue(queueDomain);
    HAVK_ASSERT(cmdList->Queue && "No such queue domain");

    VkCommandBufferAllocateInfo bufferCI = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = cmdList->Queue->CmdPool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    HAVK_CHECK(vkAllocateCommandBuffers(Device, &bufferCI, &cmdList->Handle));

    if (beginRecording) {
        cmdList->Begin();
    }
    return cmdList;
}

Future CommandList::Submit(VkSemaphore waitSemaphore, VkSemaphore signalSemaphore, VkFence fence) {
    HAVK_CHECK(vkEndCommandBuffer(Handle));
    uint64_t finishTS = Queue->NextSubmitTimestamp++;

    VkSemaphore signalSemaphores[2] = { Queue->SubmitSemaphore, signalSemaphore };
    uint64_t signalValues[2] = { finishTS, finishTS };

    VkTimelineSemaphoreSubmitInfo timelineInfo = {
        .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
        .signalSemaphoreValueCount = signalSemaphore ? 2u : 1,
        .pSignalSemaphoreValues = signalValues,
    };
    VkPipelineStageFlags waitMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo submitInfo = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .pNext = &timelineInfo,
        .waitSemaphoreCount = waitSemaphore ? 1u : 0,
        .pWaitSemaphores = &waitSemaphore,
        .pWaitDstStageMask = &waitMask,
        .commandBufferCount = 1,
        .pCommandBuffers = &Handle,
        .signalSemaphoreCount = signalSemaphore ? 2u : 1,
        .pSignalSemaphores = signalSemaphores,
    };

    VkCommandBuffer cmdBuffers[2];

    if (auto& prologue = Context->_prologueCmds) {
        HAVK_CHECK(vkEndCommandBuffer(prologue->Handle));
        cmdBuffers[0] = prologue->Handle;
        cmdBuffers[1] = Handle;
        submitInfo.pCommandBuffers = cmdBuffers;
        submitInfo.commandBufferCount = 2;
        prologue = nullptr;
    }

    if (Context->SubmitHook_) {
        Context->SubmitHook_(*this, Queue->Handle, submitInfo, fence);
    } else {
        HAVK_CHECK(vkQueueSubmit(Queue->Handle, 1, &submitInfo, fence));
    }

    // Schedule flushing of associated deletion queue
    HAVK_ASSERT(Queue == Context->GetQueue(QueueDomain::Main));
    Recycler_->FlushTimestamp = std::max(Recycler_->FlushTimestamp, finishTS);
    // Context->Log(LogLevel::Trace, "Submit: DQ=%p FinishTS=%llu Now=%llu", Recycler_, finishTS, queueTs);

    if (!Recycler_->Entries.empty() && Recycler_ == Context->_currRecycler.get()) {
        // We could probably recycle recyclers, but this should be fine for now.
        auto newRecycler = std::make_unique<DeviceContext::Recycler>();
        newRecycler->Next = std::move(Context->_currRecycler);
        Context->_currRecycler = std::move(newRecycler);

        // We don't GC() here because it would increase resource lifetimes by at least
        // one more submit, seems best to leave as user responsability.
#ifndef NDEBUG
        uint32_t numPending = 0;
        for (auto rc = Context->_currRecycler.get(); rc != nullptr; rc = rc->Next.get()) {
            HAVK_ASSERT(++numPending < 16 &&
                        "Deletion queue is growing too big, make sure to call `DeviceContext::GarbageCollect()` periodically.");
        }
#endif
    }
    Recycler_->RefCount--;
    Recycler_ = nullptr;

    return Future(Context, finishTS, Queue);
}
void Resource::QueuedDeleter::operator()(Resource* res) {
    // Workaround edge case where deleting a CommandList creates lost/cyclic refs counts,
    // preventing deletion queue from being flushed.
    // TODO: we could avoid this check by changing deleter for CommandListPtr.
    if (auto cmdList = dynamic_cast<CommandList*>(res)) {
        if (cmdList->Recycler_) {
            cmdList->Recycler_->RefCount--;
            cmdList->Recycler_ = nullptr;
        }
    }
    res->Context->_currRecycler->Entries.push_back(res);
    // res->Context->Log(LogLevel::Trace, "EnqueueDelete: DQ=%p R=%p %s", res->Context->_currRecycler.get(), res, typeid(*res).name());
}

void DeviceContext::GarbageCollect() {
    if (_reloadWatcher != nullptr) {
        _reloadWatcher->Poll(this);
    }

    Recycler* prev = _currRecycler.get();
    while (auto& rc = prev->Next) {
        uint64_t queueTs;
        vkGetSemaphoreCounterValue(Device, GetQueue(QueueDomain::Main)->SubmitSemaphore, &queueTs);

        if (queueTs >= rc->FlushTimestamp && rc->RefCount == 0) {
            for (Resource* res : rc->Entries) {
                // Log(LogLevel::Trace, "FlushDelete: DQ=%p R=%p %s FinishTS=%llu Now=%llu", rc.get(), res, typeid(*res).name(), rc->FlushTimestamp, queueTs);
                delete res;
            }
            prev->Next = std::move(rc->Next);
        } else {
            prev = rc.get();
        }
    }
}

void Future::Wait(uint64_t timeoutNs) const {
    VkSemaphoreWaitInfo waitInfo = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
        .semaphoreCount = 1,
        .pSemaphores = &Queue->SubmitSemaphore,
        .pValues = &Timestamp,
    };
    HAVK_CHECK(vkWaitSemaphores(Context->Device, &waitInfo, timeoutNs));
}
bool Future::IsComplete() const {
    uint64_t queueTs;
    vkGetSemaphoreCounterValue(Context->Device, Queue->SubmitSemaphore, &queueTs);
    return queueTs >= Timestamp;
}

void Panic(VkResult result, const char* msg) {
    char buffer[512];
    snprintf(buffer, sizeof(buffer), "%s failed with code %d", msg, result);
    throw std::runtime_error(buffer); // TODO: maybe make custom exception class to allow catching stuff like OOM
}

static std::string GetPipelineDebugName(Span<const ModuleDesc> modules) {
    std::string text;
    std::string_view prevPath;

    for (uint32_t i = 0; i < modules.size(); i++) {
        std::string_view path = modules[i].SourcePath ? modules[i].SourcePath : "source";
        size_t sepIdx = path.find_last_of("\\/");
        if (sepIdx != std::string::npos) path.remove_prefix(sepIdx + 1);

        if (i != 0) text.append(",");
        if (path != prevPath) text.append(path).append(":");
        prevPath = path;
        text.append(modules[i].EntryPoint);
    }
    return text;
}
static void SetPipelineDebugName(DeviceContext* ctx, VkPipeline handle, std::string_view text) {
    VkDebugUtilsObjectNameInfoEXT nameInfo = {
        .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
        .objectType = VK_OBJECT_TYPE_PIPELINE,
        .objectHandle = (uint64_t)handle,
        .pObjectName = text.data(),
    };
    ctx->Pfn.SetDebugUtilsObjectNameEXT(ctx->Device, &nameInfo);
}

ComputePipelinePtr DeviceContext::CreateComputePipeline(const ModuleDesc& module, const SpecConstMap& specMap) {
    VkShaderModuleCreateInfo moduleCI = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = module.CodeSize,
        .pCode = module.Code,
    };
    VkSpecializationInfo specInfo = specMap.GetSpecInfo();

    VkComputePipelineCreateInfo pipelineCI = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .pNext = &moduleCI,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .pName = module.EntryPoint,
            .pSpecializationInfo = &specInfo,
        },
        .layout = DescriptorHeap->BindlessPipelineLayout,
    };
    auto instance = MakeUniqueResource<ComputePipeline>();

    if (OnCreatePipelineHook_) {
        HAVK_CHECK(OnCreatePipelineHook_({ &module, 1 }, (VkBaseInStructure*)&pipelineCI, &pipelineCI.stage, &instance->Handle));
    } else {
        HAVK_CHECK(vkCreateComputePipelines(Device, PipelineCache, 1, &pipelineCI, nullptr, &instance->Handle));
    }

    if (Pfn.SetDebugUtilsObjectNameEXT != nullptr) {
        SetPipelineDebugName(this, instance->Handle, GetPipelineDebugName({ module }));
    }
    if (_reloadWatcher != nullptr && (module.Flags & ModuleDesc::kNoReload) == 0) {
        std::string name = GetPipelineDebugName({ module });
        auto reloadCb = [=, pipe = instance.get()](Span<const ModuleDesc> modules) {
            pipe->Context->Log(LogLevel::Info, "Reloading pipeline '%s'", name.data());

            auto newPipe = pipe->Context->CreateComputePipeline(modules[0], specMap);
            std::swap(newPipe->Handle, pipe->Handle);
        };
        _reloadWatcher->BeginTracking(instance.get(), { module }, std::move(reloadCb));
    }
    return instance;
}

GraphicsPipelinePtr DeviceContext::CreateGraphicsPipeline(Span<const ModuleDesc> modules, const GraphicsPipelineState& state,
                                                          const AttachmentLayout& outputs, const SpecConstMap& specMap) {
    auto moduleInfos = std::vector<VkShaderModuleCreateInfo>(modules.size());
    auto stageInfos = std::vector<VkPipelineShaderStageCreateInfo>(modules.size());
    auto dynamicStates = std::vector<VkDynamicState> { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkSpecializationInfo specInfo = specMap.GetSpecInfo();

    for (uint32_t i = 0; i < modules.size(); i++) {
        const ModuleDesc& mod = modules[i];

        moduleInfos[i] = {
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = mod.CodeSize,
            .pCode = mod.Code,
        };
        stageInfos[i] = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .pNext = &moduleInfos[i],
            .stage = mod.GetStage(),
            .pName = mod.EntryPoint,
            .pSpecializationInfo = &specInfo,
        };
    }

    VkPipelineVertexInputStateCreateInfo vertexInputCI = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 0,
        .pVertexBindingDescriptions = nullptr,
        .vertexAttributeDescriptionCount = 0,
        .pVertexAttributeDescriptions = nullptr,
    };
    VkPipelineInputAssemblyStateCreateInfo inputAssemblyCI = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = state.Raster.Topology,
        .primitiveRestartEnable = state.Raster.PrimitiveRestart,
    };
    VkPipelineViewportStateCreateInfo viewportCI = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount = 1,
    };

    VkPipelineRasterizationStateCreateInfo rasterizerCI = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .depthClampEnable = state.Depth.Clamp,
        .polygonMode = state.Raster.PolygonMode.Value,
        .cullMode = (VkCullModeFlags)state.Raster.CullFace.Value,
        .frontFace = state.Raster.FrontFace.Value,
        .lineWidth = state.Raster.LineWidth.Value,
    };
    if (state.Raster.PolygonMode.IsDynamic) dynamicStates.push_back(VK_DYNAMIC_STATE_POLYGON_MODE_EXT);
    if (state.Raster.CullFace.IsDynamic) dynamicStates.push_back(VK_DYNAMIC_STATE_CULL_MODE);
    if (state.Raster.FrontFace.IsDynamic) dynamicStates.push_back(VK_DYNAMIC_STATE_FRONT_FACE);
    if (state.Raster.LineWidth.IsDynamic) dynamicStates.push_back(VK_DYNAMIC_STATE_LINE_WIDTH);

    VkPipelineRasterizationConservativeStateCreateInfoEXT conservativeRasterState = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_CONSERVATIVE_STATE_CREATE_INFO_EXT,
        .pNext = rasterizerCI.pNext,
        .conservativeRasterizationMode = VK_CONSERVATIVE_RASTERIZATION_MODE_OVERESTIMATE_EXT,
        .extraPrimitiveOverestimationSize = 0,
    };
    if (state.Raster.ConservativeRaster) {
        HAVK_ASSERT(PhysicalDevice.Features.ConservativeRaster);
        rasterizerCI.pNext = &conservativeRasterState;
    }

    VkPipelineMultisampleStateCreateInfo multisampleCI = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = (VkSampleCountFlagBits)state.MSAA.NumSamples,
        .sampleShadingEnable = state.MSAA.MinSampleShading > 0.0f,
        .minSampleShading = state.MSAA.MinSampleShading,
        .alphaToCoverageEnable = state.MSAA.AlphaToCoverage,
        .alphaToOneEnable = state.MSAA.AlphaToOne,
    };
    VkPipelineDepthStencilStateCreateInfo depthStencilCI = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = state.Depth.TestOp != VK_COMPARE_OP_ALWAYS,
        .depthWriteEnable = state.Depth.Write,
        .depthCompareOp = state.Depth.TestOp,
        .depthBoundsTestEnable = state.Depth.BoundsTest,
        .stencilTestEnable = false,
    };
    if (state.Depth.BoundsTest) dynamicStates.push_back(VK_DYNAMIC_STATE_DEPTH_BOUNDS);

    uint32_t numAttachments = 0;
    for (auto attach : outputs.Formats) {
        numAttachments += (attach != VK_FORMAT_UNDEFINED);
    }
    auto& blendConsts = state.Blend.Constants;

    VkPipelineColorBlendStateCreateInfo blendCI = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .logicOpEnable = state.Blend.LogicOp != VK_LOGIC_OP_COPY,
        .logicOp = state.Blend.LogicOp,
        .attachmentCount = numAttachments,
        .pAttachments = state.Blend.AttachmentStates,
        .blendConstants = { blendConsts.x, blendConsts.y, blendConsts.z, blendConsts.w },
    };

    VkPipelineDynamicStateCreateInfo dynamicStateCI = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = (uint32_t)dynamicStates.size(),
        .pDynamicStates = dynamicStates.data(),
    };

    VkPipelineRenderingCreateInfo renderingCI = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount = numAttachments,
        .pColorAttachmentFormats = outputs.Formats,
        .depthAttachmentFormat = outputs.DepthFormat,
        .stencilAttachmentFormat = outputs.StencilFormat,
    };
    VkGraphicsPipelineCreateInfo pipelineCI = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = &renderingCI,
        .stageCount = (uint32_t)stageInfos.size(),
        .pStages = stageInfos.data(),
        .pVertexInputState = &vertexInputCI,
        .pInputAssemblyState = &inputAssemblyCI,
        .pViewportState = &viewportCI,
        .pRasterizationState = &rasterizerCI,
        .pMultisampleState = &multisampleCI,
        .pDepthStencilState = &depthStencilCI,
        .pColorBlendState = &blendCI,
        .pDynamicState = &dynamicStateCI,
        .layout = DescriptorHeap->BindlessPipelineLayout,
    };
    auto instance = MakeUniqueResource<GraphicsPipeline>();

    if (OnCreatePipelineHook_) {
        HAVK_CHECK(OnCreatePipelineHook_(modules, (VkBaseInStructure*)&pipelineCI, stageInfos.data(), &instance->Handle));
    } else {
        HAVK_CHECK(vkCreateGraphicsPipelines(Device, PipelineCache, 1, &pipelineCI, nullptr, &instance->Handle));
    }

    if (Pfn.SetDebugUtilsObjectNameEXT != nullptr) {
        SetPipelineDebugName(this, instance->Handle, GetPipelineDebugName(modules));
    }
    if (_reloadWatcher != nullptr && (modules[0].Flags & ModuleDesc::kNoReload) == 0) {
        std::string name = GetPipelineDebugName(modules);
        auto reloadCb = [=, pipe = instance.get()](Span<const ModuleDesc> modules) {
            pipe->Context->Log(LogLevel::Info, "Reloading pipeline '%s'", name.data());

            auto newPipe = pipe->Context->CreateGraphicsPipeline(modules, state, outputs, specMap);
            std::swap(newPipe->Handle, pipe->Handle);
        };
        _reloadWatcher->BeginTracking(instance.get(), modules, std::move(reloadCb));
    }
    return instance;
}

ComputePipeline* DeviceContext::CreateStaticComputeProgram(uint32_t id, const ModuleDesc& mod) {
    ComputePipelinePtr instance = CreateComputePipeline(mod);

    if (_staticPrograms.size() <= id) {
        _staticPrograms.resize(id + 1);
    }
    _staticPrograms[id] = std::move(instance);
    return _staticPrograms[id].get();
}

Pipeline::~Pipeline() {
    if (Context->_reloadWatcher != nullptr) {
        Context->_reloadWatcher->StopTracking(this);
    }
    if (Context->OnDestroyPipelineHook_) {
        Context->OnDestroyPipelineHook_(*this);
    }
    vkDestroyPipeline(Context->Device, Handle, nullptr);
}

VkShaderStageFlagBits ModuleDesc::GetStage() const {
    auto words = (const uint32_t*)Code;
    HAVK_ASSERT(words[0] == spv::MagicNumber);

    uint32_t pos = 5, numWords = CodeSize / 4;

    while (pos < numWords) {
        uint16_t length = words[pos] >> 16;
        uint16_t opcode = words[pos] & 0xFFFF;

        if (opcode == spv::OpEntryPoint && strncmp(EntryPoint, (char*)&words[pos + 3], (numWords - pos - 3) * 4) == 0) {
            switch (words[pos + 1]) {
                case spv::ExecutionModelVertex: return VK_SHADER_STAGE_VERTEX_BIT;
                case spv::ExecutionModelFragment: return VK_SHADER_STAGE_FRAGMENT_BIT;
                case spv::ExecutionModelGLCompute: return VK_SHADER_STAGE_COMPUTE_BIT;
                case spv::ExecutionModelRayGenerationKHR: return VK_SHADER_STAGE_RAYGEN_BIT_KHR;
                case spv::ExecutionModelIntersectionKHR: return VK_SHADER_STAGE_INTERSECTION_BIT_KHR;
                case spv::ExecutionModelAnyHitKHR: return VK_SHADER_STAGE_ANY_HIT_BIT_KHR;
                case spv::ExecutionModelClosestHitKHR: return VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;
                case spv::ExecutionModelMissKHR: return VK_SHADER_STAGE_MISS_BIT_KHR;
                case spv::ExecutionModelCallableKHR: return VK_SHADER_STAGE_CALLABLE_BIT_KHR;
                case spv::ExecutionModelTaskEXT: return VK_SHADER_STAGE_TASK_BIT_EXT;
                case spv::ExecutionModelMeshEXT: return VK_SHADER_STAGE_MESH_BIT_EXT;
            }
        }
        pos += length;
    }
    return (VkShaderStageFlagBits)0;
}

BufferPtr DeviceContext::CreateBuffer(size_t sizeInBytes, BufferFlags flags, VkBufferUsageFlags usage, DebugLabel label) {
    auto buffer = MakeUniqueResource<Buffer>();
    buffer->Size = sizeInBytes;

    if (!(flags & BufferFlags::DeferredAlloc)) {
        HAVK_CHECK(buffer->Realloc(sizeInBytes, flags, usage, label));
    }
    return buffer;
}
VkResult Buffer::Realloc(size_t sizeInBytes, BufferFlags flags, VkBufferUsageFlags usage, DebugLabel label) {
    HAVK_ASSERT(Allocation == nullptr && "Can only allocate buffer once!");

    if (usage == 0) {
        // Most buffer usage flags only control alignment in practice, all of these combined will
        // force 64 byte align on tested drivers. One notable exception is DESCRIPTOR_BUFFER,
        // as some hardware requires descriptors to be in a specific and limited address space.
        usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

        // Can't default these or VVL will complain
        if (Context->PhysicalDevice.Features.RayQuery || Context->PhysicalDevice.Features.RayTracingPipeline) {
            usage |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                     VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR;
        }
    }

    VkBufferCreateInfo bufferCI = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = sizeInBytes,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VmaAllocationCreateInfo allocCI = {
        .usage = (flags & BufferFlags::DeviceMem) ? VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE :
                 (flags & BufferFlags::HostMem)   ? VMA_MEMORY_USAGE_AUTO_PREFER_HOST :
                                                    VMA_MEMORY_USAGE_AUTO,
    };
    if (flags & BufferFlags::MapSeqWrite) {
        allocCI.flags |= VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
    } else if (flags & (BufferFlags::MapCached | BufferFlags::HostMem)) {
        // default to CACHED if unspecified
        allocCI.flags |= VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
    }
    if (flags & BufferFlags::AllowNonHostVisible) {
        allocCI.flags |= VMA_ALLOCATION_CREATE_HOST_ACCESS_ALLOW_TRANSFER_INSTEAD_BIT;
    } else if ((allocCI.flags & VMA_ALLOCATION_CREATE_MAPPED_BIT) && !(flags & BufferFlags::AllowNonHostCoherent)) {
        allocCI.requiredFlags |= VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    }
    VmaAllocationInfo allocInfo;
    // At least for now, we'll force all buffers to have minAlign=256 to avoid issues with AS scratch/copy buffers.
    VkResult result = vmaCreateBufferWithAlignment(Context->Allocator, &bufferCI, &allocCI, 256, &Handle, &Allocation, &allocInfo);
    if (result != VK_SUCCESS) return result;

    Size = sizeInBytes;
    MappedData = (uint8_t*)allocInfo.pMappedData;

    if (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) {
        VkBufferDeviceAddressInfo addressGI = { .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = Handle };
        DeviceAddress = vkGetBufferDeviceAddress(Context->Device, &addressGI);
    }
    if (Context->Pfn.SetDebugUtilsObjectNameEXT != nullptr) {
        label.AssignToObject(Context, VK_OBJECT_TYPE_BUFFER, Handle);
    }
    return result;
}

struct CachedImageView {
    ImageViewDesc Key;
    VkImageView Handle;
    ImageHandle Descriptor;
};
struct Image::CustomViewMap {
    std::vector<CachedImageView> Views;
};

VkImageAspectFlags Image::GetAspectMask(VkFormat format) {
    switch (format) {
        case VK_FORMAT_D16_UNORM:
        case VK_FORMAT_X8_D24_UNORM_PACK32:
        case VK_FORMAT_D32_SFLOAT: return VK_IMAGE_ASPECT_DEPTH_BIT;

        case VK_FORMAT_D16_UNORM_S8_UINT:
        case VK_FORMAT_D24_UNORM_S8_UINT:
        case VK_FORMAT_D32_SFLOAT_S8_UINT: return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;

        case VK_FORMAT_S8_UINT: return VK_IMAGE_ASPECT_STENCIL_BIT;

        default: return VK_IMAGE_ASPECT_COLOR_BIT;
    }
}
static VkImageType GetImageType(VkImageViewType type) {
    switch (type) {
        case VK_IMAGE_VIEW_TYPE_1D: return VK_IMAGE_TYPE_1D;
        case VK_IMAGE_VIEW_TYPE_2D: return VK_IMAGE_TYPE_2D;
        case VK_IMAGE_VIEW_TYPE_3D: return VK_IMAGE_TYPE_3D;
        case VK_IMAGE_VIEW_TYPE_CUBE:
        case VK_IMAGE_VIEW_TYPE_2D_ARRAY:
        case VK_IMAGE_VIEW_TYPE_CUBE_ARRAY: return VK_IMAGE_TYPE_2D;
        default: HAVK_ASSERT(!"Image type not supported"); return VK_IMAGE_TYPE_2D;
    }
}
static VkComponentSwizzle ParseSwizzle(char ch) {
    switch (ch) {
        default: HAVK_ASSERT(!"Bad swizzle string"); [[fallthrough]];
        case '0': return VK_COMPONENT_SWIZZLE_ZERO;
        case '1': return VK_COMPONENT_SWIZZLE_ONE;
        case 'R': case 'X': return VK_COMPONENT_SWIZZLE_R;
        case 'G': case 'Y': return VK_COMPONENT_SWIZZLE_G;
        case 'B': case 'Z': return VK_COMPONENT_SWIZZLE_B;
        case 'A': case 'W': return VK_COMPONENT_SWIZZLE_A;
    }
}

ImagePtr DeviceContext::CreateImage(const ImageDesc& desc, DebugLabel label) {
    uint32_t maxAxis = desc.Size.x;
    if (desc.Type == VK_IMAGE_VIEW_TYPE_2D) maxAxis = std::max(maxAxis, desc.Size.y);
    if (desc.Type == VK_IMAGE_VIEW_TYPE_3D) maxAxis = std::max(maxAxis, desc.Size.z);
    bool isLayered = desc.IsLayered();

    VkImageCreateInfo imageCI = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = GetImageType(desc.Type),
        .format = desc.Format,
        .extent = { desc.Size.x, desc.Size.y, isLayered ? 1 : desc.Size.z },
        .mipLevels = std::min(desc.MipLevels, (uint32_t)std::bit_width(maxAxis)),
        .arrayLayers = isLayered ? desc.Size.z : 1,
        .samples = (VkSampleCountFlagBits)desc.NumSamples,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = desc.Usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VmaAllocationCreateInfo allocCI = {
        .usage = VMA_MEMORY_USAGE_AUTO,
        .requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
    };

    if (desc.Type == VK_IMAGE_VIEW_TYPE_CUBE || desc.Type == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY) {
        imageCI.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    }

    auto image = MakeUniqueResource<Image>();
    image->Format = imageCI.format;
    image->Usage = imageCI.usage;
    image->Size = desc.Size;
    image->MipLevels = (uint8_t)imageCI.mipLevels;
    image->NumSamples = desc.NumSamples;
    image->IsLayered = isLayered;
    HAVK_CHECK(vmaCreateImage(Allocator, &imageCI, &allocCI, &image->Handle, &image->Allocation, nullptr));

    VkImageViewCreateInfo viewCI = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = image->Handle,
        .viewType = desc.Type,
        .format = desc.Format,
        .components = {
            .r = ParseSwizzle(desc.ChannelSwizzle[0]),
            .g = ParseSwizzle(desc.ChannelSwizzle[1]),
            .b = ParseSwizzle(desc.ChannelSwizzle[2]),
            .a = ParseSwizzle(desc.ChannelSwizzle[3]),
        },
        .subresourceRange = {
            .aspectMask = Image::GetAspectMask(desc.Format),
            .levelCount = imageCI.mipLevels,
            .layerCount = imageCI.arrayLayers,
        },
    };
    HAVK_CHECK(vkCreateImageView(Device, &viewCI, nullptr, &image->ViewHandle));

    if (desc.Usage & (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT)) {
        image->DescriptorHandle = DescriptorHeap->CreateHandle(image->ViewHandle, desc.Usage);
    }
    if (Pfn.SetDebugUtilsObjectNameEXT != nullptr) {
        label.AssignToObject(this, VK_OBJECT_TYPE_IMAGE, image->Handle);
        label.AssignToObject(this, VK_OBJECT_TYPE_IMAGE_VIEW, image->ViewHandle);
    }

    // Initialize to GENERAL layout
    VkImageMemoryBarrier2 barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .dstStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .image = image->Handle,
        .subresourceRange = viewCI.subresourceRange,
    };
    VkDependencyInfo depInfo = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &barrier,
    };

    if (_prologueCmds == nullptr) {
        _prologueCmds = CreateCommandList();
    }
    vkCmdPipelineBarrier2(_prologueCmds->Handle, &depInfo);

    return image;
}

Image::~Image() {
    if (_customViews != nullptr) {
        for (auto& view : _customViews->Views) {
            if (view.Descriptor) {
                Context->DescriptorHeap->DestroyHandle(view.Descriptor);
            }
            vkDestroyImageView(Context->Device, view.Handle, nullptr);
        }
        delete _customViews;
    }
    if (DescriptorHandle) {
        Context->DescriptorHeap->DestroyHandle(DescriptorHandle);
    }
    vkDestroyImageView(Context->Device, ViewHandle, nullptr);

    if (Allocation != nullptr) {
        vmaDestroyImage(Context->Allocator, Handle, Allocation);
    }
}

ImageView Image::GetSubView(const ImageViewDesc& desc_) {
    ImageViewDesc desc = desc_;
    if (desc.MipLevels == 0) desc.MipLevels = (uint8_t)(MipLevels - desc.MipOffset);
    if (desc.NumLayers == 0) desc.NumLayers = (uint16_t)(GetNumLayers() - desc.LayerOffset);
    if (desc.Format == VK_FORMAT_UNDEFINED) desc.Format = Format;

    if (_customViews == nullptr) {
        _customViews = new CustomViewMap();
    }
    CachedImageView* cachedView = nullptr;

    for (CachedImageView& candidateView : _customViews->Views) {
        if (candidateView.Key == desc) {
            cachedView = &candidateView;
            break;
        }
    }
    if (cachedView == nullptr) {
        VkImageViewCreateInfo viewCI = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = Handle,
            .viewType = desc.Type,
            .format = desc.Format,
            .components = {
                .r = ParseSwizzle(desc.ChannelSwizzle[0]),
                .g = ParseSwizzle(desc.ChannelSwizzle[1]),
                .b = ParseSwizzle(desc.ChannelSwizzle[2]),
                .a = ParseSwizzle(desc.ChannelSwizzle[3]),
            },
            .subresourceRange = {
                .aspectMask = GetAspectMask(desc.Format),
                .baseMipLevel = desc.MipOffset,
                .levelCount = desc.MipLevels,
                .baseArrayLayer = desc.LayerOffset,
                .layerCount = desc.NumLayers,
            },
        };
        cachedView = &_customViews->Views.emplace_back();
        cachedView->Key = desc;
        HAVK_CHECK(vkCreateImageView(Context->Device, &viewCI, nullptr, &cachedView->Handle));

        if (desc.ShaderUsage & (VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)) {
            cachedView->Descriptor = Context->DescriptorHeap->CreateHandle(cachedView->Handle, desc.ShaderUsage);
        }
    }
    ImageView view;
    view.SourceImage = this;
    view.Handle = cachedView->Handle;
    view.DescriptorHandle = cachedView->Descriptor;
    return view;
}

DescriptorHeap::DescriptorHeap(DeviceContext* ctx) {
    Context = ctx;

    VkDescriptorPoolSize poolSizes[] = {
        { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kMaxImages },
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kMaxImages },
        { VK_DESCRIPTOR_TYPE_SAMPLER, kMaxSamplers + kNumImutSamplers },
    };
    VkDescriptorPoolCreateInfo poolCI = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT,
        .maxSets = 1,
        .poolSizeCount = std::size(poolSizes),
        .pPoolSizes = poolSizes,
    };
    HAVK_CHECK(vkCreateDescriptorPool(Context->Device, &poolCI, nullptr, &Pool));

    const VkDescriptorBindingFlags flags =
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | 
        VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
        VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;

    VkDescriptorBindingFlags bindingFlags[4] = { flags, flags, 0, flags };
    VkDescriptorSetLayoutBindingFlagsCreateInfo bindingFlagsCI = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
        .bindingCount = std::size(bindingFlags),
        .pBindingFlags = bindingFlags,
    };

    float defaultAnisotropy = ctx->PhysicalDevice.Props.limits.maxSamplerAnisotropy / 2;

    for (uint32_t i = 0; i < kNumImutSamplers; i++) {
        const VkFilter kFilters[] = { VK_FILTER_NEAREST, VK_FILTER_LINEAR, VK_FILTER_LINEAR };
        const VkSamplerAddressMode kWrapModes[] = { VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT,
                                                    VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE };
        uint32_t magFilter = i % 2, minFilter = i / 2 % 3, wrap = i / 6 % 4;
        VkSamplerCreateInfo samplerCI = {
            .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .magFilter = kFilters[magFilter],
            .minFilter = kFilters[minFilter],
            .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
            .addressModeU = kWrapModes[wrap],
            .addressModeV = kWrapModes[wrap],
            .addressModeW = kWrapModes[wrap],
            .anisotropyEnable = minFilter == 2,
            .maxAnisotropy = defaultAnisotropy,
            .maxLod = VK_LOD_CLAMP_NONE,
        };
        HAVK_CHECK(vkCreateSampler(ctx->Device, &samplerCI, nullptr, &_imutSamplers[i]));
    }

    VkDescriptorSetLayoutBinding bindings[] = {
        { 0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kMaxImages, VK_SHADER_STAGE_ALL },
        { 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kMaxImages, VK_SHADER_STAGE_ALL },
        { 2, VK_DESCRIPTOR_TYPE_SAMPLER, kNumImutSamplers, VK_SHADER_STAGE_ALL, _imutSamplers },
        { 3, VK_DESCRIPTOR_TYPE_SAMPLER, kMaxSamplers, VK_SHADER_STAGE_ALL },
    };
    VkDescriptorSetLayoutCreateInfo layoutCI = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .pNext = &bindingFlagsCI,
        .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT,
        .bindingCount = std::size(bindings),
        .pBindings = bindings,
    };
    HAVK_CHECK(vkCreateDescriptorSetLayout(Context->Device, &layoutCI, nullptr, &SetLayout));

    VkDescriptorSetAllocateInfo allocCI = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = Pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &SetLayout,
    };
    HAVK_CHECK(vkAllocateDescriptorSets(Context->Device, &allocCI, &Set));

    VkPushConstantRange pcRange = { VK_SHADER_STAGE_ALL, 0, 256 };
    VkPipelineLayoutCreateInfo pipelineLayoutCI = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &SetLayout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &pcRange,
    };
    HAVK_CHECK(vkCreatePipelineLayout(ctx->Device, &pipelineLayoutCI, nullptr, &BindlessPipelineLayout));
}
DescriptorHeap::~DescriptorHeap() {
    for (auto& [desc, reduceMode, sampler, descHandle] : _samplerMap) {
        vkDestroySampler(Context->Device, sampler, nullptr);
    }
    for (auto& handle : _imutSamplers) {
        vkDestroySampler(Context->Device, handle, nullptr);
    }
    vkDestroyPipelineLayout(Context->Device, BindlessPipelineLayout, nullptr);
    vkDestroyDescriptorSetLayout(Context->Device, SetLayout, nullptr);
    vkDestroyDescriptorPool(Context->Device, Pool, nullptr);
}

ImageHandle DescriptorHeap::CreateHandle(VkImageView viewHandle, VkImageUsageFlags usage) {
    uint32_t index = _allocator.Alloc();
    if (index == ~0u) {
        throw std::runtime_error("Descriptor heap is full");
    }
    VkDescriptorImageInfo imageInfo = { .imageView = viewHandle, .imageLayout = VK_IMAGE_LAYOUT_GENERAL };
    VkWriteDescriptorSet writeReq = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = Set,
        .dstArrayElement = index,
        .descriptorCount = 1,
        .pImageInfo = &imageInfo,
    };
    if (usage & VK_IMAGE_USAGE_SAMPLED_BIT) {
        writeReq.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        writeReq.dstBinding = 0;
        vkUpdateDescriptorSets(Context->Device, 1, &writeReq, 0, nullptr);
    }
    if (usage & VK_IMAGE_USAGE_STORAGE_BIT) {
        writeReq.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writeReq.dstBinding = 1;
        vkUpdateDescriptorSets(Context->Device, 1, &writeReq, 0, nullptr);
    }
    return { index };
}
void DescriptorHeap::DestroyHandle(ImageHandle handle) {
    _allocator.Free(handle.HeapIndex);
    // Nothing else to do, spec says "descriptors become undefined after underlying resources are destroyed".
}

SamplerHandle DescriptorHeap::GetSampler(const VkSamplerCreateInfo& desc) {
    VkSamplerReductionMode reduceMode = VK_SAMPLER_REDUCTION_MODE_WEIGHTED_AVERAGE;

    for (auto ext = (const VkBaseInStructure*)desc.pNext; ext != nullptr; ext = ext->pNext) {
        if (ext->sType == VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO) {
            reduceMode = ((VkSamplerReductionModeCreateInfo*)ext)->reductionMode;
        } else {
            HAVK_ASSERT("Unsupported sampler descriptor extension");
        }
    }
    for (auto& [desc2, reduceMode2, sampler2, descHandle2] : _samplerMap) {
        if (memcmp(&desc.flags, &desc2.flags, sizeof(VkSamplerCreateInfo) - sizeof(VkBaseInStructure)) != 0) continue;
        if (reduceMode != reduceMode2) continue;

        return descHandle2;
    }

    SamplerHandle descHandle = { .HeapIndex = (uint32_t)_samplerMap.size() };
    VkSampler sampler;
    HAVK_CHECK(vkCreateSampler(Context->Device, &desc, nullptr, &sampler));

    VkDescriptorImageInfo imageInfo = { .sampler = sampler };
    VkWriteDescriptorSet writeReq = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = Set,
        .dstBinding = 3,
        .dstArrayElement = descHandle.HeapIndex,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
        .pImageInfo = &imageInfo,
    };
    vkUpdateDescriptorSets(Context->Device, 1, &writeReq, 0, nullptr);
    _samplerMap.push_back({ desc, reduceMode, sampler, descHandle });
    return descHandle;
}

uint32_t DescriptorHeap::HandleAllocator::Alloc() {
    for (uint32_t i = 0; i < std::size(UsedMap); i++) {
        uint32_t wi = (i + NextFreeWordIdxHint) % std::size(UsedMap);
        if (UsedMap[wi] != ~0ull) {
            uint32_t j = (uint32_t)std::countr_one(UsedMap[wi]);
            UsedMap[wi] |= 1ull << j;
            NextFreeWordIdxHint = wi;

            return wi * 64 + j;
        }
    }
    return ~0u;
}
void DescriptorHeap::HandleAllocator::Free(uint32_t addr) {
    HAVK_ASSERT(addr != 0 && (addr / 64) < std::size(UsedMap));
    UsedMap[addr / 64] &= ~(1ull << (addr & 63));
}

void DebugLabel::AssignToObject(DeviceContext* ctx, VkObjectType objType, void* objHandle) const {
    if (ctx->Pfn.SetDebugUtilsObjectNameEXT == nullptr) return;

    std::string text;
    const char* fmt = Format;
    uint32_t argi = 0;

    for (char ch; (ch = *fmt++) != '\0';) {
        if (ch == '%') {
            switch (*fmt++) {
                case 's': text += (const char*)Args[argi++]; break;
                case 'd': text += std::to_string((int64_t)Args[argi++]); break;
                default: HAVK_ASSERT(!"Bad format specifier"); break;
            }
        } else {
            text += ch;
        }
    }

    VkDebugUtilsObjectNameInfoEXT nameInfo = {
        .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
        .objectType = objType,
        .objectHandle = (uint64_t)objHandle,
        .pObjectName = text.data(),
    };
    ctx->Pfn.SetDebugUtilsObjectNameEXT(ctx->Device, &nameInfo);
}

void AccelStructBuildDesc::CalculateSizes(DeviceContext* ctx, VkAccelerationStructureTypeKHR type,
                                          VkBuildAccelerationStructureFlagsKHR buildFlags) {
    VkAccelerationStructureBuildGeometryInfoKHR buildInfo = {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
        .type = type,
        .flags = buildFlags,
        .mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
        .geometryCount = (uint32_t)Geometries.size(),
        .pGeometries = Geometries.data(),
    };
    VkAccelerationStructureBuildSizesInfoKHR sizeInfo = { .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
    ctx->Pfn.GetAccelerationStructureBuildSizesKHR(ctx->Device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &buildInfo,
                                                   PrimitiveCounts.data(), &sizeInfo);
    AccelStructSize = (sizeInfo.accelerationStructureSize + 255) & ~size_t(255);
    BuildScratchSize = (sizeInfo.buildScratchSize + 255) & ~size_t(255);
    UpdateScratchSize = (sizeInfo.updateScratchSize + 255) & ~size_t(255);
    Flags = buildFlags;
    Type = type;
}
void AccelStructBuildDesc::CalculateUpperBoundSizesForTLAS(DeviceContext* ctx, uint32_t maxInstanceCount,
                                                           VkBuildAccelerationStructureFlagsKHR buildFlags) {
    // Spec states:
    // > Any VkDeviceOrHostAddressKHR or VkDeviceOrHostAddressConstKHR members of pBuildInfo are ignored by this command,
    // > except that the hostAddress member of VkAccelerationStructureGeometryTrianglesDataKHR::transformData will be
    // > examined to check if it is NULL.
    VkAccelerationStructureGeometryKHR geometry = {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
        .geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR,
        .geometry = { .instances = {
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR,
            .arrayOfPointers = false,
        } },
    };
    VkAccelerationStructureBuildGeometryInfoKHR buildInfo = {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
        .type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
        .flags = buildFlags,
        .mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
        .geometryCount = 1,
        .pGeometries = &geometry,
    };
    VkAccelerationStructureBuildSizesInfoKHR sizeInfo = { .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
    ctx->Pfn.GetAccelerationStructureBuildSizesKHR(ctx->Device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &buildInfo,
                                                   &maxInstanceCount, &sizeInfo);
    AccelStructSize = (sizeInfo.accelerationStructureSize + 255) & ~size_t(255);
    BuildScratchSize = (sizeInfo.buildScratchSize + 255) & ~size_t(255);
    UpdateScratchSize = (sizeInfo.updateScratchSize + 255) & ~size_t(255);
    Flags = buildFlags;
    Type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
}

AccelStructPoolPtr DeviceContext::CreateAccelStructPool() {
    return MakeUniqueResource<AccelStructPool>();
}
AccelStructPool::~AccelStructPool() {
    for (auto node : Nodes) {
        Context->Pfn.DestroyAccelerationStructureKHR(Context->Device, node.Handle, nullptr);
    }
    if (StorageBuffer_ != nullptr) {
        vmaDestroyBuffer(Context->Allocator, StorageBuffer_, StorageAllocation_);
        vmaClearVirtualBlock(RangeAllocator_);
        vmaDestroyVirtualBlock(RangeAllocator_);
    }
}

VkResult AccelStructPool::CreateStorage(size_t capacity, bool useLinearSubAllocator) {
    if (Nodes.size() > 0 || StorageBuffer_ != nullptr) {
        this->~AccelStructPool();
        Nodes.clear();
        StorageBuffer_ = nullptr;
        StorageAllocation_ = nullptr;
        RangeAllocator_ = nullptr;
    }

    VkBufferCreateInfo bufferCI = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = capacity,
        .usage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
    };
    VmaAllocationCreateInfo allocCI = { .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE };
    VkResult res = vmaCreateBuffer(Context->Allocator, &bufferCI, &allocCI, &StorageBuffer_, &StorageAllocation_, nullptr);
    if (res != VK_SUCCESS) return res;

    VmaVirtualBlockCreateInfo blockCI = {
        .size = capacity,
        .flags = useLinearSubAllocator ? VMA_VIRTUAL_BLOCK_CREATE_LINEAR_ALGORITHM_BIT : 0u,
    };
    HAVK_CHECK(vmaCreateVirtualBlock(&blockCI, &RangeAllocator_));

    return VK_SUCCESS;
}

VkResult AccelStructPool::Reserve(uint32_t nodeIdx, size_t storageSize) {
    HAVK_ASSERT(StorageBuffer_ != nullptr && "CreateStorage() must be called first");

    if (Nodes.size() <= nodeIdx) {
        Nodes.resize(nodeIdx + 1);
    } else if (Nodes[nodeIdx].Handle != nullptr) {
        VmaVirtualAllocationInfo rangeInfo;
        vmaGetVirtualAllocationInfo(RangeAllocator_, Nodes[nodeIdx].Range, &rangeInfo);
        if (rangeInfo.size == storageSize) return VK_SUCCESS;

        Context->Pfn.DestroyAccelerationStructureKHR(Context->Device, Nodes[nodeIdx].Handle, nullptr);
        vmaVirtualFree(RangeAllocator_, Nodes[nodeIdx].Range);
        Nodes[nodeIdx] = {};
    }
    Node& node = Nodes[nodeIdx];

    VmaVirtualAllocationCreateInfo allocCI = { .size = storageSize, .alignment = 256 };
    size_t offset;
    VkResult res = vmaVirtualAllocate(RangeAllocator_, &allocCI, &node.Range, &offset);
    if (res != VK_SUCCESS) return res;

    VkAccelerationStructureCreateInfoKHR accelsCI = {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
        .buffer = StorageBuffer_,
        .offset = offset,
        .size = allocCI.size,
        .type = VK_ACCELERATION_STRUCTURE_TYPE_GENERIC_KHR,
    };
    return Context->Pfn.CreateAccelerationStructureKHR(Context->Device, &accelsCI, nullptr, &node.Handle);
}

VkResult AccelStructPool::Build(CommandList& cmdList, uint32_t nodeIdx, const AccelStructBuildDesc& buildDesc,
                                BufferSpan<uint8_t> scratchBuffer) {
    HAVK_ASSERT(scratchBuffer.size() >= buildDesc.BuildScratchSize);

    VkResult res = Reserve(nodeIdx, buildDesc.AccelStructSize);
    if (res != VK_SUCCESS) return res;

    VkAccelerationStructureBuildGeometryInfoKHR buildInfo = {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
        .type = buildDesc.Type,
        .flags = buildDesc.Flags,
        .mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
        .dstAccelerationStructure = Nodes[nodeIdx].Handle,
        .geometryCount = buildDesc.NumGeometries(),
        .pGeometries = buildDesc.Geometries.data(),
        .scratchData = { scratchBuffer.device_addr() },
    };

    // Is this an entirely redundant way to offset data? FFS.
    auto buildRanges = std::vector<VkAccelerationStructureBuildRangeInfoKHR>(buildDesc.NumGeometries());
    auto pBuildRanges = std::vector<VkAccelerationStructureBuildRangeInfoKHR*>(buildDesc.NumGeometries());
    for (uint32_t j = 0; j < buildDesc.NumGeometries(); j++) {
        buildRanges[j] = { .primitiveCount = buildDesc.PrimitiveCounts[j] };
        pBuildRanges[j] = &buildRanges[j];
    }
    Context->Pfn.CmdBuildAccelerationStructuresKHR(cmdList.Handle, 1, &buildInfo, pBuildRanges.data());
    return VK_SUCCESS;
}

void AccelStructPool::GetProps(CommandList& cmdList, Span<const uint32_t> nodeIds, BufferSpan<uint64_t> results, VkQueryType prop) {
    int propIdx = prop == VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR     ? 0 :
                  prop == VK_QUERY_TYPE_ACCELERATION_STRUCTURE_SERIALIZATION_SIZE_KHR ? 1 :
                                                                                        -1;
    HAVK_ASSERT(results.size() >= nodeIds.size() && propIdx >= 0);

    VkQueryPool& queryPool = Context->_accelsSizeQueryPools[propIdx];
    const uint32_t queryPoolCap = 512;

    if (queryPool == nullptr) {
        VkQueryPoolCreateInfo queryCI = {
            .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
            .queryType = prop,
            .queryCount = queryPoolCap,
        };
        vkCreateQueryPool(Context->Device, &queryCI, nullptr, &queryPool);
    }

    for (uint32_t offset = 0; offset < nodeIds.size(); offset += queryPoolCap) {
        uint32_t count = std::min((uint32_t)nodeIds.size() - offset, queryPoolCap);
        VkAccelerationStructureKHR nodeHandles[queryPoolCap];

        for (uint32_t i = 0; i < count; i++) {
            nodeHandles[i] = Nodes[nodeIds[offset + i]].Handle;
        }
        vkCmdResetQueryPool(cmdList.Handle, queryPool, 0, count);
        Context->Pfn.CmdWriteAccelerationStructuresPropertiesKHR(cmdList.Handle, count, nodeHandles, prop, queryPool, 0);
        vkCmdCopyQueryPoolResults(cmdList.Handle, queryPool, 0, count, results.source_buffer().Handle,
                                  results.offset_bytes() + offset * 8, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);

        cmdList.Barrier({
            .SrcStages = VK_PIPELINE_STAGE_TRANSFER_BIT,
            .DstStages = offset + count < nodeIds.size() ? VK_PIPELINE_STAGE_2_TRANSFER_BIT : VK_PIPELINE_STAGE_2_HOST_BIT,
        });
    }
}

VkResult AccelStructPool::Compact(CommandList& cmdList, uint32_t nodeIdx, AccelStructPool& destPool, uint32_t destNodeIdx,
                                  uint64_t compactedSize) {
    // We can't easily support in-place compact without allocating extra mem.
    HAVK_ASSERT(!(destPool.StorageBuffer_ == StorageBuffer_ && nodeIdx == destNodeIdx));

    VkResult res = destPool.Reserve(destNodeIdx, compactedSize);
    if (res != VK_SUCCESS) return res;

    VkCopyAccelerationStructureInfoKHR copyInfo = {
        .sType = VK_STRUCTURE_TYPE_COPY_ACCELERATION_STRUCTURE_INFO_KHR,
        .src = Nodes[nodeIdx].Handle,
        .dst = destPool.Nodes[destNodeIdx].Handle,
        .mode = VK_COPY_ACCELERATION_STRUCTURE_MODE_COMPACT_KHR,
    };
    Context->Pfn.CmdCopyAccelerationStructureKHR(cmdList.Handle, &copyInfo);
    return VK_SUCCESS;
}

void AccelStructPool::Serialize(CommandList& cmdList, uint32_t nodeIdx, BufferSpan<uint8_t> destData) {
    VkCopyAccelerationStructureToMemoryInfoKHR copyInfo = {
        .sType = VK_STRUCTURE_TYPE_COPY_ACCELERATION_STRUCTURE_TO_MEMORY_INFO_KHR,
        .src = Nodes[nodeIdx].Handle,
        .dst = { .deviceAddress = destData.device_addr() },
        .mode = VK_COPY_ACCELERATION_STRUCTURE_MODE_SERIALIZE_KHR,
    };
    Context->Pfn.CmdCopyAccelerationStructureToMemoryKHR(cmdList.Handle, &copyInfo);
}
VkResult AccelStructPool::Deserialize(CommandList& cmdList, uint32_t nodeIdx, BufferSpan<const uint8_t> srcData,
                                      uint64_t deserializedSize) {
    if (srcData.size() < 56 || deserializedSize == 0) return VK_ERROR_INCOMPATIBLE_DRIVER;

    VkResult res = Reserve(nodeIdx, deserializedSize);
    if (res != VK_SUCCESS) return res;

    VkCopyMemoryToAccelerationStructureInfoKHR copyInfo = {
        .sType = VK_STRUCTURE_TYPE_COPY_MEMORY_TO_ACCELERATION_STRUCTURE_INFO_KHR,
        .src = { .deviceAddress = srcData.device_addr() },
        .dst = Nodes[nodeIdx].Handle,
        .mode = VK_COPY_ACCELERATION_STRUCTURE_MODE_DESERIALIZE_KHR,
    };
    Context->Pfn.CmdCopyMemoryToAccelerationStructureKHR(cmdList.Handle, &copyInfo);
    return VK_SUCCESS;
}
uint64_t AccelStructPool::GetDeserializedSize(const uint8_t* headerData, size_t dataSize) {
    // Serialized header is at least 56 bytes. 2x UUID + 3x uint64
    if (dataSize < 56) return 0;

    VkAccelerationStructureVersionInfoKHR verInfo = {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_VERSION_INFO_KHR,
        .pVersionData = headerData,
    };
    VkAccelerationStructureCompatibilityKHR compat;
    Context->Pfn.GetDeviceAccelerationStructureCompatibilityKHR(Context->Device, &verInfo, &compat);
    if (compat != VK_ACCELERATION_STRUCTURE_COMPATIBILITY_COMPATIBLE_KHR) return 0;

    return *(uint64_t*)(headerData + 40);
}

void CommandList::BeginRendering(const RenderingDesc& desc, bool setViewport) {
    VkRenderingAttachmentInfo attachInfos[10];
    uint32_t attachIdx = 0;
    VkMemoryBarrier2 barrier = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = desc.SrcStages, .dstStageMask = desc.DstStages };

    auto PushAttachmentInfo = [&](const RenderAttachment& attach, bool isColor) {
        VkRenderingAttachmentInfo& info = attachInfos[attachIdx++] = {
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView = attach.Target.Handle,
            .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
            .loadOp = attach.LoadOp,
            .storeOp = attach.StoreOp,
            .clearValue = attach.ClearValue,
        };
        if (attach.ResolveTarget) {
            info.resolveMode = attach.ResolveMode;
            info.resolveImageView = attach.ResolveTarget->ViewHandle;
            info.resolveImageLayout = VK_IMAGE_LAYOUT_GENERAL;
        }
        if (attach.LoadOp == VK_ATTACHMENT_LOAD_OP_LOAD) {
            barrier.srcAccessMask |= VK_ACCESS_MEMORY_WRITE_BIT;
            barrier.dstAccessMask |= isColor ? VK_ACCESS_COLOR_ATTACHMENT_READ_BIT : VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
        }
        if (attach.StoreOp == VK_ATTACHMENT_STORE_OP_STORE || attach.LoadOp == VK_ATTACHMENT_LOAD_OP_CLEAR || attach.ResolveTarget) {
            barrier.dstAccessMask |= isColor ? VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT : VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        }
        barrier.dstStageMask |= isColor ? VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT :
                                          (VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT);
        return &info;
    };

    VkRect2D region = { (int32_t)desc.RegionOffset.x, (int32_t)desc.RegionOffset.y, desc.RegionSize.x, desc.RegionSize.y };

    if (desc.RegionSize.x == 0 && desc.RegionSize.y == 0) {
        auto& mainAttach = desc.Attachments[0].Target  ? desc.Attachments[0] :
                           desc.DepthAttachment.Target ? desc.DepthAttachment :
                                                         desc.StencilAttachment;
        auto mainSize = mainAttach.Target.SourceImage->Size;
        region = { 0, 0, mainSize.x, mainSize.y };
    }

    VkRenderingInfo info = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea = region,
        .layerCount = 1,
        .pColorAttachments = attachInfos,
    };

    for (auto& attach : desc.Attachments) {
        if (!attach.Target) break;

        PushAttachmentInfo(attach, true);
        info.colorAttachmentCount++;
    }

    if (desc.DepthAttachment.Target) {
        info.pDepthAttachment = PushAttachmentInfo(desc.DepthAttachment, false);
    }
    if (desc.StencilAttachment.Target) {
        info.pStencilAttachment = PushAttachmentInfo(desc.StencilAttachment, false);
    }

    if (desc.SrcStages != VK_PIPELINE_STAGE_FLAG_BITS_MAX_ENUM) {
        VkDependencyInfo depInfo = { .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &barrier };
        vkCmdPipelineBarrier2(Handle, &depInfo);
    }
    vkCmdBeginRendering(Handle, &info);

    if (setViewport) {
        SetViewport({ (float)region.offset.x, (float)region.offset.y, (float)region.extent.width, (float)region.extent.height, 0, +1 });
        SetScissor(region);
    }
}

void CommandList::CopyBufferToImage(const CopyBufferToImageParams& pars) {
    auto offset = pars.DstOffset, extent = pars.DstExtent;
    if (extent.x == ~0u) extent.x = pars.DstImage.Size.x;
    if (extent.y == ~0u) extent.y = pars.DstImage.Size.y;
    bool isLayered = pars.DstImage.IsLayered;

    if (pars.SrcStages != VK_PIPELINE_STAGE_FLAG_BITS_MAX_ENUM) {
        Barrier({ .SrcStages = pars.SrcStages, .DstStages = VK_PIPELINE_STAGE_2_COPY_BIT });
    }

    VkBufferImageCopy region = {
        .bufferOffset = pars.SrcData.offset_bytes(),
        .bufferRowLength = pars.PixelsPerRow,
        .bufferImageHeight = pars.RowsPerLayer,
        .imageSubresource = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .mipLevel = pars.DstMipLevel,
            .baseArrayLayer = isLayered ? offset.z : 0,
            .layerCount = isLayered ? extent.z : 1,
        },
        .imageOffset = { (int)offset.x, (int)offset.y, isLayered ? 0 : (int)offset.z },
        .imageExtent = { extent.x, extent.y, isLayered ? 1 : extent.z },
    };
    VkImage imageHandle = pars.DstImage.Handle;
    vkCmdCopyBufferToImage(Handle, pars.SrcData.source_buffer().Handle, imageHandle, VK_IMAGE_LAYOUT_GENERAL, 1, &region);

    if (pars.GenerateMips) {
        VkImageBlit blit = { .srcSubresource = region.imageSubresource, .dstSubresource = region.imageSubresource };

        for (uint32_t i = pars.DstMipLevel + 1; i < pars.DstImage.MipLevels; i++) {
            uint32_t j = i - 1;
            blit.srcSubresource.mipLevel = j;
            blit.srcOffsets[0] = { (int)offset.x >> j, (int)offset.y >> j, (int)offset.z >> j };
            blit.srcOffsets[1] = { std::max((int)extent.x >> j, 1), std::max((int)extent.y >> j, 1), std::max((int)extent.z >> j, 1) };

            blit.dstSubresource.mipLevel = i;
            blit.dstOffsets[0] = { (int)offset.x >> i, (int)offset.y >> i, (int)offset.z >> i };
            blit.dstOffsets[1] = { std::max((int)extent.x >> i, 1), std::max((int)extent.y >> i, 1), std::max((int)extent.z >> i, 1) };

            Barrier({ .SrcStages = VK_PIPELINE_STAGE_TRANSFER_BIT, .DstStages = VK_PIPELINE_STAGE_TRANSFER_BIT });
            vkCmdBlitImage(Handle, imageHandle, VK_IMAGE_LAYOUT_GENERAL, imageHandle, VK_IMAGE_LAYOUT_GENERAL, 1, &blit, VK_FILTER_LINEAR);
        }
    }

    if (pars.DstStages != VK_PIPELINE_STAGE_FLAG_BITS_MAX_ENUM) {
        Barrier({ .SrcStages = VK_PIPELINE_STAGE_TRANSFER_BIT, .DstStages = pars.DstStages });
    }
}

void CommandList::BeginDebugLabel(const char* fmt, uint32_t color, ...) {
    if (!Context->Pfn.CmdBeginDebugUtilsLabelEXT) return;

    if (color == 0) {
        color = (uint32_t)std::hash<std::string_view>{}(fmt);
    }
    char text[256];
    va_list args;
    va_start(args, color);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);

    VkDebugUtilsLabelEXT info = {
        .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT,
        .pLabelName = text,
        .color = { (color >> 16 & 255) / 255.0f, (color >> 8 & 255) / 255.0f, (color >> 0 & 255) / 255.0f, (color == 0 ? 0.0f : 1.0f) },
    };
    Context->Pfn.CmdBeginDebugUtilsLabelEXT(Handle, &info);
}

void CommandList::EndDebugLabel() {
    if (!Context->Pfn.CmdEndDebugUtilsLabelEXT) return;

    Context->Pfn.CmdEndDebugUtilsLabelEXT(Handle);
}

SwapchainPtr DeviceContext::CreateSwapchain(VkSurfaceKHR surface) {
    return std::make_unique<Swapchain>(this, surface);
}
Swapchain::Swapchain(DeviceContext* ctx, VkSurfaceKHR surface) {
    Context = ctx;
    Surface = surface;

    VkPhysicalDevice device = ctx->PhysicalDevice.Handle;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, Surface, &SurfaceCaps);

    uint32_t count;
    vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &count, nullptr);
    _availFormats = std::vector<VkSurfaceFormatKHR>(count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &count, _availFormats.data());

    vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &count, nullptr);
    _availPresentModes.resize(count);
    vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &count, _availPresentModes.data());

    _selectedPresentMode = _availPresentModes[0];
    _selectedFormat = _availFormats[0];
    _selectedUsages = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
}

Swapchain::~Swapchain() {
    DestroyCurrentSwapchain();
    vkDestroySurfaceKHR(Context->Instance, Surface, nullptr);
}

static vectors::uint2 GetClampedSurfaceSize(const VkSurfaceCapabilitiesKHR& caps, vectors::uint2 size) {
    if (caps.currentExtent.width != UINT32_MAX) {
        size = { caps.currentExtent.width, caps.currentExtent.height };
    } else {
        size.x = std::min(std::max(size.x, caps.minImageExtent.width), caps.maxImageExtent.width);
        size.y = std::min(std::max(size.y, caps.minImageExtent.height), caps.maxImageExtent.height);
    }
    return size;
}

void Swapchain::Initialize(vectors::uint2 surfaceSize) {
    if (Handle != nullptr && !_invalid) return;

    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(Context->PhysicalDevice.Handle, Surface, &SurfaceCaps);
    surfaceSize = GetClampedSurfaceSize(SurfaceCaps, surfaceSize);

    uint32_t imageCount = std::max(GetNumFramesInFlight() + 1, SurfaceCaps.minImageCount);
    uint32_t maxImages = SurfaceCaps.maxImageCount;
    if (maxImages > 0 && imageCount > maxImages) imageCount = maxImages;

    VkSwapchainCreateInfoKHR swapchainCI = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = Surface,
        .minImageCount = imageCount,
        .imageFormat = _selectedFormat.format,
        .imageColorSpace = _selectedFormat.colorSpace,
        .imageExtent = { surfaceSize.x, surfaceSize.y },
        .imageArrayLayers = 1,
        .imageUsage = _selectedUsages,

        // We assume the main queue supports both graphics and present.
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,

        .preTransform = SurfaceCaps.currentTransform,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode = GetPresentMode(),
        .clipped = VK_TRUE,
        .oldSwapchain = Handle,
    };

    VkImageFormatListCreateInfo mutableFormatsCI = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO,
        .viewFormatCount = 2,
    };
    static const VkFormat srgbFormatPairs[3][2] = {
        { VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB },
        { VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_SRGB },
        { VK_FORMAT_A8B8G8R8_UNORM_PACK32, VK_FORMAT_A8B8G8R8_SRGB_PACK32 },
    };
    for (auto pair : srgbFormatPairs) {
        if (_selectedFormat.format == pair[0] || _selectedFormat.format == pair[1]) {
            swapchainCI.flags |= VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR;
            mutableFormatsCI.pViewFormats = pair;
            swapchainCI.pNext = &mutableFormatsCI;
            break;
        }
    }

    VkSwapchainKHR newSwapchain;
    HAVK_CHECK(vkCreateSwapchainKHR(Context->Device, &swapchainCI, nullptr, &newSwapchain));

    if (Handle != nullptr) {
        DestroyCurrentSwapchain();
    }
    Handle = newSwapchain;
    _invalid = false;

    vkGetSwapchainImagesKHR(Context->Device, Handle, &imageCount, nullptr);
    auto swapchainImages = std::vector<VkImage>(imageCount);
    vkGetSwapchainImagesKHR(Context->Device, Handle, &imageCount, swapchainImages.data());

    for (VkImage& imageHandle : swapchainImages) {
        VkImageViewCreateInfo viewCI = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = imageHandle,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = swapchainCI.imageFormat,
            .subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1 },
        };
        auto image = Context->MakeUniqueResource<Image>();
        image->Handle = imageHandle;
        image->Format = swapchainCI.imageFormat;
        image->Usage = swapchainCI.imageUsage;
        image->Size = { swapchainCI.imageExtent.width, swapchainCI.imageExtent.height, 1 };

        HAVK_CHECK(vkCreateImageView(Context->Device, &viewCI, nullptr, &image->ViewHandle));

        if (swapchainCI.imageUsage & (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT)) {
            image->DescriptorHandle = Context->DescriptorHeap->CreateHandle(image->ViewHandle, swapchainCI.imageUsage);
        }
        auto label = DebugLabel("Swapchain_%d", _images.size());
        label.AssignToObject(Context, VK_OBJECT_TYPE_IMAGE, image->Handle);
        label.AssignToObject(Context, VK_OBJECT_TYPE_IMAGE_VIEW, image->ViewHandle);

        VkSemaphoreCreateInfo semaphoreCI = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        VkSemaphore availSemaphore;
        HAVK_CHECK(vkCreateSemaphore(Context->Device, &semaphoreCI, nullptr, &availSemaphore));

        _images.push_back({ .Target = std::move(image), .RenderFinishedSemaphore = availSemaphore });
    }

    HAVK_ASSERT(_frameSync.empty());
    _frameSync.resize(GetNumFramesInFlight());
    for (uint32_t i = 0; i < _frameSync.size(); i++) {
        auto& frame = _frameSync[i];
        frame.CmdList = Context->CreateCommandList(QueueDomain::Main, false);

        VkSemaphoreCreateInfo semaphoreCI = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        HAVK_CHECK(vkCreateSemaphore(Context->Device, &semaphoreCI, nullptr, &frame.AvailableSemaphore));

        VkFenceCreateInfo fenceCI = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT };
        HAVK_CHECK(vkCreateFence(Context->Device, &fenceCI, nullptr, &frame.InFlightFence));
    }
}

void Swapchain::DestroyCurrentSwapchain() {
    Context->WaitIdle();

    for (auto& frame : _frameSync) {
        vkDestroySemaphore(Context->Device, frame.AvailableSemaphore, nullptr);
        vkDestroyFence(Context->Device, frame.InFlightFence, nullptr);
        Destroy(frame.CmdList);
    }
    _frameSync.clear();

    for (auto& image : _images) {
        vkDestroySemaphore(Context->Device, image.RenderFinishedSemaphore, nullptr);
    }
    _images.clear();

    vkDestroySwapchainKHR(Context->Device, Handle, nullptr);
    Handle = nullptr;
    _currImageIdx = 0;
}

std::pair<Image*, CommandList*> Swapchain::AcquireImage(vectors::uint2 surfaceSize) {
    auto currentSize = GetClampedSurfaceSize(SurfaceCaps, GetSurfaceSize());
    _invalid |= surfaceSize.x != currentSize.x || surfaceSize.y != currentSize.y;

    if (Handle == nullptr || _invalid) {
        Initialize(surfaceSize);
    }
    FrameSyncInfo& sync = _frameSync[_currFrameIdx];
    vkWaitForFences(Context->Device, 1, &sync.InFlightFence, VK_TRUE, UINT64_MAX);
    VkResult acquireResult = vkAcquireNextImageKHR(Context->Device, Handle, UINT64_MAX, sync.AvailableSemaphore, nullptr, &_currImageIdx);

    if (acquireResult == VK_ERROR_OUT_OF_DATE_KHR || acquireResult == VK_SUBOPTIMAL_KHR) {
        _invalid = true;
        return AcquireImage(surfaceSize);
    } else if (acquireResult != VK_SUCCESS) {
        Panic(acquireResult, "Failed to acquire image from swapchain");
    }

    Image* image = _images[_currImageIdx].Target.get();
    sync.CmdList->Begin();

    VkImageMemoryBarrier2 barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .dstStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .image = image->Handle,
        .subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1 },
    };
    VkDependencyInfo depInfo = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &barrier,
    };
    vkCmdPipelineBarrier2(sync.CmdList->Handle, &depInfo);

    return { image, sync.CmdList.get() };
}

void Swapchain::Present() {
    FrameSyncInfo& currSync = _frameSync[_currFrameIdx];
    SwcImageInfo& currImage = _images[_currImageIdx];

    // https://registry.khronos.org/vulkan/specs/latest/man/html/VkPresentInfoKHR.html
    // > When transitioning the image to the appropriate layout, there is no need to delay subsequent processing,
    // > or perform any visibility operations (as vkQueuePresentKHR performs automatic visibility operations).
    VkImageMemoryBarrier2 barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_NONE,
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        .image = currImage.Target->Handle,
        .subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1 },
    };
    VkDependencyInfo depInfo = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &barrier,
    };
    vkCmdPipelineBarrier2(currSync.CmdList->Handle, &depInfo);

    vkResetFences(Context->Device, 1, &currSync.InFlightFence);
    (void)currSync.CmdList->Submit(currSync.AvailableSemaphore, currImage.RenderFinishedSemaphore, currSync.InFlightFence);

    VkPresentInfoKHR presentInfo = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &currImage.RenderFinishedSemaphore,
        .swapchainCount = 1,
        .pSwapchains = &Handle,
        .pImageIndices = &_currImageIdx,
    };
    DeviceQueue* queue = Context->GetQueue(QueueDomain::Main);
    VkResult result = vkQueuePresentKHR(queue->Handle, &presentInfo);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
        _invalid = true;
    } else if (result != VK_SUCCESS) {
        Panic(result, "Failed to present image");
    }
    _currFrameIdx = (_currFrameIdx + 1) % _frameSync.size();
}

bool Swapchain::SetPreferredPresentModes(Span<const VkPresentModeKHR> modes) {
    for (auto reqMode : modes) {
        if (_selectedPresentMode == reqMode) return true;

        for (auto availMode : _availPresentModes) {
            if (availMode != reqMode) continue;

            _invalid = true;
            _selectedPresentMode = reqMode;
            return true;
        }
    }
    return false;
}

static VkImageUsageFlags GetSupportedUsages(VkPhysicalDevice device, VkFormat format) {
    VkFormatProperties props;
    vkGetPhysicalDeviceFormatProperties(device, format, &props);

    VkFormatFeatureFlags features = props.optimalTilingFeatures;
    VkImageUsageFlags usages = 0;

    if (features & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) usages |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (features & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) usages |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (features & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) usages |= VK_IMAGE_USAGE_STORAGE_BIT;
    if (features & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT) usages |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (features & VK_FORMAT_FEATURE_TRANSFER_DST_BIT) usages |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    return usages;
}
bool Swapchain::SetPreferredFormats(Span<const VkSurfaceFormatKHR> formats, VkImageUsageFlags requiredUsages) {
    for (auto reqFormat : formats) {
        for (auto availFormat : _availFormats) {
            if (availFormat.format != reqFormat.format) continue;
            if (availFormat.colorSpace != reqFormat.colorSpace) continue;

            auto supportedUsages = GetSupportedUsages(Context->PhysicalDevice.Handle, availFormat.format);
            if ((supportedUsages & requiredUsages) != requiredUsages) continue;

            _invalid = true;
            _selectedFormat = reqFormat;
            _selectedUsages = requiredUsages;
            return true;
        }
    }
    return false;
}

};  // namespace havk
