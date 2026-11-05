#pragma once
// The tuner screen, laid out after Chroma: a big note name with its
// accidental and octave, the reference frequency, a row of 11 deviation
// bars, the deviation in cents and the detected frequency.

#include <pebble.h>

#include "settings.h"

// Value of TunerView.anim when the start-up sweep is not running.
#define UI_NO_ANIM (-1)

typedef struct {
  bool active;        // a note is shown
  bool stale;         // held after the sound stopped: shown without colour
  int midi;           // nearest note
  int cents;          // rounded deviation, -50..50
  int hz_x100;        // detected frequency, hundredths of Hz
  int target_x100;    // frequency of `midi`, hundredths of Hz
  int anim;           // start-up sweep: |bar| lit on both sides, or UI_NO_ANIM
  const char *message;  // shown instead of the tuner when not NULL
} TunerView;

void ui_create(Window *window, const Settings *settings);
void ui_destroy(void);
// Redraws when anything visible changed.
void ui_update(const TunerView *view);
// Redraw after a settings change.
void ui_refresh(void);
