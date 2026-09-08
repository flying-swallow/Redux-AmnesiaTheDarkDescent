// Build-tool validation for the Vulkan FSR3 Upscaler shader accessors.
//
// This test only examines embedded SPIR-V and reflection metadata. It never
// creates a Vulkan object or submits work to a device.

#include <FidelityFX/host/ffx_fsr3upscaler.h>
#include <FidelityFX/host/ffx_types.h>
#include "ffx_fsr3upscaler_shaderblobs.h"
#include "fsr3upscaler/ffx_fsr3upscaler_private.h"
#include "spirv_reflect.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {

struct PassCase {
    FfxFsr3UpscalerPass pass;
    const char* name;
};

// This is the complete supported switch in
// sdk/src/backends/shared/blob_accessors/ffx_fsr3upscaler_shaderblobs.cpp.
const PassCase kPasses[] = {
    { FFX_FSR3UPSCALER_PASS_PREPARE_INPUTS, "prepare_inputs" },
    { FFX_FSR3UPSCALER_PASS_LUMA_PYRAMID, "luma_pyramid" },
    { FFX_FSR3UPSCALER_PASS_SHADING_CHANGE_PYRAMID, "shading_change_pyramid" },
    { FFX_FSR3UPSCALER_PASS_SHADING_CHANGE, "shading_change" },
    { FFX_FSR3UPSCALER_PASS_PREPARE_REACTIVITY, "prepare_reactivity" },
    { FFX_FSR3UPSCALER_PASS_LUMA_INSTABILITY, "luma_instability" },
    { FFX_FSR3UPSCALER_PASS_ACCUMULATE, "accumulate" },
    { FFX_FSR3UPSCALER_PASS_ACCUMULATE_SHARPEN, "accumulate_sharpen" },
    { FFX_FSR3UPSCALER_PASS_RCAS, "rcas" },
    { FFX_FSR3UPSCALER_PASS_DEBUG_VIEW, "debug_view" },
    { FFX_FSR3UPSCALER_PASS_GENERATE_REACTIVE, "generate_reactive" },
};

enum class ResourceKind {
    Cbv,
    SrvTexture,
    UavTexture,
    SrvBuffer,
    UavBuffer,
    Sampler,
    AccelerationStructure,
};

struct ReflectedResource {
    const char* name;
    uint32_t binding;
    uint32_t set;
    uint32_t count;
    ResourceKind kind;
};

struct ResourceGroup {
    ResourceKind kind;
    uint32_t count;
    const char** names;
    const uint32_t* bindings;
    const uint32_t* counts;
    const uint32_t* sets;
};

int g_failures = 0;
uint32_t g_calls = 0;
uint32_t g_uniqueBlobs = 0;

void fail(const std::string& message) {
    ++g_failures;
    std::fprintf(stderr, "FAIL: %s\n", message.c_str());
}

void require(bool condition, const std::string& message) {
    if (!condition)
        fail(message);
}

std::string pass_prefix(const PassCase& pass, uint32_t options) {
    char buffer[128];
    std::snprintf(buffer, sizeof(buffer), "%s options=0x%02x", pass.name, options);
    return buffer;
}

std::vector<ResourceGroup> resource_groups(const FfxShaderBlob& blob) {
    return {
        { ResourceKind::Cbv, blob.cbvCount,
          blob.boundConstantBufferNames,
          blob.boundConstantBuffers, blob.boundConstantBufferCounts, blob.boundConstantBufferSpaces },
        { ResourceKind::SrvTexture, blob.srvTextureCount,
          blob.boundSRVTextureNames,
          blob.boundSRVTextures, blob.boundSRVTextureCounts, blob.boundSRVTextureSpaces },
        { ResourceKind::UavTexture, blob.uavTextureCount,
          blob.boundUAVTextureNames,
          blob.boundUAVTextures, blob.boundUAVTextureCounts, blob.boundUAVTextureSpaces },
        { ResourceKind::SrvBuffer, blob.srvBufferCount,
          blob.boundSRVBufferNames,
          blob.boundSRVBuffers, blob.boundSRVBufferCounts, blob.boundSRVBufferSpaces },
        { ResourceKind::UavBuffer, blob.uavBufferCount,
          blob.boundUAVBufferNames,
          blob.boundUAVBuffers, blob.boundUAVBufferCounts, blob.boundUAVBufferSpaces },
        { ResourceKind::Sampler, blob.samplerCount,
          blob.boundSamplerNames,
          blob.boundSamplers, blob.boundSamplerCounts, blob.boundSamplerSpaces },
        { ResourceKind::AccelerationStructure, blob.rtAccelStructCount,
          blob.boundRTAccelerationStructureNames,
          blob.boundRTAccelerationStructures, blob.boundRTAccelerationStructureCounts,
          blob.boundRTAccelerationStructureSpaces },
    };
}

