// =============================================================================
// gfx.h — 1-bit pixel-art drawing over the RGB565 framebuffer
//
// The UI is drawn on a LOGICAL 120x140 grid; every logical pixel becomes a
// 2x2 block on the 240x280 panel. That chunky grid is what gives the
// pixel-art look, and it halves the coordinates the UI has to think about.
//
// Greyscale pixel art: black, three greys and white (the palette lives in
// ui.cpp). Background layers sit in the greys and live things (the cat,
// rain, play heads, values) stay pure white, so layers separate clearly.
// Checkerboard dither (FillDither, dotted lines) is still available for
// texture.
//
// Two fonts:
//   kSmall : 3x5, advance 4 — labels, tabs, small text
//   kBig   : 5x7, advance 6 — page names and values
// Both stand in for Alagard; a bitmap Alagard can be added as a third font.
// =============================================================================
#pragma once
#include <cmath>
#include <cstdint>
#include <cstdlib>

namespace gloop
{
class Gfx
{
  public:
    enum Font
    {
        kSmall,
        kBig
    };

    static constexpr uint16_t kBlack = 0x0000;
    static constexpr uint16_t kWhite = 0xFFFF;

    void Init(uint16_t* fb, int phys_w, int phys_h, int scale)
    {
        fb_     = fb;
        pw_     = phys_w;
        ph_     = phys_h;
        s_      = scale;
        w_      = phys_w / scale;
        h_      = phys_h / scale;
        ResetClip();
    }

    int Width() const { return w_; }
    int Height() const { return h_; }

    // RGB565, pre-byte-swapped: the ST7789 wants the high byte first over
    // SPI and the Cortex-M7 is little-endian, so swapping once here means the
    // framebuffer can be sent as raw bytes.
    static constexpr uint16_t Swap(uint16_t c) { return uint16_t((c >> 8) | (c << 8)); }
    static constexpr uint16_t Rgb(uint8_t r, uint8_t g, uint8_t b)
    {
        return Swap(uint16_t(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)));
    }

    // ---- Clipping ----------------------------------------------------------------
    void SetClip(int x0, int y0, int x1, int y1)
    {
        cx0_ = x0;
        cy0_ = y0;
        cx1_ = x1;
        cy1_ = y1;
    }
    void ResetClip() { SetClip(0, 0, w_ - 1, h_ - 1); }

    // ---- Pixels ------------------------------------------------------------------
    void Fill(uint16_t c)
    {
        for(int i = 0; i < pw_ * ph_; i++)
            fb_[i] = c;
    }

    inline bool InClip(int x, int y) const
    {
        return x >= cx0_ && x <= cx1_ && y >= cy0_ && y <= cy1_;
    }

    inline void Pixel(int x, int y, uint16_t c)
    {
        if(!InClip(x, y))
            return;
        uint16_t* p = fb_ + (y * s_) * pw_ + x * s_;
        for(int j = 0; j < s_; j++, p += pw_)
            for(int i = 0; i < s_; i++)
                p[i] = c;
    }

    // XOR a logical pixel: black↔white
    inline void Invert(int x, int y)
    {
        if(!InClip(x, y))
            return;
        uint16_t* p = fb_ + (y * s_) * pw_ + x * s_;
        for(int j = 0; j < s_; j++, p += pw_)
            for(int i = 0; i < s_; i++)
                p[i] ^= 0xFFFF;
    }

    // Checkerboard test used for all dithering
    static inline bool Checker(int x, int y) { return ((x + y) & 1) == 0; }

    // ---- Lines -----------------------------------------------------------------
    void HLine(int x0, int x1, int y, uint16_t c, bool dotted = false)
    {
        if(x0 > x1)
        {
            int t = x0;
            x0    = x1;
            x1    = t;
        }
        for(int x = x0; x <= x1; x++)
            if(!dotted || Checker(x, y))
                Pixel(x, y, c);
    }

