// =============================================================================
// fx.h — per-track filter, chorus and drive + one shared reverb
//
// Per track: Filter → Chorus → Drive (distortion) → reverb send / dry mix
//
// Signal flow (2 tracks):
//
//   track 1 ─ SVF LP ─ chorus ─ drive ─┬─ x(1 - MIX1) ──────────────┐
//                                      └─ x MIX1 ──┐                 │
//   track 2 ─ SVF LP ─ chorus ─ drive ─┬─ x(1 - MIX2) ──────────────┤
//                                      └─ x MIX2 ──┴─ ReverbSc ─ wet ┴─► out
//
//  * TrackFilter: stereo state-variable filter, LP / HP / BP (SHIFT +
//    CUTOFF), crossfaded over 5 ms when the type changes. CUTOFF is exponential
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

    // Block rate. type: 0 = low-pass, 1 = high-pass, 2 = band-pass
    void SetParams(float cutoff01, float reso, float type)
    {
        float target = 40.f * powf(450.f, cutoff01);
        cutoff_hz_ += (target - cutoff_hz_) * 0.15f; // one-pole smoothing
        const float res = reso * 0.9f;
        for(auto& f : svf_)
        {
            f.SetFreq(cutoff_hz_);
            f.SetRes(res);
        }
        // The SVF's band output peaks at 1/damp (-6 dB with no RESO): lift it
        // so BP sits at about the same level as LP/HP
        bp_gain_ = 1.f + (1.f - powf(res, 0.25f));

        const int t = int(type + 0.5f);
        if(t != type_) // switch types with a ~5 ms crossfade (no click)
        {
            prev_type_ = type_;
            type_      = t;
            xf_        = 0.f;
        }
    }

    // Per sample, in place
    inline void Process(float& l, float& r)
    {
        svf_[0].Process(l);
        svf_[1].Process(r);
        l = Out(0, type_);
        r = Out(1, type_);
        if(xf_ < 1.f)
        {
            xf_ += 1.f / 240.f;
            const float k = xf_ > 1.f ? 1.f : xf_;
            l = Out(0, prev_type_) + k * (l - Out(0, prev_type_));
            r = Out(1, prev_type_) + k * (r - Out(1, prev_type_));
        }
    }

  private:
    inline float Out(int ch, int type)
    {
        return type == 1 ? svf_[ch].High() : (type == 2 ? svf_[ch].Band() * bp_gain_ : svf_[ch].Low());
    }

    daisysp::Svf svf_[2];
    int          type_      = 0;
    int          prev_type_ = 0;
    float        xf_        = 1.f;
    float        bp_gain_   = 2.f;
    float        cutoff_hz_ = 18000.f;
};

// -----------------------------------------------------------------------------
// StereoChorus — two modulated delay lines (one per channel), LFOs a quarter
// cycle apart for width. RATE 0..1 → 0.05..10 Hz (exponential). MIX 0..1:
// 0 = dry, 1 = classic 50/50 chorus. The delay lines are always written, so
// turning MIX up never plays stale audio; reading is skipped at MIX 0.
// -----------------------------------------------------------------------------
class StereoChorus
{
  public:
    static constexpr int kLen = 2048; // ~42 ms at 48 kHz (power of 2)

    void Init(float sample_rate)
    {
        sr_    = sample_rate;
        base_  = 0.012f * sample_rate; // 12 ms centre delay
        depth_ = 0.005f * sample_rate; // ±5 ms sweep
        for(auto& ch : buf_)
            for(auto& v : ch)
                v = 0.f;
    }

    // Block rate
    void SetParams(float rate01, float mix)
    {
        inc_ = 0.05f * powf(200.f, rate01) / sr_; // LFO phase step
        mix_ = mix;
    }

    inline void Process(float& l, float& r)
    {
        buf_[0][w_] = l;
        buf_[1][w_] = r;
        if(mix_ > 0.001f)
        {
            const float wl = Read(0, base_ + depth_ * Tri(phase_));
            const float wr = Read(1, base_ + depth_ * Tri(phase_ + 0.25f));
            const float dry = 1.f - 0.5f * mix_, wet = 0.5f * mix_;
            l = l * dry + wl * wet;
            r = r * dry + wr * wet;
        }
        phase_ += inc_;
        if(phase_ >= 1.f)
            phase_ -= 1.f;
        w_ = (w_ + 1) & (kLen - 1);
    }

    float Phase() const { return phase_; } // for the UI

  private:
    static inline float Tri(float p) // triangle -1..1
    {
        p -= floorf(p);
        return 4.f * fabsf(p - 0.5f) - 1.f;
    }
    inline float Read(int ch, float delay) const
    {
        float       pos = float(w_) - delay;
        if(pos < 0.f)
            pos += float(kLen);
        const int   i0 = int(pos);
        const int   i1 = (i0 + 1) & (kLen - 1);
        const float f  = pos - float(i0);
        return buf_[ch][i0] + f * (buf_[ch][i1] - buf_[ch][i0]);
    }

    float buf_[2][kLen];
    int   w_     = 0;
    float sr_    = 48000.f;
    float base_  = 576.f;
    float depth_ = 240.f;
    float phase_ = 0.f;
    float inc_   = 0.f;
    float mix_   = 0.f;
};

// -----------------------------------------------------------------------------
// Drive — soft-clip distortion with a tone control.
// AMOUNT 0..1 → input gain 1..41 into a smooth tanh-like curve, with make-up
// gain so a typical signal (~0.3) comes out at about the same level. TONE
// 0..1 → a low-pass after the clipper, 600 Hz (dark) .. 18 kHz (bright).
// AMOUNT 0 bypasses it completely.
// -----------------------------------------------------------------------------
class Drive
{
  public:
    void Init(float sample_rate) { sr_ = sample_rate; }

    // Block rate
    void SetParams(float amount, float tone)
    {
        bypass_ = amount < 0.005f;
        drive_  = 1.f + amount * amount * 40.f;
        makeup_ = 0.3f / Clip(0.3f * drive_);
        const float fc = 600.f * powf(30.f, tone);
        coef_          = 1.f - expf(-6.2831853f * fc / sr_);
    }

    inline void Process(float& l, float& r)
    {
        if(bypass_)
        {
            z_[0] = l; // keep the tone filter primed: no click on enable
            z_[1] = r;
            return;
        }
        const float yl = Clip(l * drive_) * makeup_;
        const float yr = Clip(r * drive_) * makeup_;
        z_[0] += coef_ * (yl - z_[0]);
        z_[1] += coef_ * (yr - z_[1]);
        l = z_[0];
        r = z_[1];
    }

    // Smooth tanh-like soft clip (Padé), flat at ±1 beyond ±3
    static inline float Clip(float x)
    {
        if(x > 3.f)
            return 1.f;
        if(x < -3.f)
            return -1.f;
        const float x2 = x * x;
        return x * (27.f + x2) / (27.f + 9.f * x2);
    }
    float Amount() const { return drive_; }
    float Makeup() const { return makeup_; }

  private:
    float sr_     = 48000.f;
    bool  bypass_ = true;
    float drive_  = 1.f;
    float makeup_ = 1.f;
    float coef_   = 1.f;
    float z_[2]   = {0.f, 0.f};
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
