// =============================================================================
// st7789.h — minimal ST7789V2 driver (240x280 IPS), Arduino SPI version
//
// The framebuffer (134 KB, RGB565) is a plain global array; the STM32duino
// Daisy linker script puts .bss in the 512 KB AXI SRAM, so it fits.
//
// Flush() pushes the frame with blocking SPI transfers from loop(). The
// audio callback is an interrupt, so it keeps running while the frame is
// sent — a slow frame only lowers the frame rate, never glitches audio.
//
// Speed (a full 280x240 frame is 134 KB):
//  * SPI clock: SPI1 runs from PLL2P = 50 MHz and divides by powers of 2, so
//    the choices are 25 MHz, 12.5 MHz, ... (a 20 MHz request gives 12.5).
//    kFastSpi moves SPI1 onto PLL1Q (192 MHz at 480 MHz CPU, see clock.cpp)
//    for 48 MHz.
//  * Dirty bands: Flush() compares the frame with a copy of what the panel
//    already shows and only sends the changed part of each 8-row band. Most
//    frames only change a few small areas (reels, rain, meters).
// =============================================================================
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace gloop
{
class St7789
{
  public:
    // LANDSCAPE: the 240x280 panel is rotated 90 degrees by the controller
    // itself (MADCTL), so the framebuffer is simply 280 wide x 240 tall.
    static constexpr int kWidth  = 280;
    static constexpr int kHeight = 240;
    // The 280-pixel axis sits 20 pixels into the controller's 320-pixel RAM.
    // After rotation that axis is X, so the offset moves to the columns.
    static constexpr int kColOffset = 20;
    static constexpr int kRowOffset = 0;
    // MADCTL: 0x60 = MV|MX (rotated 90 degrees clockwise).
    // If the picture is upside down for how you mount the screen, use 0xA0.
    static constexpr uint8_t kMadctl = 0x60;

    // SPI clock. false: 25 MHz (fine on a breadboard with short wires).
    // true: 48 MHz (PLL1Q 192 MHz / 4), for a PCB or very short wires; if the
    // picture shows speckles or shifted colours, go back to false.
    static constexpr bool     kFastSpi = false;
    static constexpr uint32_t kSpiHz   = kFastSpi ? 48000000 : 25000000;

    void      Init();
    uint16_t* Framebuffer();
    void      Flush();      // send only what changed since the last Flush
    void      FlushAll();   // send the whole frame
    uint32_t  LastBytes() const { return last_bytes_; } // pixel bytes sent
    bool      Busy() const { return false; } // blocking driver: never busy

  private:
    // Dirty-band height in rows (one band ≤ 65535 bytes: one SPI transfer)
    static constexpr int kBandRows = 8;
    uint32_t             last_bytes_ = 0;

    void Command(uint8_t cmd);
    void Data(const uint8_t* data, size_t len);
    void Data8(uint8_t v) { Data(&v, 1); }
    void SetWindow(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1);
};

} // namespace gloop
