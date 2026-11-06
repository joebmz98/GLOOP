// =============================================================================
// params.cpp — the page/encoder map, and value formatting without printf
//
// Formatting is done by hand rather than snprintf("%f") because float printf
// pulls a lot of newlib code into the 128 KB internal flash.
// =============================================================================
#include "params.h"
#include <cmath>

namespace gloop
{
// label, field, min, max, step, default, integer, wrap, format
static const ParamDesc kLoopParams[4] = {
    {"START", &Params::start, 0.f, 1.f, 0.005f, 0.f, false, false, Format::Seconds},
    {"LENGTH", &Params::length8, 1.f, 256.f, 1.f, 8.f, true, false, Format::Eighths},
    {"PITCH", &Params::pitch, -24.f, 24.f, 1.f, 0.f, false, false, Format::Semitones},
    {"XFADE", &Params::xfade, 0.f, 1.f, 0.01f, 0.02f, false, false, Format::Millis},
};

static const ParamDesc kGrainsParams[4] = {
    {"GRAINS", &Params::grains, 0.f, 12.f, 1.f, 0.f, true, false, Format::Int},
    {"SPRAY", &Params::spray, 0.f, 1.f, 0.01f, 0.1f, false, false, Format::Percent},
    {"SPREAD", &Params::spread, 0.f, 1.f, 0.01f, 0.3f, false, false, Format::Percent},
    {"MIX", &Params::grain_mix, 0.f, 1.f, 0.01f, 0.5f, false, false, Format::Percent},
};

// GRAINS alt layer (GRAINS pressed again); nullptr label = keeps its job
static const ParamDesc kGrainsAlt[4] = {
    {"SIZE", &Params::grain_size, 0.01f, 0.5f, 0.005f, 0.08f, false, false, Format::Millis},
    {nullptr, nullptr, 0.f, 0.f, 0.f, 0.f, false, false, Format::Percent},
    {"G.PITCH", &Params::grain_pitch, -24.f, 24.f, 1.f, 0.f, false, false, Format::Semitones},
    {"ENV", &Params::envelope, 0.f, 9.f, 1.f, 0.f, true, true, Format::EnvName},
};

const char* const kFxPageNames[kNumFxPages] = {"FILTER", "CHORUS", "RESONATOR", "REVERB"};

// One effect per FX page. MIX is always encoder 4 (FILTER: DRIVE).
static const ParamDesc kFxParams[kNumFxPages][4] = {
    {{"CUTOFF", &Params::cutoff, 0.f, 1.f, 0.01f, 1.f, false, false, Format::Hertz},
     {"RESO", &Params::reso, 0.f, 1.f, 0.01f, 0.f, false, false, Format::Percent},
     {"TYPE", &Params::filter_type, 0.f, 3.f, 1.f, 0.f, true, true, Format::FilterType},
     {"DRIVE", &Params::filter_drive, 0.f, 1.f, 0.01f, 0.f, false, false, Format::Percent}},
    {{"RATE", &Params::chorus_rate, 0.f, 1.f, 0.01f, 0.3f, false, false, Format::LfoHz},
     {"DEPTH", &Params::chorus_depth, 0.f, 1.f, 0.01f, 0.6f, false, false, Format::Percent},
     {"WIDTH", &Params::chorus_width, 0.f, 1.f, 0.01f, 0.5f, false, false, Format::Percent},
     {"MIX", &Params::chorus_mix, 0.f, 1.f, 0.01f, 0.f, false, false, Format::Percent}},
    {{"FREQ", &Params::res_note, 24.f, 96.f, 1.f, 48.f, true, false, Format::Note},
     {"STRUCT", &Params::res_struct, 0.f, 1.f, 0.01f, 0.27f, false, false, Format::Percent},
     {"DECAY", &Params::res_decay, 0.f, 1.f, 0.01f, 0.5f, false, false, Format::Percent},
     {"MIX", &Params::res_mix, 0.f, 1.f, 0.01f, 0.f, false, false, Format::Percent}},
    {{"SIZE", &Params::rev_size, 0.f, 1.f, 0.01f, 0.5f, false, false, Format::Percent},
     {"TONE", &Params::rev_tone, 0.f, 1.f, 0.01f, 0.8f, false, false, Format::ToneHz},
     {"PRE-DLY", &Params::rev_predelay, 0.f, 0.25f, 0.005f, 0.f, false, false, Format::Millis},
     {"MIX", &Params::rev_mix, 0.f, 1.f, 0.01f, 0.f, false, false, Format::Percent}},
};

// LFO page: the LFO, or (LFO pressed again) the envelope follower
static const ParamDesc kLfoParams[4] = {
    {"SHAPE", &Params::lfo_shape, 0.f, 6.f, 1.f, 0.f, true, true, Format::LfoShape},
    {"RATE", &Params::lfo_rate, 0.f, 1.f, 0.01f, 0.4f, false, false, Format::LfoHz},
    {"DEPTH", &Params::lfo_depth, -1.f, 1.f, 0.01f, 0.f, false, false, Format::Percent},
    {"DEST", &Params::lfo_dest, 0.f, float(kNumModDests - 1), 1.f, 0.f, true, true, Format::ModDest},
};
static const ParamDesc kEnvParams[4] = {
    {"SOURCE", &Params::env_src, 0.f, 3.f, 1.f, 0.f, true, true, Format::EnvSource},
    {"RELEASE", &Params::env_release, 0.02f, 2.f, 0.01f, 0.2f, false, false, Format::Millis},
    kLfoParams[2],
    kLfoParams[3],
};

const char* const kLfoShapeNames[kNumLfoShapes]
    = {"SINE", "TRI", "SQUARE", "SAW+", "SAW-", "S&H", "DRIFT"};

const char* const kModDestNames[kNumModDests] = {
    "PITCH",  "VOLUME",  "GRAINS",  "SPRAY",   "SPREAD",  "G.MIX",  "G.SIZE", "CUTOFF",
    "RESO",   "DRIVE",   "CH.RATE", "CH.DPTH", "CH.WDTH", "CH.MIX", "RES.FRQ", "STRUCT",
    "DECAY",  "BRIGHT",  "RES.MIX", "REV.SZ",  "TONE",    "PRE-DLY", "REV.MIX"};

// Hold the FX button and turn, from any page
static const ParamDesc kFxMixer[4] = {
    {"CUTOFF", &Params::cutoff, 0.f, 1.f, 0.01f, 1.f, false, false, Format::Hertz},
    {"CH.MIX", &Params::chorus_mix, 0.f, 1.f, 0.01f, 0.f, false, false, Format::Percent},
    {"RES.MIX", &Params::res_mix, 0.f, 1.f, 0.01f, 0.f, false, false, Format::Percent},
    {"REV.MIX", &Params::rev_mix, 0.f, 1.f, 0.01f, 0.f, false, false, Format::Percent},
};

// Hold-and-turn (encoder pushed in while turning)
static const ParamDesc kResBright
    = {"BRIGHT", &Params::res_bright, 0.f, 1.f, 0.01f, 0.5f, false, false, Format::Percent};

static const ParamDesc* HoldParam(const EncoderContext& c, int enc)
{
    if(c.page == Page::Fx && c.fx_page == int(FxPage::Resonator) && enc == 1)
        return &kResBright;
    return nullptr;
}

// SHIFT + turn: tempo and free length on LOOP (shared tempo)
static const ParamDesc kBpm
    = {"BPM", &Params::bpm, 40.f, 240.f, 1.f, 120.f, true, false, Format::Int};
static const ParamDesc kFreeLength
    = {"FREE", &Params::length, 0.f, 1.f, 0.005f, 1.f, false, false, Format::Seconds};

static const ParamDesc* ShiftParam(const EncoderContext& c, int enc)
{
    if(c.page == Page::Loop && enc == 0)
        return &kBpm;
    if(c.page == Page::Loop && enc == 1)
        return &kFreeLength;
    return nullptr;
}

const ParamDesc& EncoderParam(const EncoderContext& c, int enc, bool* is_alt)
{
    const ParamDesc* alt = nullptr;
    if(c.fx_held)
        alt = &kFxMixer[enc];
    else if((c.held >> enc) & 1)
        alt = HoldParam(c, enc);
    if(!alt && !c.fx_held && c.shift)
        alt = ShiftParam(c, enc);
    if(!alt && !c.fx_held && c.page == Page::Grains && c.grains_alt && kGrainsAlt[enc].label)
        alt = &kGrainsAlt[enc];
    if(is_alt)
        *is_alt = alt != nullptr;
    if(alt)
        return *alt;
    switch(c.page)
    {
        case Page::Loop: return kLoopParams[enc];
        case Page::Grains: return kGrainsParams[enc];
        case Page::Lfo: return c.lfo_env ? kEnvParams[enc] : kLfoParams[enc];
        default: return kFxParams[c.fx_page & 3][enc];
    }
}

const char* const kEnvelopeNames[10] = {"HANN",
                                        "TRI",
                                        "TRAPEZ",
                                        "PERC",
                                        "SWELL",
                                        "GAUSS",
                                        "RAMP UP",
                                        "RAMP DN",
                                        "SPIKE",
                                        "BOX"};

// --- tiny string helpers -----------------------------------------------------
static size_t Append(char* out, size_t pos, size_t n, const char* s)
{
    while(*s && pos + 1 < n)
        out[pos++] = *s++;
    out[pos] = 0;
    return pos;
}

static size_t AppendInt(char* out, size_t pos, size_t n, int v)
{
    char tmp[12];
    int  i = 0;
    if(v < 0)
    {
        pos = Append(out, pos, n, "-");
        v   = -v;
    }
    do
    {
        tmp[i++] = char('0' + v % 10);
        v /= 10;
    } while(v && i < 11);
    while(i && pos + 1 < n)
        out[pos++] = tmp[--i];
    out[pos] = 0;
    return pos;
}

// value with a fixed number of decimals, e.g. AppendFixed(1.92, 2) → "1.92"
static size_t AppendFixed(char* out, size_t pos, size_t n, float v, int decimals)
{
    int scale = 1;
    for(int i = 0; i < decimals; i++)
        scale *= 10;
    int whole = (int)lroundf(fabsf(v) * scale);
    if(v < 0)
        pos = Append(out, pos, n, "-");
    pos = AppendInt(out, pos, n, whole / scale);
    if(decimals > 0)
    {
        pos      = Append(out, pos, n, ".");
        int frac = whole % scale;
        for(int d = scale / 10; d > 0; d /= 10)
        {
            char c[2] = {char('0' + (frac / d) % 10), 0};
            pos       = Append(out, pos, n, c);
        }
    }
    return pos;
}

void FormatValue(const ParamDesc& d,
                 float            v,
                 float            loop_seconds,
                 char*            out,
                 size_t           n)
{
    size_t p = 0;
    out[0]   = 0;
    switch(d.format)
    {
        case Format::Percent:
            p = AppendInt(out, p, n, (int)lroundf(v * 100.f));
            Append(out, p, n, "%");
            break;
        case Format::Seconds:
            p = AppendFixed(out, p, n, v * loop_seconds, 2);
            Append(out, p, n, "S");
            break;
        case Format::Millis:
            p = AppendInt(out, p, n, (int)lroundf(v * 1000.f));
            Append(out, p, n, "MS");
            break;
        case Format::Semitones:
            if(v >= 0.f)
                p = Append(out, p, n, "+");
            p = AppendInt(out, p, n, (int)lroundf(v));
            Append(out, p, n, "ST");
            break;
        case Format::Int: AppendInt(out, p, n, (int)lroundf(v)); break;
        case Format::LfoHz:
        {
            const float hz = 0.05f * powf(200.f, v);
            p = AppendFixed(out, p, n, hz, hz < 10.f ? 1 : 0);
            Append(out, p, n, "HZ");
            break;
        }
        case Format::Eighths:
            p = AppendInt(out, p, n, (int)lroundf(v));
            Append(out, p, n, "/8");
            break;
        case Format::FilterType:
        {
            static const char* const kTypes[4] = {"LP", "HP", "BP", "COMB"};
            int i = (int)lroundf(v);
            Append(out, p, n, kTypes[i < 0 ? 0 : (i > 3 ? 3 : i)]);
            break;
        }
        case Format::LfoShape:
        {
            const int i = (int)lroundf(v);
            Append(out, p, n, kLfoShapeNames[i < 0 ? 0 : (i >= kNumLfoShapes ? kNumLfoShapes - 1 : i)]);
            break;
        }
        case Format::ModDest:
        {
            const int i = (int)lroundf(v);
            Append(out, p, n, kModDestNames[i < 0 ? 0 : (i >= kNumModDests ? kNumModDests - 1 : i)]);
            break;
        }
        case Format::EnvSource:
        {
            static const char* const kSrc[4] = {"T1", "T2", "T3", "IN"};
            const int i = (int)lroundf(v);
            Append(out, p, n, kSrc[i < 0 ? 0 : (i > 3 ? 3 : i)]);
            break;
        }
        case Format::Note:
        {
            static const char* const kNames[12]
                = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
            const int m = (int)lroundf(v);
            p = Append(out, p, n, kNames[((m % 12) + 12) % 12]);
            p = AppendInt(out, p, n, m / 12 - 1); // MIDI 60 = C4
            break;
        }
        case Format::ToneHz:
        {
            const float hz = 1000.f * powf(18.f, v);
            p = AppendFixed(out, p, n, hz / 1000.f, hz < 9950.f ? 1 : 0);
            Append(out, p, n, "K");
            break;
        }
        case Format::EnvName:
        {
            int i = (int)lroundf(v);
            Append(out, p, n, kEnvelopeNames[i < 0 ? 0 : (i > 9 ? 9 : i)]);
            break;
        }
        case Format::Hertz:
        {
            float hz = 40.f * powf(450.f, v);
            if(hz >= 1000.f)
            {
                p = AppendFixed(out, p, n, hz / 1000.f, 1);
                Append(out, p, n, "K");
            }
            else
                AppendInt(out, p, n, (int)lroundf(hz));
            break;
        }
    }
}

} // namespace gloop
