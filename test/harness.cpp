// Desktop test harness for the Horizon engine.
//
//   ./harness                 run all checks, render IRs + demos into ./out
//   ./harness in.wav out.wav [mode time fb warp density mod tone mix]
//                             process a 48 kHz WAV (16-bit or float, mono/stereo)
//
// No SDK or hardware dependencies.

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../core/horizon_engine.h"

using namespace horizon;

static const int SR = 48000;

// ---------------------------------------------------------------- WAV I/O
static bool writeWav(const std::string &path, const std::vector<float> &st) {
  FILE *f = fopen(path.c_str(), "wb");
  if (!f) return false;
  const uint32_t frames = st.size() / 2, dataBytes = frames * 4;
  auto w32 = [&](uint32_t v) { fwrite(&v, 4, 1, f); };
  auto w16 = [&](uint16_t v) { fwrite(&v, 2, 1, f); };
  fwrite("RIFF", 1, 4, f); w32(36 + dataBytes); fwrite("WAVEfmt ", 1, 8, f);
  w32(16); w16(1); w16(2); w32(SR); w32(SR * 4); w16(4); w16(16);
  fwrite("data", 1, 4, f); w32(dataBytes);
  for (float s : st) {
    float c = s > 0.999f ? 0.999f : (s < -0.999f ? -0.999f : s);
    int16_t v = (int16_t)lrintf(c * 32767.f);
    fwrite(&v, 2, 1, f);
  }
  fclose(f);
  return true;
}

static bool readWav(const std::string &path, std::vector<float> &st) {
  FILE *f = fopen(path.c_str(), "rb");
  if (!f) return false;
  char id[4]; uint32_t sz;
  fread(id, 1, 4, f); fread(&sz, 4, 1, f); fread(id, 1, 4, f);
  uint16_t fmt = 0, ch = 0, bits = 0; uint32_t rate = 0;
  while (fread(id, 1, 4, f) == 4 && fread(&sz, 4, 1, f) == 1) {
    if (!memcmp(id, "fmt ", 4)) {
      std::vector<uint8_t> b(sz); fread(b.data(), 1, sz, f);
      memcpy(&fmt, &b[0], 2); memcpy(&ch, &b[2], 2); memcpy(&rate, &b[4], 4); memcpy(&bits, &b[14], 2);
    } else if (!memcmp(id, "data", 4)) {
      if (rate != (uint32_t)SR) fprintf(stderr, "warning: %u Hz input, engine runs at 48000\n", rate);
      const size_t n = sz / (bits / 8);
      std::vector<float> raw(n);
      for (size_t i = 0; i < n; ++i) {
        if (fmt == 3 && bits == 32) { float v; fread(&v, 4, 1, f); raw[i] = v; }
        else if (bits == 16) { int16_t v; fread(&v, 2, 1, f); raw[i] = v / 32768.f; }
        else if (bits == 24) { uint8_t b[3]; fread(b, 1, 3, f); int32_t v = (b[0] << 8 | b[1] << 16 | b[2] << 24) >> 8; raw[i] = v / 8388608.f; }
        else { fclose(f); fprintf(stderr, "unsupported WAV format\n"); return false; }
      }
      const size_t frames = n / ch;
      st.resize(frames * 2);
      for (size_t i = 0; i < frames; ++i) {
        st[2 * i] = raw[i * ch];
        st[2 * i + 1] = raw[i * ch + (ch > 1 ? 1 : 0)];
      }
      fclose(f);
      return true;
    } else {
      fseek(f, sz + (sz & 1), SEEK_CUR);
    }
  }
  fclose(f);
  return false;
}

// ---------------------------------------------------------------- helpers
struct Settings {
  int mode; float timeMs, fb, warp, density, mod, tone, mix;
};

static void apply(Engine &e, const Settings &s) {
  e.setMode(s.mode); e.setTimeMs(s.timeMs); e.setFeedback(s.fb); e.setWarp(s.warp);
  e.setDensity(s.density); e.setMod(s.mod); e.setTone(s.tone); e.setMix(s.mix);
}

