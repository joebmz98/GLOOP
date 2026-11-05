# GLOOP — granular looper firmware (1 track prototype)

Daisy Seed firmware, built with the Arduino IDE: tape looper → grain cloud → low-pass filter → reverb, with a 240×280 ST7789 TFT (landscape) and a sample browser.

## Arduino IDE setup (once)

1. **File → Preferences → Additional boards manager URLs**:
   `https://github.com/stm32duino/BoardManagerFiles/raw/main/package_stmicroelectronics_index.json`
2. **Boards Manager** → install **STM32 MCU based boards** (STMicroelectronics).
3. **Library Manager** → install **DaisyDuino**.
4. Install **STM32CubeProgrammer** (for DFU upload).

## Board settings (Tools menu)

| Setting | Value |
|---|---|
| Board | STM32 MCU based boards → Generic STM32H7 Series |
| Board part number | Daisy Seed |
| Upload method | STM32CubeProgrammer (DFU) |
| USB support | None (CDC adds ~12 KB of flash) |
| Optimize | **Faster (-O2) with LTO** (79% of flash) |

"Faster (-O2) with LTO" also speeds up the reverb and filter, which live in the DaisyDuino library. If flash runs out as features are added, "Smallest (-Os default)" (66%) still works well: GLOOP's own audio code switches itself to `-O2` with `#pragma GCC optimize`. "Faster/Fastest" **without** LTO overflows the 128 KB flash.

**Upload:** hold BOOT, tap RESET, release BOOT, then Upload.

Open `gloop_fw/gloop_fw.ino` (the Arduino IDE needs the folder and the `.ino` to share a name).

## Wiring (Seed D pins / physical pins)

| Function | Seed pin |
|---|---|
| TFT SCL / SDA | D8 (pin 9) / D10 (pin 11) |
| TFT DC / CS / RES | D9 (pin 10) / D7 (pin 8) / D11 (pin 12) |
| TFT VCC + BLK / GND | 3V3D (pin 38) / DGND (pin 40) |
| **D27 (pin 34)** | **leave unconnected** — the display's unused SPI MISO |
| Encoder 1 A / B / push | D15 / D16 / D17 (pins 22–24) |
| Encoder 2 A / B / push | D18 / D19 / D20 (pins 25–27) |
| Encoder 3 A / B / push | D21 / D22 / D23 (pins 28–30) |
| Encoder 4 A / B / push | D24 / D25 / D26 (pins 31–33) |
| T1 / T2 | D0 (pin 1) / D1 (pin 2) |
| REC / PLAY | D2 (pin 3) / D3 (pin 4) |
| LOOP / GRAINS / FX | D4 / D5 / D6 (pins 5–7) |
| SHIFT | D12 (pin 13) |
| Audio IN L / R | pin 16 / 17 (TRS tip / ring) |
| Audio OUT L / R | pin 18 / 19 (TRS tip / ring) |
| Jack sleeves | AGND (pin 20) |

Buttons and encoder commons go to DGND (pin 40); internal pull-ups are used.
D1–D6 are also the Seed's SD card pins: when SD loading is added, the buttons on them move to a shift register.

## Controls

| Page | Enc 1 | Enc 2 | Enc 3 | Enc 4 |
|---|---|---|---|---|
| LOOP | Start | Length | Pitch (±24 st) | Crossfade |
| GRAINS | Grains (0 = tape, 1–12) · SHIFT: Size | Spray | Spread | Envelope |
| FX | Cutoff | Resonance | Reverb size | Reverb mix |

- SHIFT + turn = fine adjust (or the alternate parameter). Push an encoder to reset its parameter.
- REC starts a take; REC or PLAY again closes the loop and plays it. PLAY toggles play/stop. A take stops by itself at **30 s** (`kMaxLoopSeconds`).
- T1: first press selects the track; again on LOOP opens the sample browser (turn any encoder to move, push to open / load).
- Hold SHIFT to show CPU load (peak / average) in the footer.

## Performance limits

| Limit | Value | Where |
|---|---|---|
| Grain voices per track | 12 | `GrainCloud::kMaxGrains` (`grains.h`) |
| Loop length per track | 30 s (16-bit stereo, 5.8 MB) | `kMaxLoopSeconds` (`gloop_fw.ino`) |
| CPU safety valve | stops new grains above 80% average / 90% peak, resumes below 70% | `gloop_fw.ino` |
| Output | soft limiter from 0.8 of full scale | `OutputLimit()` (`gloop_fw.ino`) |

Hold SHIFT to read the audio CPU load: **PK** (worst block in the last second) and **CPU** (average). Cost scales with active grains, not loop length.

## Files

| File | What it does |
|---|---|
| `gloop_fw.ino` | Memory placement, audio callback, setup/loop, sample loading |
| `tape_loop.h` | 16-bit interleaved record buffer, loop region, play head, seam crossfade |
| `grains.h` | Grain pool (12 voices), block-based renderer, spray/spread/envelope, CPU throttle |
| `envelopes.h` | 10 grain window lookup tables + level compensation |
| `fx.h` | Stereo SVF low-pass + ReverbSc (idles when MIX is 0) |
| `controls.h` | Encoders/buttons → params, transport, track buttons, browser |
| `browser.h` | Sample browser state + sample library (stand-in until SD) |
| `params.h/.cpp` | Parameter struct, page map, value formatting |
| `cpu_meter.h` | Audio CPU load from the Cortex-M7 cycle counter |
| `st7789.h/.cpp` | Display driver (Arduino SPI, landscape) |
| `gfx.h` | Greyscale pixel-art drawing, 3×5 and 5×7 fonts |
| `ui.h/.cpp` | LOOP / GRAINS / FX screens and the sample browser |
