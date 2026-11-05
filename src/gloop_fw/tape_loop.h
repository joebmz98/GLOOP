// =============================================================================
// tape_loop.h — the "tape": record buffer, loop region, play head, seam xfade
//
// Model: one recording lives in SDRAM. START/LENGTH carve a region out of it.
// A play head moves through the region at `rate` (2^(pitch/12)), which is
// varispeed: pitch and speed change together, like a real tape machine.
//
// Storage: 16-bit, stereo INTERLEAVED (L0 R0 L1 R1 ...).
//  * Half the memory of float, so twice the loop time per MB.
//  * Left and right of the same moment sit side by side, so one cache line
//    fetch from SDRAM serves both channels. With 16-bit samples a 32-byte cache
//    line holds 8 stereo frames instead of 4 per channel — far fewer slow SDRAM
//    reads for the grains, which is where most of their time went.
//  * 16-bit is 96 dB of range, plenty for a looper.
//
// Record arm: REC first ARMS the tape. It keeps a 10 ms rolling pre-roll of
// the input and starts recording the moment the input crosses the threshold,
// copying the pre-roll in first so the attack of the first note isn't lost.
//
// Region start/length are whole samples, so the grains can index with plain
// integers. The play head is a double: at the end of a long loop a float can't
// hold the fractional position precisely enough for clean pitch-shifting.
// =============================================================================
#pragma once
#include <cmath>
#include <cstddef>
#include <cstdint>

// Audio hot path: compiled for speed (-O2) even when the sketch is built
// with "Smallest (-Os)" to fit the Seed's 128 KB internal flash.
#pragma GCC push_options
#pragma GCC optimize("O2")

namespace gloop
{
class TapeLoop
{
  public:
    static constexpr uint32_t kMinLoop = 480;               // 10 ms at 48 kHz
    static constexpr float    kToFloat = 1.f / 32768.f;     // int16 → ±1.0
    static constexpr uint32_t kPreroll = 480;               // 10 ms kept before the trigger
    static constexpr float    kArmThreshold = 0.02f;        // ≈ -34 dBFS input level

    // frames: interleaved stereo int16 buffer of 2 * capacity values
    void Init(int16_t* frames, size_t capacity)
    {
        buf_     = frames;
        cap_     = capacity;
        rec_len_ = 0;
    }

    // ---- Transport -----------------------------------------------------------
    // Arm: wait for input. The current loop keeps playing until it arrives.
    void Arm()
    {
        if(recording_)
            return;
        pre_count_ = 0;
        pre_pos_   = 0;
        armed_     = true;
    }
    void Disarm() { armed_ = false; }

    // Recording always starts a fresh take at the beginning of the buffer.
    void StartRecording()
    {
        armed_     = false;
        playing_   = false;
        rec_len_   = 0;
        head_      = 0.0;
        recording_ = true;
    }

    // Stopping a take makes it the loop and starts playback immediately.
    void StopRecording()
    {
        recording_ = false;
        if(rec_len_ < kMinLoop)
        {
            rec_len_ = 0; // too short, discard
            return;
        }
        head_          = 0.0;
        playing_       = true; // a finished take plays straight away
        just_finished_ = true; // tells the UI to rebuild its waveform overview
        take_done_     = true; // tells the transport (audio side)
    }

    void SetPlaying(bool p) { playing_ = p && rec_len_ > 0 && !recording_; }

    // Play from the start of the loop region (if there is a loop)
    void Restart()
    {
        head_    = 0.0;
        playing_ = rec_len_ > 0 && !recording_;
    }
    void Stop() { playing_ = false; }

    // Called for every input sample; does nothing unless armed or recording.
    inline void Write(float l, float r)
    {
        if(armed_)
        {
            // Keep the last 10 ms of input in a small ring
            pre_[2 * pre_pos_]     = ToInt16(l);
            pre_[2 * pre_pos_ + 1] = ToInt16(r);
            pre_pos_               = pre_pos_ + 1 < kPreroll ? pre_pos_ + 1 : 0;
            if(pre_count_ < kPreroll)
                pre_count_++;
            if(fabsf(l) < kArmThreshold && fabsf(r) < kArmThreshold)
                return;
            // Triggered: start the take with the pre-roll (this sample included)
            StartRecording();
            uint32_t from = (pre_pos_ + kPreroll - pre_count_) % kPreroll;
            for(uint32_t k = 0; k < pre_count_; k++)
            {
                uint32_t j            = (from + k) % kPreroll;
                buf_[2 * rec_len_]     = pre_[2 * j];
                buf_[2 * rec_len_ + 1] = pre_[2 * j + 1];
                rec_len_++;
            }
            return;
        }
        if(!recording_)
            return;
        buf_[2 * rec_len_]     = ToInt16(l);
        buf_[2 * rec_len_ + 1] = ToInt16(r);
        if(++rec_len_ >= cap_)
            StopRecording(); // loop length limit reached
    }

