// =============================================================================
// grains.h — the grain cloud
//
// The tape's play head is the "centre of gravity". A scheduler fires new
// grains at a steady interval so that, on average, GRAINS of them overlap:
//
//     interval = grain_size / grain_count
//
// Each new grain:
//   * starts near the play head, offset by SPRAY (random position jitter)
//   * gets a random pan, scaled by SPREAD
//   * freezes the ENV shape it was born with (so turning ENV doesn't make
//     grains already sounding jump)
//   * plays at the tape rate, so PITCH transposes the grains too
//
// The source is summed to mono per grain and then panned; that is what makes
// SPREAD audible as a widening cloud even on a mono recording.
// =============================================================================
#pragma once
#include <cmath>
#include <cstdint>
#include "envelopes.h"
#include "tape_loop.h"

namespace silt
{
struct Grain
{
    bool    active    = false;
    double  pos       = 0.0; // offset inside the loop region (samples)
    float   inc       = 1.f; // playback rate
    float   phase     = 0.f; // 0..1 through the grain's life
    float   phase_inc = 0.f; // 1 / duration_in_samples
    float   gain_l    = 0.f;
    float   gain_r    = 0.f;
    float   pan       = 0.f; // -1..+1 (kept for the UI)
    uint8_t env       = 0;   // envelope shape index
};

class GrainCloud
{
  public:
    static constexpr int kMaxGrains = 24;

    void Init(float sample_rate, const EnvelopeTable* env)
    {
        sr_   = sample_rate;
        env_  = env;
        rng_  = 0x1234567u;
        for(auto& g : grains_)
            g.active = false;
    }

    // Block-rate parameter update
    void SetParams(int count, float size_s, float spray, float spread, int env, float rate)
    {
        count_    = count < 0 ? 0 : (count > kMaxGrains ? kMaxGrains : count);
        size_smp_ = size_s * sr_;
        spray_    = spray;
        spread_   = spread;
        env_sel_  = env;
        rate_     = rate;
        // Overlapping grains add up; 1/sqrt(N) keeps the perceived level
        // roughly constant for uncorrelated grains.
        norm_ = count_ > 0 ? 1.4f / sqrtf(float(count_)) : 0.f;
    }

    // Per-sample: adds the cloud's output into l/r
    inline void Process(const TapeLoop& tape, float& l, float& r)
    {
        if(count_ > 0)
        {
            spawn_timer_ -= 1.f;
            if(spawn_timer_ <= 0.f)
            {
                Spawn(tape);
                spawn_timer_ += size_smp_ / float(count_);
            }
        }

        // Hot loop — runs (active grains x 48,000) times a second, so it is
        // kept lean:
        //  * The loop region always lies inside the recording
        //    (start + length <= recorded length) and every grain keeps
        //    0 <= pos < length, so start + pos is always a valid index. No
        //    per-read wrap-around checks are needed.
        //  * Left and right are read with the same index and fraction, and
        //    summed to mono BEFORE interpolating (one lerp instead of two).
        float          sum_l = 0.f, sum_r = 0.f;
        const double   len   = tape.RegionLength();
        const double   start = tape.RegionStart();
        const uint32_t last  = uint32_t(tape.RecordedLength()) - 1;
        const float*   bl    = tape.Channel(0);
        const float*   br    = tape.Channel(1);
        for(auto& g : grains_)
        {
            if(!g.active)
                continue;
            if(g.pos >= len) // region was shortened under this grain
                g.pos = fmod(g.pos, len);

            const double   idx = start + g.pos;
            const uint32_t i0  = uint32_t(idx);
            const uint32_t i1  = i0 < last ? i0 + 1 : 0;
            const float    f   = float(idx - double(i0));
            const float    a   = bl[i0] + br[i0];
            const float    s   = 0.5f * (a + f * ((bl[i1] + br[i1]) - a));

            const float amp = env_->Lookup(g.env, g.phase);
            sum_l += s * amp * g.gain_l;
            sum_r += s * amp * g.gain_r;

            g.pos += g.inc;
            if(g.pos >= len)
                g.pos -= len;
            g.phase += g.phase_inc;
            if(g.phase >= 1.f)
                g.active = false;
        }
        l += sum_l * norm_;
        r += sum_r * norm_;
    }

    const Grain* Grains() const { return grains_; }

    // CPU safety valve: while set, no NEW grains start. Grains already playing
    // finish normally, so the cloud thins out smoothly instead of clicking.
    void SetThrottle(bool on) { throttle_ = on; }

    // Active grains right now (for the UI / CPU readout)
    int ActiveCount() const
    {
        int n = 0;
        for(const auto& g : grains_)
            n += g.active ? 1 : 0;
        return n;
    }

  private:
    void Spawn(const TapeLoop& tape)
    {
        if(throttle_)
            return; // CPU near its limit: skip this grain
        Grain* g = nullptr;
        for(auto& c : grains_)
            if(!c.active)
            {
                g = &c;
                break;
            }
        if(g == nullptr)
            return; // pool exhausted: skip this grain rather than steal one

        // SPRAY: jitter of up to ±0.5 s (or half the loop if shorter)
        float max_jitter = fminf(0.5f * sr_, tape.RegionLength() * 0.5f);
        float jitter     = (Rand() * 2.f - 1.f) * spray_ * max_jitter;

        double pos = tape.Head() + jitter;
        double len = tape.RegionLength();
        while(pos >= len)
            pos -= len;
        while(pos < 0.0)
            pos += len;

        // SPREAD: equal-power pan law
        float pan   = (Rand() * 2.f - 1.f) * spread_;
        float angle = (pan + 1.f) * 0.78539816f; // 0..pi/2

        g->pos       = pos;
        g->inc       = rate_;
        g->phase     = 0.f;
        g->phase_inc = 1.f / fmaxf(size_smp_, 16.f);
        g->pan       = pan;
        float lvl    = env_->Gain(env_sel_);
        g->gain_l    = cosf(angle) * lvl;
        g->gain_r    = sinf(angle) * lvl;
        g->env       = (uint8_t)env_sel_;
        g->active    = true;
    }

    // xorshift32 → 0..1. Fast, deterministic, good enough for audio jitter.
    inline float Rand()
    {
        rng_ ^= rng_ << 13;
        rng_ ^= rng_ >> 17;
        rng_ ^= rng_ << 5;
        return float(rng_ >> 8) * (1.f / 16777216.f);
    }

    Grain                grains_[kMaxGrains];
    const EnvelopeTable* env_         = nullptr;
    float                sr_          = 48000.f;
    int                  count_       = 0;
    float                size_smp_    = 3840.f;
    float                spray_       = 0.f;
    float                spread_      = 0.f;
    int                  env_sel_     = 0;
    float                rate_        = 1.f;
    float                norm_        = 0.f;
    float                spawn_timer_ = 0.f;
    uint32_t             rng_         = 1;
    bool                 throttle_    = false;
};

} // namespace silt
