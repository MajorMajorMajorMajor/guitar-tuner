#include "ui.h"

#include <string.h>

#include "notes.h"
#include "theme.h"

// ---------------------------------------------------------------------------
// Per-display metrics. Note images come from tools/render_glyphs.py, font
// sizes from package.json ("pixelHeight"); keep them in sync.

#if defined(PBL_PLATFORM_GABBRO)  // Pebble Round 2, 260x260
#define NOTE_CAP 64
#define NOTE_DESCENT 2
#define FONT_SMALL_H 20
#define FONT_CENTS_H 34
#define FONT_OCTAVE_H 30
#define BAR_PITCH 18
#define BAR_W 5
#define BAR_H_RED 16
#define BAR_H_YELLOW 30
#define BAR_H_GREEN 48
#define GAP 6
#else  // Pebble Time 2 (emery), 200x228
#define NOTE_CAP 60
#define NOTE_DESCENT 2
#define FONT_SMALL_H 19
#define FONT_CENTS_H 32
#define FONT_OCTAVE_H 28
#define BAR_PITCH 15
#define BAR_W 4
#define BAR_H_RED 14
#define BAR_H_YELLOW 27
#define BAR_H_GREEN 44
#define GAP 6
#endif

// Digits are ~0.71 em tall in Noto Sans.
#define DIGIT_H(font_h) ((font_h) * 71 / 100)
// Octave digit baseline below the note's baseline (Chroma's subscript).
#define OCTAVE_DROP (NOTE_CAP / 8)
#define SUFFIX_GAP 2

// ---------------------------------------------------------------------------

static Layer *s_layer;
static const Settings *s_settings;
static TunerView s_view;
static GFont s_font_small, s_font_cents, s_font_octave;

static GBitmap *s_note_bitmap, *s_acc_bitmap;
static uint32_t s_note_id, s_acc_id;

static const uint32_t k_letter_ids[7] = {
    RESOURCE_ID_NOTE_C, RESOURCE_ID_NOTE_D, RESOURCE_ID_NOTE_E, RESOURCE_ID_NOTE_F,
    RESOURCE_ID_NOTE_G, RESOURCE_ID_NOTE_A, RESOURCE_ID_NOTE_B,
};
static const uint32_t k_solfege_ids[7] = {
    RESOURCE_ID_NOTE_DO, RESOURCE_ID_NOTE_RE, RESOURCE_ID_NOTE_MI, RESOURCE_ID_NOTE_FA,
    RESOURCE_ID_NOTE_SOL, RESOURCE_ID_NOTE_LA, RESOURCE_ID_NOTE_SI,
};

static GBitmap *load(GBitmap *current, uint32_t *current_id, uint32_t id) {
  if (current && *current_id == id) {
    return current;
  }
  if (current) {
    gbitmap_destroy(current);
  }
  *current_id = id;
  return id ? gbitmap_create_with_resource(id) : NULL;
}

// ---------------------------------------------------------------------------
// Drawing helpers

static int text_width(const char *text, GFont font) {
  return graphics_text_layout_get_content_size(text, font, GRect(0, 0, 400, 100),
                                               GTextOverflowModeFill, GTextAlignmentLeft)
      .w;
}

// The SDK puts a generated font's baseline pixelHeight rows below the top of
// its text box, so text can be placed by baseline.
static void draw_text(GContext *ctx, const char *text, GFont font, int font_h, int x, int baseline,
                      int w, GTextAlignment align) {
  const GRect box = GRect(x, baseline - font_h, w, font_h + font_h / 2);
  graphics_draw_text(ctx, text, font, box, GTextOverflowModeFill, align, NULL);
}

// Glyph images are white with 2-bit alpha.
static void draw_bitmap(GContext *ctx, GBitmap *bitmap, int x, int y) {
  const GRect bounds = gbitmap_get_bounds(bitmap);
  graphics_context_set_compositing_mode(ctx, GCompOpSet);
  graphics_draw_bitmap_in_rect(ctx, bitmap, GRect(x, y, bounds.size.w, bounds.size.h));
  graphics_context_set_compositing_mode(ctx, GCompOpAssign);
}

static GColor bar_color(int magnitude) {
  return magnitude == 0 ? THEME_GREEN : magnitude <= 2 ? THEME_YELLOW : THEME_RED;
}

static int bar_height(int magnitude) {
  return magnitude == 0 ? BAR_H_GREEN : magnitude <= 2 ? BAR_H_YELLOW : BAR_H_RED;
}

static void format_cents(char *buf, size_t size, int cents) {
  snprintf(buf, size, cents > 0 ? "+%d" : "%d", cents);
}

static void format_hz(char *buf, size_t size, int hz_x100) {
  notes_format_hz(buf, size, (float)hz_x100 / 100.0f);
  strncat(buf, " Hz", size - strlen(buf) - 1);
}

