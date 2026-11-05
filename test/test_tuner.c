// Host-side tests for the pitch detector, tuner smoothing and note helpers.
// Build and run with `make -C test`.

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/c/notes.h"
#include "../src/c/pitch.h"
#include "../src/c/tuner.h"

#define RATE PITCH_SAMPLE_RATE
#define FRAME 320  // samples per Pebble mic callback

// Sensitivity used by every tuner the tests create.
static TunerSensitivity s_sensitivity = TUNER_SENSITIVITY_NORMAL;
static const char *const k_sensitivity_names[] = {"low", "normal", "high"};

static int s_failures = 0;
static int s_checks = 0;

#define CHECK(cond, ...)               \
  do {                                 \
    s_checks++;                        \
    if (!(cond)) {                     \
      s_failures++;                    \
      printf("  FAIL: " __VA_ARGS__);  \
      printf("\n");                    \
    }                                  \
  } while (0)

// ---------------------------------------------------------------------------
// Signal generators

static uint32_t s_rng = 0x12345678u;
static double rand_uniform(void) {  // xorshift32, [0, 1)
  s_rng ^= s_rng << 13;
  s_rng ^= s_rng >> 17;
  s_rng ^= s_rng << 5;
  return (double)s_rng / 4294967296.0;
}
static double rand_gauss(void) {
  double u = rand_uniform() + 1e-12;
  double v = rand_uniform();
  return sqrt(-2.0 * log(u)) * cos(2.0 * M_PI * v);
}

static void gen_sine(double *out, int n, double hz, double amp) {
  const double phase0 = rand_uniform() * 2.0 * M_PI;
  for (int i = 0; i < n; i++) {
    out[i] = amp * sin(phase0 + 2.0 * M_PI * hz * i / RATE);
  }
}

// Plucked string by additive synthesis: harmonics with 1/k amplitude, higher
// partials decaying faster, optional inharmonicity B (f_k = k f0 sqrt(1+Bk^2)).
static void gen_pluck(double *out, int n, double hz, double amp, double inharmonicity) {
  memset(out, 0, sizeof(double) * (size_t)n);
  for (int k = 1; k <= 24; k++) {
    const double fk = k * hz * sqrt(1.0 + inharmonicity * k * k);
    if (fk > RATE * 0.45) {
      break;
    }
    const double ak = 1.0 / k * (k == 2 ? 1.4 : 1.0);  // guitars: strong 2nd partial
    const double decay = 1.2 + 0.6 * k;                // per second
    const double phase = rand_uniform() * 2.0 * M_PI;
    for (int i = 0; i < n; i++) {
      const double t = (double)i / RATE;
      out[i] += ak * exp(-decay * t) * sin(phase + 2.0 * M_PI * fk * t);
    }
  }
  double peak = 0;
  for (int i = 0; i < n; i++) {
    peak = fmax(peak, fabs(out[i]));
  }
  for (int i = 0; i < n; i++) {
    out[i] *= amp / peak;
  }
}

// Butterworth high-pass: `passes` cascaded 2nd-order sections (12 dB/oct
// each). Models a small MEMS microphone that barely hears a low E fundamental.
static void highpass(double *x, int n, double fc, int passes) {
  for (int pass = 0; pass < passes; pass++) {
    const double w0 = 2.0 * M_PI * fc / RATE;
    const double alpha = sin(w0) / (2.0 * M_SQRT1_2);
    const double c = cos(w0);
    const double a0 = 1.0 + alpha;
    const double b0 = (1.0 + c) / 2.0 / a0, b1 = -(1.0 + c) / a0, b2 = b0;
    const double a1 = -2.0 * c / a0, a2 = (1.0 - alpha) / a0;
    double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    for (int i = 0; i < n; i++) {
      const double y = b0 * x[i] + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
      x2 = x1;
      x1 = x[i];
      y2 = y1;
      y1 = y;
      x[i] = y;
    }
  }
}

static void add_noise(double *x, int n, double rms) {
  for (int i = 0; i < n; i++) {
    x[i] += rms * rand_gauss();
  }
}

static double signal_rms(const double *x, int n) {
  double e = 0;
  for (int i = 0; i < n; i++) {
    e += x[i] * x[i];
  }
  return sqrt(e / n);
}

