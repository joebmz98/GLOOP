// =============================================================================
// fx.h — per-track filter, chorus and resonator + one shared reverb
//
// Per track: Filter → Chorus → Resonator → reverb send / dry mix
//
// Signal flow (2 tracks):
//
//   track 1 ─ filter ─ chorus ─ reson ┬─ x(1 - MIX1) ──────────────┐
//                                      └─ x MIX1 ──┐                 │
//   track 2 ─ filter ─ chorus ─ reson ┬─ x(1 - MIX2) ──────────────┤
//                                      └─ x MIX2 ──┴─ ReverbSc ─ wet ┴─► out
//
//  * TrackFilter: stereo state-variable filter, LP / HP / BP (SHIFT +
//    CUTOFF), crossfaded over 5 ms when the type changes. CUTOFF is exponential
//    (40 Hz × 450^x ≈ 40 Hz..18 kHz) and smoothed so encoder steps don't
//    zipper. RESO is capped below self-oscillation.
//  * ReverbBus: ONE ReverbSc shared by both tracks (it's one of the most
//    expensive things in GLOOP). Each track's MIX is its send level; SIZE,
//    TONE (damping low-pass) and PRE-DLY (up to 250 ms before the reverb)
//    are shared. When every send is 0 the reverb is fed silence for 4 s so
//    its tail dies away, then skipped until a MIX comes back up.
//  * ReverbSc's delay lines and the pre-delay buffer are large, so they
//    live in SDRAM.
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

    // Block rate. type: 0 = low-pass, 1 = high-pass, 2 = band-pass.
    // drive 0..1: pushes the signal into the filter through a soft clip
    // (gain up to x16, with make-up gain so it gets grittier, not louder).
    // The clipped harmonics then go through the filter, so a low CUTOFF
    // with high RESO growls. 0 = clean (skipped). (DaisySP's own Svf drive
    // term isn't used: its cubic feedback blows up at low RESO.)
    void SetParams(float cutoff01, float reso, float type, float drive)
    {
        drive_on_ = drive > 0.005f;
        const float g = 1.f + drive * drive * 15.f;
        gain_ += 0.2f * (g - gain_);
        // Peaks match at 0.25; the extra term pulls back the loudness that
        // clipping adds (it squashes peaks, so the average comes up)
        makeup_ = 0.25f / SoftClip(0.25f * gain_) / (1.f + drive * 0.9f);
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
        if(drive_on_)
        {
            l = SoftClip(l * gain_) * makeup_;
            r = SoftClip(r * gain_) * makeup_;
        }
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

    static inline float SoftClip(float x) // tanh-like, flat at +/-1 beyond +/-3
    {
        if(x > 3.f)
            return 1.f;
        if(x < -3.f)
            return -1.f;
        const float x2 = x * x;
        return x * (27.f + x2) / (27.f + 9.f * x2);
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
    bool         drive_on_  = false;
    float        gain_      = 1.f;
    float        makeup_    = 1.f;
};

// -----------------------------------------------------------------------------
// StereoChorus — two modulated delay lines (one per channel), LFOs a quarter
// cycle apart for width. RATE 0..1 → 0.05..10 Hz (exponential). DEPTH 0..1 →
// ±0.5..8 ms sweep. WIDTH 0..1: L/R LFOs in step (mono) .. half a cycle apart
// (widest). MIX 0..1:
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
    void SetParams(float rate01, float depth01, float width01, float mix)
    {
        inc_ = 0.05f * powf(200.f, rate01) / sr_; // LFO phase step
        mix_ = mix;
        // Smoothed so turning DEPTH doesn't make the pitch jump
        depth_ += 0.1f * ((0.0005f + depth01 * 0.0075f) * sr_ - depth_);
        offset_ = 0.5f * width01;
    }

    inline void Process(float& l, float& r)
    {
        buf_[0][w_] = l;
        buf_[1][w_] = r;
        if(mix_ > 0.001f)
        {
            const float wl = Read(0, base_ + depth_ * Tri(phase_));
            const float wr = Read(1, base_ + depth_ * Tri(phase_ + offset_));
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
    float offset_ = 0.25f; // R LFO phase offset (WIDTH)
    float inc_   = 0.f;
    float mix_   = 0.f;
};

// -----------------------------------------------------------------------------
// ModalResonator — a bank of tuned band-pass "modes" that make the input ring like
// a string, tube, bar or bell. The algorithm is DaisySP's Resonator (ported
// from Mutable Instruments Rings/Plaits by Emilie Gillet, MIT licence), with
// two changes for GLOOP:
//  * the mode coefficients (a powf and a tan per mode) are worked out once
//    per BLOCK instead of once per sample, about 5x cheaper
//  * stereo for free: even modes lean left, odd modes lean right
//
//   NOTE      fundamental (MIDI note, glides ~5 ms between notes)
//   BRIGHT    0 = high modes die fast (wood) .. 1 = they ring on (glass)
//   STRUCTURE 0..0.25 squeezed partials, 0.25..0.3 harmonic (string),
//             above that stretched / inharmonic (bar, bell)
//   DECAY     how long it rings: 0 = a dull knock .. 1 = ~30 s drone
//             (DaisySP calls this "damping")
//   MIX       0 = off (and skipped: costs nothing) .. 1 = resonator only
//
// The wet level is matched to the input level automatically (like the grain
// level match), so DECAY and NOTE don't make the volume jump. A soft clip
// catches the peaks of long, loud rings.
// -----------------------------------------------------------------------------
class ModalResonator
{
  public:
    static constexpr int kModes = 16;

    void Init(float sample_rate)
    {
        sr_ = sample_rate;
        Clear();
    }

    // Block rate (also updates the level match from the last block)
    void SetParams(float note, float bright, float structure, float decay, float mix)
    {
        // Level match from the last block (rms of input vs raw wet)
        if(blk_n_ > 0)
        {
            const float k = 0.01f;
            in_ms_ += k * (in_acc_ / blk_n_ - in_ms_);
            wet_ms_ += k * (wet_acc_ / blk_n_ - wet_ms_);
            if(in_ms_ > 1e-7f && wet_ms_ > 1e-12f) // hold while silent
            {
                float target = sqrtf(in_ms_ / wet_ms_);
                target       = fminf(30.f, fmaxf(0.03f, target));
                gain_ += 0.05f * (target - gain_);
            }
        }
        in_acc_ = wet_acc_ = 0.f;
        blk_n_             = 0;

        mix_ = mix;
        if(mix_ < 0.001f)
        {
            if(active_)
                Clear(); // start silent next time it's turned up
            active_ = false;
            return;
        }
        active_ = true;

        // NOTE → Hz, with a short glide so steps don't click
        const float hz = 440.f * powf(2.f, (note - 69.f) / 12.f);
        freq_ += 0.3f * (hz - freq_);

        // ---- DaisySP Resonator::Process() coefficient maths, per block ----
        structure        = fminf(1.f, fmaxf(0.f, structure));
        decay            = fminf(1.f, fmaxf(0.f, decay));
        float stiffness  = Stiffness(structure);
        const float f0   = freq_ / sr_ * HarmonicCompensation(3, stiffness);
        float q          = 500.f * powf(2.f, decay * 79.7f / 12.f * 2.f); // q_sqrt^2
        float brightness = bright * (1.f - structure * 0.3f) * (1.f - decay * 0.3f);
        const float q_loss = brightness * (2.f - brightness) * 0.85f + 0.15f;
        float harmonic = f0, stretch = 1.f;

        n_ = 0;
        for(int i = 0; i < max_modes_; i++)
        {
            const float f = harmonic * stretch;
            if(f >= 0.45f)
                break; // the rest are above ~21 kHz: skip them (saves CPU)
            const float mq = 1.f + f * q;
            const float g  = FastTan(f);
            const float r  = 1.f / mq;
            g_[n_]   = g;
            rg_[n_]  = r + g;
            h_[n_]   = 1.f / (1.f + r * g + g * g);
            // Amplitude: Rings' attenuation towards Nyquist, scaled by
            // 1/sqrt(Q) so very resonant modes don't jump out
            const float a = (1.f - 2.f * f) / sqrtf(mq);
            al_[n_]       = a * ((i & 1) ? 0.55f : 1.f);
            ar_[n_]       = a * ((i & 1) ? 1.f : 0.55f);
            n_++;

            stretch += stiffness;
            stiffness *= stiffness < 0.f ? 0.93f : 0.98f;
            harmonic += f0;
            q *= q_loss;
        }
    }

    inline void Process(float& l, float& r)
    {
        if(!active_)
            return;
        const float in = 0.5f * (l + r);
        float       wl = 0.f, wr = 0.f;
        for(int i = 0; i < n_; i++)
        {
            const float hp = (in - rg_[i] * s1_[i] - s2_[i]) * h_[i];
            const float bp = g_[i] * hp + s1_[i];
            s1_[i]         = g_[i] * hp + bp;
            const float lp = g_[i] * bp + s2_[i];
            s2_[i]         = g_[i] * bp + lp;
            wl += al_[i] * bp;
            wr += ar_[i] * bp;
        }
        in_acc_ += in * in;
        wet_acc_ += 0.5f * (wl * wl + wr * wr);
        blk_n_++;
        wl = SoftClip(wl * gain_);
        wr = SoftClip(wr * gain_);
        level_ += 0.001f * (fabsf(wl) + fabsf(wr) - level_); // for the UI
        l += mix_ * (wl - l);
        r += mix_ * (wr - r);
    }

    // Load governor: fewer modes when the CPU is busy (6..16). The highest
    // modes go first; they're the quietest, so it mostly dulls the top end.
    void SetMaxModes(int n) { max_modes_ = n < 1 ? 1 : (n > kModes ? kModes : n); }

    float Level() const { return active_ ? level_ : 0.f; } // UI meter
    int   Modes() const { return n_; }

  private:
    void Clear()
    {
        for(int i = 0; i < kModes; i++)
            s1_[i] = s2_[i] = 0.f;
        level_ = 0.f;
    }
    static inline float SoftClip(float x) // tanh-like, flat at +/-1 beyond +/-3
    {
        if(x > 3.f)
            return 1.f;
        if(x < -3.f)
            return -1.f;
        const float x2 = x * x;
        return x * (27.f + x2) / (27.f + 9.f * x2);
    }
    static inline float FastTan(float f) // tan(pi*f), as DaisySP's fasttan
    {
        constexpr float kPi = 3.14159265f, kPi3 = kPi * kPi * kPi, kPi5 = kPi3 * kPi * kPi;
        const float     f2  = f * f;
        return f * (kPi + f2 * (0.326f * kPi3 + 0.1823f * kPi5 * f2));
    }
    static float HarmonicCompensation(int n, float stiffness)
    {
        float s = 1.f;
        for(int i = 0; i < n - 1; i++)
        {
            s += stiffness;
            stiffness *= stiffness < 0.f ? 0.93f : 0.98f;
        }
        return 1.f / s;
    }
    static float Stiffness(float sig) // DaisySP Resonator::CalcStiff
    {
        if(sig < 0.25f)
            return -(0.25f - sig) * 0.25f;
        if(sig < 0.3f)
            return 0.f;
        if(sig < 0.9f)
            return (sig - 0.3f) / 0.6f;
        sig = (sig - 0.9f) * 10.f;
        sig *= sig;
        return 1.5f - cosf(sig * 3.14159265f) * 0.5f;
    }

    float sr_ = 48000.f, freq_ = 130.8f, mix_ = 0.f;
    bool  active_ = false;
    int   n_      = 0;
    int   max_modes_ = kModes;
    float g_[kModes], rg_[kModes], h_[kModes], al_[kModes], ar_[kModes];
    float s1_[kModes], s2_[kModes];
    float in_acc_ = 0.f, wet_acc_ = 0.f, in_ms_ = 0.f, wet_ms_ = 0.f, gain_ = 1.f, level_ = 0.f;
    int   blk_n_ = 0;
};

class ReverbBus
{
  public:
    // Pre-delay buffer: up to 250 ms, stereo (lives in SDRAM)
    static constexpr int kPreLen = 16384; // power of 2, > 0.25 s at 48 kHz
    static_assert((kPreLen & (kPreLen - 1)) == 0, "kPreLen must be a power of 2");

    void Init(float sample_rate, daisysp::ReverbSc* reverb, float* pre_buf)
    {
        reverb_ = reverb;
        sr_     = sample_rate;
        pre_    = pre_buf;
        for(int i = 0; i < kPreLen * 2; i++)
            pre_[i] = 0.f;
        reverb_->Init(sample_rate);
        reverb_->SetLpFreq(10000.f);
        idle_limit_ = uint32_t(sample_rate * 4.f); // longest tail at max SIZE
        idle_       = idle_limit_;
    }

    // Block rate. any_send: is any track's MIX above zero?
    // tone 0..1 → damping low-pass 1 kHz (dark) .. 18 kHz (bright)
    // predelay: seconds (0..0.25) before the reverb starts
    void SetParams(float size, float tone, float predelay, bool any_send)
    {
        reverb_->SetFeedback(0.6f + size * 0.37f);
        reverb_->SetLpFreq(1000.f * powf(18.f, tone));
        pre_target_ = fminf(predelay * sr_, float(kPreLen - 2));
        any_send_   = any_send;
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
        // Pre-delay. The delay time glides (like a tape delay) rather than
        // jumping, so turning PRE-DLY doesn't click.
        pre_d_ += 0.0005f * (pre_target_ - pre_d_);
        pre_[pre_w_ * 2]     = in_l;
        pre_[pre_w_ * 2 + 1] = in_r;
        if(pre_d_ >= 1.f)
        {
            float pos = float(pre_w_) - pre_d_;
            if(pos < 0.f)
                pos += float(kPreLen);
            const int   i0 = int(pos);
            const int   i1 = (i0 + 1) & (kPreLen - 1);
            const float f  = pos - float(i0);
            in_l = pre_[i0 * 2] + f * (pre_[i1 * 2] - pre_[i0 * 2]);
            in_r = pre_[i0 * 2 + 1] + f * (pre_[i1 * 2 + 1] - pre_[i0 * 2 + 1]);
        }
        pre_w_ = (pre_w_ + 1) & (kPreLen - 1);
        reverb_->Process(in_l, in_r, &wet_l, &wet_r);
    }

  private:
    daisysp::ReverbSc* reverb_     = nullptr;
    bool               any_send_   = false;
    uint32_t           idle_       = 0;
    uint32_t           idle_limit_ = 192000;
    float              sr_         = 48000.f;
    float*             pre_        = nullptr;
    int                pre_w_      = 0;
    float              pre_d_      = 0.f, pre_target_ = 0.f;
};

} // namespace gloop

#pragma GCC pop_options
