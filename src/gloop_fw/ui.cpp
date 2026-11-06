// =============================================================================
// ui.cpp — 1-bit pixel-art screens, LANDSCAPE
//
// Everything is drawn on a 140 x 120 logical grid (2x2 physical pixels each,
// on the 280 x 240 rotated panel).
//
//   y   0..10   header  : [T1] chip, page name, transport status
//   y  13..84   page body
//                 LOOP   : cassette tape + waveform strip
//                 GRAINS : cat on a pavement in the rain (rain = grains)
//                 FX     : filter curve | reverb rings, side by side
//   y  87..107  readouts: 2x2 grid, mirrors the 2x2 encoders on the panel
//   y 111..119  footer  : page tabs
//
// Black, three greys and white: background layers in grey, live elements in
// white, so overlapping layers stay easy to tell apart.
// =============================================================================
#include "ui.h"
#include <cmath>
#include <cstdlib>

namespace gloop
{
// Palette: black, three greys, white. Background layers use the greys; live
// things (cat, rain, play heads, values) are pure white.
static constexpr uint16_t W      = Gfx::kWhite;
static constexpr uint16_t L      = Gfx::Rgb(176, 176, 180); // light grey
static constexpr uint16_t M      = Gfx::Rgb(108, 108, 114); // mid grey
static constexpr uint16_t D      = Gfx::Rgb(50, 50, 56);    // dark grey
static constexpr uint16_t B      = Gfx::kBlack;
// The ONLY colour: recording / record arm
static constexpr uint16_t R      = Gfx::Rgb(235, 40, 40);  // red
static constexpr uint16_t RD     = Gfx::Rgb(110, 20, 20);  // dark red (blink off)
static constexpr float    kTwoPi = 6.2831853f;

// Logical layout
static constexpr int kSW        = 140; // screen width
static constexpr int kBodyTop   = 13;
static constexpr int kBodyBot   = 84;
static constexpr int kReadLine  = 86;
static constexpr int kReadTop   = 88;
static constexpr int kCellW     = 66;
static constexpr int kFootLine  = 110;
// The panel has rounded corners: keep the header and footer rows this far in
// from the left and right edges so nothing is cut off
static constexpr int kCornerL    = 14;
static constexpr int kCornerR    = kSW - 15; // last usable column on the right

static const char* const kPageNames[kNumPages] = {"LOOP", "GRAINS", "FX"};

void Ui::Init(St7789* display, const EnvelopeTable* env)
{
    display_ = display;
    env_     = env;
    g_.Init(display->Framebuffer(), St7789::kWidth, St7789::kHeight, 2);
}

void Ui::BuildOverview(const TapeLoop& tape, int track)
{
    float* overview = overview_[track];
    size_t n = tape.RecordedLength();
    if(n == 0)
    {
        has_overview_[track] = false;
        return;
    }
    size_t       col_sz   = n / kOverviewCols;
    size_t       stride   = col_sz / 400 + 1; // ≤ ~400 reads per column
    float        peak_all = 0.0001f;
    for(int c = 0; c < kOverviewCols; c++)
    {
        float peak = 0.f;
        for(size_t i = c * col_sz; i < (c + 1) * col_sz; i += stride)
            peak = fmaxf(peak, tape.FramePeak(i));
        overview[c] = peak;
        peak_all     = fmaxf(peak_all, peak);
    }
    for(int c = 0; c < kOverviewCols; c++)
        overview[c] /= peak_all; // normalise so quiet takes are still visible
    has_overview_[track] = true;
}

// =============================================================================
// Frame
// =============================================================================
void Ui::Draw(const UiState& s)
{
    frame_++;
    g_.Fill(B);
    g_.ResetClip();
    if(s.browser && s.browser->IsOpen())
    {
        DrawBrowser(s); // the menu takes over the whole screen
        return;
    }
    DrawHeader(s);
    g_.SetClip(0, kBodyTop - 1, kSW - 1, kBodyBot);
    switch(s.page)
    {
        case Page::Loop: DrawLoop(s); break;
        case Page::Grains: DrawGrains(s); break;
        case Page::Fx: DrawFx(s); break;
    }
    if(s.load_progress >= 0.f)
        DrawLoading(s);
    else
        DrawPopup(s);
    g_.ResetClip();
    DrawReadouts(s);
    DrawFooter(s);
}

// ---- Header ------------------------------------------------------------------------
void Ui::DrawHeader(const UiState& s)
{
    DrawTrackChip(s);
    // (The mode name - LOOP / GRAINS / FX - is in the middle of the footer)

    // MIDI in activity: a small dot right of the chip while bytes arrive
    if(s.midi_rx)
        g_.FillRect(kCornerL + 15, 4, 3, 3, (frame_ / 2) % 2 ? W : M);

    // Holding FX: the encoders are the FX mixer, whatever the page
    if(s.enc.fx_held)
        g_.Chip(56, 1, "FX MIX", Gfx::kSmall);

    // Tempo next to the page name on the LOOP page, e.g. "120 BPM"
    if(s.page == Page::Loop && !s.enc.fx_held)
    {
        char bpm[12];
        int  b = int(lroundf(s.params->bpm)), k = 0;
        if(b >= 100)
            bpm[k++] = char('0' + b / 100);
        bpm[k++] = char('0' + (b / 10) % 10);
        bpm[k++] = char('0' + b % 10);
        const char* tail = s.midi_sync ? " MIDI" : " BPM"; // MIDI clock in
        for(; *tail; tail++)
            bpm[k++] = *tail;
        bpm[k] = 0;
        g_.Text(kCornerL + 19, 4, bpm, s.midi_sync ? L : (s.shift ? W : M), Gfx::kSmall);
    }

    const TapeLoop& t  = *s.tape;
    const bool      on = (frame_ / 8) % 2 == 0;
    // Record states are the only thing on the screen in colour (red)
    const int rx = kCornerR + 1; // right edge of the status text
    if(t.IsArmed())
    {
        // Armed: "ARM" with a hollow record dot, blinking, waiting for input
        g_.Circle(rx - 23, 5, 2, on ? R : RD);
        g_.TextRight(rx, 2, "ARM", on ? R : RD, Gfx::kBig);
    }
    else if(t.IsCountReady())
    {
        // Count-in armed: steady "ARM", PLAY starts the count
        g_.Circle(rx - 23, 5, 2, R);
        g_.TextRight(rx, 2, "ARM", R, Gfx::kBig);
    }
    else if(t.IsCounting())
    {
        // Counting in: the beats left, in red, with a filled dot
        char n[2] = {char('0' + (s.count_beats > 9 ? 9 : s.count_beats)), 0};
        g_.FillCircle(rx - 11, 5, 2, on ? R : RD);
        g_.TextRight(rx, 2, n, R, Gfx::kBig);
    }
    else if(t.IsRecording())
    {
        // Blinking red REC chip with a dot
        const int x = rx - 26;
        g_.FillRect(x, 0, 27, 11, on ? R : RD);
        g_.FillCircle(x + 4, 5, 2, W);
        g_.TextRight(rx, 2, "REC", W, Gfx::kBig);
    }
    else if(t.IsPlaying())
    {
        g_.TextRight(rx, 2, "PLAY", W, Gfx::kBig);
        int ax = rx - 30 + (on ? 1 : 0); // nudging play triangle
        for(int i = 0; i < 4; i++)
            g_.VLine(ax + i, 3 + i, 9 - i, W);
    }
    else
    {
        g_.TextRight(rx, 2, "STOP", W, Gfx::kBig);
        g_.FillRect(rx - 31, 3, 5, 5, W);
    }

    if(s.shift)
        g_.Chip(70, 1, "SHIFT", Gfx::kSmall);
    else
    {
        // Metronome: 4 beat dots, the current beat lit (beat 1 drawn larger).
        // Hollow and still when a track is armed/recording but the click is
        // switched off (SHIFT + PLAY).
        const bool waiting = t.IsArmed() || t.IsRecording() || t.IsCounting() || s.metro_active;
        if(waiting)
            for(int b = 0; b < 4; b++)
            {
                const int  x   = 66 + b * 5;
                const int  sz  = b == 0 ? 4 : 3;
                const int  y   = b == 0 ? 3 : 4;
                const bool cur = s.metro_active && b == s.metro_beat;
                if(!s.metro_enabled)
                    g_.Rect(x, y, sz, sz, D);
                else if(cur)
                    g_.FillRect(x, y, sz, sz, W);
                else
                    g_.FillRect(x, y, sz, sz, D);
            }
    }

    g_.HLine(0, kSW - 1, 11, D);
}

// =============================================================================
// LOOP: cassette tape + waveform strip
// =============================================================================
void Ui::DrawLoop(const UiState& s)
{
    const TapeLoop& t     = *s.tape;
    const float     total = float(t.RecordedLength());

    // How far through the loop (or buffer, when recording) we are. Tape winds
    // from the left reel onto the right reel, then the loop jumps back.
    float phase = 0.f;
    if(t.IsRecording())
        phase = total / float(t.Capacity());
    else if(t.HasLoop())
        phase = float(t.Head()) / t.RegionLength();

    // Reels spin at the tape rate, so PITCH visibly changes their speed
    if(t.IsPlaying() || t.IsRecording())
        reel_angle_ += (t.IsRecording() ? 1.f : t.Rate()) * 0.25f;
    if(reel_angle_ > kTwoPi)
        reel_angle_ -= kTwoPi;

    // ---- Shell (86 x 54, centred) ----------------------------------------------
    const int ox = 27, oy = 13, sw = 86, sh = 54;
    g_.FillRoundRect(ox, oy, sw, sh, D);  // dark grey plastic
    g_.RoundRect(ox, oy, sw, sh, L);      // light edge
    g_.RoundRect(ox + 1, oy + 1, sw - 2, sh - 2, M);
    const int scr[5][2] = {{ox + 4, oy + 4},
                           {ox + sw - 5, oy + 4},
                           {ox + 4, oy + sh - 5},
                           {ox + sw - 5, oy + sh - 5},
                           {ox + sw / 2, oy + sh - 5}};
    for(auto& p : scr) // screws: tiny "+"
    {
        g_.Pixel(p[0], p[1], L);
        g_.Pixel(p[0] - 1, p[1], M);
        g_.Pixel(p[0] + 1, p[1], M);
        g_.Pixel(p[0], p[1] - 1, M);
        g_.Pixel(p[0], p[1] + 1, M);
    }

    // ---- Label sticker -------------------------------------------------------------
    const int lx = ox + 8, ly = oy + 3, lw = 70, lh = 33;
    g_.FillRoundRect(lx, ly, lw, lh, W);
    // The label shows the loaded sample's name (11 characters fit)
    char label[12];
    {
        static const char* const kDef[3] = {"GLOOP A", "GLOOP B", "GLOOP C"};
        const char* def = kDef[s.track < 0 ? 0 : (s.track > 2 ? 2 : s.track)];
        int         n   = 0;
        for(; def[n]; n++)
            label[n] = def[n];
        label[n] = 0;
    }
    if(s.tape_name)
    {
        int n = 0;
        for(; s.tape_name[n] && n < 11; n++)
            label[n] = s.tape_name[n];
        label[n] = 0;
    }
    g_.Text(lx + 3, ly + 2, label, B, Gfx::kSmall);

    // Speed written on the label, e.g. "X1.26"
    char speed[8]   = {'X', 0};
    int  hundredths = int(lroundf(t.Rate() * 100.f));
    speed[1]        = char('0' + (hundredths / 100) % 10);
    speed[2]        = '.';
    speed[3]        = char('0' + (hundredths / 10) % 10);
    speed[4]        = char('0' + hundredths % 10);
    speed[5]        = 0;
    g_.TextRight(lx + lw - 4, ly + 2, speed, B, Gfx::kSmall);
    g_.HLine(lx + 3, lx + lw - 4, ly + 8, B);
    g_.HLine(lx + 3, lx + lw - 4, ly + lh - 2, M);

    // ---- Window with tape packs and hubs ---------------------------------------------
    const int wx = ox + 15, wy = oy + 11, ww = 56, wh = 21;
    g_.FillRoundRect(wx, wy, ww, wh, B);
    const int   hub_y   = wy + wh / 2;
    const int   hubs[2] = {wx + 13, wx + ww - 14};
    const float packs[2] = {5.f + (1.f - phase) * 11.f, 5.f + phase * 11.f};

    g_.SetClip(wx + 1, wy + 1, wx + ww - 2, wy + wh - 2);
    for(int i = 0; i < 2; i++)
    {
        int r = int(packs[i]);
        g_.FillCircle(hubs[i], hub_y, r, M); // tape pack
        g_.Circle(hubs[i], hub_y, r, L);
    }
    g_.SetClip(0, kBodyTop - 1, kSW - 1, kBodyBot);

    for(int i = 0; i < 2; i++) // hubs: ring, white disc, 6 rotating teeth
    {
        int cx = hubs[i];
        g_.FillCircle(cx, hub_y, 5, B);
        g_.Circle(cx, hub_y, 5, L);
        g_.FillCircle(cx, hub_y, 3, W);
        g_.FillCircle(cx, hub_y, 1, B);
        for(int k = 0; k < 6; k++)
        {
            float a = reel_angle_ + k * kTwoPi / 6.f;
            g_.Pixel(cx + int(lroundf(cosf(a) * 3.f)), hub_y + int(lroundf(sinf(a) * 3.f)), B);
        }
    }
    g_.RoundRect(wx, wy, ww, wh, B);

    // ---- Bottom of the shell: trapezoid with the head opening ------------------------
    const int tb = oy + sh - 1, tt = oy + 40, cx = ox + sw / 2;
    g_.Line(cx - 24, tb, cx - 19, tt, M);
    g_.HLine(cx - 19, cx + 18, tt, M);
    g_.Line(cx + 18, tt, cx + 23, tb, M);
    g_.FillCircle(cx - 12, tt + 6, 2, B); // capstan holes
    g_.Circle(cx - 12, tt + 6, 2, M);
    g_.FillCircle(cx + 11, tt + 6, 2, B);
    g_.Circle(cx + 11, tt + 6, 2, M);
    g_.FillRect(cx - 6, tt + 4, 12, 8, B); // head opening
    g_.Rect(cx - 6, tt + 4, 12, 8, M);
    if(t.IsPlaying() || t.IsRecording()) // head glows
        g_.FillRect(cx - 3, tt + 6, 6, 4, (frame_ / 4) % 2 == 0 ? W : L);
    g_.HLine(cx - 22, cx + 21, tb - 2, M); // tape across the bottom

    // ---- Waveform strip ------------------------------------------------------------
    const int top = 70, bot = 84, mid = (top + bot) / 2, x0 = 4, w = 132;
    if(t.IsRecording())
    {
        // Recording: the track strip fills in red (progress through the 30 s)
        g_.Rect(x0, mid - 3, w, 7, RD);
        g_.FillRect(x0 + 2, mid - 1, w - 4, 3, D);
        g_.FillRect(x0 + 2, mid - 1, int((w - 4) * phase) + 1, 3, R);
        if(t.IsStopPending()) // REC pressed: recording on to the bar line
            g_.TextCentered(68, top - 1, "FINISHING THE BAR", R, Gfx::kSmall);
        else if(t.GridLocked())
            g_.TextCentered(68, top - 1, "IN TIME", R, Gfx::kSmall);
    }
    else if(t.IsCountReady())
    {
        if((frame_ / 8) % 2 == 0)
            g_.TextCentered(68, mid - 2, "PLAY: 4-BEAT COUNT-IN", R, Gfx::kSmall);
    }
    else if(t.IsCounting())
    {
        // The count, big and red, in the strip
        char n[2] = {char('0' + (s.count_beats > 9 ? 9 : s.count_beats)), 0};
        g_.TextScaled(68 - 5, top - 1, n, R, Gfx::kBig, 2);
    }
    else if(t.IsArmed())
    {
        // Waiting for input: live input meter on a -60..0 dB scale, with the
        // trigger threshold marked. It turns red once it would trigger.
        // "IN TIME": another loop is playing, so the take locks to its grid.
        if((frame_ / 8) % 2 == 0)
            g_.TextCentered(68, top, s.in_time ? "WAITING FOR INPUT - IN TIME" : "WAITING FOR INPUT",
                            R, Gfx::kSmall);
        auto db_pos = [](float lin) {
            float db = 20.f * log10f(fmaxf(lin, 1e-4f)); // -80..0
            float p  = (db + 60.f) / 60.f;
            return p < 0.f ? 0.f : (p > 1.f ? 1.f : p);
        };
        const int my = mid + 1, mh = 5;
        g_.Rect(x0, my, w, mh, M);
        g_.FillRect(x0 + 1, my + 1, w - 2, mh - 2, D);
        const bool  over = s.input_peak >= TapeLoop::kArmThreshold;
        const int   fill = int((w - 2) * db_pos(s.input_peak));
        g_.FillRect(x0 + 1, my + 1, fill, mh - 2, over ? R : L);
        const int tx = x0 + 1 + int((w - 2) * db_pos(TapeLoop::kArmThreshold));
        g_.VLine(tx, my - 2, my + mh + 1, W); // threshold marker
    }
    else if(t.HasLoop() && has_overview_[s.track])
    {
        float rs = t.RegionStart() / total;
        float re = (t.RegionStart() + t.RegionLength()) / total;
        for(int x = 0; x < w; x += 2)
        {
            float f  = (x + 0.5f) / w;
            int   h  = 1 + int(overview_[s.track][int(f * kOverviewCols)] * 5.f);
            bool  in = f >= rs && f <= re;
            g_.VLine(x0 + x, mid - h, mid + h, in ? L : D); // loop light, rest dark
        }
        int xs = x0 + int(rs * w);
        int xe = x0 + int(re * w);
        int xw = int(t.Xfade() / total * w);
        g_.VLine(xs, top, bot, W); // region brackets
        g_.HLine(xs, xs + 2, top, W);
        g_.HLine(xs, xs + 2, bot, W);
        g_.VLine(xe, top, bot, W);
        g_.HLine(xe - 2, xe, top, W);
        g_.HLine(xe - 2, xe, bot, W);
        if(xw > 0) // crossfade ramps
        {
            g_.Line(xs + 1, bot - 1, xs + xw, top + 1, W);
            g_.Line(xe - xw, top + 1, xe - 1, bot - 1, W);
        }
        int ph = x0 + int((t.RegionStart() + float(t.Head())) / total * w);
        g_.VLine(ph, top, bot, W); // play head
        g_.VLine(ph + 1, top, bot, B);
    }
    else
    {
        g_.TextCentered(68, mid - 2, "PRESS REC", L, Gfx::kSmall);
        if((frame_ / 8) % 2 == 0)
            g_.FillRect(88, mid - 2, 3, 5, W);
    }
}

// =============================================================================
// GRAINS: a cat, a bench and a street lamp in the rain
//
//   every active grain   = one raindrop (so GRAINS = how hard it rains)
//   grain pan            = where the drop falls; SPREAD 0 = a single column of
//                          rain over the bench, 100% = the whole street
//   grain age            = how far it has fallen; it splashes as it ends
//   grain envelope level = white streak when loud, fading to grey when quiet
//   SPRAY                = gusts of wind that slant the rain
//   SIZE (SHIFT)         = length of the streaks
//   GRAINS = 0           = dry night; the cat sleeps curled up on the bench
//   GRAINS > 0           = the cat wakes, hops down and hides under the bench
// =============================================================================

// ---- Scene geometry (logical pixels) ------------------------------------------
static constexpr int kFloor    = 79; // where feet touch the pavement
static constexpr int kPaveBack = 72; // back edge of the pavement
static constexpr int kPaveEdge = 81; // front kerb edge
static constexpr int kSkyTop   = 20; // just under the cloud
static constexpr int kBenchL   = 47; // seat ends
static constexpr int kBenchR   = 93;
static constexpr int kSeatTop  = 64; // top of the seat plank
static constexpr int kBenchMid = (kBenchL + kBenchR) / 2;

// ---- Sprites ('#' = white; '.' inside the outline is painted black) -----------

// Sitting cat, front view, 13 x 20. Rows 0-1 ears, row 4 eyes.
static const char* const kCatSit[20] = {
    "..#.......#..",
    "..##.....##..",
    "..#########..",
    ".###########.",
    ".###.###.###.",
    ".###########.",
    ".#####.#####.",
    "..#########..",
    "...#######...",
    "...#######...",
    "..#########..",
    ".###########.",
    ".###########.",
    "#############",
    "#############",
    "#############",
    "#####.#.#####",
    "#####.#.#####",
    "#####.#.#####",
    ".####.#.####.",
};
static constexpr int kSitW = 13, kSitH = 20;

// Curled up asleep, side view, 16 x 8: head tucked on the left (closed eye
// on row 3), back curving to the right, tail wrapped along the front.
static const char* const kCatCurl[8] = {
    "..#..#..........",
    "..####...#####..",
    ".######.#######.",
    ".#..###########.",
    ".##############.",
    "################",
    "###.........####",
    ".##############.",
};
// Same pose mid-breath: back rises by a pixel
static const char* const kCatCurlIn[8] = {
    "..#..#...####...",
    "..####..######..",
    ".######.#######.",
    ".#..###########.",
    ".##############.",
    "################",
    "###.........####",
    ".##############.",
};
static constexpr int kCurlW = 16, kCurlH = 8;

// "Loaf": crouched, paws tucked, front view, 13 x 9. Fits under the bench.
static const char* const kCatLoaf[9] = {
    "..#.......#..",
    "..##.....##..",
    "..#########..",
    ".###.###.###.",
    ".###########.",
    ".#####.#####.",
    "#############",
    "#############",
    ".###########.",
};
static const char* const kCatLoafBlink[9] = {
    "..#.......#..",
    "..##.....##..",
    "..#########..",
    ".###########.",
    ".##..###..##.",
    ".#####.#####.",
    "#############",
    "#############",
    ".###########.",
};
static constexpr int kLoafW = 13, kLoafH = 9;

void Ui::DrawSprite(const char* const* rows, int h, int x, int y)
{
    for(int r = 0; r < h; r++)
    {
        const char* row   = rows[r];
        int         first = -1, last = -1;
        for(int c = 0; row[c]; c++)
            if(row[c] == '#')
            {
                if(first < 0)
                    first = c;
                last = c;
            }
        if(first < 0)
            continue;
        // Black out the span first so rain/bench don't show through gaps
        g_.HLine(x + first, x + last, y + r, B);
        for(int c = first; c <= last; c++)
            if(row[c] == '#')
                g_.Pixel(x + c, y + r, W);
    }
}

// eyes: 0 = open, 1 = closed
void Ui::DrawCatSitting(int x, int y, int eyes, bool ears_flat, float tail_sway)
{
    // Tail first, so the body overlaps its root: a 2px curve that rises to
    // the right and swishes at the tip.
    for(int i = 0; i < 12; i++)
    {
        float k  = i / 11.f;
        int   tx = x + kSitW - 1 + int(i * 0.6f + tail_sway * k * k * 4.f);
        int   ty = y + kSitH - 2 - int(i * 0.9f);
        g_.Pixel(tx, ty, W);
        g_.Pixel(tx + 1, ty, W);
    }
    const char* rows[kSitH];
    for(int r = 0; r < kSitH; r++)
        rows[r] = kCatSit[r];
    if(ears_flat) // ears pinned back in the rain
    {
        rows[0] = ".............";
        rows[1] = ".##.......##.";
    }
    if(eyes)
    {
        rows[4] = ".###########.";
        rows[5] = ".##..#.#..##.";
    }
    DrawSprite(rows, kSitH, x, y);
}

void Ui::DrawLamp()
{
    // Street lamp on the left: post, arm and lamp head (no light cone)
    const int px = 12, top = 24;
    g_.VLine(px, top, kFloor, M);
    g_.VLine(px + 1, top, kFloor, D);
    g_.HLine(px, px + 6, top, M);
    g_.FillRect(px + 5, top, 5, 3, L);
    g_.HLine(px + 6, px + 8, top + 3, W); // the bulb
    g_.HLine(px - 2, px + 3, kFloor, M);  // base
}

void Ui::DrawPavement()
{
    // A flat pavement seen slightly from above: dotted back edge, slab joints
    // fanning out in perspective, solid kerb edge, then the kerb face.
    g_.FillRect(0, kPaveBack, kSW, kPaveEdge - kPaveBack, D); // slabs
    g_.HLine(0, kSW - 1, kPaveBack, M);
    g_.HLine(0, kSW - 1, (kPaveBack + kPaveEdge) / 2, M, true);
    for(int k = -6; k <= 6; k++)
    {
        int xb = 70 + k * 13; // joint at the back
        int xf = 70 + k * 17; // same joint at the front (wider = closer)
        g_.Line(xb, kPaveBack, xf, kPaveEdge, M, 2);
    }
    g_.HLine(0, kSW - 1, kPaveEdge, L);           // kerb edge
    g_.FillRect(0, kPaveEdge + 1, kSW, 3, D);     // kerb face
    g_.HLine(0, kSW - 1, kPaveEdge + 3, M, true);
}

void Ui::DrawBench()
{
    // Park bench, front view: two backrest slats, seat plank, armrests, legs
    // Wood in mid/light grey so the white cat stands out against it
    const int l = kBenchL, r = kBenchR;
    g_.VLine(l + 3, 50, kSeatTop, M); // backrest posts
    g_.VLine(r - 3, 50, kSeatTop, M);
    g_.HLine(l + 3, r - 3, 51, M); // slats: lit top edge, darker body
    g_.HLine(l + 3, r - 3, 52, D);
    g_.HLine(l + 3, r - 3, 56, M);
    g_.HLine(l + 3, r - 3, 57, D);
    g_.HLine(l, r, kSeatTop, L); // seat
    g_.HLine(l, r, kSeatTop + 1, M);
    g_.HLine(l + 1, r - 1, kSeatTop + 2, D); // seat front edge
    g_.HLine(l - 1, l + 3, 60, M); // armrests
    g_.VLine(l - 1, 60, kSeatTop, M);
    g_.HLine(r - 3, r + 1, 60, M);
    g_.VLine(r + 1, 60, kSeatTop, M);
    const int legs[2] = {l + 2, r - 3};
    for(int x : legs) // legs, 2px wide, with feet
    {
        g_.VLine(x, kSeatTop + 2, kFloor, M);
        g_.VLine(x + 1, kSeatTop + 2, kFloor, D);
        g_.HLine(x - 1, x + 2, kFloor, M);
    }
}

void Ui::DrawCloud(int count, float spread)
{
    // SPREAD = how wide the cloud is; GRAINS = how thick and lumpy it gets.
    // Overlapping puffs are drawn in three passes (edge r+1, body r, shading)
    // so only the OUTER edge of the whole cloud gets an outline.
    const float half  = 8.f + spread * 52.f;
    const int   drift = int(sinf(frame_ * 0.02f) * 2.f);
    const int   rows  = count >= 8 ? 3 : (count >= 4 ? 2 : 1);
    struct Puff
    {
        int x, y, r;
    } puffs[64];
    int n = 0;
    for(int row = 0; row < rows && n < 64; row++)
    {
        float row_half = half - row * 4.f; // lower rows are a bit narrower
        for(float x = -row_half; x <= row_half && n < 64; x += 6.f)
        {
            int i      = n * 7 + row * 3;
            puffs[n++] = {70 + drift + int(x) + (row % 2) * 3, 14 + row * 3, 4 + (i % 3)};
        }
    }
    for(int pass = 0; pass < 3; pass++)
        for(int i = 0; i < n; i++)
        {
            const Puff& p = puffs[i];
            if(pass == 0)
                g_.FillCircle(p.x, p.y, p.r + 1, M); // outer edge
            else if(pass == 1)
                g_.FillCircle(p.x, p.y, p.r, D);     // body
            else
                g_.FillCircle(p.x, p.y + 1, p.r - 2, B, true); // dark underside
        }
}

void Ui::DrawNight()
{
    // No grains: clear night, crescent moon, twinkling stars
    g_.FillCircle(116, 22, 6, W);
    g_.FillCircle(119, 20, 6, B);
    static const int stars[6][2] = {{30, 18}, {48, 28}, {88, 16}, {100, 34}, {60, 22}, {130, 40}};
    for(int i = 0; i < 6; i++)
        g_.Pixel(stars[i][0], stars[i][1], ((frame_ / 10) + i) % 4 == 0 ? M : L);
}

// ---- The cat's routine ---------------------------------------------------------
void Ui::UpdateCat(bool raining)
{
    auto go = [this](CatState s) {
        cat_state_ = s;
        cat_t_     = 0;
    };
    cat_t_++;
    switch(cat_state_)
    {
        case kSleeping:
            if(raining)
                go(kWaking);
            break;
        case kWaking:
            if(cat_t_ >= 14)
                go(raining ? kJumpDown : kSettling);
            break;
        case kJumpDown:
            if(cat_t_ >= 12)
                go(kCrawlIn);
            break;
        case kCrawlIn:
            if(cat_t_ >= 22)
                go(kHiding);
            break;
        case kHiding:
            if(!raining)
                go(kCrawlOut);
            break;
        case kCrawlOut:
            if(cat_t_ >= 22)
                go(kJumpUp);
            break;
        case kJumpUp:
            if(cat_t_ >= 12)
                go(kSettling);
            break;
        case kSettling:
            if(cat_t_ >= 16)
                go(raining ? kWaking : kSleeping);
            break;
    }
}

void Ui::DrawCatActor(int count)
{
    // Key positions (sprite top-left corners)
    const int sit_bench_x = kBenchMid - kSitW / 2, sit_bench_y = kSeatTop - kSitH;
    const int sit_side_x  = kBenchR + 6, sit_side_y = kFloor - kSitH + 1;
    const int loaf_out_x  = sit_side_x, loaf_in_x = kBenchMid - kLoafW / 2;
    const int loaf_y      = kFloor - kLoafH + 1;
    const float sway      = sinf(frame_ * (0.06f + count * 0.006f));
    const bool  blink     = frame_ % 90 < 4;

    auto ease = [](float t) { return t * t * (3.f - 2.f * t); }; // smoothstep
    auto hop  = [&](float t, int x0, int y0, int x1, int y1) {
        // Parabolic jump between two sitting positions
        int x = x0 + int((x1 - x0) * t);
        int y = y0 + int((y1 - y0) * t) - int(10.f * 4.f * t * (1.f - t));
        DrawCatSitting(x, y, 0, true, 0.f);
    };

    switch(cat_state_)
    {
        case kSleeping:
        {
            bool in = (frame_ / 24) % 2 == 0; // slow breathing
            int  x  = kBenchMid - kCurlW / 2;
            DrawSprite(in ? kCatCurlIn : kCatCurl, kCurlH, x, kSeatTop - kCurlH);
            for(int k = 0; k < 3; k++) // floating Z's
            {
                float ph = fmodf(frame_ * 0.015f + k / 3.f, 1.f);
                g_.Text(x + 4 + int(ph * 10.f), kSeatTop - 14 - int(ph * 24.f), "Z", ph < 0.6f ? L : M, Gfx::kSmall);
            }
            break;
        }
        case kWaking:
        case kSettling:
            // Sitting up on the bench; looks around (blinks) before moving
            DrawCatSitting(sit_bench_x, sit_bench_y, (cat_t_ / 5) % 2, count >= 1, sway);
            break;
        case kJumpDown:
            hop(ease(cat_t_ / 12.f), sit_bench_x, sit_bench_y, sit_side_x, sit_side_y);
            break;
        case kJumpUp:
            hop(ease(cat_t_ / 12.f), sit_side_x, sit_side_y, sit_bench_x, sit_bench_y);
            break;
        case kCrawlIn:
        case kCrawlOut:
        {
            float t = ease(cat_t_ / 22.f);
            if(cat_state_ == kCrawlOut)
                t = 1.f - t;
            int x = loaf_out_x + int((loaf_in_x - loaf_out_x) * t);
            int y = loaf_y + ((cat_t_ / 3) % 2); // little creeping bob
            DrawSprite(kCatLoaf, kLoafH, x, y);
            break;
        }
        case kHiding:
            DrawSprite(blink ? kCatLoafBlink : kCatLoaf, kLoafH, loaf_in_x, loaf_y);
            break;
    }
}

void Ui::DrawGrains(const UiState& s)
{
    const Params& p       = *s.params;
    const int     count   = int(p.grains + 0.5f);
    const bool    raining = count >= 1; // GRAINS 0 = just the loop

    UpdateCat(raining);

    if(!raining)
    {
        DrawNight();
        g_.Chip(2, 13, "LOOP ONLY", Gfx::kSmall);
    }

    DrawPavement();
    DrawLamp();
    DrawBench();

    // ---- Rain: one drop per active grain ---------------------------------------------
    const float  gust   = p.spray * 6.f * sinf(frame_ * 0.06f); // SPRAY = wind
    const int    streak = 2 + int(p.grain_size / 0.5f * 8.f);   // SIZE = streak
    const Grain* gr     = s.cloud->Grains();
    struct Splash
    {
        int x, y;
    } splashes[GrainCloud::kMaxGrains];
    int n_splash = 0;

    // Cat out in the open (on the bench or jumping) gets rained on too
    const bool cat_on_bench = cat_state_ == kWaking || cat_state_ == kSettling;

    if(raining)
    {
        for(int i = 0; i < GrainCloud::kMaxGrains; i++)
        {
            const Grain& g = gr[i];
            if(!g.active)
                continue;
            float amp = env_->Lookup(g.env, g.phase);
            int   x0  = 70 + int(g.pan * 62.f);
            // Where does this drop land? Cat's head, the bench seat or the
            // pavement (at a depth that varies per grain, for perspective).
            int surface = kFloor - 1 - (i * 5) % 6;
            if(cat_on_bench && abs(x0 - kBenchMid) <= 5)
                surface = kSeatTop - kSitH + 1;
            else if(x0 >= kBenchL && x0 <= kBenchR)
                surface = kSeatTop - 1;
            if(g.phase > 0.88f) // about to end: splash
            {
                splashes[n_splash++] = {x0 + int(gust), surface};
                continue;
            }
            int y      = kSkyTop + int(g.phase * float(surface - kSkyTop));
            int x      = x0 + int(gust * g.phase);
            int dx     = gust > 1.f ? -1 : (gust < -1.f ? 1 : 0); // trails into the wind
            int y_tail = y - streak < kSkyTop ? kSkyTop : y - streak;
            uint16_t c = amp > 0.6f ? W : (amp > 0.3f ? L : M); // loud = bright
            g_.Line(x, y, x + dx * (y - y_tail) / 2, y_tail, c);
        }
    }

    DrawCatActor(count);

    for(int i = 0; i < n_splash; i++) // splashes on top of everything
    {
        int x = splashes[i].x, y = splashes[i].y;
        g_.Pixel(x - 1, y - 1, L);
        g_.Pixel(x + 1, y - 1, L);
        g_.Pixel(x - 2, y - 2, M);
        g_.Pixel(x + 2, y - 2, M);
    }

    if(raining)
        DrawCloud(count, p.spread);
}

// Same soft clip as TrackFilter / ModalResonator (fx.h), for the drive glyph
static inline float TrackSoftClip(float x)
{
    if(x > 3.f)
        return 1.f;
    if(x < -3.f)
        return -1.f;
    const float x2 = x * x;
    return x * (27.f + x2) / (27.f + 9.f * x2);
}

// =============================================================================
// FX: one effect per page — FILTER / CHORUS / RESONATOR / REVERB
// (FX cycles them, SHIFT + FX goes back; dots in the header show which)
// =============================================================================
static constexpr int kFxTop = 24, kFxBot = 83; // picture area under the chip

// Text "LABEL 60%" for a parameter that isn't on the readouts
static void ValueTag(Gfx& g, int right, int y, const char* label, float v01, uint16_t c)
{
    char buf[16];
    int  k = 0;
    for(; *label && k < 8; label++)
        buf[k++] = *label;
    buf[k++] = ' ';
    const int pct = int(v01 * 100.f + 0.5f);
    if(pct >= 100)
        buf[k++] = '1';
    if(pct >= 10)
        buf[k++] = char('0' + (pct / 10) % 10);
    buf[k++] = char('0' + pct % 10);
    buf[k++] = '%';
    buf[k]   = 0;
    g.TextRight(right, y, buf, c, Gfx::kSmall);
}

// ---- FILTER: the response curve across the whole width ---------------------
//   CUTOFF / RESO / TYPE shape the curve; DRIVE makes the line shimmer like
//   heat and clips the little sine wave top right.
static void DrawFxFilter(Gfx& g, const UiState& s, uint32_t frame)
{
    const Params& p     = *s.params;
    const int     ftype = int(p.filter_type + 0.5f); // 0 LP, 1 HP, 2 BP
    static const char* const kTypeNames[3] = {"LP", "HP", "BP"};
    const int cw = g.Chip(1, 13, "FILTER", Gfx::kSmall);
    g.Text(cw + 4, 15, kTypeNames[ftype], L, Gfx::kSmall);

    // Drive glyph: a sine squashed by the same soft clip as the audio
    {
        const int   gx = 108, gy = 18, gw = 30;
        const float gain = 1.f + p.filter_drive * p.filter_drive * 15.f;
        const float norm = 1.f / TrackSoftClip(gain);
        int         prv  = gy;
        for(int x = 0; x < gw; x++)
        {
            const float v = TrackSoftClip(sinf(x * 6.2831853f / gw * 1.5f) * gain) * norm;
            const int   y = gy - int(lroundf(v * 4.f));
            if(x > 0)
                g.Line(gx + x - 1, prv, gx + x, y, p.filter_drive > 0.005f ? W : M);
            prv = y;
        }
        g.Text(gx - 22, 15, "DRIVE", p.filter_drive > 0.005f ? L : D, Gfx::kSmall);
    }

    const int top = kFxTop + 2, bot = kFxBot, x0 = 3, w = 134;
    for(int gx = x0 + 33; gx < x0 + w; gx += 33)
        g.VLine(gx, top, bot, D);
    g.HLine(x0, x0 + w, top + 14, D); // 0 dB

    const float q    = 0.7f + p.reso * 4.f;
    const float heat = p.filter_drive * 1.8f;
    int         prev = 0;
    for(int x = 0; x <= w; x++)
    {
        float xq  = float(x) / w;
        float wn  = powf(10.f, (xq - p.cutoff) * 2.2f);
        float num = ftype == 1 ? wn * wn : (ftype == 2 ? wn / q : 1.f);
        float mag = num / sqrtf((1.f - wn * wn) * (1.f - wn * wn) + (wn / q) * (wn / q));
        float db  = fmaxf(-36.f, fminf(12.f, 20.f * log10f(mag)));
        int   y   = top + int((12.f - db) / 48.f * float(bot - top));
        if(heat > 0.05f) // heat shimmer: a little jitter that moves each frame
        {
            uint32_t h = (uint32_t(x) * 2654435761u) ^ (frame * 40503u);
            h ^= h >> 13;
            y += int(lroundf((float(h & 255) / 255.f - 0.5f) * 1.4f * heat));
        }
        y = y < top ? top : (y > bot ? bot : y);
        if(y < bot) // area under the curve: mid grey near the line, dark below
        {
            g.VLine(x0 + x, y + 1, y + 3 < bot ? y + 3 : bot, M);
            if(y + 4 < bot)
                g.VLine(x0 + x, y + 4, bot, D);
        }
        if(x > 0)
        {
            g.Line(x0 + x - 1, prev, x0 + x, y, W);
            if(p.filter_drive > 0.5f) // driven hard: a thicker line
                g.Line(x0 + x - 1, prev - 1, x0 + x, y - 1, L);
        }
        prev = y;
    }
    g.HLine(x0, x0 + w, bot, M);
    const int cx = x0 + int(p.cutoff * w); // cutoff marker
    for(int y = top; y < bot; y += 2)
        g.Pixel(cx, y, L);
}

// ---- CHORUS: a little choir --------------------------------------------------
//   MIX    how many sing: 1 silent singer at 0, up to 8
//   RATE   how fast their mouths open and close
//   DEPTH  how much they sway (bob up and down)
//   WIDTH  how far apart they stand
// 9x15 singer: '.' clear, W/L/M/D/B palette, 'e' = eyes (dark dots, grey
// when shut while singing), 'o' = mouth (an open "O" when singing),
// 'r' = robe (front row M, back row D), 'k' = songbook.
static const char* const kSinger[15] = {
    "...DDD...", // hair
    "..DDDDD..", //
    ".DWWWWWD.", //
    ".WWWWWWW.", //
    ".WeWWWeW.", // eyes
    ".WWWWWWW.", //
    ".WWWoWWW.", // mouth
    "..WWoWW..", //
    "...WWW...", //
    "..rrrrr..", // robe
    ".rrrrrrr.", //
    "rrkkkkkrr", // songbook
    "rrkkkkkrr", //
    "rrrrrrrrr", //
    ".rrrrrrr.", //
};

static void DrawSinger(Gfx& g, int x, int y, bool back, bool singing)
{
    const uint16_t skin = back ? L : W;
    for(int row = 0; row < 15; row++)
        for(int col = 0; col < 9; col++)
        {
            uint16_t c;
            switch(kSinger[row][col])
            {
                case 'W': c = skin; break;
                case 'D': c = back ? D : M; break;
                case 'e': c = singing ? M : B; break; // singing: eyes shut
                case 'o':
                    if(singing)
                        c = B;
                    else
                        c = row == 6 ? D : skin; // closed: a small dash
                    break;
                case 'r': c = back ? D : M; break;
                case 'k': c = back ? M : L; break;
                default: continue;
            }
            g.Pixel(x + col, y + row, c);
        }
}

static void DrawFxChorus(Gfx& g, const UiState& s, uint32_t frame)
{
    const Params& p  = *s.params;
    const bool    on = p.chorus_mix > 0.001f;
    g.Chip(1, 13, "CHORUS", Gfx::kSmall);

    // How many sing: 1 (silent) at MIX 0, then 2..8 as MIX goes up
    const int n = on ? 2 + int(p.chorus_mix * 6.f + 0.5f) : 1;
    // Slots in units of the spacing: front row (y 60) and back row (y 45),
    // joining from the middle outwards
    struct Spot { float u; bool back; };
    static const Spot kSpots[8] = {
        {0.f, false},  {-0.5f, true}, {0.5f, true},  {-1.f, false},
        {1.f, false},  {-1.5f, true}, {1.5f, true},  {-2.f, false},
    };
    const float sp  = 12.f + p.chorus_width * 9.f; // WIDTH: spread out
    const float sway = 0.5f + p.chorus_depth * 2.5f; // DEPTH: how much they bob
    const int   cx  = 70 - 4;
    for(int pass = 0; pass < 2; pass++) // back row first
        for(int i = 0; i < n; i++)
        {
            const Spot& st = kSpots[i];
            if(st.back != (pass == 0))
                continue;
            float ph = s.chorus_phase + i * 0.37f;
            ph -= floorf(ph);
            const bool singing = on && ph < 0.6f;
            const int  bob = on ? -int(lroundf(sway * (0.5f + 0.5f * sinf(ph * 6.2831853f)))) : 0;
            const int  x   = cx + int(lroundf(st.u * sp));
            DrawSinger(g, x, (st.back ? 45 : 60) + bob, st.back, singing);
        }
    g.HLine(4, 135, 76, D); // riser
    g.HLine(10, 129, 78, D);

    // Notes floating up: more singers, more notes
    if(on)
    {
        static const char* const kNote[5] = {"..WW", "..W.", "..W.", "WWW.", "WW.."};
        const int notes = (n + 1) / 2;
        for(int k = 0; k < notes; k++)
        {
            const int t  = int((frame + k * 17) % 40); // 40 frames to rise
            const int nx = 20 + ((k * 29 + int(frame + k * 17) / 40 * 37) % 96);
            const int ny = 42 - t / 2;
            const uint16_t c = t < 26 ? L : M; // fade near the top
            for(int r = 0; r < 5; r++)
                for(int q = 0; q < 4; q++)
                    if(kNote[r][q] == 'W')
                        g.Pixel(nx + q, ny + r, c);
        }
    }
}

// ---- RESONATOR: a wine glass played with a wet finger -----------------------
//   FREQ    water level (more water = lower note, like a real glass)
//   STRUCT  glass shape: narrow flute (harmonic) → wide bell bowl
//   DECAY   how many sound waves ripple out, and how far
//   BRIGHT  (hold enc 2 + turn) how shiny the glass and waves are
//   MIX     0: the glass just sits there; up: a finger circles the rim, the
//           water shivers and the waves pulse with the output level
static void DrawFxResonator(Gfx& g, const UiState& s, uint32_t frame)
{
    const Params& p = *s.params;
    g.Chip(1, 13, "RESONATOR", Gfx::kSmall);
    const bool bright_held = (s.enc.held >> 1) & 1;
    ValueTag(g, 138, 15, "BRIGHT", p.res_bright, bright_held ? W : M);

    const bool  on    = p.res_mix > 0.001f;
    const float lvl   = on ? fminf(1.f, s.res_level * 4.f) : 0.f;
    const int   cx    = 70;
    const int   rim_y = 38, bowl_bot = 64, stem_bot = 78;
    const int   rim_w = 7 + int(p.res_struct * 9.f);
    const uint16_t glass = !on ? M : (p.res_bright > 0.5f ? W : L);

    // Bowl half-width at row y: tapering flute (low STRUCT) → round bowl
    auto half = [&](int y) {
        const float t     = float(y - rim_y) / float(bowl_bot - rim_y);
        const float t2    = t * t;
        const float bowl  = sqrtf(fmaxf(0.f, 1.f - t2));
        const float flute = (1.f - 0.6f * t) * sqrtf(fmaxf(0.f, 1.f - t2 * t2 * t2));
        const float k     = p.res_struct;
        return fmaxf(1.f, rim_w * (flute * (1.f - k) + bowl * k));
    };

    // Water: FREQ C1 → nearly full .. C7 → almost empty
    const float wfrac = 0.85f - (p.res_note - 24.f) / 72.f * 0.75f;
    const int   wy    = bowl_bot - int(wfrac * (bowl_bot - rim_y - 3));
    for(int y = wy + 1; y < bowl_bot; y++)
    {
        const int hw = int(half(y)) - 1;
        if(hw > 0)
            g.HLine(cx - hw, cx + hw, y, D);
    }
    {
        const int hw  = int(half(wy)) - 1;
        int       prv = wy;
        for(int x = -hw; x <= hw; x++)
        {
            const float wob = lvl * 1.6f * sinf(x * 0.9f + frame * 0.7f);
            const int   y   = wy + int(lroundf(wob));
            if(x > -hw)
                g.Line(cx + x - 1, prv, cx + x, y, on ? L : M);
            prv = y;
        }
    }
    for(int y = rim_y + 1; y <= bowl_bot; y++) // bowl outline
    {
        const int a = int(half(y - 1)), b = int(half(y));
        g.Line(cx - a, y - 1, cx - b, y, glass);
        g.Line(cx + a, y - 1, cx + b, y, glass);
    }
    for(int x = -rim_w; x <= rim_w; x++) // rim (a flat ellipse)
    {
        const float u  = float(x) / rim_w;
        const int   dy = int(lroundf(2.f * sqrtf(fmaxf(0.f, 1.f - u * u))));
        g.Pixel(cx + x, rim_y - dy, glass);
        g.Pixel(cx + x, rim_y + dy, on ? M : D);
    }
    if(p.res_bright > 0.2f) // shine
    {
        const uint16_t shine = p.res_bright > 0.66f ? W : (p.res_bright > 0.4f ? L : M);
        for(int y = rim_y + 3; y < rim_y + 12; y++)
            g.Pixel(cx - int(half(y)) + 2, y, shine);
    }
    g.VLine(cx, bowl_bot + 1, stem_bot - 1, glass); // stem and foot
    g.HLine(cx - 10, cx + 10, stem_bot, glass);
    g.HLine(cx - 7, cx + 7, stem_bot - 1, on ? M : D);

    if(on)
    {
        // Sound waves either side: count/reach from DECAY, shade from BRIGHT
        const int   waves = 1 + int(p.res_decay * 4.f + 0.5f);
        const float phase = float(frame % 12) / 12.f;
        const int   wcy   = (rim_y + bowl_bot) / 2;
        for(int k = 0; k < waves; k++)
        {
            const float r = float(rim_w + 7) + (k + phase) * 9.f;
            if(r > 66.f)
                break;
            const float fade = 1.f - (k + phase) / float(waves + 1);
            const float b    = fade * (0.4f + 0.6f * lvl) * (0.5f + 0.5f * p.res_bright);
            if(b < 0.15f)
                continue;
            const uint16_t c = b > 0.6f ? W : (b > 0.35f ? L : M);
            for(int a = -9; a <= 9; a++) // ±40 degrees
            {
                if(a % 3 == 0 && k > 0)
                    continue; // outer waves dashed: they're fading
                const float ang = a * 0.077f;
                const int   dx  = int(r * cosf(ang));
                const int   dy  = int(r * sinf(ang) * 0.9f);
                g.Pixel(cx + dx, wcy + dy, c);
                g.Pixel(cx - dx, wcy + dy, c);
            }
        }
        // The finger, circling the rim (dimmer when it's round the back)
        const float th   = frame * 0.12f;
        const int   fx   = cx + int(rim_w * cosf(th));
        const bool  back = sinf(th) < 0.f;
        g.FillRect(fx - 1, rim_y - 11, 3, 10, back ? M : W);
        g.FillRect(fx - 1, rim_y - 2, 3, 1, back ? D : L);
        g.Pixel(fx, rim_y - 3, back ? D : L);
    }
    else
        g.TextCentered(70, 27, "MIX UP TO PLAY THE GLASS", M, Gfx::kSmall);
}

// ---- REVERB: a cathedral -----------------------------------------------------
//   SIZE     how many arches the nave has (how far back it goes)
//   TONE     how far into the hall you can see / how bright the echoes are
//   PRE-DLY  the quiet gap before the first echo ring leaves the singer
//   MIX      how bright the echo rings are (0 = a silent, dim hall)
static void DrawEllipse(Gfx& g, int cx, int cy, int rx, int ry, uint16_t c, bool dotted)
{
    const int steps = 8 + rx * 2;
    for(int i = 0; i < steps; i++)
    {
        if(dotted && (i & 1))
            continue;
        const float a = float(i) / steps * 6.2831853f;
        g.Pixel(cx + int(lroundf(rx * cosf(a))), cy + int(lroundf(ry * sinf(a))), c);
    }
}

static void DrawFxReverb(Gfx& g, const UiState& s, uint32_t frame)
{
    const Params& p = *s.params;
    g.Chip(1, 13, "REVERB", Gfx::kSmall);
    g.SetClip(0, kFxTop, kSW - 1, kFxBot);

    const int   vx = 70, vy = 47;           // vanishing point
    const int   arches = 2 + int(p.rev_size * 5.f + 0.5f);
    const float reach  = 0.6f + p.rev_tone * 1.6f; // TONE: how far you can see
    static const uint16_t kShade[4] = {L, M, D, D};

    // Arches, far to near. Each is a column pair with a rounded-pointed
    // vault on top; lines through the column feet and the vault springs
    // run back to the vanishing point, so it reads as a long nave.
    auto arch = [&](int k, int& hw, int& base, int& spring, int& apex) {
        const float sc = powf(0.70f, float(k));
        hw             = int(30.f * sc);
        base           = vy + int(39.f * sc);
        apex           = vy - int(36.f * sc);
        spring         = apex + int(hw * 1.1f);
    };
    {
        int hw, base, spring, apex;
        arch(0, hw, base, spring, apex);
        g.Line(vx - hw, base, vx, vy, D); // floor edges
        g.Line(vx + hw, base, vx, vy, D);
        g.Line(vx - hw, spring, vx, vy, D); // vault springs
        g.Line(vx + hw, spring, vx, vy, D);
        g.Line(0, kFxBot, vx - hw, base, D); // aisle floor out to the sides
        g.Line(kSW - 1, kFxBot, vx + hw, base, D);
    }
    for(int k = arches - 1; k >= 0; k--)
    {
        int hw, base, spring, apex;
        arch(k, hw, base, spring, apex);
        const int shade = int(float(k) / reach);
        if(shade > 3)
            continue; // lost in the dark
        const uint16_t c = kShade[shade];
        g.VLine(vx - hw, spring, base, c);
        g.VLine(vx + hw, spring, base, c);
        // Vault: leaves the columns going up, bends in towards the apex
        int px = vx - hw, py = spring;
        for(int i = 1; i <= 12; i++)
        {
            const float t  = float(i) / 12.f * 1.5707963f;
            const int   ax = vx - hw + int(lroundf(hw * (1.f - cosf(t))));
            const int   ay = spring - int(lroundf((spring - apex) * sinf(t)));
            g.Line(px, py, ax, ay, c);
            g.Line(2 * vx - px, py, 2 * vx - ax, ay, c);
            px = ax;
            py = ay;
        }
        if(k == arches - 1 && p.rev_tone > 0.4f && shade < 3)
            g.Circle(vx, spring - (spring - apex) / 3, hw / 3 > 1 ? hw / 3 : 1, M); // rose window
    }

    // Echo rings spreading from the singer at the front, after PRE-DLY
    const int sx = vx, sy = 74;
    if(p.rev_mix > 0.001f)
    {
        const float gap    = 4.f + p.rev_predelay / 0.25f * 26.f; // PRE-DLY
        const float travel = 18.f + p.rev_size * 46.f;
        for(int r = 0; r < 4; r++)
        {
            const float ph  = fmodf(frame * 0.012f + r * 0.25f, 1.f);
            const int   rad = int(gap + ph * travel);
            const float b   = (1.f - ph) * (0.3f + p.rev_mix) * (0.5f + 0.6f * p.rev_tone);
            if(b < 0.12f)
                continue;
            const uint16_t c = b > 0.75f ? W : (b > 0.45f ? L : (b > 0.25f ? M : D));
            DrawEllipse(g, sx, sy, rad, rad * 2 / 5, c, ph > 0.5f);
        }
        // The direct sound: a small ring right at the singer
        DrawEllipse(g, sx, sy, 3, 1, L, false);
    }
    // The singer (a tiny figure)
    g.FillRect(sx - 1, sy - 9, 3, 3, W);
    g.FillRect(sx - 2, sy - 6, 5, 6, p.rev_mix > 0.001f ? L : M);
    g.SetClip(0, kBodyTop - 1, kSW - 1, kBodyBot);
}

void Ui::DrawFx(const UiState& s)
{
    switch(FxPage(s.fx_page & 3))
    {
        case FxPage::Filter: DrawFxFilter(g_, s, frame_); break;
        case FxPage::Chorus: DrawFxChorus(g_, s, frame_); break;
        case FxPage::Resonator: DrawFxResonator(g_, s, frame_); break;
        case FxPage::Reverb: DrawFxReverb(g_, s, frame_); break;
    }
}

// =============================================================================
// Readouts: 2x2, same arrangement as the encoders
// =============================================================================
void Ui::DrawEnvGlyph(int right, int y, int shape, int w, int h)
{
    // The envelope's shape instead of its name: a small curve on a baseline
    int x0   = right - w + 1;
    int base = y + h - 1;
    int prev = base;
    for(int x = 0; x < w; x++)
    {
        int yy = base - int(env_->Lookup(shape, float(x) / (w - 1)) * (h - 1) + 0.5f);
        if(x > 0)
            g_.Line(x0 + x - 1, prev, x0 + x, yy, W);
        prev = yy;
    }
}

// The context the encoders are in, completed from the UiState
static EncoderContext ContextOf(const UiState& s)
{
    EncoderContext c = s.enc;
    c.page           = s.page;
    c.fx_page        = s.fx_page;
    c.grains_alt     = s.grain_size_mode;
    c.shift          = s.shift;
    return c;
}

// A parameter's value as text (START / free LENGTH in seconds of the loop)
static void ValueText(const UiState& s, const ParamDesc& d, char* buf, size_t n)
{
    const TapeLoop& t        = *s.tape;
    const float     v        = s.params->*(d.field);
    const bool      free_len = d.field == &Params::length8 && !s.params->synced;
    if(d.format == Format::Seconds || free_len)
    {
        float secs = 0.f;
        if(t.HasLoop())
            secs = (d.field == &Params::start ? t.RegionStart() : t.RegionLength()) / s.sample_rate;
        static const ParamDesc kSecs
            = {"", nullptr, 0.f, 1.f, 0.f, 0.f, false, false, Format::Seconds};
        FormatValue(free_len ? kSecs : d, secs, 1.f, buf, n);
    }
    else
        FormatValue(d, v, 1.f, buf, n);
}

// Big value popup while an encoder is being turned (~1 s): the name and the
// value at double size, so it's clear what's changing, even for parameters
// that aren't on the readouts (hold + turn, the FX mixer).
void Ui::DrawPopup(const UiState& s)
{
    if(!s.popup || s.popup_age_ms > 1100)
        return;
    const ParamDesc& d = *s.popup;
    const int x = 24, y = 30, w = 92, h = 36;
    g_.FillRect(x, y, w, h, B);
    g_.Rect(x, y, w, h, W);
    g_.Rect(x + 1, y + 1, w - 2, h - 2, D);
    g_.TextCentered(x + w / 2, y + 4, d.label, L, Gfx::kSmall);
    const float v = s.params->*(d.field);
    if(d.format == Format::EnvName)
        DrawEnvGlyph(x + w / 2 + 15, y + 12, int(lroundf(v)), 30, 13);
    else
    {
        char buf[12];
        ValueText(s, d, buf, sizeof(buf));
        const int k  = Gfx::TextWidthScaled(buf, Gfx::kBig, 2) > w - 8 ? 1 : 2;
        const int tw = Gfx::TextWidthScaled(buf, Gfx::kBig, k);
        g_.TextScaled(x + (w - tw) / 2, y + (k == 2 ? 11 : 15), buf, W, Gfx::kBig, k);
    }
    const float norm = (v - d.min) / (d.max - d.min);
    g_.HLine(x + 6, x + w - 7, y + h - 6, D);
    g_.HLine(x + 6, x + 6 + int(norm * (w - 13)), y + h - 6, L);
    g_.HLine(x + 6, x + 6 + int(norm * (w - 13)), y + h - 5, L);
}

void Ui::DrawReadouts(const UiState& s)
{
    g_.HLine(0, kSW - 1, kReadLine, D);
    const EncoderContext ctx = ContextOf(s);
    for(int i = 0; i < 4; i++)
    {
        bool             alt = false;
        const ParamDesc& d   = EncoderParam(ctx, i, &alt);
        float            v   = s.params->*(d.field);
        int              x   = 2 + (i % 2) * 70;
        int              y   = kReadTop + (i / 2) * 11;
        int              right = x + kCellW - 1;

        if(alt) // SHIFT-layer parameter label is shown inverted
            g_.Chip(x - 1, y - 1, d.label, Gfx::kSmall);
        else
            g_.Text(x, y + 1, d.label, L, Gfx::kSmall);

        if(d.format == Format::EnvName)
            DrawEnvGlyph(right, y, int(lroundf(v)), 22, 7);
        else
        {
            char buf[12];
            ValueText(s, d, buf, sizeof(buf));
            // GRAINS limited by the shared pool: "8/12" (gets / asked for)
            const int want = int(v + 0.5f);
            if(d.field == &Params::grains && s.grains_allowed >= 0 && s.grains_allowed < want)
            {
                int k = 0;
                if(s.grains_allowed >= 10)
                    buf[k++] = char('0' + s.grains_allowed / 10);
                buf[k++] = char('0' + s.grains_allowed % 10);
                buf[k++] = '/';
                if(want >= 10)
                    buf[k++] = char('0' + want / 10);
                buf[k++] = char('0' + want % 10);
                buf[k]   = 0;
            }
            g_.TextRight(right, y, buf, W, Gfx::kBig);
        }

        // Value bar: dark track, light fill
        float norm = (v - d.min) / (d.max - d.min);
        g_.HLine(x, right, y + 8, D);
        g_.HLine(x, x + int(norm * (kCellW - 1)), y + 8, L);
    }
}

// ---- Footer tabs: selected tab is an inverted chip ----------------------------------
void Ui::DrawFooter(const UiState& s)
{
    g_.HLine(0, kSW - 1, kFootLine, D);

    // Bottom middle: the mode (LOOP / GRAINS / FX). Bottom right: which
    // page of it, e.g. "1/2" on GRAINS, "3/4" on FX (LOOP has one page).
    if(!s.shift)
    {
        g_.TextCentered(kSW / 2, kFootLine + 2, kPageNames[int(s.page)], W, Gfx::kBig);
        int cur = 0, n = 1;
        if(s.page == Page::Grains)
        {
            cur = s.grain_size_mode ? 1 : 0;
            n   = 2;
        }
        else if(s.page == Page::Fx)
        {
            cur = s.fx_page & 3;
            n   = kNumFxPages;
        }
        if(n > 1)
        {
            const char pg[4] = {char('1' + cur), '/', char('0' + n), 0};
            g_.TextRight(kCornerR, kFootLine + 3, pg, cur > 0 ? W : M, Gfx::kSmall);
        }
    }

    // Hold SHIFT to see the audio CPU load: peak (last second) on the left,
    // average on the right. Inverted when the safety valve is limiting grains.
    if(s.shift)
    {
        char buf[12];
        auto pct = [&buf](const char* tag, float v) {
            int n   = 0;
            int p   = int(v * 100.f + 0.5f);
            p       = p > 999 ? 999 : p;
            for(; *tag; tag++)
                buf[n++] = *tag;
            if(p >= 100)
                buf[n++] = char('0' + p / 100);
            if(p >= 10)
                buf[n++] = char('0' + (p / 10) % 10);
            buf[n++] = char('0' + p % 10);
            buf[n++] = '%';
            buf[n]   = 0;
            return buf;
        };
        g_.Text(kCornerL, kFootLine + 3, pct("PK", s.cpu_peak), L, Gfx::kSmall);
        // MIDI diagnostics: bytes per second, and what the D14 pin is doing
        char md[16] = "MIDI ";
        int  k = 5, b = s.midi_bps > 999 ? 999 : s.midi_bps;
        if(b >= 100)
            md[k++] = char('0' + b / 100);
        if(b >= 10)
            md[k++] = char('0' + (b / 10) % 10);
        md[k++] = char('0' + b % 10);
        const char* ls = s.midi_line == 1 ? " SIG" : (s.midi_line == 2 ? " LOW!" : " IDLE");
        for(; *ls; ls++)
            md[k++] = *ls;
        md[k] = 0;
        g_.TextCentered(kSW / 2, kFootLine + 3, md, s.midi_line == 1 ? L : M, Gfx::kSmall);
        const char* avg = pct("CPU", s.cpu_avg);
        if(s.throttled)
            g_.Chip(kCornerR - Gfx::TextWidth(avg, Gfx::kSmall) - 2, kFootLine + 1, avg, Gfx::kSmall);
        else
            g_.TextRight(kCornerR, kFootLine + 3, avg, L, Gfx::kSmall);
    }
}

// =============================================================================
// SAMPLE BROWSER
//
//   Root   : three folder tiles side by side (FACTORY · RECORDINGS · USER)
//   Folder : BACK row + scrolling list of samples with their lengths
//   Info   : the readout area describes whatever is highlighted
//   Hints  : the footer shows what turn / push / T1 will do right now
// =============================================================================

// [T1] chip: filled when the track is selected, outlined until then
void Ui::DrawTrackChip(const UiState& s)
{
    // Muted (SHIFT + track button): dark chip with dim text
    // Inset from the corner, one row down: the panel's corners are rounded
    g_.FillRect(kCornerL, 1, 13, 10, s.muted ? D : W);
    g_.Text(kCornerL + 1, 2, TrackName(s.track), s.muted ? L : B, Gfx::kBig);
}

void Ui::DrawMask(const char* const* rows, int h, int x, int y, uint16_t c)
{
    for(int r = 0; r < h; r++)
        for(int col = 0; rows[r][col]; col++)
            if(rows[r][col] == '#')
                g_.Pixel(x + col, y + r, c);
}

// Folder emblems, 7 x 7: a star for FACTORY, a record dot for RECORDINGS,
// a person for USER
static const char* const kEmblems[3][7] = {
    {"...#...", "..###..", "#######", ".#####.", "..###..", ".##.##.", "#.....#"},
    {"..###..", ".#####.", "##...##", "##...##", "##...##", ".#####.", "..###.."},
    {"..###..", "..###..", "..###..", ".......", ".#####.", "#######", "#######"},
};

// 26 x 20 pixel-art folder with its emblem
void Ui::DrawFolderIcon(int x, int y, int folder, bool selected)
{
    const uint16_t edge = selected ? W : M;
    const uint16_t body = selected ? W : D;
    const uint16_t mark = selected ? B : M;
    // Tab
    g_.FillRect(x + 1, y, 10, 3, body);
    g_.HLine(x + 1, x + 10, y, edge);
    g_.Pixel(x + 11, y + 1, edge);
    g_.Pixel(x, y + 1, edge);
    // Body
    g_.FillRect(x, y + 3, 26, 17, body);
    g_.Rect(x, y + 3, 26, 17, edge);
    // Back sheet peeking out (a "paper" edge)
    g_.HLine(x + 3, x + 22, y + 5, selected ? L : M);
    // Emblem
    DrawMask(kEmblems[folder], 7, x + 10, y + 9, mark);
}

void Ui::DrawFolderTiles(const Browser& b)
{
    const SampleLibrary& lib = *b.library();
    for(int f = 0; f < SampleLibrary::kFolders; f++)
    {
        const bool sel = f == b.cursor();
        const int  x = 1 + f * 46, y = 15, w = 45, h = 64;

        if(sel)
        {
            g_.FillRoundRect(x, y, w, h, D);
            g_.RoundRect(x, y, w, h, W);
            g_.RoundRect(x + 1, y + 1, w - 2, h - 2, W);
        }
        else
            g_.RoundRect(x, y, w, h, D);

        // Selected folder bobs gently
        int bob = sel && (frame_ / 12) % 2 == 0 ? -1 : 0;
        DrawFolderIcon(x + 10, y + 10 + bob, f, sel);

        g_.TextCentered(x + w / 2, y + 38, lib.FolderName(f), sel ? W : L, Gfx::kSmall);

        // File count
        char buf[12];
        int  n = lib.Count(f);
        if(n == 0)
            g_.TextCentered(x + w / 2, y + 47, "EMPTY", sel ? L : M, Gfx::kSmall);
        else
        {
            int k = 0;
            if(n >= 10)
                buf[k++] = char('0' + (n / 10) % 10);
            buf[k++] = char('0' + n % 10);
            buf[k++] = ' ';
            const char* word = n == 1 ? "FILE" : "FILES";
            for(; *word; word++)
                buf[k++] = *word;
            buf[k] = 0;
            g_.TextCentered(x + w / 2, y + 47, buf, sel ? L : M, Gfx::kSmall);
        }

        // Pointer under the selected tile
        if(sel)
            for(int i = 0; i < 3; i++)
                g_.HLine(x + w / 2 - i, x + w / 2 + i, y + h + 2 + i, W);
    }
}

// Seconds as "6.4S" (one decimal) — no printf, to save flash
static void FormatSeconds(float sec, char* out)
{
    int t = int(sec * 10.f + 0.5f);
    int k = 0;
    if(t >= 1000)
        out[k++] = char('0' + (t / 1000) % 10);
    if(t >= 100)
        out[k++] = char('0' + (t / 100) % 10);
    out[k++] = char('0' + (t / 10) % 10);
    out[k++] = '.';
    out[k++] = char('0' + t % 10);
    out[k++] = 'S';
    out[k]   = 0;
}

void Ui::DrawSampleList(const Browser& b)
{
    const SampleLibrary& lib   = *b.library();
    const int            f     = b.folder();
    const int            items = b.Items();
    const int            top   = 14, rowh = 11, x0 = 1, w = 132;

    for(int r = 0; r < Browser::kVisibleRows; r++)
    {
        int item = b.scroll() + r;
        if(item >= items)
            break;
        const int  y   = top + r * rowh;
        const bool sel = item == b.cursor();
        if(sel)
            g_.FillRect(x0, y, w, rowh - 1, W);

        if(item == 0) // BACK row
        {
            uint16_t c = sel ? B : M;
            for(int i = 0; i < 3; i++) // little left arrow
                g_.VLine(x0 + 3 + i, y + 4 - i, y + 4 + i, c);
            g_.Text(x0 + 9, y + 1, "BACK", c, Gfx::kBig);
            continue;
        }

        const SampleInfo& smp = lib.Sample(f, item - 1);
        g_.Text(x0 + 3, y + 1, smp.name, sel ? B : W, Gfx::kBig);
        char len[8];
        FormatSeconds(smp.seconds, len);
        g_.TextRight(x0 + w - 3, y + 2, len, sel ? B : M, Gfx::kSmall);
        if(!sel) // faint separator between rows
            g_.HLine(x0 + 3, x0 + w - 3, y + rowh - 1, D);
    }

    // Empty folder: say what to do about it
    if(lib.Count(f) == 0)
    {
        const char* line2 = f == 1 ? "RECORD A LOOP TO" : "ADD WAV FILES TO";
        const char* line3 = f == 1 ? "SAVE IT HERE" : "/USER ON THE SD CARD";
        g_.TextCentered(68, 40, "NOTHING HERE YET", L, Gfx::kBig);
        g_.TextCentered(68, 54, line2, M, Gfx::kSmall);
        g_.TextCentered(68, 61, line3, M, Gfx::kSmall);
    }

    // Scroll bar
    const int track_top = top, track_h = Browser::kVisibleRows * rowh - 1;
    g_.VLine(136, track_top, track_top + track_h - 1, D);
    if(items > Browser::kVisibleRows)
    {
        int thumb_h = track_h * Browser::kVisibleRows / items;
        int thumb_y = track_top + (track_h - thumb_h) * b.scroll() / (items - Browser::kVisibleRows);
        g_.VLine(136, thumb_y, thumb_y + thumb_h - 1, L);
        g_.VLine(137, thumb_y, thumb_y + thumb_h - 1, L);
    }
}

// The readout area becomes an info panel for the highlighted item
void Ui::DrawBrowserInfo(const Browser& b)
{
    const SampleLibrary& lib = *b.library();
    g_.HLine(0, kSW - 1, kReadLine, D);

    auto cell = [this](int i, const char* label, const char* value) {
        int x = 2 + (i % 2) * 70, y = kReadTop + (i / 2) * 11;
        g_.Text(x, y + 1, label, L, Gfx::kSmall);
        // Values that would collide with the label drop to the small font
        int room = kCellW - Gfx::TextWidth(label, Gfx::kSmall) - 4;
        if(Gfx::TextWidth(value, Gfx::kBig) <= room)
            g_.TextRight(x + kCellW - 1, y, value, W, Gfx::kBig);
        else
            g_.TextRight(x + kCellW - 1, y + 1, value, W, Gfx::kSmall);
        g_.HLine(x, x + kCellW - 1, y + 8, D);
    };

    char a[12], c[12];
    if(b.level() == Browser::kRoot)
    {
        int   f     = b.cursor();
        float total = 0.f;
        for(int i = 0; i < lib.Count(f); i++)
            total += lib.Sample(f, i).seconds;
        int n = lib.Count(f), k = 0;
        if(n >= 10)
            a[k++] = char('0' + (n / 10) % 10);
        a[k++] = char('0' + n % 10);
        a[k]   = 0;
        FormatSeconds(total, c);
        cell(0, "FILES", a);
        cell(1, "TOTAL", n ? c : "-");
        cell(2, "SOURCE", f == 0 ? "BUILT IN" : "SD CARD");
        cell(3, "LOADS TO", TrackName(b.track()));
    }
    else if(b.cursor() == 0)
    {
        cell(0, "BACK TO", "FOLDERS");
        cell(1, "IN", lib.FolderName(b.folder()));
    }
    else
    {
        const SampleInfo& smp = lib.Sample(b.folder(), b.cursor() - 1);
        FormatSeconds(smp.seconds, a);
        cell(0, "LENGTH", a);
        cell(1, "CHANNELS", smp.stereo ? "STEREO" : "MONO");
        cell(2, "FITS TAPE", smp.seconds <= 60.f ? "YES" : "TRIMMED");
        cell(3, "LOADS TO", TrackName(b.track()));
    }
}

// Footer: what each control does right now, keys bright, actions dim
void Ui::DrawBrowserHints(const Browser& b)
{
    g_.HLine(0, kSW - 1, kFootLine, D);
    const char* push = "OPEN";
    if(b.level() == Browser::kFolder)
        push = b.cursor() == 0 ? "BACK" : "LOAD";
    const char* keys[3]    = {"TURN", "PUSH", TrackName(b.track())};
    const char* actions[3] = {"MOVE", push, "EXIT"};
    int         total      = 0;
    for(int i = 0; i < 3; i++)
        total += Gfx::TextWidth(keys[i], Gfx::kSmall) + Gfx::TextWidth(actions[i], Gfx::kSmall) + 4 + 6;
    total -= 6;
    int x = kSW / 2 - total / 2;
    for(int i = 0; i < 3; i++)
    {
        x = g_.Text(x, kFootLine + 3, keys[i], W, Gfx::kSmall) + 3;
        x = g_.Text(x, kFootLine + 3, actions[i], M, Gfx::kSmall) + 6;
    }
}

void Ui::DrawBrowser(const UiState& s)
{
    const Browser&       b   = *s.browser;
    const SampleLibrary& lib = *b.library();

    // Header: [T1]  SAMPLES  or  [T1]  FACTORY   3/12
    DrawTrackChip(s);
    g_.Text(kCornerL + 16, 2, b.level() == Browser::kRoot ? "SAMPLES" : lib.FolderName(b.folder()), W, Gfx::kBig);
    if(b.level() == Browser::kFolder && lib.Count(b.folder()) > 0)
    {
        char pos[8];
        int  k = 0, cur = b.cursor(), n = lib.Count(b.folder());
        if(cur == 0)
            pos[k++] = '-';
        else
        {
            if(cur >= 10)
                pos[k++] = char('0' + (cur / 10) % 10);
            pos[k++] = char('0' + cur % 10);
        }
        pos[k++] = '/';
        if(n >= 10)
            pos[k++] = char('0' + (n / 10) % 10);
        pos[k++] = char('0' + n % 10);
        pos[k]   = 0;
        g_.TextRight(kCornerR, 3, pos, L, Gfx::kSmall);
    }
    g_.HLine(0, kSW - 1, 11, D);

    g_.SetClip(0, kBodyTop - 1, kSW - 1, kBodyBot);
    if(b.level() == Browser::kRoot)
        DrawFolderTiles(b);
    else
        DrawSampleList(b);
    g_.ResetClip();

    DrawBrowserInfo(b);
    DrawBrowserHints(b);
}

// Loading box over the LOOP page while a sample streams in
void Ui::DrawLoading(const UiState& s)
{
    const int x = 18, y = 26, w = 104, h = 38;
    g_.FillRect(x, y, w, h, B);
    g_.RoundRect(x, y, w, h, W);
    g_.RoundRect(x + 1, y + 1, w - 2, h - 2, M);
    g_.TextCentered(70, y + 5, "LOADING", L, Gfx::kSmall);
    g_.TextCentered(70, y + 13, s.load_name ? s.load_name : "", W, Gfx::kBig);
    const int bx = x + 8, by = y + 26, bw = w - 16;
    g_.FillRect(bx, by, bw, 5, D);
    g_.FillRect(bx, by, int(bw * fminf(1.f, s.load_progress)), 5, W);
}

} // namespace gloop
