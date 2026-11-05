#pragma once
// User options, stored on the watch, and the settings menu that edits them.
// Chroma's options and defaults, plus microphone sensitivity.

#include <pebble.h>

#include "notes.h"
#include "tuner.h"

typedef struct {
  bool advanced;              // octave, frequencies and cents
  Notation notation;          // A B C or Do Re Mi
  AccidentalStyle accidental; // sharps or flats
  int precision;              // in-tune window, +/- 0..5 cents
  TunerSensitivity sensitivity;
} Settings;

void settings_load(Settings *settings);

// Push the settings menu. on_close runs once it is popped, after any change
// has been saved.
void settings_window_push(Settings *settings, void (*on_close)(void));
