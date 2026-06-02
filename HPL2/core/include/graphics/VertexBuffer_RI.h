#ifndef HPL_DRAWPACKET_H
#define HPL_DRAWPACKET_H

#include <cassert>
#include <graphics/VertexBuffer.h>
#include <graphics/RITypes.h>
#include <graphics/GraphicsTypes.h>
#include <graphics/RIBootstrap.h>
#include <system/Event.h>

#include <algorithm>
#include <array>
#include <memory>
#include <span>
#include <vector>

namespace hpl {
class VertexBuffer_RI : public iVertexBuffer {
public:
  static size_t GetSizeFromHPL(eVertexBufferElementFormat format);

  struct VertexElement {
  public:
    std::shared_ptr<RIBuffer_s> buffer;
    eVertexBufferElementFormat format =
        eVertexBufferElementFormat::eVertexBufferElementFormat_Float;
    eVertexBufferElement type =
        eVertexBufferElement::eVertexBufferElement_Position;
    tVertexElementFlag flag = 0;
    size_t num = 0;
    int programVarIndex = 0; // for legacy behavior

    size_t Stride() const;
    size_t NumElements() const;

    template <typename TData> std::span<TData> GetElements() {
      assert(sizeof(TData) == Stride() && "Data must be same size as stride");
      return std::span<TData *>(reinterpret_cast<TData *>(m_shadowData.data()),
                                m_shadowData.size() / Stride());
    }

    std::span<uint8_t> Data() const { return m_shadowData; }

    template <typename TData> TData &GetElement(size_t index) {
      assert(sizeof(TData) <= Stride() &&
             "Date must be less than or equal to stride");
      return *reinterpret_cast<TData *>(m_shadowData.data() + index * Stride());
    }

    template <typename TData> const TData &GetElement(size_t index) const {
      assert(sizeof(TData) <= Stride() &&
             "Date must be less than or equal to stride");
      return *reinterpret_cast<TData *>(m_shadowData.data() + index * Stride());
    }

  private:
    mutable size_t m_activeCopy = 0;         // the active copy of the data
    mutable size_t m_internalBufferSize = 0; // the size of the internal buffer
    mutable std::vector<uint8_t> m_shadowData = {};
    friend class VertexBuffer_RI;
  };

  VertexBuffer_RI(iLowLevelGraphics* apLowLevelGraphics,
			eVertexBufferType aType, 
			eVertexBufferDrawType aDrawType,eVertexBufferUsageType aUsageType,
			int alReserveVtxSize,int alReserveIdxSize);
  ~VertexBuffer_RI();
	virtual void CreateShadowDouble(bool abUpdateData) override;

  virtual void CreateElementArray(eVertexBufferElement aType,
                                  eVertexBufferElementFormat aFormat,
                                  int alElementNum,
                                  int alProgramVarIndex = 0) override;

  virtual void AddVertexVec3f(eVertexBufferElement aElement,
                              const cVector3f &avVtx) override;
  virtual void AddVertexVec4f(eVertexBufferElement aElement,
                              const cVector3f &avVtx, float afW) override;
  virtual void AddVertexColor(eVertexBufferElement aElement,
                              const cColor &aColor) override;
  virtual void AddIndex(unsigned int alIndex) override;

  virtual bool Compile(tVertexCompileFlag aFlags) override;
  virtual void UpdateData(tVertexElementFlag aTypes, bool abIndices) override;

  virtual void Transform(const cMatrixf &mtxTransform) override;

  virtual void Draw(eVertexBufferDrawType aDrawType =
                        eVertexBufferDrawType_LastEnum) override;
	virtual void Bind() override;
  virtual void UnBind() override;
	virtual void DrawIndices(	unsigned int *apIndices, int alCount,
								eVertexBufferDrawType aDrawType) override;

  struct GeometryBinding {
    struct VertexGeometryEntry {
      VertexElement *element;
      uint64_t offset;
    };
    struct VertexIndexEntry {
      std::shared_ptr<RIBuffer_s> m_buffer;
      uint64_t offset;
      uint32_t numIndicies;
    };
    std::array<VertexGeometryEntry, eVertexBufferElement_LastEnum>
        m_vertexElement; // elements are in the order they are requested
    VertexIndexEntry m_indexBuffer;
  };

  void AttachResourceToCntx(RIBootstrap::FrameContext *cntx);
  // Uploads the vertex/index streams and (when abBuildBlas) rebuilds the BLAS.
  // Pass abBuildBlas=false for renderables that are never TLAS instances
  // (particles/billboards/beams/ropes/decals) to skip the dead BLAS work while
  // still uploading streams for the raster pass.
  void SubmitToGPU(RICmd_s *cmd, RIDevice_s *device,
                   RIBootstrap::FrameContext *cntx, bool abBuildBlas = true);