// ---------------------------------------------------------------------------
// Parts of the screen

// The 11 bars, vertically centred on `center_y`.
static void draw_bars(GContext *ctx, int center_x, int center_y) {
  const int lit = s_view.active ? notes_deviation_bar(s_view.cents, s_settings->precision) : 99;
  for (int i = -DEVIATION_BARS_HALF; i <= DEVIATION_BARS_HALF; i++) {
    const int magnitude = i < 0 ? -i : i;
    const int h = bar_height(magnitude);
    const int x = center_x + i * BAR_PITCH;
    const bool on = (s_view.anim != UI_NO_ANIM) ? magnitude == s_view.anim : i == lit;
    const GColor color = !on ? THEME_INACTIVE : s_view.stale ? THEME_SECONDARY
                                                              : bar_color(magnitude);
    graphics_context_set_fill_color(ctx, color);
    graphics_fill_rect(ctx, GRect(x - BAR_W / 2, center_y - h / 2, BAR_W, h), 0, GCornerNone);
  }
}

// Note name with accidental (top right) and octave (bottom right), centred
// on center_x with its cap top at `top`. Returns the baseline.
static int draw_note(GContext *ctx, int center_x, int top) {
  const NoteName name = notes_name(s_view.midi, s_settings->accidental);
  const uint32_t *ids = s_settings->notation == NOTATION_SOLFEGE ? k_solfege_ids : k_letter_ids;
  s_note_bitmap = load(s_note_bitmap, &s_note_id, ids[name.degree]);
  const uint32_t acc_id = name.accidental == ACCIDENTAL_SHARP   ? RESOURCE_ID_ACCIDENTAL_SHARP
                          : name.accidental == ACCIDENTAL_FLAT ? RESOURCE_ID_ACCIDENTAL_FLAT
                                                                : 0;
  s_acc_bitmap = load(s_acc_bitmap, &s_acc_id, acc_id);

  const GRect note = gbitmap_get_bounds(s_note_bitmap);
  const int image_top = top - (note.size.h - NOTE_DESCENT - NOTE_CAP);
  const int baseline = image_top + note.size.h - NOTE_DESCENT;

  char octave[4] = "";
  if (s_settings->advanced) {
    snprintf(octave, sizeof(octave), "%d", name.octave);
  }
  const int acc_w = s_acc_bitmap ? gbitmap_get_bounds(s_acc_bitmap).size.w : 0;
  const int oct_w = octave[0] ? text_width(octave, s_font_octave) : 0;
  const int suffix_w = acc_w > oct_w ? acc_w : oct_w;
  const int block_w = note.size.w + (suffix_w ? SUFFIX_GAP + suffix_w : 0);
  const int x = center_x - block_w / 2;
  const int suffix_x = x + note.size.w + SUFFIX_GAP;

  draw_bitmap(ctx, s_note_bitmap, x, image_top);
  if (s_acc_bitmap) {
    draw_bitmap(ctx, s_acc_bitmap, suffix_x, top - NOTE_CAP / 20);
  }
  if (octave[0]) {
    graphics_context_set_text_color(ctx, THEME_FOREGROUND);
    draw_text(ctx, octave, s_font_octave, FONT_OCTAVE_H, suffix_x, baseline + OCTAVE_DROP,
              suffix_w + 4, GTextAlignmentLeft);
  }
  return baseline;
}

// "+3" in the cents font followed by "ct" in the small font, centred.
static void draw_cents(GContext *ctx, int center_x, int baseline) {
  char value[8];
  format_cents(value, sizeof(value), s_view.cents);
  const char *unit = "ct";
  const int value_w = text_width(value, s_font_cents);
  const int unit_w = text_width(unit, s_font_small);
  const int x = center_x - (value_w + 3 + unit_w) / 2;
  const int magnitude = s_view.cents < 0 ? -s_view.cents : s_view.cents;
  const int bar = notes_deviation_bar(magnitude, s_settings->precision);
  graphics_context_set_text_color(ctx, s_view.stale ? THEME_SECONDARY : bar_color(bar));
  draw_text(ctx, value, s_font_cents, FONT_CENTS_H, x, baseline, value_w + 4, GTextAlignmentLeft);
  draw_text(ctx, unit, s_font_small, FONT_SMALL_H, x + value_w + 3, baseline, unit_w + 4,
            GTextAlignmentLeft);
}

static void draw_message(GContext *ctx, GRect area) {
  const GFont font = fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD);
  const GRect box = grect_inset(area, GEdgeInsets(0, PBL_IF_ROUND_ELSE(PBL_DISPLAY_WIDTH / 8, 8)));
  const GSize size = graphics_text_layout_get_content_size(s_view.message, font, box,
                                                           GTextOverflowModeWordWrap,
                                                           GTextAlignmentCenter);
  graphics_context_set_text_color(ctx, THEME_FOREGROUND);
  graphics_draw_text(ctx, s_view.message, font,
                     GRect(box.origin.x, box.origin.y + (box.size.h - size.h) / 2 - 4, box.size.w,
                           size.h + 8),
                     GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
}

