#pragma once
/*
 * Horizon — stereo 8-line feedback delay network for massive reverbs and echoes.
 *
 * Platform-agnostic: no logue SDK dependencies. The host supplies one block of
 * memory (kMemSamples int16 values, zeroed) and calls process() with interleaved
 * stereo float frames at 48 kHz.
 *
 * Copyright (c) 2026 TwistedGenetics.
 */

#include <math.h>
#include <stddef.h>
#include <stdint.h>

#include "horizon_modes.h"

namespace horizon {

static constexpr int kLines = 8;
static constexpr int kLineLen = 61440;  // 1.28 s at 48 kHz
static constexpr int kNumDiff = 8;      // 4 per channel
static constexpr int kDiffLen = 4096;   // 85 ms, power of two
static constexpr int kDiffMask = kDiffLen - 1;
static constexpr size_t kMemSamples = (size_t)kLines * kLineLen + (size_t)kNumDiff * kDiffLen;
static constexpr size_t kMemBytes = kMemSamples * sizeof(int16_t);  // exactly 1 MB

static constexpr float kSampleRate = 48000.f;
static constexpr float kMaxTimeMs = 1250.f;
static constexpr float kMinTimeMs = 10.f;
static constexpr float kQ = 16384.f;      // int16 storage scale -> +/-2.0 range
static constexpr float kInvQ = 1.f / 16384.f;
static constexpr float kSatLevel = 1.8f;  // loop soft-clip ceiling (below int16 limit)
static constexpr float kModMargin = 1400.f;  // samples kept free for modulation + interp

class Engine {
 public:
  void init(int16_t *mem) {
    mem_ = mem;
    for (int i = 0; i < kLines; ++i) line_[i] = mem + (size_t)i * kLineLen;
    for (int k = 0; k < kNumDiff; ++k)
      diff_[k] = mem + (size_t)kLines * kLineLen + (size_t)k * kDiffLen;
    mode_ = kModeHall;
    pendingMode_ = mode_;
    applyMode();
    reset();
    dirty_ = true;
    updateTargets();
    snapSmoothers();
  }

  // Clears filter/LFO state (not the delay memory).
  void reset() {
    wLine_ = 0;
    wDiff_ = 0;
    for (int i = 0; i < kLines; ++i) {
      hpZ_[i] = 0.f;
      lpZ_[i] = 0.f;
    }
    outHpZ_[0] = outHpZ_[1] = outLpZ_[0] = outLpZ_[1] = 0.f;
    lfoS_[0] = 0.f; lfoC_[0] = 1.f;
    lfoS_[1] = 0.7071f; lfoC_[1] = 0.7071f;
    fade_ = 1.f;
    fadeTarget_ = 1.f;
  }

  void clearMemory() {
    if (!mem_) return;
    for (size_t i = 0; i < kMemSamples; ++i) mem_[i] = 0;
  }

  // ---- Parameters (normalised) ----------------------------------------------
  void setTimeMs(float ms) { timeMs_ = clampf(ms, kMinTimeMs, kMaxTimeMs); dirty_ = true; }
  void setFeedback(float v) { feedback_ = clampf(v, 0.f, 1.f); dirty_ = true; }
  void setWarp(float v) { warp_ = clampf(v, 0.f, 1.f); dirty_ = true; }
  void setDensity(float v) { density_ = clampf(v, 0.f, 1.f); dirty_ = true; }
  void setMod(float v) { mod_ = clampf(v, 0.f, 1.f); dirty_ = true; }
  void setTone(float v) { tone_ = clampf(v, -1.f, 1.f); dirty_ = true; }
  void setMix(float v) { mixTarget_ = clampf(v, -1.f, 1.f); }
  void setMode(int m) {
    if (m < 0 || m >= kNumModes) return;
    if (m == pendingMode_) return;
    pendingMode_ = m;
    fadeTarget_ = 0.f;  // fade the wet out, switch, fade back in
  }
  int mode() const { return mode_; }

