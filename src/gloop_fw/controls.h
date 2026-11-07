// =============================================================================
// controls.h — encoders and buttons → parameters and transport
//
// Called once per audio block (1 kHz at 48 samples / 48 kHz), the rate the
// DaisyDuino Encoder/Switch debouncing expects.
//
// Everything acts on the SELECTED track (T1 or T2):
//
//   T1 / T2 / T3 (track buttons): 1st press selects that track; pressed again
//                                on the LOOP page it opens the sample browser
//                                for that track (from another page it jumps to
//                                LOOP first)
//   LOOP / GRAINS buttons      : choose which page the 4 encoders edit
//   GRAINS again (on GRAINS)   : alt layer on/off: encoder 1 = grain SIZE,
//                                encoder 3 = grain PITCH, encoder 4 =
//                                ENVELOPE (labels inverted)
//   FX (on release)            : the selected track's FX slot pages: slot 1
//                                and slot 2 (FX again = the other slot)
//   SHIFT + FX                 : FX slot menu: turn any encoder to pick slot
//                                1 or 2, push to open its effect list (FILTER,
//                                CHORUS, RESONATOR, REVERB, NONE), turn and
//                                push to choose. FX = back; SHIFT + FX = close
//   Hold FX + turn             : FX mixer from any page: slot 1's first and
//                                last parameter, then slot 2's (FX release
//                                then does nothing)
//   LFO                        : LFO page (the selected track's LFO);
//                                LFO again: switches that track between
//                                LFO and ENV (envelope follower)
//   Hold an encoder + turn     : its second parameter, where it has one
//                                (RESONATOR enc 2 = BRIGHT)
//   SHIFT + T1 / T2 / T3       : mute / unmute that track
//   Encoder turn               : edit that page's parameter (selected track)
//   SHIFT + turn               : fine adjust (x0.1), or the alternate
//                                parameter where one exists:
//                                  LOOP enc 1 (START)  → BPM (both tracks)
//                                  LOOP enc 2 (LENGTH) → FREE length
//   LENGTH                     : steps in 8th notes at the BPM. If the length
//                                was free, the first turn snaps it back onto
//                                the 8th-note grid.
//   SHIFT + LENGTH             : free length, not locked to the BPM
//   SHIFT + encoder push       : reset that parameter to its default
//   REC                        : ARM: recording starts when input arrives
//                                (locked to a playing loop's grid if one is
//                                playing). Again while armed: cancel. While
//                                recording: finish the take (a grid-locked
//                                take finishes on the nearest bar line)
//   Hold REC + PLAY            : count-in arm; PLAY then counts 4 beats and
//                                records on the beat (REC/PLAY cancels)
//   SHIFT + REC                : start recording immediately (no waiting)
//   SHIFT + PLAY               : metronome on / off (it clicks while a track
//                                is armed or recording; see metronome.h)
//   PLAY                       : BOTH tracks: stop if either is playing,
//                                otherwise start both from their loop start
//                                (in sync). If the selected track is
//                                recording, PLAY finishes the take instead.
//   Finishing a take (REC or PLAY): if the other track is already playing,
//                                the new loop joins in straight away; if
//                                nothing is playing, both start together from
//                                their loop start (see gloop_fw.ino).
//   While the browser is open  : any encoder turn moves, any push selects,
//                                a track button or a page button closes it
//
// Reverb SIZE / TONE / PRE-DLY are shared by both tracks (one reverb), as is
// the BPM, so editing them on either track updates both. Every other
// parameter is per track. Which parameter an encoder edits in each situation
// is decided in one place: EncoderParam() in params.cpp.
// =============================================================================
#pragma once
#include <cmath>
#include "DaisyDuino.h"
#include "browser.h"
#include "hardware.h"
#include "params.h"
#include "tape_loop.h"

namespace gloop
{

class Controls
{
  public:
    void Init(float update_rate, float sample_rate)
    {
        sr_ = sample_rate;
        for(int i = 0; i < 4; i++)
            enc_[i].Init(update_rate,
                         pins::kEncA[i],
                         pins::kEncB[i],
                         pins::kEncSw[i],
                         INPUT_PULLUP,
                         INPUT_PULLUP,
                         INPUT_PULLUP);
        // invert = true: pressed pulls the pin LOW
        rec_.Init(update_rate, true, pins::kBtnRec, INPUT_PULLUP);
        play_.Init(update_rate, true, pins::kBtnPlay, INPUT_PULLUP);
        page_btn_[0].Init(update_rate, true, pins::kBtnLoop, INPUT_PULLUP);
        page_btn_[1].Init(update_rate, true, pins::kBtnGrains, INPUT_PULLUP);
        page_btn_[2].Init(update_rate, true, pins::kBtnFx, INPUT_PULLUP);
        page_btn_[3].Init(update_rate, true, pins::kBtnLfo, INPUT_PULLUP);
        shift_.Init(update_rate, true, pins::kBtnShift, INPUT_PULLUP);
        track_[0].Init(update_rate, true, pins::kBtnTrack1, INPUT_PULLUP);
        track_[1].Init(update_rate, true, pins::kBtnTrack2, INPUT_PULLUP);
        track_[2].Init(update_rate, true, pins::kBtnTrack3, INPUT_PULLUP);
    }

