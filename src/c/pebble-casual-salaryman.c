#include <pebble.h>
#include <stdlib.h>
#include <string.h>

#define SETTINGS_KEY 1
#define GHOST_COLOR GColorLightGray

#define CLOCK_FORMAT_SYSTEM 0
#define CLOCK_FORMAT_24H 1
#define CLOCK_FORMAT_12H 2

#define TEMP_UNIT_CELSIUS 0
#define TEMP_UNIT_FAHRENHEIT 1

#define FIRST_DAY_MONDAY 0
#define FIRST_DAY_SUNDAY 1

#define DATE_FORMAT_DD_MM 0
#define DATE_FORMAT_MM_DD 1

// Persisted as-is. New fields go at the end: an older, shorter saved copy then
// only overwrites the fields it has, and the rest keep their defaults.
typedef struct ClaySettings {
  bool ShowSeconds;
  uint8_t ClockFormat;     // CLOCK_FORMAT_*
  uint8_t TemperatureUnit; // TEMP_UNIT_*
  uint8_t FirstDayOfWeek;  // FIRST_DAY_*
  uint8_t DateFormat;      // DATE_FORMAT_*
  // Seconds stop ticking from NightStartHour up to NightEndHour (wrapping past
  // midnight), so the watch wakes once a minute instead of once a second.
  bool HideSecondsAtNight;
  uint8_t NightStartHour; // 0-23
  uint8_t NightEndHour;   // 0-23
} ClaySettings;

static Window *s_window;

static Layer *s_chrome_layer;
static TextLayer *s_week_label_layer;
static Layer *s_week_value_layer;
static Layer *s_weekday_strip_layer;
static Layer *s_middle_frame_layer;
static TextLayer *s_battery_label_layer;
static Layer *s_battery_icon_layer;
static Layer *s_date_layer;
static Layer *s_time_big_layer;
static TextLayer *s_ampm_layer;
static Layer *s_time_sec_layer;
static TextLayer *s_alarm_label_layer;
static Layer *s_alarm_value_layer;
static Layer *s_bottom_band_layer;
static Layer *s_weather_temp_layer;
static Layer *s_header_bg_layer;
static TextLayer *s_header_logo_layer;
static TextLayer *s_header_model_layer;
static Layer *s_footer_bg_layer;
static Layer *s_content_frame_layer;
static Layer *s_button_hints_layer;

static GFont s_font_weekday;
static GFont s_font_brand;

static ClaySettings s_settings;
static int s_battery_percent = 100;
static int s_last_mday = -1;
static int s_today_index = 0; // 0=Mon .. 6=Sun

// Current temperature in tenths of a degree Celsius, as sent by the phone.
#define WEATHER_TEMP_NONE INT16_MIN
static int s_weather_temp = WEATHER_TEMP_NONE;
static int s_weather_condition = -1; // WMO weather code, -1 = unknown/no data

// Y coordinate where each row starts. Every layer below is positioned as
// ROW_*_Y + <offset within the row>, so moving a whole row is a one-line
// change.
#define ROW_TOP_Y 41
#define ROW_MIDDLE_Y 70
#define MIDDLE_FRAME_H 103
#define ROW_BOTTOM_Y (ROW_MIDDLE_Y + MIDDLE_FRAME_H)

// Inside the middle frame: time on top, then a divider line, then date | week.
// Screen coordinates; the gaps around each row are kept even.
#define TIME_Y (ROW_MIDDLE_Y + 13)
#define MIDDLE_DIVIDER_Y (ROW_MIDDLE_Y + 63)
#define DATE_ROW_Y (ROW_MIDDLE_Y + 71)
#define DATE_WEEK_DIVIDER_X 102
#define FOOTER_Y 212

// Red-bordered white content area between the header and footer, wrapped in a
// blue border with a black gap. On the left and right the blue border and gap
// fall just off-screen, so only the red shows there.
#define CONTENT_FRAME_Y 38
#define CONTENT_FRAME_H 170
#define CONTENT_FRAME_RADIUS 16
#define CONTENT_FRAME_BORDER 3
#define OUTER_FRAME_BORDER 2
#define OUTER_FRAME_GAP 2
#define OUTER_FRAME_INSET (OUTER_FRAME_BORDER + OUTER_FRAME_GAP)

// Nearest colors in Pebble's 64-color palette (each channel 00/55/AA/FF) to
// Casio's dial colors: a medium royal blue around #1E4BA0 and a warm red around
// #D7282F.
#define CASIO_BLUE GColorCobaltBlue // #0055AA
#define CASIO_RED GColorRed         // #FF0000

typedef enum {
  WEATHER_CLEAR,
  WEATHER_CLOUDY,
  WEATHER_RAIN,
  WEATHER_SNOW,
  WEATHER_THUNDER,
  WEATHER_FOG,
  WEATHER_UNKNOWN
} WeatherBucket;

static const char *const WEEKDAY_ABBR[7] = {"MO", "TU", "WE", "TH",
                                            "FR", "SA", "SU"};

static char s_week_buf[4];
static char s_date_buf[16];
static char s_time_big_buf[6];
static char s_time_sec_buf[4];
static char s_alarm_buf[8];
static char s_temp_buf[5];

static void prv_default_settings(void) {
  s_settings.ShowSeconds = true;
  s_settings.ClockFormat = CLOCK_FORMAT_SYSTEM;
  s_settings.TemperatureUnit = TEMP_UNIT_CELSIUS;
  s_settings.FirstDayOfWeek = FIRST_DAY_MONDAY;
  s_settings.DateFormat = DATE_FORMAT_DD_MM;
  s_settings.HideSecondsAtNight = false;
  s_settings.NightStartHour = 22;
  s_settings.NightEndHour = 7;
}

// Whether `hour` falls in the night range. A range whose start is after its end
// wraps past midnight (22-7 covers 22:00-06:59); equal start and end is empty.
static bool prv_is_night_hour(int hour) {
  int start = s_settings.NightStartHour;
  int end = s_settings.NightEndHour;
  if (start <= end) {
    return hour >= start && hour < end;
  }
  return hour >= start || hour < end;
}

static bool prv_seconds_wanted(const struct tm *t) {
  return s_settings.ShowSeconds &&
         !(s_settings.HideSecondsAtNight && prv_is_night_hour(t->tm_hour));
}

// Whether the tick timer currently runs every second.
static bool s_seconds_ticking;

static bool prv_use_24h(void) {
  if (s_settings.ClockFormat == CLOCK_FORMAT_SYSTEM) {
    return clock_is_24h_style();
  }
  return s_settings.ClockFormat == CLOCK_FORMAT_24H;
}

// "HH:MM" in 24h mode; in 12h mode the hour has no leading zero and its unused
// tens digit is `blank` instead, so it stays unlit like on a real LCD.
static void prv_format_hhmm(char *buf, size_t size, const struct tm *t,
                            char blank) {
  if (prv_use_24h()) {
    snprintf(buf, size, "%02d:%02d", t->tm_hour, t->tm_min);
    return;
  }
  int hour = t->tm_hour % 12;
  if (hour == 0) {
    hour = 12;
  }
  if (hour < 10) {
    snprintf(buf, size, "%c%d:%02d", blank, hour, t->tm_min);
  } else {
    snprintf(buf, size, "%d:%02d", hour, t->tm_min);
  }
}

