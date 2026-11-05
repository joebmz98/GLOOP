// =============================================================================
// fx.h — filter + reverb
//
// Chain: stereo state-variable low-pass → ReverbSc (dry/wet).
//
//  * CUTOFF is mapped exponentially (40 Hz × 450^x ≈ 40 Hz..18 kHz) so the
//    knob feels even across octaves, and smoothed so steps from the encoder
//    don't zipper.
//  * RESO is capped below self-oscillation.
//  * ReverbSc's feedback is SIZE. The reverb runs all the time, even at 0 %
//    mix, so tails don't cut off when you turn MIX down.
//  * ReverbSc's delay lines are large, so the object lives in SDRAM.
// =============================================================================
#pragma once
#include <cmath>
#include <cstdint>
#include "DaisyDuino.h"


// Audio hot path: compiled for speed (-O2) even when the sketch is built
// with "Smallest (-Os)" to fit the Seed's 128 KB internal flash.
#pragma GCC push_options
#pragma GCC optimize("O2")

namespace gloop
{
class FxChain
{
  public:
    void Init(float sample_rate, daisysp::ReverbSc* reverb)
    {
        reverb_ = reverb;
        reverb_->Init(sample_rate);
        reverb_->SetLpFreq(10000.f);
        for(auto& f : svf_)
        {
            f.Init(sample_rate);
            f.SetDrive(0.f);
        }
        cutoff_hz_ = 18000.f;
        idle_limit_ = uint32_t(sample_rate * 4.f); // longest tail at max SIZE
    }

    // Block rate
    void SetParams(float cutoff01, float reso, float size, float mix)
    {
        float target = 40.f * powf(450.f, cutoff01);
        cutoff_hz_ += (target - cutoff_hz_) * 0.15f; // one-pole smoothing
        for(auto& f : svf_)
        {
            f.SetFreq(cutoff_hz_);
            f.SetRes(reso * 0.9f);
        }
        reverb_->SetFeedback(0.6f + size * 0.37f);
        mix_ = mix;
    }

    // Per sample, in place
    //
    // The reverb is one of the most expensive things in GLOOP, so it switches
    // itself off when MIX is at 0: it is fed silence for 4 s (so its tail dies
    // away and its delay lines end up empty), then skipped entirely until MIX
    // comes back up. Turning MIX up again starts from a clean, silent reverb.
    inline void Process(float& l, float& r)
    {
        svf_[0].Process(l);
        svf_[1].Process(r);
        l = svf_[0].Low();
        r = svf_[1].Low();

        if(mix_ > 0.0005f)
        {
            idle_ = 0;
            float wl, wr;
            reverb_->Process(l, r, &wl, &wr);
            l = l * (1.f - mix_) + wl * mix_;
            r = r * (1.f - mix_) + wr * mix_;
        }
        else if(idle_ < idle_limit_)
        {
            idle_++;
            float wl, wr;
            reverb_->Process(0.f, 0.f, &wl, &wr); // let the tail drain
        }
    }

    bool ReverbActive() const { return idle_ < idle_limit_; }

  private:
    daisysp::Svf       svf_[2];
    daisysp::ReverbSc* reverb_    = nullptr;
    float              cutoff_hz_ = 18000.f;
    float              mix_       = 0.f;
    uint32_t           idle_      = 0;
    uint32_t           idle_limit_ = 192000;
};

} // namespace gloop

#pragma GCC pop_options
