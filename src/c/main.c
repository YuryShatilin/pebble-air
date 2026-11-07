// Watch side: renders the AQI supplied by the phone. Select refreshes.
// The last reading is kept with its measurement time, so old data always shows its age.
//
// PebbleKit JS only runs while the app is in the foreground, so a background refresh is the
// app waking itself up, fetching, saving and returning to the watchface.
#include <pebble.h>

#define PERSIST_KEY_LEGACY_READING 1
#define PERSIST_KEY_READING 2
#define PERSIST_KEY_WAKEUP_ID 3
#define PERSIST_KEY_STATION_LABEL 4
#define RESPONSE_TIMEOUT_MS 30000
#define BACKGROUND_REFRESH_INTERVAL_SECONDS (30 * 60)
#define WAKEUP_RETRY_STEP_SECONDS (2 * 60)
#define WAKEUP_SCHEDULE_ATTEMPTS 5

enum {
  ERROR_NONE = 0,
  ERROR_NETWORK = 1,
  ERROR_STATION_OFFLINE = 2,
  ERROR_BAD_DATA = 3,
  ERROR_NO_PHONE = 4
};

typedef struct {
  int32_t aqi;
  int32_t measured_at;
} Reading;

typedef struct {
  const char *label;
  GColor background;
  GColor text;
} Category;

static Window *s_window;
static TextLayer *s_title_layer;
static TextLayer *s_value_layer;
static TextLayer *s_unit_layer;
static TextLayer *s_category_layer;
static TextLayer *s_status_layer;
static AppTimer *s_response_timer;

static Reading s_reading;
static bool s_has_reading;
static bool s_refreshing;
static bool s_js_ready;
static bool s_send_pending;
static bool s_background_run;
static int s_error = ERROR_NONE;

static char s_value_text[16];
static char s_status_text[48];
// Empty when the reading comes from the home station.
static char s_station_label[32];

static Category category_for(int32_t aqi) {
  if (aqi <= 50) return (Category){"Good", GColorGreen, GColorBlack};
  if (aqi <= 100) return (Category){"Moderate", GColorYellow, GColorBlack};
  if (aqi <= 150) return (Category){"Sensitive groups", GColorOrange, GColorBlack};
  if (aqi <= 200) return (Category){"Unhealthy", GColorRed, GColorWhite};
  if (aqi <= 300) return (Category){"Very unhealthy", GColorPurple, GColorWhite};
  return (Category){"Hazardous", GColorBulgarianRose, GColorWhite};
}

static const char *error_text(int error) {
  switch (error) {
    case ERROR_NETWORK: return "No network";
    case ERROR_STATION_OFFLINE: return "Station offline";
    case ERROR_BAD_DATA: return "Bad data";
    case ERROR_NO_PHONE: return "No phone";
    default: return "";
  }
}

static void format_age(char *buffer, size_t size) {
  int32_t age_seconds = (int32_t)time(NULL) - s_reading.measured_at;
  if (age_seconds < 60) {
    snprintf(buffer, size, "just now");
  } else if (age_seconds < 3600) {
    snprintf(buffer, size, "%d min ago", (int)(age_seconds / 60));
  } else if (age_seconds < 86400) {
    snprintf(buffer, size, "%dh %dm ago", (int)(age_seconds / 3600), (int)(age_seconds % 3600 / 60));
  } else {
    snprintf(buffer, size, "%d days ago", (int)(age_seconds / 86400));
  }
}

static void render_status(void) {
  char age[20] = "";
  if (s_has_reading) {
    format_age(age, sizeof(age));
  }
  const char *first_line = s_refreshing ? "Updating..." : error_text(s_error);
  if (first_line[0] == '\0' && !s_has_reading) {
    first_line = "Press Select";
  }
  const char *separator = (first_line[0] != '\0' && age[0] != '\0') ? "\n" : "";
  snprintf(s_status_text, sizeof(s_status_text), "%s%s%s", first_line, separator, age);
  text_layer_set_text(s_status_layer, s_status_text);
}

static void render(void) {
  text_layer_set_text(s_title_layer, s_station_label[0] != '\0' ? s_station_label : "Air Quality");

  if (!s_has_reading) {
    window_set_background_color(s_window, GColorWhite);
    text_layer_set_text_color(s_title_layer, GColorBlack);
    text_layer_set_text_color(s_value_layer, GColorBlack);
    text_layer_set_text_color(s_unit_layer, GColorBlack);
    text_layer_set_text_color(s_category_layer, GColorBlack);
    text_layer_set_text_color(s_status_layer, GColorBlack);
    text_layer_set_font(s_value_layer, fonts_get_system_font(FONT_KEY_BITHAM_42_BOLD));
    text_layer_set_text(s_value_layer, "--");
    text_layer_set_text(s_unit_layer, "US AQI");
    text_layer_set_text(s_category_layer, "");
    render_status();
    return;
  }

  Category category = category_for(s_reading.aqi);
  window_set_background_color(s_window, category.background);
  text_layer_set_text_color(s_title_layer, category.text);
  text_layer_set_text_color(s_value_layer, category.text);
  text_layer_set_text_color(s_unit_layer, category.text);
  text_layer_set_text_color(s_category_layer, category.text);
  text_layer_set_text_color(s_status_layer, category.text);

  snprintf(s_value_text, sizeof(s_value_text), "%d", (int)s_reading.aqi);
  text_layer_set_font(s_value_layer, fonts_get_system_font(FONT_KEY_LECO_60_NUMBERS_AM_PM));
  text_layer_set_text(s_unit_layer, "US AQI");
  text_layer_set_text(s_value_layer, s_value_text);
  text_layer_set_text(s_category_layer, category.label);
  render_status();
}

