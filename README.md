# Horizon

Massive reverbs and otherworldly echoes for the **Korg microKORG 2**, built with the logue SDK. It loads into the synth's **reverb** FX slot.

Horizon is one 8-line feedback delay network that can be a tight room, a cathedral, a pad that swells in after you let go of the keys, a dub echo that self-oscillates, a looper, or a ping-pong delay. Ten **modes** set the network's shape; **Warp** and **Density** morph each one from clean rhythmic echoes to a smooth wash. It's inspired by the *idea* of huge FDN reverb/delays (the Supermassive school), with its own algorithm and modes, tuned for jungle and drum & bass: big pad tails, dub echoes on breaks and stabs, risers, and Reese tails that don't turn to mud.

## Download & install

Go to [**Releases**](https://github.com/TwistedGenetics/Super-Massive/releases/latest) and download `Horizon.mk2unit`.

Needs microKORG 2 firmware **2.0.0 or later** (the version that added logue SDK support).

- **KORG Sound Librarian** (Korg's official app for v2.0): connect the microKORG 2 by USB, add `Horizon.mk2unit` as a user **reverb** effect and send it to the synth.
- **Or USB mass storage:** hold the first option button while powering on, then copy the file into the reverb units folder in `Units/` (next to `Units/Oscs/`, inside a `SlotN` folder) on the drive that appears.

Horizon then appears at the end of the reverb effect list.

**`HZmeter.mk2unit`** (optional, also in each Release) is the same effect with its CPU load shown next to the mode name, e.g. `Hall 23%` (peak over the last half second; 100 % = out of time, crackles). Load it if you hear crackles and want to know how close to the limit you are.

## Controls

Reverb units on the microKORG 2 get 8 parameters: 3 on page 1, 5 on page 2.

| Page | Param | Range | What it does |
|---|---|---|---|
| 1 | **Time** | 1/32 … 1/2D, then 10–1250 ms | Length of the longest delay line: echo spacing and the size of the space. **Turn it fully left for tempo sync**: 1/32, 1/16T, 1/16, 1/8T, 1/16D, 1/8, 1/4T, 1/8D, 1/4, 1/2T, 1/4D, 1/2, 1/2D (T = triplet, D = dotted), following the synth's tempo. Further right it's milliseconds. Moving it, or changing tempo while synced, glides the delays (tape-style pitch bends, no clicks). |
| 1 | **Feedback** | 0–100 % | How much comes back each pass. Higher = longer tails. On Swell and Abyss it also slows the attack. At 100 the sustain modes hold almost forever; **Dub** self-oscillates (the saturator keeps it from running away). |
| 1 | **Warp** | 0–100 % | Spreads the delay lengths. 0 = simple ratios: clean, rhythmic, sometimes resonant/metallic echoes. 100 = irregular ratios: smooth, smeared reverb. Halfway is where the weird stuff lives. |
| 2 | **Density** | 0–100 % | From separate echoes to a dense wash: mixes the delay lines into each other and adds input diffusion. Decay time stays the same however you set it. |
| 2 | **Mod** | 0–100 % | Chorus-like movement on the delay lines. Depth and (gently) rate rise together; depth backs off at faster rates so it stays in tune. 0 = static and dry-sounding. |
| 2 | **Tone** | −100…+100 | Left of centre = darker (high cut, and darker repeats in the filtered modes). Right of centre = thinner (low cut). Centre = open. Push it right to keep tails off your sub. |
| 2 | **Mode** | 10 modes | See below. Switching crossfades, no click. |
| 2 | **Mix** | −100…+100 | Dry/wet. 0 = dry, ±100 = all wet. Negative values flip the wet signal's polarity (as in Korg's own effects). |

### Modes

| Mode | Character |
|---|---|
| **Room** | Fast attack, dense, short. Time sets the room size. |
| **Hall** | Clean, filtered, realistic decay with cross-channel feedback. The safe all-rounder. |
| **Halo** | Fast attack, sparse echoes that diffuse as they decay. Unfiltered, lush, chorusy. |
| **Swell** | Slow attack: the tail blooms in after the note. Attack length follows Time and Feedback. |
| **Abyss** | Very slow attack and a near-endless, darkening tail. Pads and drones. |
| **EchoVerb** | A clear echo at Time (on the beat when synced) that blooms into reverb as Density goes up. |
| **Orbit** | Looper-ish: repeats at exactly Time. Sync it to 1/2 or 1/4 for bar-locked loops. Density fills the gaps with haze; Feedback 100 loops. |
| **Prism** | Each echo travels through all eight delays before it comes back: strange repeating patterns. |
| **Pong** | Ping-pong delay, left/right. Warp offsets the right side (e.g. straight vs dotted). |
| **Dub** | Band-limited, saturating echo. Push Feedback past ~90 for runaway dub spirals (they stay bounded). |

## Starter settings

| Use | Mode | Time | Fb | Warp | Dens | Mod | Tone | Mix |
|---|---|---|---|---|---|---|---|---|
| Amen/break room | Room | 250 | 40 | 70 | 90 | 20 | +30 | 25 |
| Pad cathedral | Hall | 900 | 80 | 85 | 95 | 40 | −20 | 45 |
| Reese tail (no mud) | Halo | 500 | 70 | 55 | 45 | 50 | +40 | 35 |
| Pad that blooms in | Swell | 700 | 85 | 70 | 70 | 40 | −15 | 50 |
| Endless drone | Abyss | 1100 | 92 | 80 | 85 | 45 | −25 | 60 |
| Dub stab echo | Dub | 1/8D | 75 | 0 | 10 | 35 | −20 | 40 |
| Ping-pong on snares | Pong | 1/8 | 55 | 0 | 15 | 15 | −10 | 30 |
| Riser / freeze (bar-locked) | Orbit | 1/2 | 100 | 50 | 60 | 25 | 0 | 60 |

**Tempo sync notes**
- Synced Time follows the microKORG 2's tempo (internal or external clock); tempo changes glide the delays.
- The delay lines are 1.25 s long. A division longer than that at the current tempo plays at half length, so it stays on the beat (e.g. 1/2D below 144 bpm).
- In the echo modes (EchoVerb, Orbit, Prism, Pong, Dub), low Density keeps every repeat exactly on the beat; more Density smears each hit into the space.
- Patches saved with v1.0 keep their Time in ms.

## Test checklist (by ear on the hardware)

- [ ] Loads into the reverb slot, shows **Horizon**, and the 8 params show the right names and ranges.
- [ ] **Every mode** at its starter setting: no crackles, no dropouts. Load **HZmeter** and note the % next to the mode name (the worst case is Abyss or Hall with Mod high).
- [ ] **Time sweep** while holding a chord: smooth pitch glide, no clicks.
- [ ] **Tempo sync**: Time fully left shows 1/32 … 1/2D; Dub at 1/8D with the arpeggiator or a drum loop lands on the dotted eighths. Change tempo while it rings: the echoes follow (with a short glide).
- [ ] **Mode change** while a tail is ringing: quick crossfade, no click.
- [ ] **Feedback 100** in Orbit and Abyss: the tail holds. In Dub: self-oscillates but stays under control.
- [ ] **Everything at max**: loud, but no runaway, no stuck noise.
- [ ] After switching to another reverb and back: no burst of old audio.

## CPU and memory

Reverb units get **1 MB** of delay memory (`REVFX_MEMORY_SIZE_BYTES`) and **8 params**. Horizon uses exactly 1 MB: eight 1.28 s delay lines plus eight small diffuser buffers, stored as 16-bit integers (like Korg's own `breveR`). That's why Time tops out at 1250 ms; long tails come from feedback, not line length. See [NOTES.md](NOTES.md) for the verified SDK limits and design trade-offs.

- **v1.0.1 is about 2.5–3× lighter than v1.0.0.** v1.0.0 could crackle when paired with a heavy user oscillator. The sample loop now runs the 8 delay lines as NEON vectors, with no divides and no library calls (details in [NOTES.md](NOTES.md)).
- **Desktop** (test harness): ~655 instructions per sample (v1.0.0: ~1,740).
- **microKORG 2** (Cortex-A7): not measured on hardware yet; use **HZmeter**. The oscillator and the effects share one CPU, so a heavy user oscillator with many voices plus Horizon can still run out of time. If it crackles, check HZmeter's % and the oscillator's own meter (if it has one), and try fewer voices.
- Unit size: ~11 KB loaded (limit 24 KB); CI fails the build if it ever goes over.

## Building

GitHub Actions builds the unit with Korg's own toolchain and runs the DSP tests on every push. Each run's files are under the **Actions** tab.

Releases are automatic: when `.version` in `microkorg2/header.c` changes (e.g. `0x00010001U` = 1.0.1) and is pushed to main, Actions tags it and publishes a new Release with `Horizon.mk2unit` and `HZmeter.mk2unit`. Pushes that keep the same version refresh that Release's files.

To build locally (no Docker needed; Linux only, as Korg only publishes the microKORG 2 toolchain for Linux):

```sh
./build.sh          # Horizon + HZmeter into dist/
```

This downloads the toolchain and the logue SDK into `.deps/` on first run.

Desktop DSP tests (any OS with a C++ compiler):

```sh
./test/run.sh
```

They check every mode for NaN/inf, runaway feedback at extreme settings, DC build-up, clicks on mode changes, random automation, and that Feedback 100 holds where it should. They also write impulse responses and demo renders for every mode to `test/out/`. To hear Horizon on your own audio:

```sh
./test/harness in.wav out.wav 3 700 0.85 0.7 0.7 0.4 -0.15 0.5
#                             mode time fb  warp dens mod tone  mix   (mode 0-9: Room..Dub)
```

## Layout

```
core/horizon_engine.h    the DSP engine (no SDK dependencies)
core/horizon_modes.h     the ten mode recipes
microkorg2/              microKORG 2 unit: header.c (params), unit.cc (glue), Korg Makefile
tools/mk2-meter/         HZmeter: same unit + CPU load in the Mode display
test/                    desktop harness and checks
build.sh                 local build with Korg's toolchain
NOTES.md                 verified platform limits and design notes
```

## Roadmap

- NTS-1 mkII version (the engine is platform-independent; it needs a thin wrapper and a check of that synth's memory budget).

## Notes

Developer ID `0x5477476E` ('TwGn'), unit ID 5. Horizon is an original design and isn't affiliated with Valhalla DSP or Korg.
