#ifndef HPL_RENDERER_HYBRID_H
#define HPL_RENDERER_HYBRID_H

#include "graphics/HPLGraphicsConfig.h"
#include "graphics/Material.h"
#include "graphics/RenderList2.h"
#include "graphics/Renderer.h"
#include "graphics/RIRenderer.h"

#include <array>

namespace hpl {

struct SurfelElement {
  float m_pos_x,m_pos_y,m_pos_z, m_radius;
  float m_normal_x,m_normal_y,m_normal_z, m_pad_0;
};

class cHybridRenderer : public iRenderer {
public:
  cHybridRenderer(cGraphics *apGraphics, cResources *apResources);
  ~cHybridRenderer();

  static constexpr uint32_t UBO_BUFFER_SIZE = 8 * (1024 * 1024); // 8 MB

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

  // Per-(material, frame) version tracker. The renderer re-uploads a material's
  // packed block whenever the material pointer or its Generation() drifts from
  // what we wrote last frame. Triple-buffered so in-flight frames don't overwrite
  // each other's view of the data.
  //struct MaterialDescInfo {
  //  void *m_material = nullptr;
  //  uint32_t m_version = 0;
  //};
  //struct MaterialInfo {
  //  std::array<MaterialDescInfo, RI_NUMBER_FRAMES_FLIGHT> m_perFrame{};
  //};
  //std::array<MaterialInfo, cMaterial::MaxMaterialID> m_materialInfo{};
  
  cRenderList2 renderList;

  struct RIBuffer_s positionBuffer;
  struct RIBuffer_s normalBuffer;
  struct RIBuffer_s colorBuffer;
  struct RIBuffer_s uvBuffer;
};

} // namespace hpl

#endif