static void return_to_watchface(void) {
  s_background_run = false;
  exit_reason_set(APP_EXIT_ACTION_PERFORMED_SUCCESSFULLY);
  window_stack_pop_all(false);
}

static void finish_refresh(int error) {
  APP_LOG(APP_LOG_LEVEL_INFO, "Refresh finished, error: %d, aqi: %d, measured at: %d",
          error, (int)s_reading.aqi, (int)s_reading.measured_at);
  s_refreshing = false;
  s_send_pending = false;
  s_error = error;
  if (s_response_timer != NULL) {
    app_timer_cancel(s_response_timer);
    s_response_timer = NULL;
  }
  render();
  if (s_background_run) {
    return_to_watchface();
  }
}

static void response_timeout(void *context) {
  s_response_timer = NULL;
  finish_refresh(ERROR_NO_PHONE);
}

static void start_waiting(void) {
  s_refreshing = true;
  if (s_response_timer != NULL) {
    app_timer_cancel(s_response_timer);
  }
  s_response_timer = app_timer_register(RESPONSE_TIMEOUT_MS, response_timeout, NULL);
  render();
}

static void send_refresh_request(void) {
  s_send_pending = false;

  DictionaryIterator *out;
  AppMessageResult result = app_message_outbox_begin(&out);
  if (result != APP_MSG_OK) {
    finish_refresh(ERROR_NO_PHONE);
    return;
  }
  dict_write_int32(out, MESSAGE_KEY_RequestRefresh, 1);
  result = app_message_outbox_send();
  if (result != APP_MSG_OK) {
    finish_refresh(ERROR_NO_PHONE);
  }
}

static void request_refresh(void) {
  if (s_refreshing) {
    return;
  }
  start_waiting();
  if (s_js_ready) {
    send_refresh_request();
  } else {
    s_send_pending = true;
  }
}

static void inbox_received(DictionaryIterator *iterator, void *context) {
  if (dict_find(iterator, MESSAGE_KEY_JsReady) != NULL) {
    s_js_ready = true;
    if (s_send_pending) {
      send_refresh_request();
    }
    return;
  }

  Tuple *error = dict_find(iterator, MESSAGE_KEY_ErrorCode);
  if (error != NULL) {
    finish_refresh((int)error->value->int32);
    return;
  }

  Tuple *aqi = dict_find(iterator, MESSAGE_KEY_Aqi);
  Tuple *measured_at = dict_find(iterator, MESSAGE_KEY_MeasuredAt);
  if (aqi == NULL || measured_at == NULL) {
    finish_refresh(ERROR_BAD_DATA);
    return;
  }

  s_reading = (Reading){aqi->value->int32, measured_at->value->int32};
  s_has_reading = true;
  persist_write_data(PERSIST_KEY_READING, &s_reading, sizeof(s_reading));

  Tuple *label = dict_find(iterator, MESSAGE_KEY_StationLabel);
  if (label != NULL) {
    strncpy(s_station_label, label->value->cstring, sizeof(s_station_label) - 1);
    s_station_label[sizeof(s_station_label) - 1] = '\0';
    persist_write_string(PERSIST_KEY_STATION_LABEL, s_station_label);
  } else {
    s_station_label[0] = '\0';
    persist_delete(PERSIST_KEY_STATION_LABEL);
  }

  finish_refresh(ERROR_NONE);
}

static void inbox_dropped(AppMessageResult reason, void *context) {
  APP_LOG(APP_LOG_LEVEL_ERROR, "Inbox dropped: %d", (int)reason);
  finish_refresh(ERROR_BAD_DATA);
}

static void outbox_failed(DictionaryIterator *iterator, AppMessageResult reason, void *context) {
  APP_LOG(APP_LOG_LEVEL_ERROR, "Outbox failed: %d", (int)reason);
  finish_refresh(ERROR_NO_PHONE);
}

static void select_click(ClickRecognizerRef recognizer, void *context) {
  // Pressing a button during a background run means the user wants the app to stay open.
  s_background_run = false;
  request_refresh();
}

