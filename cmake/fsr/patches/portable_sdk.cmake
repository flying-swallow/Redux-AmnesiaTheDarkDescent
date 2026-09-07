# Anchor-checked edits applied only to the staged FidelityFX SDK sources.

function(ffx_sdk_replace_required file upstream_file old_text new_text description)
    if(NOT EXISTS "${file}")
        message(FATAL_ERROR "FidelityFX SDK patch target is missing for ${description}: ${file}")
    endif()
    if(NOT EXISTS "${upstream_file}")
        message(FATAL_ERROR "FidelityFX SDK upstream source is missing for ${description}: ${upstream_file}")
    endif()

    # Check the anchor against the untouched SDK source on every configure,
    # then apply the replacement to the current staged content. This keeps
    # multiple patches to one source cumulative, keeps the staged file
    # timestamp stable, and never silently accepts a changed upstream anchor.
    file(READ "${upstream_file}" contents)
    string(FIND "${contents}" "${old_text}" anchor_offset)
    if(anchor_offset EQUAL -1)
        message(FATAL_ERROR "FidelityFX SDK patch anchor was not found for ${description}: ${upstream_file}")
    endif()

    file(READ "${file}" existing_contents)
    string(FIND "${existing_contents}" "${old_text}" staged_anchor_offset)
    if(staged_anchor_offset EQUAL -1)
        # An already-applied replacement is valid on a later configure. If
        # neither side is present, fail instead of silently accepting drift.
        string(FIND "${existing_contents}" "${new_text}" applied_offset)
        if(NOT applied_offset EQUAL -1)
            return()
        endif()
        message(FATAL_ERROR "FidelityFX SDK staged patch anchor was not found for ${description}: ${file}")
    endif()
    string(REPLACE "${old_text}" "${new_text}" patched_contents "${existing_contents}")
    if(NOT "${existing_contents}" STREQUAL "${patched_contents}")
        file(WRITE "${file}" "${patched_contents}")
    endif()
endfunction()