    // params / tapes: one pointer per track
    void Process(Params* const* params, TapeLoop* const* tapes, Browser& browser)
    {
        // 1. Debounce all inputs
        for(auto& e : enc_)
            e.Debounce();
        rec_.Debounce();
        play_.Debounce();
        shift_.Debounce();
        for(auto& t : track_)
            t.Debounce();
        for(auto& b : page_btn_)
            b.Debounce();

        now_ms_++;
        shift_held_ = shift_.Pressed();

        // 2. Page selection (also closes the browser)
        //    LOOP / GRAINS act on press. GRAINS again: its alt layer.
        for(int i = 0; i < 2; i++)
            if(page_btn_[i].RisingEdge())
            {
                if(Page(i) == Page::Grains && page_ == Page::Grains && !browser.IsOpen())
                    grain_size_mode_ = !grain_size_mode_;
                else if(Page(i) != page_)
                    grain_size_mode_ = false;
                page_ = Page(i);
                browser.Close();
                menu_.open = false;
            }
        //    FX acts on RELEASE, because holding it and turning an encoder is
        //    the FX mixer: if an encoder moved while it was held, the release
        //    does nothing. Otherwise: go to FX (at the last slot), or if
        //    already there, the other slot. SHIFT + FX opens / closes the slot
        //    menu; in the menu, FX goes back a level (and from the slot list,
        //    out to that slot's page).
        daisy::Switch& fx = page_btn_[int(Page::Fx)];
        if(fx.RisingEdge())
            fx_used_ = false;
        fx_held_ = fx.Pressed() && !browser.IsOpen() && !menu_.open;
        if(fx.FallingEdge() && !fx_used_)
        {
            if(shift_held_)
            {
                if(menu_.open)
                    menu_.open = false;
                else
                {
                    menu_.open   = true;
                    menu_.level  = 0;
                    menu_.cursor = fx_page_;
                }
            }
            else if(menu_.open && menu_.level == 1)
            {
                menu_.level  = 0; // back to the slot list
                menu_.cursor = menu_.slot;
            }
            else if(menu_.open)
            {
                menu_.open = false; // out to the highlighted slot's page
                fx_page_   = menu_.cursor;
            }
            else if(page_ == Page::Fx && !browser.IsOpen())
                fx_page_ ^= 1;
            grain_size_mode_ = false;
            page_            = Page::Fx;
            browser.Close();
        }

        //    LFO acts on press. Pressed again on the LFO page: the selected
        //    track switches between LFO and ENV (envelope follower).
        if(page_btn_[int(Page::Lfo)].RisingEdge())
        {
            if(page_ == Page::Lfo && !browser.IsOpen())
            {
                Params& sp  = *params[selected_track_];
                sp.lfo_mode = sp.lfo_mode > 0.5f ? 0.f : 1.f;
            }
            grain_size_mode_ = false;
            page_            = Page::Lfo;
            browser.Close();
            menu_.open = false;
        }

        // 3. Track buttons: select → (on LOOP) open browser → close browser.
        //    SHIFT + track button: mute / unmute that track.
        for(int t = 0; t < kNumTracks; t++)
        {
            if(!track_[t].RisingEdge())
                continue;
            if(shift_held_)
            {
                muted_[t] = !muted_[t];
                continue;
            }
            if(menu_.open) // the menu stays open, now for that track
            {
                selected_track_ = t;
                continue;
            }
            if(browser.IsOpen())
                browser.Close();
            else if(selected_track_ != t)
                selected_track_ = t;
            else if(page_ != Page::Loop)
                page_ = Page::Loop;
            else
                browser.Open(t);
        }

        Params&   p    = *params[selected_track_];
        TapeLoop& tape = *tapes[selected_track_];
        sel_params_    = &p;

        // 4. While the browser is open, the encoders drive it instead of the
        //    parameters: any turn moves, any push selects.
        if(browser.IsOpen())
        {
            int  steps  = 0;
            bool pushed = false;
            for(auto& e : enc_)
            {
                steps += e.Increment();
                pushed |= e.RisingEdge();
            }
            browser.Turn(steps, shift_held_);
            if(pushed)
                browser.Push();
            Transport(tape, tapes);
            return;
        }

        //    The FX slot menu works the same way
        if(menu_.open)
        {
            int  steps  = 0;
            bool pushed = false;
            for(auto& e : enc_)
            {
                steps += e.Increment();
                pushed |= e.RisingEdge();
            }
            const int rows = menu_.level == 0 ? kNumFxSlots : kNumFxPages + 1;
            if(steps != 0)
                menu_.cursor = ((menu_.cursor + steps) % rows + rows) % rows;
            if(pushed && menu_.level == 0)
            {
                menu_.slot   = menu_.cursor;
                menu_.level  = 1;
                menu_.cursor = p.fx[menu_.slot];
            }
            else if(pushed)
            {
                AssignFx(p, menu_.slot, menu_.cursor);
                fx_page_     = menu_.slot;
                menu_.level  = 0;
                menu_.cursor = menu_.slot;
            }
            Transport(tape, tapes);
            return;
        }

        // 5. Encoders edit the current page's parameters
        const EncoderContext ctx = Context();
        for(int i = 0; i < 4; i++)
        {
            // Which parameter (mixer / hold / SHIFT / alt layer / normal):
            // see EncoderParam() in params.cpp
            bool             alt = false;
            const ParamDesc& d   = EncoderParam(ctx, i, &alt);
            float&           v   = p.*(d.field);
            int              inc = enc_[i].Increment();
            // Push alone does nothing now (it's for hold + turn): SHIFT + push
            // resets the parameter
            const bool push = enc_[i].RisingEdge() && shift_held_;
            if(inc != 0 && fx_held_)
                fx_used_ = true; // FX was a modifier: don't change page on release
            if(inc != 0 || push)
            {
                popup_    = &d; // big value popup on screen
                popup_ms_ = now_ms_;
            }

            // LENGTH ↔ FREE: carry the current length across when switching,
            // so the loop doesn't jump
            if(inc != 0 && tape.HasLoop())
            {
                const float eighth = Eighth(p.bpm);
                if(d.field == &Params::length8 && !p.synced)
                    v = roundf(float(tape.RegionLength()) / eighth); // snap
                if(d.field == &Params::length && p.synced)
                {
                    const float room = float(tape.RecordedLength()) - float(tape.RegionStart())
                                       - float(TapeLoop::kMinLoop);
                    if(room > 0.f)
                        v = (float(tape.RegionLength()) - float(TapeLoop::kMinLoop)) / room;
                }
            }
            const float old = v;

            if(inc != 0)
            {
                float step = d.step;
                if(shift_held_ && !alt && !d.integer)
                    step *= 0.1f; // fine adjust
                v += inc * step;
                if(d.wrap)
                {
                    float span = d.max - d.min + d.step;
                    while(v > d.max)
                        v -= span;
                    while(v < d.min)
                        v += span;
                }
                v = fminf(d.max, fmaxf(d.min, v));
                if(d.integer)
                    v = roundf(v);
            }
            if(push)
                v = d.def;
            if(push && d.field == &Params::cutoff)
                v = FilterOpenCutoff(p.filter_type); // "open" for this type

            // Filter type changed: if the filter was open, keep it open for
            // the new type (an HP at LP's open position would be silent)
            // (a comb has no open position, so its pitch is left alone)
            if(d.field == &Params::filter_type && v != old && int(old + 0.5f) != 3
               && fabsf(p.cutoff - FilterOpenCutoff(old)) < 0.02f)
                p.cutoff = FilterOpenCutoff(v);

            // Which length mode is in charge
            if(inc != 0 || push)
            {
                if(d.field == &Params::length8)
                    p.synced = true;
                if(d.field == &Params::length)
                    p.synced = false;
            }

            // Synced length can't be longer than the recording allows
            if(d.field == &Params::length8 && tape.HasLoop())
            {
                const float room = float(tape.RecordedLength()) - float(tape.RegionStart());
                const float max8 = fmaxf(1.f, floorf(room / Eighth(p.bpm)));
                v                = fminf(v, max8);
            }

            // Shared settings (one reverb, one tempo): keep both tracks equal
            if(v != old
               && (d.field == &Params::rev_size || d.field == &Params::rev_tone
                   || d.field == &Params::rev_predelay || d.field == &Params::bpm))
                for(int t = 0; t < kNumTracks; t++)
                    params[t]->*(d.field) = v;
        }

        Transport(tape, tapes);
    }