  // ---- Audio ----------------------------------------------------------------
  void process(const float *in, float *out, size_t frames) {
    if (dirty_) updateTargets();

    const float glide = glideCoef_;
    const float sm = smoothCoef_;
    const float fadeStep = fadeStep_;

    for (size_t n = 0; n < frames; ++n) {
      const float dryL = in[2 * n];
      const float dryR = in[2 * n + 1];

      // Mode-change crossfade on the wet signal.
      if (fade_ != fadeTarget_) {
        if (fade_ > fadeTarget_) {
          fade_ -= fadeStep;
          if (fade_ <= 0.f) {
            fade_ = 0.f;
            if (pendingMode_ != mode_) {
              mode_ = pendingMode_;
              applyMode();
              updateTargets();
            }
            fadeTarget_ = 1.f;
          }
        } else {
          fade_ += fadeStep * 0.25f;  // 60 ms fade-in
          if (fade_ >= 1.f) fade_ = 1.f;
        }
      }

      // Parameter smoothing.
      blend_ += (blendT_ - blend_) * sm;
      if ((n & 15) == 0) {  // cheap: refresh rotation every 16 samples
        const float th = blend_ * 0.78539816f;
        rotC_ = cosf(th);
        rotS_ = sinf(th);
      }
      diffG_ += (diffGT_ - diffG_) * sm;
      mix_ += (mixTarget_ - mix_) * sm;
      for (int i = 0; i < kLines; ++i) gain_[i] += (gainT_[i] - gain_[i]) * sm;

      // ---- Input diffusion (4 allpasses per side) ----
      float dL = dryL * inGain_;
      float dR = dryR * inGain_;
      const float g = diffG_;
      for (int k = 0; k < 4; ++k) {
        dL = allpass(k, dL, g);
        dR = allpass(k + 4, dR, g);
      }
      wDiff_ = (wDiff_ + 1) & kDiffMask;

      // ---- Modulation: two quadrature LFOs -> 8 phases ----
      for (int o = 0; o < 2; ++o) {
        lfoS_[o] += lfoK_[o] * lfoC_[o];
        lfoC_[o] -= lfoK_[o] * lfoS_[o];
      }
      const float mv[8] = {lfoS_[0], lfoS_[1], lfoC_[0], lfoC_[1],
                           -lfoS_[0], -lfoS_[1], -lfoC_[0], -lfoC_[1]};

      // ---- Read lines ----
      float rd[kLines];
      for (int i = 0; i < kLines; ++i) {
        len_[i] += (lenT_[i] - len_[i]) * glide;
        const float d = len_[i] + mv[i] * modDepth_[i];
        rd[i] = readLine(i, d);
      }

      // ---- Output taps ----
      float wetL = 0.f, wetR = 0.f;
      for (int i = 0; i < kLines; ++i) {
        wetL += outL_[i] * rd[i];
        wetR += outR_[i] * rd[i];
      }

      // ---- Loop: gain, filters ----
      float x[kLines];
      for (int i = 0; i < kLines; ++i) {
        float v = rd[i] * gain_[i];
        hpZ_[i] += (v - hpZ_[i]) * loopHpA_;
        v -= hpZ_[i];
        if (loopLp_) {
          lpZ_[i] += (v - lpZ_[i]) * loopLpA_;
          v = lpZ_[i];
        }
        x[i] = v;
      }

      // ---- Feedback matrix: permutation, then energy-preserving rotation ----
      // Three butterfly stages of Givens rotations by theta. theta = 0 leaves the
      // permutation untouched (clean echoes); theta = pi/4 is a full Hadamard mix.
      // Orthogonal at every theta, so DENSITY never changes the decay time.
      float f[kLines];
      for (int i = 0; i < kLines; ++i) f[i] = x[src_[i]];
      rotate8(f, rotC_, rotS_);

      // ---- Inject + saturate + write ----
      for (int i = 0; i < kLines; ++i) {
        float v = f[i] + injL_[i] * dL + injR_[i] * dR;
        v = softClip(v * drive_) * invDrive_;
        line_[i][wLine_] = toQ(v);
      }
      if (++wLine_ >= kLineLen) wLine_ = 0;

      // ---- Output tone ----
      outHpZ_[0] += (wetL - outHpZ_[0]) * outHpA_;
      outHpZ_[1] += (wetR - outHpZ_[1]) * outHpA_;
      wetL -= outHpZ_[0];
      wetR -= outHpZ_[1];
      outLpZ_[0] += (wetL - outLpZ_[0]) * outLpA_;
      outLpZ_[1] += (wetR - outLpZ_[1]) * outLpA_;
      wetL = outLpZ_[0] * outGain_ * fade_;
      wetR = outLpZ_[1] * outGain_ * fade_;

      // ---- Mix (bipolar: negative = inverted wet) ----
      const float m = mix_;
      const float dry = 1.f - fabsf(m);
      out[2 * n] = dryL * dry + wetL * m;
      out[2 * n + 1] = dryR * dry + wetR * m;
    }

    // Keep tiny filter states from going denormal on hosts without FTZ.
    for (int i = 0; i < kLines; ++i) {
      if (fabsf(hpZ_[i]) < 1e-15f) hpZ_[i] = 0.f;
      if (fabsf(lpZ_[i]) < 1e-15f) lpZ_[i] = 0.f;
    }
    // Re-normalise LFO amplitude (magic-circle drift).
    for (int o = 0; o < 2; ++o) {
      const float a = lfoS_[o] * lfoS_[o] + lfoC_[o] * lfoC_[o];
      const float s = 1.5f - 0.5f * a;
      lfoS_[o] *= s;
      lfoC_[o] *= s;
    }
  }

