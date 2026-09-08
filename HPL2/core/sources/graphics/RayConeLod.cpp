#include "graphics/RayConeLod.h"

#include <cmath>

namespace hpl {
namespace {

constexpr float kRayConeMinCosine = 0.05f;

} // namespace

float RayConePrimarySpreadAngle(float verticalFovRadians,
                                uint32_t renderHeight) {
  if (renderHeight == 0 || !(verticalFovRadians > 0.0f))
    return 0.0f;

  return std::atan(2.0f * std::tan(0.5f * verticalFovRadians) /
                   static_cast<float>(renderHeight));
}

float RayConeSurfaceSpreadAngle(float ggxAlpha, bool sampledSpecular) {
  if (!sampledSpecular)
    return kRayConeMaxSpreadAngle;

  return 2.0f * std::atan(std::fmin(std::fmax(ggxAlpha, 0.0f), 1.0f));
}

float WaterGuideNormalSpreadAlpha(const float normalSum[3], uint32_t count) {
  if (count < 2 || !std::isfinite(normalSum[0]) ||
      !std::isfinite(normalSum[1]) || !std::isfinite(normalSum[2]))
    return 0.0f;

  const float normalLength =
      std::sqrt(normalSum[0] * normalSum[0] +
                normalSum[1] * normalSum[1] + normalSum[2] * normalSum[2]);
  const float meanResultantLength =
      std::fmin(std::fmax(normalLength / static_cast<float>(count), 0.0f),
                1.0f);
  const float theta =
      std::sqrt(2.0f * std::fmax(0.0f, 1.0f - meanResultantLength));
  constexpr float kPiOverFour = 0.7853981633974483f;
  return std::tan(std::fmin(theta, kPiOverFour));
}

float WaterNrdLinearRoughness(float spreadAlpha) {
  if (!std::isfinite(spreadAlpha))
    return 0.0f;

  return std::sqrt(std::fmin(std::fmax(spreadAlpha, 0.0f), 1.0f));
}

float RayConeWidthAt(float spreadAngle, float startWidth, float hitDistance) {
  return startWidth + spreadAngle * hitDistance;
}

float RayConeWidenSpreadAngle(float spreadAngle, float surfaceSpreadAngle) {
  return std::fmin(spreadAngle + std::fmax(surfaceSpreadAngle, 0.0f),
                   kRayConeMaxSpreadAngle);
}

float RayConeTriangleLodConstant(float uvArea, float worldArea) {
  if (!(uvArea > 0.0f) || !(worldArea > 0.0f))
    return 0.0f;

  return 0.5f * std::log2(uvArea / worldArea);
}

float RayConeTextureLod(float triangleLodConstant, float coneWidth,
                        float normalDotRayDir, float textureWidth,
                        float textureHeight, float materialMipBias) {
  if (!(textureWidth > 0.0f) || !(textureHeight > 0.0f))
    return 0.0f;

  return triangleLodConstant +
         std::log2(std::fmax(std::fabs(coneWidth), 1.0e-8f)) -
         std::log2(std::fmax(std::fabs(normalDotRayDir),
                             kRayConeMinCosine)) +
         0.5f * std::log2(std::fmax(textureWidth * textureHeight, 1.0f)) +
         materialMipBias;
}

float RayConeSurfaceFootprint(float coneWidth, float normalDotRayDir) {
  return std::fabs(coneWidth) /
         std::fmax(std::fabs(normalDotRayDir), kRayConeMinCosine);
}

void WaterWaveUvJacobian(float effectiveAmplitude, float effectiveFrequency,
                         float phase, float v, float modifiedUvX,
                         float outJacobian[4]) {
  // Keep the inputs in shader order: k1 uses the pre-y-update v, while k2
  // uses the x coordinate after the first sequential sine update.
  const float k1 = effectiveAmplitude * effectiveFrequency *
                   std::cos(phase + v * effectiveFrequency);
  const float k2 = effectiveAmplitude * effectiveFrequency *
                   std::cos(phase + modifiedUvX * effectiveFrequency);
  outJacobian[0] = 1.0f;
  outJacobian[1] = k1;
  outJacobian[2] = k2;
  outJacobian[3] = 1.0f + k1 * k2;
}

void WaterWaveUvGradient(const float jacobian[4],
                         const float baseUvGradient[2],
                         float outSampleUvGradient[2]) {
  outSampleUvGradient[0] = jacobian[0] * baseUvGradient[0] +
                           jacobian[1] * baseUvGradient[1];
  outSampleUvGradient[1] = jacobian[2] * baseUvGradient[0] +
                           jacobian[3] * baseUvGradient[1];
}

float ProjectedGoboUvPerWorldUnit(const float rowX[4], const float rowY[4],
                                  const float rowW[4], const float posW[3]) {
  const float clipW = rowW[0] * posW[0] + rowW[1] * posW[1] +
                      rowW[2] * posW[2] + rowW[3];
  if (!(clipW > 0.0f))
    return 0.0f;

  const float u = (rowX[0] * posW[0] + rowX[1] * posW[1] +
                   rowX[2] * posW[2] + rowX[3]) /
                  clipW;
  const float v = (rowY[0] * posW[0] + rowY[1] * posW[1] +
                   rowY[2] * posW[2] + rowY[3]) /
                  clipW;
  const float jx0 = (rowX[0] - u * rowW[0]) / clipW;
  const float jx1 = (rowX[1] - u * rowW[1]) / clipW;
  const float jx2 = (rowX[2] - u * rowW[2]) / clipW;
  const float jy0 = (rowY[0] - v * rowW[0]) / clipW;
  const float jy1 = (rowY[1] - v * rowW[1]) / clipW;
  const float jy2 = (rowY[2] - v * rowW[2]) / clipW;
  const float jxLength = std::sqrt(jx0 * jx0 + jx1 * jx1 + jx2 * jx2);
  const float jyLength = std::sqrt(jy0 * jy0 + jy1 * jy1 + jy2 * jy2);
  return std::fmax(jxLength, jyLength);
}

float ProjectedGoboTexLodBase(float surfaceFootprintW, float uvPerWorldUnit) {
  if (!(surfaceFootprintW > 0.0f) || !(uvPerWorldUnit > 0.0f))
    return kTexLodTopMipBase;

  return std::log2(surfaceFootprintW * uvPerWorldUnit);
}

float CubeFaceUvPerRadian(const float dir[3]) {
  const float dx = dir[0];
  const float dy = dir[1];
  const float dz = dir[2];
  const float m = std::fmax(std::fmax(std::fabs(dx), std::fabs(dy)),
                            std::fabs(dz));
  if (!(m > 0.0f))
    return 0.0f;

  return 0.5f * (dx * dx + dy * dy + dz * dz) / (m * m);
}

float CubeGoboTexLodBase(float surfaceFootprintW, float distanceToLight,
                         const float dir[3]) {
  if (!(surfaceFootprintW > 0.0f) || !(distanceToLight > 0.0f))
    return kTexLodTopMipBase;

  const float uvPerRadian = CubeFaceUvPerRadian(dir);
  if (!(uvPerRadian > 0.0f))
    return kTexLodTopMipBase;

  const float uvDiameter = (surfaceFootprintW / distanceToLight) *
                           uvPerRadian;
  if (!(uvDiameter > 0.0f))
    return kTexLodTopMipBase;

  return std::log2(uvDiameter);
}

float DecalUvPerWorldUnit(const float rowX[4], const float rowZ[4],
                          uint32_t subDivX, uint32_t subDivY) {
  const uint32_t sx = subDivX > 0 ? subDivX : 1;
  const uint32_t sy = subDivY > 0 ? subDivY : 1;
  const float rowXLength =
      std::sqrt(rowX[0] * rowX[0] + rowX[1] * rowX[1] + rowX[2] * rowX[2]);
  const float rowZLength =
      std::sqrt(rowZ[0] * rowZ[0] + rowZ[1] * rowZ[1] + rowZ[2] * rowZ[2]);
  return std::fmax(rowXLength / static_cast<float>(sx),
                   rowZLength / static_cast<float>(sy));
}

float DecalAtlasMaxTexLodBase(uint32_t subDivX, uint32_t subDivY) {
  const uint32_t sx = subDivX > 0 ? subDivX : 1;
  const uint32_t sy = subDivY > 0 ? subDivY : 1;
  return -std::log2(std::fmax(static_cast<float>(sx),
                              static_cast<float>(sy)));
}

float DecalTexLodBase(float surfaceFootprintW, float uvPerWorldUnit,
                      uint32_t subDivX, uint32_t subDivY) {
  if (!(surfaceFootprintW > 0.0f) || !(uvPerWorldUnit > 0.0f))
    return kTexLodTopMipBase;

  return std::fmin(std::log2(surfaceFootprintW * uvPerWorldUnit),
                   DecalAtlasMaxTexLodBase(subDivX, subDivY));
}

float AreaLightSourceTexLodBase(float surfaceFootprintW, float distanceToLight,
                                float rectWidth, float rectHeight) {
  const float minExtent = std::fmin(rectWidth, rectHeight);
  if (!(minExtent > 0.0f))
    return kTexLodTopMipBase;

  const float f =
      std::fmax(std::fmax(surfaceFootprintW, 0.0f),
                std::fmax(distanceToLight, 0.0f)) /
      minExtent;
  if (!(f > 0.0f))
    return kTexLodTopMipBase;

  return std::log2(f);
}

} // namespace hpl
