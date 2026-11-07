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

    // FX: each track has two slots, each holding one effect (FxPage, or
    // kFxNone). The signal goes slot 1 → slot 2; REVERB in either slot is the
    // send to the shared reverb, taken at the end of the track. SHIFT + FX
    // opens the slot menu; FX steps between the two slots' pages.
    int fx[2] = {0, 3}; // default FILTER → REVERB
    float none = 0.f;   // what an empty slot's encoders point at (no-op)
    // FILTER
    float cutoff       = 1.f;  // 0..1 → 40 Hz..18 kHz (exponential)
    float reso         = 0.f;  // 0..1
    float filter_type  = 0.f;  // 0 = LP, 1 = HP, 2 = BP, 3 = COMB
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

    // LFO page: one modulation source per track, either an LFO or an
    // envelope follower (LFO pressed again switches). DEPTH and DEST are
    // shared by both modes. See modulation.h.
    float lfo_mode    = 0.f;   // 0 = LFO, 1 = ENV (envelope follower)
    float lfo_shape   = 0.f;   // 0..6 SINE TRI SQUARE SAW+ SAW- S&H DRIFT
    float lfo_rate    = 0.4f;  // 0..1 → 0.05..10 Hz (exponential)
    float lfo_depth   = 0.f;   // -1..1 (negative = inverted)
    float lfo_dest    = 0.f;   // ModDest index (see kModDestNames)
    float env_src     = 0.f;   // 0..2 = T1..T3, 3 = audio IN
    float env_release = 0.2f;  // 0.02..2 s (attack is fixed, ~3 ms)
};

enum class Page : int
{
    Loop   = 0,
    Grains = 1,
    Fx     = 2,
    Lfo    = 3,
};
constexpr int kNumPages = 4;

// Tracks (T1..T3). Shared by the engine, controls and UI.
constexpr int kNumTracks = 3;
// "T1", "T2", "T3"
inline const char* TrackName(int t)
{
    static const char* const kNames[3] = {"T1", "T2", "T3"};
    return kNames[t < 0 ? 0 : (t > 2 ? 2 : t)];
}

// The effects a slot can hold (Params::fx), plus kFxNone = an empty slot
enum class FxPage : int
{
    Filter    = 0,
    Chorus    = 1,
    Resonator = 2,
    Reverb    = 3,
};
constexpr int kNumFxPages = 4;
constexpr int kFxNone     = 4; // empty slot
constexpr int kNumFxSlots = 2;
extern const char* const kFxPageNames[kNumFxPages + 1]; // + "NONE"

// Does this track have effect `e` in one of its slots?
inline bool HasFx(const Params& p, FxPage e)
{
    return p.fx[0] == int(e) || p.fx[1] == int(e);
}

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
    FilterType, // 0..3 → "LP" / "HP" / "BP" / "COMB"
    Note,       // MIDI note → "C3", "F#4"
    ToneHz,     // 0..1 → 1..18 kHz → "10K"
    LfoShape,   // 0..6 → "SINE" .. "DRIFT"
    ModDest,    // ModDest index → "CUTOFF"
    EnvSource,  // 0..3 → "T1" / "T2" / "T3" / "IN"
    Dash,       // an empty slot's encoders → blank
};

// Where a track's LFO / envelope follower goes (Params::lfo_dest)
enum class ModDest : int
{
    Pitch, Volume, Grains, Spray, Spread, GrainMix, GrainSize,
    Cutoff, Reso, Drive,
    ChorusRate, ChorusDepth, ChorusWidth, ChorusMix,
    ResFreq, ResStruct, ResDecay, ResBright, ResMix,
    RevSize, RevTone, RevPredelay, RevMix,
    Count
};
constexpr int kNumModDests = int(ModDest::Count);
extern const char* const kModDestNames[kNumModDests];
constexpr int kNumLfoShapes = 7;
extern const char* const kLfoShapeNames[kNumLfoShapes];

// Describes one encoder's job on one page
// Where CUTOFF leaves the sound untouched for each filter type
// (LP fully up, HP fully down, BP in the middle). A comb is never "open":
// its reset is the middle too (~850 Hz).
inline float FilterOpenCutoff(float type)
{
    const int t = int(type + 0.5f);
    return t == 0 ? 1.f : (t == 1 ? 0.f : 0.5f);
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

// SHIFT + FX: the selected track's FX slot menu. Level 0 lists the two
// slots; pushing an encoder on one opens level 1, the list of effects
// (FILTER, CHORUS, RESONATOR, REVERB, NONE). Any encoder turns, any push picks.
struct FxMenu
{
    bool open   = false;
    int  level  = 0; // 0 = slots, 1 = effects for `slot`
    int  cursor = 0; // highlighted row
    int  slot   = 0; // the slot being changed (level 1)
};

// SHIFT + LFO: pick the selected track's modulation source
struct LfoMenu
{
    bool open   = false;
    int  cursor = 0; // 0 = LFO, 1 = ENVELOPE follower
};

// Everything that decides what an encoder edits right now
struct EncoderContext
{
    Page    page       = Page::Loop;
    int     fx_page    = 0;     // FX page: which slot (0 / 1)
    int     fx[2]      = {0, 3}; // the selected track's slot effects
    bool    grains_alt = false; // GRAINS pressed again: alt layer
    bool    shift      = false; // SHIFT held
    uint8_t held       = 0;     // bit i: encoder i pushed in (hold + turn)
    bool    fx_held    = false; // FX button held: the FX mixer, any page
    bool    lfo_env    = false; // LFO page: the track's source is the ENV
};

// The parameter encoder `enc` edits in this context. Priority:
//   FX held        → mixer: slot 1's first + last parameter (e.g. CUTOFF,
//                    DRIVE), then slot 2's (e.g. SIZE, MIX)
//   encoder held   → its hold-and-turn parameter, if it has one
//   SHIFT          → its SHIFT parameter, if it has one (else fine adjust)
//   GRAINS alt     → SIZE / G.PITCH / ENV
//   LFO page       → SHAPE / RATE / DEPTH / DEST, or in ENV mode
//                    SOURCE / RELEASE / DEPTH / DEST
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
