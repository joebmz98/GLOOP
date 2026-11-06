// =============================================================================
// midi_clock.h — follow the BPM of an external MIDI clock
//
// TRS MIDI in (through an optocoupler) arrives on D14 (pin 15), USART1 RX,
// at 31,250 baud. Note: on the H750, PB7 (D14) belongs to two UARTs, and the
// core's default pin entry for it is LPUART1, not USART1 — opening the port
// by pin number (the core's Serial / Serial1) picks the wrong one. So the
// port is created here from USART1's own pin entries: PB_7_ALT1 (RX, D14)
// and PB_6_ALT2 (TX, D13). Only the real-time Timing
// Clock byte (0xF8, 24 per quarter note) is used; everything else is ignored.
//
// Poll() runs in the audio callback (1 kHz), so each clock is time-stamped to
// within 1 ms. The tempo is measured across the last 48 clocks (2 beats),
// which averages that out to about ±0.1%, then rounded to a whole BPM with a
// little hysteresis: LENGTH is counted in 8th notes, so a BPM that flickered
// would keep changing the loop lengths, and a new tempo is only taken once
// it has held for a beat (not every step of the glide). No clock for 0.5 s = back to GLOOP's
// own tempo (it keeps the last MIDI BPM).
// =============================================================================
#pragma once
#include <Arduino.h>
#include <cmath>

namespace gloop
{
// The core's serial class is HardwareSerial up to 2.x and Uart from 3.0
#if defined(STM32_CORE_VERSION_MAJOR) && STM32_CORE_VERSION_MAJOR >= 3
using MidiPort = Uart;
#else
using MidiPort = HardwareSerial;
#endif

inline MidiPort& MidiUart()
{
    static MidiPort uart(PB_7_ALT1, PB_6_ALT2); // USART1: RX D14, TX D13
    return uart;
}

class MidiClockIn
{
  public:
    static constexpr int kSpan = 48; // clocks per measurement (2 beats)

    void Init(float sample_rate)
    {
        sr_ = sample_rate;
        MidiUart().begin(31250);
    }

    // Audio callback. now: samples since start (block start is fine)
    void Poll(uint32_t now)
    {
        while(MidiUart().available() > 0)
        {
            const int b = MidiUart().read();
            last_byte_  = now; // any MIDI at all (activity dot on screen)
            got_any_    = true;
            if(b != 0xF8)
                continue; // only the timing clock matters here
            stamps_[w_] = now;
            w_          = (w_ + 1) % (kSpan + 1);
            if(count_ < kSpan + 1)
                count_++;
            last_ = now;
            if(count_ > kSpan)
            {
                // Oldest stamp is kSpan clocks ago
                const uint32_t span = now - stamps_[w_];
                if(span > 0)
                {
                    const float bpm = 60.f * sr_ * float(kSpan) / 24.f / float(span);
                    if(bpm >= 40.f && bpm <= 240.f)
                    {
                        // Commit a new tempo only once it has held steady
                        // for a beat (24 clocks): during a tempo change the
                        // average glides, and every step would re-cut the
                        // BPM-synced loop lengths
                        const float cand = roundf(bpm);
                        if(!active_)
                            bpm_ = cand; // first lock: take it straight away
                        else if(fabsf(bpm - bpm_) > 0.6f)
                        {
                            steady_  = cand == pending_ ? steady_ + 1 : 0;
                            pending_ = cand;
                            if(steady_ >= 24)
                                bpm_ = cand;
                        }
                        else
                            steady_ = 0;
                    }
                    active_ = true;
                }
            }
        }
        if(active_ && now - last_ > uint32_t(0.5f * sr_))
        {
            active_ = false; // clock stopped: back to our own tempo
            count_  = 0;
        }
    }

    bool  Active() const { return active_; }
    // Any MIDI byte in the last 100 ms: shows the input circuit works even if
    // the source isn't sending clock (many only send it while playing)
    bool Activity(uint32_t now) const { return got_any_ && now - last_byte_ < uint32_t(0.1f * sr_); }
    float Bpm() const { return bpm_; }

  private:
    float    sr_     = 48000.f;
    uint32_t stamps_[kSpan + 1] = {};
    int      w_      = 0;
    int      count_  = 0;
    uint32_t last_   = 0;
    uint32_t last_byte_ = 0;
    bool     got_any_   = false;
    bool     active_ = false;
    float    bpm_    = 120.f;
    float    pending_ = 0.f; // a new tempo waiting to prove it's steady
    int      steady_  = 0;
};

} // namespace gloop
