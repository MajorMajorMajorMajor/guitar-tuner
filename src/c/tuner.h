#pragma once
// Streaming tuner: buffers microphone frames, runs the pitch detector at a
// fixed hop and turns raw estimates into a steady note + cents reading.
// Portable C99 (no Pebble deps).

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pitch.h"

// Samples between estimates: 40 ms, i.e. 25 readings per second.
#define TUNER_HOP 640
// Keep showing the last note this many estimates after the sound stops.
#define TUNER_HOLD 20

typedef struct {
  bool active;    // a note is being shown
  bool fresh;     // this reading came from a detection, not the hold
  int midi;       // nearest equal-tempered note, MIDI number (69 = A4)
  float cents;    // deviation from that note
  float hz;       // smoothed detected frequency
  float target_hz;
  float clarity;  // periodicity of the latest detection, 0..1
  int32_t rms;    // input level of the latest window
} TunerReading;

typedef struct {
  // Pre-filter state (fixed point, see tuner.c).
  int32_t hp_x1, hp_y1, lp1, lp2;
  // Most recent PITCH_WINDOW filtered samples.
  int16_t window[PITCH_WINDOW];
  int fill;
  int since_estimate;
  float history[3];
  int history_len;
  float smooth;   // smoothed fractional MIDI number
  int span;       // samples to measure over: grows from the onset of a note
  int settle;     // estimates left in the fast-settling phase of a new note
  int misses;
  int32_t min_rms;
  TunerReading reading;
} Tuner;

void tuner_init(Tuner *tuner, int32_t min_rms);
// Feed microphone samples. Returns true when tuner->reading was updated.
bool tuner_feed(Tuner *tuner, const int16_t *samples, size_t count);
