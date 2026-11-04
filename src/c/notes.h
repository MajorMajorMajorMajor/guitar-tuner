#pragma once
// Note naming, cents and small math helpers. Portable C99 (no Pebble deps).

#include <stdbool.h>
#include <stddef.h>

typedef enum {
  NOTATION_LETTER = 0,  // C D E F G A B
  NOTATION_SOLFEGE,     // Do Re Mi Fa Sol La Si
} Notation;

typedef enum {
  ACCIDENTAL_STYLE_SHARP = 0,  // C#, D#, F#, G#, A#
  ACCIDENTAL_STYLE_FLAT,       // Db, Eb, Gb, Ab, Bb
} AccidentalStyle;

typedef enum {
  ACCIDENTAL_NONE = 0,
  ACCIDENTAL_SHARP,
  ACCIDENTAL_FLAT,
} Accidental;

typedef struct {
  int degree;             // 0..6 for C D E F G A B
  Accidental accidental;
  int octave;             // scientific pitch notation: A4 = 440 Hz
} NoteName;

// Number of deviation bars either side of the centre bar.
#define DEVIATION_BARS_HALF 5

// log2 accurate to ~1e-7, without libm.
float notes_log2f(float x);

// Fractional MIDI note number (69.0 = A4 = 440 Hz).
float notes_hz_to_midi(float hz);
// Equal-tempered frequency of a MIDI note.
float notes_midi_to_hz(int midi);

NoteName notes_name(int midi, AccidentalStyle style);
// Text of a note degree (0..6) in the given notation.
const char *notes_degree_text(int degree, Notation notation);

// Which of the 2 * DEVIATION_BARS_HALF + 1 bars lights up for a deviation,
// from -DEVIATION_BARS_HALF (flat) to +DEVIATION_BARS_HALF (sharp). Same
// bands as Chroma: centre within +/-precision, then 20, 30, 40, 50 cents.
int notes_deviation_bar(int cents, int precision);

// Round to the nearest integer, halves away from zero.
int notes_round(float value);

// "146.83" style text with two decimals (Pebble's printf has no %f).
void notes_format_hz(char *buf, size_t size, float hz);