 private:
  static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

  static inline float onePoleA(float hz) {
    if (hz > 20000.f) hz = 20000.f;
    return 1.f - expf(-2.f * 3.14159265f * hz / kSampleRate);
  }

  // Rational tanh-style soft clip, bounded to +/-kSatLevel.
  static inline float softClip(float v) {
    float t = v * (1.f / kSatLevel);
    if (t > 3.f) t = 3.f;
    if (t < -3.f) t = -3.f;
    const float t2 = t * t;
    return kSatLevel * t * (27.f + t2) / (27.f + 9.f * t2);
  }

  // Truncate toward zero. Rounding lets tails get stuck as +/-1 LSB limit cycles
  // that never decay; truncation's tiny bias only matters at those levels.
  static inline int16_t toQ(float v) {
    float s = v * kQ;
    if (s > 32767.f) s = 32767.f;
    if (s < -32767.f) s = -32767.f;
    return (int16_t)s;
  }

  static inline void rotate8(float *x, float c, float s) {
    for (int len = 1; len < 8; len <<= 1) {
      for (int i = 0; i < 8; i += len << 1) {
        for (int j = i; j < i + len; ++j) {
          const float a = x[j], b = x[j + len];
          x[j] = c * a + s * b;
          x[j + len] = c * b - s * a;
        }
      }
    }
  }

  inline float readLine(int i, float d) {
    int di = (int)d;
    const float fr = d - (float)di;
    int r0 = wLine_ - di;
    if (r0 < 0) r0 += kLineLen;
    int r1 = r0 - 1;
    if (r1 < 0) r1 += kLineLen;
    const float a = (float)line_[i][r0];
    const float c = (float)line_[i][r1];
    return (a + (c - a) * fr) * kInvQ;
  }

  inline float allpass(int k, float x, float g) {
    const float z = (float)diff_[k][(wDiff_ - diffLen_[k]) & kDiffMask] * kInvQ;
    const float y = z - g * x;
    diff_[k][wDiff_] = toQ(x + g * y);
    return y;
  }

