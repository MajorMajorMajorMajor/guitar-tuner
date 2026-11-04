#include "pitch.h"

// Overview
// --------
// 1. Decimate the latest 64 ms from 16 kHz to 8 kHz with a [1 2 1] low-pass
//    and run YIN (difference function, cumulative-mean normalisation,
//    absolute threshold) over the whole lag range. This finds the period
//    cheaply and robustly.
// 2. Re-evaluate the raw difference function at 16 kHz, over up to 128 ms, on
//    the few lags around that period and interpolate the minimum. This gives
//    the sub-sample precision a tuner needs (well under 1 cent).
//
// Cost per estimate is roughly 65k integer multiply-accumulates.

#define DECIMATION 2
#define RATE8 (PITCH_SAMPLE_RATE / DECIMATION)
#define N8 (PITCH_COARSE_WINDOW / DECIMATION)
#define TAU8_MIN (RATE8 / PITCH_MAX_HZ)
#define TAU8_MAX (RATE8 / PITCH_MIN_HZ + 2)
// Coarse integration window. Must satisfy W8 + TAU8_MAX + 1 <= N8.
#define W8 (N8 - TAU8_MAX - 1)

// A lag whose normalised difference falls below this is accepted as periodic.
// With additive noise d'(T) ~ 1 / (1 + SNR), so 0.12 needs roughly 9 dB of
// in-band SNR; below that the estimate wanders by several cents and it is
// better to show nothing.
#define YIN_THRESHOLD 0.12f
// If the dip at twice the chosen lag is this much deeper, the chosen lag was
// a strong 2nd harmonic rather than the fundamental (common when the
// fundamental is weak, e.g. a low string through a small microphone).
#define OCTAVE_RATIO 0.35f
// ...unless the chosen dip is already this clean, in which case any deeper
// dip at twice the lag is just noise or lag quantisation...
#define OCTAVE_FLOOR 0.02f
// ...and the dip at twice the lag must be clean in absolute terms, so noise
// near the threshold cannot push a note down an octave.
#define OCTAVE_MAX_DEPTH 0.05f
// Only candidates below ~670 Hz are checked: a weak fundamental is a low-note
// problem, and dips only a few 8 kHz lags wide are too coarse to compare.
#define OCTAVE_MIN_TAU 12
// Refinement searches +/- this many 16 kHz lags around the coarse estimate.
#define REFINE_SPAN 2

static int32_t s_dec[N8];
static int64_t s_diff[TAU8_MAX + 2];
static float s_cmnd[TAU8_MAX + 2];

static uint32_t isqrt64(uint64_t v) {
  uint64_t res = 0;
  uint64_t bit = (uint64_t)1 << 62;
  while (bit > v) {
    bit >>= 2;
  }
  while (bit != 0) {
    if (v >= res + bit) {
      v -= res + bit;
      res = (res >> 1) + bit;
    } else {
      res >>= 1;
    }
    bit >>= 2;
  }
  return (uint32_t)res;
}

// Offset of the vertex of the parabola through (-1, a), (0, b), (1, c).
static float parabolic_offset(int64_t a, int64_t b, int64_t c) {
  const int64_t den = a - 2 * b + c;
  if (den <= 0) {
    return 0.0f;
  }
  float offset = 0.5f * (float)(a - c) / (float)den;
  if (offset > 0.5f) {
    offset = 0.5f;
  } else if (offset < -0.5f) {
    offset = -0.5f;
  }
  return offset;
}

// Like parabolic_offset, but for a dip shaped like 1 - cos(w (x - offset))
// with w = 2 pi / period, which is what the difference function of a
// near-sinusoid looks like. The parabola is its small-w limit and is biased by
// up to ~0.8 cent once a period spans only ~12 samples (top of the range):
//   tan(w offset) = tan(w / 2) (a - c) / (a - 2b + c)
static float atan_small(float x) {  // |x| <= ~0.4
  const float x2 = x * x;
  return x * (1.0f - x2 * (1.0f / 3.0f - x2 * (1.0f / 5.0f - x2 * (1.0f / 7.0f))));
}
static float tan_small(float x) {  // |x| <= ~0.3
  const float x2 = x * x;
  return x * (1.0f + x2 * (1.0f / 3.0f + x2 * (2.0f / 15.0f + x2 * (17.0f / 315.0f))));
}
static float cosine_offset(int64_t a, int64_t b, int64_t c, float period) {
  const int64_t den = a - 2 * b + c;
  if (den <= 0 || period < 10.0f) {
    return parabolic_offset(a, b, c);
  }
  const float w = 6.28318531f / period;
  float t = tan_small(0.5f * w) * (float)(a - c) / (float)den;
  t = t > 0.4f ? 0.4f : t < -0.4f ? -0.4f : t;
  float offset = atan_small(t) / w;
  return offset > 0.5f ? 0.5f : offset < -0.5f ? -0.5f : offset;
}

// Index of the local minimum of s_cmnd reached by walking downhill from tau.
static int descend(int tau) {
  while (tau + 1 <= TAU8_MAX && s_cmnd[tau + 1] < s_cmnd[tau]) {
    tau++;
  }
  while (tau - 1 >= TAU8_MIN && s_cmnd[tau - 1] < s_cmnd[tau]) {
    tau--;
  }
  return tau;
}

static bool is_local_min(int tau) {
  return s_cmnd[tau] <= s_cmnd[tau - 1] && s_cmnd[tau] <= s_cmnd[tau + 1];
}

