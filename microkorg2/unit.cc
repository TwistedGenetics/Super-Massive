/*
 *  File: unit.cc
 *
 *  Horizon - microKORG 2 reverb (revfx) unit glue.
 *  All DSP lives in core/horizon_engine.h (shared with the desktop test harness).
 *
 *  Built with -DHORIZON_LOADMETER (tools/mk2-meter), the Mode display also
 *  shows the CPU load of this unit, e.g. "Hall 23%".
 *
 *  Copyright (c) 2026 TwistedGenetics. BSD 3-Clause, see LICENSE.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "unit.h"
#include "runtime.h"
#include "unit_revfx.h"
#include "horizon_engine.h"
#ifdef HORIZON_LOADMETER
#include <stdio.h>
#include <time.h>
#endif

namespace {

enum {
  kParamTime = 0,
  kParamFeedback,
  kParamWarp,
  kParamDensity,
  kParamMod,
  kParamTone,
  kParamMode,
  kParamMix,
  kNumParams
};

static_assert(kNumParams <= UNIT_REVFX_MAX_PARAM_COUNT, "too many params for a revfx unit");
static_assert(horizon::kMemBytes <= REVFX_MEMORY_SIZE_BYTES, "delay memory over the revfx SDRAM budget");

horizon::Engine s_engine;
int16_t *s_mem = nullptr;
int32_t s_params[kNumParams];

// Delay memory is cleared a slice per render call (never 1 MB in one go on the
// audio thread). Until it's clean the unit passes audio through dry.
size_t s_clear_pos = 0;
bool s_clearing = false;
constexpr size_t kClearSlice = 8192;  // int16 values per render call (16 KB)

#ifdef HORIZON_LOADMETER
// Time spent in unit_render as a share of the buffer's real-time length.
// Peak over ~0.5 s. 100 % = no time left (crackles).
float s_load_peak = 0.f, s_load_show = 0.f;
uint32_t s_load_frames = 0;
char s_mode_str[16];
inline double nowNs() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}
#endif

// NaN/inf check on the bits: the build uses -ffast-math, which lets the
// compiler drop float NaN comparisons.
inline bool finiteBits(float x) {
  union { float f; uint32_t u; } b;
  b.f = x;
  return (b.u & 0x7F800000u) != 0x7F800000u;
}

void startClear() {
  s_clear_pos = 0;
  s_clearing = true;
}

void applyParam(uint8_t id, int32_t v) {
  switch (id) {
    case kParamTime:     s_engine.setTimeMs((float)v); break;
    case kParamFeedback: s_engine.setFeedback(v * 0.01f); break;
    case kParamWarp:     s_engine.setWarp(v * 0.01f); break;
    case kParamDensity:  s_engine.setDensity(v * 0.01f); break;
    case kParamMod:      s_engine.setMod(v * 0.01f); break;
    case kParamTone:     s_engine.setTone(v * 0.01f); break;
    case kParamMode:     s_engine.setMode((int)v); break;
    case kParamMix:      s_engine.setMix(v * 0.01f); break;
    default: break;
  }
}

}  // namespace

__attribute__((used)) int8_t unit_init(const unit_runtime_desc_t *desc) {
  if (!desc) return k_unit_err_undef;
  if (desc->target != unit_header.target) return k_unit_err_target;
  if (!UNIT_API_IS_COMPAT(desc->api)) return k_unit_err_api_version;
  if (desc->samplerate != 48000) return k_unit_err_samplerate;
  if (desc->input_channels != 2 || desc->output_channels != 2) return k_unit_err_geometry;
  if (!desc->hooks.sdram_alloc) return k_unit_err_memory;

  s_mem = reinterpret_cast<int16_t *>(desc->hooks.sdram_alloc(horizon::kMemBytes));
  if (!s_mem) return k_unit_err_memory;

  s_engine.init(s_mem);
  for (int i = 0; i < kNumParams; ++i) {
    s_params[i] = unit_header.params[i].init;
    applyParam((uint8_t)i, s_params[i]);
  }
  startClear();
  return k_unit_err_none;
}

__attribute__((used)) void unit_teardown() {
  // sdram_alloc memory is freed by the runtime after teardown
  s_mem = nullptr;
}

__attribute__((used)) void unit_reset() {
  s_engine.reset();
  startClear();
}

__attribute__((used)) void unit_resume() {}

__attribute__((used)) void unit_suspend() {}

__attribute__((used)) void unit_render(const float *in, float *out, uint32_t frames) {
#ifdef HORIZON_LOADMETER
  const double t_start = nowNs();
#endif
  if (!s_mem) {
    memcpy(out, in, frames * 2 * sizeof(float));
    return;
  }

  if (s_clearing) {
    const size_t end = s_clear_pos + kClearSlice < horizon::kMemSamples
                           ? s_clear_pos + kClearSlice
                           : horizon::kMemSamples;
    memset(s_mem + s_clear_pos, 0, (end - s_clear_pos) * sizeof(int16_t));
    s_clear_pos = end;
    if (s_clear_pos >= horizon::kMemSamples) {
      s_clearing = false;
      s_engine.reset();
    }
    memcpy(out, in, frames * 2 * sizeof(float));
  } else {
    s_engine.process(in, out, frames);

    // Safety net: if anything upstream feeds NaN/inf, don't let it live in the loop.
    bool ok = true;
    for (uint32_t i = 0; i < frames * 2; i += 8) ok = ok && finiteBits(out[i]);
    if (!ok) {
      memcpy(out, in, frames * 2 * sizeof(float));
      for (uint32_t i = 0; i < frames * 2; ++i)
        if (!finiteBits(out[i])) out[i] = 0.f;
      startClear();
    }
  }

#ifdef HORIZON_LOADMETER
  const double used = nowNs() - t_start;
  const float load = (float)(used / ((double)frames * (1e9 / 48000.0)));
  if (load > s_load_peak) s_load_peak = load;
  s_load_frames += frames;
  if (s_load_frames >= 24000) {
    s_load_show = s_load_peak;
    s_load_peak = 0.f;
    s_load_frames = 0;
  }
#endif
}

__attribute__((used)) void unit_set_param_value(uint8_t id, int32_t value) {
  if (id >= kNumParams) return;
  const int32_t lo = unit_header.params[id].min, hi = unit_header.params[id].max;
  value = value < lo ? lo : (value > hi ? hi : value);
  s_params[id] = value;
  applyParam(id, value);
}

__attribute__((used)) int32_t unit_get_param_value(uint8_t id) {
  return id < kNumParams ? s_params[id] : 0;
}

__attribute__((used)) const char *unit_get_param_str_value(uint8_t id, int32_t value) {
  if (id != kParamMode || value < 0 || value >= horizon::kNumModes) return nullptr;
#ifdef HORIZON_LOADMETER
  int pct = (int)(s_load_show * 100.f + 0.5f);
  if (pct > 999) pct = 999;
  snprintf(s_mode_str, sizeof(s_mode_str), "%.4s %d%%", horizon::kModes[value].name, pct);
  return s_mode_str;
#else
  return horizon::kModes[value].name;
#endif
}

__attribute__((used)) void unit_set_tempo(uint32_t tempo) {
  (void)tempo;  // TIME is in ms for now (tempo sync is on the roadmap)
}