bool descriptor_type_matches(ResourceKind kind, SpvReflectDescriptorType type) {
    switch (kind) {
        case ResourceKind::Cbv:
            return type == SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_BUFFER ||
                   type == SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        case ResourceKind::SrvTexture:
            return type == SPV_REFLECT_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ||
                   type == SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        case ResourceKind::UavTexture:
            return type == SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        case ResourceKind::SrvBuffer:
            return type == SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
        case ResourceKind::UavBuffer:
            return type == SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER ||
                   type == SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_BUFFER ||
                   type == SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
        case ResourceKind::Sampler:
            return type == SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLER;
        case ResourceKind::AccelerationStructure:
            return type == SPV_REFLECT_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    }
    return false;
}

bool validate_blob(const PassCase& pass, uint32_t options, const FfxShaderBlob& blob) {
    const std::string prefix = pass_prefix(pass, options);
    bool valid = true;

    require(blob.data != nullptr && blob.size != 0, prefix + ": returned an empty blob");
    if (blob.data == nullptr || blob.size == 0)
        return false;
    require(blob.size % sizeof(uint32_t) == 0, prefix + ": SPIR-V size is not word aligned");
    if (blob.size % sizeof(uint32_t) != 0)
        valid = false;
    const uint32_t* words = reinterpret_cast<const uint32_t*>(blob.data);
    require(words[0] == 0x07230203u, prefix + ": invalid SPIR-V magic");
    if (words[0] != 0x07230203u)
        valid = false;

    std::vector<ResourceGroup> groups = resource_groups(blob);
    std::set<std::pair<uint32_t, uint32_t>> metadataSlots;
    uint32_t metadataCount = 0;
    std::vector<ReflectedResource> metadata;
    for (const ResourceGroup& group : groups) {
        if (group.count == 0)
            continue;
        require(group.names != nullptr && group.bindings != nullptr &&
                    group.counts != nullptr && group.sets != nullptr,
                prefix + ": reflection arrays are incomplete");
        if (group.names == nullptr || group.bindings == nullptr ||
            group.counts == nullptr || group.sets == nullptr)
            valid = false;
        for (uint32_t index = 0; index < group.count; ++index) {
            if (group.names == nullptr || group.bindings == nullptr ||
                group.counts == nullptr || group.sets == nullptr)
                break;
            require(group.names[index] != nullptr && group.names[index][0] != '\0',
                    prefix + ": reflection resource has no name");
            require(group.counts[index] != 0, prefix + ": reflection resource has zero count");
            const auto slot = std::make_pair(group.sets[index], group.bindings[index]);
            require(metadataSlots.insert(slot).second,
                    prefix + ": duplicate reflection descriptor slot");
            metadata.push_back({ group.names[index], group.bindings[index], group.sets[index],
                                 group.counts[index], group.kind });
            ++metadataCount;
        }
    }

    SpvReflectShaderModule module{};
    const SpvReflectResult createResult = spvReflectCreateShaderModule(blob.size, blob.data, &module);
    require(createResult == SPV_REFLECT_RESULT_SUCCESS, prefix + ": SPIRV-Reflect could not parse blob");
    if (createResult != SPV_REFLECT_RESULT_SUCCESS)
        return false;

    uint32_t reflectedCount = 0;
    SpvReflectResult result = spvReflectEnumerateDescriptorBindings(&module, &reflectedCount, nullptr);
    require(result == SPV_REFLECT_RESULT_SUCCESS, prefix + ": could not enumerate descriptor bindings");
    require(reflectedCount == metadataCount, prefix + ": reflection count does not match SPIR-V");
    if (result != SPV_REFLECT_RESULT_SUCCESS || reflectedCount != metadataCount) {
        spvReflectDestroyShaderModule(&module);
        return false;
    }

    std::vector<SpvReflectDescriptorBinding*> reflected(reflectedCount);
    result = spvReflectEnumerateDescriptorBindings(&module, &reflectedCount, reflected.data());
    require(result == SPV_REFLECT_RESULT_SUCCESS, prefix + ": descriptor enumeration failed");
    if (result == SPV_REFLECT_RESULT_SUCCESS) {
        std::set<std::pair<uint32_t, uint32_t>> spirvSlots;
        for (SpvReflectDescriptorBinding* binding : reflected) {
            require(binding != nullptr && binding->name != nullptr,
                    prefix + ": SPIR-V descriptor has no name");
            if (binding == nullptr || binding->name == nullptr) {
                valid = false;
                continue;
            }
            require(spirvSlots.insert({ binding->set, binding->binding }).second,
                    prefix + ": duplicate SPIR-V descriptor slot");
            const ReflectedResource* matching = nullptr;
            for (const ReflectedResource& expected : metadata) {
                if (std::strcmp(expected.name, binding->name) == 0 &&
                    expected.binding == binding->binding && expected.set == binding->set) {
                    matching = &expected;
                    break;
                }
            }
            require(matching != nullptr, prefix + ": SPIR-V descriptor is absent from blob metadata");
            if (matching == nullptr)
                valid = false;
            else {
                require(binding->count == matching->count,
                        prefix + ": descriptor array count disagrees with SPIR-V");
                require(descriptor_type_matches(matching->kind, binding->descriptor_type),
                        prefix + ": descriptor type disagrees with metadata category");
            }
        }
    }

    spvReflectDestroyShaderModule(&module);
    return valid;
}

} // namespace

