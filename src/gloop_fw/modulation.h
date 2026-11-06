// =============================================================================
// modulation.h — one modulation source per track: an LFO or an envelope
// follower, and where it goes (LFO page)
//
// Runs at block rate (1 kHz). Each block the engine copies the track's
// Params, and Apply() moves ONE parameter of the copy (DEST) by the source
// × DEPTH. The knobs themselves never move: the screen shows where you set
// them, the sound gets the modulated copy.
//
//   LFO : SHAPE (sine, triangle, square, saw up, saw down, sample & hold,
//         drift = smooth random), RATE 0.05..10 Hz. Bipolar: it swings
//         either side of the knob. Restarts when the track starts playing,
//         so it lines up with the loop.
//   ENV : follows the level of T1, T2, T3 or the audio IN (SOURCE), measured
//         after that track's FX and before its mute (so a muted track can
//         still drive a sidechain). ~3 ms attack, RELEASE 20 ms..2 s, and a
//         -48..-6 dBFS window mapped to 0..1. Unipolar: it pushes the
//         parameter up from the knob (negative DEPTH: down).
//   DEPTH -1..1: full depth swings each destination across its whole range
//   (PITCH ±12 semitones, GRAINS ±12, RES.FRQ ±24 semitones, G.SIZE ±2
//   octaves; 0..1 parameters by ±1). VOLUME is special: the LFO is a
//   tremolo (full depth dips to silence), the ENV a gate (+) or ducker (-).
//
// REV.SZ / TONE / PRE-DLY are shared by all tracks (one reverb), so every
// track's modulation of them adds up on the shared value.
// =============================================================================
#pragma once
#include <cmath>
#include <cstdint>
#include "params.h"

namespace gloop
{

class ModSource
{
  public:
    void Init(float block_rate, uint32_t seed)
    {
        br_   = block_rate;
        rng_  = seed | 1u;
        held_ = NextRandom();
        from_ = held_;
        to_   = NextRandom();
    }

    void Restart() { phase_ = 0.f; }

    // Block rate. level: the followed source's peak in the last block
    // (only used in ENV mode). Returns the modulation signal: -1..1 (LFO)
    // or 0..1 (ENV).
    float Process(const Params& p, float level)
    {
        env_mode_ = p.lfo_mode > 0.5f;

        // Envelope follower (kept running so switching modes is smooth)
        const float a   = level > env_ ? kAttack : ReleaseCoef(p.env_release);
        env_ += a * (level - env_);
        float db        = env_ > 1e-6f ? 20.f * log10f(env_) : -120.f;
        env01_          = fminf(1.f, fmaxf(0.f, (db + 48.f) / 42.f));

        // LFO
        const float hz = 0.05f * powf(200.f, p.lfo_rate); // as Format::LfoHz
        phase_ += hz / br_;
        bool wrapped = false;
        if(phase_ >= 1.f)
        {
            phase_ -= floorf(phase_);
            wrapped = true;
        }
        if(wrapped) // a new cycle: new random values
        {
            held_ = NextRandom();
            from_ = to_;
            to_   = NextRandom();
        }
        const float ph = phase_;
        switch(int(p.lfo_shape + 0.5f))
        {
            case 0: lfo_ = sinf(ph * 6.2831853f); break;
            case 1: lfo_ = 1.f - 4.f * fabsf(ph - 0.5f); break; // 0 → -1 .. 0.5 → +1
            case 2: lfo_ = ph < 0.5f ? 1.f : -1.f; break;
            case 3: lfo_ = 2.f * ph - 1.f; break;
            case 4: lfo_ = 1.f - 2.f * ph; break;
            case 5: lfo_ = held_; break;
            default: // DRIFT: cosine glide from one random value to the next
            {
                const float k = 0.5f - 0.5f * cosf(ph * 3.1415927f);
                lfo_          = from_ + k * (to_ - from_);
                break;
            }
        }
        out_ = env_mode_ ? env01_ : lfo_;
        return out_;
    }

    float Out() const { return out_; }     // last value (UI)
    float Phase() const { return phase_; } // LFO phase 0..1 (UI)
    bool  EnvMode() const { return env_mode_; }

