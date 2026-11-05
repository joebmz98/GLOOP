// =============================================================================
// controls.h — encoders and buttons → parameters and transport
//
// Called once per audio block (1 kHz at 48 samples / 48 kHz), the rate the
// DaisyDuino Encoder/Switch debouncing expects.
//
//   LOOP / GRAINS / FX buttons : choose which page the 4 encoders edit
//   Encoder turn               : edit that page's parameter
//   SHIFT + turn               : fine adjust (×0.1), or the alternate
//                                parameter where one exists
//                                (GRAINS page, encoder 1 → grain SIZE)
//   Encoder push               : reset that parameter to its default
//   REC                        : start a new take / finish it (→ plays)
//   PLAY                       : play / stop (also finishes a take)
//   T1 / T2 (track buttons)    : 1st press selects that track; pressed again
//                                on the LOOP page it opens the sample browser
//                                (from another page it jumps to LOOP first).
//                                T2 only selects for now: there is no second
//                                track engine yet.
//   While the browser is open  : any encoder turn moves, any push selects,
//                                a track button or a page button closes it
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
    void Init(float update_rate)
    {
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
        shift_.Init(update_rate, true, pins::kBtnShift, INPUT_PULLUP);
        track_[0].Init(update_rate, true, pins::kBtnTrack1, INPUT_PULLUP);
        track_[1].Init(update_rate, true, pins::kBtnTrack2, INPUT_PULLUP);
    }

    void Process(Params& p, TapeLoop& tape, Browser& browser)
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

        // 2. Page selection (also closes the browser)
        for(int i = 0; i < kNumPages; i++)
            if(page_btn_[i].RisingEdge())
            {
                page_ = Page(i);
                browser.Close();
            }

        shift_held_ = shift_.Pressed();

        // 3. Track buttons: select → (on LOOP) open browser → close browser
        for(int t = 0; t < kNumTrackButtons; t++)
        {
            if(!track_[t].RisingEdge())
                continue;
            const bool has_engine = t == 0; // only T1 has a track for now
            if(browser.IsOpen())
                browser.Close();
            else if(selected_track_ != t)
                selected_track_ = t;
            else if(page_ != Page::Loop)
                page_ = Page::Loop;
            else if(has_engine)
                browser.Open();
        }

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
            Transport(tape);
            return;
        }

        // 5. Encoders edit the current page's parameters
        for(int i = 0; i < 4; i++)
        {
            const ParamDesc* alt = shift_held_ ? ShiftParam(page_, i) : nullptr;
            const ParamDesc& d   = alt ? *alt : kPageParams[int(page_)][i];
            float&           v   = p.*(d.field);
            int              inc = enc_[i].Increment();

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
            if(enc_[i].RisingEdge())
                v = d.def;
        }

        Transport(tape);
    }

    Page page() const { return page_; }
    bool shift() const { return shift_held_; }
    int  selected_track() const { return selected_track_; }

  private:
    // REC / PLAY work whether or not the browser is open
    void Transport(TapeLoop& tape)
    {
        if(rec_.RisingEdge())
        {
            if(tape.IsRecording())
                tape.StopRecording();
            else
                tape.StartRecording();
        }
        if(play_.RisingEdge())
        {
            if(tape.IsRecording())
                tape.StopRecording();
            else
                tape.SetPlaying(!tape.IsPlaying());
        }
    }

    daisy::Encoder enc_[4];
    static constexpr int kNumTrackButtons = 2;
    daisy::Switch  rec_, play_, shift_, track_[kNumTrackButtons], page_btn_[kNumPages];
    int            selected_track_ = -1; // none until T1 is first pressed
    Page           page_       = Page::Loop;
    bool           shift_held_ = false;
};

} // namespace gloop
