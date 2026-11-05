// =============================================================================
// envelopes.h — the 10 grain window shapes as lookup tables
//
// Like Quanta, the ENV control picks between distinct shapes, each with its
// own attack/decay curve. Computing pow/cos per grain per sample would be too
// slow, so every shape is pre-rendered once into a 256-point table and read
// with linear interpolation. The UI draws its curves from the same tables, so
// what you see is exactly what the grains use.
// =============================================================================
#pragma once
#include <cmath>


// Audio hot path: compiled for speed (-O2) even when the sketch is built
// with "Smallest (-Os)" to fit the Seed's 128 KB internal flash.
#pragma GCC push_options
#pragma GCC optimize("O2")

namespace gloop
{
class EnvelopeTable
{
  public:
    static constexpr int kShapes = 10;
    static constexpr int kSize   = 256;

    void Init()
    {
        for(int s = 0; s < kShapes; s++)
        {
            float power = 0.f;
            for(int i = 0; i <= kSize; i++)
            {
                table_[s][i] = Shape(s, float(i) / kSize);
                power += table_[s][i] * table_[s][i];
            }
            // Level compensation: a BOX window carries far more energy than a
            // SPIKE. Scale every shape to the same RMS as HANN (mean power
            // 0.375) so switching ENV doesn't jump the volume.
            power /= float(kSize + 1);
            gain_[s] = sqrtf(0.375f / power);
        }
    }

    // Per-shape loudness compensation, applied once when a grain is born
    inline float Gain(int shape) const { return gain_[shape]; }

    // Raw table for one shape (kSize + 1 points) — the grain loop reads it
    // directly so the shape lookup isn't repeated every sample
    inline const float* Table(int shape) const { return table_[shape]; }

    // phase 0..1 across the grain's life → gain 0..1
    inline float Lookup(int shape, float phase) const
    {
        float x  = phase * kSize;
        int   i  = (int)x;
        if(i >= kSize)
            return table_[shape][kSize];
        float f = x - i;
        return table_[shape][i] + f * (table_[shape][i + 1] - table_[shape][i]);
    }

  private:
    static float Shape(int k, float x)
    {
        const float kPi = 3.14159265f;
        switch(k)
        {
            case 0: return 0.5f - 0.5f * cosf(2.f * kPi * x);             // HANN
            case 1: return 1.f - fabsf(2.f * x - 1.f);                    // TRI
            case 2: return fminf(1.f, fminf(x, 1.f - x) / 0.15f);         // TRAPEZ
            case 3:                                                       // PERC
                return x < 0.04f ? x / 0.04f : powf(1.f - (x - 0.04f) / 0.96f, 3.f);
            case 4:                                                       // SWELL
                return x > 0.96f ? (1.f - x) / 0.04f : powf(x / 0.96f, 3.f);
            case 5: return expf(-0.5f * powf((x - 0.5f) / 0.16f, 2.f));   // GAUSS
            case 6: return x < 0.9f ? x / 0.9f : (1.f - x) / 0.1f;        // RAMP UP
            case 7: return x < 0.1f ? x / 0.1f : 1.f - (x - 0.1f) / 0.9f; // RAMP DN
            case 8: return powf(0.5f - 0.5f * cosf(2.f * kPi * x), 4.f);  // SPIKE
            default:                                                      // BOX
                return x < 0.03f ? x / 0.03f : (x > 0.97f ? (1.f - x) / 0.03f : 1.f);
        }
    }

    float table_[kShapes][kSize + 1];
    float gain_[kShapes];
};

} // namespace gloop

#pragma GCC pop_options
