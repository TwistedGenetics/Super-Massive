# Design notes

Verified against `korginc/logue-sdk` at commit `e78b16c` (2026-10-20), `platform/microkorg2`.
Firmware ≥ 2.0.0 is required for units built with SDK 2.1.0 (platform README).

## Verified platform limits

| Item | Value | Source |
|---|---|---|
| CPU | ARM Cortex-A7 (i.MX6ULZ), NEON/VFPv4, hard float | `microkorg2/README.md` |
| Sample rate | 48 000 Hz (unit must reject anything else) | `dummy-revfx/reverb.h` `Init()` |
| I/O | stereo in, stereo out, interleaved `float` L/R | `dummy-revfx/reverb.h` `Init()`, `Process()` |
| FX slot used | **revfx** (reverb) — 32 user slots | `microkorg2/README.md` module table |
| Unit code size | ~24 KB max, 24 KB RAM load | `microkorg2/README.md` |
| External (SDRAM) memory | **1 MB** (`REVFX_MEMORY_SIZE_BYTES 0x100000`) | `common/unit_revfx.h:53` |
| Max params | **8** (`UNIT_REVFX_MAX_PARAM_COUNT (8)`) | `common/unit_revfx.h:61` |
| Param layout | page 1 = params 0–2, page 2 = params 3–7 | all Korg FX `header.c` files |
| Unit / param name length | 8 characters | `common/attributes.h:14,16` |
| String params | `k_unit_param_type_strings` + `unit_get_param_str_value()` | `dummy-revfx` |
| Tempo | `unit_set_tempo()`, 16.16 fixed BPM | `dummy-revfx/unit.cc` |

delfx has the same 1 MB / 8 params, so revfx was chosen: it's the slot a long-tail
reverb belongs in, and it leaves the delay slot free for a normal delay in front.

## Consequences for the design