// Fresh engine at given settings with mode already active (no crossfade).
static std::vector<int16_t> g_mem(kMemSamples);
static void fresh(Engine &e, const Settings &s) {
  std::fill(g_mem.begin(), g_mem.end(), 0);
  e.init(g_mem.data());
  apply(e, s);
  // let mode crossfade + smoothers settle on silence
  std::vector<float> z(2 * 64, 0.f), o(2 * 64);
  for (int i = 0; i < 300; ++i) e.process(z.data(), o.data(), 64);
  std::fill(g_mem.begin(), g_mem.end(), 0);
}

static void run(Engine &e, const std::vector<float> &in, std::vector<float> &out) {
  out.resize(in.size());
  const size_t frames = in.size() / 2;
  for (size_t p = 0; p < frames; p += 64) {
    const size_t n = std::min<size_t>(64, frames - p);
    e.process(&in[2 * p], &out[2 * p], n);
  }
}

static bool finiteAll(const std::vector<float> &v) {
  for (float x : v) if (!std::isfinite(x)) return false;
  return true;
}

static float peak(const std::vector<float> &v) {
  float p = 0; for (float x : v) p = std::max(p, std::fabs(x)); return p;
}

// Time (s) until the 50 ms RMS envelope falls 60 dB below its max. -1 if never.
static float t60(const std::vector<float> &v) {
  const size_t win = SR / 20, frames = v.size() / 2;
  std::vector<float> env;
  for (size_t p = 0; p + win <= frames; p += win) {
    double e = 0; for (size_t i = p; i < p + win; ++i) e += v[2 * i] * v[2 * i] + v[2 * i + 1] * v[2 * i + 1];
    env.push_back((float)std::sqrt(e / (2 * win)));
  }
  float mx = 0; size_t mi = 0;
  for (size_t i = 0; i < env.size(); ++i) if (env[i] > mx) { mx = env[i]; mi = i; }
  for (size_t i = mi; i < env.size(); ++i) if (env[i] < mx * 0.001f) return i * 0.05f;
  return -1.f;
}

// Time (s) of the RMS envelope peak — a proxy for "attack".
static float tPeak(const std::vector<float> &v) {
  const size_t win = SR / 50, frames = v.size() / 2;
  float mx = 0, at = 0;
  for (size_t p = 0; p + win <= frames; p += win) {
    double e = 0; for (size_t i = p; i < p + win; ++i) e += v[2 * i] * v[2 * i] + v[2 * i + 1] * v[2 * i + 1];
    if (e > mx) { mx = (float)e; at = p / (float)SR; }
  }
  return at;
}

static float dcOffset(const std::vector<float> &v, size_t fromFrame) {
  double s = 0; size_t n = 0;
  for (size_t i = fromFrame; i < v.size() / 2; ++i) { s += v[2 * i] + v[2 * i + 1]; n += 2; }
  return n ? (float)(s / n) : 0.f;
}

static float lrCorr(const std::vector<float> &v) {
  double l2 = 0, r2 = 0, lr = 0;
  for (size_t i = 0; i < v.size() / 2; ++i) { l2 += v[2*i]*v[2*i]; r2 += v[2*i+1]*v[2*i+1]; lr += v[2*i]*v[2*i+1]; }
  return (l2 > 0 && r2 > 0) ? (float)(lr / std::sqrt(l2 * r2)) : 0.f;
}

// Test material: chord stabs + noise snare + sub hit, 4 s then silence.
static std::vector<float> demoInput(float seconds) {
  const size_t frames = (size_t)(seconds * SR);
  std::vector<float> v(frames * 2, 0.f);
  uint32_t rng = 12345;
  auto noise = [&]() { rng = rng * 1664525u + 1013904223u; return ((rng >> 9) / 4194304.f) - 1.f; };
  const float chord[3] = {220.f, 261.63f, 329.63f};
  for (int hit = 0; hit < 4; ++hit) {
    const size_t t0 = (size_t)(hit * 0.75f * SR);
    for (size_t i = 0; i < SR / 3 && t0 + i < frames; ++i) {
      const float t = i / (float)SR;
      float s = 0;
      for (float f : chord) s += 0.18f * (2.f * std::fmod(f * t, 1.f) - 1.f);
      s *= std::exp(-t * 9.f);
      if (hit % 2) s += 0.5f * noise() * std::exp(-t * 25.f);          // snare-ish
      else s += 0.6f * std::sin(2 * 3.14159f * 55.f * t) * std::exp(-t * 6.f);  // sub
      v[2 * (t0 + i)] += s;
      v[2 * (t0 + i) + 1] += s;
    }
  }
  return v;
}

