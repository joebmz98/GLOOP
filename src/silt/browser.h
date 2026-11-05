// =============================================================================
// browser.h — the sample browser (state + navigation) and the sample library
//
// Opening it:  press the track button to select the track, then press it
//              again while on the LOOP page.
// In the menu: turn ANY encoder to move, push ANY encoder to open / choose.
//              SHIFT + turn jumps 5 rows. The track button closes the menu,
//              as does pressing a page button.
//
//   Root      : three folders  FACTORY · RECORDINGS · USER
//   Folder    : "BACK" row, then the samples in that folder
//   Choose    : the menu closes and a load request is raised for loop() to
//               carry out (SD reads must never happen in the audio callback)
//
// SampleLibrary is a STAND-IN until the SD card is wired: it serves a fixed
// list of example names so the menu can be built and tried. Its four methods
// are the whole interface the menu needs; the SD version fills them by
// scanning /FACTORY, /RECORDINGS and /USER for .wav files at start-up.
// =============================================================================
#pragma once
#include <cstdint>

namespace silt
{
struct SampleInfo
{
    const char* name;    // display name, upper case, ≤ 16 characters
    float       seconds; // length
    bool        stereo;
};

class SampleLibrary
{
  public:
    static constexpr int kFolders = 3;

    const char* FolderName(int f) const
    {
        static const char* const kNames[kFolders] = {"FACTORY", "RECORDINGS", "USER"};
        return kNames[f];
    }

    int Count(int f) const { return kCounts[f]; }

    const SampleInfo& Sample(int f, int i) const
    {
        switch(f)
        {
            case 0: return kFactory[i];
            case 1: return kRecordings[i];
            default: return kFactory[0]; // USER is empty in the stand-in
        }
    }

  private:
    // ---- Example entries (placeholders until the SD card is read) ----------
    static constexpr SampleInfo kFactory[12] = {
        {"DUSTY PIANO", 6.40f, true},
        {"GLASS PAD", 8.00f, true},
        {"TAPE CHOIR", 5.25f, true},
        {"VINYL CRACKLE", 4.00f, true},
        {"FIELD RAIN", 12.0f, true},
        {"KALIMBA", 2.10f, false},
        {"BOWED METAL", 7.80f, true},
        {"MUSIC BOX", 3.60f, false},
        {"CELLO DRONE", 10.5f, false},
        {"SYNTH SWELL", 4.80f, true},
        {"WIND CHIMES", 9.20f, true},
        {"BREATH NOISE", 3.00f, false},
    };
    static constexpr SampleInfo kRecordings[4] = {
        {"REC 001", 1.92f, true},
        {"REC 002", 4.36f, true},
        {"REC 003", 0.88f, true},
        {"REC 004", 12.4f, true},
    };
    static constexpr int kCounts[kFolders] = {12, 4, 0};
};

class Browser
{
  public:
    enum Level
    {
        kRoot,   // choosing a folder
        kFolder, // inside a folder; row 0 is BACK
    };
    static constexpr int kVisibleRows = 6; // rows on screen in folder view

    void Init(const SampleLibrary* lib) { lib_ = lib; }

    bool IsOpen() const { return open_; }
    void Open()
    {
        open_  = true;
        level_ = kRoot;
        // Start on the folder used last time
        cursor_ = folder_;
        scroll_ = 0;
    }
    void Close() { open_ = false; }

    // Turn from any encoder (steps already summed); fast = SHIFT held
    void Turn(int steps, bool fast)
    {
        if(!open_ || steps == 0)
            return;
        cursor_ += steps * (fast ? 5 : 1);
        int n = Items();
        if(cursor_ < 0)
            cursor_ = 0;
        if(cursor_ > n - 1)
            cursor_ = n - 1;
        KeepVisible();
    }

    // Push from any encoder
    void Push()
    {
        if(!open_)
            return;
        if(level_ == kRoot)
        {
            folder_ = cursor_;
            level_  = kFolder;
            cursor_ = lib_->Count(folder_) > 0 ? 1 : 0; // first sample, or BACK
            scroll_ = 0;
        }
        else if(cursor_ == 0) // BACK
        {
            level_  = kRoot;
            cursor_ = folder_;
            scroll_ = 0;
        }
        else // choose a sample
        {
            req_folder_ = folder_;
            req_index_  = cursor_ - 1;
            request_    = true;
            open_       = false;
        }
    }

    // loop() picks the request up once
    bool ConsumeLoadRequest(int& folder, int& index)
    {
        if(!request_)
            return false;
        request_ = false;
        folder   = req_folder_;
        index    = req_index_;
        return true;
    }

    // ---- For the UI ----------------------------------------------------------
    Level                level() const { return level_; }
    int                  cursor() const { return cursor_; }
    int                  scroll() const { return scroll_; }
    int                  folder() const { return folder_; }
    int                  Items() const { return level_ == kRoot ? SampleLibrary::kFolders : 1 + lib_->Count(folder_); }
    const SampleLibrary* library() const { return lib_; }

  private:
    void KeepVisible()
    {
        if(level_ != kFolder)
            return;
        if(cursor_ < scroll_)
            scroll_ = cursor_;
        if(cursor_ >= scroll_ + kVisibleRows)
            scroll_ = cursor_ - kVisibleRows + 1;
    }

    const SampleLibrary* lib_        = nullptr;
    volatile bool        open_       = false;
    Level                level_      = kRoot;
    int                  cursor_     = 0;
    int                  scroll_     = 0;
    int                  folder_     = 0;
    volatile bool        request_    = false;
    int                  req_folder_ = 0, req_index_ = 0;
};

} // namespace silt
