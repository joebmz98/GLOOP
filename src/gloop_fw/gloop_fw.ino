// =============================================================================
// GLOOP — 2-track granular looper, Daisy Seed (Arduino IDE)
//
// Board setup (Arduino IDE):
//   Board manager URL : https://github.com/stm32duino/BoardManagerFiles/raw/main/package_stmicroelectronics_index.json
//   Board             : STM32 MCU based boards → Generic STM32H7 Series
//   Board part number : Daisy Seed
//   Upload method     : STM32CubeProgrammer (DFU)
//   USB support       : None   (CDC adds ~12 KB of flash)
//   Optimize          : Faster (-O2) with LTO (also speeds up the reverb/filter
//                       inside DaisyDuino). "Smallest (-Os)" also works:
//                       GLOOP's own audio code switches itself to -O2 with
//                       #pragma. Faster/Fastest WITHOUT LTO overflows.
//   Library           : DaisyDuino (Library Manager)
//
// Two threads of execution:
//
//   AUDIO CALLBACK (interrupt, every 48 samples = 1 ms)
//     1. read controls → params / transport (selected track)
//     2. push each track's params into its engine (block rate)
//     3. grains for each track, a whole block at a time
//     4. per sample: record/arm → tape playback → filter → mix + reverb → out
//
//   loop()  (whatever time is left)
//     - when a take finishes, rebuild that track's waveform overview
//     - every ~33 ms: draw the screen and send it to the display
//
// Signal flow:
//
//   IN ──┬────────────────────────────────────────────────────────┐ (monitor)
//        ├─► Track 1: tape (arm/record) ─► tape or grains ─► SVF ─┤─┐
//        └─► Track 2: tape (arm/record) ─► tape or grains ─► SVF ─┤ │ MIX sends
//                                                                  │ └► ReverbSc
//                                                                  ▼      │
//                                                       + ─► limiter ─► OUT
// =============================================================================
#include "DaisyDuino.h"

#include "browser.h"
#include "controls.h"
#include "cpu_meter.h"
#include "envelopes.h"
#include "fx.h"
#include "grains.h"
#include "metronome.h"
#include "params.h"
#include "st7789.h"
#include "tape_loop.h"
#include "ui.h"

using namespace gloop;

// ---- Memory placement --------------------------------------------------------
// LOOP LENGTH LIMIT per track. A take stops recording (and starts looping) when
// it reaches this. 30 s of 16-bit stereo = 5.8 MB per track of the 64 MB SDRAM.
// Loop length does not affect CPU load — only the number of grains does.
static constexpr size_t kSampleRate     = 48000;
static constexpr size_t kMaxLoopSeconds = 30;
static constexpr size_t kTapeFrames     = kSampleRate * kMaxLoopSeconds;
static constexpr size_t kBlockSize      = 48; // DaisyDuino default
static constexpr size_t kMaxBlock       = 64; // scratch buffers' size

static int16_t DSY_SDRAM_BSS tape_frames[kNumTracks][kTapeFrames * 2]; // L R L R
static daisysp::ReverbSc DSY_SDRAM_BSS reverb;

// ---- One track ------------------------------------------------------------------
struct Track
{
    Params      params;
    TapeLoop    tape;
    GrainCloud  cloud;
    TrackFilter  filter;
    StereoChorus chorus;
    ModalResonator res;
    float        mute_gain = 1.f; // glides to 0 when muted (no click)
    const char* tape_name = nullptr; // sample loaded onto the tape (label)
    float       grain_l[kMaxBlock];  // this block's grain output
    float       grain_r[kMaxBlock];

    // Grain level matching (see "Level match" in AudioCallback)
    float ref_ms   = 0.f; // smoothed mean-square of the plain loop at the head
    float grain_ms = 0.f; // smoothed mean-square of the raw grain output
    float gain     = 1.f; // gain that makes the grains as loud as the loop
    float gain_prev = 1.f;

    // GRAINS MIX (loop ↔ grains balance), ramped across each block
    float loop_g = 1.f, loop_g_prev = 1.f;
    float grain_g = 0.f, grain_g_prev = 0.f;
};