    Page page() const { return page_; }
    bool shift() const { return shift_held_; }
    int  selected_track() const { return selected_track_; }
    bool metronome_on() const { return metronome_on_; }
    bool grain_size_mode() const { return grain_size_mode_; } // GRAINS alt layer
    int  fx_page() const { return fx_page_; }
    const FxMenu& fx_menu() const { return menu_; }
    bool fx_held() const { return fx_held_; }

    // What the encoders edit right now (the UI shows the same thing)
    EncoderContext Context()
    {
        EncoderContext c;
        c.page       = page_;
        c.fx_page    = fx_page_;
        c.grains_alt = grain_size_mode_;
        c.shift      = shift_held_;
        c.fx_held    = fx_held_;
        c.lfo_env    = sel_params_ && sel_params_->lfo_mode > 0.5f;
        if(sel_params_)
            for(int k = 0; k < kNumFxSlots; k++)
                c.fx[k] = sel_params_->fx[k];
        for(int i = 0; i < 4; i++)
            if(enc_[i].Pressed())
                c.held |= uint8_t(1u << i);
        return c;
    }

    // Value popup: the last parameter turned, and how long ago (ms)
    const ParamDesc* popup() const { return popup_; }
    uint32_t         popup_age_ms() const { return now_ms_ - popup_ms_; }
    bool muted(int t) const { return muted_[t]; }