static void prv_save_settings(void) {
  persist_write_data(SETTINGS_KEY, &s_settings, sizeof(s_settings));
}

static void prv_load_settings(void) {
  prv_default_settings();
  persist_read_data(SETTINGS_KEY, &s_settings, sizeof(s_settings));
}

static void prv_tick_handler(struct tm *tick_time, TimeUnits units_changed);

// Subscribes to per-second or per-minute ticks, whichever `t` calls for.
static void prv_update_tick_subscription(const struct tm *t) {
  s_seconds_ticking = prv_seconds_wanted(t);

  tick_timer_service_unsubscribe();
  if (!s_seconds_ticking) {
    // Seconds off: leave only the ghost "88" showing.
    s_time_sec_buf[0] = '\0';
    layer_mark_dirty(s_time_sec_layer);
  }
  tick_timer_service_subscribe(s_seconds_ticking ? SECOND_UNIT : MINUTE_UNIT,
                               prv_tick_handler);
}

// Monday-anchored approximation of the ISO-8601 week number. Skips the
// leap-year-aware boundary math needed for full ISO correctness around
// week 1/52/53, since this is a glance-once-a-week display value.
static int prv_iso_week_number(struct tm *tick_time) {
  int wday = tick_time->tm_wday; // 0=Sun..6=Sat
  int dow_mon0 = (wday == 0) ? 6 : wday - 1;
  int week = (tick_time->tm_yday - dow_mon0 + 10) / 7;
  if (week < 1) {
    week = 52;
  } else if (week > 52) {
    week = 1;
  }
  return week;
}

static void prv_update_date_and_week(struct tm *tick_time) {
  // Right-aligned like a real LCD, so week 7 shows as a ghost 8 followed by a
  // lit 7.
  snprintf(s_week_buf, sizeof(s_week_buf), "%2d",
           prv_iso_week_number(tick_time));
  layer_mark_dirty(s_week_value_layer);

  int wday = tick_time->tm_wday;              // 0=Sun..6=Sat
  s_today_index = (wday == 0) ? 6 : wday - 1; // 0=Mon..6=Sun
  layer_mark_dirty(s_weekday_strip_layer);

  strftime(s_date_buf, sizeof(s_date_buf),
           s_settings.DateFormat == DATE_FORMAT_MM_DD ? "%m-%d" : "%d-%m",
           tick_time);
  layer_mark_dirty(s_date_layer);
}

static void prv_update_time_display(struct tm *tick_time) {
  prv_format_hhmm(s_time_big_buf, sizeof(s_time_big_buf), tick_time, ' ');
  layer_mark_dirty(s_time_big_layer);

  bool show_ampm = !prv_use_24h();
  layer_set_hidden(text_layer_get_layer(s_ampm_layer), !show_ampm);
  if (show_ampm) {
    text_layer_set_text(s_ampm_layer, tick_time->tm_hour < 12 ? "AM" : "PM");
  }

  if (s_seconds_ticking) {
    strftime(s_time_sec_buf, sizeof(s_time_sec_buf), "%S", tick_time);
    layer_mark_dirty(s_time_sec_layer);
  }
}

static void prv_refresh_alarm(void) {
  time_t alarm_time;
  if (alarm_service_peek_next(&alarm_time)) {
    struct tm *alarm_tm = localtime(&alarm_time);
    prv_format_hhmm(s_alarm_buf, sizeof(s_alarm_buf), alarm_tm, ' ');
  } else {
    // No alarm: no lit segments, so only the faint "88:88" ghost shows.
    s_alarm_buf[0] = '\0';
  }
  layer_mark_dirty(s_alarm_value_layer);
}

static void prv_tick_handler(struct tm *tick_time, TimeUnits units_changed) {
  // Entering or leaving the night range: switch between per-second and
  // per-minute ticks.
  if ((units_changed & MINUTE_UNIT) &&
      prv_seconds_wanted(tick_time) != s_seconds_ticking) {
    prv_update_tick_subscription(tick_time);
  }

  prv_update_time_display(tick_time);

  if (tick_time->tm_mday != s_last_mday) {
    s_last_mday = tick_time->tm_mday;
    prv_update_date_and_week(tick_time);
  }

  if (tick_time->tm_sec == 0) {
    prv_refresh_alarm();
  }
}

#define BATTERY_BAR_COUNT 10
#define BATTERY_BAR_WIDTH 3
#define BATTERY_BAR_GAP 1
#define ICON_BADGE_PADDING 3
#define ICON_BADGE_RADIUS 3

static void prv_battery_icon_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);

  GRect inner = GRect(bounds.origin.x + ICON_BADGE_PADDING,
                      bounds.origin.y + ICON_BADGE_PADDING - 1,
                      bounds.size.w - ICON_BADGE_PADDING * 2,
                      bounds.size.h - (ICON_BADGE_PADDING - 1) * 2);

  int total_w = BATTERY_BAR_COUNT * BATTERY_BAR_WIDTH +
                (BATTERY_BAR_COUNT - 1) * BATTERY_BAR_GAP;
  int start_x = inner.origin.x + (inner.size.w - total_w) / 2;
  int bar_h = inner.size.h;
  int filled = (s_battery_percent * BATTERY_BAR_COUNT + 50) / 100;
  if (filled > BATTERY_BAR_COUNT)
    filled = BATTERY_BAR_COUNT;

  for (int i = 0; i < BATTERY_BAR_COUNT; i++) {
    GRect bar = GRect(start_x + i * (BATTERY_BAR_WIDTH + BATTERY_BAR_GAP),
                      inner.origin.y, BATTERY_BAR_WIDTH, bar_h);
    graphics_context_set_fill_color(ctx,
                                    i < filled ? GColorWhite : GColorDarkGray);
    graphics_fill_rect(ctx, bar, 0, GCornerNone);
  }
}

static void prv_battery_callback(BatteryChargeState state) {
  s_battery_percent = state.charge_percent;
  layer_mark_dirty(s_battery_icon_layer);
}

static WeatherBucket prv_weather_bucket(int wmo_code) {
  if (wmo_code == 0)
    return WEATHER_CLEAR;
  if (wmo_code >= 1 && wmo_code <= 3)
    return WEATHER_CLOUDY;
  if (wmo_code == 45 || wmo_code == 48)
    return WEATHER_FOG;
  if ((wmo_code >= 51 && wmo_code <= 67) || (wmo_code >= 80 && wmo_code <= 82))
    return WEATHER_RAIN;
  if ((wmo_code >= 71 && wmo_code <= 77) || wmo_code == 85 || wmo_code == 86)
    return WEATHER_SNOW;
  if (wmo_code >= 95 && wmo_code <= 99)
    return WEATHER_THUNDER;
  return WEATHER_UNKNOWN;
}

