#ifndef HPL_WATER_REFLECTION_JITTER_MATH_H
#define HPL_WATER_REFLECTION_JITTER_MATH_H

#include <cstdint>

namespace hpl {

// Returns the ceil(full / 2) extent used by the half-resolution water
// reflection and NRD resources.
uint32_t WaterReflectionHalfResExtent(uint32_t full);

// Converts full-render-pixel camera jitter into the input-pixel units of the
// half-resolution water reflection NRD instance. A zero extent produces zero
// on that axis rather than dividing by zero.
void WaterReflectionScaleJitterToHalfRes(
    const float fullPixelJitter[2], uint32_t fullWidth, uint32_t fullHeight,
    float outHalfPixelJitter[2]);

} // namespace hpl

#endif // HPL_WATER_REFLECTION_JITTER_MATH_H
