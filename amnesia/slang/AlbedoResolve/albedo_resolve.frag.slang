// Albedo resolve pass.
//
// Fullscreen pass that reconstructs the per-pixel surface albedo from the
// resolved visibility buffer (gPackedHitInfo, RT-primary, with the raster
// gPackedHitInfoRaster fallback — same decode SurfelGIRenderPass.frag uses) and
// writes it to a screen-space albedo buffer. Decals are then rasterized into
// this buffer (native blend modes) and SurfelGIRenderPass samples it for
// sd.albedo, so decals become part of the surface and get lit — matching the
// base game's albedo-G-buffer decals instead of compositing on top of the lit
// result.
//
// Albedo is stored LINEAR (RGBA16F buffer): the lighting pass reads it directly,
// and decals (sampled sRGB → linear) blend into it in linear space. No gamma
// round-trip.
//
// Water/glass: the main pass shades water in its own isWater branch and never
// reads this buffer for those pixels, so we leave a = 0 there.

#include "../HostDefinitions.h"
#include "../Constants.h"

import SceneTypes;
import Scene;
import Bindless.resource;      // gPackedHitInfo (set 1, kBindingPackedHitInfo)
import Bindless.interface;
import PerFrame.resource;
import SurfelGI.SurfelShade;   // DiffuseShading / prepareDiffuseShading

// Rasterized V-buffer fallback — same layout/binding as SurfelGIRenderPass.frag.
[vk::binding(1, 2)]
Texture2D<uint4> gPackedHitInfoRaster;

struct PSIn
{
    [vk::location(0)] float2 v_uv;
    float4 posH : SV_Position;
};

[shader("fragment")]
float4 psMain(PSIn pin) : SV_Target
{
    uint2 pixelPos = uint2(pin.posH.xy);

    HitInfo hitInfo = HitInfo(gPackedHitInfo[pixelPos]);
    if (!hitInfo.isValid())
    {
        hitInfo = HitInfo(gPackedHitInfoRaster.Load(int3(pixelPos, 0)));
        if (!hitInfo.isValid())
            return float4(0.f, 0.f, 0.f, 0.f);   // miss → no albedo
    }

    TriangleHit triangleHit = hitInfo.getTriangleHit();
    uint materialID = gScene.getMaterialID(triangleHit.instanceID);

    // Water is shaded directly in the main pass and ignores this buffer.
    if (gScene.materials.isWater(materialID))
        return float4(0.f, 0.f, 0.f, 0.f);

    VertexData v = gScene.getVertexData(triangleHit);
    float illuminationAmount = gScene.getObject(triangleHit.instanceID).illuminationAmount;
    DiffuseShading sd = prepareDiffuseShading(v, materialID, illuminationAmount);

    // sd.albedo is already linear (diffuse sampled through the sRGB view). Store
    // it linear; the decal blend and the lighting read both work in linear space.
    return float4(sd.albedo, 1.0f);
}
