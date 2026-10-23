#pragma once
/*
 * Horizon — stereo 8-line feedback delay network for massive reverbs and echoes.
 *
 * Platform-agnostic: no logue SDK dependencies. The host supplies one block of
 * memory (kMemSamples int16 values, zeroed) and calls process() with interleaved
 * stereo float frames at 48 kHz.
 *
 * Performance notes (v1.0.1): the 8 delay lines are processed as two 4-wide
 * GCC vectors (NEON on the microKORG 2, SSE on a desktop; same code, same
 * results), the permutation + 3-stage rotation feedback network is folded into
 * one 8x8 matrix rebuilt once per block, the loop saturator is a division-free
 * cubic, and parameter smoothing runs once per block. ~6x fewer instructions
 * per sample than v1.0.0.
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

// 4-wide float vector: lines 0-3 in one, lines 4-7 in the other.
typedef float hf4 __attribute__((vector_size(16)));
typedef int hi4 __attribute__((vector_size(16)));  // type of a float-vector comparison
// 2-wide: the left and right input diffuser chains side by side.
typedef float hf2 __attribute__((vector_size(8)));
typedef int hi2 __attribute__((vector_size(8)));
#define HZ_INLINE inline __attribute__((always_inline))

namespace v4 {
static HZ_INLINE hf4 splat(float v) { return (hf4){v, v, v, v}; }
static HZ_INLINE hf4 sel(hi4 m, hf4 a, hf4 b) { return m ? a : b; }
static HZ_INLINE hf4 clamp(hf4 x, float lo, float hi) {
  x = sel(x > hi, splat(hi), x);
  return sel(x < lo, splat(lo), x);
}
// Cubic soft clip: unity slope at 0, flat at +-1.5 (-> +-1). No division.
static HZ_INLINE hf4 softclip(hf4 x) {
  x = clamp(x, -1.5f, 1.5f);
  return x - 0.148148148f * x * x * x;
}
static HZ_INLINE float hsum(hf4 x) { return (x[0] + x[1]) + (x[2] + x[3]); }
static HZ_INLINE hf2 clamp2(hf2 x, float lo, float hi) {
  const hf2 l = (hf2){lo, lo}, h = (hf2){hi, hi};
  x = (x > hi) ? h : x;
  return (x < lo) ? l : x;
}
}  // namespace v4

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
    for (int h = 0; h < 2; ++h) {
      hpZ_[h] = v4::splat(0.f);
      lpZ_[h] = v4::splat(0.f);
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
    if (frames == 0) return;
    if (dirty_) updateTargets();

    // ---- Control rate: once per block ----
    if (frames != lastFrames_) {
      lastFrames_ = frames;
      blockSm_ = 1.f - expf(-(float)frames / (0.02f * kSampleRate));
    }
    const float bs = blockSm_;
    for (int h = 0; h < 2; ++h) gain_[h] += (gainT_[h] - gain_[h]) * bs;
    blend_ += (blendT_ - blend_) * bs;
    if (matDirty_ || fabsf(blend_ - matBlend_) > 1e-4f) buildMatrix();
    diffG_ += (diffGT_ - diffG_) * bs;

    const float invFrames = 1.f / (float)frames;
    const float mixEnd = mix_ + (mixTarget_ - mix_) * bs;
    const float mixStep = (mixEnd - mix_) * invFrames;

    // Mode-change crossfade on the wet signal: linear ramp, switch at block end.
    float fadeEnd = fade_;
    bool switchAtEnd = false;
    if (fadeTarget_ < fade_) {
      fadeEnd = fade_ - fadeOutStep_ * (float)frames;
      if (fadeEnd <= 0.f) {
        fadeEnd = 0.f;
        switchAtEnd = true;
      }
    } else if (fade_ < 1.f) {
      fadeEnd = fade_ + fadeInStep_ * (float)frames;
      if (fadeEnd > 1.f) fadeEnd = 1.f;
    }
    const float fadeStep = (fadeEnd - fade_) * invFrames;

    // Hoist everything the sample loop reads into locals (registers).
    const hf4 glide = v4::splat(glideCoef_);
    const hf4 hpA = v4::splat(loopHpA_), lpA = v4::splat(loopLpA_);
    const hf4 lenT0 = lenT_[0], lenT1 = lenT_[1];
    const hf4 md0 = modDepth_[0], md1 = modDepth_[1];
    const hf4 g0 = gain_[0], g1 = gain_[1];
    const hf4 iL0 = injL_[0], iL1 = injL_[1], iR0 = injR_[0], iR1 = injR_[1];
    const hf4 oL0 = outL_[0], oL1 = outL_[1], oR0 = outR_[0], oR1 = outR_[1];
    const float satIn = satIn_, satOut = satOut_;
    const float g = diffG_, inGain = inGain_, wetGain = outGain_;
    const float lk0 = lfoK_[0], lk1 = lfoK_[1];
    const float oHpA = outHpA_, oLpA = outLpA_;
    hf4 len0 = len_[0], len1 = len_[1];
    hf4 hp0 = hpZ_[0], hp1 = hpZ_[1], lp0 = lpZ_[0], lp1 = lpZ_[1];
    float s0 = lfoS_[0], c0 = lfoC_[0], s1 = lfoS_[1], c1 = lfoC_[1];
    float mix = mix_, fade = fade_;

    for (size_t n = 0; n < frames; ++n) {
      const float dryL = in[2 * n];
      const float dryR = in[2 * n + 1];

      // ---- Input diffusion (4 allpasses per side, L and R side by side) ----
      hf2 dLR = (hf2){dryL, dryR} * inGain;
      dLR = allpass2(0, dLR, g);
      dLR = allpass2(1, dLR, g);
      dLR = allpass2(2, dLR, g);
      dLR = allpass2(3, dLR, g);
      wDiff_ = (wDiff_ + 1) & kDiffMask;
      const float dL = dLR[0], dR = dLR[1];

      // ---- Modulation: two quadrature LFOs -> 8 phases ----
      s0 += lk0 * c0; c0 -= lk0 * s0;
      s1 += lk1 * c1; c1 -= lk1 * s1;
      const hf4 m0 = (hf4){s0, s1, c0, c1};

      // ---- Read lines (glide towards target lengths) ----
      len0 += (lenT0 - len0) * glide;
      len1 += (lenT1 - len1) * glide;
      const hf4 rd0 = readLines(len0 + m0 * md0, 0);
      const hf4 rd1 = readLines(len1 - m0 * md1, 4);

      // ---- Output taps ----
      float wetL = v4::hsum(oL0 * rd0 + oL1 * rd1);
      float wetR = v4::hsum(oR0 * rd0 + oR1 * rd1);

      // ---- Loop: gain, high-pass, low-pass (lpA = 1 in unfiltered modes) ----
      hf4 x0 = rd0 * g0, x1 = rd1 * g1;
      hp0 += (x0 - hp0) * hpA; x0 -= hp0;
      hp1 += (x1 - hp1) * hpA; x1 -= hp1;
      lp0 += (x0 - lp0) * lpA; x0 = lp0;
      lp1 += (x1 - lp1) * lpA; x1 = lp1;

      // ---- Feedback matrix (permutation x rotation, one 8x8 multiply) ----
      hf4 f0 = mat_[0][0] * x0[0] + mat_[1][0] * x0[1] + mat_[2][0] * x0[2] + mat_[3][0] * x0[3] +
               mat_[4][0] * x1[0] + mat_[5][0] * x1[1] + mat_[6][0] * x1[2] + mat_[7][0] * x1[3];
      hf4 f1 = mat_[0][1] * x0[0] + mat_[1][1] * x0[1] + mat_[2][1] * x0[2] + mat_[3][1] * x0[3] +
               mat_[4][1] * x1[0] + mat_[5][1] * x1[1] + mat_[6][1] * x1[2] + mat_[7][1] * x1[3];

      // ---- Inject + saturate + write ----
      f0 += iL0 * dL + iR0 * dR;
      f1 += iL1 * dL + iR1 * dR;
      f0 = v4::softclip(f0 * satIn) * satOut;  // |f| <= kSatLevel, fits int16 unclamped
      f1 = v4::softclip(f1 * satIn) * satOut;
      writeLines(f0 * kQ, 0);
      writeLines(f1 * kQ, 4);
      if (++wLine_ >= kLineLen) wLine_ = 0;

      // ---- Output tone ----
      outHpZ_[0] += (wetL - outHpZ_[0]) * oHpA;
      outHpZ_[1] += (wetR - outHpZ_[1]) * oHpA;
      wetL -= outHpZ_[0];
      wetR -= outHpZ_[1];
      outLpZ_[0] += (wetL - outLpZ_[0]) * oLpA;
      outLpZ_[1] += (wetR - outLpZ_[1]) * oLpA;
      const float wg = wetGain * fade;
      wetL = outLpZ_[0] * wg;
      wetR = outLpZ_[1] * wg;

      // ---- Mix (bipolar: negative = inverted wet) ----
      const float dry = 1.f - (mix < 0.f ? -mix : mix);
      out[2 * n] = dryL * dry + wetL * mix;
      out[2 * n + 1] = dryR * dry + wetR * mix;
      mix += mixStep;
      fade += fadeStep;
    }

    // ---- Store state ----
    len_[0] = len0; len_[1] = len1;
    hpZ_[0] = flushDenormals(hp0); hpZ_[1] = flushDenormals(hp1);
    lpZ_[0] = flushDenormals(lp0); lpZ_[1] = flushDenormals(lp1);
    for (int o = 0; o < 2; ++o) {
      if (fabsf(outHpZ_[o]) < 1e-15f) outHpZ_[o] = 0.f;
      if (fabsf(outLpZ_[o]) < 1e-15f) outLpZ_[o] = 0.f;
    }
    // Re-normalise LFO amplitude (magic-circle drift).
    {
      float a = s0 * s0 + c0 * c0, k = 1.5f - 0.5f * a;
      lfoS_[0] = s0 * k; lfoC_[0] = c0 * k;
      a = s1 * s1 + c1 * c1; k = 1.5f - 0.5f * a;
      lfoS_[1] = s1 * k; lfoC_[1] = c1 * k;
    }
    mix_ = mixEnd;
    fade_ = fadeEnd;
    if (switchAtEnd) {
      if (pendingMode_ != mode_) {
        mode_ = pendingMode_;
        applyMode();
        updateTargets();
      }
      fadeTarget_ = 1.f;
    }
  }

 private:
  static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

  static inline float onePoleA(float hz) {
    if (hz > 20000.f) hz = 20000.f;
    return 1.f - expf(-2.f * 3.14159265f * hz / kSampleRate);
  }

  static inline hf4 flushDenormals(hf4 x) {
    for (int j = 0; j < 4; ++j)
      if (fabsf(x[j]) < 1e-15f) x[j] = 0.f;
    return x;
  }

  // Three butterfly stages of Givens rotations (theta = 0: identity,
  // theta = pi/4: 8x8 Hadamard). Orthogonal for every theta. Only used to
  // build the matrix.
  static void rotate8(float *x, float c, float s) {
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

  // mat_[j] = column j of (rotation x permutation): the contribution of x[j]
  // to all 8 lines. f = R * P * x where (P x)[i] = x[src[i]].
  void buildMatrix() {
    const float th = blend_ * 0.78539816f;
    const float c = cosf(th), s = sinf(th);
    float col[8][8] = {};
    for (int i = 0; i < kLines; ++i) {
      float e[8] = {};
      e[i] = 1.f;
      rotate8(e, c, s);  // column i of R
      float *dst = col[src_[i]];
      for (int r = 0; r < kLines; ++r) dst[r] += e[r];
    }
    for (int j = 0; j < kLines; ++j) {
      mat_[j][0] = (hf4){col[j][0], col[j][1], col[j][2], col[j][3]};
      mat_[j][1] = (hf4){col[j][4], col[j][5], col[j][6], col[j][7]};
    }
    matBlend_ = blend_;
    matDirty_ = false;
  }

  // Read 4 lines (base..base+3) at fractional delays d (samples), linear interp.
  HZ_INLINE hf4 readLines(hf4 d, int base) {
    hf4 a, c;
#if defined(__clang__) || (defined(__GNUC__) && __GNUC__ >= 9)
    // Index math 4-wide; only the 8 loads are scalar.
    const hi4 di = __builtin_convertvector(d, hi4);
    const hf4 fr = d - __builtin_convertvector(di, hf4);
    const hi4 L = (hi4){kLineLen, kLineLen, kLineLen, kLineLen};
    hi4 r0 = wLine_ - di;
    r0 += (r0 < 0) & L;  // comparison gives -1 (true) / 0 per lane
    hi4 r1 = r0 - 1;
    r1 += (r1 < 0) & L;
    for (int j = 0; j < 4; ++j) {
      const int16_t *ln = line_[base + j];
      a[j] = (float)ln[r0[j]];
      c[j] = (float)ln[r1[j]];
    }
#else
    hf4 ip;
    for (int j = 0; j < 4; ++j) {
      const int dj = (int)d[j];
      ip[j] = (float)dj;
      int r0 = wLine_ - dj;
      if (r0 < 0) r0 += kLineLen;
      const int r1 = r0 > 0 ? r0 - 1 : kLineLen - 1;
      const int16_t *ln = line_[base + j];
      a[j] = (float)ln[r0];
      c[j] = (float)ln[r1];
    }
    const hf4 fr = d - ip;
#endif
    return (a + (c - a) * fr) * kInvQ;
  }

  // q is already scaled by kQ and bounded by the saturator (|q| < 29500).
  // One vector float->int conversion where the compiler has it (GCC 9+, clang).
  HZ_INLINE void writeLines(hf4 q, int base) {
#if defined(__clang__) || (defined(__GNUC__) && __GNUC__ >= 9)
    const hi4 qi = __builtin_convertvector(q, hi4);
    for (int j = 0; j < 4; ++j) line_[base + j][wLine_] = (int16_t)qi[j];
#else
    for (int j = 0; j < 4; ++j) line_[base + j][wLine_] = (int16_t)(int)q[j];
#endif
  }

  // Allpass stage k for both channels: left uses buffer k, right buffer k + 4.
  HZ_INLINE hf2 allpass2(int k, hf2 x, float g) {
    int16_t *bl = diff_[k], *br = diff_[k + 4];
    const hf2 z = (hf2){(float)bl[(wDiff_ - diffLen_[k]) & kDiffMask],
                        (float)br[(wDiff_ - diffLen_[k + 4]) & kDiffMask]} * kInvQ;
    const hf2 y = z - g * x;
    const hf2 q = v4::clamp2((x + g * y) * kQ, -32768.f, 32767.f);
#if defined(__clang__) || (defined(__GNUC__) && __GNUC__ >= 9)
    const hi2 qi = __builtin_convertvector(q, hi2);
    bl[wDiff_] = (int16_t)qi[0];
    br[wDiff_] = (int16_t)qi[1];
#else
    bl[wDiff_] = (int16_t)(int)q[0];
    br[wDiff_] = (int16_t)(int)q[1];
#endif
    return y;
  }

  void applyMode() {
    const ModeDef &m = kModes[mode_];
    float injL[8] = {}, injR[8] = {}, outL[8] = {}, outR[8] = {};
    for (int i = 0; i < kLines; ++i) src_[i] = m.src[i];
    static const float sgnA[4] = {1.f, -1.f, 1.f, -1.f};
    static const float sgnB[4] = {1.f, 1.f, -1.f, -1.f};
    switch (m.inject) {
      case kInjAll:
        for (int p = 0; p < 4; ++p) {
          injL[2 * p] = 0.5f * sgnA[p];
          injR[2 * p + 1] = 0.5f * sgnB[p];
        }
        break;
      case kInjBloom:
      case kInjPair:
        injL[0] = 1.f;
        injR[1] = 1.f;
        break;
      case kInjMono0:
        injL[0] = 0.5f;
        injR[0] = 0.5f;
        break;
      case kInjPrism:
        injL[0] = 1.f;
        injR[4] = 1.f;
        break;
    }
    switch (m.out) {
      case kOutAll:
        for (int p = 0; p < 4; ++p) {
          outL[2 * p] = 0.5f * sgnB[p];
          outR[2 * p + 1] = 0.5f * sgnA[p];
        }
        break;
      case kOutBloom: {
        static const float w[4] = {0.12f, 0.35f, 0.65f, 1.0f};
        for (int p = 0; p < 4; ++p) {
          outL[2 * p] = w[p] * 0.75f * sgnA[p];
          outR[2 * p + 1] = w[p] * 0.75f * sgnB[p];
        }
        break;
      }
      case kOutPairPlus:
        outL[0] = 0.85f;
        outR[1] = 0.85f;
        for (int p = 1; p < 4; ++p) {
          outL[2 * p] = 0.25f * sgnA[p];
          outR[2 * p + 1] = 0.25f * sgnB[p];
        }
        break;
    }
    toVec(injL, injL_); toVec(injR, injR_);
    toVec(outL, outL_); toVec(outR, outR_);

    // Diffuser lengths (ms), scaled per mode; distinct per side.
    static const float baseMs[8] = {4.77f, 3.59f, 12.73f, 9.31f, 5.21f, 3.97f, 11.83f, 8.71f};
    for (int k = 0; k < kNumDiff; ++k) {
      int l = (int)(baseMs[k] * m.diffScale * kSampleRate * 0.001f);
      if (l < 1) l = 1;
      if (l > kDiffLen - 1) l = kDiffLen - 1;
      diffLen_[k] = l;
    }
    loopLp_ = m.loopLpHz > 0.f;
    satIn_ = m.drive / kSatLevel;
    satOut_ = kSatLevel / m.drive;
    inGain_ = m.inGain;
    outGain_ = m.outGain;
    matDirty_ = true;
    dirty_ = true;
  }

  static void toVec(const float *v, hf4 *dst) {
    dst[0] = (hf4){v[0], v[1], v[2], v[3]};
    dst[1] = (hf4){v[4], v[5], v[6], v[7]};
  }

  void updateTargets() {
    dirty_ = false;
    const ModeDef &m = kModes[mode_];
    const float T = timeMs_ * kSampleRate * 0.001f;

    // Feedback: more resolution near the top without bunching all long tails there.
    // Knob 50 ~ 0.65/pass, 80 ~ 0.91, 90 ~ 0.97, 100 = mode ceiling (hold).
    const float k = 1.f - feedback_;
    const float fb = m.fbMax * (1.f - k * sqrtf(k));

    float lenT[8], gainT[8];
    float lref = 4.f;
    for (int i = 0; i < kLines; ++i) {
      const float r = m.clean[i] + warp_ * (m.warped[i] - m.clean[i]);
      float l = T * r;
      if (l < 4.f) l = 4.f;
      if (l > (float)kLineLen - kModMargin) l = (float)kLineLen - kModMargin;
      lenT[i] = l;
      if (l > lref) lref = l;
    }
    // Equal decay rate across lines: FEEDBACK is the gain per pass of the longest line.
    for (int i = 0; i < kLines; ++i)
      gainT[i] = (fb <= 0.f) ? 0.f : powf(fb, lenT[i] / lref);
    toVec(lenT, lenT_);
    toVec(gainT, gainT_);

    blendT_ = m.denseMin + density_ * (m.denseMax - m.denseMin);
    diffGT_ = m.diffMax * density_;

    // Modulation: depth rises with the knob; rate rises gently; depth backs off at high rate.
    const float rate = m.modRateHz * (0.5f + mod_);
    const float rateComp = rate > 0.6f ? 0.6f / rate : 1.f;
    const float depth = mod_ * mod_ * m.modDepthMs * kSampleRate * 0.001f * rateComp;
    // Line i gets LFO phase {s0, s1, c0, c1, -s0, -s1, -c0, -c1}[i].
    static const float spread[8] = {1.f, 0.83f, 0.91f, 0.74f, 0.97f, 0.79f, 0.88f, 0.69f};
    float md[8];
    for (int i = 0; i < kLines; ++i) {
      float dd = depth * spread[i];
      if (dd > kModMargin - 8.f) dd = kModMargin - 8.f;
      if (dd > lenT[i] - 2.f) dd = lenT[i] - 2.f;
      if (dd < 0.f) dd = 0.f;
      md[i] = dd;
    }
    toVec(md, modDepth_);
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
    fadeOutStep_ = 1.f / (0.015f * kSampleRate);  // 15 ms fade-out
    fadeInStep_ = 1.f / (0.060f * kSampleRate);   // 60 ms fade-in
  }

  void snapSmoothers() {
    for (int h = 0; h < 2; ++h) {
      len_[h] = lenT_[h];
      gain_[h] = gainT_[h];
    }
    blend_ = blendT_;
    diffG_ = diffGT_;
    mix_ = mixTarget_;
    buildMatrix();
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
  hf4 injL_[2] = {}, injR_[2] = {}, outL_[2] = {}, outR_[2] = {};
  int diffLen_[kNumDiff] = {};
  bool loopLp_ = true;
  float satIn_ = 1.f / kSatLevel, satOut_ = kSatLevel, inGain_ = 0.5f, outGain_ = 1.f;

  // Feedback matrix
  hf4 mat_[kLines][2] = {};
  float matBlend_ = -1.f;
  bool matDirty_ = true;

  // Targets + smoothed state
  hf4 lenT_[2] = {}, len_[2] = {};
  hf4 gainT_[2] = {}, gain_[2] = {};
  hf4 modDepth_[2] = {};
  float blendT_ = 0.f, blend_ = 0.f, diffGT_ = 0.f, diffG_ = 0.f, mix_ = 0.f;
  float glideCoef_ = 0.f, fadeOutStep_ = 0.f, fadeInStep_ = 0.f;
  float fade_ = 1.f, fadeTarget_ = 1.f;
  size_t lastFrames_ = 0;
  float blockSm_ = 0.f;

  // Filters + LFO
  hf4 hpZ_[2] = {}, lpZ_[2] = {};
  float loopHpA_ = 0.f, loopLpA_ = 1.f;
  float outHpZ_[2] = {}, outLpZ_[2] = {};
  float outHpA_ = 0.f, outLpA_ = 1.f;
  float lfoS_[2] = {0.f, 0.7071f}, lfoC_[2] = {1.f, 0.7071f}, lfoK_[2] = {};
};

}  // namespace horizon
