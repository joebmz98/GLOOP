# SILT — granular looper prototype (1 track), Arduino IDE version

Tape looper → grain cloud → low-pass filter → reverb, with a 240×280 ST7789 TFT UI.

## Arduino IDE setup (once)

1. **File → Preferences → Additional boards manager URLs**, add:
   `https://github.com/stm32duino/BoardManagerFiles/raw/main/package_stmicroelectronics_index.json`
2. **Boards Manager** → install **STM32 MCU based boards** (STMicroelectronics).
3. **Library Manager** → install **DaisyDuino**.
4. Install **STM32CubeProgrammer** (needed for DFU upload).

## Board settings (Tools menu)

| Setting | Value |
|---|---|
| Board | STM32 MCU based boards → Generic STM32H7 Series |
| Board part number | Daisy Seed |
| Upload method | STM32CubeProgrammer (DFU) |
| USB support | None (or "CDC (generic Serial supersede U(S)ART)" for Serial debugging) |
| Optimize | Smallest (-Os default) is fine; "Faster (-O2)" recommended for audio |

**To upload:** hold BOOT on the Seed, tap RESET, release BOOT, then click Upload.

Last build: 75 KB / 128 KB flash · 151 KB RAM (framebuffer) · 23 MB SDRAM (60 s stereo tape + reverb).

## Wiring (prototype, direct GPIO — numbers are the Seed's D pins)

| Function | Seed pin |
|---|---|
| TFT SCK / MOSI | D8 / D10 |
| TFT CS / DC / RST | D7 / D9 / D11 |
| TFT BLK | 3V3 |
| Encoder 1 A / B / push | D15 / D16 / D17 |
| Encoder 2 A / B / push | D18 / D19 / D20 |
| Encoder 3 A / B / push | D21 / D22 / D23 |
| Encoder 4 A / B / push | D24 / D25 / D26 |
| REC / PLAY | D27 / D28 |
| LOOP / GRAINS / FX page | D0 / D12 / D13 |
| SHIFT | D29 |

Buttons and encoder commons go to GND (internal pull-ups).

## Controls

| Page | Enc 1 | Enc 2 | Enc 3 | Enc 4 |
|---|---|---|---|---|
| LOOP | Start | Length | Pitch (±24 st, varispeed) | Crossfade (0–250 ms) |
| GRAINS | Grains (0 = tape, 1–24) · SHIFT: Size | Spray | Spread | Envelope (10 shapes) |
| FX | Cutoff | Resonance | Reverb size | Reverb mix |

- SHIFT + turn = fine adjust (or the alternate parameter).
- Push an encoder to reset its parameter.
- REC starts a new take; press REC (or PLAY) again to close the loop and play it.
- PLAY toggles play/stop.

## Files (all in the sketch folder — the IDE shows them as tabs)

| File | What it does |
|---|---|
| `silt.ino` | Memory placement, audio callback, setup/loop |
| `tape_loop.h` | Record buffer, loop region, play head, seam crossfade |
| `grains.h` | Grain pool, scheduler, spray/spread/envelope |
| `envelopes.h` | 10 grain window lookup tables + level compensation |
| `fx.h` | Stereo SVF low-pass + ReverbSc (DaisySP inside DaisyDuino) |
| `controls.h` | Encoders/buttons → params and transport |
| `params.h/.cpp` | Parameter struct, page map, value formatting |
| `st7789.h/.cpp` | Display driver (Arduino SPI) |
| `gfx.h` | Drawing primitives + 5×7 font (placeholder for Alagard) |
| `ui.h/.cpp` | LOOP / GRAINS / FX screens |