  virtual iVertexBuffer *CreateCopy(eVertexBufferType aType,
                                    eVertexBufferUsageType aUsageType,
                                    tVertexElementFlag alVtxToCopy) override;

  virtual cBoundingVolume CreateBoundingVolume() override;

  virtual int GetVertexNum() override;
  virtual int GetIndexNum() override;

  virtual int GetElementNum(eVertexBufferElement aElement) override;
  virtual eVertexBufferElementFormat
  GetElementFormat(eVertexBufferElement aElement) override;
  virtual int GetElementProgramVarIndex(eVertexBufferElement aElement) override;

  virtual float *GetFloatArray(eVertexBufferElement aElement) override;
  virtual int *GetIntArray(eVertexBufferElement aElement) override;
  virtual unsigned char *GetByteArray(eVertexBufferElement aElement) override;

  virtual unsigned int *GetIndices() override;

  virtual void ResizeArray(eVertexBufferElement aElement, int alSize) override;
  virtual void ResizeIndices(int alSize) override;

  const VertexBuffer_RI::VertexElement *GetElement(eVertexBufferElement elementType);
  const std::shared_ptr<RIAccelStructure_s> accelStructure() { return m_blas; }
  const std::shared_ptr<RIBuffer_s> &GetIndexRIBuffer() const { return m_indexBuffer; }

  // GI surfel-cleanup hook. The surfel renderer binds an EventHandler (stored in
  // its bindless-slot cache) to this event; on destruction the VB Signals it so
  // the renderer can retire this VB's bindless slot(s) and clear surfels anchored
  // to them before the now-freed vertex buffer-device-address is dereferenced ->
  // GPUVM fault.
  Event<> &OnDestroyed() { return m_onDestroyed; }

  // GI surfel-invalidation hook. Signaled whenever this VB's GPU geometry is
  // rebuilt in a way that can move triangles around — Compile() (incl. the
  // CreateCopy finalize) and the SubmitToGPU realloc path (first submit,
  // shadow-data growth, CreateCopy sentinel). The surfel renderer binds a
  // per-slot handler (stored in its bindless-slot cache) that bumps the slot's
  // reuse generation, so surfels carrying a cached primitiveIndex from the old
  // layout self-invalidate in collectCellInfo before the now-stale vertex/index
  // BDA is dereferenced -> OOB read -> GPUVM fault. Plain UpdateData() re-uploads
  // (same buffer, same triangle count) do NOT signal, so animated meshes are not
  // over-invalidated.
  Event<> &OnGeometryChanged() { return m_onGeometryChanged; }

  // Lazily builds (or returns) a BLAS for this VB's position/index buffers. Returns nullptr
  // if RT isn't supported, the VB hasn't been Compile()'d, or there's no index buffer.
  // The build is recorded into `cmd`; the caller must ensure that command buffer is submitted
  // and finished before the BLAS is read (e.g. by inserting a barrier before the TLAS build).
  struct RIAccelStructure_s *GetOrBuildBlas(struct RIDevice_s *device, struct RICmd_s *cmd);



protected:
  static void
  PushVertexElements(std::span<const float> values,
                     eVertexBufferElement elementType,
                     std::span<VertexBuffer_RI::VertexElement> elements);

  std::vector<VertexElement> m_vertexElements = {};
  std::shared_ptr<RIBuffer_s> m_indexBuffer;
  std::vector<uint32_t> m_indices = {};

  uint32_t m_generation = 0;
  uint32_t m_lastSubmitted = 0;

  size_t m_indexBufferActiveCopy = 0;
  size_t m_indexBufferCapacity = 0; // bytes allocated in m_indexBuffer
  tVertexElementFlag m_updateFlags = 0; // update no need to rebuild buffers
  bool m_updateIndices = false;

  // Cached BLAS built on first GetOrBuildBlas call. Storage shares the deferred-free path
  // (same shared_ptr deleter as element buffers) so it outlives any in-flight build.
  std::shared_ptr<RIBuffer_s> m_blasStorage;
  std::shared_ptr<RIAccelStructure_s> m_blas = {};

  // Signaled in the destructor; the surfel renderer connects a per-slot destroy
  // handler (owned by its bindless-slot cache) to it.
  Event<> m_onDestroyed;

  // Signaled on geometry rebuild (Compile / SubmitToGPU realloc). See
  // OnGeometryChanged() above.
  Event<> m_onGeometryChanged;

  friend struct VertexElement;
};
} // namespace hpl


#endif
