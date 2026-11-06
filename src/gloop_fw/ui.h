// =============================================================================
// ui.h — draws the LOOP / GRAINS / FX screens from the live engine state
//
// The UI only READS engine state (tape head, grain pool, params). It runs in
// loop(), never in the audio interrupt, so a slow frame can only drop the
// frame rate — it can never glitch audio.
// =============================================================================
#pragma once
#include <cstdint>
#include "browser.h"
#include "envelopes.h"
#include "gfx.h"
#include "grains.h"
#include "params.h"
#include "st7789.h"
#include "tape_loop.h"

namespace gloop
{
struct UiState
{
    Page              page;
    bool              shift;
    const Params*     params;
    const TapeLoop*   tape;
    const GrainCloud* cloud;
    float             sample_rate;
    float             cpu_avg   = 0.f;   // audio CPU load, 0..1 (smoothed)
    float             cpu_peak  = 0.f;   // worst block in the last second
    bool              throttled = false; // safety valve is limiting grains
    int               fps       = 0;     // screen frames in the last second
    int               grains_allowed = -1; // grains this track actually gets
                                           // from the shared pool (-1: n/a)

    // Sample browser / loading
    const Browser* browser        = nullptr; // menu shown when open
    int             track      = 0;       // selected track: 0 = T1, 1 = T2
    const TapeLoop* others[kNumTracks - 1] = {}; // the other tracks (footer)
    int             other_id[kNumTracks - 1] = {}; // ...which track each is
    float           input_peak = 0.f;     // input level (record-arm meter)
    bool            metro_active  = false; // metronome clicking
    bool            metro_enabled = true;  // SHIFT + PLAY toggles
    int             metro_beat    = 0;     // 0..3 within the bar
    bool            grain_size_mode = false; // GRAINS pressed twice: alt layer
    EncoderContext  enc;                     // what each encoder edits now
    int             fx_page      = 0;        // FxPage on the FX page
    const ParamDesc* popup       = nullptr;  // last parameter turned
    uint32_t        popup_age_ms = 100000;   // ...and how long ago
    bool            muted        = false; // selected track muted
    bool            other_muted[kNumTracks - 1] = {}; // other tracks muted
    float           chorus_phase = 0.f;   // for the FX page 2 picture
    float           res_level    = 0.f;   // resonator output level (FX page 2)
    const char*    tape_name      = nullptr; // sample on the tape (label)
    const char*    load_name      = nullptr; // sample being loaded
    float          load_progress  = -1.f;    // 0..1 while loading, else <0
};

class Ui
{
  public:
    void Init(St7789* display, const EnvelopeTable* env);
    void Draw(const UiState& s);

    // Scan the recording once (after a take finishes) into peak columns for
    // the waveform strip, so the strip never has to read SDRAM per frame.
    void BuildOverview(const TapeLoop& tape, int track);

  private:
    void DrawHeader(const UiState& s);
    void DrawLoop(const UiState& s);
    void DrawGrains(const UiState& s);
    void DrawFx(const UiState& s);
    void DrawReadouts(const UiState& s);
    void DrawPopup(const UiState& s);
    void DrawFooter(const UiState& s);

    // Pieces of the GRAINS street scene
    void DrawSprite(const char* const* rows, int h, int x, int y);
    void DrawCatSitting(int x, int y, int eyes, bool ears_flat, float tail_sway);
    void DrawLamp();
    void DrawPavement();
    void DrawBench();
    void DrawCloud(int count, float spread);
    void DrawNight();
    void UpdateCat(bool raining);
    void DrawCatActor(int count);

    // The cat's little routine: asleep on the bench when it's dry, hides
    // under it when it rains.
    enum CatState
    {
        kSleeping,  // curled up on the bench
        kWaking,    // sits up on the bench
        kJumpDown,  // hops off to the right
        kCrawlIn,   // crouches and creeps under the bench
        kHiding,    // loafing under the bench
        kCrawlOut,  // creeps back out
        kJumpUp,    // hops back onto the bench
        kSettling,  // sits a moment, then curls up
    };

    // Sample browser
    void DrawBrowser(const UiState& s);
    void DrawFolderTiles(const Browser& b);
    void DrawSampleList(const Browser& b);
    void DrawBrowserInfo(const Browser& b);
    void DrawBrowserHints(const Browser& b);
    void DrawFolderIcon(int x, int y, int folder, bool selected);
    void DrawMask(const char* const* rows, int h, int x, int y, uint16_t c);
    void DrawLoading(const UiState& s);
    void DrawTrackChip(const UiState& s);
    void DrawOtherTrack(const UiState& s);

    // Tiny envelope curve used in place of the shape's name
    void DrawEnvGlyph(int right, int y, int shape, int w, int h);

    static constexpr int kOverviewCols = 66;

    Gfx                  g_;
    St7789*              display_                 = nullptr;
    const EnvelopeTable* env_                     = nullptr;
    float                overview_[kNumTracks][kOverviewCols] = {};
    bool                 has_overview_[kNumTracks]            = {};
    float                reel_angle_              = 0.f;
    uint32_t             frame_                   = 0;
    CatState             cat_state_               = kSleeping;
    int                  cat_t_                   = 0; // frames in this state
};

} // namespace gloop