    // Length of one 8th note in samples
    float Eighth(float bpm) const { return 60.f / bpm * 0.5f * sr_; }

  private:
    // Put effect e into a slot. If the other slot already has it, the two
    // swap (each effect exists once per track). An effect whose MIX is at 0
    // gets a starting MIX, so choosing it is heard straight away.
    static void AssignFx(Params& p, int slot, int e)
    {
        const int other = slot ^ 1;
        if(e != kFxNone && p.fx[other] == e)
            p.fx[other] = p.fx[slot];
        p.fx[slot] = e;
        switch(FxPage(e))
        {
            case FxPage::Chorus:
                if(p.chorus_mix < 0.01f)
                    p.chorus_mix = 0.5f;
                break;
            case FxPage::Resonator:
                if(p.res_mix < 0.01f)
                    p.res_mix = 0.4f;
                break;
            case FxPage::Reverb:
                if(p.rev_mix < 0.01f)
                    p.rev_mix = 0.3f;
                break;
            default: break;
        }
    }

    // REC acts on the selected track; PLAY on both. Works with the browser
    // open too.
    void Transport(TapeLoop& tape, TapeLoop* const* tapes)
    {
        if(rec_.RisingEdge())
        {
            if(tape.IsRecording())
                tape.RequestStop(); // finish the take → loops
            else if(shift_held_)
                tape.StartRecording(); // SHIFT + REC: record now
            else if(tape.IsArmed() || tape.IsCountReady() || tape.IsCounting())
                tape.Disarm(); // second press: cancel
            else
                tape.Arm(); // wait for input (in time with a playing loop)
        }
        if(play_.RisingEdge() && rec_.Pressed() && tape.IsArmed())
            tape.ArmCountIn(); // hold REC + PLAY: count-in arm
        else if(play_.RisingEdge() && shift_held_)
            metronome_on_ = !metronome_on_; // SHIFT + PLAY: click on / off
        else if(play_.RisingEdge() && tape.IsCountReady())
            tape.StartCountIn(); // 4 beats, then record on the beat
        else if(play_.RisingEdge() && tape.IsCounting())
            tape.Disarm(); // PLAY during the count-in: cancel
        else if(play_.RisingEdge())
        {
            bool any_playing = false;
            for(int t = 0; t < kNumTracks; t++)
                any_playing |= tapes[t]->IsPlaying();

            if(tape.IsRecording())
                tape.RequestStop(); // joins in / starts all: gloop_fw.ino
            else if(any_playing)
                for(int t = 0; t < kNumTracks; t++)
                    tapes[t]->Stop();
            else
                for(int t = 0; t < kNumTracks; t++)
                    tapes[t]->Restart(); // from the loop start, in sync
        }
    }

    daisy::Encoder enc_[4];
    daisy::Switch  rec_, play_, shift_, track_[kNumTracks], page_btn_[kNumPages];
    int            selected_track_ = 0; // T1 selected at power-up
    Page           page_           = Page::Loop;
    bool           shift_held_     = false;
    float          sr_             = 48000.f;
    bool           metronome_on_   = true;
    bool           grain_size_mode_ = false; // GRAINS pressed twice
    int            fx_page_  = 0;     // FX page: slot 1 or 2 (0 / 1)
    FxMenu         menu_;             // SHIFT + FX slot menu
    bool           fx_held_  = false; // FX button down: encoders = FX mixer
    bool           fx_used_  = false; // an encoder moved while FX was down
    uint32_t       now_ms_   = 0;     // Process() runs at 1 kHz
    const ParamDesc* popup_  = nullptr;
    uint32_t       popup_ms_ = 0;
    bool           muted_[kNumTracks] = {};
    const Params*  sel_params_ = nullptr; // selected track's params (LFO/ENV)
};

} // namespace gloop
