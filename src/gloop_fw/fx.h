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
//  * TrackFilter: stereo state-variable filter, LP / HP / BP, plus a tuned
//    feedback COMB (TYPE), crossfaded over 5 ms when the type changes. CUTOFF
//    is exponential (40 Hz × 450^x ≈ 40 Hz..18 kHz) and smoothed so encoder
//    steps don't zipper. RESO is capped below self-oscillation. In COMB,
//    CUTOFF is the comb's pitch (50 Hz..12 kHz) and RESO its feedback.
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
// Most blocks are 48 samples; scratch buffers inside the FX hold this many
// (longer blocks are processed in chunks)
static constexpr int kFxChunk = 64;

// Chamberlin state-variable filter, double-sampled: the same maths as
// DaisySP's Svf (with its drive term at 0, as GLOOP always used it), but
// inline, stereo, and with the coefficients worked out once per block. The
// library's version is a function call per channel per sample, compiled at
// -Os with the rest of the libraries.
struct StereoSvf
{
    float low[2] = {0.f, 0.f}, band[2] = {0.f, 0.f};
    float o_low[2] = {0.f, 0.f}, o_high[2] = {0.f, 0.f}, o_band[2] = {0.f, 0.f};
    float f = 0.25f, damp = 2.f;

    // As Svf::SetFreq + SetRes. res_q = res^0.25 (the caller has it already)
    void Set(float hz, float res_q, float sr)
    {
        hz = hz < 0.000001f ? 0.000001f : (hz > sr * 0.5f ? sr * 0.5f - 1.f : hz);
        f  = 2.f * sinf(3.14159265358979f * fminf(0.25f, hz / (sr * 2.f)));
        damp = fminf(2.f * (1.f - res_q), fminf(2.f, 2.f / f - f * 0.5f));
    }

    // One channel, one sample (both passes); results in o_low / o_high / o_band
    inline void Run(int ch, float in)
    {
        float lo = low[ch], bp = band[ch];
        float n1 = in - damp * bp;
        lo += f * bp;
        float hi = n1 - lo;
        bp       = f * hi + bp;
        float ol = 0.5f * lo, oh = 0.5f * hi, ob = 0.5f * bp;
        n1       = in - damp * bp;
        lo += f * bp;
        hi = n1 - lo;
        bp = f * hi + bp;
        o_low[ch]  = ol + 0.5f * lo;
        o_high[ch] = oh + 0.5f * hi;
        o_band[ch] = ob + 0.5f * bp;
        low[ch]    = lo;
        band[ch]   = bp;
    }
};

class TrackFilter
{
  public:
    static constexpr int kComb    = 3;    // TYPE index of the comb
    static constexpr int kCombLen = 1024; // ~21 ms → lowest comb pitch ~47 Hz

    void Init(float sample_rate)
    {
        sr_        = sample_rate;
        cutoff_hz_ = 18000.f;
        Reset();
    }

    // Back to silence (when it's put into an FX slot)
    void Reset()
    {
        svf_ = StereoSvf(); // (SetParams sets its coefficients every block)
        xf_  = 1.f;
        ClearComb();
    }

    // Block rate. type: 0 = low-pass, 1 = high-pass, 2 = band-pass, 3 = comb.
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
        const float res   = reso * 0.9f;
        const float res_q = powf(res, 0.25f);
        svf_.Set(cutoff_hz_, res_q, sr_);
        // The SVF's band output peaks at 1/damp (-6 dB with no RESO): lift it
        // so BP sits at about the same level as LP/HP
        bp_gain_ = 1.f + (1.f - res_q);

        // Comb: CUTOFF is the pitch (delay = one period), RESO the feedback
        const float comb_hz = fminf(kCombMaxHz, fmaxf(kCombMinHz, cutoff_hz_));
        comb_target_        = sr_ / comb_hz;
        comb_fb_            = CombFeedback(reso);
        comb_comp_          = CombMakeup(comb_fb_);

