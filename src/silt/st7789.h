// =============================================================================
// st7789.h — minimal ST7789V2 driver (240x280 IPS), Arduino SPI version
//
// The framebuffer (134 KB, RGB565) is a plain global array; the STM32duino
// Daisy linker script puts .bss in the 512 KB AXI SRAM, so it fits.
//
// Flush() pushes the frame with blocking SPI transfers from loop(). The
// audio callback is an interrupt, so it keeps running while the frame is
// sent — a slow frame only lowers the frame rate, never glitches audio.
// =============================================================================
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace silt
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

    // SPI clock request; the core picks the nearest rate at or below this.
    // Drop to 20 MHz if long breadboard wires give a noisy picture.
    static constexpr uint32_t kSpiHz = 40000000;

    void      Init();
    uint16_t* Framebuffer();
    void      Flush();
    bool      Busy() const { return false; } // blocking driver: never busy

  private:
    // STM32H7 SPI can move at most 65535 bytes per transfer: send 10 bands
    static constexpr int    kBands     = 10;
    static constexpr size_t kBandBytes = size_t(kWidth) * (kHeight / kBands) * 2;

    void Command(uint8_t cmd);
    void Data(const uint8_t* data, size_t len);
    void Data8(uint8_t v) { Data(&v, 1); }
    void SetWindow(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1);
};

} // namespace silt
