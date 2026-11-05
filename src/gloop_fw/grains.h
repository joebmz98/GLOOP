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
//
// PERFORMANCE — this is the most expensive code in GLOOP, so:
//   * It runs a whole audio block at a time, one grain after another, rather
//     than all grains once per sample. Each grain's state stays in CPU
//     registers for the whole block and there is one function call per block
//     instead of 48.
//   * Positions are a whole-sample index (uint32) plus a float fraction, so
//     stepping and wrapping are plain integer ops (no double maths).
//   * The tape is int16 stereo interleaved: one 32-bit read gets L and R.
//   * Gains include the int16→float scale and the level normalisation, so the
//     inner loop has no extra multiplies.
//   * New grains can start part-way through a block (`delay`), so timing is
//     still sample-accurate.
// =============================================================================
#pragma once
#include <cmath>
#include <cstddef>
#include <cstdint>
#include "envelopes.h"
#include "tape_loop.h"

// Audio hot path: compiled for speed (-O2) even when the sketch is built
// with "Smallest (-Os)" to fit the Seed's 128 KB internal flash.
#pragma GCC push_options
#pragma GCC optimize("O2")

namespace gloop
{
struct Grain
{
    bool     active    = false;
    uint32_t pos_i     = 0;   // whole-sample position inside the loop region
    float    pos_f     = 0.f; // fractional part, 0..1
    uint32_t inc_i     = 1;   // playback rate, whole part
    float    inc_f     = 0.f; // playback rate, fractional part
    float    phase     = 0.f; // 0..1 through the grain's life
    float    phase_inc = 0.f; // 1 / duration_in_samples
    float    gain_l    = 0.f; // pan x level x int16 scale
    float    gain_r    = 0.f;
    float    pan       = 0.f; // -1..+1 (kept for the UI)
    uint8_t  env       = 0;   // envelope shape index
    uint16_t delay     = 0;   // samples to wait before starting (this block)
};

class GrainCloud
{
  public:
    // VOICE LIMIT per track. Cost grows with the number of overlapping grains,
    // so this is the main CPU control. 12 per track keeps two tracks plus FX
    // well inside the budget (see performance-budget.md).
    static constexpr int kMaxGrains = 12;

    void Init(float sample_rate, const EnvelopeTable* env)
    {
        sr_  = sample_rate;
        env_ = env;
        rng_ = 0x1234567u;
        for(auto& g : grains_)
            g.active = false;
    }

    // Block-rate parameter update
    // rate: the tape's speed (PITCH on the LOOP page) — where the play head
    //       goes, so where new grains start
    // grain_ratio: extra transposition of the grains only (GRAINS alt layer)
    void SetParams(int count, float size_s, float spray, float spread, int env, float rate,
                   float grain_ratio = 1.f)
    {
        grain_rate_ = rate * grain_ratio;
        count_    = count < 0 ? 0 : (count > kMaxGrains ? kMaxGrains : count);
        size_smp_ = size_s * sr_;
        spray_    = spray;
        spread_   = spread;
        env_sel_  = env;
        rate_     = rate;
        // Level: overlapping grains add up. Partly-correlated grains (low
        // SPRAY) add up faster than 1/sqrt(N) assumes, so use N^0.65, tuned so
        // the cloud lands near the plain loop's level; the level match in
        // gloop_fw.ino then trims the rest, and the output limiter catches peaks.
        norm_ = count_ > 0 ? 2.4f / powf(float(count_), 0.65f) : 0.f;
    }