        const int t = int(type + 0.5f);
        if(t != type_) // switch types with a ~5 ms crossfade (no click)
        {
            if(t == kComb && !CombRunning())
            {
                ClearComb(); // don't replay whatever was left in the line
                comb_delay_ = comb_target_;
            }
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
        const bool fading = xf_ < 1.f;
        // Only run what's audible: the SVF or the comb (both while fading
        // between them)
        if(type_ != kComb || (fading && prev_type_ != kComb))
        {
            svf_.Run(0, l);
            svf_.Run(1, r);
        }
        if(CombRunning())
            ProcessComb(l, r);
        l = Out(0, type_);
        r = Out(1, type_);
        if(fading)
        {
            xf_ += 1.f / 240.f;
            const float k = xf_ > 1.f ? 1.f : xf_;
            const float pl = Out(0, prev_type_), pr = Out(1, prev_type_);
            l = pl + k * (l - pl);
            r = pr + k * (r - pr);
        }
    }

    // A block, in place. The usual case (LP / HP / BP, not mid-crossfade)
    // runs with the filter state held in registers; the rest per sample.
    void ProcessBlock(float* __restrict l, float* __restrict r, int n)
    {
        if(xf_ >= 1.f && type_ != kComb)
        {
            StereoSvf   s   = svf_; // local copy: stays in registers
            const bool  drv = drive_on_;
            const float g = gain_, mk = makeup_, bpg = bp_gain_;
            const int   t = type_;
            for(int i = 0; i < n; i++)
            {
                float a = l[i], b = r[i];
                if(drv)
                {
                    a = SoftClip(a * g) * mk;
                    b = SoftClip(b * g) * mk;
                }
                s.Run(0, a);
                s.Run(1, b);
                if(t == 1)
                {
                    l[i] = s.o_high[0];
                    r[i] = s.o_high[1];
                }
                else if(t == 2)
                {
                    l[i] = s.o_band[0] * bpg;
                    r[i] = s.o_band[1] * bpg;
                }
                else
                {
                    l[i] = s.o_low[0];
                    r[i] = s.o_low[1];
                }
            }
            svf_ = s;
            return;
        }
        for(int i = 0; i < n; i++)
            Process(l[i], r[i]);
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

    // Shared with the UI so the drawn comb matches what you hear
    static constexpr float kCombMinHz = 50.f;
    static constexpr float kCombMaxHz = 12000.f;
    static constexpr float kCombDamp  = 0.7f; // one-pole low-pass in the loop
    static inline float CombFeedback(float reso) { return reso * 0.97f; }
    // Keeps broadband material at a similar loudness as RESO rises; tones
    // that land on a tooth are held in check by the soft clip in the loop
    static inline float CombMakeup(float fb) { return 1.25f / (1.f + 0.8f * fb * fb); }

  private:
    inline bool CombRunning() const { return type_ == kComb || (xf_ < 1.f && prev_type_ == kComb); }

    void ClearComb()
    {
        for(auto& ch : comb_buf_)
            for(auto& v : ch)
                v = 0.f;
        comb_lp_[0] = comb_lp_[1] = 0.f;
        comb_out_[0] = comb_out_[1] = 0.f;
    }

    // Feedback comb with a damping low-pass and a soft clip in the loop.
    // Output = (dry + delayed) / 2, so even at RESO 0 it's a feed-forward
    // comb (flanger-like notches); RESO adds feedback until it rings at the
    // CUTOFF pitch and its harmonics. The delay glides per sample (~4 ms),
    // so sweeping CUTOFF bends the pitch instead of clicking.
    inline void ProcessComb(float l, float r)
    {
        comb_delay_ += 0.005f * (comb_target_ - comb_delay_);
        float pos = float(comb_w_) - comb_delay_;
        if(pos < 0.f)
            pos += float(kCombLen);
        const int   i0 = int(pos);
        const int   i1 = (i0 + 1) & (kCombLen - 1);
        const float f  = pos - float(i0);
        const float in[2] = {l, r};
        for(int ch = 0; ch < 2; ch++)
        {
            const float* b = comb_buf_[ch];
            const float  d = b[i0] + f * (b[i1] - b[i0]);
            comb_lp_[ch] += kCombDamp * (d - comb_lp_[ch]);
            comb_buf_[ch][comb_w_] = SoftClip(in[ch] + comb_fb_ * comb_lp_[ch]);
            comb_out_[ch]          = 0.5f * (in[ch] + comb_lp_[ch]) * comb_comp_;
        }
        comb_w_ = (comb_w_ + 1) & (kCombLen - 1);
    }

    inline float Out(int ch, int type)
    {
        switch(type)
        {
            case 1: return svf_.o_high[ch];
            case 2: return svf_.o_band[ch] * bp_gain_;
            case kComb: return comb_out_[ch];
            default: return svf_.o_low[ch];
        }
    }

    StereoSvf    svf_;
    int          type_      = 0;
    int          prev_type_ = 0;
    float        xf_        = 1.f;
    float        bp_gain_   = 2.f;
    float        cutoff_hz_ = 18000.f;
    float        sr_        = 48000.f;
    bool         drive_on_  = false;
    float        gain_      = 1.f;
    float        makeup_    = 1.f;

    // Comb
    float comb_buf_[2][kCombLen];
    int   comb_w_      = 0;
    float comb_delay_  = 100.f; // samples, glides toward comb_target_
    float comb_target_ = 100.f;
    float comb_fb_     = 0.f;
    float comb_comp_   = 1.25f;
    float comb_lp_[2]  = {0.f, 0.f};
    float comb_out_[2] = {0.f, 0.f};
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

    void ProcessBlock(float* __restrict l, float* __restrict r, int n)
    {
        for(int i = 0; i < n; i++)
            Process(l[i], r[i]);
    }

    // Back to silence (when it's put into an FX slot)
    void Reset()
    {
        for(auto& ch : buf_)
            for(auto& v : ch)
                v = 0.f;
    }

    // Track idle (stopped and silent, see AudioCallback): the delay line
    // already holds silence, so only the LFO moves on (the UI's choir)
    void Skip(int n)
    {
        for(int i = 0; i < n; i++) // per sample, exactly as Process would
        {
            phase_ += inc_;
            if(phase_ >= 1.f)
                phase_ -= 1.f;
        }
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
            // (even modes lean left, odd right: see ProcessBlock)
            amp_[n_] = (1.f - 2.f * f) / sqrtf(mq);
            n_++;

            stretch += stiffness;
            stiffness *= stiffness < 0.f ? 0.93f : 0.98f;
            harmonic += f0;
            q *= q_loss;
        }
    }

