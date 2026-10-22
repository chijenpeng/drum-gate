#pragma once

#include <cmath>

namespace drumgate
{

/** Second-order Butterworth section (RBJ cookbook), transposed direct form II. */
class Biquad
{
public:
    void setLowPass (double sampleRate, double freq) noexcept { set (sampleRate, freq, false); }
    void setHighPass (double sampleRate, double freq) noexcept { set (sampleRate, freq, true); }

    void reset() noexcept { z1 = z2 = 0.0f; }

    float process (float x) noexcept
    {
        const float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }

private:
    void set (double sampleRate, double freq, bool highPass) noexcept
    {
        const double pi = 3.14159265358979323846;
        const double f = freq < sampleRate * 0.45 ? freq : sampleRate * 0.45;
        const double w = 2.0 * pi * f / sampleRate;
        const double cosw = std::cos (w);
        const double alpha = std::sin (w) / (2.0 * 0.7071067811865476);
        const double a0 = 1.0 + alpha;

        const double edge = highPass ? (1.0 + cosw) : (1.0 - cosw);
        b0 = (float) (edge * 0.5 / a0);
        b1 = (float) ((highPass ? -edge : edge) / a0);
        b2 = b0;
        a1 = (float) (-2.0 * cosw / a0);
        a2 = (float) ((1.0 - alpha) / a0);
    }

    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
    float z1 = 0.0f, z2 = 0.0f;
};

} // namespace drumgate