    void VLine(int x, int y0, int y1, uint16_t c, bool dotted = false)
    {
        if(y0 > y1)
        {
            int t = y0;
            y0    = y1;
            y1    = t;
        }
        for(int y = y0; y <= y1; y++)
            if(!dotted || Checker(x, y))
                Pixel(x, y, c);
    }

    // Bresenham. mode: 0 = draw, 1 = invert, 2 = dotted
    void Line(int x0, int y0, int x1, int y1, uint16_t c, int mode = 0)
    {
        int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
        int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
        int err = dx + dy;
        while(true)
        {
            if(mode == 1)
                Invert(x0, y0);
            else if(mode == 0 || Checker(x0, y0))
                Pixel(x0, y0, c);
            if(x0 == x1 && y0 == y1)
                break;
            int e2 = 2 * err;
            if(e2 >= dy)
            {
                err += dy;
                x0 += sx;
            }
            if(e2 <= dx)
            {
                err += dx;
                y0 += sy;
            }
        }
    }

    // ---- Rectangles ----------------------------------------------------------------
    void Rect(int x, int y, int w, int h, uint16_t c)
    {
        HLine(x, x + w - 1, y, c);
        HLine(x, x + w - 1, y + h - 1, c);
        VLine(x, y, y + h - 1, c);
        VLine(x + w - 1, y, y + h - 1, c);
    }

    void FillRect(int x, int y, int w, int h, uint16_t c)
    {
        for(int j = y; j < y + h; j++)
            HLine(x, x + w - 1, j, c);
    }

    void FillDither(int x, int y, int w, int h, uint16_t c)
    {
        for(int j = y; j < y + h; j++)
            HLine(x, x + w - 1, j, c, true);
    }

    void InvertRect(int x, int y, int w, int h)
    {
        for(int j = y; j < y + h; j++)
            for(int i = x; i < x + w; i++)
                Invert(i, j);
    }

    // Rectangle with 1-pixel cut corners (pixel-art "rounded")
    void RoundRect(int x, int y, int w, int h, uint16_t c)
    {
        HLine(x + 1, x + w - 2, y, c);
        HLine(x + 1, x + w - 2, y + h - 1, c);
        VLine(x, y + 1, y + h - 2, c);
        VLine(x + w - 1, y + 1, y + h - 2, c);
    }

    void FillRoundRect(int x, int y, int w, int h, uint16_t c)
    {
        HLine(x + 1, x + w - 2, y, c);
        FillRect(x, y + 1, w, h - 2, c);
        HLine(x + 1, x + w - 2, y + h - 1, c);
    }

    // ---- Circles -----------------------------------------------------------------
    void Circle(int cx, int cy, int r, uint16_t c, bool dotted = false)
    {
        int x = r, y = 0, err = 1 - r;
        while(x >= y)
        {
            const int px[8] = {cx + x, cx + y, cx - y, cx - x, cx - x, cx - y, cx + y, cx + x};
            const int py[8] = {cy + y, cy + x, cy + x, cy + y, cy - y, cy - x, cy - x, cy - y};
            for(int k = 0; k < 8; k++)
                if(!dotted || Checker(px[k], py[k]))
                    Pixel(px[k], py[k], c);
            y++;
            if(err < 0)
                err += 2 * y + 1;
            else
            {
                x--;
                err += 2 * (y - x) + 1;
            }
        }
    }

    void FillCircle(int cx, int cy, int r, uint16_t c, bool dither = false)
    {
        for(int dy = -r; dy <= r; dy++)
        {
            int dx = int(sqrtf(float(r * r - dy * dy)) + 0.5f);
            HLine(cx - dx, cx + dx, cy + dy, c, dither);
        }
    }

    // ---- Text ------------------------------------------------------------------
    static int Advance(Font f) { return f == kSmall ? 4 : 6; }
    static int GlyphH(Font f) { return f == kSmall ? 5 : 7; }

    static int TextWidth(const char* s, Font f)
    {
        int n = 0;
        while(s[n])
            n++;
        return n ? n * Advance(f) - 1 : 0;
    }

