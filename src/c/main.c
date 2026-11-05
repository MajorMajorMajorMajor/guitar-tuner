// Guitar tuner for Pebble: local 16 kHz microphone capture, YIN pitch
// detection on the watch, and a Chroma-style display.

#include <pebble.h>

#include "notes.h"
#include "settings.h"
#include "theme.h"
#include "tuner.h"
#include "ui.h"

// Start-up sweep, as in Chroma: bars light from the outside in and back out.
#define ANIM_STEP_MS 120
#define ANIM_STEPS 12
// Keep the backlight on while notes keep coming in.
#define LIGHT_INTERVAL_MS 2500

static Window *s_window;
static Settings s_settings;
static Tuner s_tuner;
static bool s_capturing;
static const char *s_message;
static AppTimer *s_anim_timer;
static int s_anim_step = -1;
static uint32_t s_last_light_ms;

static uint32_t now_ms(void) {
  time_t s;
  uint16_t ms;
  time_ms(&s, &ms);
  return (uint32_t)s * 1000u + ms;
}

static void show(void) {
  TunerView view;
  memset(&view, 0, sizeof(view));
  view.anim = UI_NO_ANIM;
  view.message = s_message;
  const TunerReading *r = &s_tuner.reading;
  if (r->active) {
    int cents = notes_round(r->cents);
    cents = cents > 50 ? 50 : cents < -50 ? -50 : cents;
    view.active = true;
    view.stale = !r->fresh;
    view.midi = r->midi;
    view.cents = cents;
    view.hz_x100 = (int)(r->hz * 100.0f + 0.5f);
    view.target_x100 = (int)(r->target_hz * 100.0f + 0.5f);
  } else if (s_anim_step >= 0) {
    // 5 4 3 2 1 0 0 1 2 3 4 5
    view.anim = s_anim_step < ANIM_STEPS / 2 ? DEVIATION_BARS_HALF - s_anim_step
                                             : s_anim_step - ANIM_STEPS / 2;
  }
  ui_update(&view);
}

// ---------------------------------------------------------------------------
// Start-up animation

static void anim_tick(void *context) {
  s_anim_timer = NULL;
  if (++s_anim_step >= ANIM_STEPS || s_tuner.reading.active) {
    s_anim_step = -1;
  } else {
    s_anim_timer = app_timer_register(ANIM_STEP_MS, anim_tick, NULL);
  }
  show();
}

static void anim_start(void) {
  if (s_anim_timer) {
    app_timer_cancel(s_anim_timer);
  }
  s_anim_step = 0;
  s_anim_timer = app_timer_register(ANIM_STEP_MS, anim_tick, NULL);
  show();
}

// ---------------------------------------------------------------------------
// Microphone

static void on_audio(const int16_t *samples, size_t count, void *context) {
  if (!tuner_feed(&s_tuner, samples, count)) {
    return;
  }
  const TunerReading *r = &s_tuner.reading;
  if (r->active && r->fresh) {
    const uint32_t t = now_ms();
    if (t - s_last_light_ms > LIGHT_INTERVAL_MS) {
      s_last_light_ms = t;
      light_enable_interaction();
    }
  }
  show();
}

static void on_stopped(MicCaptureStopReason reason, void *context) {
  s_capturing = false;
  // Losing focus (a notification) or dictation: capture restarts when the
  // app is back in the foreground.
  if (reason == MicCaptureStopReasonPermissionRevoked) {
    s_message = "Microphone access was turned off for Guitar Tuner.";
  }
  show();
}

static const char *start_error(MicCaptureStartResult result) {
  switch (result) {
    case MicCaptureStartErrPermissionDenied:
      return "Allow the microphone for Guitar Tuner in the Pebble app.";
    case MicCaptureStartErrUnavailable:
      return "This watch can't listen locally. Update its firmware.";
    case MicCaptureStartErrBusy:
      return "The microphone is busy.";
    case MicCaptureStartErrNoMemory:
      return "Not enough memory to listen.";
    default:
      return NULL;  // not in the foreground yet: retried on focus
  }
}

static void capture_start(void) {
  if (s_capturing || window_stack_get_top_window() != s_window) {
    return;
  }
  tuner_init(&s_tuner, s_settings.sensitivity);
  const MicCaptureStartResult result = mic_capture_start(
      (MicCaptureHandlers){
          .data = on_audio,
          .stopped = on_stopped,
      },
      NULL);
  s_capturing = result == MicCaptureStartOk;
  s_message = s_capturing ? NULL : start_error(result);
  if (result != MicCaptureStartOk) {
    APP_LOG(APP_LOG_LEVEL_WARNING, "mic_capture_start: %d", (int)result);
  }
  show();
}

static void capture_stop(void) {
  if (s_capturing) {
    mic_capture_stop();
    s_capturing = false;
  }
}

static void focus_changed(bool in_focus) {
  if (in_focus) {
    capture_start();
  }
}

// ---------------------------------------------------------------------------
// Window

static void settings_closed(void) {
  tuner_set_sensitivity(&s_tuner, s_settings.sensitivity);
  ui_refresh();
}

static void select_click(ClickRecognizerRef recognizer, void *context) {
  settings_window_push(&s_settings, settings_closed);
}

static void click_config(void *context) {
  window_single_click_subscribe(BUTTON_ID_SELECT, select_click);
}

static void window_load(Window *window) {
  ui_create(window, &s_settings);
}

static void window_appear(Window *window) {
  capture_start();
}

static void window_disappear(Window *window) {
  capture_stop();
}

static void window_unload(Window *window) {
  ui_destroy();
}

static void init(void) {
  settings_load(&s_settings);
  tuner_init(&s_tuner, s_settings.sensitivity);
  s_window = window_create();
  window_set_background_color(s_window, THEME_BACKGROUND);
  window_set_click_config_provider(s_window, click_config);
  window_set_window_handlers(s_window, (WindowHandlers){
                                           .load = window_load,
                                           .appear = window_appear,
                                           .disappear = window_disappear,
                                           .unload = window_unload,
                                       });
  window_stack_push(s_window, true);
  app_focus_service_subscribe(focus_changed);
  anim_start();
}

static void deinit(void) {
  app_focus_service_unsubscribe();
  if (s_anim_timer) {
    app_timer_cancel(s_anim_timer);
  }
  capture_stop();
  window_destroy(s_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
