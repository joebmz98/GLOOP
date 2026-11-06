// =============================================================================
// st7789.cpp — Arduino SPI version
// =============================================================================
#include <Arduino.h>
#include <SPI.h>
#include "st7789.h"
#include "hardware.h"

namespace gloop
{
static uint16_t s_fb[St7789::kWidth * St7789::kHeight];
// What the panel shows right now (for the dirty-band compare). 4-byte aligned
// so it can be compared two pixels at a time.
alignas(4) static uint16_t s_shown[St7789::kWidth * St7789::kHeight];
// One band's changed rectangle, packed for a single SPI transfer
static uint16_t s_band[St7789::kWidth * 8];

uint16_t* St7789::Framebuffer()
{
    return s_fb;
}

void St7789::Init()
{
    pinMode(pins::kLcdCs, OUTPUT);
    pinMode(pins::kLcdDc, OUTPUT);
    pinMode(pins::kLcdRst, OUTPUT);
    // Keep the display DESELECTED until SPI is set up: SPI.begin() can put a
    // glitch on the clock line, which a selected display would read as a bit
    // and then stay one bit out of step for every byte after it.
    digitalWrite(pins::kLcdCs, HIGH);

    if(kFastSpi)
    {
        // SPI1/2/3 kernel clock: PLL2P (50 MHz) → PLL1Q (192 MHz), so the
        // ÷4 prescaler gives 48 MHz. Must happen before SPI.begin(), which
        // reads this clock to pick the prescaler.
        __HAL_RCC_PLLCLKOUT_ENABLE(RCC_PLL1_DIVQ);
        __HAL_RCC_SPI123_CONFIG(RCC_SPI123CLKSOURCE_PLL);
    }
    SPI.setMOSI(pins::kLcdMosi);
    SPI.setSCLK(pins::kLcdSck);
    // Must come before begin(): otherwise SPI takes D9 (our DC pin) as MISO
    SPI.setMISO(pins::kLcdMisoUnused);
    SPI.begin();
    // Re-assert DC as a plain output after SPI has set up its pins
    pinMode(pins::kLcdDc, OUTPUT);
    SPI.beginTransaction(SPISettings(kSpiHz, MSBFIRST, SPI_MODE0));

    // --- Hardware reset ---------------------------------------------------------
    digitalWrite(pins::kLcdRst, HIGH);
    delay(5);
    digitalWrite(pins::kLcdRst, LOW);
    delay(20);
    digitalWrite(pins::kLcdRst, HIGH);
    delay(150);

    // --- Init sequence ------------------------------------------------------------
    Command(0x01); // SWRESET
    delay(150);
    Command(0x11); // SLPOUT
    delay(120);
    Command(0x3A); // COLMOD
    Data8(0x55);   //   16-bit RGB565
    Command(0x36);   // MADCTL
    Data8(kMadctl);  //   landscape (see st7789.h)
    Command(0x21); // INVON — IPS panels need inversion on for correct colours
    Command(0x13); // NORON
    Command(0x29); // DISPON
    delay(20);

    for(auto& px : s_fb)
        px = 0;
    FlushAll();
}

// Each command / data burst selects the display (CS low) only while it is
// being sent, so any stray clock edge between bursts is ignored.
void St7789::Command(uint8_t cmd)
{
    digitalWrite(pins::kLcdCs, LOW);
    digitalWrite(pins::kLcdDc, LOW);
    SPI.transfer(cmd, SPI_TRANSMITONLY);
    digitalWrite(pins::kLcdCs, HIGH);
}

void St7789::Data(const uint8_t* data, size_t len)
{
    digitalWrite(pins::kLcdCs, LOW);
    digitalWrite(pins::kLcdDc, HIGH);
    // transmit-only: the buffer is sent, not overwritten with received bytes
    SPI.transfer((void*)data, len, SPI_TRANSMITONLY);
    digitalWrite(pins::kLcdCs, HIGH);
}

void St7789::SetWindow(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    x0 += kColOffset;
    x1 += kColOffset;
    y0 += kRowOffset;
    y1 += kRowOffset;
    uint8_t ca[4] = {uint8_t(x0 >> 8), uint8_t(x0), uint8_t(x1 >> 8), uint8_t(x1)};
    uint8_t ra[4] = {uint8_t(y0 >> 8), uint8_t(y0), uint8_t(y1 >> 8), uint8_t(y1)};
    Command(0x2A); // CASET
    Data(ca, 4);
    Command(0x2B); // RASET
    Data(ra, 4);
    Command(0x2C); // RAMWR — pixel data follows
}

void St7789::FlushAll()
{
    // Mark everything as different, then send it as dirty bands
    for(size_t i = 0; i < sizeof(s_shown) / sizeof(s_shown[0]); i++)
        s_shown[i] = uint16_t(~s_fb[i]);
    Flush();
}

// For each 8-row band: find the leftmost and rightmost changed pixel pair
// (compared 32 bits at a time), pack that rectangle into s_band, send it in
// one SPI transfer and remember it as shown. Unchanged bands cost only the
// compare (~10 µs for the whole screen at 400 MHz).
void St7789::Flush()
{
    static_assert(kBandRows == 8 && kHeight % kBandRows == 0, "s_band size");
    constexpr int kWords = kWidth / 2; // pixel pairs per row
    const uint32_t* fb   = reinterpret_cast<const uint32_t*>(s_fb);
    uint32_t*       shown = reinterpret_cast<uint32_t*>(s_shown);
    last_bytes_          = 0;

    for(int y0 = 0; y0 < kHeight; y0 += kBandRows)
    {
        int lo = kWords, hi = -1;
        for(int y = y0; y < y0 + kBandRows; y++)
        {
            const uint32_t* a = fb + y * kWords;
            const uint32_t* b = shown + y * kWords;
            int             l = 0, r = kWords - 1;
            while(l < lo && a[l] == b[l])
                l++;
            lo = l < lo ? l : lo;
            while(r > hi && a[r] == b[r])
                r--;
            hi = r > hi ? r : hi;
        }
        if(hi < lo)
            continue; // nothing changed in this band

        // Pack the changed rectangle (and mark it as shown)
        const int w   = (hi - lo + 1) * 2; // in pixels
        uint16_t* dst = s_band;
        for(int y = y0; y < y0 + kBandRows; y++)
        {
            const uint16_t* src = s_fb + y * kWidth + lo * 2;
            uint16_t*       sh  = s_shown + y * kWidth + lo * 2;
            for(int x = 0; x < w; x++)
                dst[x] = sh[x] = src[x];
            dst += w;
        }
        const size_t bytes = size_t(w) * kBandRows * 2;
        SetWindow(uint16_t(lo * 2), uint16_t(y0), uint16_t(lo * 2 + w - 1),
                  uint16_t(y0 + kBandRows - 1));
        Data(reinterpret_cast<const uint8_t*>(s_band), bytes);
        last_bytes_ += bytes;
    }
}

} // namespace gloop
