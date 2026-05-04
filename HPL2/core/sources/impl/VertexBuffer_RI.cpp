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
#include "math/Math.h"
#include "system/LowLevelSystem.h"

#include <algorithm>
#include <cstdint>
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

VertexBuffer_RI::VertexBuffer_RI(iLowLevelGraphics* apLowLevelGraphics,
			eVertexBufferType aType, 
			eVertexBufferDrawType aDrawType,eVertexBufferUsageType aUsageType,
			int alReserveVtxSize,int alReserveIdxSize) 
    : iVertexBuffer(apLowLevelGraphics, aType, aDrawType, aUsageType, alReserveVtxSize, alReserveIdxSize) {
}

VertexBuffer_RI::~VertexBuffer_RI() {
  // waitForToken(&m_bufferSync);
  // m_indexBuffer.TryFree();
  for (auto &element : m_vertexElements) {
    //   element.m_buffer.TryFree();
  }
}
void VertexBuffer_RI::Bind() {

} 

void VertexBuffer_RI::DrawIndices(	unsigned int *apIndices, int alCount,
								eVertexBufferDrawType aDrawType) {} 
	
void VertexBuffer_RI::CreateShadowDouble(bool abUpdateData) {}

// void VertexBuffer_RI::cmdBindGeometry(Cmd* cmd,
// ForgeRenderer::CommandResourcePool* resourcePool,
// LegacyVertexBuffer::GeometryBinding& binding) {
//     folly::small_vector<Buffer*, 16> vbBuffer;
//     folly::small_vector<uint64_t, 16> vbOffsets;
//     folly::small_vector<uint32_t, 16> vbStride;

//     for (auto& element : binding.m_vertexElement) {
//         vbBuffer.push_back(element.element->m_buffer.m_handle);
//         vbOffsets.push_back(element.offset);
//         vbStride.push_back(element.element->Stride());
//         resourcePool->Push(element.element->m_buffer);
//     }
//     resourcePool->Push(*binding.m_indexBuffer.element);

//     cmdBindVertexBuffer(cmd, binding.m_vertexElement.size(), vbBuffer.data(),
//     vbStride.data(), vbOffsets.data()); cmdBindIndexBuffer(cmd,
//     binding.m_indexBuffer.element->m_handle, INDEX_TYPE_UINT32,
//     binding.m_indexBuffer.offset);
// }
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
  // SyncToken token = {};

  for (auto &element : m_vertexElements) {
    m_updateFlags |= element.flag;
  }
  m_updateIndices = true;
  return true;
}

void VertexBuffer_RI::UpdateData(tVertexElementFlag aTypes, bool abIndices) {
  m_updateFlags |= aTypes;
  m_updateIndices |= abIndices;
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
  }
}

void VertexBuffer_RI::ResizeIndices(int alSize) {
  m_updateIndices = true;
  m_indices.resize(alSize);
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
      //vb.m_buffer.TryFree();
    }
  }
  vertexBuffer->Compile(0); // actually create the buffers
  return vertexBuffer;
}

} // namespace hpl