static void update_proc(Layer *layer, GContext *ctx) {
  const GRect full = layer_get_bounds(layer);
  // The system's Listening indicator covers the bottom while the mic is on.
  const GRect area = layer_get_unobstructed_bounds(layer);
  graphics_context_set_fill_color(ctx, THEME_BACKGROUND);
  graphics_fill_rect(ctx, full, 0, GCornerNone);

  if (s_view.message) {
    draw_message(ctx, area);
    return;
  }

  const bool advanced = s_settings->advanced;
  GRect accessory = GRectZero;
  const bool has_accessory = layer_get_mic_accessory_bounds(layer, &accessory);
  // Round screens have no room beside the indicator: put the detected
  // frequency under the cents instead.
  const bool hz_below = advanced && PBL_IF_ROUND_ELSE(true, false);

  // Stack heights, top of the note's capitals to the last baseline.
  const int note_h = NOTE_CAP + (advanced ? OCTAVE_DROP : 0);
  const int target_h = advanced ? GAP + DIGIT_H(FONT_SMALL_H) : 0;
  const int bars_h = (advanced ? GAP : 2 * GAP) + BAR_H_GREEN;
  const int cents_h = advanced ? GAP + DIGIT_H(FONT_CENTS_H) : 0;
  const int hz_h = hz_below ? GAP + DIGIT_H(FONT_SMALL_H) : 0;
  const int total = note_h + target_h + bars_h + cents_h + hz_h;
  int y = area.origin.y + (area.size.h - total) / 2;
  const int cx = full.size.w / 2;

  if (s_view.active) {
    draw_note(ctx, cx, y);
  }
  y += note_h;

  char text[16];
  if (advanced) {
    y += target_h;
    if (s_view.active) {
      format_hz(text, sizeof(text), s_view.target_x100);
      graphics_context_set_text_color(ctx, THEME_SECONDARY);
      draw_text(ctx, text, s_font_small, FONT_SMALL_H, 0, y, full.size.w, GTextAlignmentCenter);
    }
  }

  draw_bars(ctx, cx, y + bars_h - BAR_H_GREEN / 2);
  y += bars_h;

  if (!advanced || !s_view.active) {
    return;
  }
  y += cents_h;
  draw_cents(ctx, cx, y);

  format_hz(text, sizeof(text), s_view.hz_x100);
  graphics_context_set_text_color(ctx, s_view.stale ? THEME_SECONDARY : THEME_FOREGROUND);
  if (hz_below) {
    y += hz_h;
    draw_text(ctx, text, s_font_small, FONT_SMALL_H, 0, y, full.size.w, GTextAlignmentCenter);
  } else if (has_accessory) {
    const int baseline = accessory.origin.y + (accessory.size.h + DIGIT_H(FONT_SMALL_H)) / 2;
    draw_text(ctx, text, s_font_small, FONT_SMALL_H, accessory.origin.x, baseline,
              accessory.size.w, GTextAlignmentCenter);
  }
}

// ---------------------------------------------------------------------------

static void area_changed(AnimationProgress progress, void *context) {
  layer_mark_dirty(s_layer);
}

void ui_create(Window *window, const Settings *settings) {
  s_settings = settings;
  s_view = (TunerView){.anim = UI_NO_ANIM};
  s_font_small = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_SMALL));
  s_font_cents = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_CENTS));
  s_font_octave = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_OCTAVE));

  Layer *root = window_get_root_layer(window);
  s_layer = layer_create(layer_get_bounds(root));
  layer_set_update_proc(s_layer, update_proc);
  layer_add_child(root, s_layer);
  unobstructed_area_service_subscribe((UnobstructedAreaHandlers){.change = area_changed}, NULL);
}

void ui_destroy(void) {
  unobstructed_area_service_unsubscribe();
  layer_destroy(s_layer);
  s_layer = NULL;
  s_note_bitmap = load(s_note_bitmap, &s_note_id, 0);
  s_acc_bitmap = load(s_acc_bitmap, &s_acc_id, 0);
  fonts_unload_custom_font(s_font_small);
  fonts_unload_custom_font(s_font_cents);
  fonts_unload_custom_font(s_font_octave);
}

void ui_update(const TunerView *view) {
  if (memcmp(view, &s_view, sizeof(s_view)) == 0) {
    return;
  }
  s_view = *view;
  if (s_layer) {
    layer_mark_dirty(s_layer);
  }
}

void ui_refresh(void) {
  if (s_layer) {
    layer_mark_dirty(s_layer);
  }
}
