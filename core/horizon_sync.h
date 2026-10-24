#pragma once
/*
 * Horizon — tempo sync for the Time knob.
 *
 * The Time parameter has two zones:
 *   -13 .. -1   synced note divisions, shortest (1/32) to longest (1/2D)
 *     0 .. 1250 milliseconds (values below 10 play as 10 ms)
 * so turning right always means longer within each zone, and patches saved
 * with v1.0.x (Time 10..1250) keep their exact times.
 *
 * A division longer than the 1.25 s maximum at the current tempo is halved
 * until it fits, so it stays on the beat (e.g. 1/2D below 144 bpm plays as 3/8).
 *
 * Copyright (c) 2026 TwistedGenetics.
 */

#include "horizon_engine.h"

namespace horizon {

struct SyncDiv {
  const char *name;
  float beats;  // length in quarter notes
};

static const SyncDiv kSyncDivs[] = {
    {"1/32", 0.125f},       {"1/16T", 1.f / 6.f}, {"1/16", 0.25f},  {"1/8T", 1.f / 3.f},
    {"1/16D", 0.375f},      {"1/8", 0.5f},        {"1/4T", 2.f / 3.f}, {"1/8D", 0.75f},
    {"1/4", 1.f},           {"1/2T", 4.f / 3.f},  {"1/4D", 1.5f},   {"1/2", 2.f},
    {"1/2D", 3.f},
};
static constexpr int kNumSyncDivs = (int)(sizeof(kSyncDivs) / sizeof(kSyncDivs[0]));
static constexpr int kTimeParamMin = -kNumSyncDivs;  // -13
static constexpr int kTimeParamMax = (int)kMaxTimeMs;  // 1250

static inline bool timeParamIsSynced(int v) { return v < 0; }

// The division for a synced Time value (v in -13..-1), or nullptr.
static inline const SyncDiv *timeParamDiv(int v) {
  const int i = v + kNumSyncDivs;
  return (v < 0 && i >= 0) ? &kSyncDivs[i] : nullptr;
}

static inline float clampBpm(float bpm) {
  return bpm < 20.f ? 20.f : (bpm > 300.f ? 300.f : bpm);
}

// Delay time in ms for a Time parameter value at the given tempo.
static inline float timeParamToMs(int v, float bpm) {
  const SyncDiv *d = timeParamDiv(v);
  if (!d) {
    if (v < (int)kMinTimeMs) return kMinTimeMs;
    if (v > kTimeParamMax) return kMaxTimeMs;
    return (float)v;
  }
  float ms = d->beats * 60000.f / clampBpm(bpm);
  while (ms > kMaxTimeMs) ms *= 0.5f;  // halve until it fits: stays on the beat
  if (ms < kMinTimeMs) ms = kMinTimeMs;
  return ms;
}

}  // namespace horizon
