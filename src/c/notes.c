#include "notes.h"

#include <stdint.h>
#include <stdio.h>

float notes_log2f(float x) {
  if (x <= 0.0f) {
    return -128.0f;
  }
  union {
    float f;
    uint32_t i;
  } u = {x};
  int exponent = (int)((u.i >> 23) & 0xff) - 127;
  u.i = (u.i & 0x007fffffu) | 0x3f800000u;  // mantissa in [1, 2)
  float m = u.f;
  if (m > 1.41421356f) {  // recentre to [0.707, 1.414) so the series converges fast
    m *= 0.5f;
    exponent += 1;
  }
  // ln(m) = 2 atanh(z), z = (m - 1) / (m + 1), |z| <= 0.172
  const float z = (m - 1.0f) / (m + 1.0f);
  const float z2 = z * z;
  const float ln =
      2.0f * z *
      (1.0f + z2 * (1.0f / 3.0f + z2 * (1.0f / 5.0f + z2 * (1.0f / 7.0f + z2 * (1.0f / 9.0f)))));
  return (float)exponent + ln * 1.44269504f;
}

float notes_hz_to_midi(float hz) {
  return 69.0f + 12.0f * notes_log2f(hz / 440.0f);
}

static const float k_semitone_ratio[12] = {
    1.00000000f, 1.05946309f, 1.12246205f, 1.18920712f, 1.25992105f, 1.33483985f,
    1.41421356f, 1.49830708f, 1.58740105f, 1.68179283f, 1.78179744f, 1.88774863f,
};

static int floor_div(int a, int b) {
  int q = a / b;
  if ((a % b != 0) && ((a < 0) != (b < 0))) {
    q--;
  }
  return q;
}

float notes_midi_to_hz(int midi) {
  const int rel = midi - 69;
  int octaves = floor_div(rel, 12);
  float hz = 440.0f * k_semitone_ratio[rel - octaves * 12];
  for (; octaves > 0; octaves--) {
    hz *= 2.0f;
  }
  for (; octaves < 0; octaves++) {
    hz *= 0.5f;
  }
  return hz;
}

// Pitch class (0 = C) -> degree and whether it is a black key.
static const int k_sharp_degree[12] = {0, 0, 1, 1, 2, 3, 3, 4, 4, 5, 5, 6};
static const bool k_black_key[12] = {false, true, false, true, false, false,
                                     true, false, true, false, true, false};

NoteName notes_name(int midi, AccidentalStyle style) {
  const int pitch_class = midi - floor_div(midi, 12) * 12;
  NoteName name = {
      .degree = k_sharp_degree[pitch_class],
      .accidental = ACCIDENTAL_NONE,
      .octave = floor_div(midi, 12) - 1,
  };
  if (k_black_key[pitch_class]) {
    if (style == ACCIDENTAL_STYLE_FLAT) {
      name.degree += 1;  // C# -> Db: the next degree, same octave
      name.accidental = ACCIDENTAL_FLAT;
    } else {
      name.accidental = ACCIDENTAL_SHARP;
    }
  }
  return name;
}

const char *notes_degree_text(int degree, Notation notation) {
  static const char *const k_letters[7] = {"C", "D", "E", "F", "G", "A", "B"};
  static const char *const k_solfege[7] = {"Do", "Re", "Mi", "Fa", "Sol", "La", "Si"};
  if (degree < 0 || degree > 6) {
    return "";
  }
  return notation == NOTATION_SOLFEGE ? k_solfege[degree] : k_letters[degree];
}

int notes_deviation_bar(int cents, int precision) {
  const int magnitude = cents < 0 ? -cents : cents;
  int bar;
  if (magnitude <= precision) {
    bar = 0;
  } else if (magnitude < 20) {
    bar = 1;
  } else if (magnitude < 30) {
    bar = 2;
  } else if (magnitude < 40) {
    bar = 3;
  } else if (magnitude < 50) {
    bar = 4;
  } else {
    bar = 5;
  }
  return cents < 0 ? -bar : bar;
}

int notes_round(float value) {
  return value < 0.0f ? -(int)(0.5f - value) : (int)(value + 0.5f);
}

void notes_format_hz(char *buf, size_t size, float hz) {
  if (hz < 0.0f) {
    hz = 0.0f;
  }
  const int hundredths = (int)(hz * 100.0f + 0.5f);
  snprintf(buf, size, "%d.%02d", hundredths / 100, hundredths % 100);
}