// Length of one 8th note in samples
static inline float EighthSamples(float bpm, float sr)
{
    return 60.f / bpm * 0.5f * sr;
}

// ---- Objects -------------------------------------------------------------------
static DaisyHardware hw;
static float         sample_rate;
static Track         tracks[kNumTracks];
// The controls edit each track's params and tape through these
static Params*       track_params[kNumTracks] = {&tracks[0].params, &tracks[1].params};
static TapeLoop*     track_tapes[kNumTracks]  = {&tracks[0].tape, &tracks[1].tape};
static EnvelopeTable envelopes;
static ReverbBus     reverb_bus;
static Controls      controls;
static St7789        display;
static Ui            ui;
static CpuMeter      cpu;
static SampleLibrary library;
static Browser       browser;
static Metronome     metronome;

// Metronome state for the UI
static volatile bool metro_active = false; // clicking right now
static volatile int  metro_beat   = 0;     // 0..3 within the bar

// Input level for the record-arm meter (peak of the last block)
static volatile float input_peak = 0.f;

// Sample loading (run from loop(), never from the audio callback)
static const char* loading_name  = nullptr; // what's being loaded
static int         loading_track = 0;
static uint32_t    load_start    = 0;
static uint32_t    last_frame    = 0;
static uint32_t    fps_start     = 0;   // FPS readout (hold SHIFT)
static int         fps_count     = 0;
static int         fps_shown     = 0;

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

// Output soft limiter: untouched below 0.8, then curves smoothly towards 1.0
// instead of hard-clipping. Dense grain clouds plus the dry input can add up
// past full scale; this keeps that from turning into harsh digital distortion.
static inline float OutputLimit(float x)
{
    const float t = 0.8f;
    const float a = fabsf(x);
    if(a <= t)
        return x;
    const float over = (a - t) / (1.f - t); // 0.. as it goes past the knee
    const float y    = t + (1.f - t) * over / (1.f + over); // → 1.0, never past
    return x < 0.f ? -y : y;
}

