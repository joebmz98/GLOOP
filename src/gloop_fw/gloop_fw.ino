// =============================================================================
// GLOOP — granular looper prototype, 1 track, Daisy Seed (Arduino IDE)
//
// Board setup (Arduino IDE):
//   Board manager URL : https://github.com/stm32duino/BoardManagerFiles/raw/main/package_stmicroelectronics_index.json
//   Board             : STM32 MCU based boards → Generic STM32H7 Series
//   Board part number : Daisy Seed
//   Upload method     : STM32CubeProgrammer (DFU)
//   USB support       : None   (CDC adds ~12 KB of flash)
//   Optimize          : Smallest (-Os default). The audio code switches
//                       itself to -O2 with #pragma, so it stays fast while
//                       the whole sketch fits the Seed's 128 KB flash.
//   Library           : DaisyDuino (Library Manager)
//
// Two threads of execution:
//
//   AUDIO CALLBACK (interrupt, every 48 samples = 1 ms)
//     1. read controls → params / transport
//     2. push params into the engine (block rate)
//     3. per sample: record → tape or grains → filter → reverb → out
//
//   loop()  (whatever time is left)
//     - when a take finishes, rebuild the waveform overview
//     - every ~33 ms: draw the screen and send it to the display
//
// Signal flow:
//
//   IN ──┬──────────────────────────────────────────────┐ (monitor)
//        └─► TapeLoop (record) ─► play head ─┬─ GRAINS=0 ─► tape read ─┐
//                                            └─ GRAINS>0 ─► GrainCloud ┤
//                                                                      ▼
//                                                    SVF LP ─► ReverbSc ─► + ─► OUT
// =============================================================================
#include "DaisyDuino.h"

#include "browser.h"
#include "controls.h"
#include "cpu_meter.h"
#include "envelopes.h"
#include "fx.h"
#include "grains.h"
#include "params.h"
#include "st7789.h"
#include "tape_loop.h"
#include "ui.h"

using namespace gloop;

// ---- Memory placement --------------------------------------------------------
// 60 s of stereo float audio = 23 MB → SDRAM (64 MB, initialised by DAISY.init).
static constexpr size_t kSampleRate = 48000;
static constexpr size_t kMaxSeconds = 60;
static constexpr size_t kTapeSize   = kSampleRate * kMaxSeconds;
static constexpr size_t kBlockSize  = 48; // DaisyDuino default

static float DSY_SDRAM_BSS              tape_l[kTapeSize];
static float DSY_SDRAM_BSS              tape_r[kTapeSize];
static daisysp::ReverbSc DSY_SDRAM_BSS reverb;

// ---- Objects -------------------------------------------------------------------
static DaisyHardware hw;
static float         sample_rate;
static Params        params;
static TapeLoop      tape;
static EnvelopeTable envelopes;
static GrainCloud    cloud;
static FxChain       fx;
static Controls      controls;
static St7789        display;
static Ui            ui;
static CpuMeter      cpu;
static SampleLibrary library;
static Browser       browser;

// Sample loading (run from loop(), never from the audio callback)
static const char* tape_name    = nullptr; // what's on the tape now
static const char* loading_name = nullptr; // what's being loaded
static uint32_t    load_start   = 0;
static uint32_t      last_frame = 0;

// ---- CPU safety valve ----------------------------------------------------------
// If the audio callback gets close to its time budget, stop starting new grains
// until it drops back. Playing grains finish naturally, so this sounds like the
// cloud thinning slightly, never like a click. Two thresholds (hysteresis) stop
// it flickering on and off.
static constexpr float kThrottleOn  = 0.80f; // average load to start limiting
static constexpr float kThrottleHot = 0.90f; // or any single block above this
static constexpr float kThrottleOff = 0.70f; // average load to stop limiting
static volatile bool   throttled    = false;

