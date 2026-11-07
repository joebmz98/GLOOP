# GLOOP — 2-track granular looper firmware

Daisy Seed firmware, built with the Arduino IDE. Three tracks, each: tape looper → grain cloud → filter (LP/HP/BP/COMB) → chorus → resonator, mixed into one shared reverb. 240×280 ST7789 TFT (landscape) and a sample browser.

## Arduino IDE setup (once)

1. **File → Preferences → Additional boards manager URLs**:
   `https://github.com/stm32duino/BoardManagerFiles/raw/main/package_stmicroelectronics_index.json`
2. **Boards Manager** → install **STM32 MCU based boards** (STMicroelectronics). Tested with **2.12.0** (and 3.0.0).
3. **Library Manager** → install **DaisyDuino**.
4. Install **STM32CubeProgrammer** (for DFU upload).

## Board settings (Tools menu)

| Setting | Value |
|---|---|
| Board | STM32 MCU based boards → Generic STM32H7 Series |
| Board part number | Daisy Seed |
| Upload method | STM32CubeProgrammer (DFU) |
| USB support | None (CDC adds ~12 KB of flash) |
| Optimize | **Smallest (-Os default)** (86% of flash) |

GLOOP's own audio code (grains, tape, filter drive, chorus, resonator, reverb bus) switches itself to `-O2` with `#pragma GCC optimize`, so "Smallest" keeps it fast. "Faster (-O2) with LTO" **no longer fits** the 128 KB flash (MIDI and three tracks pushed it over); "Faster/Fastest" without LTO doesn't either. Next step when flash runs out: run the program from the Seed's 8 MB QSPI flash.

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
| T1 / T2 / T3 | D0 (pin 1) / D1 (pin 2) / **D28 (pin 35)** |
| MIDI in (optocoupler output) | **D14 (pin 15)**, USART1 RX, pulled up to **3V3** (not 5 V) |
| REC / PLAY | D2 (pin 3) / D3 (pin 4) |
| LOOP / GRAINS / FX | D4 / D5 / D6 (pins 5–7) |
| SHIFT | D12 (pin 13) |
| LFO | **D29 (pin 36)** |
| Audio IN L / R | pin 16 / 17 (TRS tip / ring) |
| Audio OUT L / R | pin 18 / 19 (TRS tip / ring) |
| Jack sleeves | AGND (pin 20) |

Buttons and encoder commons go to DGND (pin 40); internal pull-ups are used.
D1–D6 are also the Seed's SD card pins: when SD loading is added, the buttons on them move to a shift register.

## Controls

| Page | Enc 1 | Enc 2 | Enc 3 | Enc 4 |
|---|---|---|---|---|
| LOOP | Start · SHIFT: **BPM** | Length (8th notes) · SHIFT: **free length** | Pitch (±24 st) | Crossfade (0–1 s) |
| GRAINS | Grains (0 = loop only, 1–12 = grains on top of the loop) · alt: **Size** | Spray | Spread · alt: **Grain pitch** (±24 st) | Mix (loop ↔ grains, default 50%) · alt: **Envelope** |
| FX ▪ FILTER | Cutoff | Resonance | Type (LP / HP / BP / COMB) | Drive |
| FX ▪ CHORUS | Rate | Depth | Width | Mix |
| FX ▪ RESONATOR | Freq (note) | Structure · hold + turn: **Bright** | Decay | Mix |
| FX ▪ REVERB | Size | Tone | Pre-delay | Mix |
| **Hold FX** (any page) | Filter cutoff | Chorus mix | Resonator mix | Reverb mix |
| LFO | Shape | Rate (0.05–10 Hz) | Depth (−100..+100%) | Destination |
| LFO ▪ ENV (LFO again) | Source (T1 / T2 / T3 / IN) | Release (20 ms–2 s) | Depth (−100..+100%) | Destination |

Everything acts on the **selected track** (T1, T2 or T3, shown in the header chip). The mode (**LOOP / GRAINS / FX**) is shown in the middle of the footer, and the page within it at the bottom right: GRAINS `1/2` / `2/2` (alt layer), FX `1/4`..`4/4` (FILTER, CHORUS, RESONATOR, REVERB).

