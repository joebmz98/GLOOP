// =============================================================================
// hardware.h — pin map for the 1-track prototype (Arduino / DaisyDuino)
//
// In the STM32duino Daisy Seed variant, Arduino pin numbers ARE the Daisy
// "D" numbers printed on the Seed pinout (D15 = 15, D29 = 29, ...).
//
// Buttons sit on D0–D6 and D12. NOTE: D1–D6 are also the Seed's SD card
// pins (SDMMC). When the SD card goes in, those six buttons must move to a
// shift register (or other free pins); the code only needs this file changed.
//
// Free for later:
//   D13/D14 : USART1 (D14 = RX for TRS MIDI in)
//   D28–D30 : spare GPIO (D27 is reserved: the display's unused SPI MISO)
// =============================================================================
#pragma once
#include <stdint.h>

namespace gloop
{
namespace pins
{
// --- ST7789 240x280 IPS TFT on SPI1 ------------------------------------------
constexpr uint8_t kLcdSck  = 8;  // D8  (SPI1 SCK, the default Arduino SPI pin)
constexpr uint8_t kLcdMosi = 10; // D10 (SPI1 MOSI)
constexpr uint8_t kLcdCs   = 7;  // D7  held low (only device on the bus)
constexpr uint8_t kLcdDc   = 9;  // D9  data/command
constexpr uint8_t kLcdRst  = 11; // D11 reset
// SPI1 always claims a MISO (input) pin. Its default is D9 (PB4), which is our
// DC pin — that would silently kill DC and leave the screen blank. The display
// never sends data back, so MISO is parked on D27 (PG9) instead.
// Leave D27 UNCONNECTED.
constexpr uint8_t kLcdMisoUnused = 27; // D27
// Backlight: tie BLK to 3V3.

// --- 4 push encoders (A, B, push) -------------------------------------------
constexpr uint8_t kEncA[4]  = {15, 18, 21, 24};
constexpr uint8_t kEncB[4]  = {16, 19, 22, 25};
constexpr uint8_t kEncSw[4] = {17, 20, 23, 26};

// --- Buttons (to GND, internal pull-ups) ------------------------------------
constexpr uint8_t kBtnTrack1 = 0;  // D0  T1: select track / open sample browser
constexpr uint8_t kBtnTrack2 = 1;  // D1  T2: select track 2 (no engine yet)
constexpr uint8_t kBtnRec    = 2;  // D2
constexpr uint8_t kBtnPlay   = 3;  // D3
constexpr uint8_t kBtnLoop   = 4;  // D4
constexpr uint8_t kBtnGrains = 5;  // D5
constexpr uint8_t kBtnFx     = 6;  // D6
constexpr uint8_t kBtnShift  = 12; // D12

} // namespace pins
} // namespace gloop