    int Text(int x, int y, const char* s, uint16_t c, Font f = kBig)
    {
        for(; *s; s++)
        {
            if(f == kSmall)
                Glyph3x5(x, y, *s, c);
            else
                Glyph5x7(x, y, *s, c);
            x += Advance(f);
        }
        return x;
    }

    void TextCentered(int cx, int y, const char* s, uint16_t c, Font f = kBig)
    {
        Text(cx - TextWidth(s, f) / 2, y, s, c, f);
    }

    void TextRight(int right, int y, const char* s, uint16_t c, Font f = kBig)
    {
        Text(right - TextWidth(s, f) + 1, y, s, c, f);
    }

    // Text on a filled chip (black text on white), returns chip width
    int Chip(int x, int y, const char* s, Font f)
    {
        int w = TextWidth(s, f) + 4;
        FillRect(x, y, w, GlyphH(f) + 4, kWhite);
        Text(x + 2, y + 2, s, kBlack, f);
        return w;
    }

  private:
    static int Index(char ch, const char* map)
    {
        if(ch >= 'a' && ch <= 'z')
            ch = char(ch - 32);
        for(int i = 0; map[i]; i++)
            if(map[i] == ch)
                return i;
        return -1;
    }

    static constexpr const char* kMap = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ.:+-%/<>#";

    // 3x5: 5 rows, bit 2 = left column
    void Glyph3x5(int x, int y, char ch, uint16_t c)
    {
        static const uint8_t kG[][5] = {
            {7, 5, 5, 5, 7}, {2, 6, 2, 2, 7}, {7, 1, 7, 4, 7}, {7, 1, 7, 1, 7}, // 0-3
            {5, 5, 7, 1, 1}, {7, 4, 7, 1, 7}, {7, 4, 7, 5, 7}, {7, 1, 1, 2, 2}, // 4-7
            {7, 5, 7, 5, 7}, {7, 5, 7, 1, 7},                                   // 8 9
            {2, 5, 7, 5, 5}, {6, 5, 6, 5, 6}, {3, 4, 4, 4, 3}, {6, 5, 5, 5, 6}, // A-D
            {7, 4, 6, 4, 7}, {7, 4, 6, 4, 4}, {3, 4, 5, 5, 3}, {5, 5, 7, 5, 5}, // E-H
            {7, 2, 2, 2, 7}, {1, 1, 1, 5, 2}, {5, 5, 6, 5, 5}, {4, 4, 4, 4, 7}, // I-L
            {5, 7, 7, 5, 5}, {6, 5, 5, 5, 5}, {2, 5, 5, 5, 2}, {6, 5, 6, 4, 4}, // M-P
            {2, 5, 5, 6, 3}, {6, 5, 6, 5, 5}, {3, 4, 2, 1, 6}, {7, 2, 2, 2, 2}, // Q-T
            {5, 5, 5, 5, 7}, {5, 5, 5, 5, 2}, {5, 5, 7, 7, 5}, {5, 5, 2, 5, 5}, // U-X
            {5, 5, 2, 2, 2}, {7, 1, 2, 4, 7},                                   // Y Z
            {0, 0, 0, 0, 2}, {0, 2, 0, 2, 0}, {0, 2, 7, 2, 0}, {0, 0, 7, 0, 0}, // . : + -
            {5, 1, 2, 4, 5}, {1, 1, 2, 4, 4}, {1, 2, 4, 2, 1}, {4, 2, 1, 2, 4}, // % / < >
            {5, 7, 5, 7, 5},                                                    // #
        };
        int i = Index(ch, kMap);
        if(i < 0)
            return;
        for(int row = 0; row < 5; row++)
            for(int col = 0; col < 3; col++)
                if(kG[i][row] & (4 >> col))
                    Pixel(x + col, y + row, c);
    }