// Depth of the dip at tau, read off the parabola through its neighbours. On
// short periods the true minimum often falls between two 8 kHz lags, so the
// sampled value alone overstates the aperiodicity.
static float dip_depth(int tau) {
  const float a = s_cmnd[tau - 1];
  const float b = s_cmnd[tau];
  const float c = s_cmnd[tau + 1];
  const float den = a - 2.0f * b + c;
  if (den <= 0.0f) {
    return b;
  }
  const float v = b - (a - c) * (a - c) / (8.0f * den);
  return v < 0.0f ? 0.0f : v;
}

static int64_t diff16(const int16_t *x, int lag, int width) {
  int64_t acc = 0;
  const int16_t *a = x;
  const int16_t *b = x + lag;
  for (int j = 0; j < width; j++) {
    const int32_t d = (int32_t)a[j] - (int32_t)b[j];
    acc += (int64_t)d * d;
  }
  return acc;
}

void pitch_estimate(const int16_t *samples, int count, int32_t min_rms, PitchEstimate *out) {
  out->hz = 0.0f;
  out->clarity = 0.0f;
  out->rms = 0;
  if (count < PITCH_COARSE_WINDOW || count > PITCH_WINDOW) {
    return;
  }
  const int16_t *x = samples + count - PITCH_COARSE_WINDOW;

  // Level gate on the latest 64 ms.
  int64_t sum = 0;
  for (int i = 0; i < PITCH_COARSE_WINDOW; i++) {
    sum += x[i];
  }
  const int32_t mean = (int32_t)(sum / PITCH_COARSE_WINDOW);
  uint64_t energy = 0;
  for (int i = 0; i < PITCH_COARSE_WINDOW; i++) {
    const int32_t v = (int32_t)x[i] - mean;
    energy += (uint64_t)((int64_t)v * v);
  }
  out->rms = (int32_t)isqrt64(energy / PITCH_COARSE_WINDOW);
  if (out->rms < min_rms) {
    return;
  }

  // 1. Decimate to 8 kHz (scaled by 4; the scale cancels in every ratio).
  for (int i = 0; i < N8; i++) {
    const int k = i * DECIMATION;
    const int32_t prev = x[k > 0 ? k - 1 : 0];
    const int32_t next = x[k + 1 < PITCH_COARSE_WINDOW ? k + 1 : PITCH_COARSE_WINDOW - 1];
    s_dec[i] = prev + 2 * (int32_t)x[k] + next;
  }

  // Difference function d(tau) and its cumulative-mean normalisation d'(tau).
  s_diff[0] = 0;
  s_cmnd[0] = 1.0f;
  float running = 0.0f;
  for (int tau = 1; tau <= TAU8_MAX + 1; tau++) {
    int64_t acc = 0;
    const int32_t *a = s_dec;
    const int32_t *b = s_dec + tau;
    for (int j = 0; j < W8; j++) {
      const int32_t d = a[j] - b[j];
      acc += (int64_t)d * d;
    }
    s_diff[tau] = acc;
    running += (float)acc;
    s_cmnd[tau] = running > 0.0f ? (float)acc * (float)tau / running : 1.0f;
  }

  // Absolute threshold: the first dip (shortest period) that is periodic
  // enough.
  int tau = -1;
  for (int t = TAU8_MIN + 1; t < TAU8_MAX; t++) {
    if (is_local_min(t) && dip_depth(t) < YIN_THRESHOLD) {
      tau = t;
      break;
    }
  }
  if (tau < 0) {
    return;
  }

  // Prefer the sub-octave when it is far more periodic: the chosen dip was a
  // strong 2nd harmonic and the fundamental is weak (missed fundamental).
  while (tau >= OCTAVE_MIN_TAU && 2 * tau + 2 < TAU8_MAX) {
    const float here = dip_depth(tau);
    const int twice = descend(2 * tau);
    const float there = dip_depth(twice);
    if (here < OCTAVE_FLOOR || twice < 2 * tau - 2 || twice > 2 * tau + 2 ||
        there >= OCTAVE_RATIO * here || there >= OCTAVE_MAX_DEPTH) {
      break;
    }
    tau = twice;
  }

  const float clarity = 1.0f - dip_depth(tau);
  const float coarse = (float)tau + parabolic_offset(s_diff[tau - 1], s_diff[tau], s_diff[tau + 1]);

  // 2. Refine at 16 kHz around the coarse period, over all `count` samples.
  const int center = (int)(coarse * DECIMATION + 0.5f);
  const int lo = center - REFINE_SPAN - 1;
  const int hi = center + REFINE_SPAN + 1;
  const int width = count - hi - 1;
  int64_t d16[2 * REFINE_SPAN + 3];
  for (int lag = lo; lag <= hi; lag++) {
    d16[lag - lo] = diff16(samples, lag, width);
  }
  int best = 1;
  for (int i = 2; i <= 2 * REFINE_SPAN + 1; i++) {
    if (d16[i] < d16[best]) {
      best = i;
    }
  }
  const float period = (float)(lo + best) + cosine_offset(d16[best - 1], d16[best], d16[best + 1],
                                                          (float)(lo + best));
  if (period <= 0.0f) {
    return;
  }

  out->hz = (float)PITCH_SAMPLE_RATE / period;
  out->clarity = clarity < 0.0f ? 0.0f : clarity;
}
