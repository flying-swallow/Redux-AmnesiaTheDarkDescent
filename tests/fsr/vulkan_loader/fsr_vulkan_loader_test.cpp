#include "volk.h"
#include <FidelityFX/host/backends/vk/ffx_vk.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <vector>

uint32_t findMemoryTypeIndex(VkPhysicalDevice physicalDevice, VkMemoryRequirements memRequirements,
                             VkMemoryPropertyFlags requestedProperties,
                             VkMemoryPropertyFlags& outProperties);

namespace {

struct FakePhysicalDeviceToken {};
struct FakeVulkanState;

struct FakeDeviceToken {
    FakeVulkanState* state = nullptr;
};

struct FakeEnumerationState {
    VkPhysicalDevice lastPhysicalDevice = VK_NULL_HANDLE;
    const char* lastLayerName = nullptr;
    uint32_t callCount = 0;
    uint32_t extensionCount = 0;
    uint32_t nonNullPropertiesCallCount = 0;
    uint32_t populatedEntryCount = 0;
};

FakeEnumerationState g_fakeEnumeration;

struct FakePropertiesState {
    VkPhysicalDeviceProperties properties = {};
    VkPhysicalDevice lastPhysicalDevice = VK_NULL_HANDLE;
    uint32_t callCount = 0;
};

FakePropertiesState g_fakeProperties;

struct FakeMemoryState {
    VkPhysicalDeviceMemoryProperties properties = {};
    VkPhysicalDevice lastPhysicalDevice = VK_NULL_HANDLE;
    uint32_t callCount = 0;
};

FakeMemoryState g_fakeMemory;

struct FakeBufferToken {
    bool alive = false;
    VkDeviceSize size = 0;
    VkDeviceMemory boundMemory = VK_NULL_HANDLE;
};

struct FakeMemoryToken {
    bool alive = false;
    bool mapped = false;
    std::vector<uint8_t> storage;
};

struct FakeDescriptorPoolToken {
    bool alive = false;
};

struct FakeVulkanState {
    std::array<FakeBufferToken, 16> buffers = {};
    std::array<FakeMemoryToken, 8> memories = {};
    std::array<FakeDescriptorPoolToken, 8> descriptorPools = {};

    uint32_t deviceBufferCreateCalls = 0;
    uint32_t directBufferCreateCalls = 0;
    uint32_t bufferDestroyCalls = 0;
    uint32_t descriptorPoolCreateCalls = 0;
    uint32_t descriptorPoolDestroyCalls = 0;
    uint32_t allocateMemoryCalls = 0;
    uint32_t freeMemoryCalls = 0;
    uint32_t mapMemoryCalls = 0;
    uint32_t unmapMemoryCalls = 0;
    uint32_t bindBufferMemoryCalls = 0;
    uint32_t deviceProcAddrCalls = 0;
    bool callbackError = false;

