#pragma once
// Monophonic pitch detection (YIN) for 16 kHz signed 16-bit PCM.
//
// Portable C99 with no Pebble dependencies so it can be unit-tested on a
// desktop (see test/). The heavy loops are integer-only because Pebble apps
// are compiled for Cortex-M3 with software floating point.

#include <stdbool.h>
#include <stdint.h>

#define PITCH_SAMPLE_RATE 16000
// The period is found on the most recent 64 ms (~5 periods of low E) and then
// measured precisely over up to 128 ms, which halves the jitter on low notes.
#define PITCH_COARSE_WINDOW 1024
#define PITCH_WINDOW 2048
// Detection range: covers a 7-string low B (61.7 Hz) up to the 24th fret
// of the high E string (1318.5 Hz).
#define PITCH_MIN_HZ 58
#define PITCH_MAX_HZ 1400

typedef struct {
  // Detected fundamental in Hz, or 0 when the window has no clear pitch.
  float hz;
  // Periodicity of the window, 0..1 (1 - YIN aperiodicity at the chosen lag).
  float clarity;
  // RMS level of the window, in sample units (full scale is 32767).
  int32_t rms;
} PitchEstimate;

// Analyse the most recent `count` samples (oldest first), where count is
// PITCH_COARSE_WINDOW..PITCH_WINDOW. Audio whose latest 64 ms is quieter than
// min_rms is reported as unpitched without running the detector.
void pitch_estimate(const int16_t *samples, int count, int32_t min_rms, PitchEstimate *out);
