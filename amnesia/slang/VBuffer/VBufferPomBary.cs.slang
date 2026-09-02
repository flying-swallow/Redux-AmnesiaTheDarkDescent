// POM barycentric correction compute pass.
//
// Copies the raw raster V-buffer (gPackedHitInfoRaster / visibilityTexture)
// into gPackedHitInfo, then for pixels that hit an opaque diffuse surface
// with a height map it applies a parallax-occlusion barycentric
// perturbation: the POM-stepped UV is inverted back into a barycentric
// delta so the stored hit lands on the occluded surface point.
//
// Keeping the perturbation here rather than in the raster pixel shader lets
// the raw raster hit (gPackedHitInfoRaster) stay as the clean surface key
// for temporal reprojection while gPackedHitInfo carries the POM-shifted
// version that the surfel generation + composite use to reconstruct the
// occluded surface point.
//
// One 16×16 thread group per tile. Out-of-bounds threads early-out.

#include "../HostDefinitions.h"

import SceneTypes;
import Scene;               // cameraComputeRayPinhole
import Material;
import bindless;            // gPackedHitInfo (RWTexture2D<uint4>, set 1 binding kBindingPackedHitInfo)
import HitInfo;
import PerFrame.resource;
import BindlessTriangle;    // getVertexDataAndTri, BindlessTriangleVtx
import Parallax;            // parallaxOcclusionUV

// Raw raster V-buffer — sampled (read-only). Transitions from
// RENDER_TARGET → SHADER_RESOURCE before this dispatch. Bound in the
// per-pass descriptor set (set 2) by the C++ side.
[vk::binding(0, 2)]
Texture2D<uint4> gPackedHitInfoRaster;

[shader("compute")]
[numthreads(16, 16, 1)]
void csMain(uint3 dtid : SV_DispatchThreadID)
{
    uint2 pixelPos = dtid.xy;

    uint w, h;
    gPackedHitInfo.GetDimensions(w, h);
    if (pixelPos.x >= w || pixelPos.y >= h)
        return;

    uint4 raw = gPackedHitInfoRaster[pixelPos];

    HitInfo hitInfo = HitInfo(raw);

    // Propagate sky / miss pixels unchanged.
    if (!hitInfo.isValid())
    {
        gPackedHitInfo[pixelPos] = raw;
        return;
    }

    TriangleHit triangleHit = hitInfo.getTriangleHit();
    uint materialID = gSceneObjects[triangleHit.instanceID].materialID;

    // Only opaque diffuse surfaces can carry a height map.
    if (loadMaterialHeader(materialID).type == MATERIAL_TYPE_DIFFUSE)
    {
        DiffuseMaterial m = loadDiffuseMaterial(materialID);
        if ((m.materialConfig & kMaterialFlagEnableHeight) != 0u
            && m.tex[4] != kInvalidTextureIndex)
        {
            BindlessTriangleVtx vtx;
            VertexData v = getVertexDataAndTri(triangleHit, vtx);

            // TBN matching the legacy parallax basis: cross(N,T)*handedness.
            // Bitangent is cross(N,T)*handedness — NOT the normal-map
            // bitangent used in Translucent.vert.slang.
            float3 N = normalize(v.normalW);
            float3 T = normalize(v.tangentW - N * dot(N, v.tangentW));
            float3 B = normalize(cross(N, T)) * vtx.tangentL[0].w;

            // Reconstruct primary ray for this pixel (with jitter, matching RT).
            Ray ray = cameraComputeRayPinhole(pixelPos, uint2(w, h), /*applyJitter=*/true);

            bool single = (m.materialConfig & kMaterialFlagIsHeightMapSingleChannel) != 0u;
            float2 newUv = parallaxOcclusionUV(
                m.tex[4], m.heightMapScale * kParalaxScale, single,
                normalize(ray.dir), N, T, B, v.texC);
            float2 duv = newUv - v.texC;

            // Invert the UV delta into a barycentric delta using the
            // triangle's UV edge vectors (UV is affine in barycentrics).
            float2 E1 = vtx.uv[1] - vtx.uv[0];
            float2 E2 = vtx.uv[2] - vtx.uv[0];
            float det = E1.x * E2.y - E2.x * E1.y;
            if (abs(det) >= 1.0e-12f)
            {
                float invDet = 1.0f / det;
                float db1 = invDet * ( E2.y * duv.x - E2.x * duv.y);
                float db2 = invDet * (-E1.y * duv.x + E1.x * duv.y);

                float nb1 = triangleHit.barycentrics.x + db1;
                float nb2 = triangleHit.barycentrics.y + db2;
                float nb0 = 1.0f - nb1 - nb2;
                if (nb0 >= 0.0f && nb1 >= 0.0f && nb2 >= 0.0f)
                    triangleHit.barycentrics = float2(nb1, nb2);
            }
        }
    }

    gPackedHitInfo[pixelPos] = triangleHit.pack();
}
