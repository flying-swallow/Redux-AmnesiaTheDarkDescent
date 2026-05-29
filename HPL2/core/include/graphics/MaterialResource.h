#pragma once

#include "graphics/Material.h"
#include <cstdint>

namespace hpl {
    class cMaterial;
    namespace material {

        // Material-config flag bits live in amnesia/slang/Constants.h as
        // `kMaterialFlag*` and are shared with shaders via the SHARED_CONST
        // macro. Use those names directly (e.g. `kMaterialFlagEnableNormal`)
        // — they resolve from the enclosing `hpl::` namespace.

        struct UniformMaterialBlock {
            union {
                struct {
                    uint32_t m_materialConfig;
                } m_common;

                struct {
                    uint32_t m_materialConfig;
                    float m_heightMapScale;
                    float m_heightMapBias;
                    float m_frenselBias;

                    float m_frenselPow;
                    uint32_t m_pad0;
                    uint32_t m_pad1;
                    uint32_t m_pad2;
                } m_solid;

                struct {
                    uint32_t m_materialConfig;
                    float m_refractionScale;
                    float m_frenselBias;
                    float m_frenselPow;

                    float m_rimLightMul;
                    float m_rimLightPow;
                    uint32_t m_pad0;
                    uint32_t m_pad1;
                } m_translucent;

                struct {
                    uint32_t m_materialConfig;
                    float m_refractionScale;
                    float m_frenselBias;
                    float m_frenselPow;

                    float m_reflectionFadeStart;
                    float m_reflectionFadeEnd;
                    float m_waveSpeed;
                    float m_waveAmplitude;

                    float m_waveFreq;
                    uint32_t m_pad0;
                    uint32_t m_pad1;
                    uint32_t m_pad2;
                } m_water;

                // Pad the union to a single 64-byte cacheline so the SSBO
                // stride is power-of-two and per-material reads stay aligned.
                uint32_t m_pad[16];
            };

            static UniformMaterialBlock CreateFromMaterial(cMaterial& material);
            static uint32_t CreateMaterailConfigFlags(cMaterial& material);
        };

        static_assert(sizeof(UniformMaterialBlock) == 64,
                      "UniformMaterialBlock must be 64 bytes (one cacheline) to match the SSBO stride");

    } // namespace material
} // namespace hpl
