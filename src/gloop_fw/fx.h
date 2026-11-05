// =============================================================================
// fx.h — per-track filter + one shared reverb
//
// Signal flow (2 tracks):
//
//   track 1 ─ SVF LP ─┬─ x(1 - MIX1) ─────────────────┐
//                     └─ x MIX1 ──┐                    │
//   track 2 ─ SVF LP ─┬─ x(1 - MIX2) ─────────────────┤
//                     └─ x MIX2 ──┴─ ReverbSc ─ wet ───┴─► out
//
//  * TrackFilter: stereo state-variable low-pass. CUTOFF is exponential
//    (40 Hz × 450^x ≈ 40 Hz..18 kHz) and smoothed so encoder steps don't
//    zipper. RESO is capped below self-oscillation.
//  * ReverbBus: ONE ReverbSc shared by both tracks (it's one of the most
//    expensive things in GLOOP). Each track's MIX is its send level; SIZE is
//    shared. When every send is 0 the reverb is fed silence for 4 s so its
//    tail dies away, then skipped until a MIX comes back up.
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
class TrackFilter
{
  public:
    void Init(float sample_rate)
    {
        for(auto& f : svf_)
        {
            f.Init(sample_rate);
            f.SetDrive(0.f);
        }
        cutoff_hz_ = 18000.f;
    }

    // Block rate
    void SetParams(float cutoff01, float reso)
    {
        float target = 40.f * powf(450.f, cutoff01);
        cutoff_hz_ += (target - cutoff_hz_) * 0.15f; // one-pole smoothing
        for(auto& f : svf_)
        {
            f.SetFreq(cutoff_hz_);
            f.SetRes(reso * 0.9f);
        }
    }

    // Per sample, in place
    inline void Process(float& l, float& r)
    {
        svf_[0].Process(l);
        svf_[1].Process(r);
        l = svf_[0].Low();
        r = svf_[1].Low();
    }

  private:
    daisysp::Svf svf_[2];
    float        cutoff_hz_ = 18000.f;
};

class ReverbBus
{
  public:
    void Init(float sample_rate, daisysp::ReverbSc* reverb)
    {
        reverb_ = reverb;
        reverb_->Init(sample_rate);
        reverb_->SetLpFreq(10000.f);
        idle_limit_ = uint32_t(sample_rate * 4.f); // longest tail at max SIZE
        idle_       = idle_limit_;
    }

    // Block rate. any_send: is any track's MIX above zero?
    void SetParams(float size, bool any_send)
    {
        reverb_->SetFeedback(0.6f + size * 0.37f);
        any_send_ = any_send;
    }

    // Per sample: send in, wet out
    inline void Process(float in_l, float in_r, float& wet_l, float& wet_r)
    {
        if(any_send_)
            idle_ = 0;
        else if(idle_ < idle_limit_)
        {
            idle_++;
            in_l = in_r = 0.f; // let the tail drain
        }
        else
        {
            wet_l = wet_r = 0.f; // fully idle: skip the reverb
            return;
        }
        reverb_->Process(in_l, in_r, &wet_l, &wet_r);
    }

  private:
    daisysp::ReverbSc* reverb_     = nullptr;
    bool               any_send_   = false;
    uint32_t           idle_       = 0;
    uint32_t           idle_limit_ = 192000;
};

} // namespace gloop

#pragma GCC pop_options