static void prv_draw_cloud(GContext *ctx, GPoint center, int r) {
  graphics_fill_circle(ctx, GPoint(center.x - r * 40 / 100, center.y),
                       r * 60 / 100);
  graphics_fill_circle(ctx,
                       GPoint(center.x + r * 30 / 100, center.y - r * 15 / 100),
                       r * 70 / 100);
  GRect base = GRect(center.x - r, center.y, r * 2, r * 55 / 100);
  graphics_fill_rect(ctx, base, r * 20 / 100, GCornersBottom);
}

static void prv_weather_icon_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  GPoint center = GPoint(bounds.origin.x + bounds.size.w / 2,
                         bounds.origin.y + bounds.size.h / 2);
  int r =
      (bounds.size.w < bounds.size.h ? bounds.size.w : bounds.size.h) / 2 - 2;

  graphics_context_set_stroke_color(ctx, GColorBlack);
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_context_set_stroke_width(ctx, 1);

  if (s_weather_condition < 0) {
    // No data yet - faint placeholder dash
    graphics_context_set_stroke_color(ctx, GColorLightGray);
    graphics_draw_line(ctx, GPoint(center.x - r / 2, center.y),
                       GPoint(center.x + r / 2, center.y));
    return;
  }

  switch (prv_weather_bucket(s_weather_condition)) {
  case WEATHER_CLEAR: {
    GPoint sun_center = GPoint(center.x, center.y - r * 15 / 100);
    graphics_fill_circle(ctx, sun_center, r * 45 / 100);
    for (int i = 0; i < 8; i++) {
      int32_t angle = TRIG_MAX_ANGLE * i / 8;
      GPoint p1 = GPoint(
          sun_center.x + sin_lookup(angle) * (r * 55 / 100) / TRIG_MAX_RATIO,
          sun_center.y - cos_lookup(angle) * (r * 55 / 100) / TRIG_MAX_RATIO);
      GPoint p2 = GPoint(sun_center.x + sin_lookup(angle) * r / TRIG_MAX_RATIO,
                         sun_center.y - cos_lookup(angle) * r / TRIG_MAX_RATIO);
      graphics_draw_line(ctx, p1, p2);
    }
    break;
  }
  case WEATHER_CLOUDY:
    prv_draw_cloud(ctx, center, r);
    break;
  case WEATHER_RAIN:
    prv_draw_cloud(ctx, GPoint(center.x, center.y - r * 15 / 100),
                   r * 85 / 100);
    for (int i = -1; i <= 1; i++) {
      int x = center.x + i * r * 35 / 100;
      graphics_draw_line(ctx, GPoint(x, center.y + r * 45 / 100),
                         GPoint(x - 2, center.y + r * 80 / 100));
    }
    break;
  case WEATHER_SNOW:
    prv_draw_cloud(ctx, GPoint(center.x, center.y - r * 15 / 100),
                   r * 85 / 100);
    for (int i = -1; i <= 1; i++) {
      int x = center.x + i * r * 35 / 100;
      graphics_fill_circle(ctx, GPoint(x, center.y + r * 65 / 100), 1);
    }
    break;
  case WEATHER_THUNDER:
    prv_draw_cloud(ctx, GPoint(center.x, center.y - r * 15 / 100),
                   r * 85 / 100);
    graphics_draw_line(ctx, GPoint(center.x + 2, center.y + r * 40 / 100),
                       GPoint(center.x - 3, center.y + r * 65 / 100));
    graphics_draw_line(ctx, GPoint(center.x - 3, center.y + r * 65 / 100),
                       GPoint(center.x + 1, center.y + r * 70 / 100));
    graphics_draw_line(ctx, GPoint(center.x + 1, center.y + r * 70 / 100),
                       GPoint(center.x - 4, center.y + r * 95 / 100));
    break;
  case WEATHER_FOG:
    for (int i = -1; i <= 1; i++) {
      int y = center.y + i * r * 40 / 100;
      graphics_draw_line(ctx, GPoint(center.x - r, y), GPoint(center.x + r, y));
    }
    break;
  default:
    break;
  }
}

// Knocks out every other ghost-colored pixel in a checkerboard pattern, so
// ghost segments read as a stippled, fainter-than-solid gray. Must run after
// the ghost glyphs are drawn but before any live (black) content goes on top.
// `rect` is in screen coordinates; the checkerboard is anchored to the screen
// so adjacent ghost layers line up.
static void prv_stipple_ghost(GContext *ctx, GRect rect) {
  GBitmap *fb = graphics_capture_frame_buffer(ctx);
  if (!fb) {
    return;
  }

  GRect fb_bounds = gbitmap_get_bounds(fb);
  int y_start =
      rect.origin.y > fb_bounds.origin.y ? rect.origin.y : fb_bounds.origin.y;
  int y_end = rect.origin.y + rect.size.h;
  int fb_y_end = fb_bounds.origin.y + fb_bounds.size.h;
  if (y_end > fb_y_end)
    y_end = fb_y_end;

  for (int y = y_start; y < y_end; y++) {
    GBitmapDataRowInfo row = gbitmap_get_data_row_info(fb, y);
    int x_start = rect.origin.x > row.min_x ? rect.origin.x : row.min_x;
    int x_end = rect.origin.x + rect.size.w - 1;
    if (x_end > row.max_x)
      x_end = row.max_x;

    for (int x = x_start + ((x_start + y) & 1); x <= x_end; x += 2) {
      if (row.data[x] == GHOST_COLOR.argb) {
        row.data[x] = GColorWhite.argb;
      }
    }
  }

  graphics_release_frame_buffer(ctx, fb);
}

// Hand-drawn 7-segment LCD digits, used for every readout, so all of them share
// one segment shape and stay crisp at any size.
//
//    aaa
//   f   b
//    ggg
//   e   c
//    ddd
#define SEG_A (1 << 0)
#define SEG_B (1 << 1)
#define SEG_C (1 << 2)
#define SEG_D (1 << 3)
#define SEG_E (1 << 4)
#define SEG_F (1 << 5)
#define SEG_G (1 << 6)
#define SEG_ALL 0x7F

static const uint8_t SEG_DIGITS[10] = {
    SEG_A | SEG_B | SEG_C | SEG_D | SEG_E | SEG_F, // 0
    SEG_B | SEG_C,                                 // 1
    SEG_A | SEG_B | SEG_G | SEG_E | SEG_D,         // 2
    SEG_A | SEG_B | SEG_G | SEG_C | SEG_D,         // 3
    SEG_F | SEG_G | SEG_B | SEG_C,                 // 4
    SEG_A | SEG_F | SEG_G | SEG_C | SEG_D,         // 5
    SEG_A | SEG_F | SEG_G | SEG_E | SEG_D | SEG_C, // 6
    SEG_A | SEG_B | SEG_C,                         // 7
    SEG_ALL,                                       // 8
    SEG_A | SEG_B | SEG_C | SEG_D | SEG_F | SEG_G, // 9
};