    // Adds the cloud's output for one block into out_l / out_r.
    // Call BEFORE the tape head is advanced for this block.
    void ProcessBlock(const TapeLoop& tape, float* out_l, float* out_r, size_t n)
    {
        // ---- 1. Schedule this block's new grains (sample-accurate) ----------
        if(count_ > 0)
        {
            const float interval = size_smp_ / float(count_);
            while(spawn_timer_ < float(n))
            {
                Spawn(tape, uint16_t(spawn_timer_));
                spawn_timer_ += interval;
            }
            spawn_timer_ -= float(n);
        }
        else
            spawn_timer_ = 0.f;

        // ---- 2. Render every active grain across the block -------------------
        const uint32_t len   = tape.RegionLength();
        const uint32_t start = tape.RegionStart();
        const uint32_t last  = uint32_t(tape.RecordedLength()) - 1;
        const int16_t* fr    = tape.Frames();

        for(auto& g : grains_)
        {
            if(!g.active)
                continue;
            if(g.pos_i >= len) // region was shortened under this grain
                g.pos_i %= len;

            // Copy state into locals so it lives in registers for the loop
            uint32_t       pi = g.pos_i, ii = g.inc_i;
            float          pf = g.pos_f, fi = g.inc_f;
            float          ph = g.phase, dph = g.phase_inc;
            const float    gl = g.gain_l, gr = g.gain_r;
            const float*   et = env_->Table(g.env);
            size_t         i  = g.delay;
            g.delay           = 0;

            for(; i < n; i++)
            {
                // Tape read: region start + pos is always inside the recording
                const uint32_t a  = start + pi;
                const uint32_t b  = a < last ? a + 1 : 0;
                const float    s0 = float(fr[2 * a] + fr[2 * a + 1]);
                const float    s1 = float(fr[2 * b] + fr[2 * b + 1]);
                const float    s  = s0 + pf * (s1 - s0);

                // Envelope (phase < 1, so idx <= 255 and idx + 1 is valid)
                const float    x   = ph * float(EnvelopeTable::kSize);
                const uint32_t idx = uint32_t(x);
                const float    e0  = et[idx];
                const float    amp = e0 + (x - float(idx)) * (et[idx + 1] - e0);

                const float v = s * amp;
                out_l[i] += v * gl;
                out_r[i] += v * gr;

                // Step position (integer + fraction) and wrap in the region
                pf += fi;
                if(pf >= 1.f)
                {
                    pf -= 1.f;
                    pi++;
                }
                pi += ii;
                if(pi >= len)
                    pi -= len;

                ph += dph;
                if(ph >= 1.f)
                {
                    g.active = false;
                    break;
                }
            }
            g.pos_i = pi;
            g.pos_f = pf;
            g.phase = ph;
        }
    }

    const Grain* Grains() const { return grains_; }

    // Forget all grains (on a restart, so nothing resumes from old positions)
    void Reset()
    {
        for(auto& g : grains_)
            g.active = false;
        spawn_timer_ = 0.f;
    }

    // CPU safety valve: while set, no NEW grains start. Grains already playing
    // finish normally, so the cloud thins out smoothly instead of clicking.
    void SetThrottle(bool on) { throttle_ = on; }

    // Active grains right now (for the UI / CPU readout)
    int ActiveCount() const
    {
        int c = 0;
        for(const auto& g : grains_)
            c += g.active ? 1 : 0;
        return c;
    }

  private:
    void Spawn(const TapeLoop& tape, uint16_t offset)
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

        const double len = double(tape.RegionLength());

        // SPRAY: jitter of up to ±0.5 s (or half the loop if shorter)
        const float max_jitter = fminf(0.5f * sr_, float(len) * 0.5f);
        const float jitter     = (Rand() * 2.f - 1.f) * spray_ * max_jitter;

        // Where the play head will be at this grain's start sample
        double pos = tape.Head() + double(offset) * rate_ + jitter;
        pos        = fmod(pos, len);
        if(pos < 0.0)
            pos += len;

        // SPREAD: equal-power pan law
        const float pan   = (Rand() * 2.f - 1.f) * spread_;
        const float angle = (pan + 1.f) * 0.78539816f; // 0..pi/2
        // Level: envelope compensation x count normalisation x int16 scale
        // (x0.5 because L and R are summed to mono)
        const float lvl = env_->Gain(env_sel_) * norm_ * (0.5f / 32768.f);

        g->pos_i     = uint32_t(pos);
        g->pos_f     = float(pos - double(g->pos_i));
        g->inc_i     = uint32_t(grain_rate_); // grains play at their own pitch
        g->inc_f     = grain_rate_ - float(g->inc_i);
        g->phase     = 0.f;
        g->phase_inc = 1.f / fmaxf(size_smp_, 16.f);
        g->pan       = pan;
        g->gain_l    = cosf(angle) * lvl;
        g->gain_r    = sinf(angle) * lvl;
        g->env       = uint8_t(env_sel_);
        g->delay     = offset;
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
    float                grain_rate_  = 1.f;
    float                norm_        = 0.f;
    float                spawn_timer_ = 0.f;
    uint32_t             rng_         = 1;
    bool                 throttle_    = false;
};

} // namespace gloop

#pragma GCC pop_options
