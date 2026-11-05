# GLOOP — 2-track granular looper firmware

Daisy Seed firmware, built with the Arduino IDE. Two tracks, each: tape looper → grain cloud → low-pass filter, mixed into one shared reverb. 240×280 ST7789 TFT (landscape) and a sample browser.

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
| LOOP | Start · SHIFT: **BPM** | Length (8th notes) · SHIFT: **free length** | Pitch (±24 st) | Crossfade |
| GRAINS | Grains (1 = plain loop, 2–12 = grain cloud) · SHIFT: Size | Spray | Spread | Envelope |
| FX | Cutoff | Resonance | Reverb size | Reverb mix |

Everything acts on the **selected track** (T1 or T2, shown in the header chip). The footer's bottom-left shows what the other track is doing.

- **T1 / T2:** first press selects that track; pressed again on LOOP opens the sample browser for it (turn any encoder to move, push to open / load).
- **REC arms** the track: it waits for input and starts recording when the input crosses about -34 dBFS, keeping the 10 ms before the trigger so the first attack isn't lost. The screen shows ARM and a live input meter with the trigger level marked. REC again cancels.
- **SHIFT + REC** records immediately, without waiting.
- While recording, REC or PLAY closes the loop and plays it. A take stops by itself at **30 s** (`kMaxLoopSeconds`).
- **PLAY is global:** it stops both tracks if either is playing, otherwise starts both from their loop start points, in sync. If the selected track is recording, PLAY finishes the take instead.
- **When a take finishes** (REC, PLAY or the 30 s limit): if the other track is already playing, the new loop **joins in** straight away; if nothing was playing, both tracks start together from their loop start.
- **BPM:** SHIFT + START sets the tempo (40–240, shared by both tracks), shown next to LOOP in the header.
- **LENGTH** is counted in 8th notes at that BPM (shown as e.g. `6/8`, `16/8`), so loops stay in time when the BPM changes. A new take is fitted to the most whole 8ths it holds.
- **SHIFT + LENGTH** frees the length from the grid (shown in seconds). Turning LENGTH again without SHIFT snaps it back onto the nearest 8th.
- **GRAINS 1** plays the plain loop (a loop is like one grain the length of the loop); the grain cloud takes over from 2. The grain cloud's level is matched to the plain loop's automatically, so switching modes or changing SPRAY / ENV / SIZE doesn't change the volume.
- SHIFT + turn = fine adjust (or the alternate parameter). Push an encoder to reset its parameter.
- Reverb **SIZE** is shared by both tracks (one reverb); each track's **MIX** is its send into it. Every other parameter is per track.
- Hold SHIFT to show CPU load (peak / average) in the footer.

## Performance limits

| Limit | Value | Where |
|---|---|---|
| Grain voices per track | 12 (24 across both tracks) | `GrainCloud::kMaxGrains` (`grains.h`) |
| Loop length per track | 30 s (16-bit stereo, 5.8 MB) | `kMaxLoopSeconds` (`gloop_fw.ino`) |
| CPU safety valve | stops new grains above 80% average / 90% peak, resumes below 70% | `gloop_fw.ino` |
| Output | soft limiter from 0.8 of full scale | `OutputLimit()` (`gloop_fw.ino`) |

Hold SHIFT to read the audio CPU load: **PK** (worst block in the last second) and **CPU** (average). Cost scales with active grains, not loop length.

## Files

| File | What it does |
|---|---|
| `gloop_fw.ino` | The two tracks, audio callback and mix, setup/loop, sample loading |
| `tape_loop.h` | 16-bit interleaved record buffer, record-arm with pre-roll, loop region, play head, seam crossfade |
| `grains.h` | Grain pool (12 voices), block-based renderer, spray/spread/envelope, CPU throttle |
| `envelopes.h` | 10 grain window lookup tables + level compensation |
| `fx.h` | Per-track stereo SVF low-pass + one shared ReverbSc (idles when every MIX is 0) |
| `controls.h` | Encoders/buttons → params, transport, track buttons, browser |
| `browser.h` | Sample browser state + sample library (stand-in until SD) |
| `params.h/.cpp` | Parameter struct, page map, value formatting |
| `cpu_meter.h` | Audio CPU load from the Cortex-M7 cycle counter |
| `st7789.h/.cpp` | Display driver (Arduino SPI, landscape) |
| `gfx.h` | Greyscale pixel-art drawing, 3×5 and 5×7 fonts |
| `ui.h/.cpp` | LOOP / GRAINS / FX screens and the sample browser |
