#include "tuner.h"

#include <string.h>

#include "notes.h"

// Smoothing factor of the displayed pitch (per 40 ms estimate).
#define SMOOTHING 0.25f
// Readings below CLARITY_FULL move the display proportionally less, down to
// CLARITY_MIN_WEIGHT at the detector's threshold: as a note decays into the
// noise its estimates wander, and the needle should settle, not follow them.
#define CLARITY_FULL 0.96f
#define CLARITY_MIN_WEIGHT 0.08f
// A median that moves further than this (in semitones) is a new note: jump
// straight to it instead of gliding.
#define JUMP_SEMITONES 0.5f
// Stay on the current note until the pitch is this far from it (semitones),
// so a note right on the +/-50 cent boundary does not flicker.
#define NOTE_HYSTERESIS 0.55f
// Clear the median history after this many misses so a new pluck starts fresh.
#define HISTORY_RESET_MISSES 3
// For the first estimates of a new note the display follows faster, so it
// settles on the note within ~0.2 s rather than easing in from the attack.
#define SETTLE_ESTIMATES 4
#define SETTLE_SMOOTHING 0.6f
// Showing a new note (or jumping away from the current one) takes one clean
// estimate, or two agreeing ones (within CONFIRM_SEMITONES) of at least the
// sensitivity's confirm_clarity. Anything less, typical of a note decaying
// into noise, can only nudge the current note; otherwise one stray
// sub-harmonic would show up as a wrong note.
#define CLARITY_SURE 0.92f
#define CONFIRM_SEMITONES 0.3f

// Per sensitivity: level gate, detector threshold, confirm clarity. Low is
// the original tuning (needs ~11 dB SNR to start a note); Normal starts one
// ~2 dB and High ~4 dB lower, and both keep a decaying note longer.
static const struct {
  int32_t min_rms;
  float threshold;
  float confirm_clarity;
} k_sensitivity[] = {
    [TUNER_SENSITIVITY_LOW] = {30, 0.12f, CLARITY_SURE},
    [TUNER_SENSITIVITY_NORMAL] = {12, 0.15f, 0.88f},
    [TUNER_SENSITIVITY_HIGH] = {5, 0.20f, 0.84f},
};

// Pre-filter, in Q6 fixed point:
// - one-pole DC/rumble blocker, y = x - x1 + R y1, R = 0.985 (~38 Hz)
// - two one-pole low-passes, s += k (in - s), k = 0.5 (~1.8 kHz each), which
//   remove hiss above the guitar's range and tame upper partials so the
//   difference function's minimum is smooth for the sub-sample fit.
#define FILTER_SHIFT 6
#define HP_R_Q15 32276
#define LP_K_Q8 128

static int16_t prefilter(Tuner *t, int16_t sample) {
  const int32_t x = (int32_t)sample * (1 << FILTER_SHIFT);
  const int32_t y = x - t->hp_x1 + (int32_t)(((int64_t)t->hp_y1 * HP_R_Q15) >> 15);
  t->hp_x1 = x;
  t->hp_y1 = y;
  t->lp1 += ((y - t->lp1) * LP_K_Q8) >> 8;
  t->lp2 += ((t->lp1 - t->lp2) * LP_K_Q8) >> 8;
  const int32_t out = t->lp2 >> FILTER_SHIFT;
  return (int16_t)(out > 32767 ? 32767 : out < -32768 ? -32768 : out);
}

void tuner_set_sensitivity(Tuner *tuner, TunerSensitivity sensitivity) {
  if ((unsigned)sensitivity > TUNER_SENSITIVITY_HIGH) {
    sensitivity = TUNER_SENSITIVITY_NORMAL;
  }
  tuner->pitch.min_rms = k_sensitivity[sensitivity].min_rms;
  tuner->pitch.threshold = k_sensitivity[sensitivity].threshold;
  tuner->confirm_clarity = k_sensitivity[sensitivity].confirm_clarity;
}

void tuner_init(Tuner *tuner, TunerSensitivity sensitivity) {
  memset(tuner, 0, sizeof(*tuner));
  tuner->span = PITCH_COARSE_WINDOW;
  tuner_set_sensitivity(tuner, sensitivity);
}

static float median3(const float *v, int n) {
  if (n == 1) {
    return v[0];
  }
  if (n == 2) {
    return 0.5f * (v[0] + v[1]);
  }
  const float a = v[0];
  const float b = v[1];
  const float c = v[2];
  if ((a <= b && b <= c) || (c <= b && b <= a)) {
    return b;
  }
  if ((b <= a && a <= c) || (c <= a && a <= b)) {
    return a;
  }
  return c;
}

static float absf(float v) {
  return v < 0.0f ? -v : v;
}

static void miss(Tuner *tuner) {
  TunerReading *r = &tuner->reading;
  r->fresh = false;
  tuner->misses++;
  if (tuner->misses >= HISTORY_RESET_MISSES) {
    tuner->history_len = 0;
  }
  if (tuner->misses >= TUNER_HOLD) {
    r->active = false;
    tuner->span = PITCH_COARSE_WINDOW;
  }
}