    // ---- Region & speed (block rate) ----------------------------------------
    void SetRegion(float start01, float length01, float xfade_s, float sr)
    {
        if(rec_len_ == 0)
            return;
        const uint32_t total = rec_len_;
        start_ = uint32_t(start01 * float(total - kMinLoop));
        len_   = kMinLoop + uint32_t(length01 * float(total - start_ - kMinLoop));
        // start_ + len_ <= total always holds: grains rely on it
        xfade_ = fminf(xfade_s * sr, float(len_) * 0.5f);
        if(head_ >= len_)
            head_ = fmod(head_, double(len_));
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
        const double idx        = double(start_) + head_;
        const float  fade_start = float(len_) - xfade_;
        ReadAbs(idx, l, r);
        if(xfade_ > 1.f && head_ > fade_start)
        {
            const float g = float(head_ - fade_start) / xfade_; // 0 → 1
            const float a = sqrtf(1.f - g);
            const float b = sqrtf(g);
            float       l2, r2;
            ReadAbs(idx - double(len_), l2, r2); // same distance before start
            l = l * a + l2 * b;
            r = r * a + r2 * b;
        }
    }

    // Linear-interpolated stereo read anywhere in the recording (wraps)
    inline void ReadAbs(double idx, float& l, float& r) const
    {
        const double n = double(rec_len_);
        if(idx >= n)
            idx -= n;
        if(idx < 0.0)
            idx += n;
        const uint32_t i0 = uint32_t(idx);
        const uint32_t i1 = (i0 + 1 < rec_len_) ? i0 + 1 : 0;
        const float    f  = float(idx - double(i0));
        const float    l0 = buf_[2 * i0], r0 = buf_[2 * i0 + 1];
        l = (l0 + f * (float(buf_[2 * i1]) - l0)) * kToFloat;
        r = (r0 + f * (float(buf_[2 * i1 + 1]) - r0)) * kToFloat;
    }

    // ---- State for the grains and the UI ------------------------------------
    bool     IsRecording() const { return recording_; }
    bool     IsArmed() const { return armed_; }
    bool     IsPlaying() const { return playing_; }
    bool     HasLoop() const { return rec_len_ > 0 && !recording_; }
    double   Head() const { return head_; }               // offset in region
    uint32_t RegionLength() const { return len_; }        // samples
    uint32_t RegionStart() const { return start_; }       // samples
    float    Xfade() const { return xfade_; }             // samples
    float    Rate() const { return rate_; }
    size_t   RecordedLength() const { return rec_len_; }
    size_t   Capacity() const { return cap_; }
    const int16_t* Frames() const { return buf_; }        // interleaved L R

    // Peak of one stereo frame, 0..1 (for the UI's waveform overview)
    float FramePeak(size_t i) const
    {
        int a = buf_[2 * i] < 0 ? -buf_[2 * i] : buf_[2 * i];
        int b = buf_[2 * i + 1] < 0 ? -buf_[2 * i + 1] : buf_[2 * i + 1];
        return float(a > b ? a : b) * kToFloat;
    }

    // True once after a take finishes — read by the audio callback's transport
    bool ConsumeTakeDone()
    {
        bool f     = take_done_;
        take_done_ = false;
        return f;
    }

    // True once after a take finishes (consumed by the main loop)
    bool ConsumeJustFinished()
    {
        bool f         = just_finished_;
        just_finished_ = false;
        return f;
    }

  private:
    static inline int16_t ToInt16(float x)
    {
        x *= 32767.f;
        if(x > 32767.f)
            x = 32767.f;
        if(x < -32768.f)
            x = -32768.f;
        return int16_t(lrintf(x));
    }

    int16_t*          buf_     = nullptr;
    size_t            cap_     = 0;
    volatile uint32_t rec_len_ = 0;
    double            head_    = 0.0;
    uint32_t          start_   = 0;
    uint32_t          len_     = kMinLoop;
    float             xfade_   = 0.f;
    float             rate_    = 1.f;
    volatile bool     recording_     = false;
    volatile bool     armed_         = false;
    int16_t           pre_[2 * kPreroll];   // pre-roll ring (internal RAM)
    uint32_t          pre_pos_       = 0;
    uint32_t          pre_count_     = 0;
    volatile bool     playing_       = false;
    volatile bool     just_finished_ = false;
    volatile bool     take_done_     = false;
};

} // namespace gloop

#pragma GCC pop_options
