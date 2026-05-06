#include "graphics/MaterialResource.h"
#include "graphics/Material.h"

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
        // TODO: probe alpha/height channel-count via the RIFormat once a
        // RIFormatChannelCount helper exists; reference uses TheForge's
        // TinyImageFormat_ChannelCount which has no Amnesia64 equivalent yet.
        // The IsAlphaSingleChannel / IsHeightMapSingleChannel flags stay 0 until then.
        uint32_t flags =
            (material.GetTexture(eMaterialTexture_Diffuse) ? EnableDiffuse : 0) |
            (material.GetTexture(eMaterialTexture_NMap) ? EnableNormal : 0) |
            (material.GetTexture(eMaterialTexture_Specular) ? EnableSpecular : 0) |
            (material.GetTexture(eMaterialTexture_Alpha) ? EnableAlpha : 0) |
            (material.GetTexture(eMaterialTexture_Height) ? EnableHeight : 0) |
            (material.GetTexture(eMaterialTexture_Illumination) ? EnableIllumination : 0) |
            (material.GetTexture(eMaterialTexture_CubeMap) ? EnableCubeMap : 0) |
            (material.GetTexture(eMaterialTexture_DissolveAlpha) ? EnableDissolveAlpha : 0) |
            (material.GetTexture(eMaterialTexture_CubeMapAlpha) ? EnableCubeMapAlpha : 0);

        const ShaderMaterialData& descriptor = material.Descriptor();
        switch (descriptor.m_id) {
            case MaterialID::SolidDiffuse:
                flags |= (descriptor.m_solid.m_alphaDissolveFilter ? UseDissolveFilter : 0);
                break;
            case MaterialID::Translucent:
                flags |= (descriptor.m_translucent.m_refractionNormals ? UseRefractionNormals : 0) |
                         (descriptor.m_translucent.m_hasRefraction &&
                          descriptor.m_translucent.m_refractionEdgeCheck ? UseRefractionEdgeCheck : 0);
                break;
            default:
                break;
        }
        return flags;
    }

} // namespace hpl::material
