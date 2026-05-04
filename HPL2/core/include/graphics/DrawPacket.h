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

#ifndef HPL_DRAW_PACKET_H
#define HPL_DRAW_PACKET_H

#include "graphics/RITypes.h"
#include <array>
#include <cstdint>
#include <memory>

namespace hpl {

struct DrawPacket {
  uint32_t numIndices;
  uint32_t numStreams;
  struct DrawVertexStream {
    std::shared_ptr<RIBuffer_s> buffer;
    uint64_t offset;
    uint32_t stride;
  };
  std::array<DrawPacket::DrawVertexStream, 16> vertexStreams;
  struct {
    std::shared_ptr<RIBuffer_s> buffer;
    uint64_t offset;
  } indexStream;
};
} // namespace hpl

#endif