**Memory.** 1 MB is ~3× less than an 8-line network with 2-second float lines would need.
Delay lines are stored as **int16** (as Korg's own `breveR` does), with ±2.0 float range
mapped to int16 for loop headroom. Layout:

| Block | Samples (int16) | Time at 48 kHz |
|---|---|---|
| 8 FDN lines × 61 440 | 491 520 | 1.28 s each |
| 8 input diffuser allpasses × 4 096 | 32 768 | 85 ms each |
| **Total** | **524 288 = exactly 1 MB** | |

So TIME tops out at **1250 ms** (a little headroom is kept for modulation and interpolation).
Long "near-endless" tails come from feedback, not from line length, so this costs echo
spacing, not decay length.

**Tempo sync (v1.1.0).** No free param slot, so sync lives on the Time knob:
`-13..-1` = divisions 1/32 … 1/2D (string-type param, like Korg's MorphEQ "Mid Fc"),
`0..1250` = ms (unchanged from v1.0, so saved patches keep their times). Tempo comes from
`unit_set_tempo()` (16.16 BPM); changes under 0.05 BPM are ignored so MIDI-clock jitter
doesn't make the delays wobble. Divisions longer than the 1.25 s lines are halved until
they fit (stays on the beat). The maths is in `core/horizon_sync.h` and tested in the
harness.

The input diffusers also delay the signal (~30 ms × the mode's diffuser scale), so in
v1.0 every echo landed that much after the beat. Echo modes now blend the undiffused
input back in by Density (`onBeat` in `horizon_modes.h`): a synced 1/4 at 174 bpm repeats
at 344.9 ms (target 344.8). Reverb modes are unchanged (measured T60/attack identical).

**Params.** 8 instead of the 10 planned. Merges:
- MOD RATE + MOD DEPTH → one **Mod** knob (depth rises with the knob; rate rises gently
  with it, scaled per mode; depth is reduced at higher rates so it stays in tune).
- HIGH CUT + LOW CUT → one bipolar **Tone** knob (left = darker, right = thinner; centre open).
- WIDTH dropped as a knob: each mode sets its own stereo layout.

**int16 storage.** ~96 dB per-pass quantisation floor; inaudible in practice, and the
loop saturator + DC blocker keep values bounded.

## Verification so far

- Desktop harness (`test/run.sh`): all checks pass. That covers 10 modes × extreme corners
  with 20 s of near-full-scale noise (no NaN/inf, peak < 1.7, no DC), 60 s of random
  automation and mode changes, a mode-switch click check, and FEEDBACK 100 sustain
  (Orbit −1.5 dB over 9 s, Dub self-oscillates and stays bounded).
- Measured T60 at the README starter settings: Room ~1 s, Hall ~6 s, Halo/Swell/Prism
  ~8–9 s, EchoVerb ~4 s, Dub ~3 s, Pong ~0.5 s; Abyss and Orbit (by design) longer
  than the 12 s test window. Attack (envelope peak): Swell ~0.9 s, Abyss ~1.9 s.
- ARMv7 test build with clang (`-mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard`,
  SDK headers): compiles warning-free, ~17 KB total (limit 24 KB), imports only
  `memcpy memset expf powf sinf cosf sqrtf fabsf` (+ `clock_gettime snprintf` for
  HZmeter), the same kind of libc/libm use as Rollers. The release build uses Korg's
  gcc in CI.
- **Not yet done: running on a microKORG 2.** See the README test checklist.

## CPU

**v1.0.0 was too heavy.** On the synth it crackled when paired with a demanding user
oscillator (stock oscillators were fine). A profile showed ~1,740 instructions per sample
(desktop count, `-Os`), plus 8 floating-point divides per sample (the rational
soft-clipper; VDIV is ~15 cycles on the A7 and not pipelined) — roughly 5x the original
estimate. The cost was loop overhead (`-Os` doesn't unroll the 8-line loops), per-sample
parameter smoothing, the butterfly network, the divides and the int16 clamps.

**v1.0.1 rewrite of the sample loop** (same algorithm, same decay times/attacks):
- The 8 lines are two 4-wide GCC vectors (`vector_size(16)`, as in Rollers' lean engine):
  NEON on the microKORG 2, SSE on a desktop, so the tested code is the shipped code.
- Permutation + 3-stage rotation are folded into one 8×8 matrix, rebuilt only when
  DENSITY moves (per block). Checked against the butterfly version: max error 1.8e-7.
- Division-free cubic saturator (Rollers' `softclip`), which bounds the loop at ±1.8, so
  line writes need no clamp.
- Gains, density, diffusion, mix and the mode crossfade update once per block
  (mix and fade are linearly ramped per sample, so no zipper).
- Read-index math 4-wide; input diffusers run L and R side by side (2-wide).

| | v1.0.0 | v1.0.1 |
|---|---|---|
| Desktop instructions/sample (`-Os`, valgrind) | ~1,740 | ~655 |
| **Synth build** (Korg's gcc 6.5, `-Os`), per-sample loop | 828 instr. wrapped around 21 inner loops (mostly ×8 per sample) | 735 instr. straight through, 4 tiny ×4 loops |
| …divide instructions in it | 4 (inside the ×8 loops) | 0 |
| …library calls in it | several | none |
| …NEON instructions | 13 | 378 |

The synth-build row comes from `tools/code_stats.py`, which CI runs on Korg's
disassembly listing on every build (see the "Sample loop" note on each Actions run).
Korg's toolchain is **gcc 6.5** (crosstool-NG 1.24.0), which lacks
`__builtin_convertvector`, so float↔int conversions use NEON intrinsics directly.
`-O2` was tried: the unit grew to 21.8 KB with no verifiable gain, so the build stays `-Os`.

Expect roughly 2.5–3× less CPU on the synth. Output vs v1.0.0 matches to ~40–50 dB
across modes (limited by int16 rounding noise and the different saturator curve); T60
and attack measurements are unchanged. **Not yet measured on hardware: use HZmeter.**
If it's still too heavy with a big oscillator patch, the next levers are fewer diffuser
stages and fewer lines (a "lite" build).

## Algorithm summary

- Stereo-in → 4-stage allpass diffuser per channel (amount from DENSITY × mode).
- 8 delay lines. Lengths = TIME × ratio(i), where WARP morphs each mode's ratio table from
  a simple/harmonic set (clean rhythmic echoes) to an incommensurate set (smooth reverb).
  Length changes glide (one-pole on read position) → tape-style pitch bends, no clicks.
- Feedback matrix = a mode-specific **permutation** (echoes circulate intact, giving
  patterns/ping-pong/cascades) followed by three butterfly stages of **Givens rotations**
  by θ (since v1.0.1 folded into one precomputed 8×8 matrix). θ = 0 leaves the permutation alone; θ = π/4 is a full 8×8 Hadamard mix.
  DENSITY sets θ. Every stage is orthogonal, so the matrix is energy-preserving at
  every DENSITY and density never changes the decay time.
  (First version linearly blended permutation and Hadamard. That's stable but loses
  up to 3 dB per pass at mid density, so FEEDBACK 100 couldn't sustain. Replaced.)
- Per-line gain scaled by line length (`fb^(len/longest)`) so all lines decay at the
  same rate. FEEDBACK maps to per-pass gain as `fbMax · (1 − (1−k)^1.5)`.
- int16 writes **truncate** toward zero. Rounding was tried: it lets tiny tails get
  stuck as ±1 LSB limit cycles that never reach −60 dB.
- Soft saturation in the loop (division-free cubic since v1.0.1): FEEDBACK at 100 holds/sustains without blowing up.
- "Bloom" modes inject into the first lines of a cascade and listen mostly to the last,
  so sound builds up over several passes → slow swells; the swell length tracks TIME and
  FEEDBACK.
- Output: per-mode L/R tap weights, then Tone filters, then bipolar Mix
  (negative = phase-inverted wet, as in Korg's examples).