    // A block, in place. Four modes at a time over the whole block (rather
    // than sample by sample over all the modes), so their coefficients and
    // state stay in registers: per mode per sample that's half an
    // accumulate instead of seven reads and two writes. Even modes are summed apart from
    // odd ones; the stereo spread (even left, odd right, the other side at
    // 0.55) is applied to the two sums afterwards.
    void ProcessBlock(float* __restrict l, float* __restrict r, int n)
    {
        if(!active_)
            return;
        for(int off = 0; off < n; off += kFxChunk)
        {
            const int m = n - off < kFxChunk ? n - off : kFxChunk;
            Chunk(l + off, r + off, m);
        }
    }

    // Load governor: fewer modes when the CPU is busy (6..16). The highest
    // modes go first; they're the quietest, so it mostly dulls the top end.
    void SetMaxModes(int n) { max_modes_ = n < 1 ? 1 : (n > kModes ? kModes : n); }

    float Level() const { return active_ ? level_ : 0.f; } // UI meter
    int   Modes() const { return n_; }

  private:
    void Chunk(float* __restrict l, float* __restrict r, int n)
    {
        float in[kFxChunk], ev[kFxChunk], od[kFxChunk];
        float in_acc = 0.f;
        for(int i = 0; i < n; i++)
        {
            in[i] = 0.5f * (l[i] + r[i]);
            ev[i] = od[i] = 0.f;
            in_acc += in[i] * in[i];
        }
        // Modes four at a time (even, odd, even, odd): four independent
        // filters side by side, so the in-order M7 always has another mode's
        // maths to issue while it waits on a result. One mode alone is a
        // single dependent chain and stalls (~46 → ~18 cycles per mode per
        // sample on llvm-mca's Cortex-M7 model). Leftover modes go one by one.
        int k = 0;
        for(; k + 3 < n_; k += 4)
        {
            const float g0 = g_[k], rg0 = rg_[k], h0 = h_[k], a0 = amp_[k];
            const float g1 = g_[k + 1], rg1 = rg_[k + 1], h1 = h_[k + 1], a1 = amp_[k + 1];
            const float g2 = g_[k + 2], rg2 = rg_[k + 2], h2 = h_[k + 2], a2 = amp_[k + 2];
            const float g3 = g_[k + 3], rg3 = rg_[k + 3], h3 = h_[k + 3], a3 = amp_[k + 3];
            float       p1 = s1_[k], p2 = s2_[k], q1 = s1_[k + 1], q2 = s2_[k + 1];
            float       u1 = s1_[k + 2], u2 = s2_[k + 2], v1 = s1_[k + 3], v2 = s2_[k + 3];
            for(int i = 0; i < n; i++)
            {
                const float x   = in[i];
                const float hp0 = (x - rg0 * p1 - p2) * h0;
                const float hp1 = (x - rg1 * q1 - q2) * h1;
                const float hp2 = (x - rg2 * u1 - u2) * h2;
                const float hp3 = (x - rg3 * v1 - v2) * h3;
                const float bp0 = g0 * hp0 + p1;
                const float bp1 = g1 * hp1 + q1;
                const float bp2 = g2 * hp2 + u1;
                const float bp3 = g3 * hp3 + v1;
                p1              = g0 * hp0 + bp0;
                q1              = g1 * hp1 + bp1;
                u1              = g2 * hp2 + bp2;
                v1              = g3 * hp3 + bp3;
                const float lp0 = g0 * bp0 + p2;
                const float lp1 = g1 * bp1 + q2;
                const float lp2 = g2 * bp2 + u2;
                const float lp3 = g3 * bp3 + v2;
                p2              = g0 * bp0 + lp0;
                q2              = g1 * bp1 + lp1;
                u2              = g2 * bp2 + lp2;
                v2              = g3 * bp3 + lp3;
                ev[i] += a0 * bp0 + a2 * bp2;
                od[i] += a1 * bp1 + a3 * bp3;
            }
            s1_[k]     = p1;
            s2_[k]     = p2;
            s1_[k + 1] = q1;
            s2_[k + 1] = q2;
            s1_[k + 2] = u1;
            s2_[k + 2] = u2;
            s1_[k + 3] = v1;
            s2_[k + 3] = v2;
        }
        for(; k < n_; k++)
        {
            const float g = g_[k], rg = rg_[k], h = h_[k], a = amp_[k];
            float       p1 = s1_[k], p2 = s2_[k];
            float* __restrict acc = (k & 1) ? od : ev;
            for(int i = 0; i < n; i++)
            {
                const float hp = (in[i] - rg * p1 - p2) * h;
                const float bp = g * hp + p1;
                p1             = g * hp + bp;
                const float lp = g * bp + p2;
                p2             = g * bp + lp;
                acc[i] += a * bp;
            }
            s1_[k] = p1;
            s2_[k] = p2;
        }
        float       wet_acc = 0.f, level = level_;
        const float gain = gain_, mix = mix_;
        for(int i = 0; i < n; i++)
        {
            float wl = ev[i] + 0.55f * od[i];
            float wr = 0.55f * ev[i] + od[i];
            wet_acc += 0.5f * (wl * wl + wr * wr);
            wl = SoftClip(wl * gain);
            wr = SoftClip(wr * gain);
            level += 0.001f * (fabsf(wl) + fabsf(wr) - level); // for the UI
            l[i] += mix * (wl - l[i]);
            r[i] += mix * (wr - r[i]);
        }
        level_ = level;
        in_acc_ += in_acc;
        wet_acc_ += wet_acc;
        blk_n_ += n;
    }

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
    float g_[kModes], rg_[kModes], h_[kModes], amp_[kModes];
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
