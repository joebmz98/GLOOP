// =============================================================================
// cpu_meter.h — exact audio CPU load from the Cortex-M7 cycle counter
//
// Load = cycles spent in the audio callback / cycles available per block.
// At 400 MHz and 48 samples per block at 48 kHz there are 400,000 cycles per
// block; using 200,000 of them is 50 %.
//
// (DaisyDuino's CpuLoadMeter uses a timer that the Arduino startup never
// starts, so it would always read 0 — hence this small version.)
// =============================================================================
#pragma once
#include <Arduino.h>

namespace silt
{
class CpuMeter
{
  public:
    void Init(float sample_rate, size_t block_size)
    {
        CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk; // enable trace unit
        DWT->LAR = 0xC5ACCE55;                           // unlock DWT (M7)
        DWT->CYCCNT = 0;
        DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk; // start the cycle counter
        budget_ = float(SystemCoreClock) * float(block_size) / sample_rate;
    }

    inline void BlockStart() { t0_ = DWT->CYCCNT; }

    inline void BlockEnd()
    {
        last_ = float(DWT->CYCCNT - t0_) / budget_;
        avg_ += 0.01f * (last_ - avg_); // ~0.1 s smoothing at 1 kHz blocks
        if(last_ > peak_)
            peak_ = last_;
    }

    float Last() const { return last_; } // this block, 0..1+
    float Avg() const { return avg_; }   // smoothed
    float Peak() const { return peak_; } // worst since ResetPeak()
    void  ResetPeak() { peak_ = 0.f; }
    float CyclesPerBlock() const { return budget_; }

  private:
    float             budget_ = 1.f;
    volatile uint32_t t0_     = 0;
    volatile float    last_ = 0.f, avg_ = 0.f, peak_ = 0.f;
};

} // namespace silt
