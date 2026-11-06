// =============================================================================
// record_sync.h — timing maths for recording in time with a playing loop
//
// Kept separate (and free of hardware) so it can be tested on a computer.
//
// Grid lock: when a take starts while another loop plays, the take is tied to
// that loop's 8th-note grid. At the trigger we find the grid point nearest to
// "now" and which frame of the take it is; if it's before the start of the
// take (we started a little late), the take gets that much silence in front.
// When the take finishes, its loop starts at that frame, holds whole 8ths,
// and its play head is set to how long ago that grid point was, so it joins
// in exactly in phase.
// =============================================================================
#pragma once
#include <cmath>
#include <cstdint>

namespace gloop
{
struct GridPoint
{
    double   idx; // take frame on the grid (after padding)
    uint32_t pad; // frames of silence to put in front of the take
};

// pos: the playing loop's position in real samples since its loop start
// e: one 8th note in samples; rec_len: frames in the take so far (the last
// one was written at "now")
inline GridPoint FindGridPoint(double pos, double e, uint32_t rec_len)
{
    const double g   = floor(pos / e + 0.5) * e; // nearest 8th
    double       idx = double(rec_len) - 1.0 + (g - pos);
    uint32_t     pad = 0;
    if(idx < 0.0)
    {
        pad = uint32_t(ceil(-idx));
        idx += double(pad);
    }
    return {idx, pad};
}

// Finished grid-locked take of rec_len frames: loop START (0..1 as the START
// parameter counts it), length in 8ths, and where the head should be now
struct GridFit
{
    float  start01;
    float  eighths;
    double head; // in tape samples (at rate 1)
};
inline GridFit FitGridLocked(double rec_len, double idx, double e, double min_loop, double unit)
{
    // Whole units (bars) if there's at least one (e.g. the 30 s limit cut
    // it short), else whole 8ths
    const double units = floor((rec_len - idx) / unit + 0.01);
    const double n     = units >= 1.0 ? floor(units * unit / e + 0.5)
                                      : fmax(1.0, floor((rec_len - idx) / e + 0.01));
    const double room = rec_len - min_loop;
    GridFit f;
    f.start01 = room > 0.0 ? float(fmin(1.0, idx / room)) : 0.f;
    f.eighths = float(n);
    f.head    = fmod(rec_len - idx, n * e);
    return f;
}

// Length unit for a grid-locked take: one bar (8 eighths) if the playing loop
// is a whole number of bars, otherwise the playing loop's own length.
inline double GridUnit(bool loop_synced, float loop_eighths, double loop_len_real, double e)
{
    const int n = int(loop_eighths + 0.5f);
    if(loop_synced && n > 0 && n % 8 == 0)
        return 8.0 * e;
    return loop_len_real;
}

// Count-in length in samples. With a loop playing (pos = its position in real
// samples): to its first bar line at least 4 beats away. Otherwise 4 beats.
inline int32_t CountInSamples(bool loop_playing, double pos, double beat)
{
    if(!loop_playing)
        return int32_t(4.0 * beat);
    const double bar = 4.0 * beat;
    return int32_t(ceil((pos + 4.0 * beat) / bar) * bar - pos);
}

} // namespace gloop