  private:
    static constexpr float kAttack = 0.28f; // ~3 ms at 1 kHz blocks

    float ReleaseCoef(float seconds) const
    {
        if(seconds != rel_s_) // only recalculated when RELEASE moves
        {
            rel_s_ = seconds;
            rel_k_ = 1.f - expf(-1.f / (fmaxf(0.005f, seconds) * br_));
        }
        return rel_k_;
    }

    float NextRandom() // -1..1
    {
        rng_ ^= rng_ << 13;
        rng_ ^= rng_ >> 17;
        rng_ ^= rng_ << 5;
        return float(rng_ >> 8) * (2.f / 16777216.f) - 1.f;
    }

    float         br_    = 1000.f;
    float         phase_ = 0.f;
    float         lfo_   = 0.f;
    float         env_   = 0.f;
    float         env01_ = 0.f;
    float         out_   = 0.f;
    bool          env_mode_ = false;
    float         held_ = 0.f, from_ = 0.f, to_ = 0.f;
    uint32_t      rng_  = 1u;
    mutable float rel_s_ = -1.f, rel_k_ = 0.01f;
};

// Is DEST one of the reverb's shared settings?
inline bool ModDestShared(int dest)
{
    return dest == int(ModDest::RevSize) || dest == int(ModDest::RevTone)
           || dest == int(ModDest::RevPredelay);
}

// Moves one parameter of `m` by signal × DEPTH. VOLUME isn't a Params field:
// it's multiplied into *volume (1 = untouched).
inline void ApplyMod(Params& m, int dest, float signal, float depth, bool env_mode, float* volume)
{
    const float amt = signal * depth;
    auto        u01 = [amt](float& v) { v = fminf(1.f, fmaxf(0.f, v + amt)); };
    switch(ModDest(dest))
    {
        case ModDest::Pitch: m.pitch += amt * 12.f; break;
        case ModDest::Volume:
        {
            // u: the source as 0..1 (LFO swing, or the ENV level). Positive
            // DEPTH: loud when u is high (tremolo / gate). Negative: quiet
            // when u is high (inverted tremolo / ducking).
            const float u = env_mode ? signal : 0.5f + 0.5f * signal;
            const float g = depth >= 0.f ? 1.f - depth * (1.f - u) : 1.f + depth * u;
            if(volume)
                *volume *= fminf(1.f, fmaxf(0.f, g));
            break;
        }
        case ModDest::Grains: m.grains = fminf(12.f, fmaxf(0.f, roundf(m.grains + amt * 12.f))); break;
        case ModDest::Spray: u01(m.spray); break;
        case ModDest::Spread: u01(m.spread); break;
        case ModDest::GrainMix: u01(m.grain_mix); break;
        case ModDest::GrainSize:
            m.grain_size = fminf(0.5f, fmaxf(0.01f, m.grain_size * powf(4.f, amt)));
            break;
        case ModDest::Cutoff: u01(m.cutoff); break;
        case ModDest::Reso: u01(m.reso); break;
        case ModDest::Drive: u01(m.filter_drive); break;
        case ModDest::ChorusRate: u01(m.chorus_rate); break;
        case ModDest::ChorusDepth: u01(m.chorus_depth); break;
        case ModDest::ChorusWidth: u01(m.chorus_width); break;
        case ModDest::ChorusMix: u01(m.chorus_mix); break;
        case ModDest::ResFreq: m.res_note = fminf(108.f, fmaxf(12.f, m.res_note + amt * 24.f)); break;
        case ModDest::ResStruct: u01(m.res_struct); break;
        case ModDest::ResDecay: u01(m.res_decay); break;
        case ModDest::ResBright: u01(m.res_bright); break;
        case ModDest::ResMix: u01(m.res_mix); break;
        case ModDest::RevSize: u01(m.rev_size); break;
        case ModDest::RevTone: u01(m.rev_tone); break;
        case ModDest::RevPredelay:
            m.rev_predelay = fminf(0.25f, fmaxf(0.f, m.rev_predelay + amt * 0.25f));
            break;
        case ModDest::RevMix: u01(m.rev_mix); break;
        default: break;
    }
}

} // namespace gloop