// One readout: `pattern` lays out the slots and doubles as the ghost ('8' =
// digit,
// '-' = dash-only slot, ':' = colon). `text` holds the lit characters, one per
// slot; a space or a missing (past end of string) character leaves that slot
// unlit. Each segment's pixels, precomputed as a few rectangles: the mitered
// end rows plus one merged rectangle for the straight body. 2 * thickness + 1
// is enough for thickness up to 6; prv_build_seg_rects() refuses to overflow
// regardless.
#define SEG_MAX_RECTS 13

typedef struct SegRect {
  uint8_t x, y, w, h;
} SegRect;

typedef struct SegDisplay {
  const char *pattern;
  const char *text;
  GTextAlignment alignment;
  uint8_t digit_w;
  uint8_t digit_h;
  uint8_t thickness;
  uint8_t spacing;
  uint8_t rect_count[7];
  SegRect rects[7][SEG_MAX_RECTS];
} SegDisplay;

static uint8_t prv_seg_mask(char c) {
  if (c >= '0' && c <= '9')
    return SEG_DIGITS[c - '0'];
  if (c == '-')
    return SEG_G;
  return 0;
}

static int prv_seg_slot_width(const SegDisplay *d, char pattern_char) {
  return pattern_char == ':' ? d->thickness + 2 : d->digit_w;
}

// Which segment the pixel at (x, y) inside a digit cell belongs to (0 = none).
// Segments run all the way into the corners and meet their neighbours along a
// 45° miter, with a 1px diagonal gap - the classic LCD segment shape.
static uint8_t prv_seg_at(const SegDisplay *d, int x, int y) {
  int w = d->digit_w;
  int h = d->digit_h;
  int t = d->thickness;
  int ym = h / 2;
  int dm = y - ym;
  int adm = dm < 0 ? -dm : dm;

  bool left = x < t;
  bool right = x >= w - t;
  bool top = y < t;
  bool bottom = y >= h - t;

  if (!left && !right) {
    if (top)
      return SEG_A;
    if (bottom)
      return SEG_D;
    if (adm <= t / 2)
      return SEG_G;
    return 0;
  }

  int u = left ? x : w - 1 - x; // distance in from the nearer side edge
  uint8_t upper = left ? SEG_F : SEG_B;
  uint8_t lower = left ? SEG_E : SEG_C;

  if (top) {
    int v = y;
    return u > v ? SEG_A : (v > u ? upper : 0);
  }
  if (bottom) {
    int v = h - 1 - y;
    return u > v ? SEG_D : (v > u ? lower : 0);
  }
  if (adm < t) {
    if (u > adm)
      return SEG_G;
    if (u == adm)
      return 0;
    return dm < 0 ? upper : lower;
  }
  return dm < 0 ? upper : lower;
}

// Precomputes each segment's rectangles from prv_seg_at(), merging rows whose
// run has the same position and width into one taller rectangle. Every segment
// covers at most one contiguous run per row, so a row scan finds it.
static void prv_build_seg_rects(SegDisplay *d) {
  for (int seg = 0; seg < 7; seg++) {
    uint8_t bit = 1 << seg;
    int count = 0;
    for (int row = 0; row < d->digit_h; row++) {
      int start = -1;
      int end = -1;
      for (int col = 0; col < d->digit_w; col++) {
        if (prv_seg_at(d, col, row) == bit) {
          if (start < 0)
            start = col;
          end = col;
        }
      }
      if (start < 0)
        continue;

      SegRect *prev = count ? &d->rects[seg][count - 1] : NULL;
      if (prev && prev->x == start && prev->w == end - start + 1 &&
          prev->y + prev->h == row) {
        prev->h++;
      } else if (count < SEG_MAX_RECTS) {
        d->rects[seg][count++] = (SegRect){start, row, end - start + 1, 1};
      } else {
        APP_LOG(APP_LOG_LEVEL_ERROR, "segment %d needs more than %d rects", seg,
                SEG_MAX_RECTS);
      }
    }
    d->rect_count[seg] = count;
  }
}

static void prv_draw_seg_digit(GContext *ctx, const SegDisplay *d, int x, int y,
                               uint8_t mask) {
  for (int seg = 0; seg < 7; seg++) {
    if (!(mask & (1 << seg)))
      continue;
    for (int i = 0; i < d->rect_count[seg]; i++) {
      const SegRect *r = &d->rects[seg][i];
      graphics_fill_rect(ctx, GRect(x + r->x, y + r->y, r->w, r->h), 0,
                         GCornerNone);
    }
  }
}

static void prv_draw_seg_colon(GContext *ctx, const SegDisplay *d, int x,
                               int y) {
  int t = d->thickness;
  int yg = y + d->digit_h / 2;
  int offset = d->digit_h / 4;
  graphics_fill_rect(ctx, GRect(x + 1, yg - offset - t / 2, t, t), 0,
                     GCornerNone);
  graphics_fill_rect(ctx, GRect(x + 1, yg + offset - t / 2, t, t), 0,
                     GCornerNone);
}

// Draws every slot of `pattern` in one color: `lit` false draws the full ghost,
// true draws only the segments of the characters in `text`.
static void prv_draw_seg_pass(GContext *ctx, const SegDisplay *d, int x, int y,
                              bool lit) {
  size_t text_len = (lit && d->text) ? strlen(d->text) : 0;

  for (size_t i = 0; d->pattern[i]; i++) {
    char slot = d->pattern[i];
    char c = lit ? (i < text_len ? d->text[i] : ' ') : slot;

    if (slot == ':') {
      if (c == ':')
        prv_draw_seg_colon(ctx, d, x, y);
    } else {
      uint8_t mask = prv_seg_mask(c);
      if (mask)
        prv_draw_seg_digit(ctx, d, x, y, mask);
    }
    x += prv_seg_slot_width(d, slot) + d->spacing;
  }
}

// Draws the stippled ghost, then the lit segments on top. Returns the x just
// past the last slot, for placing trailing decorations like a degree sign.
static int prv_draw_seg_display(Layer *layer, GContext *ctx,
                                const SegDisplay *d) {
  GRect bounds = layer_get_bounds(layer);

  int total_w = 0;
  for (size_t i = 0; d->pattern[i]; i++) {
    total_w += prv_seg_slot_width(d, d->pattern[i]) + (i ? d->spacing : 0);
  }
  int x = bounds.origin.x;
  if (d->alignment == GTextAlignmentCenter) {
    x += (bounds.size.w - total_w) / 2;
  } else if (d->alignment == GTextAlignmentRight) {
    x += bounds.size.w - total_w;
  }
  int y = bounds.origin.y;

  graphics_context_set_antialiased(ctx, false);
  graphics_context_set_stroke_width(ctx, 1);

  graphics_context_set_stroke_color(ctx, GHOST_COLOR);
  graphics_context_set_fill_color(ctx, GHOST_COLOR);
  prv_draw_seg_pass(ctx, d, x, y, false);
  prv_stipple_ghost(ctx, layer_convert_rect_to_screen(layer, bounds));

  graphics_context_set_stroke_color(ctx, GColorBlack);
  graphics_context_set_fill_color(ctx, GColorBlack);
  prv_draw_seg_pass(ctx, d, x, y, true);

  graphics_context_set_antialiased(ctx, true);
  return x + total_w;
}

