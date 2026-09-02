#ifndef HPL_RENDERER_HYBRID_H
#define HPL_RENDERER_HYBRID_H

#include "graphics/GlobalManagedSets.h"
#include "graphics/HPLGraphicsConfig.h"
#include "graphics/Material.h"
#include "graphics/RenderList.h"
#include "graphics/Renderer.h"
#include "graphics/RISegmentAlloc.h"
#include "graphics/RITypes.h"
#include <array>

#include "Constants.h"

namespace hpl {

class Image;
class cVertexBuffer;

class cHybridRenderer : public iRenderer {
public:
  cHybridRenderer(cGraphics *apGraphics, cResources *apResources);
  ~cHybridRenderer();

  // Per-viewport internals live in HybridViewportState (pure data — see
  // scene/Viewport.h), held by cViewport; this renderer owns its
  // creation/sizing (file-local helper in HybridRenderer.cpp).

  // kObjectSlotCapacity / kTextureSlotCapacity / kMaterialCapacity come from
  // amnesia/glsl/forward_shared.h.

  virtual void Draw(cGraphics::FrameContext *cntx, cViewport *viewport,
                    float afFrameTime, cFrustum *apFrustum, cWorld *apWorld,
                    cRenderSettings *apSettings,
                    bool abSendFrameBufferToPostEffects) override;

  virtual bool LoadData() override { return true; };
  virtual void DestroyData() override {};

  void SetOverlay(int alOverlay) { m_overlayMode = (uint32_t)alOverlay; }

private:
  cRenderList2 m_rendererList;

  // The object-slot cache now lives in m_global (GlobalManagedSets::
  // m_objectSlots); submitObject() owns slot allocation + the slot-generation
  // bump (on new occupant / index-count change / VB destroy), so the renderer no
  // longer holds the cache, the per-slot hooks, or a geometry-dirty array.

  // Default-value fallback vertex streams for renderables that omit an optional
  // stream now live on the cGraphics singleton (Interface<cGraphics>::Get()->fallback*Vertex), created once
  // at init; detail::BindVertexStreams binds them in the raster passes.

  RISegmentAlloc<RI_NUMBER_FRAME_SEGMENTS> m_indirectSegment;
  struct RIBuffer m_indirectDrawBuffer;

  // The ray-tracing TLAS (storage + instance buffer) is owned by cWorld and built
  // in cWorld::PrepareFrame; each RT pass binds it via apWorld->GetTlas().

  // (The per-viewport frame textures — packed hit info,
  // velocity, direct-lighting history + ping-pong index/init, prev-frame
  // camera — live on cViewport::HybridViewportState; their Update/Dispose
  // are defined in HybridRenderer.cpp.)

  // prepare gbuffer
	RIProgram m_gbuffer;

  // VBuffer POM barycentric correction compute pass (amnesia/slang/VBuffer/).
  // Copies visibilityTexture (raw raster hit) → packedHitInfoTexture and
  // perturbs barycentrics on height-mapped diffuse surfaces so downstream
  // getVertexData() reconstructs the parallax-occluded point.
  // There is no refraction pass: water/glass pixels keep the rasterized
  // front-surface hit, so nothing behind them is bent.
  RIProgram m_vBufferPomBary;

  // Clustered light-grid build (LightGridBuildPass.cs). Bins the frame's
  // lights into the world-space cell grid that next-event estimation reads:
  // both the direct-lighting pass and the path tracer's getCellLights()
  // depend on it, so it runs before either.
  RIProgram m_lightGrid;

  // Per-pixel path tracer (amnesia/slang/PathTracer/PathTracePass.rt.slang).
  // Rooted at the primary V-buffer hit: one cosine-sampled path per pixel,
  // NEE per vertex, writing demodulated indirect irradiance + the
  // (viewZ, normal) denoiser key. The rgen drives an iterative trace loop
  // (no recursive TraceRay); closest-hit handles material shading +
  // next-event estimation using an inline ray-query for shadow visibility.
  RIProgram m_pathTrace;

  // Final composite (amnesia/slang/Composite/MainCompositePass.cs.slang). Reads
  // gPackedHitInfo + gIndirectLighting + gDirectLighting and writes gOutput.
  RIProgram m_composite;

  // Direct-lighting passes (amnesia/slang/DirectLighting/):
  // DirectLightingPass.cs — soft-shadowed analytic direct lighting, temporally
  // accumulated via the velocity texture; writes the ping-pong direct texture.
  RIProgram m_directLighting;
  // ReSTIR DI spatial reuse + resolve (DirectSpatialReusePass.cs).
  RIProgram m_directSpatialReuse;
  // SVGF-lite à-trous spatial denoise for the direct pass (DirectAtrousPass.cs).
  RIProgram m_directAtrous;
  // Temporal accumulation for the path-traced indirect term. The spatial
  // half reuses m_directAtrous, bound against the indirect textures.
  RIProgram m_indirectTemporal;


	// Particle (translucent) pass — port of legacy RendererDeferred's
	// translucency_particle.{vert,frag}.fsl. Reuses the opaque object/material
	// bindless pools (one OBJECT slot + one MATERIAL slot per emitter per
	// frame); cHybridRenderer writes the particle VB's BDA into the same
	// opaque*Handles[] arrays so the VS can pull pos/uv/color via instanceId.
	// Hardware blend state varies per blend mode — one pipeline per mode is
	// stamped on demand via the program's PipelineSlot cache.
	RIProgram m_particle;

	// Non-particle translucent meshes (glass, lamp glass, decals tagged
	// translucent, etc.). Renders in its own pass after the particle pass into
	// the same pogo "read" half, depth read-only. One pipeline per
	// eMaterialBlendMode (Add/Mul/MulX2/Alpha/PremulAlpha) is stamped on demand
	// via the program's PipelineSlot cache, mirroring m_particle. Refraction
	// and cube-map reflection materials are filtered out at the call site —
	// those need a screen-color copy + cube-map binding the renderer doesn't
	// have yet.
	RIProgram m_translucentMesh;

	// Decal overlay pass. Thin clipped meshes (blood / scorch / impact marks,
	// posters) drawn after the translucent mesh pass into the same pogo "read"
	// half, depth read-only. Reuses the translucent 5-stream fixed-function
	// vertex layout, the m_diffuseBindless / m_objectBuffer object pool,
	// and resolveMaterial (DiffuseMaterial slot — only tex[0] diffuse is used).
	// One pipeline per eMaterialBlendMode is stamped via the program's
	// PipelineSlot cache. Port of decal.frag.fsl / decal.vert.fsl.
	RIProgram m_decal;
	RIProgram m_water;

	struct OverlayPushConstants { uint32_t overlayMode; };
	uint32_t m_overlayMode = kDefaultOverlayMode;
};

} // namespace hpl

#endif