The panel has rounded corners, so the header and footer keep 14 px (logical, 28 physical) clear of the left and right edges (`kCornerL` / `kCornerR` in `ui.cpp`).

- **T1 / T2 / T3:** first press selects that track; pressed again on LOOP opens the sample browser for it (turn any encoder to move, push to open / load).
- **REC arms** the track: it waits for input and starts recording when the input crosses about -34 dBFS, keeping the 10 ms before the trigger so the first attack isn't lost. The screen shows ARM and a live input meter with the trigger level marked. REC again cancels.
- **In time with a playing loop:** if another track is playing when the take starts (REC arm, SHIFT + REC or a count-in), the take locks to that loop's 8th-note grid (**IN TIME** on screen). It still starts the moment you play, but the new loop's start is placed on the nearest 8th of the playing loop (with silence in front if you came in a little late), and it joins in exactly in phase, so every note plays back where you played it. Pressing REC (or PLAY) to finish rounds the take to the **nearest whole bar** (or the playing loop's length if that isn't whole bars): if that bar line has already passed it stops and trims there, otherwise it keeps recording and stops exactly on the bar (**FINISHING THE BAR**).
- **Count-in:** hold **REC** and press **PLAY** → steady ARM ("PLAY: 4-BEAT COUNT-IN"). Press **PLAY** again → the metronome counts **4 beats** (the number shows big, always clicks even if the metronome is off) and recording starts **on the beat**. With a loop playing, the count ends on that loop's next bar line at least 4 beats away, so you always hear a full 1-2-3-4. REC or PLAY during the count cancels it.
- **SHIFT + REC** records immediately, without waiting.
- **Red is the only colour on the screen:** the ARM / REC status, the record dots and the track strip while recording.
- **Metronome:** while a track is armed or recording it clicks every beat at the BPM (4/4, higher click on beat 1), with four beat dots in the header. If another track is playing, beat 1 is that loop's start, so a new take stays in time with it; otherwise it starts when you arm and restarts on beat 1 when recording begins. The click goes to the output only, never into a recording. **SHIFT + PLAY** turns it on/off (dots show hollow when off).
- While recording, REC or PLAY closes the loop and plays it. A take stops by itself at **30 s** (`kMaxLoopSeconds`).
- **PLAY is global:** it stops every track if any is playing, otherwise starts them all from their loop start points, in sync. If the selected track is recording, PLAY finishes the take instead.
- **When a take finishes** (REC, PLAY or the 30 s limit): if another track is already playing, the new loop **joins in** straight away; if nothing was playing, every track with a loop starts together from its loop start.
- **BPM:** SHIFT + START sets the tempo (40–240, shared by all tracks), shown next to LOOP in the header.
- **MIDI clock in** (D14): while a MIDI clock is coming in, the BPM follows it (whole BPM; a new tempo is taken once it has held for a beat, so loop lengths don't re-cut through a tempo glide). The header shows e.g. `120 MIDI` instead of `120 BPM`, and a small dot right of the track chip flickers whenever any MIDI bytes arrive (so you can tell the input circuit works even if the source isn't sending clock; many only send it while playing). Note: the port is opened on USART1's own alternate pins (`PB_7_ALT1`), because the core's default for D14 is a different UART (LPUART1). The UART interrupt is set above the audio interrupt (DaisyDuino's audio DMA otherwise blocks it and MIDI bytes overrun).
- **MIDI diagnostics:** hold **SHIFT**: the middle of the footer shows e.g. `MIDI 48 SIG` = bytes per second received, and what the D14 pin itself is doing (read straight from the pin, independent of the UART): **SIG** = MIDI is reaching the pin, **IDLE** = the pin never goes low (nothing arriving: wiring, TRS type A/B, or the source isn't sending), **LOW!** = stuck low (opto supply or pull-up). SIG with 0 bytes = signal arrives but isn't decoded. If the clock stops for half a second, GLOOP keeps that tempo and SHIFT + START works again.
- **LENGTH** is counted in 8th notes at that BPM (shown as e.g. `6/8`, `16/8`), so loops stay in time when the BPM changes. A new take is fitted to the most whole 8ths it holds.
- **SHIFT + LENGTH** frees the length from the grid (shown in seconds). Turning LENGTH again without SHIFT snaps it back onto the nearest 8th.
- **GRAINS 0** is just the loop; **1–12** add that many grains alongside the loop. The cloud's level is matched to the loop automatically, so changing GRAINS / SPRAY / ENV / SIZE doesn't jump the volume.
- **MIX** (GRAINS encoder 4) balances the loop against the grains: 0% = loop only, 100% = grains only, **50% (default)** = both at -3 dB (an equal-power blend), which keeps the total about as loud as the loop on its own.
- **Press GRAINS again** while on the GRAINS page for the **alt layer**: encoder 1 = grain **SIZE**, encoder 3 = grain **PITCH** (±24 semitones, the grains only; the loop keeps its own PITCH), encoder 4 = grain **ENVELOPE** (window shape). Their labels show inverted; press GRAINS again to go back.
- **FX pages:** one effect per page. **FX** goes to the FX pages (at the one you used last); pressed again it steps **FILTER → CHORUS → RESONATOR → REVERB**, and **SHIFT + FX** steps back. The footer shows which. Per track the order is **Filter → Chorus → Resonator → Reverb**, and **MIX is always encoder 4**.
- **Hold FX and turn** = the **FX mixer**, from any page, without leaving it: encoder 1 = filter cutoff, 2 = chorus mix, 3 = resonator mix, 4 = reverb mix. The header shows **FX MIX** and the readouts switch to the mixer while FX is held. (FX acts when you *release* it, so holding it to use the mixer doesn't change page.)
- **Hold an encoder in and turn it** for its second parameter, where it has one: on RESONATOR, encoder 2 = **BRIGHT** (also shown top right of the picture). Elsewhere hold + turn is the same as turning.
- **Value popup:** while you turn an encoder, its name and value show large in the middle of the screen for about a second. Useful for the mixer and hold + turn, whose values aren't on the readouts.
- **FILTER:** TYPE switches **LP / HP / BP / COMB** with a 5 ms crossfade. If the filter was wide open it stays open for the new type (e.g. HP moves CUTOFF to the bottom); SHIFT + push on CUTOFF resets it to the open position for the current type. **DRIVE** pushes the sound through a soft clip into the filter, with make-up gain so it gets grittier rather than louder (0 = clean). The picture: the response curve across the screen, shimmering with heat when driven, plus a small sine top right that squashes as DRIVE goes up.
- **COMB:** a tuned feedback comb. CUTOFF is its pitch (50 Hz..12 kHz; it glides rather than clicks when swept) and RESO is the feedback: at 0 it's a static flanger-like notch comb, turned up it rings at that pitch and its harmonics, with a damping low-pass and a soft clip in the loop so it darkens as it rings and can't run away. Its CUTOFF reset (SHIFT + push) is the middle (~850 Hz), and switching away from COMB leaves CUTOFF where it is. DRIVE still works in front of it. The picture shows the comb's teeth. Costs ~8 KB of RAM per track (a 1024-sample stereo delay line); the SVF is skipped while COMB is selected.
- **CHORUS:** RATE 0.05–10 Hz, DEPTH ±0.5–8 ms, WIDTH from mono (L and R sweep together) to widest (half a cycle apart), MIX 0 = off. The picture is a little choir: one silent singer at MIX 0 and up to eight singing at 100%; they stand further apart with WIDTH, sway more with DEPTH and sing in time with RATE.
- **RESONATOR** (DaisySP's Rings-style modal resonator): FREQ is the note it rings at (C1–C7, e.g. `A2`); STRUCT 0–25% squeezed overtones, 25–30% harmonic (string / tube), above that stretched and inharmonic (bar, bell); DECAY from a dull knock to a ~30 s drone (DaisySP's "damping"; higher = rings longer); BRIGHT (hold encoder 2 + turn) from wood to glass; **MIX 0% = off** (the default, and then it costs no CPU). Its level is matched to the input automatically. The picture is a wine glass played with a wet finger: FREQ is the water level (more water = lower note), STRUCT morphs the glass from a narrow flute to a wide bowl, BRIGHT makes it shine, DECAY sets how many sound waves ripple out; the finger only circles the rim while MIX is up.
- **REVERB:** SIZE (feedback), TONE (damping, 1–18 kHz: dark → bright), **PRE-DLY** (0–250 ms before the reverb starts, so the dry sound stays clear) and MIX (this track's send). SIZE, TONE and PRE-DLY are shared by all tracks (one reverb); MIX is per track. The picture is a cathedral nave: SIZE adds arches, TONE lets you see further down it, and the echo rings leave the singer after a gap set by PRE-DLY.
- **LFO page** (LFO button, D29): one modulation source per track, which moves one parameter. **LFO** mode: SHAPE (SINE, TRI, SQUARE, SAW+, SAW-, S&H, DRIFT = smooth random), RATE 0.05–10 Hz, DEPTH, DEST. The LFO restarts when its track starts playing, so it lines up with the loop. **Press LFO again** to switch that track to **ENV**, an envelope follower: SOURCE is the track it listens to (T1, T2, T3, or the audio IN), measured after that track's FX and before its mute, so a muted track can still drive it; attack is ~3 ms and RELEASE sets how fast it falls; levels from −48 to −6 dBFS map to 0–100%. DEPTH and DEST are shared by the two modes. The footer shows LFO `1/2` or ENV `2/2`.
- **Destinations:** PITCH (±12 st at full depth), VOLUME, GRAINS (±12), SPRAY, SPREAD, G.MIX, G.SIZE (±2 octaves), CUTOFF, RESO, DRIVE, CH.RATE, CH.DPTH, CH.WDTH, CH.MIX, RES.FRQ (±24 st), STRUCT, DECAY, BRIGHT, RES.MIX, REV.SZ, TONE, PRE-DLY, REV.MIX (filter TYPE isn't a destination: it's a switch). The LFO swings either side of where the knob is; the ENV pushes up from it (negative DEPTH: down). The knobs and readouts don't move; only the sound does. **VOLUME**: an LFO is a tremolo (negative DEPTH flips it); an ENV with positive DEPTH is a gate (quiet when the source is quiet), with negative DEPTH a **ducker / sidechain** (quiet when the source is loud). REV.SZ / TONE / PRE-DLY are shared, so every track's modulation of them adds up.
- **LFO picture:** a night at sea, in the same pixel-art style as the other pages. **LFO**: a little sailboat riding waves that *are* the LFO: the surface is the SHAPE (sine swell, triangle chop, square steps, saw ramps), as tall as DEPTH (flat calm at 0, upside down when negative), rolling past at RATE; the boat sits on the current value and tips with the slope. S&H and DRIFT are random, so the water behind the boat shows where it has been and fog hides what's coming. **ENV**: a lighthouse on a rock whose beam reaches further and brightens with the followed track's level, fading at RELEASE; the SOURCE chip sits below it. DEST and DEPTH are shown top right.
- **SHIFT + T1 / T2 / T3 mutes** that track (press again to unmute). It keeps playing silently, so it stays in sync; the fade is about 4 ms so there's no click. A muted track's header chip turns dark, and in the footer a muted track's name is struck through. Its reverb tail still rings out.
- SHIFT + turn = fine adjust (or the alternate parameter where there is one). **SHIFT + push** resets that parameter to its default (a push on its own no longer resets, so hold + turn is safe).
- Reverb **SIZE / TONE / PRE-DLY** and the **BPM** are shared by all tracks; every other parameter is per track.
- Hold SHIFT to show CPU load (peak / average) and the screen frame rate (FPS) in the footer.

## Display speed

The screen redraws at 30 fps. A full frame is 134 KB, which takes 43 ms over SPI at 25 MHz, so `St7789::Flush()` compares each frame with what the panel already shows and only sends the changed part of each 8-row band, typically 2–8 KB (1–3 ms). A full frame only goes out when you change page. All of this runs in `loop()`; the audio is an interrupt, so the display can never glitch the sound.

SPI1's clock divides by powers of 2 (50 MHz → 25 or 12.5 MHz). For 40 MHz, set `kFastSpi = true` in `st7789.h`, but only with short wires or on a PCB; if the picture shows speckles or wrong colours, set it back.

## Performance limits

| Limit | Value | Where |
|---|---|---|
| Grain voices | 12 per track, but a **shared pool of 16** across all tracks: if the playing tracks ask for more, each gets a fair share (e.g. 12 + 12 → 8 + 8, shown as `8/12` on GRAINS) | `kGrainPool` (`gloop_fw.ino`) |
| Loop length per track | 30 s (16-bit stereo, 5.8 MB; 17.3 MB of the 64 MB SDRAM for three) | `kMaxLoopSeconds` (`gloop_fw.ino`) |
| Load governor | above 85% on any block (or 80% average) the grain pool and the resonators' modes shrink within ~15 ms (down to a quarter), and grow back over ~1.5 s once the average is under 65% | `gloop_fw.ino` |
| CPU safety valve | also stops new grains above 80% avg / 90% peak, resumes below 70% | `gloop_fw.ino` |
| CPU clock | **480 MHz** (the board default is 400) | `clock.cpp` (`kBoost480`) |
| Per-track FX | run a block (48 samples) at a time: filter → chorus → resonator. The filter is GLOOP's own inline copy of DaisySP's SVF maths (no library call per sample); the resonator runs four modes side by side (~2.5× cheaper on the M7) | `fx.h` |
| Idle tracks | a stopped track's FX are skipped once they've been silent (below −120 dBFS) for 64 ms, so a stopped track costs almost nothing | `kQuietBlocks` (`gloop_fw.ino`) |
| Output | soft limiter from 0.8 of full scale | `OutputLimit()` (`gloop_fw.ino`) |

Hold SHIFT to read the audio CPU load: **PK** (worst block in the last second) and **CPU** (average). CPU is shown inverted while the governor or safety valve is holding things back. Cost scales with active grains, resonators and the reverb, not loop length.

## Files

| File | What it does |
|---|---|
| `gloop_fw.ino` | The two tracks, audio callback and mix, setup/loop, sample loading |
| `metronome.h` | Click track while armed/recording/counting in, locked to a playing loop or to the take start |
| `record_sync.h` | Grid-lock and count-in timing maths (recording in time with a playing loop) |
| `midi_clock.h` | MIDI clock in (D14) → BPM |
| `tape_loop.h` | 16-bit interleaved record buffer, record-arm with pre-roll, loop region, play head, seam crossfade |
| `grains.h` | Grain pool (12 voices), block-based renderer, spray/spread/envelope, CPU throttle |
| `envelopes.h` | 10 grain window lookup tables + level compensation |
| `fx.h` | Per-track filter (LP/HP/BP/COMB + drive), stereo chorus and modal resonator + one shared ReverbSc with pre-delay (idles when every MIX is 0) |
| `modulation.h` | Per-track LFO / envelope follower and how each destination is modulated |
| `controls.h` | Encoders/buttons → params, transport, track buttons, browser |
| `browser.h` | Sample browser state + sample library (stand-in until SD) |
| `params.h/.cpp` | Parameter struct, page map, value formatting |
| `cpu_meter.h` | Audio CPU load from the Cortex-M7 cycle counter |
| `clock.cpp` | Runs the CPU at 480 MHz (replaces the board's 400 MHz clock setup; `kBoost480 = false` to undo) |
| `st7789.h/.cpp` | Display driver (Arduino SPI at 25 MHz, landscape); sends only the parts of the screen that changed |
| `gfx.h` | Greyscale pixel-art drawing, 3×5 and 5×7 fonts |
| `ui.h/.cpp` | LOOP / GRAINS / FX screens and the sample browser |