# The SDK private context embeds twelve FfxPipelineState values.  On a
# non-MSVC host their public wchar_t arrays make that private object larger
# than the SDK's fixed 128 KiB opaque context buffer.  Keep the public Ffx ABI
# unchanged and store the private object behind that opaque buffer instead.
ffx_sdk_replace_required(
    "${FSR_STAGED_FSR3_SOURCE}"
    "${FSR_SDK_ROOT}/sdk/src/components/fsr3upscaler/ffx_fsr3upscaler.cpp"
    [=[#include "ffx_fsr3upscaler_private.h"

// lists to map shader resource bindpoint name to resource identifier]=]
    [=[#include "ffx_fsr3upscaler_private.h"
#include <stdlib.h>

static FfxFsr3UpscalerContext_Private* ffxFsr3UpscalerGetPrivateContext(FfxFsr3UpscalerContext* context)
{
    FfxFsr3UpscalerContext_Private* privateContext = nullptr;
    memcpy(&privateContext, context->data, sizeof(privateContext));
    return privateContext;
}

static void ffxFsr3UpscalerSetPrivateContext(FfxFsr3UpscalerContext* context, FfxFsr3UpscalerContext_Private* privateContext)
{
    memset(context->data, 0, sizeof(context->data));
    memcpy(context->data, &privateContext, sizeof(privateContext));
}

// lists to map shader resource bindpoint name to resource identifier]=]
    "keep the oversized private context behind the public opaque buffer")

ffx_sdk_replace_required(
    "${FSR_STAGED_FSR3_SOURCE}"
    "${FSR_SDK_ROOT}/sdk/src/components/fsr3upscaler/ffx_fsr3upscaler.cpp"
    [=[    // ensure the context is large enough for the internal context.
    FFX_STATIC_ASSERT(sizeof(FfxFsr3UpscalerContext) >= sizeof(FfxFsr3UpscalerContext_Private));]=]
    [=[    // The private object is allocated separately on non-MSVC hosts because
    // native wchar_t makes the embedded pipeline state arrays exceed the opaque
    // public context buffer.]=]
    "remove the invalid non-MSVC private-context size assertion")

ffx_sdk_replace_required(
    "${FSR_STAGED_FSR3_SOURCE}"
    "${FSR_SDK_ROOT}/sdk/src/components/fsr3upscaler/ffx_fsr3upscaler.cpp"
    [=[    // zero context memory
    memset(context, 0, sizeof(FfxFsr3UpscalerContext));

    // check pointers are valid.
    FFX_RETURN_ON_ERROR(
        context,
        FFX_ERROR_INVALID_POINTER);]=]
    [=[    // check pointers are valid before touching the public context.
    FFX_RETURN_ON_ERROR(
        context,
        FFX_ERROR_INVALID_POINTER);]=]
    "validate the public context before zeroing it")

ffx_sdk_replace_required(
    "${FSR_STAGED_FSR3_SOURCE}"
    "${FSR_SDK_ROOT}/sdk/src/components/fsr3upscaler/ffx_fsr3upscaler.cpp"
    [=[    // create the context.
    FfxFsr3UpscalerContext_Private* contextPrivate = (FfxFsr3UpscalerContext_Private*)(context);
    const FfxErrorCode errorCode = fsr3upscalerCreate(contextPrivate, contextDescription);

    return errorCode;]=]
    [=[    // Create the private implementation outside the public opaque buffer.
    FfxFsr3UpscalerContext_Private* contextPrivate = static_cast<FfxFsr3UpscalerContext_Private*>(malloc(sizeof(FfxFsr3UpscalerContext_Private)));
    FFX_RETURN_ON_ERROR(contextPrivate, FFX_ERROR_OUT_OF_MEMORY);
    ffxFsr3UpscalerSetPrivateContext(context, contextPrivate);
    const FfxErrorCode errorCode = fsr3upscalerCreate(contextPrivate, contextDescription);

    return errorCode;]=]
    "allocate the private context without changing the public ABI")

ffx_sdk_replace_required(
    "${FSR_STAGED_FSR3_SOURCE}"
    "${FSR_SDK_ROOT}/sdk/src/components/fsr3upscaler/ffx_fsr3upscaler.cpp"
    [=[    // destroy the context.
    FfxFsr3UpscalerContext_Private* contextPrivate = (FfxFsr3UpscalerContext_Private*)(context);
    const FfxErrorCode errorCode = fsr3upscalerRelease(contextPrivate);
    return errorCode;]=]
    [=[    // destroy the context.
    FfxFsr3UpscalerContext_Private* contextPrivate = ffxFsr3UpscalerGetPrivateContext(context);
    FFX_RETURN_ON_ERROR(contextPrivate, FFX_ERROR_INVALID_POINTER);
    const FfxErrorCode errorCode = fsr3upscalerRelease(contextPrivate);
    free(contextPrivate);
    ffxFsr3UpscalerSetPrivateContext(context, nullptr);
    return errorCode;]=]
    "release the separately allocated private context")

# All remaining public entry points that consume a context must recover the
# staged private allocation rather than reinterpret the public buffer as the
# private object.  The create and destroy entry points above intentionally
# replace their full blocks first, so this final replacement handles all
# other upstream occurrences cumulatively.
ffx_sdk_replace_required(
    "${FSR_STAGED_FSR3_SOURCE}"
    "${FSR_SDK_ROOT}/sdk/src/components/fsr3upscaler/ffx_fsr3upscaler.cpp"
    [=[(FfxFsr3UpscalerContext_Private*)(context)]=]
    [=[ffxFsr3UpscalerGetPrivateContext(context)]=]
    "use the allocated private context in public entry points")

# ffx_vk.cpp installs this callback even for the upscaler-only backend, but the
# implementation lives in the excluded FrameInterpolationSwapchain sources.
# The FSR3 Upscaler interface does not use the callback, so leave it unset.
ffx_sdk_replace_required(
    "${FSR_STAGED_VK_SOURCE}"
    "${FSR_SDK_ROOT}/sdk/src/backends/vk/ffx_vk.cpp"
    "    backendInterface->fpSwapChainConfigureFrameGeneration = ffxSetFrameGenerationConfigToSwapchainVK;"
    "    backendInterface->fpSwapChainConfigureFrameGeneration = nullptr;"
    "disable the excluded frame-interpolation callback")

# RADV can expose AMD coherent or uncached memory types that require an
# optional device feature this selector cannot enable, so never choose them.
ffx_sdk_replace_required(
    "${FSR_STAGED_VK_SOURCE}"
    "${FSR_SDK_ROOT}/sdk/src/backends/vk/ffx_vk.cpp"
    [=[uint32_t findMemoryTypeIndex(VkPhysicalDevice physicalDevice, VkMemoryRequirements memRequirements, VkMemoryPropertyFlags requestedProperties, VkMemoryPropertyFlags& outProperties)
{
    FFX_ASSERT(NULL != physicalDevice);

    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProperties);

    uint32_t bestCandidate = UINT32_MAX;

    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        if ((memRequirements.memoryTypeBits & (1 << i)) && (memProperties.memoryTypes[i].propertyFlags & requestedProperties)) {

            // if just device-local memory is requested, make sure this is the invisible heap to prevent over-subscribing the local heap
            if (requestedProperties == VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT && (memProperties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
                continue;

            bestCandidate = i;
            outProperties = memProperties.memoryTypes[i].propertyFlags;

            // if host-visible memory is requested, check for host coherency as well and if available, return immediately
            if ((requestedProperties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) && (memProperties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
                return bestCandidate;
        }
    }

    return bestCandidate;
}]=]
    [=[uint32_t findMemoryTypeIndex(VkPhysicalDevice physicalDevice, VkMemoryRequirements memRequirements, VkMemoryPropertyFlags requestedProperties, VkMemoryPropertyFlags& outProperties)
{
    outProperties = 0;
    FFX_ASSERT(NULL != physicalDevice);

    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProperties);

    uint32_t bestCandidate = UINT32_MAX;

    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        const VkMemoryPropertyFlags propertyFlags = memProperties.memoryTypes[i].propertyFlags;

        if ((memRequirements.memoryTypeBits & (1u << i)) == 0)
            continue;
        if ((propertyFlags & requestedProperties) != requestedProperties)
            continue;
        if (propertyFlags & (VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD | VK_MEMORY_PROPERTY_DEVICE_UNCACHED_BIT_AMD))
            continue;

        // if just device-local memory is requested, make sure this is the invisible heap to prevent over-subscribing the local heap
        if (requestedProperties == VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT && (propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
            continue;

        bestCandidate = i;
        outProperties = propertyFlags;

        // if host-visible memory is requested, check for host coherency as well and if available, return immediately
        if ((requestedProperties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) && (propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
            return bestCandidate;
    }

    return bestCandidate;
}]=]
    "avoid unsupported AMD specialized memory types in the SDK selector")

# ffx_message.cpp uses FFX_UNUSED in its non-Windows branch without including
# the SDK utility header that defines it.
ffx_sdk_replace_required(
    "${FSR_STAGED_MESSAGE_SOURCE}"
    "${FSR_SDK_ROOT}/sdk/src/shared/ffx_message.cpp"
    "    FFX_UNUSED(type);\n    FFX_UNUSED(message);"
    "    (void)type;\n    (void)message;"
    "fix the missing non-Windows FFX_UNUSED include")

# EffectContext is over-aligned, so align its mapped array from the actual
# scratch-buffer address before constructing objects in it.
ffx_sdk_replace_required(
    "${FSR_STAGED_VK_SOURCE}"
    "${FSR_SDK_ROOT}/sdk/src/backends/vk/ffx_vk.cpp"
    [=[        // Map context array
        backendContext->pEffectContexts = (BackendContext_VK::EffectContext*)pMem;
        memset(backendContext->pEffectContexts, 0, contextArraySize);]=]
    [=[        const uintptr_t effectContextAlignment = alignof(BackendContext_VK::EffectContext);
        const uintptr_t pMemAddress = reinterpret_cast<uintptr_t>(pMem);
        const uintptr_t alignedEffectContextAddress =
            pMemAddress + (effectContextAlignment - (pMemAddress % effectContextAlignment)) % effectContextAlignment;
        pMem = reinterpret_cast<uint8_t*>(alignedEffectContextAddress);

        // Map context array
        backendContext->pEffectContexts = (BackendContext_VK::EffectContext*)pMem;
        if ((reinterpret_cast<uintptr_t>(backendContext->pEffectContexts) % effectContextAlignment) != 0)
        {
            FFX_ASSERT_FAIL("Vulkan effect-context array is not correctly aligned.");
            return FFX_ERROR_BACKEND_API_ERROR;
        }
        memset(backendContext->pEffectContexts, 0, contextArraySize);]=]
    "align the over-aligned Vulkan effect-context array")

# Include worst-case alignment padding in the advertised scratch size.  The
# per-array locals above remain SDK-compatible uint32_t values, but the total
# is deliberately accumulated as size_t.
ffx_sdk_replace_required(
    "${FSR_STAGED_VK_SOURCE}"
    "${FSR_SDK_ROOT}/sdk/src/backends/vk/ffx_vk.cpp"
    [=[    uint32_t contextArraySize = FFX_ALIGN_UP(maxContexts * sizeof(BackendContext_VK::EffectContext), sizeof(uint32_t));
    
    return FFX_ALIGN_UP(sizeof(BackendContext_VK) + extensionPropArraySize + gpuJobDescArraySize + resourceViewArraySize + stagingRingBufferArraySize +
                            pipelineArraySize + resourceArraySize + contextArraySize,
                        sizeof(uint64_t));]=]
    [=[    uint32_t contextArraySize = FFX_ALIGN_UP(maxContexts * sizeof(BackendContext_VK::EffectContext), sizeof(uint32_t));
    const size_t effectContextAlignmentPadding = alignof(BackendContext_VK::EffectContext) - 1;
    const size_t scratchMemorySize = static_cast<size_t>(sizeof(BackendContext_VK)) +
        static_cast<size_t>(extensionPropArraySize) +
        static_cast<size_t>(gpuJobDescArraySize) +
        static_cast<size_t>(resourceViewArraySize) +
        static_cast<size_t>(stagingRingBufferArraySize) +
        static_cast<size_t>(pipelineArraySize) +
        static_cast<size_t>(resourceArraySize) +
        static_cast<size_t>(contextArraySize) +
        effectContextAlignmentPadding;
    
    return FFX_ALIGN_UP(scratchMemorySize, sizeof(uint64_t));]=]
    "reserve scratch space for effect-context alignment padding")
