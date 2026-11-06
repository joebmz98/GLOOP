// =============================================================================
// params.h — every user-facing parameter, and how the encoders map onto them
//
// One flat struct of floats is shared by the controls (writer, audio-rate
// interrupt) and the DSP / UI (readers). 32-bit float loads and stores are
// atomic on the Cortex-M7, so no locking is needed for single values.
// =============================================================================
#pragma once
#include <cstddef>
#include <cstdint>

namespace gloop
{
struct Params
{
    // LOOP page
    float start   = 0.f;   // 0..1  start point within the recording
    float length8 = 8.f;   // loop length in 8th notes at the BPM (synced)
    float length  = 1.f;   // 0..1  free length: fraction of what remains
                           //       after start (SHIFT + LENGTH)
    bool  synced  = true;  // true: length8 (BPM grid); false: free length
    float pitch   = 0.f;   // -24..+24 semitones (varispeed, like tape)
    float xfade   = 0.02f; // 0..1 s crossfade across the loop seam (max half the loop)
    float bpm     = 120.f; // 40..240, shared by both tracks (SHIFT + START)

    // GRAINS page
    float grains     = 0.f;   // 0 = just the loop, 1..12 grains layered on
                              //     top of the loop
    float spray      = 0.1f;  // 0..1 position randomness around the play head
    float spread     = 0.3f;  // 0..1 stereo randomness
    float grain_mix  = 0.5f;  // 0 = loop only .. 1 = grains only (GRAINS ≥ 1)
    float envelope   = 0.f;   // 0..9 grain window shape (alt layer, enc 4)
    float grain_size = 0.08f; // 0.01..0.5 s   (GRAINS alt layer, encoder 1)
    float grain_pitch = 0.f;  // -24..+24 semitones, grains only (alt, enc 3)

    // FX pages (FX cycles them). Chain: filter → chorus → resonator → reverb
    // FILTER
    float cutoff       = 1.f;  // 0..1 → 40 Hz..18 kHz (exponential)
    float reso         = 0.f;  // 0..1
    float filter_type  = 0.f;  // 0 = LP, 1 = HP, 2 = BP
    float filter_drive = 0.f;  // 0..1 drive into the filter (0 = clean)
    // CHORUS
    float chorus_rate  = 0.3f; // 0..1 → 0.05..10 Hz
    float chorus_depth = 0.6f; // 0..1 → ±0.5..8 ms sweep
    float chorus_width = 0.5f; // 0..1 mono .. widest
    float chorus_mix   = 0.f;  // 0..1 (0 = off, 1 = classic 50/50 chorus)
    // RESONATOR
    float res_note   = 48.f;   // MIDI note 24..96 (C1..C7), default C3
    float res_struct = 0.27f;  // 0..1, 0.25..0.3 = harmonic
    float res_decay  = 0.5f;   // 0..1 knock → ~30 s drone
    float res_mix    = 0.f;    // 0..1 (0 = off)
    float res_bright = 0.5f;   // 0..1 wood → glass (hold encoder 2 + turn)
    // REVERB (SIZE, TONE and PRE-DLY are shared: there's one reverb)
    float rev_size     = 0.5f; // 0..1 → reverb feedback
    float rev_tone     = 0.8f; // 0..1 → damping 1 kHz..18 kHz
    float rev_predelay = 0.f;  // 0..0.25 s
    float rev_mix      = 0.f;  // 0..1 send (dry/wet)
};

enum class Page : int
{
    Loop   = 0,
    Grains = 1,
    Fx     = 2,
};
constexpr int kNumPages = 3;

// Tracks (T1..T3). Shared by the engine, controls and UI.
constexpr int kNumTracks = 3;
// "T1", "T2", "T3"
inline const char* TrackName(int t)
{
    static const char* const kNames[3] = {"T1", "T2", "T3"};
    return kNames[t < 0 ? 0 : (t > 2 ? 2 : t)];
}

// The FX page is four pages, one per effect (FX cycles, SHIFT + FX goes back)
enum class FxPage : int
{
    Filter    = 0,
    Chorus    = 1,
    Resonator = 2,
    Reverb    = 3,
};
constexpr int kNumFxPages = 4;
extern const char* const kFxPageNames[kNumFxPages];

// How a value is printed on the screen
enum class Format
{
    Percent,   // 0..1  → "35%"
    Seconds,   // 0..1 of the loop, shown in seconds
    Millis,    // seconds → "40MS"
    Semitones, // → "+7ST"
    Int,       // → "12"
    EnvName,   // → "PERC"
    Hertz,     // 0..1 cutoff → "2.4K"
    Eighths,   // 8th-note count → "6/8", "16/8"
    LfoHz,     // 0..1 → 0.05..10 Hz → "0.8HZ"
    FilterType, // 0/1/2 → "LP" / "HP" / "BP"
    Note,       // MIDI note → "C3", "F#4"
    ToneHz,     // 0..1 → 1..18 kHz → "10K"
};

// Describes one encoder's job on one page
// Where CUTOFF leaves the sound untouched for each filter type
// (LP fully up, HP fully down, BP in the middle)
inline float FilterOpenCutoff(float type)
{
    const int t = int(type + 0.5f);
    return t == 1 ? 0.f : (t == 2 ? 0.5f : 1.f);
}

struct ParamDesc
{
    const char*   label;
    float Params::*field; // pointer-to-member: which Params value it edits
    float         min, max, step, def;
    bool          integer; // round to whole numbers (grain count, envelope)
    bool          wrap;    // wrap around at the ends (envelope list)
    Format        format;
};

// Everything that decides what an encoder edits right now
struct EncoderContext
{
    Page    page       = Page::Loop;
    int     fx_page    = 0;     // FxPage on the FX page
    bool    grains_alt = false; // GRAINS pressed again: alt layer
    bool    shift      = false; // SHIFT held
    uint8_t held       = 0;     // bit i: encoder i pushed in (hold + turn)
    bool    fx_held    = false; // FX button held: the FX mixer, any page
};

// The parameter encoder `enc` edits in this context. Priority:
//   FX held        → mixer: CUTOFF / CHORUS MIX / RES MIX / REVERB MIX
//   encoder held   → its hold-and-turn parameter, if it has one
//   SHIFT          → its SHIFT parameter, if it has one (else fine adjust)
//   GRAINS alt     → SIZE / G.PITCH / ENV
//   otherwise      → the page's normal job
// *is_alt is set when it isn't the page's normal job (label shown inverted).
const ParamDesc& EncoderParam(const EncoderContext& c, int enc, bool* is_alt = nullptr);

// Envelope shape names, shared by the UI and the formatter
extern const char* const kEnvelopeNames[10];

// Writes a short upper-case display string for a value, e.g. "0.48S".
// `loop_seconds` lets Start/Length show real time instead of a fraction.
void FormatValue(const ParamDesc& d,
                 float            value,
                 float            loop_seconds,
                 char*            out,
                 size_t           n);

} // namespace gloop
