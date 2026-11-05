// Watch side: renders the AQI supplied by the phone. Select refreshes.
// The last reading is kept with its measurement time, so old data always shows its age.
#include <pebble.h>

#define PERSIST_KEY_LEGACY_READING 1
#define PERSIST_KEY_READING 2
#define RESPONSE_TIMEOUT_MS 30000
#define REFRESH_INTERVAL_MINUTES 15

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
static int s_error = ERROR_NONE;
static int s_minutes_since_refresh;

static char s_value_text[16];
static char s_status_text[48];

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
  const char *separator = (first_line[0] != '\0' && age[0] != '\0') ? "\n" : "";
  snprintf(s_status_text, sizeof(s_status_text), "%s%s%s", first_line, separator, age);
  text_layer_set_text(s_status_layer, s_status_text);
}

static void render(void) {
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

static void finish_refresh(int error) {
  s_refreshing = false;
  s_error = error;
  if (s_response_timer != NULL) {
    app_timer_cancel(s_response_timer);
    s_response_timer = NULL;
  }
  render();
}

static void response_timeout(void *context) {
  s_response_timer = NULL;
  finish_refresh(ERROR_NO_PHONE);
}

static void start_waiting(void) {
  s_refreshing = true;
  s_minutes_since_refresh = 0;
  if (s_response_timer != NULL) {
    app_timer_cancel(s_response_timer);
  }
  s_response_timer = app_timer_register(RESPONSE_TIMEOUT_MS, response_timeout, NULL);
  render();
}

static void request_refresh(void) {
  if (s_refreshing) {
    return;
  }
  start_waiting();

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

static void inbox_received(DictionaryIterator *iterator, void *context) {
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
  s_minutes_since_refresh++;
  if (s_minutes_since_refresh >= REFRESH_INTERVAL_MINUTES) {
    request_refresh();
  } else {
    render_status();
  }
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
  text_layer_set_text(s_title_layer, "Air Quality");
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

  s_window = window_create();
  window_set_click_config_provider(s_window, click_config);
  window_set_window_handlers(s_window, (WindowHandlers){.load = window_load, .unload = window_unload});

  app_message_register_inbox_received(inbox_received);
  app_message_register_inbox_dropped(inbox_dropped);
  app_message_register_outbox_failed(outbox_failed);
  app_message_open(128, 64);

  window_stack_push(s_window, true);
  tick_timer_service_subscribe(MINUTE_UNIT, tick);

  // The phone fetches by itself on its 'ready' event, so here we only wait for the answer.
  start_waiting();
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
