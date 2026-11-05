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
    Flush();
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

void St7789::Flush()
{
    SetWindow(0, 0, kWidth - 1, kHeight - 1);
    // One selection for the whole frame (the SPI peripheral can only move
    // 65,535 bytes per transfer, so it goes out in bands)
    const uint8_t* p = reinterpret_cast<const uint8_t*>(s_fb);
    digitalWrite(pins::kLcdCs, LOW);
    digitalWrite(pins::kLcdDc, HIGH);
    for(int b = 0; b < kBands; b++)
        SPI.transfer((void*)(p + size_t(b) * kBandBytes), kBandBytes, SPI_TRANSMITONLY);
    digitalWrite(pins::kLcdCs, HIGH);
}

} // namespace gloop