int main() {
    constexpr uint32_t kShaderPermutationMask =
        FSR3UPSCALER_SHADER_PERMUTATION_USE_LANCZOS_TYPE |
        FSR3UPSCALER_SHADER_PERMUTATION_HDR_COLOR_INPUT |
        FSR3UPSCALER_SHADER_PERMUTATION_LOW_RES_MOTION_VECTORS |
        FSR3UPSCALER_SHADER_PERMUTATION_JITTER_MOTION_VECTORS |
        FSR3UPSCALER_SHADER_PERMUTATION_DEPTH_INVERTED |
        FSR3UPSCALER_SHADER_PERMUTATION_ENABLE_SHARPENING;
    std::map<std::pair<const uint8_t*, uint32_t>, bool> validated;
    for (const PassCase& pass : kPasses) {
        for (uint32_t base = 0; base <= kShaderPermutationMask; ++base) {
            for (uint32_t wave64 = 0; wave64 <= 1; ++wave64) {
                for (uint32_t fp16 = 0; fp16 <= 1; ++fp16) {
                    const uint32_t variants =
                        (wave64 != 0 ? FSR3UPSCALER_SHADER_PERMUTATION_FORCE_WAVE64 : 0) |
                        (fp16 != 0 ? FSR3UPSCALER_SHADER_PERMUTATION_ALLOW_FP16 : 0);
                const uint32_t options = base | variants;
                FfxShaderBlob blob{};
                const FfxErrorCode result =
                    fsr3UpscalerGetPermutationBlobByIndex(pass.pass, options, &blob);
                ++g_calls;
                require(result == FFX_OK, pass_prefix(pass, options) + ": accessor failed");
                if (result != FFX_OK)
                    continue;

                const auto key = std::make_pair(blob.data, blob.size);
                if (validated.find(key) == validated.end()) {
                    ++g_uniqueBlobs;
                    validated.emplace(key, validate_blob(pass, options, blob));
                } else {
                    require(validated[key], pass_prefix(pass, options) + ": shared blob failed validation");
                }
                }
            }
        }
    }

    // The accessor intentionally ignores the FP16 bit for prepare_inputs,
    // luma_pyramid, shading_change_pyramid, luma_instability, debug_view and
    // RCAS on non-Xbox builds. Check the especially easy-to-regress case that
    // prepare_inputs has exactly that behavior.
    for (uint32_t base = 0; base <= kShaderPermutationMask; ++base) {
        for (uint32_t wave = 0; wave <= FSR3UPSCALER_SHADER_PERMUTATION_FORCE_WAVE64; wave +=
             FSR3UPSCALER_SHADER_PERMUTATION_FORCE_WAVE64) {
            FfxShaderBlob withoutFp16{};
            FfxShaderBlob withFp16{};
            fsr3UpscalerGetPermutationBlobByIndex(
                FFX_FSR3UPSCALER_PASS_PREPARE_INPUTS, base | wave, &withoutFp16);
            fsr3UpscalerGetPermutationBlobByIndex(
                FFX_FSR3UPSCALER_PASS_PREPARE_INPUTS,
                base | wave | FSR3UPSCALER_SHADER_PERMUTATION_ALLOW_FP16, &withFp16);
            require(withoutFp16.data == withFp16.data && withoutFp16.size == withFp16.size,
                    "prepare_inputs unexpectedly consumes the FP16 permutation bit");
        }
    }

    std::printf("FSR shader accessor checks: %u calls, %u distinct blobs, %d failures\n",
                g_calls, g_uniqueBlobs, g_failures);
    return g_failures == 0 ? 0 : 1;
}
