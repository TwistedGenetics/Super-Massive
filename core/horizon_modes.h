#pragma once
/*
 * Horizon — mode definitions.
 *
 * Each mode is a recipe for the same 8-line feedback delay network:
 *   clean[]   delay ratios (x TIME) at WARP = 0   -> simple/harmonic, rhythmic echoes
 *   warped[]  delay ratios (x TIME) at WARP = 1   -> incommensurate, smooth reverb
 *   src[]     feedback permutation: line i is fed by line src[i]
 *             (identity = parallel echoes, swaps = ping-pong, shifts = cascades/patterns)
 *   denseMin/denseMax   Hadamard blend at DENSITY 0 / 100
 *   diffMax/diffScale   input allpass diffusion amount at DENSITY 100, and its size
 *   inject / out        how the stereo input enters and how the output is tapped
 *   fbMax               per-pass gain at FEEDBACK 100 (>= 1.0 sustains, held by the saturator)
 *   loopHpHz/loopLpHz   filters inside the loop (loopLpHz = 0: unfiltered loop)
 *   modRateHz/modDepthMs  modulation at MOD 100 (rate is scaled down with the knob)
 *   drive               loop saturation drive (1 = clean, >1 = grit)
 *   inGain/outGain      level staging
 *   onBeat              echo modes: Density blends the undiffused input (echoes land
 *                       exactly on Time / the beat) into the diffused one. Reverb modes
 *                       always use the diffused input (the diffusers add a little predelay).
 *
 * Even lines lean left, odd lines lean right.
 */

#include <stdint.h>