// Compiled for speed even under "Smallest (-Os)" (see the board settings above)
__attribute__((optimize("O2"))) void AudioCallback(float** in, float** out, size_t size)
{
    cpu.BlockStart();
    if(size > kMaxBlock)
        size = kMaxBlock;

    // ---- 0. CPU safety valve (uses the previous block's measurement) ----
    if(!throttled && (cpu.Avg() > kThrottleOn || cpu.Last() > kThrottleHot))
        throttled = true;
    else if(throttled && cpu.Avg() < kThrottleOff)
        throttled = false;

    // ---- 1. Controls (1 kHz), acting on the selected track ----
    controls.Process(track_params, track_tapes, browser);

    // ---- 1b. A take just finished (REC, PLAY or the 30 s limit) ----
    // The new loop is already playing from its start. If the other track is
    // playing it simply joins in; if nothing else was playing, every track
    // with a loop starts from its loop start together with it.
    for(int t = 0; t < kNumTracks; t++)
    {
        if(!tracks[t].tape.ConsumeTakeDone())
            continue;
        // Fit the new take to the BPM grid: as many whole 8th notes as the
        // recording holds (from the START point)
        Params&     np     = tracks[t].params;
        const float avail  = float(tracks[t].tape.RecordedLength()) * (1.f - np.start);
        np.length8         = fmaxf(1.f, floorf(avail / EighthSamples(np.bpm, sample_rate)));
        bool others_playing = false;
        for(int o = 0; o < kNumTracks; o++)
            if(o != t)
                others_playing |= tracks[o].tape.IsPlaying();
        if(!others_playing)
            for(int o = 0; o < kNumTracks; o++)
                if(o != t)
                    tracks[o].tape.Restart();
    }

    // ---- 2. Block-rate engine updates, per track ----
    static bool was_playing[kNumTracks] = {false, false};
    bool any_send = false;
    bool grain_mode[kNumTracks];
    bool playing[kNumTracks];
    for(int t = 0; t < kNumTracks; t++)
    {
        Track&        tr   = tracks[t];
        const Params& p    = tr.params;
        const float   rate = powf(2.f, p.pitch / 12.f); // varispeed

        // LENGTH: whole 8th notes at the BPM (synced), or free (SHIFT+LENGTH)
        const float len_smp = p.synced ? p.length8 * EighthSamples(p.bpm, sample_rate) : -1.f;
        tr.tape.SetRegion(p.start, p.length, p.xfade, sample_rate, len_smp);
        tr.tape.SetRate(rate);

        // GRAINS 0 = just the loop; 1..12 = that many grains layered ON TOP
        // of the loop (the loop keeps playing underneath)
        const int count = int(p.grains + 0.5f);
        grain_mode[t]   = count >= 1;
        playing[t]      = tr.tape.IsPlaying();
        if(playing[t] && !was_playing[t])
            tr.cloud.Reset(); // (re)started: begin with a fresh grain cloud
        was_playing[t] = playing[t];
        tr.cloud.SetThrottle(throttled);
        tr.cloud.SetParams(count, p.grain_size, p.spray, p.spread, int(p.envelope + 0.5f), rate,
                           powf(2.f, p.grain_pitch / 12.f)); // grains-only pitch
        tr.filter.SetParams(p.cutoff, p.reso, p.filter_type);
        tr.chorus.SetParams(p.chorus_rate, p.chorus_mix);
        tr.res.SetParams(p.res_note, p.res_bright, p.res_struct, p.res_decay, p.res_mix);
        any_send |= p.rev_mix > 0.0005f;

        // MIX: equal-power balance between the loop and the grains. At 50%
        // each is at 71% (-3 dB), which keeps the total as loud as the loop
        // on its own, because the level match makes the grains as loud as
        // the loop. GRAINS 0 = loop only, whatever the MIX.
        tr.loop_g_prev  = tr.loop_g;
        tr.grain_g_prev = tr.grain_g;
        if(grain_mode[t])
        {
            const float a = p.grain_mix * 1.5707963f;
            tr.loop_g     = cosf(a);
            tr.grain_g    = sinf(a);
        }
        else
        {
            tr.loop_g  = 1.f;
            tr.grain_g = 0.f;
        }

        // ---- 3. Grains: the whole block in one go (before the head moves) ----
        for(size_t i = 0; i < size; i++)
            tr.grain_l[i] = tr.grain_r[i] = 0.f;
        if(playing[t] && grain_mode[t])
            tr.cloud.ProcessBlock(tr.tape, tr.grain_l, tr.grain_r, size);
    }
    reverb_bus.SetParams(tracks[0].params.rev_size, any_send); // SIZE is shared

    // ---- 4. Per sample: record, tape playback, filter, mix, reverb, out ----
    //
    // Level match: in grain mode the plain loop is still read at the play
    // head (not heard) and both its level and the grains' level are tracked.
    // A slowly-moving gain then makes the grain cloud as loud as the loop, so
    // switching GRAINS 1 → 2..12, or changing SPRAY / ENV / SIZE, doesn't
    // change the volume. The gain ramps across each block (no zipper noise).
    float ref_sum[kNumTracks]   = {0.f, 0.f};
    float grain_sum[kNumTracks] = {0.f, 0.f};
    const float inv_size        = 1.f / float(size);
    float in_pk = 0.f;

    // ---- Metronome: clicks while any track is armed or recording ----
    // Beat 1 follows a loop that's playing (its loop start), so a new take
    // stays in time with it. With nothing playing it runs from the moment a
    // track is armed, and restarts on beat 1 when the recording begins.
    static bool   was_armed[kNumTracks] = {false, false};
    static bool   was_rec[kNumTracks]   = {false, false};
    static double free_pos              = 0.0;
    bool          click_on              = false;
    int           follow                = -1;
    for(int t = 0; t < kNumTracks; t++)
    {
        const TapeLoop& tp = tracks[t].tape;
        if(tp.IsArmed() && !was_armed[t]) // just armed: count from here
        {
            free_pos = 0.0;
            metronome.Resync();
        }
        was_armed[t] = tp.IsArmed();
        click_on |= tp.IsArmed() || tp.IsRecording();
        if(follow < 0 && tp.IsPlaying() && !tp.IsRecording())
            follow = t;
    }
    click_on &= controls.metronome_on();
    const float beat_len = 60.f / tracks[0].params.bpm * sample_rate; // quarter

    for(size_t i = 0; i < size; i++)
    {
        const float in_l = in[0][i];
        const float in_r = in[1][i];
        in_pk            = fmaxf(in_pk, fmaxf(fabsf(in_l), fabsf(in_r)));

        // Click position: the followed loop's head, or the free counter
        const double beat_pos = follow >= 0 ? tracks[follow].tape.Head() : free_pos;
        const float  click    = metronome.Process(beat_pos, beat_len, click_on);
        free_pos += 1.0;

        float mix_l = 0.f, mix_r = 0.f;   // dry track mix
        float send_l = 0.f, send_r = 0.f; // into the reverb
        for(int t = 0; t < kNumTracks; t++)
        {
            Track& tr = tracks[t];
            tr.tape.Write(in_l, in_r); // no-op unless armed or recording
            if(tr.tape.IsRecording() && !was_rec[t]) // take started this sample
            {
                free_pos = 0.0; // bar 1 = start of the take
                metronome.Resync();
            }
            was_rec[t] = tr.tape.IsRecording();

            float l = 0.f, r = 0.f;
            if(playing[t])
            {
                tr.tape.ReadTape(l, r); // the loop always plays
                // MIX gains, ramped across the block (no zipper noise)
                const float k  = float(i + 1) * inv_size;
                const float lg = tr.loop_g_prev + (tr.loop_g - tr.loop_g_prev) * k;
                const float gg = tr.grain_g_prev + (tr.grain_g - tr.grain_g_prev) * k;
                if(grain_mode[t])
                {
                    // The level match measures the plain loop and the raw
                    // cloud, then makes the cloud as loud as the loop; MIX
                    // then balances the two
                    const float gl = tr.grain_l[i], gr = tr.grain_r[i];
                    ref_sum[t] += l * l + r * r;
                    grain_sum[t] += gl * gl + gr * gr;
                    const float g = gg * (tr.gain_prev + (tr.gain - tr.gain_prev) * k);
                    l = l * lg + gl * g;
                    r = r * lg + gr * g;
                }
                else if(lg < 0.9999f) // GRAINS just went to 0: finish the ramp
                {
                    l *= lg;
                    r *= lg;
                }
                tr.tape.Advance();
            }
            // Per-track FX chain: filter → chorus → resonator (then reverb send)
            tr.filter.Process(l, r);
            tr.chorus.Process(l, r);
            tr.res.Process(l, r);

            // Mute (SHIFT + track button): ~4 ms glide, the track keeps
            // running underneath so it comes back in time
            tr.mute_gain += 0.005f * ((controls.muted(t) ? 0.f : 1.f) - tr.mute_gain);
            l *= tr.mute_gain;
            r *= tr.mute_gain;

            const float send = tr.params.rev_mix;
            mix_l += l * (1.f - send);
            mix_r += r * (1.f - send);
            send_l += l * send;
            send_r += r * send;
        }

        float wet_l, wet_r;
        reverb_bus.Process(send_l, send_r, wet_l, wet_r);

        // Input is monitored dry so you can hear what you're recording
        // The click goes to the output only, never into a recording
        out[0][i] = OutputLimit(mix_l + wet_l + in_l + click);
        out[1][i] = OutputLimit(mix_r + wet_r + in_r + click);
    }
    input_peak   = in_pk;
    metro_active = click_on;
    metro_beat   = metronome.BarBeat();

    // Update the level match from this block (used from the next block)
    for(int t = 0; t < kNumTracks; t++)
    {
        Track& tr     = tracks[t];
        tr.gain_prev  = tr.gain;
        if(!(playing[t] && grain_mode[t]))
            continue;
        const float k = 0.005f; // ~0.2 s smoothing at 1000 blocks/s
        tr.ref_ms += k * (ref_sum[t] * inv_size - tr.ref_ms);
        tr.grain_ms += k * (grain_sum[t] * inv_size - tr.grain_ms);
        if(tr.ref_ms > 1e-7f && tr.grain_ms > 1e-9f) // hold during silence
        {
            float target = sqrtf(tr.ref_ms / tr.grain_ms);
            target       = fminf(4.f, fmaxf(0.25f, target)); // ±12 dB at most
            tr.gain += 0.02f * (target - tr.gain);           // ~50 ms glide
        }
    }

    cpu.BlockEnd();
}