static Settings defaults(int mode) {
  // Sensible "massive" starting points per mode (also used in the README).
  static const Settings s[kNumModes] = {
    {kModeRoom,     300, 0.50f, 0.70f, 0.90f, 0.30f,  0.00f, 0.35f},
    {kModeHall,     700, 0.75f, 0.80f, 0.90f, 0.35f, -0.10f, 0.40f},
    {kModeHalo,     500, 0.70f, 0.55f, 0.45f, 0.50f,  0.00f, 0.40f},
    {kModeSwell,    600, 0.85f, 0.70f, 0.70f, 0.40f, -0.15f, 0.50f},
    {kModeAbyss,   1000, 0.90f, 0.80f, 0.85f, 0.45f, -0.25f, 0.50f},
    {kModeEchoVerb, 375, 0.65f, 0.60f, 0.50f, 0.25f, -0.20f, 0.35f},
    {kModeOrbit,    750, 0.75f, 0.50f, 0.20f, 0.20f,  0.00f, 0.40f},
    {kModePrism,    600, 0.65f, 0.30f, 0.20f, 0.20f,  0.00f, 0.40f},
    {kModePong,     375, 0.60f, 0.00f, 0.15f, 0.15f, -0.10f, 0.35f},
    {kModeDub,      375, 0.75f, 0.00f, 0.10f, 0.35f, -0.20f, 0.40f},
  };
  return s[mode];
}

