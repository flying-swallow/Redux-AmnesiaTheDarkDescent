#include "graphics/MaterialResource.h"
#include "graphics/Material.h"
#include "graphics/Image.h" // HPLTexture::format + RIFormatChannelCount probe

// Single source of truth for kMaterialFlag* — these used to be mirrored
// here as the `TextureConfigFlags` enum but now come from the shared
// shader/host header.
#include "Constants.h"

namespace hpl::material {

    UniformMaterialBlock UniformMaterialBlock::CreateFromMaterial(cMaterial& material) {
        UniformMaterialBlock block = {};
        const ShaderMaterialData& descriptor = material.Descriptor();
        block.m_common.m_materialConfig = UniformMaterialBlock::CreateMaterailConfigFlags(material);
        switch (descriptor.m_id) {
            case MaterialID::SolidDiffuse:
                block.m_solid.m_heightMapScale = descriptor.m_solid.m_heightMapScale;
                block.m_solid.m_heightMapBias = descriptor.m_solid.m_heightMapBias;
                block.m_solid.m_frenselBias = descriptor.m_solid.m_frenselBias;
                block.m_solid.m_frenselPow = descriptor.m_solid.m_frenselPow;
                break;
            case MaterialID::Decal:
                break;
            case MaterialID::Translucent:
                block.m_translucent.m_refractionScale = descriptor.m_translucent.m_refractionScale;
                block.m_translucent.m_frenselBias = descriptor.m_translucent.m_frenselBias;
                block.m_translucent.m_frenselPow = descriptor.m_translucent.m_frenselPow;
                block.m_translucent.m_rimLightMul = descriptor.m_translucent.m_rimLightMul;
                block.m_translucent.m_rimLightPow = descriptor.m_translucent.m_rimLightPow;
                break;
            case MaterialID::Water:
                block.m_water.m_refractionScale = descriptor.m_water.m_refractionScale;
                block.m_water.m_frenselBias = descriptor.m_water.m_frenselBias;
                block.m_water.m_frenselPow = descriptor.m_water.m_frenselPow;
                block.m_water.m_reflectionFadeStart = descriptor.m_water.m_reflectionFadeStart;
                block.m_water.m_reflectionFadeEnd = descriptor.m_water.m_reflectionFadeEnd;
                block.m_water.m_waveSpeed = descriptor.m_water.m_waveSpeed;
                block.m_water.m_waveAmplitude = descriptor.m_water.m_waveAmplitude;
                block.m_water.m_waveFreq = descriptor.m_water.m_waveFreq;
                break;
            case MaterialID::Unknown:
            case MaterialID::MaterialIDCount:
            default:
                break;
        }
        return block;
    }

    uint32_t UniformMaterialBlock::CreateMaterailConfigFlags(cMaterial& material) {
        // Alpha/height maps come in two layouts: dedicated grayscale files
        // (data in .r — e.g. chandelier_simple_alpha.tga loads as R8_UNORM)
        // and packed RGBA (data in .a). Probe the channel count of the
        // texture the image was actually created with so the shaders can
        // pick the right channel; sampling .a on a single-channel texture
        // reads the constant fill value (POM runs its full 32 steps and
        // warps walls at grazing angles, alpha test cuts everything or
        // nothing).
        auto isSingleChannel = [](const Image* image) {
            if (!image) {
                return false;
            }
            const auto texture = image->GetTexture();
            return texture && RIFormatChannelCount(texture->format) == 1;
        };
        const Image* alphaImage = material.GetImage(eMaterialTexture_Alpha);
        const Image* heightImage = material.GetImage(eMaterialTexture_Height);
        uint32_t flags =
            (material.GetImage(eMaterialTexture_Diffuse) ? kMaterialFlagEnableDiffuse : 0) |
            (material.GetImage(eMaterialTexture_NMap) ? kMaterialFlagEnableNormal : 0) |
            (material.GetImage(eMaterialTexture_Specular) ? kMaterialFlagEnableSpecular : 0) |
            (alphaImage ? kMaterialFlagEnableAlpha : 0) |
            (isSingleChannel(alphaImage) ? kMaterialFlagIsAlphaSingleChannel : 0) |
            (heightImage ? kMaterialFlagEnableHeight : 0) |
            (isSingleChannel(heightImage) ? kMaterialFlagIsHeightMapSingleChannel : 0) |
            (material.GetImage(eMaterialTexture_Illumination) ? kMaterialFlagEnableIllumination : 0) |
            (material.GetImage(eMaterialTexture_CubeMap) ? kMaterialFlagEnableCubeMap : 0) |
            (material.GetImage(eMaterialTexture_DissolveAlpha) ? kMaterialFlagEnableDissolveAlpha : 0) |
            (material.GetImage(eMaterialTexture_CubeMapAlpha) ? kMaterialFlagEnableCubeMapAlpha : 0);

        const ShaderMaterialData& descriptor = material.Descriptor();
        switch (descriptor.m_id) {
            case MaterialID::SolidDiffuse:
                flags |= (descriptor.m_solid.m_alphaDissolveFilter ? kMaterialFlagUseDissolveFilter : 0);
                break;
            case MaterialID::Translucent:
                flags |= (descriptor.m_translucent.m_refractionNormals ? kMaterialFlagUseRefractionNormals : 0) |
                         (descriptor.m_translucent.m_hasRefraction &&
                          descriptor.m_translucent.m_refractionEdgeCheck ? kMaterialFlagUseRefractionEdgeCheck : 0) |
                         (descriptor.m_translucent.m_hasRefraction ? kMaterialFlagHasRefraction : 0) |
                         (descriptor.m_translucent.m_isAffectedByLightLevel ? kMaterialFlagAffectedByLightLevel : 0);
                break;
            case MaterialID::Water:
                // Water always refracts + reflects via the SurfelVBuffer.rt
                // wave-animated bounce. IsWater drives that branch; HasRefraction
                // makes the GIRenderPass swap show the refracted background.
                flags |= kMaterialFlagIsWater | kMaterialFlagHasRefraction;
                break;
            default:
                break;
        }
        return flags;
    }

} // namespace hpl::material