static void prv_seg_display_update_proc(Layer *layer, GContext *ctx) {
  prv_draw_seg_display(layer, ctx, layer_get_data(layer));
}

static Layer *prv_create_seg_display(GRect rect, const char *pattern,
                                     const char *text, GTextAlignment alignment,
                                     int digit_w, int digit_h, int thickness,
                                     int spacing, LayerUpdateProc update_proc) {
  Layer *layer = layer_create_with_data(rect, sizeof(SegDisplay));
  SegDisplay *d = layer_get_data(layer);
  d->pattern = pattern;
  d->text = text;
  d->alignment = alignment;
  d->digit_w = digit_w;
  d->digit_h = digit_h;
  d->thickness = thickness;
  d->spacing = spacing;
  prv_build_seg_rects(d);
  layer_set_update_proc(layer, update_proc);
  return layer;
}

// A real fixed-width 3-digit LCD readout: a sign slot that's always present
// (lit when negative, ghosted when not) followed by 3 digit slots, where unused
// leading digits show as ghost '8's instead of disappearing - just like a
// digital thermometer/scale display, and leaves room to grow into 3-digit
// Fahrenheit values.
// Rounds n / d to the nearest integer, halves away from zero.
static int prv_div_round(int n, int d) {
  return n >= 0 ? (n + d / 2) / d : -((-n + d / 2) / d);
}

static void prv_weather_temp_update_proc(Layer *layer, GContext *ctx) {
  if (s_weather_temp == WEATHER_TEMP_NONE) {
    s_temp_buf[0] = '\0';
  } else {
    int temp = s_settings.TemperatureUnit == TEMP_UNIT_FAHRENHEIT
                   ? prv_div_round(s_weather_temp * 9 + 1600, 50)
                   : prv_div_round(s_weather_temp, 10);
    int abs_temp = temp < 0 ? -temp : temp;
    if (abs_temp > 999)
      abs_temp = 999;
    snprintf(s_temp_buf, sizeof(s_temp_buf), "%c%3d", temp < 0 ? '-' : ' ',
             abs_temp);
  }

  SegDisplay *display = layer_get_data(layer);
  int end_x = prv_draw_seg_display(layer, ctx, display);

  graphics_context_set_stroke_color(ctx, GColorBlack);
  graphics_draw_circle(
      ctx, GPoint(end_x + 5, layer_get_bounds(layer).origin.y + 3), 3);
}

#define WEEKDAY_BOX_PAD_X 3
#define WEEKDAY_BOX_RADIUS 3

// Casio-style weekday strip: two-letter days in faint gray, with today "lit" as
// white text in a small black rounded box.
static void prv_weekday_strip_update_proc(Layer *layer, GContext *ctx) {
  GRect band = layer_get_bounds(layer);

  // Black band across the top of the white content area, its top corners
  // following the content area's rounding. It runs on behind the middle
  // frame's rounded top corners (the frame paints its own white inside), so the
  // black wraps around them with no white gaps.
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, band, CONTENT_FRAME_RADIUS, GCornersTop);

  // Days centered in the visible part of the band, above the middle frame.
  int visible_h = ROW_MIDDLE_Y - ROW_TOP_Y;
  GRect bounds =
      GRect(band.origin.x + 5, band.origin.y + visible_h / 2 - 10, 182, 19);
  int slot_w = bounds.size.w / 7;
  int first_day = s_settings.FirstDayOfWeek == FIRST_DAY_SUNDAY ? 6 : 0;

  for (int i = 0; i < 7; i++) {
    GRect slot = GRect(bounds.origin.x + i * slot_w, bounds.origin.y, slot_w,
                       bounds.size.h);
    int day = (first_day + i) % 7; // 0=Mon..6=Sun
    bool today = day == s_today_index;

    if (today) {
      GSize text_size = graphics_text_layout_get_content_size(
          WEEKDAY_ABBR[day], s_font_weekday, slot, GTextOverflowModeFill,
          GTextAlignmentCenter);
      int box_w = text_size.w + WEEKDAY_BOX_PAD_X * 2;
      GRect box = GRect(slot.origin.x + (slot_w - box_w) / 2,
                        bounds.origin.y + 3, box_w, bounds.size.h - 4);
      graphics_context_set_fill_color(ctx, GColorWhite);
      graphics_fill_rect(ctx, box, WEEKDAY_BOX_RADIUS, GCornersAll);
    }

    graphics_context_set_text_color(ctx, today ? GColorBlack : GColorLightGray);
    graphics_draw_text(ctx, WEEKDAY_ABBR[day], s_font_weekday, slot,
                       GTextOverflowModeFill, GTextAlignmentCenter, NULL);
  }
}

static void prv_header_bg_update_proc(Layer *layer, GContext *ctx) {
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, layer_get_bounds(layer), 0, GCornerNone);
}

static void prv_footer_bg_update_proc(Layer *layer, GContext *ctx) {
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, layer_get_bounds(layer), 0, GCornerNone);
}

static void prv_content_frame_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);

  // Fill entire area with black
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);

  // Blue outer border, concentric with the red one
  graphics_context_set_fill_color(ctx, CASIO_BLUE);
  graphics_fill_rect(ctx, bounds, CONTENT_FRAME_RADIUS + OUTER_FRAME_INSET,
                     GCornersAll);

  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, grect_inset(bounds, GEdgeInsets(OUTER_FRAME_BORDER)),
                     CONTENT_FRAME_RADIUS + OUTER_FRAME_GAP, GCornersAll);

  // Red rounded border, then the white content area inset inside it with the
  // same radius
  GRect red = grect_inset(bounds, GEdgeInsets(OUTER_FRAME_INSET));
  graphics_context_set_fill_color(ctx, CASIO_RED);
  graphics_fill_rect(ctx, red, CONTENT_FRAME_RADIUS, GCornersAll);

  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_fill_rect(ctx, grect_inset(red, GEdgeInsets(CONTENT_FRAME_BORDER)),
                     CONTENT_FRAME_RADIUS, GCornersAll);
}

// Generic rounded-rect frame drawn behind a group of layers (e.g. an icon + its
// value), to visually pair them into one badge.
static void prv_group_frame_update_proc(Layer *layer, GContext *ctx) {
  graphics_context_set_stroke_color(ctx, GColorBlack);
  graphics_draw_round_rect(ctx, layer_get_bounds(layer), ICON_BADGE_RADIUS);
}

#define MIDDLE_FRAME_RADIUS 12
#define MIDDLE_FRAME_STROKE 3
#define MIDDLE_FRAME_INNER_INSET 4