  void applyMode() {
    const ModeDef &m = kModes[mode_];
    for (int i = 0; i < kLines; ++i) {
      src_[i] = m.src[i];
      injL_[i] = injR_[i] = 0.f;
      outL_[i] = outR_[i] = 0.f;
    }
    static const float sgnA[4] = {1.f, -1.f, 1.f, -1.f};
    static const float sgnB[4] = {1.f, 1.f, -1.f, -1.f};
    switch (m.inject) {
      case kInjAll:
        for (int p = 0; p < 4; ++p) {
          injL_[2 * p] = 0.5f * sgnA[p];
          injR_[2 * p + 1] = 0.5f * sgnB[p];
        }
        break;
      case kInjBloom:
      case kInjPair:
        injL_[0] = 1.f;
        injR_[1] = 1.f;
        break;
      case kInjMono0:
        injL_[0] = 0.5f;
        injR_[0] = 0.5f;
        break;
      case kInjPrism:
        injL_[0] = 1.f;
        injR_[4] = 1.f;
        break;
    }
    switch (m.out) {
      case kOutAll:
        for (int p = 0; p < 4; ++p) {
          outL_[2 * p] = 0.5f * sgnB[p];
          outR_[2 * p + 1] = 0.5f * sgnA[p];
        }
        break;
      case kOutBloom: {
        static const float w[4] = {0.12f, 0.35f, 0.65f, 1.0f};
        for (int p = 0; p < 4; ++p) {
          outL_[2 * p] = w[p] * 0.75f * sgnA[p];
          outR_[2 * p + 1] = w[p] * 0.75f * sgnB[p];
        }
        break;
      }
      case kOutPairPlus:
        outL_[0] = 0.85f;
        outR_[1] = 0.85f;
        for (int p = 1; p < 4; ++p) {
          outL_[2 * p] = 0.25f * sgnA[p];
          outR_[2 * p + 1] = 0.25f * sgnB[p];
        }
        break;
    }
    // Diffuser lengths (ms), scaled per mode; distinct primes-ish per side.
    static const float baseMs[8] = {4.77f, 3.59f, 12.73f, 9.31f, 5.21f, 3.97f, 11.83f, 8.71f};
    for (int k = 0; k < kNumDiff; ++k) {
      int l = (int)(baseMs[k] * m.diffScale * kSampleRate * 0.001f);
      if (l < 1) l = 1;
      if (l > kDiffLen - 1) l = kDiffLen - 1;
      diffLen_[k] = l;
    }
    loopLp_ = m.loopLpHz > 0.f;
    drive_ = m.drive;
    invDrive_ = 1.f / m.drive;
    inGain_ = m.inGain;
    outGain_ = m.outGain;
    dirty_ = true;
  }

  void updateTargets() {
    dirty_ = false;
    const ModeDef &m = kModes[mode_];
    const float T = timeMs_ * kSampleRate * 0.001f;

    // Feedback: more resolution near the top without bunching all long tails there.
    // Knob 50 ~ 0.65/pass, 80 ~ 0.91, 90 ~ 0.97, 100 = mode ceiling (hold).
    const float k = 1.f - feedback_;
    const float fb = m.fbMax * (1.f - k * sqrtf(k));

    float lref = 4.f;
    for (int i = 0; i < kLines; ++i) {
      const float r = m.clean[i] + warp_ * (m.warped[i] - m.clean[i]);
      float l = T * r;
      if (l < 4.f) l = 4.f;
      if (l > (float)kLineLen - kModMargin) l = (float)kLineLen - kModMargin;
      lenT_[i] = l;
      if (l > lref) lref = l;
    }
    // Equal decay rate across lines: FEEDBACK is the gain per pass of the longest line.
    for (int i = 0; i < kLines; ++i)
      gainT_[i] = (fb <= 0.f) ? 0.f : powf(fb, lenT_[i] / lref);

    blendT_ = m.denseMin + density_ * (m.denseMax - m.denseMin);
    diffGT_ = m.diffMax * density_;

    // Modulation: depth rises with the knob; rate rises gently; depth backs off at high rate.
    const float rate = m.modRateHz * (0.5f + mod_);
    const float rateComp = rate > 0.6f ? 0.6f / rate : 1.f;
    const float depth = mod_ * mod_ * m.modDepthMs * kSampleRate * 0.001f * rateComp;
    static const float spread[8] = {1.f, 0.83f, 0.91f, 0.74f, 0.97f, 0.79f, 0.88f, 0.69f};
    for (int i = 0; i < kLines; ++i) {
      float dd = depth * spread[i];
      if (dd > kModMargin - 8.f) dd = kModMargin - 8.f;
      if (dd > lenT_[i] - 2.f) dd = lenT_[i] - 2.f;
      if (dd < 0.f) dd = 0.f;
      modDepth_[i] = dd;
    }
    lfoK_[0] = 2.f * 3.14159265f * rate / kSampleRate;
    lfoK_[1] = lfoK_[0] * 1.37f;

    // Tone: left = darker, right = thinner, centre = open.
    const float t = tone_;
    const float outLpHz = t < 0.f ? 18000.f * powf(600.f / 18000.f, -t) : 18000.f;
    const float outHpHz = t > 0.f ? 20.f * powf(1200.f / 20.f, t) : 20.f;
    outLpA_ = onePoleA(outLpHz);
    outHpA_ = onePoleA(outHpHz);
    float loopLpHz = m.loopLpHz;
    if (t < 0.f) loopLpHz *= powf(0.15f, -t);
    float loopHpHz = m.loopHpHz * (t > 0.f ? powf(8.f, t) : 1.f);
    if (loopHpHz > 800.f) loopHpHz = 800.f;
    loopLpA_ = loopLp_ ? onePoleA(loopLpHz) : 1.f;
    loopHpA_ = onePoleA(loopHpHz);

    glideCoef_ = 1.f - expf(-1.f / (0.12f * kSampleRate));
    smoothCoef_ = 1.f - expf(-1.f / (0.02f * kSampleRate));
    fadeStep_ = 1.f / (0.015f * kSampleRate);
  }

