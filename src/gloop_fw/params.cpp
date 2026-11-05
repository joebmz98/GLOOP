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
const ParamDesc kPageParams[kNumPages][4] = {
    // LOOP
    {{"START", &Params::start, 0.f, 1.f, 0.005f, 0.f, false, false, Format::Seconds},
     {"LENGTH", &Params::length8, 1.f, 256.f, 1.f, 8.f, true, false, Format::Eighths},
     {"PITCH", &Params::pitch, -24.f, 24.f, 1.f, 0.f, false, false, Format::Semitones},
     {"XFADE", &Params::xfade, 0.f, 0.25f, 0.005f, 0.02f, false, false, Format::Millis}},
    // GRAINS
    {{"GRAINS", &Params::grains, 0.f, 12.f, 1.f, 0.f, true, false, Format::Int},
     {"SPRAY", &Params::spray, 0.f, 1.f, 0.01f, 0.1f, false, false, Format::Percent},
     {"SPREAD", &Params::spread, 0.f, 1.f, 0.01f, 0.3f, false, false, Format::Percent},
     {"ENV", &Params::envelope, 0.f, 9.f, 1.f, 0.f, true, true, Format::EnvName}},
    // FX
    {{"CUTOFF", &Params::cutoff, 0.f, 1.f, 0.01f, 1.f, false, false, Format::Hertz},
     {"RESO", &Params::reso, 0.f, 1.f, 0.01f, 0.f, false, false, Format::Percent},
     {"SIZE", &Params::rev_size, 0.f, 1.f, 0.01f, 0.5f, false, false, Format::Percent},
     {"MIX", &Params::rev_mix, 0.f, 1.f, 0.01f, 0.f, false, false, Format::Percent}},
};

static const ParamDesc kGrainSize
    = {"SIZE", &Params::grain_size, 0.01f, 0.5f, 0.005f, 0.08f, false, false, Format::Millis};

// SHIFT + START: tempo (shared by both tracks)
static const ParamDesc kBpm
    = {"BPM", &Params::bpm, 40.f, 240.f, 1.f, 120.f, true, false, Format::Int};

// SHIFT + LENGTH: free length, not locked to the BPM grid
static const ParamDesc kFreeLength
    = {"FREE", &Params::length, 0.f, 1.f, 0.005f, 1.f, false, false, Format::Seconds};

const ParamDesc* ShiftParam(Page page, int encoder)
{
    if(page == Page::Grains && encoder == 0)
        return &kGrainSize;
    if(page == Page::Loop && encoder == 0)
        return &kBpm;
    if(page == Page::Loop && encoder == 1)
        return &kFreeLength;
    return nullptr;
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
        case Format::Eighths:
            p = AppendInt(out, p, n, (int)lroundf(v));
            Append(out, p, n, "/8");
            break;
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
