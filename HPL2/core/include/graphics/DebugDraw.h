/**
 * Copyright 2026 Michael Pollind
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include "graphics/Color.h"
#include "graphics/Graphics.h"
#include "graphics/RIProgram.h"
#include "graphics/RISegmentAlloc.h"
#include "graphics/RITypes.h"
#include "math/MathTypes.h"

#include <memory>
#include <variant>
#include <vector>

// X11's X.h (pulled in via the Vulkan WSI headers above) defines None and
// Always as macros, which collide with the DebugDepthTest enumerators below.
// Nothing in the engine uses the X11 macros directly, and X.h is include-
// guarded, so they stay gone. (The TDD fork solved the same clash with
// The-Forge's FixPreprocessor.h.)
#ifdef None
#undef None
#endif
#ifdef Always
#undef Always
#endif

namespace hpl {

class cResources;
class cCamera;
class cFrustum;
class cVertexBuffer;
class Image;
struct cTexture;

// Immediate-mode batcher for editor / debug overlays (grids, gizmos,
// selection wireframes, entity icons) on the RI backend. Callers enqueue
// requests in world (or GUI-pixel) space during the frame; flush() streams
// them into a per-frame ring and draws them into whatever dynamic-rendering
// scope the caller has open (normally HybridRenderer's offscreen tail, into
// the pogo read-half against the scene depth — see the offTarget branch).
//
// API takes HPL types natively (cVector3f / cColor / cMatrixf); conversion
// to ml:: happens once at UBO-fill time, matching HybridRenderer's frustum
// handling. Standalone non-bindless programs (gui-style) — deliberately does
// not touch GlobalManagedSets.
class DebugDraw final {
public:
    enum class DebugDepthTest : uint8_t {
        None,           // never passes
        Less,
        LessEqual,
        Equal,
        GreaterEqual,
        Greater,
        NotEqual,
        Always,         // always passes
        Count
    };

    struct DebugDrawOptions {
        DebugDrawOptions() {}
        DebugDepthTest m_depthTest = DebugDepthTest::LessEqual;
        cMatrixf m_transform = cMatrixf::Identity;
    };

    DebugDraw() = default;
    DebugDraw(const DebugDraw&) = delete;
    DebugDraw& operator=(const DebugDraw&) = delete;

    // Builds the three RIPrograms from the DebugDraw slang shaders
    // (debug.vert/frag, debug_2d.vert, debug_uv.vert/frag).
    void Init(cResources* apResources);

    // Drops all queued requests without drawing (flush() does this itself).
    void Reset();
    bool HasRequests() const;

    /////////////////////////////////////////////
    // 3D primitives (world space; options.m_transform applies at enqueue)
    void DebugDrawLine(const cVector3f& avStart, const cVector3f& avEnd, const cColor& aColor,
                       const DebugDrawOptions& aOptions = DebugDrawOptions());
    void DebugDrawBoxMinMax(const cVector3f& avMin, const cVector3f& avMax, const cColor& aColor,
                            const DebugDrawOptions& aOptions = DebugDrawOptions());
    void DebugDrawSphere(const cVector3f& avPos, float afRadius, const cColor& aColor,
                         const DebugDrawOptions& aOptions = DebugDrawOptions());
    void DebugDrawSphere(const cVector3f& avPos, float afRadius, const cColor& aC1, const cColor& aC2,
                         const cColor& aC3, const DebugDrawOptions& aOptions = DebugDrawOptions());
    void DrawTri(const cVector3f& avV1, const cVector3f& avV2, const cVector3f& avV3, const cColor& aColor,
                 const DebugDrawOptions& aOptions = DebugDrawOptions());
    void DrawQuad(const cVector3f& avV1, const cVector3f& avV2, const cVector3f& avV3, const cVector3f& avV4,
                  const cColor& aColor, const DebugDrawOptions& aOptions = DebugDrawOptions());
    void DrawQuad(const cVector3f& avV1, const cVector3f& avV2, const cVector3f& avV3, const cVector3f& avV4,
                  const cVector2f& avUv0, const cVector2f& avUv1, Image* apImage, const cColor& aTint,
                  const DebugDrawOptions& aOptions = DebugDrawOptions());
    void DrawPyramid(const cVector3f& avBaseCenter, const cVector3f& avTop, float afHalfWidth,
                     const cColor& aColor, const DebugDrawOptions& aOptions = DebugDrawOptions());
    void DrawBillboard(const cVector3f& avPos, const cVector2f& avSize, const cVector2f& avUv0,
                       const cVector2f& avUv1, Image* apImage, const cColor& aTint,
                       const DebugDrawOptions& aOptions = DebugDrawOptions());

    // Triangle walks over an engine vertex buffer (selection outlines, solid
    // highlight fills).
    void DebugWireFrameFromVertexBuffer(cVertexBuffer* apVertexBuffer, const cColor& aColor,
                                        const DebugDrawOptions& aOptions = DebugDrawOptions());
    void DebugSolidFromVertexBuffer(cVertexBuffer* apVertexBuffer, const cColor& aColor,
                                    const DebugDrawOptions& aOptions = DebugDrawOptions());

    /////////////////////////////////////////////
    // 2D primitives (target-pixel space, y origin at the top — GUI convention)
    void DebugDraw2DLine(const cVector2f& avStart, const cVector2f& avEnd, const cColor& aColor);
    void DebugDraw2DLineQuad(cRect2f aRect, const cColor& aColor);

    // Scale factor so a billboard keeps constant screen size with distance.
    static float BillboardScale(cCamera* apCamera, const cVector3f& avPos);

    // Streams all queued requests and draws them. The caller has a
    // dynamic-rendering scope open whose color attachment has format
    // aColorFormat (the pipelines key on it) and whose depth attachment is
    // the scene depth (LOADed, read-only — the pipelines never write depth).
    // Sets its own Y-flipped viewport/scissor to the target extent. Calls
    // Reset() at the end.
    void flush(cGraphics::FrameContext* cntx, struct RICmd* cmd, const cFrustum* apFrustum,
               uint32_t alTargetWidth, uint32_t alTargetHeight,
               enum RI_Format_e aColorFormat = cGraphics::PogoColorFormat);

private:
    // Mirrors the unified vertex stream the DebugDraw slang shaders declare
    // (pos@0, uv@12, color@20 — stride 36). Color-only stages skip uv.
    struct DebugVertex {
        float position[3];
        float uv[2];
        float color[4];
    };

    struct LineSegmentRequest {
        DebugDepthTest m_depthTest = DebugDepthTest::LessEqual;
        cVector3f m_start;
        cVector3f m_end;
        cColor m_color;
    };

    struct ColorTriRequest {
        DebugDepthTest m_depthTest = DebugDepthTest::LessEqual;
        cVector3f m_v1, m_v2, m_v3;
        cColor m_color;
    };

    struct ColorQuadRequest {
        DebugDepthTest m_depthTest = DebugDepthTest::LessEqual;
        cVector3f m_v1, m_v2, m_v3, m_v4;
        cColor m_color;
    };

    struct UVQuadRequest {
        struct Quad {
            cVector3f m_v1, m_v2, m_v3, m_v4;
        };
        struct Billboard {
            cVector3f m_pos;
            cVector2f m_size;
            cMatrixf m_transform;
        };
        DebugDepthTest m_depthTest = DebugDepthTest::LessEqual;
        std::variant<Quad, Billboard> m_type;
        cVector2f m_uv0, m_uv1;
        SharedResourceHandle<Image> m_texture;
        cColor m_color;
    };

    struct Line2DSegmentRequest {
        cVector2f m_start;
        cVector2f m_end;
        cColor m_color;
    };

    // Grow-on-demand per-frame streaming ring (GuiSet pattern): on overflow
    // the buffer is recreated 1.5x larger and the old one is deferred onto
    // the frame freelist.
    bool RequestStream(cGraphics::FrameContext* cntx, size_t alNumVertices, size_t alNumIndices,
                       struct RISegmentReq* apVtxReq, struct RISegmentReq* apIdxReq);

    std::vector<LineSegmentRequest> m_lineSegments;
    std::vector<ColorTriRequest> m_colorTriangles;
    std::vector<ColorQuadRequest> m_colorQuads;
    std::vector<UVQuadRequest> m_uvQuads;
    std::vector<Line2DSegmentRequest> m_line2DSegments;

    RIProgram m_colorProgram;   // debug.vert + debug.frag
    RIProgram m_color2DProgram; // debug_2d.vert + debug.frag
    RIProgram m_uvProgram;      // debug_uv.vert + debug_uv.frag

    RISegmentAlloc<RI_NUMBER_FRAME_SEGMENTS> m_vertexAlloc;
    RISharedPointer<RIBuffer> m_vertexBuffer;
    RISegmentAlloc<RI_NUMBER_FRAME_SEGMENTS> m_indexAlloc;
    RISharedPointer<RIBuffer> m_indexBuffer;
};

} // namespace hpl
