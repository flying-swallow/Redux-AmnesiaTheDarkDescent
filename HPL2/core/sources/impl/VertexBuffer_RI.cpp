/*
 * Copyright © 2009-2020 Frictional Games
 *
 * This file is part of Amnesia: The Dark Descent.
 *
 * Amnesia: The Dark Descent is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.

 * Amnesia: The Dark Descent is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Amnesia: The Dark Descent.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "graphics/VertexBuffer_RI.h"
#include "graphics/RIBootstrap.h"
#include "graphics/RIRenderer.h"
#include "graphics/RIResourceUploader.h"
#include "math/Math.h"
#include "system/LowLevelSystem.h"
#include "graphics/RIScratchAlloc.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace hpl {

size_t VertexBuffer_RI::GetSizeFromHPL(eVertexBufferElementFormat format) {
  switch (format) {
  case eVertexBufferElementFormat_Float:
    return sizeof(float);
  case eVertexBufferElementFormat_Int:
    return sizeof(uint32_t);
  case eVertexBufferElementFormat_Byte:
    return sizeof(char);
  default:
    break;
  }
  assert(false && "Unknown vertex attribute type.");
  return 0;
}

static const char* elementTypeName(eVertexBufferElement type) {
    switch (type) {
    case eVertexBufferElement_Position: return "Position";
    case eVertexBufferElement_Normal: return "Normal";
    case eVertexBufferElement_Color0: return "Color0";
    case eVertexBufferElement_Color1: return "Color1";
    case eVertexBufferElement_Texture0: return "Texture0";
    case eVertexBufferElement_Texture1Tangent: return "Texture1Tangent";
    case eVertexBufferElement_Texture1: return "Texture1";
    case eVertexBufferElement_Texture2: return "Texture2";
    case eVertexBufferElement_Texture3: return "Texture3";
    case eVertexBufferElement_Texture4: return "Texture4";
    case eVertexBufferElement_User0: return "User0";
    case eVertexBufferElement_User1: return "User1";
    case eVertexBufferElement_User2: return "User2";
    case eVertexBufferElement_User3: return "User3";
    default: return "Unknown";
    }
}

VertexBuffer_RI::VertexBuffer_RI(iLowLevelGraphics* apLowLevelGraphics,
			eVertexBufferType aType, 
			eVertexBufferDrawType aDrawType,eVertexBufferUsageType aUsageType,
			int alReserveVtxSize,int alReserveIdxSize) 
    : iVertexBuffer(apLowLevelGraphics, aType, aDrawType, aUsageType, alReserveVtxSize, alReserveIdxSize) {
}

VertexBuffer_RI::~VertexBuffer_RI() {
  AttachResourceToCntx(RI.GetActiveSet());
  m_onDestroyed.Signal();
}
void VertexBuffer_RI::Bind() {

} 

void VertexBuffer_RI::DrawIndices(	unsigned int *apIndices, int alCount,
								eVertexBufferDrawType aDrawType) {} 
	
void VertexBuffer_RI::CreateShadowDouble(bool abUpdateData) {}

void VertexBuffer_RI::PushVertexElements(
    std::span<const float> values, eVertexBufferElement elementType,
    std::span<VertexBuffer_RI::VertexElement> elements) {
  for (auto &element : elements) {
    if (element.type == elementType) {
      auto &buffer = element.m_shadowData;
      switch (element.format) {
      case eVertexBufferElementFormat_Float: {
        for (size_t i = 0; i < element.num; i++) {
          union {
            float f;
            unsigned char b[sizeof(float)];
          } entry = {i < values.size() ? values[i] : 0};
          buffer.insert(buffer.end(), std::begin(entry.b), std::end(entry.b));
        }
      } break;
      case eVertexBufferElementFormat_Int: {
        for (size_t i = 0; i < element.num; i++) {
          union {
            int f;
            unsigned char b[sizeof(int)];
          } entry = {i < values.size() ? static_cast<int>(values[i]) : 0};
          buffer.insert(buffer.end(), std::begin(entry.b), std::end(entry.b));
        }
      } break;
      case eVertexBufferElementFormat_Byte: {
        for (size_t i = 0; i < element.num; i++) {
          buffer.emplace_back(i < values.size() ? static_cast<char>(values[i])
                                                : 0);
        }
      } break;
      default:
        break;
      }
      return;
    }
  }
}

size_t VertexBuffer_RI::VertexElement::Stride() const {
  return GetSizeFromHPL(format) * num;
}

size_t VertexBuffer_RI::VertexElement::NumElements() const {
  return m_shadowData.size() / Stride();
}

void VertexBuffer_RI::AddVertexVec3f(eVertexBufferElement aElement,
                                     const cVector3f &avVtx) {
  m_updateFlags |= GetVertexElementFlagFromEnum(aElement);
  PushVertexElements(
      std::span(std::begin(avVtx.v), std::end(avVtx.v)), aElement,
      std::span(m_vertexElements.begin(), m_vertexElements.end()));
}

void VertexBuffer_RI::AddVertexVec4f(eVertexBufferElement aElement,
                                     const cVector3f &avVtx, float afW) {
  m_updateFlags |= GetVertexElementFlagFromEnum(aElement);
  PushVertexElements(
      std::span(std::begin(avVtx.v), std::end(avVtx.v)), aElement,
      std::span(m_vertexElements.begin(), m_vertexElements.end()));
}

void VertexBuffer_RI::AddVertexColor(eVertexBufferElement aElement,
                                     const cColor &aColor) {
  m_updateFlags |= GetVertexElementFlagFromEnum(aElement);
  PushVertexElements(
      std::span(std::begin(aColor.v), std::end(aColor.v)), aElement,
      std::span(m_vertexElements.begin(), m_vertexElements.end()));
}

void VertexBuffer_RI::AddIndex(unsigned int alIndex) {
  m_updateIndices = true;
  m_indices.push_back(alIndex);
}

void VertexBuffer_RI::Transform(const cMatrixf &mtxTransform) {
  cMatrixf mtxRot = mtxTransform.GetRotation();
  cMatrixf mtxNormalRot = cMath::MatrixInverse(mtxRot).GetTranspose();

  ///////////////
  // Get position
  auto positionElement =
      std::find_if(m_vertexElements.begin(), m_vertexElements.end(),
                   [](const auto &element) {
                     return element.type == eVertexBufferElement_Position;
                   });
  if (positionElement != m_vertexElements.end()) {
    assert(positionElement->format == eVertexBufferElementFormat_Float &&
           "Only float format supported");
    assert(positionElement->num >= 3 && "Only 3 component format supported");
    struct PackedVec3 {
      float x;
      float y;
      float z;
    };
    for (size_t i = 0; i < positionElement->NumElements(); i++) {
      auto &position = positionElement->GetElement<PackedVec3>(i);
      cVector3f outputPos = cMath::MatrixMul(
          mtxTransform, cVector3f(position.x, position.y, position.z));
      position = {outputPos.x, outputPos.y, outputPos.z};
    }
  }

  auto normalElement =
      std::find_if(m_vertexElements.begin(), m_vertexElements.end(),
                   [](const auto &element) {
                     return element.type == eVertexBufferElement_Normal;
                   });
  if (normalElement != m_vertexElements.end()) {
    assert(normalElement->format == eVertexBufferElementFormat_Float &&
           "Only float format supported");
    assert(normalElement->num >= 3 && "Only 3 component format supported");
    struct PackedVec3 {
      float x;
      float y;
      float z;
    };
    for (size_t i = 0; i < normalElement->NumElements(); i++) {
      auto &normal = normalElement->GetElement<PackedVec3>(i);
      cVector3f outputNormal = cMath::MatrixMul(
          mtxNormalRot, cVector3f(normal.x, normal.y, normal.z));
      normal = {outputNormal.x, outputNormal.y, outputNormal.z};
    }
  }

  auto tangentElement = std::find_if(
      m_vertexElements.begin(), m_vertexElements.end(),
      [](const auto &element) {
        return element.type == eVertexBufferElement_Texture1Tangent;
      });
  if (tangentElement != m_vertexElements.end()) {
    assert(tangentElement->format == eVertexBufferElementFormat_Float &&
           "Only float format supported");
    assert(tangentElement->num >= 3 && "Only 4 component format supported");
    struct PackedVec3 {
      float x;
      float y;
      float z;
    };
    for (size_t i = 0; i < normalElement->NumElements(); i++) {
      auto &tangent = tangentElement->GetElement<PackedVec3>(i);
      cVector3f outputTangent =
          cMath::MatrixMul(mtxRot, cVector3f(tangent.x, tangent.y, tangent.z));
      tangent = {outputTangent.x, outputTangent.y, outputTangent.z};
    }
  }

  // ////////////////////////////
  // //Update the data
  tVertexElementFlag vtxFlag = eVertexElementFlag_Position;
  if (normalElement != m_vertexElements.end()) {
    vtxFlag |= eVertexElementFlag_Normal;
  }
  if (tangentElement != m_vertexElements.end()) {
    vtxFlag |= eVertexElementFlag_Texture1;
  }

  UpdateData(vtxFlag, false);
}

int VertexBuffer_RI::GetElementNum(eVertexBufferElement aElement) {
  auto element = std::find_if(
      m_vertexElements.begin(), m_vertexElements.end(),
      [aElement](const auto &element) { return element.type == aElement; });
  if (element != m_vertexElements.end()) {
    return element->num;
  }
  return 0;
}

eVertexBufferElementFormat
VertexBuffer_RI::GetElementFormat(eVertexBufferElement aElement) {
  auto element = std::find_if(
      m_vertexElements.begin(), m_vertexElements.end(),
      [aElement](const auto &element) { return element.type == aElement; });
  if (element != m_vertexElements.end()) {
    return element->format;
  }
  return eVertexBufferElementFormat_LastEnum;
}

int VertexBuffer_RI::GetElementProgramVarIndex(eVertexBufferElement aElement) {
  auto element = std::find_if(
      m_vertexElements.begin(), m_vertexElements.end(),
      [aElement](const auto &element) { return element.type == aElement; });
  if (element != m_vertexElements.end()) {
    return element->programVarIndex;
  }
  return 0;
}

bool VertexBuffer_RI::Compile(tVertexCompileFlag aFlags) {
  if (aFlags & eVertexCompileFlag_CreateTangents) {
    CreateElementArray(eVertexBufferElement_Texture1Tangent,
                       eVertexBufferElementFormat_Float, 4);
    auto positionElement =
        std::find_if(m_vertexElements.begin(), m_vertexElements.end(),
                     [](const auto &element) {
                       return element.type == eVertexBufferElement_Position;
                     });
    auto normalElement =
        std::find_if(m_vertexElements.begin(), m_vertexElements.end(),
                     [](const auto &element) {
                       return element.type == eVertexBufferElement_Normal;
                     });
    auto textureElement =
        std::find_if(m_vertexElements.begin(), m_vertexElements.end(),
                     [](const auto &element) {
                       return element.type == eVertexBufferElement_Texture0;
                     });
    auto tangentElement = std::find_if(
        m_vertexElements.begin(), m_vertexElements.end(),
        [](const auto &element) {
          return element.type == eVertexBufferElement_Texture1Tangent;
        });

    assert(positionElement != m_vertexElements.end() &&
           "No position element found");
    assert(normalElement != m_vertexElements.end() &&
           "No normal element found");
    assert(textureElement != m_vertexElements.end() &&
           "No texture element found");
    assert(tangentElement != m_vertexElements.end() &&
           "No tangent element found");
    assert(positionElement->format == eVertexBufferElementFormat_Float &&
           "Only float format supported");
    assert(normalElement->format == eVertexBufferElementFormat_Float &&
           "Only float format supported");
    assert(textureElement->format == eVertexBufferElementFormat_Float &&
           "Only float format supported");
    assert(tangentElement->format == eVertexBufferElementFormat_Float &&
           "Only float format supported");
    assert(positionElement->num >= 3 && "Only 3 component format supported");
    assert(normalElement->num >= 3 && "Only 3 component format supported");
    assert(textureElement->num >= 2 && "Only 2 component format supported");
    assert(tangentElement->num >= 4 && "Only 4 component format supported");

    ResizeArray(eVertexBufferElement_Texture1Tangent, GetVertexNum() * 4);

    cMath::CreateTriTangentVectors(
        reinterpret_cast<float *>(tangentElement->m_shadowData.data()),
        m_indices.data(), m_indices.size(),
        reinterpret_cast<float *>(positionElement->m_shadowData.data()),
        positionElement->num,
        reinterpret_cast<float *>(textureElement->m_shadowData.data()),
        reinterpret_cast<float *>(normalElement->m_shadowData.data()),
        positionElement->NumElements());
  }
  //// Shared deleter: route through the active frame's deferred-free queue so
  //// the VkBuffer outlives any in-flight upload / draw command buffer that
  //// still references it. The buffer + allocation get destroyed when the set
  //// rotates back around (i.e. RI_NUMBER_FRAMES_FLIGHT frames later).
  //auto destroyBuffer = [](RIBuffer_s *b) {
  //  if (b->vk.buffer) {
  //    auto *cntx = RI.GetActiveSet();
  //    cntx->freelist.push_back(RIFree(b->vk.buffer));
  //    cntx->freelist.push_back(RIFree(b->vk.allocation));
  //  }
  //  delete b;
  //};

  //VmaAllocator allocator = RI.device.vk.vmaAllocator;

  //auto elementTypeName = [](eVertexBufferElement type) -> const char * {
  //  switch (type) {
  //  case eVertexBufferElement_Position: return "Position";
  //  case eVertexBufferElement_Normal: return "Normal";
  //  case eVertexBufferElement_Color0: return "Color0";
  //  case eVertexBufferElement_Color1: return "Color1";
  //  case eVertexBufferElement_Texture0: return "Texture0";
  //  case eVertexBufferElement_Texture1Tangent: return "Texture1Tangent";
  //  case eVertexBufferElement_Texture1: return "Texture1";
  //  case eVertexBufferElement_Texture2: return "Texture2";
  //  case eVertexBufferElement_Texture3: return "Texture3";
  //  case eVertexBufferElement_Texture4: return "Texture4";
  //  case eVertexBufferElement_User0: return "User0";
  //  case eVertexBufferElement_User1: return "User1";
  //  case eVertexBufferElement_User2: return "User2";
  //  case eVertexBufferElement_User3: return "User3";
  //  default: return "Unknown";
  //  }
  //};

  //auto uploadBuffer = [&](VkDeviceSize size, const void *src,
  //                        VkBufferUsageFlags usage,
  //                        VkPipelineStageFlags2 postStage,
  //                        VkAccessFlags2 postAccess,
  //                        const char *label)
  //    -> std::shared_ptr<RIBuffer_s> {
  //  std::shared_ptr<RIBuffer_s> buf(new RIBuffer_s(), destroyBuffer);

  //  uint32_t queueFamilies[RI_QUEUE_LEN] = {0};
  //  VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  //  VK_ConfigureBufferQueueFamilies(&bci, RI.device.queues, RI_QUEUE_LEN,
  //                                  queueFamilies, RI_QUEUE_LEN);
  //  bci.size = size;
  //  bci.usage = usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
  //              VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

  //  VmaAllocationCreateInfo aci = {};
  //  aci.usage = VMA_MEMORY_USAGE_AUTO;
  //  VK_WrapResult(vmaCreateBuffer(allocator, &bci, &aci, &buf->vk.buffer,
  //                                &buf->vk.allocation, nullptr));

  //  if (vkSetDebugUtilsObjectNameEXT) {
  //    char debugName[128];
  //    std::snprintf(debugName, sizeof(debugName), "VB[%p]:%s",
  //                  static_cast<void *>(this), label ? label : "Unknown");
  //    VkDebugUtilsObjectNameInfoEXT nameInfo = {
  //        VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT, NULL,
  //        VK_OBJECT_TYPE_BUFFER, (uint64_t)buf->vk.buffer, debugName};
  //    VK_WrapResult(
  //        vkSetDebugUtilsObjectNameEXT(RI.device.vk.device, &nameInfo));
  //  }

  //  RIResourceBufferTransaction_s trans = {};
  //  trans.target = *buf;
  //  trans.size = size;
  //  trans.offset = 0;
  //  trans.vk.current_stage = VK_PIPELINE_STAGE_2_NONE;
  //  trans.vk.current_access = VK_ACCESS_2_NONE;
  //  trans.vk.post_stage = postStage;
  //  trans.vk.post_access = postAccess;
  //  RI_ResourceBeginCopyBuffer(&RI.device, &RI.uploader, &trans);
  //  std::memcpy(trans.mapped.data, src, size);
  //  RI_ResourceEndCopyBuffer(&RI.device, &RI.uploader, &trans);
  //  
  //  auto *cntx = RI.GetActiveSet();
  //  cntx->bufferLink.push_back(buf);

  //  return buf;
  //};

  //// Position and index buffers double as BLAS build inputs — ray tracing is
  //// required.
  //for (auto &element : m_vertexElements) {
  //  if (element.m_shadowData.empty())
  //    continue;
  //  VkBufferUsageFlags usage =
  //      VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  //  if (element.type == eVertexBufferElement_Position) {
  //    usage |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
  //  }
  //  element.buffer = uploadBuffer(
  //      element.m_shadowData.size(), element.m_shadowData.data(), usage,
  //      VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT,
  //      elementTypeName(element.type));
  //}

  //if (!m_indices.empty()) {
  //  m_indexBuffer = uploadBuffer(
  //      m_indices.size() * sizeof(uint32_t), m_indices.data(),
  //      VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
  //          VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
  //      VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT,
  //      "Index");
  //}

  m_generation++;
  m_updateFlags = 0;
  m_updateIndices = false;
  // Topology rebuilt: tell the renderer so it bumps the reuse generation of any
  // bindless slot anchoring surfels to this VB (see OnGeometryChanged()).
  m_onGeometryChanged.Signal();
  return true;
}

void VertexBuffer_RI::SubmitToGPU(RICmd_s *cmd, RIDevice_s *device,
                                    RIBootstrap::FrameContext *cntx) {
  assert(cmd);
  assert(device);
  assert(cntx);

  if(m_generation == m_lastSubmitted) {
    return;
  }

  // Allocate a fresh GPU buffer. Used only on the first submit, on shadow-data
  // growth (ResizeArray / ResizeIndices), or when a CreateCopy'd element still
  // points at its source buffer with a zero capacity tag.
  auto allocBuffer = [&](VkDeviceSize size, VkBufferUsageFlags usage,
                         const char *label) -> std::shared_ptr<RIBuffer_s> {
    std::shared_ptr<RIBuffer_s> buf(new RIBuffer_s(), [](RIBuffer_s *b) {
      b->dispose(&RI.device);
      delete b;
    });

    uint32_t queueFamilies[RI_QUEUE_LEN] = {0};
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    VK_ConfigureBufferQueueFamilies(&bci, RI.device.queues, RI_QUEUE_LEN,
                                    queueFamilies, RI_QUEUE_LEN);
    bci.size = size;
    bci.usage = usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

    VmaAllocationCreateInfo aci = {};
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    *buf = RIBuffer_s::VK_createFromVMA(device, &bci, &aci);

    char debugName[128];
    std::snprintf(debugName, sizeof(debugName), "VB[%p]:%s",
                  static_cast<void *>(this), label ? label : "Unknown");
    buf->setDebugObjectName(device, debugName);
    return buf;
  };

  // Stage-copy `src` into an existing buffer. The transaction takes a value
  // copy of the RIBuffer_s, but the underlying VkBuffer / VmaAllocation handles
  // are shared, so the upload lands in the same GPU memory.
  auto stageUpload = [&](RIBuffer_s *target, VkDeviceSize size, const void *src,
                         VkPipelineStageFlags2 postStage,
                         VkAccessFlags2 postAccess) {
    RIResourceBufferTransaction_s trans = {};
    trans.target = *target;
    trans.size = size;
    trans.offset = 0;
    trans.vk.current_stage = VK_PIPELINE_STAGE_2_NONE;
    trans.vk.current_access = VK_ACCESS_2_NONE;
    trans.vk.post_stage = postStage;
    trans.vk.post_access = postAccess;
    RI_ResourceBeginCopyBuffer(&RI.device, &RI.uploader, &trans);
    std::memcpy(trans.mapped.data, src, size);
    RI_ResourceEndCopyBuffer(&RI.device, &RI.uploader, &trans);
  };

  // Set when any stream gets a fresh VkBuffer below (first submit, shadow-data
  // growth, CreateCopy sentinel). A realloc moves the buffer-device-address, so
  // any surfel anchored here via the old BDA / triangle count is now stale; we
  // signal OnGeometryChanged once after the uploads so the renderer bumps the
  // slot generation.
  bool reallocated = false;

  for (auto &element : m_vertexElements) {
    if (element.m_shadowData.empty())
      continue;
    const size_t needed = element.m_shadowData.size();
    VkBufferUsageFlags usage =
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if (element.type == eVertexBufferElement_Position) {
      usage |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
    }
    // m_internalBufferSize == 0 with element.buffer set is the CreateCopy
    // sentinel: the shared_ptr was inherited from the source VB but we own no
    // GPU storage of our own yet. Treat it as "needs allocation."
    const bool needsAlloc = !element.buffer ||
                            element.m_internalBufferSize == 0 ||
                            element.m_internalBufferSize < needed;
    if (needsAlloc) {
      element.buffer = allocBuffer(needed, usage, elementTypeName(element.type));
      element.m_internalBufferSize = needed;
      reallocated = true;
    }
    // Always upload on (re)alloc; otherwise only when the caller flagged this
    // stream dirty via UpdateData().
    if (needsAlloc || (m_updateFlags & element.flag)) {
      stageUpload(element.buffer.get(), needed, element.m_shadowData.data(),
                  VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
                  VK_ACCESS_2_SHADER_READ_BIT);
    }
  }

  if (!m_indices.empty()) {
    const size_t needed = m_indices.size() * sizeof(uint32_t);
    const VkBufferUsageFlags idxUsage =
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
    const bool needsAlloc = !m_indexBuffer || m_indexBufferCapacity < needed;
    if (needsAlloc) {
      m_indexBuffer = allocBuffer(needed, idxUsage, "Index");
      m_indexBufferCapacity = needed;
      reallocated = true;
    }
    if (needsAlloc || m_updateIndices) {
      stageUpload(m_indexBuffer.get(), needed, m_indices.data(),
                  VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
                  VK_ACCESS_2_SHADER_READ_BIT);
    }
  }
  // A realloc already moved the BDAs above, so notify regardless of whether the
  // BLAS rebuild below proceeds — surfels anchored to the old address must be
  // invalidated either way.
  if (reallocated)
    m_onGeometryChanged.Signal();

  // Validate inputs BEFORE tearing down the cached BLAS. If we exit here we
  // want the previously-built BLAS to remain valid so the object stays in the
  // TLAS — destroying it first would silently drop the object from ray-traced
  // shadows until a future successful rebuild.
  const VertexElement *position = GetElement(eVertexBufferElement_Position);
  const uint32_t vertexNum =
      position ? static_cast<uint32_t>(position->NumElements()) : 0;
  const uint32_t indexNum = static_cast<uint32_t>(m_indices.size());
  if (!position || !position->buffer || !m_indexBuffer || vertexNum == 0 ||
      indexNum < 3) {
    return;
  }

  m_blas.reset();
  m_blasStorage.reset();

  RIAccelGeometryDesc_s geom = {};
  geom.type = RI_ACCEL_GEOMETRY_TYPE_TRIANGLES;
  // Non-opaque: candidate triangle hits are reported to the ray-query proceed
  // loop in traceray_rq.glsl, which runs the alpha-reject test that mirrors
  // gbuffer.frag:28-30. Marking the BLAS opaque would let the driver auto-
  // commit hits and bypass the shader filter entirely, so shadow/surfel rays
  // would get blocked by alpha-cutout texels the gbuffer already discarded.
  geom.flags = 0;
  geom.triangles.vertexBuffer = position->buffer.get();
  geom.triangles.vertexOffset = 0;
  geom.triangles.vertexNum = vertexNum;
  geom.triangles.vertexStride = static_cast<uint16_t>(position->Stride());
  geom.triangles.vertexFormat = RI_FORMAT_RGB32_SFLOAT;
  geom.triangles.indexBuffer = m_indexBuffer.get();
  geom.triangles.indexOffset = 0;
  geom.triangles.indexNum = indexNum;
  geom.triangles.indexType = RI_INDEX_TYPE_32;
  geom.triangles.transformBuffer = nullptr;
  geom.triangles.transformOffset = 0;

  RIAccelStructureDesc_s asDesc = {};
  asDesc.type = RI_ACCEL_STRUCTURE_TYPE_BOTTOM_LEVEL;
  asDesc.flags = RI_ACCEL_BUILD_PREFER_FAST_TRACE;
  asDesc.geometryOrInstanceNum = 1;
  asDesc.geometries = &geom;

  uint64_t storageSize = 0;
  uint64_t buildScratchSize = 0;
  GetRIAccelStructureMemoryReqs(device, &asDesc, &storageSize, &buildScratchSize, NULL );

  m_blasStorage = std::shared_ptr<RIBuffer_s>(
      new RIBuffer_s(),
      [](RIBuffer_s *b) { b->dispose(&RI.device); delete b; });
  {
    uint32_t queueFamilies[RI_QUEUE_LEN] = {0};
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    VK_ConfigureBufferQueueFamilies(&bci, device->queues, RI_QUEUE_LEN,
                                    queueFamilies, RI_QUEUE_LEN);
    bci.size = storageSize;
    bci.usage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    VmaAllocationCreateInfo aci = {};
    aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    *m_blasStorage = RIBuffer_s::VK_createFromVMA(device, &bci, &aci);
  }

  char dbgName[128];
  std::snprintf(dbgName, sizeof(dbgName), "VB[%p]:BlasStorage",
                static_cast<void *>(this));
  m_blasStorage->setDebugObjectName(device, dbgName);

  asDesc.storage = m_blasStorage.get();
  asDesc.storageOffset = 0;
  asDesc.storageSize = storageSize;

  m_blas = std::shared_ptr<RIAccelStructure_s>(
      new RIAccelStructure_s(),
      [](RIAccelStructure_s *a) { a->dispose(&RI.device); delete a; });
  if (InitRIAccelStructure(device, &asDesc, m_blas.get()) != RI_SUCCESS) {
    Error("failed to construct acceel structure");
  }

  struct RIBufferScratchAllocReq_s scratchReq = RIAllocBufferFromScratchAlloc(
      device, &cntx->accelScratchAlloc, buildScratchSize);
  RIBuildBlasDesc_s buildDesc = {};
  buildDesc.dst = m_blas.get();
  buildDesc.src = nullptr;
  buildDesc.mode = RI_ACCEL_BUILD_MODE_BUILD;
  buildDesc.geometries = &geom;
  buildDesc.geometryNum = 1;
  buildDesc.scratchOffset = scratchReq.bufferOffset;
  buildDesc.scratchBuffer = &scratchReq.block.buffer;
  CmdBuildRIBlas(device, cmd, &buildDesc, 1);

  // Dirty flags only drive the *partial* upload path above; once we've fully
  // re-uploaded and rebuilt the BLAS, clear them so the next submit (after a
  // future UpdateData) doesn't re-upload streams that weren't touched.
  m_updateFlags = 0;
  m_updateIndices = false;
  m_lastSubmitted = m_generation;
  return;
}

void VertexBuffer_RI::AttachResourceToCntx(RIBootstrap::FrameContext *cntx) {

  for (auto &element : m_vertexElements) {
    if(element.buffer) {
      cntx->bufferLink.push_back(element.buffer);
    }
  }
  if(m_indexBuffer) {
    cntx->bufferLink.push_back(m_indexBuffer);
  }
  if(m_blas) {
    cntx->bufferLink.push_back(m_blasStorage);
    cntx->accelLink.push_back(m_blas);
  }
}

void VertexBuffer_RI::UpdateData(tVertexElementFlag aTypes, bool abIndices) {
  m_updateFlags |= aTypes;
  m_updateIndices |= abIndices;
  // Without this bump, SubmitToGPU short-circuits and the GPU buffers / BLAS
  // stay frozen at the last submitted state — silently breaking ray-traced
  // shadows for skinned meshes that re-skin into m_shadowData each frame.
  m_generation++;
}

float *VertexBuffer_RI::GetFloatArray(eVertexBufferElement aElement) {
  auto element = std::find_if(
      m_vertexElements.begin(), m_vertexElements.end(),
      [aElement](const auto &element) { return element.type == aElement; });
  if (element != m_vertexElements.end()) {
    return reinterpret_cast<float *>(element->m_shadowData.data());
  }
  return nullptr;
}

int *VertexBuffer_RI::GetIntArray(eVertexBufferElement aElement) {
  auto element = std::find_if(
      m_vertexElements.begin(), m_vertexElements.end(),
      [aElement](const auto &element) { return element.type == aElement; });
  if (element != m_vertexElements.end()) {
    return reinterpret_cast<int *>(element->m_shadowData.data());
  }
  return nullptr;
}

unsigned char *VertexBuffer_RI::GetByteArray(eVertexBufferElement aElement) {
  auto element = std::find_if(
      m_vertexElements.begin(), m_vertexElements.end(),
      [aElement](const auto &element) { return element.type == aElement; });
  if (element != m_vertexElements.end()) {
    return element->m_shadowData.data();
  }
  return nullptr;
}

unsigned int *VertexBuffer_RI::GetIndices() { return m_indices.data(); }

void VertexBuffer_RI::ResizeArray(eVertexBufferElement aElement, int alSize) {
  auto element = std::find_if(
      m_vertexElements.begin(), m_vertexElements.end(),
      [aElement](const auto &element) { return element.type == aElement; });
  if (element != m_vertexElements.end()) {
    m_updateFlags |= element->flag;
    element->m_shadowData.resize(alSize * GetSizeFromHPL(element->format));
    m_generation++;
  }
}

void VertexBuffer_RI::ResizeIndices(int alSize) {
  m_updateIndices = true;
  m_indices.resize(alSize);
  m_generation++;
}

const VertexBuffer_RI::VertexElement *
VertexBuffer_RI::GetElement(eVertexBufferElement elementType) {
  auto element = std::find_if(m_vertexElements.begin(), m_vertexElements.end(),
                              [elementType](const auto &element) {
                                return element.type == elementType;
                              });
  if (element != m_vertexElements.end()) {
    return &(*element);
  }
  return nullptr;
}

RIAccelStructure_s *VertexBuffer_RI::GetOrBuildBlas(RIDevice_s *device,
                                                    RICmd_s *cmd) {
  //if (m_blasValid)
  //  return &m_blas;
  //if (!device || !cmd)
  //  return nullptr;

  //const VertexElement *position = GetElement(eVertexBufferElement_Position);
  //if (!position || !position->buffer || !m_indexBuffer)
  //  return nullptr;
  //if (position->buffer->vk.buffer == VK_NULL_HANDLE ||
  //    m_indexBuffer->vk.buffer == VK_NULL_HANDLE)
  //  return nullptr;

  //const uint32_t vertexNum = static_cast<uint32_t>(position->NumElements());
  //const uint32_t indexNum = static_cast<uint32_t>(m_indices.size());
  //if (vertexNum == 0 || indexNum < 3)
  //  return nullptr;

  //// 1. Describe geometry. RGB32 sfloat is HPL's only position format; index buffer is
  ////    always uint32_t (see Compile()).
  //RIAccelGeometryDesc_s geom = {};
  //geom.type = RI_ACCEL_GEOMETRY_TYPE_TRIANGLES;
  //geom.flags = RI_ACCEL_GEOMETRY_OPAQUE;
  //geom.triangles.vertexBuffer = position->buffer.get();
  //geom.triangles.vertexOffset = 0;
  //geom.triangles.vertexNum = vertexNum;
  //geom.triangles.vertexStride = static_cast<uint16_t>(position->Stride());
  //geom.triangles.vertexFormat = RI_FORMAT_RGB32_SFLOAT;
  //geom.triangles.indexBuffer = m_indexBuffer.get();
  //geom.triangles.indexOffset = 0;
  //geom.triangles.indexNum = indexNum;
  //geom.triangles.indexType = RI_INDEX_TYPE_32;
  //geom.triangles.transformBuffer = nullptr;
  //geom.triangles.transformOffset = 0;

  //RIAccelStructureDesc_s asDesc = {};
  //asDesc.type = RI_ACCEL_STRUCTURE_TYPE_BOTTOM_LEVEL;
  //asDesc.flags = RI_ACCEL_BUILD_PREFER_FAST_TRACE;
  //asDesc.geometryOrInstanceNum = 1;
  //asDesc.geometries = &geom;

  //uint64_t storageSize = 0;
  //uint64_t buildScratchSize = 0;
  //GetRIAccelStructureMemoryReqs(device, &asDesc, &storageSize, &buildScratchSize,
  //                              nullptr);
  //if (storageSize == 0 || buildScratchSize == 0)
  //  return nullptr;

  //// 2. Allocate AS storage and scratch buffers. Both share the deferred-free deleter so
  ////    they outlive any in-flight cmd that still references them.
  //auto destroyBuffer = [](RIBuffer_s *b) {
  //  if (b->vk.buffer) {
  //    auto *cntx = RI.GetActiveSet();
  //    cntx->freelist.push_back(RIFree(b->vk.buffer));
  //    cntx->freelist.push_back(RIFree(b->vk.allocation));
  //  }
  //  delete b;
  //};

  //auto createDeviceBuffer = [&](VkDeviceSize size, VkBufferUsageFlags usage,
  //                              const char *label) -> std::shared_ptr<RIBuffer_s> {
  //  std::shared_ptr<RIBuffer_s> buf(new RIBuffer_s(), destroyBuffer);
  //  uint32_t queueFamilies[RI_QUEUE_LEN] = {0};
  //  VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  //  VK_ConfigureBufferQueueFamilies(&bci, device->queues, RI_QUEUE_LEN,
  //                                  queueFamilies, RI_QUEUE_LEN);
  //  bci.size = size;
  //  bci.usage = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
  //  VmaAllocationCreateInfo aci = {};
  //  aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
  //  VK_WrapResult(vmaCreateBuffer(device->vk.vmaAllocator, &bci, &aci,
  //                                &buf->vk.buffer, &buf->vk.allocation, nullptr));
  //  
  //  char dbg[128];
  //  std::snprintf(dbg, sizeof(dbg), "VB[%p]:%s", static_cast<void *>(this),
  //                label);
  //  buf->setDebugObjectName(&RI.device, dbg);
  //  return buf;
  //};

  //m_blasStorage = createDeviceBuffer(
  //    storageSize, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR,
  //    "BlasStorage");

  //std::shared_ptr<RIBuffer_s> scratch = createDeviceBuffer(
  //    buildScratchSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "BlasScratch");

  //// 3. Create the BLAS object on top of the storage buffer.
  //asDesc.storage = m_blasStorage.get();
  //asDesc.storageOffset = 0;
  //asDesc.storageSize = storageSize;
  //if (InitRIAccelStructure(device, &asDesc, &m_blas) != RI_SUCCESS) {
  //  m_blasStorage.reset();
  //  return nullptr;
  //}

  //// 4. Record the build. The scratch buffer's lifetime is extended by capturing it on the
  ////    active frame's freelist via its deleter — it'll be destroyed once frames-in-flight
  ////    rotate past this submission.
  //RIBuildBlasDesc_s buildDesc = {};
  //buildDesc.dst = &m_blas;
  //buildDesc.src = nullptr;
  //buildDesc.mode = RI_ACCEL_BUILD_MODE_BUILD;
  //buildDesc.geometries = &geom;
  //buildDesc.geometryNum = 1;
  //buildDesc.scratchBuffer = scratch.get();
  //buildDesc.scratchOffset = 0;
  //CmdBuildRIBlas(device, cmd, &buildDesc, 1);

  //// scratch ref-count drops here; the shared_ptr deleter queues it for deferred free.
  //m_blasValid = true;
  return NULL;
}

void VertexBuffer_RI::CreateElementArray(eVertexBufferElement aType,
                                         eVertexBufferElementFormat aFormat,
                                         int alElementNum,
                                         int alProgramVarIndex) {
  tVertexElementFlag elementFlag = GetVertexElementFlagFromEnum(aType);
  if (elementFlag & mVertexFlags) {
    Error("Vertex element of type %d already present in buffer %d!\n", aType,
          this);
    return;
  }
  mVertexFlags |= elementFlag;

  VertexElement element;
  element.type = aType;
  element.flag = elementFlag;
  element.format = aFormat;
  element.num = alElementNum;
  element.programVarIndex = alProgramVarIndex;
  m_vertexElements.push_back(std::move(element));
}

int VertexBuffer_RI::GetVertexNum() {
  auto positionElement =
      std::find_if(m_vertexElements.begin(), m_vertexElements.end(),
                   [](const auto &element) {
                     return element.type == eVertexBufferElement_Position;
                   });
  assert(positionElement != m_vertexElements.end() &&
         "No position element found");
  return positionElement->NumElements();
}

int VertexBuffer_RI::GetIndexNum() { return m_indices.size(); }

cBoundingVolume VertexBuffer_RI::CreateBoundingVolume() {
  cBoundingVolume bv;
  if ((mVertexFlags & eVertexElementFlag_Position) == 0) {
    Warning("Could not create bounding volume from buffer %d  because no "
            "position element was present!\n",
            this);
    return bv;
  }

  auto positionElement =
      std::find_if(m_vertexElements.begin(), m_vertexElements.end(),
                   [](const auto &element) {
                     return element.type == eVertexBufferElement_Position;
                   });

  if (positionElement == m_vertexElements.end()) {
    return bv;
  }

  if (positionElement->format != eVertexBufferElementFormat_Float) {
    Warning("Could not breate bounding volume since postion was not for format "
            "float in buffer %d!\n",
            this);
    return bv;
  }

  bv.AddArrayPoints(
      reinterpret_cast<float *>(positionElement->m_shadowData.data()),
      GetVertexNum());
  bv.CreateFromPoints(positionElement->num);

  return bv;
}

void VertexBuffer_RI::Draw(eVertexBufferDrawType aDrawType) {}

// DrawPacket VertexBuffer_RI::resolveGeometryBinding(
//     uint32_t frameIndex, std::span<eVertexBufferElement> elements) {
//     DrawPacket packet;
//     packet.m_type = DrawPacket::DrawIndvidualBuffers;
//     if(m_updateFlags) {
//         for (auto& element : m_vertexElements) {
//             const bool isDynamicAccess = detail::IsDynamicMemory(mUsageType);
//             const bool isUpdate = (m_updateFlags & element.m_flag) > 0;
//             if (isUpdate) {
//                 element.m_activeCopy = isDynamicAccess ?
//                 ((element.m_activeCopy + 1) % ForgeRenderer::SwapChainLength)
//                 : 0; size_t minimumSize = element.m_shadowData.size() *
//                 (isDynamicAccess ? ForgeRenderer::SwapChainLength: 1); if
//                 (!isDynamicAccess || elemenm_programVarIndex t.m_buffer.m_handle == nullptr ||
//                 element.m_buffer.m_handle->mSize > minimumSize) {
//                     // wait for any buffers updating before creating a new
//                     buffer waitForToken(&m_bufferSync);
//                     element.m_buffer.Load([&](Buffer** buffer) {
//                         BufferLoadDesc loadDesc = {};
//                         loadDesc.ppBuffer = buffer;
//                         loadDesc.mDesc.mDescriptors =
//                         DESCRIPTOR_TYPE_VERTEX_BUFFER;
//                         loadDesc.mDesc.mMemoryUsage =
//                         detail::toMemoryUsage(mUsageType);
//                         loadDesc.mDesc.mSize = minimumSize;
//                         if (detail::IsDynamicMemory(mUsageType)) {
//                             loadDesc.mDesc.mFlags =
//                             BUFFER_CREATION_FLAG_PERSISTENT_MAP_BIT;
//                         } else {
//                             loadDesc.pData = element.m_shadowData.data();
//                         }
//                         addResource(&loadDesc, &m_bufferSync);
//                         return true;
//                     });
//                 }
//                 if (isDynamicAccess) {
//                     ASSERT(element.m_buffer.IsValid() && "Buffer not
//                     initialized"); BufferUpdateDesc updateDesc = {
//                     element.m_buffer.m_handle }; updateDesc.mSize =
//                     element.m_shadowData.size(); updateDesc.mDstOffset =
//                     element.m_activeCopy * updateDesc.mSize;
//                     beginUpdateResource(&updateDesc);
//                     std::copy(element.m_shadowData.begin(),
//                     element.m_shadowData.end(),
//                     reinterpret_cast<uint8_t*>(updateDesc.pMappedData));
//                     endUpdateResource(&updateDesc, &m_bufferSync);
//                 }
//             }
//         }
//         m_updateFlags = 0;
//     }
//     if (m_updateIndices) {
//         const bool isDynamicAccess = detail::IsDynamicMemory(mUsageType);
//         size_t minimumSize = m_indices.size() * (isDynamicAccess ?
//         ForgeRenderer::SwapChainLength : 1) * sizeof(uint32_t); if
//         (!isDynamicAccess || m_indexBuffer.m_handle == nullptr ||
//         m_indexBuffer.m_handle->mSize > minimumSize) {
//             waitForToken(&m_bufferSync);
//             m_indexBuffer.Load([&](Buffer** buffer) {
//                 BufferLoadDesc loadDesc = {};
//                 loadDesc.ppBuffer = &m_indexBuffer.m_handle;
//                 loadDesc.mDesc.mDescriptors = DESCRIPTOR_TYPE_INDEX_BUFFER;
//                 loadDesc.mDesc.mMemoryUsage =
//                 detail::toMemoryUsage(mUsageType); loadDesc.mDesc.mSize =
//                 minimumSize; if (isDynamicAccess) {
//                     loadDesc.mDesc.mFlags =
//                     BUFFER_CREATION_FLAG_PERSISTENT_MAP_BIT;
//                 } else {
//                     loadDesc.pData = m_indices.data();
//                 }
//                 addResource(&loadDesc, &m_bufferSync);
//                 return true;
//             });
//         }
//         if (isDynamicAccess) {
//             ASSERT(m_indexBuffer.IsValid() && "Buffer not initialized");
//             m_indexBufferActiveCopy = isDynamicAccess ?
//             ((m_indexBufferActiveCopy + 1) % ForgeRenderer::SwapChainLength)
//             : 0;

//            BufferUpdateDesc updateDesc = { m_indexBuffer.m_handle };
//            updateDesc.mSize =  m_indices.size() * sizeof(uint32_t);
//            updateDesc.mDstOffset = m_indexBufferActiveCopy *
//            updateDesc.mSize; beginUpdateResource(&updateDesc);
//            std::copy(m_indices.begin(), m_indices.end(),
//            reinterpret_cast<uint32_t*>(updateDesc.pMappedData));
//            endUpdateResource(&updateDesc, &m_bufferSync);
//        }
//        m_updateIndices = false;
//    }

//    // GeometryBinding binding = {};
//    packet.m_indvidual.m_numStreams = 0;
//    for (auto& targetEle : elements) {
//        auto found = std::find_if(m_vertexElements.begin(),
//        m_vertexElements.end(), [&](auto& element) {
//            return element.m_type == targetEle;
//        });
//        ASSERT(found != m_vertexElements.end() && "Element not found");
//        auto& stream =
//        packet.m_indvidual.m_vertexStream[packet.m_indvidual.m_numStreams++];
//        stream.m_buffer = &found->m_buffer;
//        stream.m_offset = found->m_activeCopy * found->m_shadowData.size();
//        stream.m_stride = found->Stride();
//    }
//    const int requestNumIndecies = GetRequestNumberIndecies();
//    const uint32_t numIndecies = (requestNumIndecies >= 0) ?
//    requestNumIndecies : static_cast<uint32_t>(m_indices.size()) ;
//    packet.m_indvidual.m_indexStream.m_offset = m_indexBufferActiveCopy *
//    m_indices.size() * sizeof(uint32_t); packet.m_indvidual.m_numIndices =
//    numIndecies; packet.m_indvidual.m_indexStream.buffer = &m_indexBuffer;
//    return packet;
//}

void VertexBuffer_RI::UnBind() { }

iVertexBuffer *VertexBuffer_RI::CreateCopy(eVertexBufferType aType,
                                           eVertexBufferUsageType aUsageType,
                                           tVertexElementFlag alVtxToCopy) {
  auto *vertexBuffer =
      new VertexBuffer_RI(mpLowLevelGraphics, mType, mDrawType, aUsageType, GetIndexNum(), GetVertexNum());
  vertexBuffer->m_indices = m_indices;

  for (auto element : m_vertexElements) {
    if (element.flag & alVtxToCopy) {
      auto &vb = vertexBuffer->m_vertexElements.emplace_back(element);
      // The copied VertexElement inherited the source's GPU-buffer shared_ptr
      // and its m_internalBufferSize. The copy needs its own GPU storage
      // (otherwise its first UpdateData would stage into the source's buffer),
      // so drop both — SubmitToGPU's first run will allocate fresh.
      vb.buffer.reset();
      vb.m_internalBufferSize = 0;
    }
  }
  vertexBuffer->Compile(0); // actually create the buffers
  return vertexBuffer;
}

} // namespace hpl