// ---------------------------------------------------------------- checks
static int g_fail = 0;
#define CHECK(cond, ...) do { if (!(cond)) { ++g_fail; printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void runAll(const std::string &outDir) {
  Engine e;
  std::vector<float> out;

  printf("Memory: %zu int16 samples = %zu bytes (limit 1048576)\n", kMemSamples, kMemBytes);
  CHECK(kMemBytes <= 0x100000, "memory over 1 MB");

  // 1) Impulse responses + demo renders per mode, wet only.
  printf("\n%-9s %8s %8s %8s %8s %8s %8s\n", "mode", "IRpeak", "IR rms", "attack", "T60(s)", "L/R corr", "demoPk");
  for (int m = 0; m < kNumModes; ++m) {
    Settings s = defaults(m);
    s.mix = 1.f;
    fresh(e, s);
    std::vector<float> imp((size_t)SR * 12 * 2, 0.f);
    imp[0] = imp[1] = 1.f;
    run(e, imp, out);
    double rms = 0; for (float x : out) rms += x * x; rms = std::sqrt(rms / out.size());
    const float pk = peak(out), at = tPeak(out), tt = t60(out), corr = lrCorr(out);
    CHECK(finiteAll(out), "%s IR non-finite", kModes[m].name);
    writeWav(outDir + "/ir_" + kModes[m].name + ".wav", out);

    Settings d = defaults(m);
    fresh(e, d);
    std::vector<float> demo = demoInput(12.f);
    run(e, demo, out);
    const float dpk = peak(out);
    CHECK(finiteAll(out), "%s demo non-finite", kModes[m].name);
    CHECK(dpk < 2.5f, "%s demo too hot (%.2f)", kModes[m].name, dpk);
    writeWav(outDir + "/demo_" + kModes[m].name + ".wav", out);

    printf("%-9s %8.3f %8.4f %8.2f %8.2f %8.2f %8.2f\n", kModes[m].name, pk, rms, at, tt, corr, dpk);
  }
  std::vector<float> dry = demoInput(12.f);
  writeWav(outDir + "/demo__dry.wav", dry);

  // 2) Stability: every mode, extreme corners, 40 s of loud noise then silence.
  printf("\nStability (extreme settings, 20 s loud noise + 20 s silence):\n");
  uint32_t rng = 777;
  auto noise = [&]() { rng = rng * 1664525u + 1013904223u; return ((rng >> 9) / 4194304.f) - 1.f; };
  std::vector<float> loud((size_t)SR * 40 * 2, 0.f);
  for (size_t i = 0; i < (size_t)SR * 20 * 2; ++i) loud[i] = 0.9f * noise();
  const float corners[][7] = {
    // time, fb, warp, dens, mod, tone, mix
    {10, 1, 0, 0, 1, 1, 1}, {10, 1, 1, 1, 1, -1, 1}, {1250, 1, 0, 0, 1, -1, 1},
    {1250, 1, 1, 1, 1, 1, 1}, {1250, 1, 0.5f, 0.5f, 0, 0, 1}, {200, 1, 0.2f, 1, 0.7f, 0, 1},
  };
  for (int m = 0; m < kNumModes; ++m) {
    float worst = 0, worstDc = 0;
    bool ok = true;
    for (auto &c : corners) {
      Settings s = {m, c[0], c[1], c[2], c[3], c[4], c[5], c[6]};
      fresh(e, s);
      run(e, loud, out);
      ok = ok && finiteAll(out);
      worst = std::max(worst, peak(out));
      worstDc = std::max(worstDc, std::fabs(dcOffset(out, (size_t)SR * 25)));
    }
    printf("  %-9s finite=%s  maxPeak=%.2f  maxDC=%.4f\n", kModes[m].name, ok ? "yes" : "NO", worst, worstDc);
    CHECK(ok, "%s produced NaN/inf", kModes[m].name);
    CHECK(worst < 4.f, "%s runaway output %.2f", kModes[m].name, worst);
    CHECK(worstDc < 0.02f, "%s DC buildup %.4f", kModes[m].name, worstDc);
  }

  // 3) Sustain: FEEDBACK 100 in sustain modes should hold the tail.
  printf("\nSustain at FEEDBACK 100 (level 10 s after input stops vs. 1 s after):\n");
  for (int m : {kModeAbyss, kModeOrbit, kModeDub, kModeHall}) {
    Settings s = defaults(m); s.fb = 1.f; s.mix = 1.f;
    fresh(e, s);
    std::vector<float> in = demoInput(14.f);
    run(e, in, out);
    auto rmsAt = [&](float t0) {
      double r = 0;
      const size_t a = (size_t)(t0 * SR), b = a + SR / 2;
      for (size_t i = a; i < b; ++i) r += out[2*i]*out[2*i] + out[2*i+1]*out[2*i+1];
      return std::sqrt(r / SR);
    };
    const float r1 = (float)rmsAt(4.f), r10 = (float)rmsAt(13.f);
    const float db = 20 * std::log10((r10 + 1e-9f) / (r1 + 1e-9f));
    printf("  %-9s %.4f -> %.4f (%.1f dB)\n", kModes[m].name, r1, r10, db);
    if (m == kModeOrbit) CHECK(db > -3.f, "Orbit should hold at FEEDBACK 100 (%.1f dB)", db);
  }

  // 4) Automation torture: random param jumps every block, mode switches.
  printf("\nAutomation: random parameter/mode changes every block for 60 s...\n");
  {
    fresh(e, defaults(kModeHall));
    std::vector<float> in(2 * 64), o(2 * 64);
    float pk = 0; bool ok = true; double maxStep = 0; float prev = 0;
    for (int b = 0; b < 60 * SR / 64; ++b) {
      for (int i = 0; i < 128; ++i) in[i] = 0.5f * noise();
      if (b % 3 == 0) {
        rng = rng * 1664525u + 1013904223u;
        switch (rng % 8) {
          case 0: e.setTimeMs(10 + (rng >> 8) % 1240); break;
          case 1: e.setFeedback(((rng >> 8) % 1000) / 1000.f); break;
          case 2: e.setWarp(((rng >> 8) % 1000) / 1000.f); break;
          case 3: e.setDensity(((rng >> 8) % 1000) / 1000.f); break;
          case 4: e.setMod(((rng >> 8) % 1000) / 1000.f); break;
          case 5: e.setTone(((rng >> 8) % 2000) / 1000.f - 1.f); break;
          case 6: if (b % 300 == 0) e.setMode((rng >> 8) % kNumModes); break;
          case 7: e.setMix(((rng >> 8) % 2000) / 1000.f - 1.f); break;
        }
      }
      e.process(in.data(), o.data(), 64);
      for (int i = 0; i < 128; ++i) {
        if (!std::isfinite(o[i])) ok = false;
        pk = std::max(pk, std::fabs(o[i]));
        if (i % 2 == 0) { maxStep = std::max(maxStep, (double)std::fabs(o[i] - prev)); prev = o[i]; }
      }
    }
    printf("  finite=%s  peak=%.2f\n", ok ? "yes" : "NO", pk);
    CHECK(ok, "automation produced NaN/inf");
    CHECK(pk < 4.f, "automation runaway %.2f", pk);
  }

  // 5) Mode-switch click check: steady tone, switch mode, look at the jump size.
  printf("\nMode switch smoothness (max sample-to-sample jump during switch, wet 100%%):\n");
  {
    Settings s = defaults(kModeHall); s.mix = 1.f;
    fresh(e, s);
    const size_t N = SR * 3;
    std::vector<float> in(2 * N);
    for (size_t i = 0; i < N; ++i) in[2*i] = in[2*i+1] = 0.3f * std::sin(2 * 3.14159f * 220.f * i / SR);
    std::vector<float> o(2 * N);
    // Compare the switch window against the steadiest of the two modes' own
    // sample-to-sample movement, so a louder target mode isn't mistaken for a click.
    float before = 0, during = 0, after = 0;
    const size_t sw = (size_t)SR * 3 / 2;
    for (size_t p = 0; p < N; p += 64) {
      if (p == sw) e.setMode(kModeOrbit);
      e.process(&in[2*p], &o[2*p], 64);
    }
    for (size_t i = 1; i < N; ++i) {
      const float j = std::fabs(o[2*i] - o[2*(i-1)]);
      if (i < sw) before = std::max(before, j);
      else if (i < sw + SR / 10) during = std::max(during, j);
      else if (i > N - SR / 2) after = std::max(after, j);
    }
    const float ref = std::max(before, after);
    printf("  old mode=%.4f  switch window=%.4f  new mode=%.4f\n", before, during, after);
    CHECK(during < ref * 1.5f + 0.005f, "mode switch click (%.4f vs %.4f)", during, ref);
  }

  // 6) CPU (desktop x86 — only a relative sanity check, NOT the A7 figure).
  printf("\nCPU (this machine, -O2, per mode, 20 s of audio):\n");
  for (int m = 0; m < kNumModes; ++m) {
    Settings s = defaults(m);
    fresh(e, s);
    std::vector<float> in((size_t)SR * 20 * 2), o(in.size());
    for (float &x : in) x = 0.3f * noise();
    auto t0 = std::chrono::steady_clock::now();
    run(e, in, o);
    auto t1 = std::chrono::steady_clock::now();
    const double sec = std::chrono::duration<double>(t1 - t0).count();
    printf("  %-9s %6.1f ns/sample  (%.2f%% of realtime)\n", kModes[m].name, sec * 1e9 / (SR * 20.0), 100.0 * sec / 20.0);
  }

  printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "ALL CHECKS PASSED", g_fail, g_fail == 1 ? "" : "s");
}

int main(int argc, char **argv) {
  if (argc >= 3) {
    std::vector<float> in, out;
    if (!readWav(argv[1], in)) { fprintf(stderr, "can't read %s\n", argv[1]); return 1; }
    Settings s = defaults(kModeHall);
    float *f[] = {nullptr, &s.timeMs, &s.fb, &s.warp, &s.density, &s.mod, &s.tone, &s.mix};
    if (argc > 3) s = defaults(atoi(argv[3]) % kNumModes);
    for (int i = 4; i < argc && i - 3 < 8; ++i) *f[i - 3] = (float)atof(argv[i]);
    in.resize(in.size() + (size_t)SR * 2 * 8, 0.f);  // 8 s tail
    Engine e;
    fresh(e, s);
    run(e, in, out);
    if (!writeWav(argv[2], out)) { fprintf(stderr, "can't write %s\n", argv[2]); return 1; }
    printf("wrote %s (mode %s)\n", argv[2], kModes[s.mode].name);
    return 0;
  }
  const std::string outDir = argc == 2 ? argv[1] : "out";
  std::string cmd = "mkdir -p " + outDir;
  if (system(cmd.c_str()) != 0) return 1;
  runAll(outDir);
  return g_fail ? 1 : 0;
}
