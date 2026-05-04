#ifndef HPL_RENDERER_HYBRID_H
#define HPL_RENDERER_HYBRID_H

#include "graphics/Renderer.h"
#include "graphics/RIRenderer.h"

namespace hpl {

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
  struct RIBuffer_s positionBuffer;
  struct RIBuffer_s normalBuffer;
  struct RIBuffer_s colorBuffer;
  struct RIBuffer_s uvBuffer;
};

} // namespace hpl

#endif
