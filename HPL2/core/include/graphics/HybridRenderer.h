#ifndef HPL_RENDERER_HYBRID_H
#define HPL_RENDERER_HYBRID_H

#include "graphics/BindlessPool.h"
#include "graphics/HPLGraphicsConfig.h"
#include "graphics/Material.h"
#include "graphics/RenderList2.h"
#include "graphics/Renderer.h"
#include "graphics/RISegmentAlloc.h"
#include "graphics/RITypes.h"
#include "graphics/RIRenderer.h"
#include "system/Hasher.h"

#include <array>
#include <vector>

namespace hpl {

struct SurfelElement {
  float m_pos_x,m_pos_y,m_pos_z, m_radius;
  float m_normal_x,m_normal_y,m_normal_z, m_pad_0;
};

// Mirrors `UniformObject` in amnesia/glsl/forward_shader_common.glsl (std430).
struct alignas(16) ObjectGPUData {
  float    dissolveAmount;
  uint32_t materialID;
  float    lightLevel;
  float    illuminationAmount;
  float    modelMat[16];
  float    invModelMat[16];
  float    uvMat[16];
};
static_assert(sizeof(ObjectGPUData) == 208,
              "must match forward_shader_common.glsl::UniformObject std430");

// Mirrors `PerFrameConstants` in amnesia/glsl/forward_shader_common.glsl (std140).
struct alignas(16) PerFrameConstants {
  float invViewRotationMat[16];
  float viewMat[16];
  float invViewMat[16];
  float projMat[16];
  float viewProjMat[16];
  float worldFogStart;
  float worldFogLength;
  float oneMinusFogAlpha;
  float fogFalloffExp;
  float worldFogColor[4];
  float viewTexel[2];
  float viewportSize[2];
  float afT;
  float _pad[3];
};

class cHybridRenderer : public iRenderer {
public:
  cHybridRenderer(cGraphics *apGraphics, cResources *apResources);
  ~cHybridRenderer();

  static constexpr uint32_t OBJECT_SLOT_CAPACITY = 16384 * 2;

  virtual void Draw(RIBootstrap::FrameContext *cntx, cViewport *viewport,
                    float afFrameTime, cFrustum *apFrustum, cWorld *apWorld,
                    cRenderSettings *apSettings,
                    bool abSendFrameBufferToPostEffects) override;

  virtual bool LoadData() override { return true; };
  virtual void DestroyData() override {};
  virtual void CopyToFrameBuffer() override {};
  virtual void SetupRenderList() override {};
  virtual void RenderObjects() override {};

private:
  
  cRenderList2 m_rendererList;

  BindlessPool m_diffuseBindless;
  struct RIBuffer_s m_diffuseObjectBuffer;
  struct RIBuffer_s m_opaquePositionHandles;
  struct RIBuffer_s m_opaqueTangentHandles;
  struct RIBuffer_s m_opaqueNormalHandles;
  struct RIBuffer_s m_opaqueUv0Handles;
  struct RIBuffer_s m_opaqueColorHandles;
  struct RIBuffer_s m_opaqueIndexHandles;

  RISegmentAlloc<RI_NUMBER_FRAME_SEGMENTS> m_indirectSegment;
  struct RIBuffer_s m_indirectDrawBuffer;

	RIProgram m_forwardDiffuse;
};

} // namespace hpl

#endif