    // 5x7: column-major, bit 0 = top row
    void Glyph5x7(int x, int y, char ch, uint16_t c)
    {
        static const uint8_t kG[][5] = {
            {0x3E, 0x51, 0x49, 0x45, 0x3E}, {0x00, 0x42, 0x7F, 0x40, 0x00}, // 0 1
            {0x42, 0x61, 0x51, 0x49, 0x46}, {0x21, 0x41, 0x45, 0x4B, 0x31}, // 2 3
            {0x18, 0x14, 0x12, 0x7F, 0x10}, {0x27, 0x45, 0x45, 0x45, 0x39}, // 4 5
            {0x3C, 0x4A, 0x49, 0x49, 0x30}, {0x01, 0x71, 0x09, 0x05, 0x03}, // 6 7
            {0x36, 0x49, 0x49, 0x49, 0x36}, {0x06, 0x49, 0x49, 0x29, 0x1E}, // 8 9
            {0x7E, 0x11, 0x11, 0x11, 0x7E}, {0x7F, 0x49, 0x49, 0x49, 0x36}, // A B
            {0x3E, 0x41, 0x41, 0x41, 0x22}, {0x7F, 0x41, 0x41, 0x22, 0x1C}, // C D
            {0x7F, 0x49, 0x49, 0x49, 0x41}, {0x7F, 0x09, 0x09, 0x09, 0x01}, // E F
            {0x3E, 0x41, 0x49, 0x49, 0x7A}, {0x7F, 0x08, 0x08, 0x08, 0x7F}, // G H
            {0x00, 0x41, 0x7F, 0x41, 0x00}, {0x20, 0x40, 0x41, 0x3F, 0x01}, // I J
            {0x7F, 0x08, 0x14, 0x22, 0x41}, {0x7F, 0x40, 0x40, 0x40, 0x40}, // K L
            {0x7F, 0x02, 0x0C, 0x02, 0x7F}, {0x7F, 0x04, 0x08, 0x10, 0x7F}, // M N
            {0x3E, 0x41, 0x41, 0x41, 0x3E}, {0x7F, 0x09, 0x09, 0x09, 0x06}, // O P
            {0x3E, 0x41, 0x51, 0x21, 0x5E}, {0x7F, 0x09, 0x19, 0x29, 0x46}, // Q R
            {0x46, 0x49, 0x49, 0x49, 0x31}, {0x01, 0x01, 0x7F, 0x01, 0x01}, // S T
            {0x3F, 0x40, 0x40, 0x40, 0x3F}, {0x1F, 0x20, 0x40, 0x20, 0x1F}, // U V
            {0x3F, 0x40, 0x38, 0x40, 0x3F}, {0x63, 0x14, 0x08, 0x14, 0x63}, // W X
            {0x07, 0x08, 0x70, 0x08, 0x07}, {0x61, 0x51, 0x49, 0x45, 0x43}, // Y Z
            {0x00, 0x60, 0x60, 0x00, 0x00}, {0x00, 0x36, 0x36, 0x00, 0x00}, // . :
            {0x08, 0x08, 0x3E, 0x08, 0x08}, {0x08, 0x08, 0x08, 0x08, 0x08}, // + -
            {0x23, 0x13, 0x08, 0x64, 0x62}, {0x20, 0x10, 0x08, 0x04, 0x02}, // % /
            {0x08, 0x14, 0x22, 0x41, 0x00}, {0x00, 0x41, 0x22, 0x14, 0x08}, // < >
            {0x14, 0x7F, 0x14, 0x7F, 0x14},                                 // #
        };
        int i = Index(ch, kMap);
        if(i < 0)
            return;
        for(int col = 0; col < 5; col++)
            for(int row = 0; row < 7; row++)
                if(kG[i][col] & (1 << row))
                    Pixel(x + col, y + row, c);
    }

    uint16_t* fb_ = nullptr;
    int       pw_ = 0, ph_ = 0, w_ = 0, h_ = 0, s_ = 1;
    int       cx0_ = 0, cy0_ = 0, cx1_ = 0, cy1_ = 0;
};

} // namespace gloop