static void up_click(ClickRecognizerRef recognizer, void *context) {
  window_stack_pop_all(true);
}

static void click_config(void *context) {
  window_single_click_subscribe(BUTTON_ID_UP, up_click);
  window_single_click_subscribe(BUTTON_ID_SELECT, select_click);
}

static void tick(struct tm *tick_time, TimeUnits units_changed) {
  render_status();
}

// Only schedules when nothing is pending: rescheduling on every launch would keep pushing the
// wakeup back for anyone opening the app more often than the interval.
static void ensure_wakeup_scheduled(void) {
  time_t scheduled_at;
  if (persist_exists(PERSIST_KEY_WAKEUP_ID) &&
      wakeup_query(persist_read_int(PERSIST_KEY_WAKEUP_ID), &scheduled_at)) {
    APP_LOG(APP_LOG_LEVEL_INFO, "Wakeup pending at %d", (int)scheduled_at);
    return;
  }

  time_t when = time(NULL) + BACKGROUND_REFRESH_INTERVAL_SECONDS;
  for (int attempt = 0; attempt < WAKEUP_SCHEDULE_ATTEMPTS; attempt++) {
    WakeupId id = wakeup_schedule(when, 0, false);
    if (id >= 0) {
      persist_write_int(PERSIST_KEY_WAKEUP_ID, id);
      APP_LOG(APP_LOG_LEVEL_INFO, "Wakeup scheduled at %d", (int)when);
      return;
    }
    APP_LOG(APP_LOG_LEVEL_ERROR, "Could not schedule wakeup: %d", (int)id);
    if (id != E_RANGE) {
      return;
    }
    // E_RANGE: the slot is too close to a wakeup of another app.
    when += WAKEUP_RETRY_STEP_SECONDS;
  }
}

static void wakeup_fired(WakeupId id, int32_t cookie) {
  ensure_wakeup_scheduled();
  request_refresh();
}

static TextLayer *add_text_layer(Layer *root, GRect frame, const char *font_key) {
  TextLayer *layer = text_layer_create(frame);
  text_layer_set_background_color(layer, GColorClear);
  text_layer_set_text_alignment(layer, GTextAlignmentCenter);
  text_layer_set_font(layer, fonts_get_system_font(font_key));
  layer_add_child(root, text_layer_get_layer(layer));
  return layer;
}

static void window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  int width = layer_get_bounds(root).size.w;

  s_title_layer = add_text_layer(root, GRect(0, 8, width, 30), FONT_KEY_GOTHIC_24_BOLD);
  s_value_layer = add_text_layer(root, GRect(0, 48, width, 72), FONT_KEY_BITHAM_42_BOLD);
  s_unit_layer = add_text_layer(root, GRect(0, 118, width, 24), FONT_KEY_GOTHIC_18);
  s_category_layer = add_text_layer(root, GRect(0, 142, width, 30), FONT_KEY_GOTHIC_24_BOLD);
  s_status_layer = add_text_layer(root, GRect(0, 178, width, 46), FONT_KEY_GOTHIC_18);
  render();
}

static void window_unload(Window *window) {
  text_layer_destroy(s_title_layer);
  text_layer_destroy(s_value_layer);
  text_layer_destroy(s_unit_layer);
  text_layer_destroy(s_category_layer);
  text_layer_destroy(s_status_layer);
}

static void init(void) {
  persist_delete(PERSIST_KEY_LEGACY_READING);
  if (persist_exists(PERSIST_KEY_READING) &&
      persist_read_data(PERSIST_KEY_READING, &s_reading, sizeof(s_reading)) == (int)sizeof(s_reading)) {
    s_has_reading = true;
  }
  if (persist_exists(PERSIST_KEY_STATION_LABEL)) {
    persist_read_string(PERSIST_KEY_STATION_LABEL, s_station_label, sizeof(s_station_label));
  }

  s_window = window_create();
  window_set_click_config_provider(s_window, click_config);
  window_set_window_handlers(s_window, (WindowHandlers){.load = window_load, .unload = window_unload});

  app_message_register_inbox_received(inbox_received);
  app_message_register_inbox_dropped(inbox_dropped);
  app_message_register_outbox_failed(outbox_failed);
  app_message_open(128, 64);

  s_background_run = launch_reason() == APP_LAUNCH_WAKEUP;
  APP_LOG(APP_LOG_LEVEL_INFO, "Started, background run: %d", (int)s_background_run);
  window_stack_push(s_window, !s_background_run);
  tick_timer_service_subscribe(MINUTE_UNIT, tick);

  wakeup_service_subscribe(wakeup_fired);
  ensure_wakeup_scheduled();

  if (s_background_run) {
    if (connection_service_peek_pebble_app_connection()) {
      request_refresh();
    } else {
      return_to_watchface();
    }
  }
}

static void deinit(void) {
  tick_timer_service_unsubscribe();
  window_destroy(s_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