  void snapSmoothers() {
    for (int i = 0; i < kLines; ++i) {
      len_[i] = lenT_[i];
      gain_[i] = gainT_[i];
    }
    blend_ = blendT_;
    rotC_ = cosf(blend_ * 0.78539816f);
    rotS_ = sinf(blend_ * 0.78539816f);
    diffG_ = diffGT_;
    mix_ = mixTarget_;
  }

  // Memory
  int16_t *mem_ = nullptr;
  int16_t *line_[kLines] = {};
  int16_t *diff_[kNumDiff] = {};
  int wLine_ = 0;
  int wDiff_ = 0;

  // Params
  float timeMs_ = 400.f, feedback_ = 0.65f, warp_ = 0.5f, density_ = 0.6f;
  float mod_ = 0.3f, tone_ = 0.f, mixTarget_ = 0.35f;
  int mode_ = kModeHall, pendingMode_ = kModeHall;
  bool dirty_ = true;

  // Mode config
  int8_t src_[kLines] = {};
  float injL_[kLines] = {}, injR_[kLines] = {};
  float outL_[kLines] = {}, outR_[kLines] = {};
  int diffLen_[kNumDiff] = {};
  bool loopLp_ = true;
  float drive_ = 1.f, invDrive_ = 1.f, inGain_ = 0.5f, outGain_ = 1.f;

  // Targets + smoothed state
  float lenT_[kLines] = {}, len_[kLines] = {};
  float gainT_[kLines] = {}, gain_[kLines] = {};
  float modDepth_[kLines] = {};
  float blendT_ = 0.f, blend_ = 0.f, diffGT_ = 0.f, diffG_ = 0.f, mix_ = 0.f;
  float rotC_ = 1.f, rotS_ = 0.f;
  float glideCoef_ = 0.f, smoothCoef_ = 0.f, fadeStep_ = 0.f;
  float fade_ = 1.f, fadeTarget_ = 1.f;

  // Filters + LFO
  float hpZ_[kLines] = {}, lpZ_[kLines] = {};
  float loopHpA_ = 0.f, loopLpA_ = 1.f;
  float outHpZ_[2] = {}, outLpZ_[2] = {};
  float outHpA_ = 0.f, outLpA_ = 1.f;
  float lfoS_[2] = {0.f, 0.7071f}, lfoC_[2] = {1.f, 0.7071f}, lfoK_[2] = {};
};

}  // namespace horizon
