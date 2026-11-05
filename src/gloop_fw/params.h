// =============================================================================
// params.h — every user-facing parameter, and how the encoders map onto them
//
// One flat struct of floats is shared by the controls (writer, audio-rate
// interrupt) and the DSP / UI (readers). 32-bit float loads and stores are
// atomic on the Cortex-M7, so no locking is needed for single values.
// =============================================================================
#pragma once
#include <cstddef>

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
    float xfade   = 0.02f; // 0..0.25 s crossfade across the loop seam
    float bpm     = 120.f; // 40..240, shared by both tracks (SHIFT + START)

    // GRAINS page
    float grains     = 0.f;   // 0 = just the loop, 1..12 grains layered on
                              //     top of the loop
    float spray      = 0.1f;  // 0..1 position randomness around the play head
    float spread     = 0.3f;  // 0..1 stereo randomness
    float envelope   = 0.f;   // 0..9 grain window shape (Quanta-style list)
    float grain_size = 0.08f; // 0.01..0.5 s   (GRAINS alt layer, encoder 1)
    float grain_pitch = 0.f;  // -24..+24 semitones, grains only (alt, enc 3)

    // FX page
    float cutoff   = 1.f;  // 0..1 → 40 Hz..18 kHz (exponential)
    float reso     = 0.f;  // 0..1
    float rev_size = 0.5f; // 0..1 → reverb feedback
    float rev_mix  = 0.f;  // 0..1 dry/wet

    // FX page 2 (FX pressed again). Chain: filter → chorus → drive → reverb
    float drive       = 0.f;  // 0..1 distortion amount (0 = off)
    float drive_tone  = 0.6f; // 0..1 dark → bright (600 Hz..18 kHz low-pass)
    float chorus_rate = 0.3f; // 0..1 → 0.05..10 Hz
    float chorus_mix  = 0.f;  // 0..1 (0 = off, 1 = classic 50/50 chorus)
};

enum class Page : int
{
    Loop   = 0,
    Grains = 1,
    Fx     = 2,
};
constexpr int kNumPages = 3;

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
};

// Describes one encoder's job on one page
struct ParamDesc
{
    const char*   label;
    float Params::*field; // pointer-to-member: which Params value it edits
    float         min, max, step, def;
    bool          integer; // round to whole numbers (grain count, envelope)
    bool          wrap;    // wrap around at the ends (envelope list)
    Format        format;
};

// kPageParams[page][encoder]
extern const ParamDesc kPageParams[kNumPages][4];

// SHIFT layer: an alternate parameter for an encoder, or nullptr when SHIFT
// just means "fine adjust" for that encoder.
const ParamDesc* ShiftParam(Page page, int encoder);

// GRAINS alt layer (GRAINS pressed again): encoder 1 = SIZE, encoder 3 =
// grain PITCH; nullptr for encoders that keep their normal job
const ParamDesc* GrainsAltParam(int encoder);

// The alt layer of a page (page button pressed again): GRAINS → SIZE /
// grain PITCH, FX → drive and chorus. nullptr = encoder keeps its job.
const ParamDesc* AltParam(Page page, int encoder);

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
