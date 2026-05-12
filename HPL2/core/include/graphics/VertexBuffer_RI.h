#ifndef HPL_DRAWPACKET_H
#define HPL_DRAWPACKET_H

#include <cassert>
#include <graphics/VertexBuffer.h>
#include <graphics/RITypes.h>
#include <graphics/GraphicsTypes.h>

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

  const std::shared_ptr<RIBuffer_s> &GetIndexRIBuffer() const { return m_indexBuffer; }

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
  uint32_t m_bufferSync = {};

  size_t m_indexBufferActiveCopy = 0;
  tVertexElementFlag m_updateFlags = 0; // update no need to rebuild buffers
  bool m_updateIndices = false;

  // Cached BLAS built on first GetOrBuildBlas call. Storage shares the deferred-free path
  // (same shared_ptr deleter as element buffers) so it outlives any in-flight build.
  std::shared_ptr<RIBuffer_s> m_blasStorage;
  RIAccelStructure_s m_blas = {};
  bool m_blasValid = false;

  friend struct VertexElement;
};
} // namespace hpl


#endif