static void to_pcm(const double *x, int16_t *out, int n) {
  for (int i = 0; i < n; i++) {
    double v = round(x[i]);
    out[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
  }
}

// ---------------------------------------------------------------------------
// Harness

typedef struct {
  int readings;      // readings inside the measured interval
  int detected;      // fresh readings
  int wrong_note;    // detected, but not the expected note
  int octave_errors; // detected a whole number of octaves away
  double max_err;    // max |cents error| on right-note readings
  double sum_err;
} Stats;

// Feeds the PCM through a tuner in mic-sized frames and scores readings
// between t0 and t1 seconds against the expected frequency.
static Stats run(const int16_t *pcm, int n, double expect_hz, double t0, double t1) {
  static Tuner tuner;
  tuner_init(&tuner, s_sensitivity);
  Stats st = {0};
  const double expect_midi = 69.0 + 12.0 * log2(expect_hz / 440.0);
  const int expect_note = (int)lround(expect_midi);
  for (int pos = 0; pos + FRAME <= n; pos += FRAME) {
    if (!tuner_feed(&tuner, pcm + pos, FRAME)) {
      continue;
    }
    const double t = (double)(pos + FRAME) / RATE;
    if (t < t0 || t > t1) {
      continue;
    }
    st.readings++;
    const TunerReading *r = &tuner.reading;
    // TRACE_HZ=82.4 prints every reading of runs expecting that frequency.
    const char *trace = getenv("TRACE_HZ");
    if (trace && fabs(atof(trace) / expect_hz - 1.0) < 0.01) {
      printf("    t=%.3f fresh=%d midi=%d cents=%+.2f err=%+.2f rms=%d clarity=%.3f\n", t, r->fresh,
             r->midi, r->cents, (r->midi + r->cents / 100.0 - expect_midi) * 100.0, (int)r->rms,
             r->clarity);
    }
    if (!r->fresh) {
      continue;
    }
    st.detected++;
    if (r->midi != expect_note) {
      st.wrong_note++;
      if ((r->midi - expect_note) % 12 == 0) {
        st.octave_errors++;
      }
      continue;
    }
    const double err = fabs((r->midi + r->cents / 100.0 - expect_midi) * 100.0);
    st.max_err = fmax(st.max_err, err);
    st.sum_err += err;
  }
  return st;
}

static double detect_rate(const Stats *st) {
  return st->readings ? (double)st->detected / st->readings : 0.0;
}

#define SECONDS(s) ((int)((s) * RATE))
#define MAX_SAMPLES (3 * RATE)
static double s_buf[MAX_SAMPLES];
static int16_t s_pcm[MAX_SAMPLES];

// ---------------------------------------------------------------------------
// Tests

static void test_notes(void) {
  printf("notes helpers\n");
  double worst = 0;
  for (double x = 0.01; x < 100.0; x *= 1.0137) {
    worst = fmax(worst, fabs(notes_log2f((float)x) - log2(x)));
  }
  CHECK(worst < 2e-6, "log2 max error %.3g", worst);

  CHECK(fabsf(notes_midi_to_hz(69) - 440.0f) < 1e-3f, "A4");
  CHECK(fabsf(notes_midi_to_hz(40) - 82.4069f) < 2e-3f, "E2 = %.4f", notes_midi_to_hz(40));
  CHECK(fabsf(notes_midi_to_hz(64) - 329.6276f) < 3e-3f, "E4 = %.4f", notes_midi_to_hz(64));
  CHECK(fabsf(notes_midi_to_hz(12) - 16.3516f) < 1e-3f, "C0 = %.4f", notes_midi_to_hz(12));
  CHECK(fabsf(notes_hz_to_midi(146.8324f) - 50.0f) < 1e-4f, "D3 midi");

  NoteName n = notes_name(50, ACCIDENTAL_STYLE_SHARP);  // D3
  CHECK(n.degree == 1 && n.accidental == ACCIDENTAL_NONE && n.octave == 3, "D3 name");
  n = notes_name(61, ACCIDENTAL_STYLE_SHARP);  // C#4
  CHECK(n.degree == 0 && n.accidental == ACCIDENTAL_SHARP && n.octave == 4, "C#4");
  n = notes_name(61, ACCIDENTAL_STYLE_FLAT);  // Db4
  CHECK(n.degree == 1 && n.accidental == ACCIDENTAL_FLAT && n.octave == 4, "Db4");
  n = notes_name(70, ACCIDENTAL_STYLE_FLAT);  // Bb4
  CHECK(n.degree == 6 && n.accidental == ACCIDENTAL_FLAT && n.octave == 4, "Bb4");
  n = notes_name(59, ACCIDENTAL_STYLE_SHARP);  // B3
  CHECK(n.degree == 6 && n.octave == 3, "B3");
  CHECK(strcmp(notes_degree_text(4, NOTATION_SOLFEGE), "Sol") == 0, "Sol");
  CHECK(strcmp(notes_degree_text(1, NOTATION_LETTER), "D") == 0, "D");

  CHECK(notes_deviation_bar(0, 2) == 0, "bar 0");
  CHECK(notes_deviation_bar(2, 2) == 0, "bar +2 in tune at +/-2");
  CHECK(notes_deviation_bar(-3, 2) == -1, "bar -3");
  CHECK(notes_deviation_bar(1, 0) == 1, "bar +1 at +/-0");
  CHECK(notes_deviation_bar(19, 2) == 1 && notes_deviation_bar(20, 2) == 2, "bar 20");
  CHECK(notes_deviation_bar(-35, 2) == -3 && notes_deviation_bar(45, 2) == 4, "bar 35/45");
  CHECK(notes_deviation_bar(-50, 2) == -5 && notes_deviation_bar(54, 2) == 5, "bar 50+");

  CHECK(notes_round(2.5f) == 3 && notes_round(-2.5f) == -3 && notes_round(-0.4f) == 0, "round");
  char buf[16];
  notes_format_hz(buf, sizeof(buf), 146.8324f);
  CHECK(strcmp(buf, "146.83") == 0, "format %s", buf);
  notes_format_hz(buf, sizeof(buf), 82.405f);
  CHECK(strcmp(buf, "82.41") == 0, "format %s", buf);
}

static void test_sines(void) {
  printf("sine sweep 61.7..1318.5 Hz\n");
  const int n = SECONDS(0.6);
  double worst = 0;
  for (int midi = 35; midi <= 88; midi++) {
    for (int c = -40; c <= 40; c += 20) {
      const double hz = 440.0 * pow(2.0, (midi + c / 100.0 - 69) / 12.0);
      gen_sine(s_buf, n, hz, 6000);
      to_pcm(s_buf, s_pcm, n);
      Stats st = run(s_pcm, n, hz, 0.25, 0.6);
      CHECK(detect_rate(&st) == 1.0 && st.wrong_note == 0,
            "sine %.2f Hz: detected %d/%d wrong %d", hz, st.detected, st.readings, st.wrong_note);
      worst = fmax(worst, st.max_err);
    }
  }
  printf("  worst error %.3f cents\n", worst);
  CHECK(worst < 0.5, "sine worst error %.3f cents", worst);
}

static const struct {
  const char *name;
  double hz;
} k_strings[] = {
    {"B1 (7-string)", 61.735}, {"D2 (drop D)", 73.416}, {"E2", 82.407},  {"A2", 110.000},
    {"D3", 146.832},           {"G3", 195.998},         {"B3", 246.942}, {"E4", 329.628},
    {"E5 (12th fret)", 659.255}, {"E6 (24th fret)", 1318.51},
};
#define NUM_STRINGS ((int)(sizeof(k_strings) / sizeof(k_strings[0])))

static void test_plucks(void) {
  printf("plucked strings, 0.15..2.5 s\n");
  const int n = SECONDS(2.5);
  for (int s = 0; s < NUM_STRINGS; s++) {
    const double hz = k_strings[s].hz * pow(2.0, 7.0 / 1200.0);  // 7 cents sharp
    gen_pluck(s_buf, n, hz, 12000, 0.0);
    to_pcm(s_buf, s_pcm, n);
    Stats st = run(s_pcm, n, hz, 0.15, 2.5);
    printf("  %-15s detect %5.1f%%  max err %.2f ct  mean %.2f ct\n", k_strings[s].name,
           100 * detect_rate(&st), st.max_err, st.detected ? st.sum_err / st.detected : 0.0);
    CHECK(detect_rate(&st) >= 0.97 && st.wrong_note == 0 && st.max_err < 1.0,
          "pluck %s: detect %.2f wrong %d err %.2f", k_strings[s].name, detect_rate(&st),
          st.wrong_note, st.max_err);
  }
}

// Stress tests run every case under several noise seeds and score the worst.
static const uint32_t k_seeds[] = {0x12345678u, 0xdeadbeefu, 0x2545f491u, 0x9e3779b9u};
#define NUM_SEEDS ((int)(sizeof(k_seeds) / sizeof(k_seeds[0])))

typedef enum { STRESS_SMALL_MIC, STRESS_HARSH_MIC, STRESS_NOISY } Stress;

static Stats stress_case(Stress kind, double hz, uint32_t seed) {
  s_rng = seed;
  int n;
  double t1;
  switch (kind) {
    case STRESS_SMALL_MIC:  // 12 dB/oct at 200 Hz, mild inharmonicity, a little hiss
      n = SECONDS(t1 = 2.0);
      gen_pluck(s_buf, n, hz, 12000, 3.0e-5);
      highpass(s_buf, n, 200.0, 1);
      add_noise(s_buf, n, signal_rms(s_buf, SECONDS(0.5)) * 0.02);
      break;
    case STRESS_HARSH_MIC:  // 24 dB/oct at 250 Hz: low E fundamental at -39 dB
      n = SECONDS(t1 = 1.5);
      gen_pluck(s_buf, n, hz, 12000, 1.0e-4);
      highpass(s_buf, n, 250.0, 2);
      add_noise(s_buf, n, signal_rms(s_buf, SECONDS(0.5)) * 0.02);
      break;
    default:  // white noise at 10 dB SNR over the whole note
      n = SECONDS(t1 = 1.5);
      gen_pluck(s_buf, n, hz, 8000, 0.0);
      add_noise(s_buf, n, signal_rms(s_buf, n) * 0.316);
      break;
  }
  to_pcm(s_buf, s_pcm, n);
  return run(s_pcm, n, hz, 0.15, t1);
}

// Worst detection rate, total wrong notes, worst max error and mean error.
// Limits are for the low sensitivity; the more sensitive levels keep showing
// a note further into its noisy tail, where readings wander more, so their
// error limits get some slack. Wrong notes are never allowed.
static void stress(Stress kind, int strings, double min_detect, double max_err, double max_mean,
                   double b1_max_err) {
  static const double k_err_slack[] = {1.0, 1.25, 1.6};
  static const double k_mean_slack[] = {0.0, 0.25, 0.5};
  max_err *= k_err_slack[s_sensitivity];
  b1_max_err *= k_err_slack[s_sensitivity];
  max_mean += k_mean_slack[s_sensitivity];
  for (int s = 0; s < strings; s++) {
    double detect = 1.0, worst = 0.0, worst_mean = 0.0;
    int wrong = 0, octave = 0;
    for (int k = 0; k < NUM_SEEDS; k++) {
      const Stats st = stress_case(kind, k_strings[s].hz, k_seeds[k]);
      detect = fmin(detect, detect_rate(&st));
      worst = fmax(worst, st.max_err);
      worst_mean = fmax(worst_mean, st.detected ? st.sum_err / st.detected : 0.0);
      wrong += st.wrong_note;
      octave += st.octave_errors;
    }
    printf("  %-15s detect >= %5.1f%%  wrong %d (octave %d)  max err %5.2f ct  mean <= %.2f ct\n",
           k_strings[s].name, 100 * detect, wrong, octave, worst, worst_mean);
    // The 7-string low B is the bottom of the range: ~4 periods per 64 ms, so
    // noise moves it more. It gets its own limits.
    const bool b1 = s == 0;
    CHECK(wrong == 0, "%s: %d wrong notes", k_strings[s].name, wrong);
    CHECK(detect >= (b1 ? min_detect - 0.1 : min_detect), "%s: detect %.2f", k_strings[s].name,
          detect);
    CHECK(worst < (b1 ? b1_max_err : max_err) && worst_mean < (b1 ? 2.0 * max_mean : max_mean),
          "%s: max err %.2f mean %.2f", k_strings[s].name, worst, worst_mean);
  }
}

static void test_stress(void) {
  // Inharmonicity pulls the attack a little sharp, as on real tuners; the
  // worst readings are in the tail, near the noise floor. The last ~0.3 s is
  // below it and rightly not shown.
  printf("low strings through a 12 dB/oct 200 Hz high-pass, 0.15..2.0 s, %d seeds\n", NUM_SEEDS);
  stress(STRESS_SMALL_MIC, 6, 0.8, 4.0, 1.5, 5.0);

  // Pessimistic microphone: what matters is never showing a wrong note.
  printf("low strings through a 24 dB/oct 250 Hz high-pass, 0.15..1.5 s, %d seeds\n", NUM_SEEDS);
  stress(STRESS_HARSH_MIC, 5, 0.6, 15.0, 5.0, 15.0);

  // SNR is over the whole note: its tail is far below 0 dB and goes unshown.
  printf("plucks at 10 dB SNR, 0.15..1.5 s, %d seeds\n", NUM_SEEDS);
  stress(STRESS_NOISY, 8, 0.55, 5.0, 1.5, 9.0);
}

// A hard pluck sets an acoustic guitar's body ringing at its air resonance
// (~90-130 Hz) for a fraction of a second. High E plus a 110 Hz thump
// repeats every 3 E periods and used to read as A2; G and B strings had
// the same problem an octave down.
static void add_thump(double *x, int n, double hz, double amp, double decay_s) {
  const double phase = rand_uniform() * 2.0 * M_PI;
  for (int i = 0; i < n; i++) {
    const double t = (double)i / RATE;
    x[i] += amp * exp(-t / decay_s) * sin(phase + 2.0 * M_PI * hz * t);
  }
}

static void test_body_thump(void) {
  printf("plucks with a body resonance thump at the attack\n");
  static const double k_thump_hz[] = {95, 105, 110, 115, 125};
  static const double k_thump_level[] = {0.5, 1.0};  // of the string's peak
  static const double k_thump_decay[] = {0.05, 0.15};
  const int n = SECONDS(1.0);
  for (int s = 3; s < 8; s++) {  // A2 .. E4
    const double hz = k_strings[s].hz;
    int readings = 0, wrong = 0;
    double worst = 0.0;
    for (int f = 0; f < 5; f++) {
      for (int l = 0; l < 2; l++) {
        for (int d = 0; d < 2; d++) {
          s_rng = 0x51u + (uint32_t)(s * 1000 + f * 100 + l * 10 + d);
          gen_pluck(s_buf, n, hz, 12000, 1.0e-5);
          add_thump(s_buf, n, k_thump_hz[f], 12000 * k_thump_level[l], k_thump_decay[d]);
          add_noise(s_buf, n, 30);
          to_pcm(s_buf, s_pcm, n);
          const Stats st = run(s_pcm, n, hz, 0.0, 1.0);
          readings += st.detected;
          wrong += st.wrong_note;
          worst = fmax(worst, st.max_err);
        }
      }
    }
    printf("  %-15s wrong %d of %d readings  max err %.2f ct\n", k_strings[s].name, wrong,
           readings, worst);
    CHECK(wrong <= readings / 50, "%s thump: %d/%d wrong", k_strings[s].name, wrong, readings);
    if (s == 7) {
      CHECK(wrong == 0, "E4 thump: %d wrong", wrong);
    }
  }
}

// Softer and softer plucks over a fixed microphone noise floor: how long the
// note stays on screen at each sensitivity, and that it is never wrong.
static void test_soft_plucks(void) {
  printf("soft plucks over a noise floor (seconds shown of 2.0, per sensitivity)\n");
  static const double k_peaks[] = {4000, 1500, 600, 250};
  const int n = SECONDS(2.0);
  double shown[3][4] = {{0}};
  for (int level = 0; level < 3; level++) {
    s_sensitivity = (TunerSensitivity)level;
    for (int p = 0; p < 4; p++) {
      for (int str = 2; str < 8; str++) {  // E2 .. E4
        for (int k = 0; k < 2; k++) {
          s_rng = k_seeds[k] + (uint32_t)(p * 31 + str);
          gen_pluck(s_buf, n, k_strings[str].hz, k_peaks[p], 1.0e-5);
          add_noise(s_buf, n, 25);
          to_pcm(s_buf, s_pcm, n);
          const Stats st = run(s_pcm, n, k_strings[str].hz, 0.0, 2.0);
          shown[level][p] += st.detected * (TUNER_HOP / (double)RATE) / 12.0;
          CHECK(st.wrong_note == 0, "soft %s peak %.0f %s: %d wrong", k_sensitivity_names[level],
                k_peaks[p], k_strings[str].name, st.wrong_note);
        }
      }
    }
  }
  for (int p = 0; p < 4; p++) {
    printf("  peak %5.0f (%+3.0f dB):  low %.2f s  normal %.2f s  high %.2f s\n", k_peaks[p],
           20 * log10(k_peaks[p] / 4000), shown[0][p], shown[1][p], shown[2][p]);
    CHECK(shown[1][p] >= shown[0][p] && shown[2][p] >= shown[1][p],
          "sensitivity should not reduce detection at peak %.0f", k_peaks[p]);
  }
  s_sensitivity = TUNER_SENSITIVITY_NORMAL;
}

static void test_unpitched(void) {
  printf("silence and white noise\n");
  const int n = SECONDS(2.0);
  memset(s_buf, 0, sizeof(double) * (size_t)n);
  add_noise(s_buf, n, 5);  // below the gate
  to_pcm(s_buf, s_pcm, n);
  Stats st = run(s_pcm, n, 440, 0.0, 2.0);
  CHECK(st.detected == 0, "quiet: %d detections", st.detected);

  memset(s_buf, 0, sizeof(double) * (size_t)n);
  add_noise(s_buf, n, 3000);
  to_pcm(s_buf, s_pcm, n);
  st = run(s_pcm, n, 440, 0.0, 2.0);
  printf("  white noise: %d/%d readings detected a pitch\n", st.detected, st.readings);
  CHECK(st.detected <= st.readings / 20, "noise: %d/%d detections", st.detected, st.readings);
}

static void test_cents(void) {
  printf("cents offsets on A2 pluck\n");
  const int n = SECONDS(1.0);
  for (int c = -45; c <= 45; c += 5) {
    const double hz = 110.0 * pow(2.0, c / 1200.0);
    gen_pluck(s_buf, n, hz, 10000, 0.0);
    to_pcm(s_buf, s_pcm, n);
    static Tuner tuner;
    tuner_init(&tuner, s_sensitivity);
    for (int pos = 0; pos + FRAME <= SECONDS(0.6); pos += FRAME) {
      tuner_feed(&tuner, s_pcm + pos, FRAME);
    }
    const TunerReading *r = &tuner.reading;
    CHECK(r->active && r->midi == 45 && fabs(r->cents - c) < 0.6,
          "A2 %+d ct: midi %d cents %.2f", c, r->midi, r->cents);
    CHECK(notes_deviation_bar(notes_round(r->cents), 2) == notes_deviation_bar(c, 2),
          "A2 %+d ct bar", c);
  }
}

static void test_note_change_and_hold(void) {
  printf("note change, hold and release\n");
  const int half = SECONDS(1.0);
  gen_pluck(s_buf, half, 82.407, 10000, 0.0);
  gen_pluck(s_buf + half, half, 110.0, 10000, 0.0);
  memset(s_buf + 2 * half, 0, sizeof(double) * (size_t)half);
  to_pcm(s_buf, s_pcm, 3 * half);

  static Tuner tuner;
  tuner_init(&tuner, s_sensitivity);
  int midi_at_1_2 = -1, midi_at_2_5 = -1;
  bool active_at_2_5 = false, active_end = true;
  for (int pos = 0; pos + FRAME <= 3 * half; pos += FRAME) {
    tuner_feed(&tuner, s_pcm + pos, FRAME);
    const int t_ms = (pos + FRAME) * 1000 / RATE;
    if (t_ms == 1200) {
      midi_at_1_2 = tuner.reading.midi;
    }
    if (t_ms == 2500) {
      midi_at_2_5 = tuner.reading.midi;
      active_at_2_5 = tuner.reading.active;
    }
  }
  active_end = tuner.reading.active;
  CHECK(midi_at_1_2 == 45, "A2 200 ms after change: midi %d", midi_at_1_2);
  CHECK(active_at_2_5 && midi_at_2_5 == 45, "held 0.5 s after stop");
  CHECK(!active_end, "released 1 s after stop");
}

int main(void) {
  test_notes();
  test_sines();
  test_plucks();
  for (int level = 0; level < 3; level++) {
    s_sensitivity = (TunerSensitivity)level;
    printf("\n== sensitivity: %s ==\n", k_sensitivity_names[level]);
    test_stress();
    test_body_thump();
    test_unpitched();
  }
  s_sensitivity = TUNER_SENSITIVITY_NORMAL;
  printf("\n");
  test_soft_plucks();
  test_cents();
  test_note_change_and_hold();
  printf("\n%d/%d checks passed\n", s_checks - s_failures, s_checks);
  return s_failures ? 1 : 0;
}
