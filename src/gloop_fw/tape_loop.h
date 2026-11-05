// =============================================================================
// tape_loop.h — the "tape": record buffer, loop region, play head, seam xfade
//
// Model: one long recording lives in SDRAM. START/LENGTH carve a region out of
// it. A play head moves through the region at `rate` (2^(pitch/12)), which is
// varispeed: pitch and speed change together, like a real tape machine.
//
// Positions are doubles. A 60 s buffer is 2.88 M samples; a float only has 24
// bits of mantissa, so near the end of the buffer the fractional part (which
// the interpolator needs) would be quantised to ~1/4 sample and pitch-shifted
// playback would get grainy. The H750 has a double-precision FPU, so this is
// cheap.
// =============================================================================
#pragma once
#include <cmath>
#include <cstddef>


// Audio hot path: compiled for speed (-O2) even when the sketch is built
// with "Smallest (-Os)" to fit the Seed's 128 KB internal flash.
#pragma GCC push_options
#pragma GCC optimize("O2")

namespace gloop
{
class TapeLoop
{
  public:
    static constexpr float kMinLoop = 480.f; // 10 ms at 48 kHz

    void Init(float* left, float* right, size_t capacity)
    {
        buf_[0]  = left;
        buf_[1]  = right;
        cap_     = capacity;
        rec_len_ = 0;
    }

    // ---- Transport -----------------------------------------------------------
    // Recording always starts a fresh take at the beginning of the buffer.
    void StartRecording()
    {
        playing_   = false;
        rec_len_   = 0;
        head_      = 0.0;
        recording_ = true;
    }

    // Stopping a take makes it the loop and starts playback immediately.
    void StopRecording()
    {
        recording_ = false;
        if(rec_len_ < (size_t)kMinLoop)
        {
            rec_len_ = 0; // too short, discard
            return;
        }
        head_          = 0.0;
        playing_       = true;
        just_finished_ = true; // tells the UI to rebuild its waveform overview
    }

    void SetPlaying(bool p) { playing_ = p && rec_len_ > 0; }

    // Called for every input sample; does nothing unless recording.
    inline void Write(float l, float r)
    {
        if(!recording_)
            return;
        buf_[0][rec_len_] = l;
        buf_[1][rec_len_] = r;
        if(++rec_len_ >= cap_)
            StopRecording(); // buffer full
    }

    // ---- Region & speed (block rate) ----------------------------------------
    void SetRegion(float start01, float length01, float xfade_s, float sr)
    {
        if(rec_len_ == 0)
            return;
        float total = float(rec_len_);
        start_      = start01 * (total - kMinLoop);
        len_        = kMinLoop + length01 * (total - start_ - kMinLoop);
        xfade_      = fminf(xfade_s * sr, len_ * 0.5f);
        if(head_ >= len_)
            head_ = fmod(head_, (double)len_);
    }

    void SetRate(float r) { rate_ = r; }

    // ---- Per-sample ------------------------------------------------------------
    inline void Advance()
    {
        head_ += rate_;
        if(head_ >= len_)
            head_ -= len_;
    }

    // Plain tape playback. In the last `xfade_` samples of the region we fade
    // into the audio that sits just BEFORE the region start, so that when the
    // head wraps back to the start the waveform continues smoothly instead of
    // clicking. Equal-power (sqrt) gains keep the level steady.
    inline void ReadTape(float& l, float& r) const
    {
        double idx = start_ + head_;
        float  fade_start = len_ - xfade_;
        if(xfade_ > 1.f && head_ > fade_start)
        {
            float  g    = float(head_ - fade_start) / xfade_; // 0 → 1
            float  a    = sqrtf(1.f - g);
            float  b    = sqrtf(g);
            double idx2 = idx - len_; // same distance before the region start
            l           = ReadAbs(0, idx) * a + ReadAbs(0, idx2) * b;
            r           = ReadAbs(1, idx) * a + ReadAbs(1, idx2) * b;
        }
        else
        {
            l = ReadAbs(0, idx);
            r = ReadAbs(1, idx);
        }
    }

    // Read at an offset inside the region (wraps within the region). Grains use
    // this so they never wander outside the loop.
    inline float ReadRegion(int ch, double offset) const
    {
        while(offset >= len_)
            offset -= len_;
        while(offset < 0.0)
            offset += len_;
        return ReadAbs(ch, start_ + offset);
    }

    // Linear-interpolated read anywhere in the recording (wraps the recording).
    inline float ReadAbs(int ch, double idx) const
    {
        const double n = double(rec_len_);
        while(idx >= n)
            idx -= n;
        while(idx < 0.0)
            idx += n;
        size_t i0 = (size_t)idx;
        size_t i1 = (i0 + 1 < rec_len_) ? i0 + 1 : 0;
        float  f  = float(idx - double(i0));
        float  a  = buf_[ch][i0];
        return a + f * (buf_[ch][i1] - a);
    }

    // ---- State for the grains and the UI ------------------------------------
    bool   IsRecording() const { return recording_; }
    bool   IsPlaying() const { return playing_; }
    bool   HasLoop() const { return rec_len_ > 0 && !recording_; }
    double Head() const { return head_; }          // offset in region
    float  RegionLength() const { return len_; }   // samples
    float  RegionStart() const { return start_; }  // samples
    float  Xfade() const { return xfade_; }        // samples
    float  Rate() const { return rate_; }
    size_t RecordedLength() const { return rec_len_; }
    size_t Capacity() const { return cap_; }
    const float* Channel(int ch) const { return buf_[ch]; }

    // True once after a take finishes (consumed by the main loop)
    bool ConsumeJustFinished()
    {
        bool f         = just_finished_;
        just_finished_ = false;
        return f;
    }

  private:
    float*        buf_[2]  = {nullptr, nullptr};
    size_t        cap_     = 0;
    volatile size_t rec_len_ = 0;
    double        head_    = 0.0;
    float         start_   = 0.f;
    float         len_     = kMinLoop;
    float         xfade_   = 0.f;
    float         rate_    = 1.f;
    volatile bool recording_     = false;
    volatile bool playing_       = false;
    volatile bool just_finished_ = false;
};

} // namespace gloop

#pragma GCC pop_options