// =============================================================================
// Audio
// =============================================================================
// Compiled for speed even under "Smallest (-Os)" (see the board settings above)
__attribute__((optimize("O2"))) void AudioCallback(float** in, float** out, size_t size)
{
    cpu.BlockStart();

    // ---- 0. CPU safety valve (uses the previous block's measurement) ----
    if(!throttled && (cpu.Avg() > kThrottleOn || cpu.Last() > kThrottleHot))
        throttled = true;
    else if(throttled && cpu.Avg() < kThrottleOff)
        throttled = false;
    cloud.SetThrottle(throttled);

    // ---- 1. Controls (1 kHz) ----
    controls.Process(params, tape, browser);

    // ---- 2. Block-rate engine updates ----
    const float rate = powf(2.f, params.pitch / 12.f); // varispeed

    tape.SetRegion(params.start, params.length, params.xfade, sample_rate);
    tape.SetRate(rate);

    const int  grain_count = int(params.grains + 0.5f);
    const bool grain_mode  = grain_count > 0;
    cloud.SetParams(grain_count,
                    params.grain_size,
                    params.spray,
                    params.spread,
                    int(params.envelope + 0.5f),
                    rate);

    fx.SetParams(params.cutoff, params.reso, params.rev_size, params.rev_mix);

    // ---- 3. Samples ----
    for(size_t i = 0; i < size; i++)
    {
        const float in_l = in[0][i];
        const float in_r = in[1][i];

        tape.Write(in_l, in_r); // no-op unless recording

        float l = 0.f, r = 0.f;
        if(tape.IsPlaying())
        {
            if(grain_mode)
                cloud.Process(tape, l, r);
            else
                tape.ReadTape(l, r);
            tape.Advance();
        }

        fx.Process(l, r);

        // Input is monitored dry so you can hear what you're recording
        out[0][i] = l + in_l;
        out[1][i] = r + in_r;
    }

    cpu.BlockEnd();
}

// =============================================================================
// Setup / loop
// =============================================================================
void setup()
{
    hw          = DAISY.init(DAISY_SEED, AUDIO_SR_48K);
    sample_rate = DAISY.get_samplerate();

    envelopes.Init();
    browser.Init(&library);
    tape.Init(tape_l, tape_r, kTapeSize);
    cloud.Init(sample_rate, &envelopes);
    fx.Init(sample_rate, &reverb);
    controls.Init(sample_rate / kBlockSize); // controls run once per block

    display.Init();
    ui.Init(&display, &envelopes);

    cpu.Init(sample_rate, kBlockSize);
    DAISY.begin(AudioCallback);
}

void loop()
{
    if(tape.ConsumeJustFinished())
        ui.BuildOverview(tape);

    const uint32_t now = millis();

    // ---- Sample loading -----------------------------------------------------------
    // STAND-IN until the SD card is wired: takes 0.9 s and only sets the tape
    // label. The SD version opens the WAV with FatFS here and streams it into
    // the track's tape buffer a chunk per loop() pass (playback paused),
    // driving the progress bar from bytes read.
    int folder, index;
    if(browser.ConsumeLoadRequest(folder, index))
    {
        loading_name = library.Sample(folder, index).name;
        load_start   = now;
    }
    float load_progress = -1.f;
    if(loading_name)
    {
        load_progress = float(now - load_start) / 900.f;
        if(load_progress >= 1.f)
        {
            tape_name     = loading_name;
            loading_name  = nullptr;
            load_progress = -1.f;
        }
    }

    if(now - last_frame >= 33) // ~30 fps target
    {
        last_frame = now;
        UiState s{controls.page(), controls.shift(), &params, &tape, &cloud, sample_rate};
        s.cpu_avg   = cpu.Avg();
        s.cpu_peak  = cpu.Peak();
        s.throttled = throttled;
        s.browser        = &browser;
        s.track_selected = controls.selected_track() == 0;
        s.t2_selected    = controls.selected_track() == 1;
        s.tape_name      = tape_name;
        s.load_name      = loading_name;
        s.load_progress  = load_progress;
        ui.Draw(s);
        display.Flush();
    }

    // The peak reading covers the last second
    static uint32_t last_peak_reset = 0;
    if(now - last_peak_reset >= 1000)
    {
        last_peak_reset = now;
        cpu.ResetPeak();
    }
}