static void prv_middle_frame_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_fill_rect(ctx, bounds, MIDDLE_FRAME_RADIUS, GCornersAll);

  graphics_context_set_stroke_color(ctx, GColorBlack);
  graphics_context_set_stroke_width(ctx, MIDDLE_FRAME_STROKE);
  graphics_draw_round_rect(ctx, bounds, MIDDLE_FRAME_RADIUS);

  // Second, thinner border just inside the first, with a concentric radius
  graphics_context_set_stroke_width(ctx, 1);
  graphics_draw_round_rect(
      ctx, grect_inset(bounds, GEdgeInsets(MIDDLE_FRAME_INNER_INSET)),
      MIDDLE_FRAME_RADIUS - MIDDLE_FRAME_INNER_INSET);

  // Divider lines, the same weight as the inner border: one under the time,
  // and one between the date and the week, joining the inner border.
  GRect frame = layer_get_frame(layer);
  int inner_left = MIDDLE_FRAME_INNER_INSET;
  int inner_right = bounds.size.w - 1 - MIDDLE_FRAME_INNER_INSET;
  int inner_bottom = bounds.size.h - 1 - MIDDLE_FRAME_INNER_INSET;
  int divider_y = MIDDLE_DIVIDER_Y - frame.origin.y;
  int divider_x = DATE_WEEK_DIVIDER_X - frame.origin.x;
  graphics_draw_line(ctx, GPoint(inner_left, divider_y),
                     GPoint(inner_right, divider_y));
  graphics_draw_line(ctx, GPoint(divider_x, divider_y),
                     GPoint(divider_x, inner_bottom));
}

// Temperature and alarm readouts, in screen coordinates. Each sits in a small
// white "LCD window" on the bottom band, with a printed white label to its
// left.
#define BOTTOM_DIGITS_Y (ROW_BOTTOM_Y + 7)
#define BOTTOM_TEMP_RECT                                                       \
  GRect(41, BOTTOM_DIGITS_Y, 59, 17) // "-888" + degree sign
#define BOTTOM_ALARM_RECT GRect(129, BOTTOM_DIGITS_Y, 57, 17) // "88:88"
#define BOTTOM_WINDOW_PAD_X 2
#define BOTTOM_WINDOW_PAD_Y 2
#define BOTTOM_WINDOW_RADIUS 3
#define BOTTOM_LABEL_GAP 3

// Draws a white window around `digits` (screen coordinates) and a white label
// right-aligned just left of it.
static void prv_draw_lcd_window(GContext *ctx, GRect band_frame, GRect digits,
                                const char *label) {
  GRect window =
      GRect(digits.origin.x - band_frame.origin.x - BOTTOM_WINDOW_PAD_X,
            digits.origin.y - band_frame.origin.y - BOTTOM_WINDOW_PAD_Y,
            digits.size.w + BOTTOM_WINDOW_PAD_X * 2,
            digits.size.h + BOTTOM_WINDOW_PAD_Y * 2);
  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_fill_rect(ctx, window, BOTTOM_WINDOW_RADIUS, GCornersAll);

  // Gothic 14 bold, like the BATTERY and WEEK labels: its capitals are 9px tall
  // and start 5px below the top of the text box.
  GFont font = fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD);
  const int label_w = 40;
  const int label_h = 18;
  const int cap_top = 5;
  const int cap_h = 9;
  int center_y = window.origin.y + window.size.h / 2;
  graphics_context_set_text_color(ctx, GColorWhite);
  graphics_draw_text(ctx, label, font,
                     GRect(window.origin.x - BOTTOM_LABEL_GAP - label_w,
                           center_y - cap_h / 2 - cap_top, label_w, label_h),
                     GTextOverflowModeFill, GTextAlignmentRight, NULL);
}

// Black band across the bottom of the white content area, mirroring the
// weekday band at the top. Like that one, it runs on behind the middle frame's
// rounded bottom corners, which the frame then paints white again.
static void prv_bottom_band_update_proc(Layer *layer, GContext *ctx) {
  GRect band = layer_get_bounds(layer);
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, band, CONTENT_FRAME_RADIUS, GCornersBottom);

  GRect frame = layer_get_frame(layer);
  prv_draw_lcd_window(ctx, frame, BOTTOM_TEMP_RECT, "TMP");
  prv_draw_lcd_window(ctx, frame, BOTTOM_ALARM_RECT, "AL");
}

#define BUTTON_HINT_ARROW_SIZE 3
#define BUTTON_HINT_EDGE 3
#define BUTTON_HINT_GAP 3

// Solid triangle pointing left or right, built from vertical lines, with its
// tip at `tip`.
static void prv_draw_hint_arrow(GContext *ctx, GPoint tip, bool point_left) {
  for (int i = 0; i <= BUTTON_HINT_ARROW_SIZE; i++) {
    int x = point_left ? tip.x + i : tip.x - i;
    graphics_draw_line(ctx, GPoint(x, tip.y - i), GPoint(x, tip.y + i));
  }
}

// A small white label next to the physical button it describes, with an arrow
// pointing out towards the screen edge where the button is. `y` is the text box
// top; `arrow_dy` nudges the arrow up (negative) or down from its default spot.
static void prv_draw_button_hint(GContext *ctx, GRect bounds, const char *text,
                                 int y, bool left_side, int arrow_dy) {
  GFont font = fonts_get_system_font(FONT_KEY_GOTHIC_09);
  const int text_h = 12;
  const int arrow_y = y + 7 + arrow_dy;
  const int text_inset =
      BUTTON_HINT_EDGE + BUTTON_HINT_ARROW_SIZE + 1 + BUTTON_HINT_GAP;

  if (left_side) {
    prv_draw_hint_arrow(
        ctx, GPoint(bounds.origin.x + BUTTON_HINT_EDGE, arrow_y), true);
    graphics_draw_text(
        ctx, text, font,
        GRect(bounds.origin.x + text_inset, y, bounds.size.w / 2, text_h),
        GTextOverflowModeFill, GTextAlignmentLeft, NULL);
  } else {
    int right = bounds.origin.x + bounds.size.w - 1;
    prv_draw_hint_arrow(ctx, GPoint(right - BUTTON_HINT_EDGE, arrow_y), false);
    graphics_draw_text(ctx, text, font,
                       GRect(right - text_inset - bounds.size.w / 2, y,
                             bounds.size.w / 2, text_h),
                       GTextOverflowModeFill, GTextAlignmentRight, NULL);
  }
}

static void prv_button_hints_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  graphics_context_set_text_color(ctx, GColorWhite);
  graphics_context_set_stroke_color(ctx, GColorWhite);

  prv_draw_button_hint(ctx, bounds, "LIGHT", 20, true, 0);
  prv_draw_button_hint(ctx, bounds, "UP", 1, false, 0);
  prv_draw_button_hint(ctx, bounds, "ENTER", 20, false, 0);
  prv_draw_button_hint(ctx, bounds, "DOWN", FOOTER_Y + 1, false, -2);
}