// `est` was measured over tuner->span samples; `recent` re-measures the latest
// PITCH_COARSE_WINDOW only, for when the longer span may straddle a note change.
static void update(Tuner *tuner, PitchEstimate *est, void (*recent)(Tuner *, PitchEstimate *)) {
  TunerReading *r = &tuner->reading;
  r->rms = est->rms;

  if (est->hz <= 0.0f) {
    miss(tuner);
    return;
  }

  // Median of the last three estimates, including this one.
  float history[3];
  int len = tuner->history_len;
  memcpy(history, tuner->history, sizeof(history));
  if (len < 3) {
    history[len++] = notes_hz_to_midi(est->hz);
  } else {
    history[0] = history[1];
    history[1] = history[2];
    history[2] = notes_hz_to_midi(est->hz);
  }
  const float median = median3(history, len);

  const bool jump = !r->active || absf(median - tuner->smooth) > JUMP_SEMITONES;
  if (jump && est->clarity < CLARITY_SURE) {
    const float raw = history[len - 1];
    const bool confirmed = tuner->has_pending && absf(raw - tuner->pending) < CONFIRM_SEMITONES;
    if (est->clarity < tuner->confirm_clarity || !confirmed) {
      // Too marginal to change the note on its own: drop it, but remember
      // it in case the next estimate agrees.
      tuner->pending = raw;
      tuner->has_pending = est->clarity >= tuner->confirm_clarity;
      miss(tuner);
      return;
    }
  }
  tuner->has_pending = false;
  if (jump && tuner->span > PITCH_COARSE_WINDOW) {
    // A new note: measure it only over audio that follows the change.
    tuner->span = PITCH_COARSE_WINDOW;
    recent(tuner, est);
    if (est->hz <= 0.0f) {
      miss(tuner);
      return;
    }
    history[len - 1] = notes_hz_to_midi(est->hz);
  }
  memcpy(tuner->history, history, sizeof(history));
  tuner->history_len = len;

  if (jump) {
    tuner->smooth = history[len - 1];
    tuner->settle = SETTLE_ESTIMATES;
  } else {
    const float clarity_low = 1.0f - tuner->pitch.threshold;
    float weight = (est->clarity - clarity_low) / (CLARITY_FULL - clarity_low);
    weight = weight > 1.0f ? 1.0f : weight < CLARITY_MIN_WEIGHT ? CLARITY_MIN_WEIGHT : weight;
    const float alpha = tuner->settle > 0 ? SETTLE_SMOOTHING : SMOOTHING;
    if (tuner->settle > 0) {
      tuner->settle--;
    }
    tuner->smooth += alpha * weight * (median - tuner->smooth);
  }
  // Measure the note over a longer span as it continues.
  tuner->span += TUNER_HOP;
  if (tuner->span > PITCH_WINDOW) {
    tuner->span = PITCH_WINDOW;
  }

  const int nearest = notes_round(tuner->smooth);
  if (!r->active || absf(tuner->smooth - (float)r->midi) > NOTE_HYSTERESIS) {
    r->midi = nearest;
  }

  r->cents = (tuner->smooth - (float)r->midi) * 100.0f;
  r->target_hz = notes_midi_to_hz(r->midi);
  // Report the smoothed pitch so the Hz readout is as steady as the cents:
  // hz = target * 2^(cents / 1200), via exp(x) to 3rd order (|x| < 0.04).
  const float x = r->cents * (0.69314718f / 1200.0f);
  r->hz = r->target_hz * (1.0f + x * (1.0f + x * (0.5f + x * (1.0f / 6.0f))));
  r->clarity = est->clarity;
  r->active = true;
  r->fresh = true;
  tuner->misses = 0;
}

static void measure(Tuner *tuner, PitchEstimate *est) {
  const int count = tuner->span < tuner->fill ? tuner->span : tuner->fill;
  pitch_estimate(tuner->window + tuner->fill - count, count, &tuner->pitch, est);
}

static void measure_recent(Tuner *tuner, PitchEstimate *est) {
  pitch_estimate(tuner->window + tuner->fill - PITCH_COARSE_WINDOW, PITCH_COARSE_WINDOW,
                 &tuner->pitch, est);
}

bool tuner_feed(Tuner *tuner, const int16_t *samples, size_t count) {
  if (count >= PITCH_WINDOW) {
    samples += count - PITCH_WINDOW;
    count = PITCH_WINDOW;
  }
  const int n = (int)count;
  const int overflow = tuner->fill + n - PITCH_WINDOW;
  if (overflow > 0) {
    memmove(tuner->window, tuner->window + overflow,
            (size_t)(tuner->fill - overflow) * sizeof(int16_t));
    tuner->fill -= overflow;
  }
  int16_t *dst = tuner->window + tuner->fill;
  for (int i = 0; i < n; i++) {
    dst[i] = prefilter(tuner, samples[i]);
  }
  tuner->fill += n;
  tuner->since_estimate += n;

  if (tuner->fill < PITCH_COARSE_WINDOW || tuner->since_estimate < TUNER_HOP) {
    return false;
  }
  tuner->since_estimate = 0;

  PitchEstimate est;
  measure(tuner, &est);
  update(tuner, &est, measure_recent);
  return true;
}
