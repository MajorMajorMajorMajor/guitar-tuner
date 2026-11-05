#include "settings.h"

#include "theme.h"

enum {
  KEY_ADVANCED = 1,
  KEY_NOTATION = 2,
  KEY_ACCIDENTAL = 3,
  KEY_PRECISION = 4,
  KEY_SENSITIVITY = 5,
};

enum {
  ROW_ADVANCED,
  ROW_NOTATION,
  ROW_ACCIDENTAL,
  ROW_PRECISION,
  ROW_SENSITIVITY,
  ROW_COUNT,
};

#define MAX_PRECISION 5

static int read_int(uint32_t key, int fallback, int min, int max) {
  if (!persist_exists(key)) {
    return fallback;
  }
  const int value = persist_read_int(key);
  return value < min || value > max ? fallback : value;
}

void settings_load(Settings *settings) {
  settings->advanced = read_int(KEY_ADVANCED, 1, 0, 1) != 0;
  settings->notation = (Notation)read_int(KEY_NOTATION, NOTATION_LETTER, 0, 1);
  settings->accidental = (AccidentalStyle)read_int(KEY_ACCIDENTAL, ACCIDENTAL_STYLE_SHARP, 0, 1);
  settings->precision = read_int(KEY_PRECISION, 2, 0, MAX_PRECISION);
  settings->sensitivity = (TunerSensitivity)read_int(KEY_SENSITIVITY, TUNER_SENSITIVITY_NORMAL,
                                                     TUNER_SENSITIVITY_LOW,
                                                     TUNER_SENSITIVITY_HIGH);
}

static void save(const Settings *settings) {
  persist_write_int(KEY_ADVANCED, settings->advanced ? 1 : 0);
  persist_write_int(KEY_NOTATION, settings->notation);
  persist_write_int(KEY_ACCIDENTAL, settings->accidental);
  persist_write_int(KEY_PRECISION, settings->precision);
  persist_write_int(KEY_SENSITIVITY, settings->sensitivity);
}

// ---------------------------------------------------------------------------
// Menu

static Window *s_window;
static MenuLayer *s_menu;
static Settings *s_settings;
static void (*s_on_close)(void);
static char s_precision_text[32];

static uint16_t get_num_rows(MenuLayer *menu, uint16_t section, void *ctx) {
  return ROW_COUNT;
}

static void draw_row(GContext *ctx, const Layer *cell, MenuIndex *index, void *data) {
  const char *title = "";
  const char *value = "";
  switch (index->row) {
    case ROW_ADVANCED:
      title = "Advanced mode";
      value = s_settings->advanced ? "On: octave, Hz, cents" : "Off: note only";
      break;
    case ROW_NOTATION:
      title = "Notation";
      value = s_settings->notation == NOTATION_SOLFEGE ? "Do Re Mi" : "A B C";
      break;
    case ROW_ACCIDENTAL:
      title = "Accidentals";
      value = s_settings->accidental == ACCIDENTAL_STYLE_FLAT ? "Flat" : "Sharp";
      break;
    case ROW_PRECISION:
      snprintf(s_precision_text, sizeof(s_precision_text), "In tune within \xc2\xb1%d ct",
               s_settings->precision);
      title = "Precision";
      value = s_precision_text;
      break;
    case ROW_SENSITIVITY:
      title = "Sensitivity";
      value = s_settings->sensitivity == TUNER_SENSITIVITY_LOW    ? "Low: noisy rooms"
              : s_settings->sensitivity == TUNER_SENSITIVITY_HIGH ? "High: soft playing"
                                                                   : "Normal";
      break;
  }
  menu_cell_basic_draw(ctx, cell, title, value, NULL);
}

static int16_t get_cell_height(MenuLayer *menu, MenuIndex *index, void *ctx) {
  return PBL_IF_ROUND_ELSE(menu_layer_is_index_selected(menu, index) ? 64 : 44, 56);
}

static void select_click(MenuLayer *menu, MenuIndex *index, void *ctx) {
  switch (index->row) {
    case ROW_ADVANCED:
      s_settings->advanced = !s_settings->advanced;
      break;
    case ROW_NOTATION:
      s_settings->notation =
          s_settings->notation == NOTATION_LETTER ? NOTATION_SOLFEGE : NOTATION_LETTER;
      break;
    case ROW_ACCIDENTAL:
      s_settings->accidental = s_settings->accidental == ACCIDENTAL_STYLE_SHARP
                                   ? ACCIDENTAL_STYLE_FLAT
                                   : ACCIDENTAL_STYLE_SHARP;
      break;
    case ROW_PRECISION:
      s_settings->precision = (s_settings->precision + 1) % (MAX_PRECISION + 1);
      break;
    case ROW_SENSITIVITY:
      s_settings->sensitivity = (TunerSensitivity)((s_settings->sensitivity + 1) %
                                                   (TUNER_SENSITIVITY_HIGH + 1));
      break;
  }
  save(s_settings);
  menu_layer_reload_data(menu);
}

static void window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  s_menu = menu_layer_create(layer_get_bounds(root));
  menu_layer_set_callbacks(s_menu, NULL,
                           (MenuLayerCallbacks){
                               .get_num_rows = get_num_rows,
                               .draw_row = draw_row,
                               .get_cell_height = get_cell_height,
                               .select_click = select_click,
                           });
  menu_layer_set_normal_colors(s_menu, THEME_BACKGROUND, GColorWhite);
  menu_layer_set_highlight_colors(s_menu, THEME_ACCENT, GColorWhite);
  menu_layer_set_click_config_onto_window(s_menu, window);
  layer_add_child(root, menu_layer_get_layer(s_menu));
}

static void window_unload(Window *window) {
  menu_layer_destroy(s_menu);
  s_menu = NULL;
  window_destroy(s_window);
  s_window = NULL;
  if (s_on_close) {
    s_on_close();
  }
}

void settings_window_push(Settings *settings, void (*on_close)(void)) {
  s_settings = settings;
  s_on_close = on_close;
  s_window = window_create();
  window_set_background_color(s_window, THEME_BACKGROUND);
  window_set_window_handlers(s_window, (WindowHandlers){
                                           .load = window_load,
                                           .unload = window_unload,
                                       });
  window_stack_push(s_window, true);
}