    void reset() {
        *this = {};
    }
};

FakeVulkanState g_fakeVulkan;

FakeVulkanState* fake_state(VkDevice device) {
    if (device == VK_NULL_HANDLE)
        return nullptr;
    return reinterpret_cast<FakeDeviceToken*>(device)->state;
}

FakeBufferToken* allocate_fake_buffer(FakeVulkanState* state) {
    if (!state)
        return nullptr;
    for (FakeBufferToken& buffer : state->buffers) {
        if (!buffer.alive)
            return &buffer;
    }
    state->callbackError = true;
    return nullptr;
}

FakeMemoryToken* allocate_fake_memory(FakeVulkanState* state) {
    if (!state)
        return nullptr;
    for (FakeMemoryToken& memory : state->memories) {
        if (!memory.alive)
            return &memory;
    }
    state->callbackError = true;
    return nullptr;
}

FakeDescriptorPoolToken* allocate_fake_descriptor_pool(FakeVulkanState* state) {
    if (!state)
        return nullptr;
    for (FakeDescriptorPoolToken& descriptorPool : state->descriptorPools) {
        if (!descriptorPool.alive)
            return &descriptorPool;
    }
    state->callbackError = true;
    return nullptr;
}

VKAPI_ATTR VkResult VKAPI_CALL fake_vkEnumerateDeviceExtensionProperties(
    VkPhysicalDevice physicalDevice,
    const char* pLayerName,
    uint32_t* pPropertyCount,
    VkExtensionProperties* pProperties) {
    ++g_fakeEnumeration.callCount;
    g_fakeEnumeration.lastPhysicalDevice = physicalDevice;
    g_fakeEnumeration.lastLayerName = pLayerName;
    if (pPropertyCount) {
        const uint32_t requestedCount = *pPropertyCount;
        *pPropertyCount = g_fakeEnumeration.extensionCount;
        if (pProperties) {
            ++g_fakeEnumeration.nonNullPropertiesCallCount;
            const uint32_t countToPopulate = requestedCount < g_fakeEnumeration.extensionCount
                ? requestedCount
                : g_fakeEnumeration.extensionCount;
            for (uint32_t i = 0; i < countToPopulate; ++i) {
                pProperties[i] = {};
                std::snprintf(pProperties[i].extensionName, VK_MAX_EXTENSION_NAME_SIZE,
                              "FAKE_inert_extension_%u", i);
                pProperties[i].specVersion = 1;
                ++g_fakeEnumeration.populatedEntryCount;
            }
        }
    }
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL fake_vkGetPhysicalDeviceMemoryProperties(
    VkPhysicalDevice physicalDevice, VkPhysicalDeviceMemoryProperties* pMemoryProperties) {
    ++g_fakeMemory.callCount;
    g_fakeMemory.lastPhysicalDevice = physicalDevice;
    if (pMemoryProperties)
        *pMemoryProperties = g_fakeMemory.properties;
}

VKAPI_ATTR void VKAPI_CALL fake_vkGetPhysicalDeviceProperties(
    VkPhysicalDevice physicalDevice, VkPhysicalDeviceProperties* pProperties) {
    ++g_fakeProperties.callCount;
    g_fakeProperties.lastPhysicalDevice = physicalDevice;
    if (pProperties)
        *pProperties = g_fakeProperties.properties;
}

VKAPI_ATTR VkResult VKAPI_CALL fake_vkCreateDescriptorPool(
    VkDevice device, const VkDescriptorPoolCreateInfo*, const VkAllocationCallbacks*,
    VkDescriptorPool* pDescriptorPool) {
    FakeVulkanState* state = fake_state(device);
    FakeDescriptorPoolToken* descriptorPool = allocate_fake_descriptor_pool(state);
    if (!descriptorPool || !pDescriptorPool)
        return VK_ERROR_INITIALIZATION_FAILED;

    descriptorPool->alive = true;
    ++state->descriptorPoolCreateCalls;
    *pDescriptorPool = reinterpret_cast<VkDescriptorPool>(descriptorPool);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL fake_vkDestroyDescriptorPool(
    VkDevice device, VkDescriptorPool descriptorPool, const VkAllocationCallbacks*) {
    FakeVulkanState* state = fake_state(device);
    if (!state || descriptorPool == VK_NULL_HANDLE)
        return;

    FakeDescriptorPoolToken* token = reinterpret_cast<FakeDescriptorPoolToken*>(descriptorPool);
    if (!token->alive) {
        state->callbackError = true;
        return;
    }
    token->alive = false;
    ++state->descriptorPoolDestroyCalls;
}

VkResult fake_vkCreateBufferCommon(VkDevice device, const VkBufferCreateInfo* pCreateInfo,
                                   VkBuffer* pBuffer, bool directCall) {
    FakeVulkanState* state = fake_state(device);
    FakeBufferToken* buffer = allocate_fake_buffer(state);
    if (!buffer || !pCreateInfo || !pBuffer)
        return VK_ERROR_INITIALIZATION_FAILED;

    buffer->alive = true;
    buffer->size = pCreateInfo->size;
    buffer->boundMemory = VK_NULL_HANDLE;
    if (directCall)
        ++state->directBufferCreateCalls;
    else
        ++state->deviceBufferCreateCalls;
    *pBuffer = reinterpret_cast<VkBuffer>(buffer);
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL fake_vkCreateBufferDevice(
    VkDevice device, const VkBufferCreateInfo* pCreateInfo, const VkAllocationCallbacks*,
    VkBuffer* pBuffer) {
    return fake_vkCreateBufferCommon(device, pCreateInfo, pBuffer, false);
}

VKAPI_ATTR VkResult VKAPI_CALL fake_vkCreateBufferDirect(
    VkDevice device, const VkBufferCreateInfo* pCreateInfo, const VkAllocationCallbacks*,
    VkBuffer* pBuffer) {
    return fake_vkCreateBufferCommon(device, pCreateInfo, pBuffer, true);
}

VKAPI_ATTR void VKAPI_CALL fake_vkGetBufferMemoryRequirements(
    VkDevice device, VkBuffer buffer, VkMemoryRequirements* pMemoryRequirements) {
    FakeVulkanState* state = fake_state(device);
    if (!state || buffer == VK_NULL_HANDLE || !pMemoryRequirements) {
        if (state)
            state->callbackError = true;
        return;
    }

    FakeBufferToken* token = reinterpret_cast<FakeBufferToken*>(buffer);
    if (!token->alive) {
        state->callbackError = true;
        return;
    }
    pMemoryRequirements->size = token->size < 256 ? 256 : token->size;
    pMemoryRequirements->alignment = 256;
    pMemoryRequirements->memoryTypeBits = 0x3u;
}

VKAPI_ATTR VkResult VKAPI_CALL fake_vkAllocateMemory(
    VkDevice device, const VkMemoryAllocateInfo* pAllocateInfo, const VkAllocationCallbacks*,
    VkDeviceMemory* pMemory) {
    FakeVulkanState* state = fake_state(device);
    FakeMemoryToken* memory = allocate_fake_memory(state);
    if (!memory || !pAllocateInfo || !pMemory)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (pAllocateInfo->allocationSize > std::numeric_limits<size_t>::max())
        return VK_ERROR_OUT_OF_HOST_MEMORY;

    try {
        memory->storage.resize(static_cast<size_t>(pAllocateInfo->allocationSize));
    } catch (...) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    memory->alive = true;
    memory->mapped = false;
    ++state->allocateMemoryCalls;
    *pMemory = reinterpret_cast<VkDeviceMemory>(memory);
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL fake_vkMapMemory(
    VkDevice device, VkDeviceMemory memory, VkDeviceSize offset, VkDeviceSize size,
    VkMemoryMapFlags, void** ppData) {
    FakeVulkanState* state = fake_state(device);
    if (!state || memory == VK_NULL_HANDLE || !ppData) {
        if (state)
            state->callbackError = true;
        return VK_ERROR_MEMORY_MAP_FAILED;
    }

    FakeMemoryToken* token = reinterpret_cast<FakeMemoryToken*>(memory);
    const VkDeviceSize storageSize = static_cast<VkDeviceSize>(token->storage.size());
    const VkDeviceSize mappedSize = size == VK_WHOLE_SIZE ? storageSize - offset : size;
    if (!token->alive || token->mapped || offset > storageSize || mappedSize > storageSize - offset) {
        state->callbackError = true;
        return VK_ERROR_MEMORY_MAP_FAILED;
    }
    token->mapped = true;
    ++state->mapMemoryCalls;
    *ppData = token->storage.data() + static_cast<size_t>(offset);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL fake_vkUnmapMemory(VkDevice device, VkDeviceMemory memory) {
    FakeVulkanState* state = fake_state(device);
    if (!state || memory == VK_NULL_HANDLE)
        return;

    FakeMemoryToken* token = reinterpret_cast<FakeMemoryToken*>(memory);
    if (!token->alive || !token->mapped) {
        state->callbackError = true;
        return;
    }
    token->mapped = false;
    ++state->unmapMemoryCalls;
}

VKAPI_ATTR VkResult VKAPI_CALL fake_vkBindBufferMemory(
    VkDevice device, VkBuffer buffer, VkDeviceMemory memory, VkDeviceSize) {
    FakeVulkanState* state = fake_state(device);
    if (!state || buffer == VK_NULL_HANDLE || memory == VK_NULL_HANDLE)
        return VK_ERROR_INITIALIZATION_FAILED;

    FakeBufferToken* bufferToken = reinterpret_cast<FakeBufferToken*>(buffer);
    FakeMemoryToken* memoryToken = reinterpret_cast<FakeMemoryToken*>(memory);
    if (!bufferToken->alive || !memoryToken->alive) {
        state->callbackError = true;
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    bufferToken->boundMemory = memory;
    ++state->bindBufferMemoryCalls;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL fake_vkDestroyBuffer(
    VkDevice device, VkBuffer buffer, const VkAllocationCallbacks*) {
    FakeVulkanState* state = fake_state(device);
    if (!state || buffer == VK_NULL_HANDLE)
        return;

    FakeBufferToken* token = reinterpret_cast<FakeBufferToken*>(buffer);
    if (!token->alive) {
        state->callbackError = true;
        return;
    }
    token->alive = false;
    ++state->bufferDestroyCalls;
}

VKAPI_ATTR void VKAPI_CALL fake_vkFreeMemory(
    VkDevice device, VkDeviceMemory memory, const VkAllocationCallbacks*) {
    FakeVulkanState* state = fake_state(device);
    if (!state || memory == VK_NULL_HANDLE)
        return;

    FakeMemoryToken* token = reinterpret_cast<FakeMemoryToken*>(memory);
    if (!token->alive || token->mapped) {
        state->callbackError = true;
        return;
    }
    token->alive = false;
    token->storage.clear();
    ++state->freeMemoryCalls;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL fake_vkGetDeviceProcAddr(
    VkDevice device, const char* pName) {
    FakeVulkanState* state = fake_state(device);
    if (state)
        ++state->deviceProcAddrCalls;
    if (!pName)
        return nullptr;

    if (std::strcmp(pName, "vkCreateDescriptorPool") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(fake_vkCreateDescriptorPool);
    if (std::strcmp(pName, "vkCreateBuffer") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(fake_vkCreateBufferDevice);
    if (std::strcmp(pName, "vkGetBufferMemoryRequirements") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(fake_vkGetBufferMemoryRequirements);
    if (std::strcmp(pName, "vkAllocateMemory") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(fake_vkAllocateMemory);
    if (std::strcmp(pName, "vkMapMemory") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(fake_vkMapMemory);
    if (std::strcmp(pName, "vkBindBufferMemory") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(fake_vkBindBufferMemory);
    if (std::strcmp(pName, "vkDestroyBuffer") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(fake_vkDestroyBuffer);
    if (std::strcmp(pName, "vkUnmapMemory") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(fake_vkUnmapMemory);
    if (std::strcmp(pName, "vkFreeMemory") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(fake_vkFreeMemory);
    if (std::strcmp(pName, "vkDestroyDescriptorPool") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(fake_vkDestroyDescriptorPool);
    return nullptr;
}

void set_fake_memory_properties(std::initializer_list<VkMemoryPropertyFlags> propertyFlags) {
    g_fakeMemory = {};
    g_fakeMemory.properties.memoryTypeCount = static_cast<uint32_t>(propertyFlags.size());
    g_fakeMemory.properties.memoryHeapCount = propertyFlags.size() == 0 ? 0 : 1;
    if (propertyFlags.size() != 0)
        g_fakeMemory.properties.memoryHeaps[0].size = 1;

    uint32_t memoryTypeIndex = 0;
    for (const VkMemoryPropertyFlags flags : propertyFlags) {
        g_fakeMemory.properties.memoryTypes[memoryTypeIndex].heapIndex = 0;
        g_fakeMemory.properties.memoryTypes[memoryTypeIndex].propertyFlags = flags;
        ++memoryTypeIndex;
    }
}

void reset_fake(uint32_t extensionCount) {
    g_fakeEnumeration = {};
    g_fakeEnumeration.extensionCount = extensionCount;
}

bool check_u32(const char* label, uint32_t observed, uint32_t expected) {
    if (observed != expected) {
        std::fprintf(stderr, "FAIL: %s: observed %u expected %u\n",
                     label, observed, expected);
        return false;
    }
    return true;
}

bool check_size_nonzero(const char* label, size_t observed) {
    if (observed == 0) {
        std::fprintf(stderr, "FAIL: %s: observed 0 expected nonzero\n", label);
        return false;
    }
    return true;
}

bool check_size_changed(const char* label, size_t observed, size_t previous) {
    if (observed == previous) {
        std::fprintf(stderr, "FAIL: %s: observed %zu expected a value different from %zu\n",
                     label, observed, previous);
        return false;
    }
    return true;
}

bool check_physical_device(const char* label, VkPhysicalDevice observed,
                           VkPhysicalDevice expected) {
    if (observed != expected) {
        std::fprintf(stderr, "FAIL: %s: observed %p expected %p\n", label,
                     reinterpret_cast<const void*>(observed),
                     reinterpret_cast<const void*>(expected));
        return false;
    }
    return true;
}

bool check_null_layer_name(const char* label, const char* observed) {
    if (observed != nullptr) {
        std::fprintf(stderr, "FAIL: %s: observed %p expected nullptr\n", label,
                     static_cast<const void*>(observed));
        return false;
    }
    return true;
}

bool check_error_code(const char* label, FfxErrorCode observed, FfxErrorCode expected) {
    if (observed != expected) {
        std::fprintf(stderr, "FAIL: %s: observed %d expected %d\n", label,
                     static_cast<int>(observed), static_cast<int>(expected));
        return false;
    }
    return true;
}

template <typename T>
bool check_non_null(const char* label, T pointer) {
    if (pointer == nullptr) {
        std::fprintf(stderr, "FAIL: %s: observed nullptr expected non-null\n", label);
        return false;
    }
    return true;
}

bool check_guard_bytes(const char* label, const uint8_t* bytes, size_t size, uint8_t expected) {
    for (size_t i = 0; i < size; ++i) {
        if (bytes[i] != expected) {
            std::fprintf(stderr, "FAIL: %s: guard byte %zu was %u expected %u\n",
                         label, i, static_cast<unsigned>(bytes[i]),
                         static_cast<unsigned>(expected));
            return false;
        }
    }
    return true;
}

struct VolkPointerRestore {
    PFN_vkEnumerateDeviceExtensionProperties previousEnumeration;
    PFN_vkGetPhysicalDeviceMemoryProperties previousMemoryProperties;
    PFN_vkGetPhysicalDeviceProperties previousProperties;
    PFN_vkCreateBuffer previousCreateBuffer;

    ~VolkPointerRestore() {
        vkEnumerateDeviceExtensionProperties = previousEnumeration;
        vkGetPhysicalDeviceMemoryProperties = previousMemoryProperties;
        vkGetPhysicalDeviceProperties = previousProperties;
        vkCreateBuffer = previousCreateBuffer;
    }
};

struct BackendContextCaseSummary {
    size_t scratchSize = 0;
    size_t baseOffset = 0;
    uintptr_t baseAddressMod32 = 0;
};

bool check_fake_lifecycle(const char* label, const FakeVulkanState& state,
                          uint32_t expectedInitializations) {
    bool passed = true;
    if (state.callbackError) {
        std::fprintf(stderr, "FAIL: %s: fake Vulkan callback lifecycle error\n", label);
        passed = false;
    }
    if (state.descriptorPoolCreateCalls != expectedInitializations) {
        std::fprintf(stderr, "FAIL: %s: descriptor-pool creates %u expected %u\n", label,
                     state.descriptorPoolCreateCalls, expectedInitializations);
        passed = false;
    }
    if (state.descriptorPoolDestroyCalls != expectedInitializations) {
        std::fprintf(stderr, "FAIL: %s: descriptor-pool destroys %u expected %u\n", label,
                     state.descriptorPoolDestroyCalls, expectedInitializations);
        passed = false;
    }
    if (state.deviceBufferCreateCalls != expectedInitializations ||
        state.directBufferCreateCalls != expectedInitializations) {
        std::fprintf(stderr, "FAIL: %s: buffer creates device/direct %u/%u expected %u/%u\n",
                     label, state.deviceBufferCreateCalls, state.directBufferCreateCalls,
                     expectedInitializations, expectedInitializations);
        passed = false;
    }
    if (state.bufferDestroyCalls != expectedInitializations * 2) {
        std::fprintf(stderr, "FAIL: %s: buffer destroys %u expected %u\n", label,
                     state.bufferDestroyCalls, expectedInitializations * 2);
        passed = false;
    }
    if (state.allocateMemoryCalls != expectedInitializations ||
        state.freeMemoryCalls != expectedInitializations ||
        state.mapMemoryCalls != expectedInitializations ||
        state.unmapMemoryCalls != expectedInitializations ||
        state.bindBufferMemoryCalls != expectedInitializations) {
        std::fprintf(stderr, "FAIL: %s: memory calls alloc/free/map/unmap/bind %u/%u/%u/%u/%u expected %u each\n",
                     label, state.allocateMemoryCalls, state.freeMemoryCalls,
                     state.mapMemoryCalls, state.unmapMemoryCalls,
                     state.bindBufferMemoryCalls, expectedInitializations);
        passed = false;
    }
    return passed;
}

BackendContextCaseSummary run_backend_context_case(
    const char* label, VkPhysicalDevice physicalDevice, VkDeviceContext* deviceContext,
    uint32_t extensionCount, size_t maxContexts, uintptr_t requestedBaseAddressMod32,
    int& failures) {
    constexpr size_t kGuardBytes = 64;
    constexpr uint8_t kGuardByte = 0xa5;

    reset_fake(extensionCount);
    const size_t scratchSize = ffxGetScratchMemorySizeVK(physicalDevice, maxContexts);
    BackendContextCaseSummary summary = {scratchSize, 0, 0};
    if (!check_size_nonzero(label, scratchSize)) {
        ++failures;
        return summary;
    }
    if (!check_u32("context scratch-size extension enumeration call count",
                   g_fakeEnumeration.callCount, 1))
        ++failures;

    const size_t rawStorageSize = scratchSize + (kGuardBytes * 2) + 32;
    std::vector<uint8_t> rawStorage(rawStorageSize, kGuardByte);
    const uintptr_t rawAddress = reinterpret_cast<uintptr_t>(rawStorage.data());
    if ((rawAddress % alignof(std::max_align_t)) != 0) {
        std::fprintf(stderr, "FAIL: %s: raw byte storage is not ordinarily aligned\n", label);
        ++failures;
        return summary;
    }

    const uintptr_t guardAddress = rawAddress + kGuardBytes;
    const size_t addressDelta = static_cast<size_t>(
        (requestedBaseAddressMod32 + 32 - (guardAddress % 32)) % 32);
    const size_t baseOffset = kGuardBytes + addressDelta;
    uint8_t* scratch = rawStorage.data() + baseOffset;
    const uintptr_t scratchAddress = reinterpret_cast<uintptr_t>(scratch);
    summary.baseOffset = baseOffset;
    summary.baseAddressMod32 = scratchAddress % 32;

    if ((scratchAddress % alignof(std::max_align_t)) != 0) {
        std::fprintf(stderr, "FAIL: %s: scratch base is not ordinarily aligned\n", label);
        ++failures;
    }
    if (!check_u32("scratch base address modulo 32",
                   static_cast<uint32_t>(summary.baseAddressMod32),
                   static_cast<uint32_t>(requestedBaseAddressMod32)))
        ++failures;
    std::memset(scratch, 0, scratchSize);

    g_fakeVulkan.reset();
    g_fakeProperties.callCount = 0;
    g_fakeProperties.lastPhysicalDevice = VK_NULL_HANDLE;
    set_fake_memory_properties({VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT});
    reset_fake(extensionCount);

    const FfxDevice device = ffxGetDeviceVK(deviceContext);
    if (!check_non_null("context ffxGetDeviceVK result", device)) {
        ++failures;
        return summary;
    }

    FfxInterface backendInterface = {};
    const FfxErrorCode interfaceResult = ffxGetInterfaceVK(
        &backendInterface, device, scratch, scratchSize, maxContexts);
    if (!check_error_code("context ffxGetInterfaceVK result", interfaceResult, FFX_OK)) {
        ++failures;
        return summary;
    }
    if (!check_u32("context interface scratch-size enumeration call count",
                   g_fakeEnumeration.callCount, 1))
        ++failures;

    reset_fake(extensionCount);
    FfxUInt32 firstContextId = UINT32_MAX;
    const FfxErrorCode firstCreateResult = backendInterface.fpCreateBackendContext(
        &backendInterface, FFX_EFFECT_FSR3UPSCALER, nullptr, &firstContextId);
    const bool firstCreatePassed = check_error_code(
        "context first create result", firstCreateResult, FFX_OK);
    if (!firstCreatePassed)
        ++failures;
    if (!check_u32("context create extension enumeration call count",
                   g_fakeEnumeration.callCount, 2))
        ++failures;
    if (!check_u32("context non-null extension enumeration call count",
                   g_fakeEnumeration.nonNullPropertiesCallCount, 1))
        ++failures;
    if (!check_u32("context populated extension entry count",
                   g_fakeEnumeration.populatedEntryCount, extensionCount))
        ++failures;
    if (!check_u32("context first effect id", firstContextId, 0))
        ++failures;
    if (!check_guard_bytes("guard after first create", scratch - kGuardBytes,
                           kGuardBytes, kGuardByte) ||
        !check_guard_bytes("trailing guard after first create", scratch + scratchSize,
                           kGuardBytes, kGuardByte))
        ++failures;
    if (!firstCreatePassed)
        return summary;

    const uint32_t expectedInitializations = maxContexts == 1 ? 2 : 1;
    if (maxContexts > 1) {
        FfxUInt32 secondContextId = UINT32_MAX;
        const FfxErrorCode secondCreateResult = backendInterface.fpCreateBackendContext(
            &backendInterface, FFX_EFFECT_FSR3UPSCALER, nullptr, &secondContextId);
        if (!check_error_code("context second create result", secondCreateResult, FFX_OK)) {
            ++failures;
            return summary;
        }
        if (secondContextId == firstContextId) {
            std::fprintf(stderr, "FAIL: context second effect id reused active id %u\n",
                         firstContextId);
            ++failures;
        }
        if (!check_error_code("context first destroy before reuse",
                              backendInterface.fpDestroyBackendContext(
                                  &backendInterface, firstContextId), FFX_OK))
            ++failures;

        FfxUInt32 reusedContextId = UINT32_MAX;
        const FfxErrorCode reusedCreateResult = backendInterface.fpCreateBackendContext(
            &backendInterface, FFX_EFFECT_FSR3UPSCALER, nullptr, &reusedContextId);
        if (!check_error_code("context reused create result", reusedCreateResult, FFX_OK)) {
            ++failures;
            return summary;
        }
        if (!check_u32("context reused effect id", reusedContextId, firstContextId))
            ++failures;
        if (!check_error_code("context reused destroy result",
                              backendInterface.fpDestroyBackendContext(
                                  &backendInterface, reusedContextId), FFX_OK))
            ++failures;
        if (!check_error_code("context second destroy result",
                              backendInterface.fpDestroyBackendContext(
                                  &backendInterface, secondContextId), FFX_OK))
            ++failures;
    } else {
        if (!check_error_code("context first destroy result",
                              backendInterface.fpDestroyBackendContext(
                                  &backendInterface, firstContextId), FFX_OK))
            ++failures;

        FfxUInt32 reusedContextId = UINT32_MAX;
        const FfxErrorCode reusedCreateResult = backendInterface.fpCreateBackendContext(
            &backendInterface, FFX_EFFECT_FSR3UPSCALER, nullptr, &reusedContextId);
        if (!check_error_code("context reused create result", reusedCreateResult, FFX_OK)) {
            ++failures;
            return summary;
        }
        if (!check_u32("context reused effect id", reusedContextId, firstContextId))
            ++failures;
        if (!check_error_code("context reused destroy result",
                              backendInterface.fpDestroyBackendContext(
                                  &backendInterface, reusedContextId), FFX_OK))
            ++failures;
    }

    if (!check_guard_bytes("guard after destroy", scratch - kGuardBytes,
                           kGuardBytes, kGuardByte) ||
        !check_guard_bytes("trailing guard after destroy", scratch + scratchSize,
                           kGuardBytes, kGuardByte))
        ++failures;
    if (!check_fake_lifecycle(label, g_fakeVulkan, expectedInitializations))
        ++failures;
    if (!check_u32("context physical-properties call count", g_fakeProperties.callCount,
                   expectedInitializations))
        ++failures;
    if (!check_physical_device("context physical-properties physical device",
                               g_fakeProperties.lastPhysicalDevice, physicalDevice))
        ++failures;
    if (!check_u32("context memory-properties call count", g_fakeMemory.callCount,
                   expectedInitializations * 2))
        ++failures;
    if (!check_physical_device("context memory-properties physical device",
                               g_fakeMemory.lastPhysicalDevice, physicalDevice))
        ++failures;

    return summary;
}

} // namespace

int main() {
    const PFN_vkEnumerateDeviceExtensionProperties previousEnumerationFunction =
        vkEnumerateDeviceExtensionProperties;
    const PFN_vkGetPhysicalDeviceMemoryProperties previousMemoryPropertiesFunction =
        vkGetPhysicalDeviceMemoryProperties;
    const PFN_vkGetPhysicalDeviceProperties previousPropertiesFunction =
        vkGetPhysicalDeviceProperties;
    const PFN_vkCreateBuffer previousCreateBufferFunction = vkCreateBuffer;
    vkEnumerateDeviceExtensionProperties = fake_vkEnumerateDeviceExtensionProperties;
    vkGetPhysicalDeviceMemoryProperties = fake_vkGetPhysicalDeviceMemoryProperties;
    vkGetPhysicalDeviceProperties = fake_vkGetPhysicalDeviceProperties;
    vkCreateBuffer = fake_vkCreateBufferDirect;
    // Restore both global Volk pointers even if a check fails or the process exits early.
    VolkPointerRestore restore{previousEnumerationFunction, previousMemoryPropertiesFunction,
                               previousPropertiesFunction, previousCreateBufferFunction};

    int failures = 0;
    FakePhysicalDeviceToken physicalDeviceToken;
    const VkPhysicalDevice fakePhysicalDevice =
        reinterpret_cast<VkPhysicalDevice>(&physicalDeviceToken);
    g_fakeProperties.properties = {};
    g_fakeProperties.properties.limits.minUniformBufferOffsetAlignment = 256;

    constexpr uint32_t kFirstExtensionCount = 0;
    reset_fake(kFirstExtensionCount);
    const size_t firstScratchSize = ffxGetScratchMemorySizeVK(fakePhysicalDevice, 1);
    if (!check_u32("fake enumeration call count", g_fakeEnumeration.callCount, 1))
        ++failures;
    if (!check_physical_device("fake enumeration physical device",
                               g_fakeEnumeration.lastPhysicalDevice, fakePhysicalDevice))
        ++failures;
    if (!check_null_layer_name("fake enumeration layer name", g_fakeEnumeration.lastLayerName))
        ++failures;
    if (!check_size_nonzero("scratch size for zero extensions", firstScratchSize))
        ++failures;

    constexpr uint32_t kSecondExtensionCount = 7;
    reset_fake(kSecondExtensionCount);
    const size_t secondScratchSize = ffxGetScratchMemorySizeVK(fakePhysicalDevice, 1);
    if (!check_u32("fake enumeration call count", g_fakeEnumeration.callCount, 1))
        ++failures;
    if (!check_physical_device("fake enumeration physical device",
                               g_fakeEnumeration.lastPhysicalDevice, fakePhysicalDevice))
        ++failures;
    if (!check_null_layer_name("fake enumeration layer name", g_fakeEnumeration.lastLayerName))
        ++failures;
    if (!check_size_nonzero("scratch size for seven extensions", secondScratchSize))
        ++failures;
    if (!check_size_changed("scratch size after changing extension count",
                            secondScratchSize, firstScratchSize))
        ++failures;

    reset_fake(kSecondExtensionCount);
    const size_t nullPhysicalDeviceScratchSize =
        ffxGetScratchMemorySizeVK(VK_NULL_HANDLE, 1);
    if (!check_u32("null physical device enumeration call count",
                   g_fakeEnumeration.callCount, 0))
        ++failures;
    if (!check_size_nonzero("scratch size for null physical device", nullPhysicalDeviceScratchSize))
        ++failures;

    FakeDeviceToken deviceToken;
    VkDeviceContext deviceContext = {};
    deviceContext.vkDevice = reinterpret_cast<VkDevice>(&deviceToken);
    deviceContext.vkPhysicalDevice = fakePhysicalDevice;
    deviceContext.vkDeviceProcAddr = fake_vkGetDeviceProcAddr;
    deviceToken.state = &g_fakeVulkan;

    if (firstScratchSize == 0) {
        std::fprintf(stderr, "FAIL: insufficient-scratch case has no advertised size\n");
        ++failures;
    } else {
        std::vector<uint8_t> insufficientScratch(firstScratchSize, 0);
        reset_fake(kFirstExtensionCount);
        FfxInterface rejectedInterface = {};
        const FfxDevice device = ffxGetDeviceVK(&deviceContext);
        const FfxErrorCode insufficientResult = ffxGetInterfaceVK(
            &rejectedInterface, device, insufficientScratch.data(), firstScratchSize - 1, 1);
        if (!check_error_code("insufficient scratch rejection", insufficientResult,
                              FFX_ERROR_INSUFFICIENT_MEMORY))
            ++failures;
        if (!check_u32("insufficient scratch enumeration call count",
                       g_fakeEnumeration.callCount, 1))
            ++failures;
    }

    const size_t scratchAlignment = alignof(std::max_align_t);
    if (secondScratchSize > std::numeric_limits<size_t>::max() - (scratchAlignment - 1)) {
        std::fprintf(stderr, "FAIL: scratch size overflow while calculating alignment\n");
        ++failures;
    } else {
        const size_t scratchUnits =
            (secondScratchSize + scratchAlignment - 1) / scratchAlignment;
        std::vector<std::max_align_t> scratch(scratchUnits, std::max_align_t{});
        std::memset(scratch.data(), 0, scratch.size() * sizeof(std::max_align_t));

        reset_fake(kSecondExtensionCount);
        const FfxDevice device = ffxGetDeviceVK(&deviceContext);
        if (!check_non_null("ffxGetDeviceVK result", device))
            ++failures;

        FfxInterface backendInterface = {};
        const FfxErrorCode interfaceResult = ffxGetInterfaceVK(
            &backendInterface, device, scratch.data(), secondScratchSize, 1);
        if (!check_error_code("ffxGetInterfaceVK result", interfaceResult, FFX_OK))
            ++failures;
        if (!check_u32("interface scratch-size enumeration call count",
                       g_fakeEnumeration.callCount, 1))
            ++failures;
        if (!check_physical_device("interface scratch-size enumeration physical device",
                                   g_fakeEnumeration.lastPhysicalDevice, fakePhysicalDevice))
            ++failures;
        if (!check_null_layer_name("interface scratch-size enumeration layer name",
                                   g_fakeEnumeration.lastLayerName))
            ++failures;
        if (!check_non_null("ffxGetInterfaceVK fpCreateBackendContext",
                            backendInterface.fpCreateBackendContext))
            ++failures;
        if (!check_non_null("ffxGetInterfaceVK fpGetDeviceCapabilities",
                            backendInterface.fpGetDeviceCapabilities))
            ++failures;
        if (!check_non_null("ffxGetInterfaceVK fpDestroyBackendContext",
                            backendInterface.fpDestroyBackendContext))
            ++failures;
        if (!check_non_null("ffxGetInterfaceVK fpCreateResource",
                            backendInterface.fpCreateResource))
            ++failures;
        if (!check_non_null("ffxGetInterfaceVK fpScheduleGpuJob",
                            backendInterface.fpScheduleGpuJob))
            ++failures;
    }

    const BackendContextCaseSummary zeroExtensionCase = run_backend_context_case(
        "zero-extension max-one context case", fakePhysicalDevice, &deviceContext,
        kFirstExtensionCount, 1, 16, failures);
    const BackendContextCaseSummary multipleContextCase = run_backend_context_case(
        "nonzero-extension multiple-context case", fakePhysicalDevice, &deviceContext,
        kSecondExtensionCount, 2, 0, failures);
    if (zeroExtensionCase.baseAddressMod32 == multipleContextCase.baseAddressMod32) {
        std::fprintf(stderr, "FAIL: context cases used the same base-address modulo 32\n");
        ++failures;
    }

    VkMemoryRequirements memoryRequirements = {};
    memoryRequirements.memoryTypeBits = UINT32_MAX;

    set_fake_memory_properties({VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, 0, 0, 0, 0, 0,
                                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
                                    VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD});
    VkMemoryPropertyFlags outProperties = 0;
    uint32_t memoryTypeIndex = findMemoryTypeIndex(
        fakePhysicalDevice, memoryRequirements, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, outProperties);
    if (!check_u32("AMD coherent device-local candidate selects legal low index",
                   memoryTypeIndex, 0))
        ++failures;
    if (!check_u32("AMD coherent device-local candidate preserves exact flags",
                   outProperties, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
        ++failures;
    if (!check_u32("AMD coherent case memory-properties call count", g_fakeMemory.callCount, 1))
        ++failures;
    if (!check_physical_device("AMD coherent case physical device",
                               g_fakeMemory.lastPhysicalDevice, fakePhysicalDevice))
        ++failures;

    set_fake_memory_properties({VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, 0, 0, 0, 0, 0,
                                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
                                    VK_MEMORY_PROPERTY_DEVICE_UNCACHED_BIT_AMD});
    outProperties = 0;
    memoryTypeIndex = findMemoryTypeIndex(
        fakePhysicalDevice, memoryRequirements, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, outProperties);
    if (!check_u32("AMD uncached device-local candidate selects legal low index",
                   memoryTypeIndex, 0))
        ++failures;
    if (!check_u32("AMD uncached device-local candidate preserves exact flags",
                   outProperties, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
        ++failures;
    if (!check_u32("AMD uncached case memory-properties call count", g_fakeMemory.callCount, 1))
        ++failures;
    if (!check_physical_device("AMD uncached case physical device",
                               g_fakeMemory.lastPhysicalDevice, fakePhysicalDevice))
        ++failures;

    set_fake_memory_properties({VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT});
    memoryRequirements.memoryTypeBits = 0x3u;
    outProperties = 0;
    memoryTypeIndex = findMemoryTypeIndex(
        fakePhysicalDevice, memoryRequirements,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, outProperties);
    if (!check_u32("combined host-visible/device-local request rejects partial early match",
                   memoryTypeIndex, 1))
        ++failures;
    if (!check_u32("combined host-visible/device-local request exact flags",
                   outProperties,
                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
        ++failures;
    if (!check_u32("combined request memory-properties call count", g_fakeMemory.callCount, 1))
        ++failures;
    if (!check_physical_device("combined request physical device",
                               g_fakeMemory.lastPhysicalDevice, fakePhysicalDevice))
        ++failures;

    set_fake_memory_properties({VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
                                    VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD});
    memoryRequirements.memoryTypeBits = 0x1u;
    outProperties = 0xdeadbeefu;
    memoryTypeIndex = findMemoryTypeIndex(
        fakePhysicalDevice, memoryRequirements, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, outProperties);
    if (!check_u32("only AMD coherent device-local candidates return no match",
                   memoryTypeIndex, UINT32_MAX))
        ++failures;
    if (!check_u32("only AMD coherent candidates clear outProperties", outProperties, 0))
        ++failures;
    if (!check_u32("only AMD coherent case memory-properties call count", g_fakeMemory.callCount, 1))
        ++failures;
    if (!check_physical_device("only AMD coherent case physical device",
                               g_fakeMemory.lastPhysicalDevice, fakePhysicalDevice))
        ++failures;

    set_fake_memory_properties({VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
                                    VK_MEMORY_PROPERTY_DEVICE_UNCACHED_BIT_AMD});
    outProperties = 0xdeadbeefu;
    memoryTypeIndex = findMemoryTypeIndex(
        fakePhysicalDevice, memoryRequirements, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, outProperties);
    if (!check_u32("only AMD uncached device-local candidates return no match",
                   memoryTypeIndex, UINT32_MAX))
        ++failures;
    if (!check_u32("only AMD uncached candidates clear outProperties", outProperties, 0))
        ++failures;
    if (!check_u32("only AMD uncached case memory-properties call count", g_fakeMemory.callCount, 1))
        ++failures;
    if (!check_physical_device("only AMD uncached case physical device",
                               g_fakeMemory.lastPhysicalDevice, fakePhysicalDevice))
        ++failures;

    set_fake_memory_properties({VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0});
    memoryRequirements.memoryTypeBits = 0x2u;
    outProperties = 0xdeadbeefu;
    memoryTypeIndex = findMemoryTypeIndex(
        fakePhysicalDevice, memoryRequirements, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, outProperties);
    if (!check_u32("memoryTypeBits exclusion returns no match", memoryTypeIndex, UINT32_MAX))
        ++failures;
    if (!check_u32("memoryTypeBits exclusion clears outProperties", outProperties, 0))
        ++failures;
    if (!check_u32("memoryTypeBits exclusion memory-properties call count",
                   g_fakeMemory.callCount, 1))
        ++failures;
    if (!check_physical_device("memoryTypeBits exclusion physical device",
                               g_fakeMemory.lastPhysicalDevice, fakePhysicalDevice))
        ++failures;

    std::vector<VkMemoryPropertyFlags> highIndexMemoryTypes(32, 0);
    highIndexMemoryTypes[31] = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    set_fake_memory_properties({});
    g_fakeMemory.properties.memoryTypeCount = static_cast<uint32_t>(highIndexMemoryTypes.size());
    g_fakeMemory.properties.memoryHeapCount = 1;
    g_fakeMemory.properties.memoryHeaps[0].size = 1;
    for (uint32_t i = 0; i < g_fakeMemory.properties.memoryTypeCount; ++i) {
        g_fakeMemory.properties.memoryTypes[i].heapIndex = 0;
        g_fakeMemory.properties.memoryTypes[i].propertyFlags = highIndexMemoryTypes[i];
    }
    memoryRequirements.memoryTypeBits = 1u << 31;
    outProperties = 0;
    memoryTypeIndex = findMemoryTypeIndex(
        fakePhysicalDevice, memoryRequirements, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, outProperties);
    if (!check_u32("memory type index 31 is selected", memoryTypeIndex, 31))
        ++failures;
    if (!check_u32("memory type index 31 exact flags",
                   outProperties, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
        ++failures;
    if (!check_u32("memory type index 31 memory-properties call count",
                   g_fakeMemory.callCount, 1))
        ++failures;
    if (!check_physical_device("memory type index 31 physical device",
                               g_fakeMemory.lastPhysicalDevice, fakePhysicalDevice))
        ++failures;

    set_fake_memory_properties({VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT});
    memoryRequirements.memoryTypeBits = 0x3u;
    outProperties = 0xdeadbeefu;
    memoryTypeIndex = findMemoryTypeIndex(
        fakePhysicalDevice, memoryRequirements,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, outProperties);
    if (!check_u32("missing combined host-visible/device-local type returns no match",
                   memoryTypeIndex, UINT32_MAX))
        ++failures;
    if (!check_u32("missing combined type clears outProperties", outProperties, 0))
        ++failures;
    if (!check_u32("missing combined type memory-properties call count",
                   g_fakeMemory.callCount, 1))
        ++failures;
    if (!check_physical_device("missing combined type physical device",
                               g_fakeMemory.lastPhysicalDevice, fakePhysicalDevice))
        ++failures;

    set_fake_memory_properties({VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT});
    outProperties = 0xdeadbeefu;
    memoryTypeIndex = findMemoryTypeIndex(
        fakePhysicalDevice, memoryRequirements, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, outProperties);
    if (!check_u32("host-visible fallback selects host-visible-only type", memoryTypeIndex, 0))
        ++failures;
    if (!check_u32("host-visible fallback exact flags", outProperties,
                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
        ++failures;
    if (!check_u32("host-visible fallback memory-properties call count",
                   g_fakeMemory.callCount, 1))
        ++failures;
    if (!check_physical_device("host-visible fallback physical device",
                               g_fakeMemory.lastPhysicalDevice, fakePhysicalDevice))
        ++failures;

    set_fake_memory_properties({VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT});
    memoryRequirements.memoryTypeBits = 0x3u;
    outProperties = 0;
    memoryTypeIndex = findMemoryTypeIndex(
        fakePhysicalDevice, memoryRequirements, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, outProperties);
    if (!check_u32("pure device-local request prefers invisible-heap type", memoryTypeIndex, 1))
        ++failures;
    if (!check_u32("pure device-local preference exact flags",
                   outProperties, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
        ++failures;
    if (!check_u32("pure device-local preference memory-properties call count",
                   g_fakeMemory.callCount, 1))
        ++failures;
    if (!check_physical_device("pure device-local preference physical device",
                               g_fakeMemory.lastPhysicalDevice, fakePhysicalDevice))
        ++failures;

    set_fake_memory_properties({VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT});
    outProperties = 0;
    memoryTypeIndex = findMemoryTypeIndex(
        fakePhysicalDevice, memoryRequirements, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, outProperties);
    if (!check_u32("host-visible request prefers first coherent type", memoryTypeIndex, 0))
        ++failures;
    if (!check_u32("host-visible preference exact flags", outProperties,
                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
        ++failures;
    if (!check_u32("host-visible preference memory-properties call count",
                   g_fakeMemory.callCount, 1))
        ++failures;
    if (!check_physical_device("host-visible preference physical device",
                               g_fakeMemory.lastPhysicalDevice, fakePhysicalDevice))
        ++failures;

    if (failures != 0) {
        std::fprintf(stderr, "FSR Vulkan loader regression: %d failure(s)\n", failures);
        return 1;
    }

    std::printf("FSR Vulkan loader regression: passed (Volk fake calls=%u, scratch=%zu -> %zu bytes; "
                "ext0/max1=%zu@+%zu(mod32=%zu), ext7/max2=%zu@+%zu(mod32=%zu))\n",
                g_fakeEnumeration.callCount, firstScratchSize, secondScratchSize,
                zeroExtensionCase.scratchSize, zeroExtensionCase.baseOffset,
                zeroExtensionCase.baseAddressMod32, multipleContextCase.scratchSize,
                multipleContextCase.baseOffset, multipleContextCase.baseAddressMod32);
    return 0;
}
