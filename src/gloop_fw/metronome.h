// =============================================================================
// metronome.h — click track while recording
//
// Clicks on every beat (quarter note at the BPM), 4 beats to the bar, with a
// higher click on beat 1. The click is mixed into the OUTPUT only, so it never
// ends up in a recording (the tapes record the input, not the output).
//
// The caller supplies the position in "beat time" each sample:
//   * following a playing loop: that loop's play head, so beat 1 is the loop
//     start and the click stays locked to the loop (and to PITCH changes)
//   * free-running: samples since the track was armed / started recording
// A click fires whenever the position crosses into a new beat (including the
// jump back to 0 when the followed loop wraps).
//
// The click: a short sine burst (2 ms attack-free, ~25 ms decay). Cheap: one
// sinf per sample only while a click is sounding.
// =============================================================================
#pragma once
#include <cmath>
#include <cstdint>

#pragma GCC push_options
#pragma GCC optimize("O2")

namespace gloop
{
class Metronome
{
  public:
    static constexpr int kBeatsPerBar = 4;

    void Init(float sample_rate)
    {
        sr_    = sample_rate;
        decay_ = expf(-1.f / (0.025f * sample_rate)); // ~25 ms
    }

    // Forget the last beat, so the next Process() clicks straight away
    void Resync() { last_beat_ = -1; }

    // pos: position in samples of beat time; beat_len: samples per beat.
    // Returns the click sample to add to the output (0 when silent).
    inline float Process(double pos, float beat_len, bool active)
    {
        if(active)
        {
            const int beat = int(pos / double(beat_len));
            if(beat != last_beat_)
            {
                last_beat_ = beat;
                const bool accent = (beat % kBeatsPerBar) == 0;
                bar_beat_         = beat % kBeatsPerBar;
                env_              = accent ? 0.32f : 0.22f;
                phase_inc_        = (accent ? 1760.f : 1320.f) / sr_;
                phase_            = 0.f;
            }
        }
        else
            last_beat_ = -1;

        if(env_ < 0.0005f)
            return 0.f;
        const float s = sinf(phase_ * 6.2831853f) * env_;
        phase_ += phase_inc_;
        if(phase_ >= 1.f)
            phase_ -= 1.f;
        env_ *= decay_;
        return s;
    }

    int BarBeat() const { return bar_beat_; } // 0..3, for the UI

  private:
    float sr_        = 48000.f;
    float decay_     = 0.999f;
    float env_       = 0.f;
    float phase_     = 0.f;
    float phase_inc_ = 0.f;
    int   last_beat_ = -1;
    int   bar_beat_  = 0;
};

} // namespace gloop

#pragma GCC pop_options