static void prv_chrome_layer_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  graphics_context_set_stroke_color(ctx, GColorBlack);

  // Header divider
  graphics_draw_line(ctx, GPoint(bounds.origin.x + 6, 22),
                     GPoint(bounds.origin.x + bounds.size.w - 6, 22));
}

// PebbleKit JS picks the smallest wire type that fits each number (e.g. a small
// positive temperature is sent as uint8, not int32), so reading
// tuple->value->int32 unconditionally reads garbage for anything but a 4-byte
// value - most obviously breaking negative numbers, which need proper
// sign-extension. Read according to the tuple's actual type/length instead.
static int32_t prv_tuple_get_int(Tuple *tuple) {
  if (tuple->type == TUPLE_INT) {
    switch (tuple->length) {
    case 1:
      return tuple->value->int8;
    case 2:
      return tuple->value->int16;
    default:
      return tuple->value->int32;
    }
  } else {
    switch (tuple->length) {
    case 1:
      return tuple->value->uint8;
    case 2:
      return tuple->value->uint16;
    default:
      return (int32_t)tuple->value->uint32;
    }
  }
}

// Clay sends select values as strings ("1") and toggles as integers.
static int32_t prv_tuple_get_setting(Tuple *tuple) {
  if (tuple->type == TUPLE_CSTRING) {
    return atoi(tuple->value->cstring);
  }
  return prv_tuple_get_int(tuple);
}

// Re-renders everything that depends on a setting.
static void prv_apply_settings(void) {
  time_t now = time(NULL);
  struct tm *tick_time = localtime(&now);
  prv_update_tick_subscription(tick_time);
  prv_update_time_display(tick_time);
  prv_update_date_and_week(tick_time);
  prv_refresh_alarm();
  layer_mark_dirty(s_weather_temp_layer);
}

static void prv_inbox_received_callback(DictionaryIterator *iterator,
                                        void *context) {
  bool settings_changed = false;

  Tuple *t = dict_find(iterator, MESSAGE_KEY_ShowSeconds);
  if (t) {
    s_settings.ShowSeconds = prv_tuple_get_setting(t) == 1;
    settings_changed = true;
  }
  t = dict_find(iterator, MESSAGE_KEY_ClockFormat);
  if (t) {
    s_settings.ClockFormat = prv_tuple_get_setting(t);
    settings_changed = true;
  }
  t = dict_find(iterator, MESSAGE_KEY_TemperatureUnit);
  if (t) {
    s_settings.TemperatureUnit = prv_tuple_get_setting(t);
    settings_changed = true;
  }
  t = dict_find(iterator, MESSAGE_KEY_FirstDayOfWeek);
  if (t) {
    s_settings.FirstDayOfWeek = prv_tuple_get_setting(t);
    settings_changed = true;
  }
  t = dict_find(iterator, MESSAGE_KEY_DateFormat);
  if (t) {
    s_settings.DateFormat = prv_tuple_get_setting(t);
    settings_changed = true;
  }
  t = dict_find(iterator, MESSAGE_KEY_HideSecondsAtNight);
  if (t) {
    s_settings.HideSecondsAtNight = prv_tuple_get_setting(t) == 1;
    settings_changed = true;
  }
  t = dict_find(iterator, MESSAGE_KEY_NightStartHour);
  if (t) {
    s_settings.NightStartHour = prv_tuple_get_setting(t) % 24;
    settings_changed = true;
  }
  t = dict_find(iterator, MESSAGE_KEY_NightEndHour);
  if (t) {
    s_settings.NightEndHour = prv_tuple_get_setting(t) % 24;
    settings_changed = true;
  }
  if (settings_changed) {
    prv_save_settings();
    prv_apply_settings();
  }

  Tuple *temperature_t = dict_find(iterator, MESSAGE_KEY_TemperatureTenths);
  if (temperature_t) {
    s_weather_temp = prv_tuple_get_int(temperature_t);
    layer_mark_dirty(s_weather_temp_layer);
  }

  Tuple *condition_t = dict_find(iterator, MESSAGE_KEY_WeatherCondition);
  if (condition_t) {
    s_weather_condition = prv_tuple_get_int(condition_t);
  }
}

static TextLayer *prv_create_text_layer(GRect rect, GFont font,
                                        GTextAlignment alignment) {
  TextLayer *layer = text_layer_create(rect);
  text_layer_set_background_color(layer, GColorClear);
  text_layer_set_text_color(layer, GColorBlack);
  text_layer_set_font(layer, font);
  text_layer_set_text_alignment(layer, alignment);
  text_layer_set_overflow_mode(layer, GTextOverflowModeFill);
  return layer;
}