namespace horizon {

enum Inject : uint8_t {
  kInjAll = 0,   // L -> even lines, R -> odd lines (signed)
  kInjBloom,     // L -> line 0, R -> line 1 (start of the cascade)
  kInjPair,      // L -> line 0, R -> line 1
  kInjMono0,     // (L+R)/2 -> line 0 only (true ping-pong)
  kInjPrism,     // L -> line 0, R -> line 4
};

enum Out : uint8_t {
  kOutAll = 0,   // L from even lines, R from odd lines (signed)
  kOutBloom,     // weighted towards the end of the cascade
  kOutPairPlus,  // L = line 0, R = line 1, plus a little of the rest
};

struct ModeDef {
  const char *name;
  float clean[8];
  float warped[8];
  int8_t src[8];
  float denseMin, denseMax;
  float diffMax, diffScale;
  uint8_t inject, out;
  float fbMax;
  float loopHpHz, loopLpHz;
  float modRateHz, modDepthMs;
  float drive, inGain, outGain;
  uint8_t onBeat;
};

enum {
  kModeRoom = 0,
  kModeHall,
  kModeHalo,
  kModeSwell,
  kModeAbyss,
  kModeEchoVerb,
  kModeOrbit,
  kModePrism,
  kModePong,
  kModeDub,
  kNumModes
};

static const ModeDef kModes[kNumModes] = {
  // Room: fast attack, dense, shortish. Time = size.
  {"Room",
   {0.500f, 0.375f, 0.333f, 0.250f, 0.200f, 0.167f, 0.125f, 0.100f},
   {0.500f, 0.437f, 0.391f, 0.344f, 0.307f, 0.271f, 0.244f, 0.217f},
   {1, 0, 3, 2, 5, 4, 7, 6},
   0.85f, 1.00f, 0.75f, 1.0f, kInjAll, kOutAll,
   0.96f, 40.f, 6000.f, 0.8f, 0.4f, 1.0f, 0.5f, 2.2f, 0},

  // Hall: clean, filtered, realistic. Cross-channel pairs.
  {"Hall",
   {1.000f, 0.750f, 0.667f, 0.500f, 0.375f, 0.333f, 0.250f, 0.188f},
   {1.000f, 0.873f, 0.781f, 0.687f, 0.613f, 0.541f, 0.487f, 0.433f},
   {1, 0, 3, 2, 5, 4, 7, 6},
   0.75f, 1.00f, 0.70f, 1.6f, kInjAll, kOutAll,
   0.995f, 40.f, 8500.f, 0.5f, 0.9f, 1.0f, 0.5f, 2.0f, 0},

  // Halo: fast attack, sparse echoes that diffuse as they decay. Unfiltered, lush.
  {"Halo",
   {1.000f, 0.750f, 0.667f, 0.500f, 0.375f, 0.333f, 0.250f, 0.188f},
   {1.000f, 0.873f, 0.781f, 0.687f, 0.613f, 0.541f, 0.487f, 0.433f},
   {0, 1, 2, 3, 4, 5, 6, 7},
   0.00f, 0.55f, 0.35f, 2.0f, kInjAll, kOutAll,
   0.995f, 30.f, 0.f, 0.25f, 2.5f, 1.0f, 0.5f, 1.7f, 0},

  // Swell: slow attack that blooms; attack tracks TIME and FEEDBACK.
  {"Swell",
   {1.000f, 0.970f, 0.500f, 0.490f, 0.750f, 0.740f, 0.250f, 0.245f},
   {0.930f, 1.000f, 0.710f, 0.660f, 0.830f, 0.780f, 0.590f, 0.550f},
   {6, 7, 0, 1, 2, 3, 4, 5},
   0.20f, 0.85f, 0.65f, 2.5f, kInjBloom, kOutBloom,
   0.995f, 40.f, 9000.f, 0.35f, 1.5f, 1.0f, 0.6f, 2.2f, 0},

  // Abyss: very slow attack, near-endless pad tail.
  {"Abyss",
   {1.000f, 0.990f, 0.750f, 0.740f, 0.875f, 0.860f, 0.625f, 0.610f},
   {0.970f, 1.000f, 0.890f, 0.850f, 0.940f, 0.910f, 0.810f, 0.770f},
   {6, 7, 0, 1, 2, 3, 4, 5},
   0.40f, 1.00f, 0.75f, 5.0f, kInjBloom, kOutBloom,
   1.010f, 50.f, 6500.f, 0.12f, 3.5f, 1.0f, 0.6f, 1.8f, 0},

  // EchoVerb: clear echo at TIME that blooms into reverb with DENSITY.
  {"EchoVerb",
   {1.000f, 1.000f, 0.500f, 0.500f, 0.250f, 0.250f, 0.125f, 0.125f},
   {1.000f, 1.000f, 0.370f, 0.410f, 0.290f, 0.230f, 0.170f, 0.130f},
   {0, 1, 3, 2, 5, 4, 7, 6},
   0.00f, 0.80f, 0.50f, 1.5f, kInjPair, kOutPairPlus,
   0.99f, 40.f, 9000.f, 0.4f, 1.0f, 1.0f, 0.7f, 1.0f, 1},

  // Orbit: looper-ish. Repeats at TIME; DENSITY fills the gaps with haze.
  {"Orbit",
   {1.000f, 1.000f, 0.500f, 0.500f, 0.250f, 0.250f, 0.125f, 0.125f},
   {1.000f, 1.000f, 0.310f, 0.270f, 0.190f, 0.160f, 0.110f, 0.070f},
   {0, 1, 2, 3, 4, 5, 6, 7},
   0.00f, 0.60f, 0.40f, 2.0f, kInjPair, kOutPairPlus,
   1.010f, 25.f, 0.f, 0.2f, 0.6f, 1.0f, 0.7f, 1.0f, 1},

  // Prism: echoes travel through all 8 lines before repeating -> strange patterns.
  {"Prism",
   {1.000f, 0.500f, 0.750f, 0.250f, 0.875f, 0.375f, 0.625f, 0.125f},
   {1.000f, 0.618f, 0.809f, 0.382f, 0.927f, 0.472f, 0.705f, 0.236f},
   {7, 0, 1, 2, 3, 4, 5, 6},
   0.00f, 0.70f, 0.30f, 1.0f, kInjPrism, kOutAll,
   0.99f, 30.f, 0.f, 0.3f, 0.5f, 1.0f, 0.7f, 1.1f, 1},

  // Pong: small sparse stereo delay, cross-channel feedback. WARP offsets the right side.
  {"Pong",
   {1.000f, 1.000f, 0.500f, 0.500f, 0.250f, 0.250f, 0.125f, 0.125f},
   {1.000f, 0.750f, 0.330f, 0.290f, 0.210f, 0.170f, 0.120f, 0.090f},
   {1, 0, 3, 2, 5, 4, 7, 6},
   0.00f, 0.60f, 0.30f, 0.8f, kInjMono0, kOutPairPlus,
   0.99f, 60.f, 7000.f, 0.3f, 0.4f, 1.0f, 0.8f, 1.0f, 1},

  // Dub: band-limited, saturating tape-ish echo. Self-oscillates near the top of FEEDBACK.
  {"Dub",
   {1.000f, 0.750f, 0.500f, 0.375f, 0.250f, 0.188f, 0.125f, 0.094f},
   {1.000f, 0.667f, 0.410f, 0.330f, 0.230f, 0.170f, 0.130f, 0.090f},
   {0, 1, 3, 2, 5, 4, 7, 6},
   0.00f, 0.50f, 0.25f, 0.8f, kInjPair, kOutPairPlus,
   1.12f, 120.f, 3200.f, 0.45f, 1.2f, 2.5f, 0.7f, 1.0f, 1},
};

}  // namespace horizon
