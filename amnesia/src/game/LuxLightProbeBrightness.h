// SPDX-License-Identifier: GPL-3.0
#ifndef LUX_LIGHT_PROBE_BRIGHTNESS_H
#define LUX_LIGHT_PROBE_BRIGHTNESS_H

#include <algorithm>
#include <cmath>

// CPU-only policy for the asynchronous physical-light sensor. Retain the
// environmental reading separately so the lantern bonus never feeds back.
class cLuxLightProbeBrightness {
public:
    void Reset() { mfLuminance = 0.0f; mfEnvironment = 1.0f; mbLantern = false; }

    void Update(const float (*samples)[3], int count, float gain) {
        if (!samples || count <= 0) return;
        if (!std::isfinite(gain) || gain <= 0.0f) gain = 1.0f;
        double brightest = 0.0;
        for (int i = 0; i < count; ++i) {
            const double luminance = 0.2126 * Component(samples[i][0])
                                   + 0.7152 * Component(samples[i][1])
                                   + 0.0722 * Component(samples[i][2]);
            brightest = std::max(brightest, luminance);
        }
        mfLuminance = static_cast<float>(brightest);
        mfEnvironment = static_cast<float>(std::min(brightest * gain, 1.0));
    }

    void SetLantern(bool active) { mbLantern = active; }
    float GetLevel() const { return mfEnvironment + (mbLantern ? 1.0f : 0.0f); }
    float GetLuminance() const { return mfLuminance; }

private:
    static double Component(float value) {
        return std::isfinite(value) && value > 0.0f ? value : 0.0;
    }
    float mfLuminance = 0.0f;
    float mfEnvironment = 1.0f;
    bool mbLantern = false;
};

#endif