static void prv_window_load(Window *window) {
  Layer *window_layer = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(window_layer);

  s_font_brand =
      fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_BRAND_12));

  GFont label_font = fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD);
  s_font_weekday = fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD);

  s_chrome_layer = layer_create(bounds);
  layer_set_update_proc(s_chrome_layer, prv_chrome_layer_update_proc);
  layer_add_child(window_layer, s_chrome_layer);

  // Header
  s_header_bg_layer = layer_create(GRect(0, 0, bounds.size.w, 40));
  layer_set_update_proc(s_header_bg_layer, prv_header_bg_update_proc);
  layer_add_child(window_layer, s_header_bg_layer);

  s_header_logo_layer = prv_create_text_layer(
      GRect(0, 1, bounds.size.w, 18), s_font_brand, GTextAlignmentCenter);
  text_layer_set_text_color(s_header_logo_layer, GColorYellow);
  text_layer_set_text(s_header_logo_layer, "CASUAL");
  layer_add_child(window_layer, text_layer_get_layer(s_header_logo_layer));

  s_header_model_layer = prv_create_text_layer(
      GRect(0, 19, bounds.size.w, 18), s_font_brand, GTextAlignmentCenter);
  text_layer_set_text_color(s_header_model_layer, GColorYellow);
  text_layer_set_text(s_header_model_layer, "SALARYMAN");
  layer_add_child(window_layer, text_layer_get_layer(s_header_model_layer));

  // Content frame (black sides with rounded inner corners)
  s_content_frame_layer = layer_create(
      GRect(-OUTER_FRAME_INSET, CONTENT_FRAME_Y - OUTER_FRAME_INSET,
            bounds.size.w + OUTER_FRAME_INSET * 2,
            CONTENT_FRAME_H + OUTER_FRAME_INSET * 2));
  layer_set_update_proc(s_content_frame_layer, prv_content_frame_update_proc);
  layer_add_child(window_layer, s_content_frame_layer);

  // Top row: weekday strip on a black band
  s_weekday_strip_layer = layer_create(GRect(
      CONTENT_FRAME_BORDER, ROW_TOP_Y, bounds.size.w - CONTENT_FRAME_BORDER * 2,
      ROW_MIDDLE_Y + MIDDLE_FRAME_RADIUS - ROW_TOP_Y));
  layer_set_update_proc(s_weekday_strip_layer, prv_weekday_strip_update_proc);
  layer_add_child(window_layer, s_weekday_strip_layer);

  // Middle row: time on top, date + week below, evenly spaced inside the frame
  // Bottom row: temperature and alarm in small LCD windows on a black band
  int white_bottom = CONTENT_FRAME_Y + CONTENT_FRAME_H - CONTENT_FRAME_BORDER;
  int band_top = ROW_BOTTOM_Y - MIDDLE_FRAME_RADIUS;
  s_bottom_band_layer = layer_create(
      GRect(CONTENT_FRAME_BORDER, band_top,
            bounds.size.w - CONTENT_FRAME_BORDER * 2, white_bottom - band_top));
  layer_set_update_proc(s_bottom_band_layer, prv_bottom_band_update_proc);
  layer_add_child(window_layer, s_bottom_band_layer);

  s_middle_frame_layer =
      layer_create(GRect(1, ROW_MIDDLE_Y, 198, MIDDLE_FRAME_H));
  layer_set_update_proc(s_middle_frame_layer, prv_middle_frame_update_proc);
  layer_add_child(window_layer, s_middle_frame_layer);

  s_time_big_layer = prv_create_seg_display(
      GRect(5, TIME_Y, 148, 42), "88:88", s_time_big_buf, GTextAlignmentCenter,
      28, 42, 5, 5, prv_seg_display_update_proc);
  layer_add_child(window_layer, s_time_big_layer);

  // AM/PM indicator in 12h mode, above the seconds, its capitals level with
  // the top of the big digits (Gothic 14 bold capitals start 5px into the box).
  s_ampm_layer = prv_create_text_layer(GRect(155, TIME_Y - 5, 36, 18),
                                       label_font, GTextAlignmentLeft);
  layer_add_child(window_layer, text_layer_get_layer(s_ampm_layer));

  s_time_sec_layer = prv_create_seg_display(
      GRect(155, TIME_Y + 42 - 23, 36, 23), "88", s_time_sec_buf,
      GTextAlignmentLeft, 15, 23, 3, 4, prv_seg_display_update_proc);
  layer_add_child(window_layer, s_time_sec_layer);

  s_date_layer = prv_create_seg_display(GRect(18, DATE_ROW_Y, 72, 19), "88-88",
                                        s_date_buf, GTextAlignmentLeft, 12, 19,
                                        3, 3, prv_seg_display_update_proc);
  layer_add_child(window_layer, s_date_layer);

  s_week_label_layer = prv_create_text_layer(GRect(114, DATE_ROW_Y - 1, 67, 26),
                                             label_font, GTextAlignmentLeft);
  text_layer_set_text(s_week_label_layer, "WEEK");
  layer_add_child(window_layer, text_layer_get_layer(s_week_label_layer));

  s_week_value_layer = prv_create_seg_display(
      GRect(155, DATE_ROW_Y, 27, 19), "88", s_week_buf, GTextAlignmentLeft, 12,
      19, 3, 3, prv_seg_display_update_proc);
  layer_add_child(window_layer, s_week_value_layer);

  // Bottom row
  s_weather_temp_layer = prv_create_seg_display(
      BOTTOM_TEMP_RECT, "-888", s_temp_buf, GTextAlignmentLeft, 11, 17, 3, 2,
      prv_weather_temp_update_proc);
  layer_add_child(window_layer, s_weather_temp_layer);

  s_alarm_value_layer = prv_create_seg_display(
      BOTTOM_ALARM_RECT, "88:88", s_alarm_buf, GTextAlignmentLeft, 11, 17, 3, 2,
      prv_seg_display_update_proc);
  layer_add_child(window_layer, s_alarm_value_layer);

  // Footer
  s_footer_bg_layer =
      layer_create(GRect(0, FOOTER_Y, bounds.size.w, bounds.size.h - FOOTER_Y));
  layer_set_update_proc(s_footer_bg_layer, prv_footer_bg_update_proc);
  layer_add_child(window_layer, s_footer_bg_layer);

  // Battery, in reverse colors on the black footer
  s_battery_label_layer = prv_create_text_layer(GRect(8, FOOTER_Y - 2, 60, 14),
                                                label_font, GTextAlignmentLeft);
  text_layer_set_text_color(s_battery_label_layer, GColorWhite);
  text_layer_set_text(s_battery_label_layer, "BAT");
  layer_add_child(window_layer, text_layer_get_layer(s_battery_label_layer));

  s_battery_icon_layer = layer_create(GRect(38, FOOTER_Y, 40, 15));
  layer_set_update_proc(s_battery_icon_layer, prv_battery_icon_update_proc);
  layer_add_child(window_layer, s_battery_icon_layer);

  // Button hints, drawn on top of the black header/footer areas
  s_button_hints_layer = layer_create(bounds);
  layer_set_update_proc(s_button_hints_layer, prv_button_hints_update_proc);
  layer_add_child(window_layer, s_button_hints_layer);

  battery_state_service_subscribe(prv_battery_callback);
  prv_battery_callback(battery_state_service_peek());

  prv_refresh_alarm();

  time_t now = time(NULL);
  struct tm *tick_time = localtime(&now);
  s_last_mday = tick_time->tm_mday;
  prv_update_tick_subscription(tick_time);
  prv_update_date_and_week(tick_time);
  prv_update_time_display(tick_time);
}

static void prv_window_unload(Window *window) {
  layer_destroy(s_header_bg_layer);
  layer_destroy(s_footer_bg_layer);
  layer_destroy(s_button_hints_layer);
  layer_destroy(s_content_frame_layer);
  text_layer_destroy(s_header_logo_layer);
  text_layer_destroy(s_header_model_layer);
  text_layer_destroy(s_week_label_layer);
  layer_destroy(s_week_value_layer);
  layer_destroy(s_weekday_strip_layer);
  layer_destroy(s_middle_frame_layer);
  text_layer_destroy(s_battery_label_layer);
  layer_destroy(s_battery_icon_layer);
  layer_destroy(s_date_layer);
  layer_destroy(s_time_big_layer);
  text_layer_destroy(s_ampm_layer);
  layer_destroy(s_time_sec_layer);
  text_layer_destroy(s_alarm_label_layer);
  layer_destroy(s_alarm_value_layer);
  layer_destroy(s_bottom_band_layer);
  layer_destroy(s_weather_temp_layer);
  layer_destroy(s_chrome_layer);

  fonts_unload_custom_font(s_font_brand);
}

static void prv_init(void) {
  prv_load_settings();

  app_message_register_inbox_received(prv_inbox_received_callback);
  app_message_open(256, 256);

  s_window = window_create();
  window_set_background_color(s_window, GColorWhite);
  window_set_window_handlers(s_window, (WindowHandlers){
                                           .load = prv_window_load,
                                           .unload = prv_window_unload,
                                       });
  window_stack_push(s_window, true);
}

static void prv_deinit(void) {
  tick_timer_service_unsubscribe();
  battery_state_service_unsubscribe();
  window_destroy(s_window);
}

int main(void) {
  prv_init();
  app_event_loop();
  prv_deinit();
}