// =============================================================================
// Setup / loop
// =============================================================================
void setup()
{
    // Heartbeat: the Seed's onboard LED blinks while loop() is running, so a
    // blank screen can be told apart from firmware that never started.
    pinMode(LED_BUILTIN, OUTPUT);

    hw          = DAISY.init(DAISY_SEED, AUDIO_SR_48K);
    sample_rate = DAISY.get_samplerate();

    envelopes.Init();
    browser.Init(&library);
    for(int t = 0; t < kNumTracks; t++)
    {
        tracks[t].tape.Init(tape_frames[t], kTapeFrames);
        tracks[t].cloud.Init(sample_rate, &envelopes);
        tracks[t].filter.Init(sample_rate);
        tracks[t].chorus.Init(sample_rate);
        tracks[t].res.Init(sample_rate);
    }
    reverb_bus.Init(sample_rate, &reverb);
    metronome.Init(sample_rate);
    controls.Init(sample_rate / kBlockSize, sample_rate); // once per block

    display.Init();
    ui.Init(&display, &envelopes);

    cpu.Init(sample_rate, kBlockSize);
    DAISY.begin(AudioCallback);
}

void loop()
{
    digitalWrite(LED_BUILTIN, (millis() / 500) % 2); // heartbeat

    for(int t = 0; t < kNumTracks; t++)
        if(tracks[t].tape.ConsumeJustFinished())
        {
            tracks[t].tape_name = nullptr; // a fresh recording, not a sample
            ui.BuildOverview(tracks[t].tape, t);
        }

    const uint32_t now = millis();

    // ---- Sample loading -----------------------------------------------------------
    // STAND-IN until the SD card is wired: takes 0.9 s and only sets the tape
    // label. The SD version opens the WAV with FatFS here and streams it into
    // the track's tape buffer a chunk per loop() pass (playback paused),
    // driving the progress bar from bytes read.
    int folder, index, track;
    if(browser.ConsumeLoadRequest(folder, index, track))
    {
        loading_name  = library.Sample(folder, index).name;
        loading_track = track;
        load_start    = now;
    }
    float load_progress = -1.f;
    if(loading_name)
    {
        load_progress = float(now - load_start) / 900.f;
        if(load_progress >= 1.f)
        {
            tracks[loading_track].tape_name = loading_name;
            loading_name                    = nullptr;
            load_progress                   = -1.f;
        }
    }

    if(now - last_frame >= 33) // ~30 fps target
    {
        last_frame   = now;
        const int sel = controls.selected_track();
        Track&    tr  = tracks[sel];
        UiState   s{controls.page(), controls.shift(), &tr.params, &tr.tape, &tr.cloud, sample_rate};
        s.cpu_avg       = cpu.Avg();
        s.cpu_peak      = cpu.Peak();
        s.throttled     = throttled;
        s.browser       = &browser;
        s.track         = sel;
        s.other_tape    = &tracks[1 - sel].tape;
        s.input_peak    = input_peak;
        s.metro_active  = metro_active;
        s.metro_beat    = metro_beat;
        s.metro_enabled = controls.metronome_on();
        s.grain_size_mode = controls.grain_size_mode();
        s.muted         = controls.muted(sel);
        s.other_muted   = controls.muted(1 - sel);
        s.chorus_phase  = tr.chorus.Phase();
        s.res_level     = tr.res.Level();
        s.tape_name     = tr.tape_name;
        s.fps           = fps_shown;
        s.load_name     = loading_track == sel ? loading_name : nullptr;
        s.load_progress = loading_track == sel ? load_progress : -1.f;
        ui.Draw(s);
        display.Flush(); // only the parts that changed (see st7789.h)
        fps_count++;
    }
    if(now - fps_start >= 1000) // frames per second, for the SHIFT readout
    {
        fps_shown = fps_count;
        fps_count = 0;
        fps_start = now;
    }

    // The peak reading covers the last second
    static uint32_t last_peak_reset = 0;
    if(now - last_peak_reset >= 1000)
    {
        last_peak_reset = now;
        cpu.ResetPeak();
    }
}
