#include "ui.h"
#include "comm.h"

/**
 * Screen 2: what the selected player is doing, and the controls for it.
 *
 * Per navigation_concept.md the touchscreen carries playback control, which
 * frees the hardware buttons for navigation: Select opens the LMS menu, Up and
 * Down double as volume, Back returns to the player list.
 *
 * Cover art sits above the text. The watch decides how big it wants the image
 * from the space the layout leaves over, and the phone has LMS scale it to
 * exactly that -- see lms_cover(). The pixels arrive as ARGB2222, one byte per
 * pixel, straight into a blank GBitmap this window owns.
 */

// How far a finger has to travel before a touch counts as a swipe rather than
// a tap, and how still it has to stay to count as a tap at all.
#define SWIPE_MIN_PX 30
#define TAP_MAX_PX 10

// slim.request words, not numbers, because that is what the relay forwards.
#define VOLUME_UP   "mixer volume +5"
#define VOLUME_DOWN "mixer volume -5"

/**
 * Track skipping.
 *
 * Deliberately not "button jump_fwd"/"jump_rew": those are the CD-player
 * buttons, and jump_rew rewinds to the start of the current track before it
 * will go anywhere, so the first swipe appeared to do nothing but restart the
 * song. "playlist index" moves by one entry and nothing else.
 */
#define TRACK_NEXT     "playlist index +1"
#define TRACK_PREVIOUS "playlist index -1"

// LMS needs a moment to act on a command before `status` reports the result.
#define REFRESH_DELAY_MS 600

/**
 * How often the status is re-read while this screen is up.
 *
 * Polling rather than reacting to a track change, because there is nothing to
 * react to: plain JSON-RPC has no push, and LMS's push channel is CometD long
 * polling, which is a great deal of machinery to run from PebbleKit JS. A
 * status query is a few hundred bytes, so asking now and then is the cheaper
 * trade -- and it is what makes a radio station's title, artist and artwork
 * appear without stopping and starting playback by hand.
 *
 * Only ever while the screen is actually on top, and slower when nothing is
 * playing, since then the only thing that can change is somebody else's doing.
 */
#define POLL_PLAYING_MS 10000
#define POLL_IDLE_MS    30000

#define TEXT_STATE_H  28
#define TEXT_TITLE_H  34
#define TEXT_ARTIST_H 28

typedef struct {
  Window *window;
  TextLayer *artist_layer;
  TextLayer *title_layer;
  TextLayer *state_layer;
  BitmapLayer *cover_layer;

  char player_id[LMS_ID_LEN];
  char player_name[LMS_NAME_LEN];

  char artist_text[LMS_TRACK_LEN];
  char title_text[LMS_TRACK_LEN];
  char state_text[40];

  GBitmap *cover;
  uint8_t *cover_data;
  uint32_t cover_capacity;   //!< bytes the GBitmap can hold
  uint32_t cover_expected;
  uint32_t cover_received;
  int16_t cover_w;           //!< full screen width
  int16_t cover_h;           //!< what is free above the text; 0 = no room
  bool cover_receiving;
  char cover_id[LMS_COVER_ID_LEN];  //!< artwork the current cover is

  int16_t touch_x;
  int16_t touch_y;
  bool touch_down;

  AppTimer *refresh_timer;
} StatusWindow;

static StatusWindow *s_state;

static void prv_request_status(StatusWindow *state);
static void prv_schedule_refresh(StatusWindow *state, uint32_t delay_ms);

// --- cover -----------------------------------------------------------------

static void prv_drop_cover(StatusWindow *state) {
  state->cover_receiving = false;
  state->cover_data = NULL;
  state->cover_capacity = 0;
  state->cover_expected = 0;
  state->cover_received = 0;
  if (state->cover_layer)
    bitmap_layer_set_bitmap(state->cover_layer, NULL);
  if (state->cover) {
    gbitmap_destroy(state->cover);
    state->cover = NULL;
  }
}

static void prv_on_cover(const char *err, const LMSCover *cover, void *ctx) {
  StatusWindow *state = ctx;
  if (err) {
    // Nothing to show is better than the previous track's sleeve, but a
    // transient failure should not blank a cover that is already correct.
    state->cover_receiving = false;
    return;
  }

  prv_drop_cover(state);

  state->cover = gbitmap_create_blank(GSize(cover->width, cover->height),
                                      GBitmapFormat8Bit);
  if (!state->cover)
    return;

  state->cover_data = gbitmap_get_data(state->cover);
  state->cover_capacity =
      (uint32_t)gbitmap_get_bytes_per_row(state->cover) * cover->height;
  state->cover_expected = cover->byte_length < state->cover_capacity
                              ? cover->byte_length : state->cover_capacity;
  state->cover_received = 0;
  state->cover_receiving = true;

  // Shown while it fills. gbitmap_create_blank zeroes the buffer, and a zero
  // byte in ARGB2222 is fully transparent, so the unwritten part simply is not
  // drawn rather than showing as noise.
  bitmap_layer_set_bitmap(state->cover_layer, state->cover);
}

static void prv_on_cover_chunk(uint32_t offset, const uint8_t *data,
                               uint16_t length, void *ctx) {
  StatusWindow *state = ctx;
  if (!state->cover_receiving || !state->cover_data)
    return;
  // Chunks of an abandoned transfer can still be in flight; anything that does
  // not fit the buffer we have now is from one of those.
  if (offset + length > state->cover_capacity)
    return;

  memcpy(state->cover_data + offset, data, length);
  state->cover_received += length;
  if (state->cover_received >= state->cover_expected)
    state->cover_receiving = false;

  layer_mark_dirty(bitmap_layer_get_layer(state->cover_layer));
}

static void prv_request_cover(StatusWindow *state, const char *cover_id) {
  if (state->cover_w <= 0 || state->cover_h <= 0)
    return;
  lms_cover(cover_id, state->cover_w, state->cover_h, prv_on_cover, state);
}

// --- status ----------------------------------------------------------------

static void prv_on_status(const char *err, const LMSStatus *status, void *ctx) {
  StatusWindow *state = ctx;

  if (err) {
    snprintf(state->state_text, sizeof(state->state_text), "Error: %s", err);
    text_layer_set_text(state->state_layer, state->state_text);
    prv_schedule_refresh(state, POLL_IDLE_MS);  // keep trying, slowly
    return;
  }

  // Every response schedules the next read, so the chain cannot run twice or
  // stop dead after an error.
  prv_schedule_refresh(state,
                       status->playing ? POLL_PLAYING_MS : POLL_IDLE_MS);

  strncpy(state->artist_text, status->artist[0] ? status->artist : "—",
          sizeof(state->artist_text) - 1);
  state->artist_text[sizeof(state->artist_text) - 1] = '\0';

  strncpy(state->title_text, status->title[0] ? status->title : "Nothing playing",
          sizeof(state->title_text) - 1);
  state->title_text[sizeof(state->title_text) - 1] = '\0';

  snprintf(state->state_text, sizeof(state->state_text), "%s   Vol %d",
           status->playing ? "Playing" : "Paused", status->volume);

  text_layer_set_text(state->artist_layer, state->artist_text);
  text_layer_set_text(state->title_layer, state->title_text);
  text_layer_set_text(state->state_layer, state->state_text);

  // Keyed on the artwork, not the track: playing an album through keeps the
  // same coverid, so the sleeve is fetched once instead of once per song.
  if (strncmp(state->cover_id, status->cover_id, LMS_COVER_ID_LEN) == 0)
    return;

  strncpy(state->cover_id, status->cover_id, sizeof(state->cover_id) - 1);
  state->cover_id[sizeof(state->cover_id) - 1] = '\0';

  if (state->cover_id[0] == '\0') {
    prv_drop_cover(state);  // this track has no artwork
    return;
  }
  prv_request_cover(state, state->cover_id);
}

static void prv_on_refresh_timer(void *ctx) {
  StatusWindow *state = ctx;
  state->refresh_timer = NULL;
  prv_request_status(state);
}

/** Replaces whatever refresh was pending with one in `delay_ms`. */
static void prv_schedule_refresh(StatusWindow *state, uint32_t delay_ms) {
  if (state->refresh_timer)
    app_timer_cancel(state->refresh_timer);
  state->refresh_timer =
      app_timer_register(delay_ms, prv_on_refresh_timer, state);
}

static void prv_request_status(StatusWindow *state) {
  lms_status(state->player_id, prv_on_status, state);
}

static void prv_on_command(const char *err, void *ctx) {
  StatusWindow *state = ctx;
  if (err) {
    snprintf(state->state_text, sizeof(state->state_text), "Error: %s", err);
    text_layer_set_text(state->state_layer, state->state_text);
    return;
  }
  prv_schedule_refresh(state, REFRESH_DELAY_MS);
}

static void prv_send(StatusWindow *state, const char *command) {
  lms_command(state->player_id, command, prv_on_command, state);
}

// --- touch -----------------------------------------------------------------

static void prv_on_touch(const TouchEvent *event, void *ctx) {
  StatusWindow *state = ctx;

  if (event->type == TouchEvent_Touchdown) {
    state->touch_x = event->x;
    state->touch_y = event->y;
    state->touch_down = true;
    return;
  }
  if (event->type != TouchEvent_Liftoff || !state->touch_down)
    return;

  state->touch_down = false;
  const int dx = event->x - state->touch_x;
  const int dy = event->y - state->touch_y;
  const int adx = abs(dx);
  const int ady = abs(dy);

  if (adx > ady && adx > SWIPE_MIN_PX) {
    // Right for the next track. The carousel reading -- drag the sleeve left to
    // bring the next one on -- is the other common convention, so this is a
    // one-line change if it ever feels wrong.
    prv_send(state, dx > 0 ? TRACK_NEXT : TRACK_PREVIOUS);
  } else if (ady > adx && ady > SWIPE_MIN_PX) {
    prv_send(state, dy < 0 ? VOLUME_UP : VOLUME_DOWN);
  } else if (adx < TAP_MAX_PX && ady < TAP_MAX_PX) {
    prv_send(state, "pause");
  }
}

// --- buttons ---------------------------------------------------------------

static void prv_click_select(ClickRecognizerRef recognizer, void *ctx) {
  StatusWindow *state = ctx;
  browse_window_push_root(state->player_id);
}

static void prv_click_up(ClickRecognizerRef recognizer, void *ctx) {
  prv_send(ctx, VOLUME_UP);
}

static void prv_click_down(ClickRecognizerRef recognizer, void *ctx) {
  prv_send(ctx, VOLUME_DOWN);
}

static void prv_click_config(void *ctx) {
  window_single_click_subscribe(BUTTON_ID_SELECT, prv_click_select);
  window_single_repeating_click_subscribe(BUTTON_ID_UP, 200, prv_click_up);
  window_single_repeating_click_subscribe(BUTTON_ID_DOWN, 200, prv_click_down);
}

// --- window ----------------------------------------------------------------

static TextLayer *prv_make_label(Layer *root, GRect frame, const char *font,
                                 GColor color) {
  TextLayer *layer = text_layer_create(frame);
  text_layer_set_background_color(layer, GColorClear);
  text_layer_set_text_color(layer, color);
  text_layer_set_font(layer, fonts_get_system_font(font));
  text_layer_set_text_alignment(layer, PBL_IF_ROUND_ELSE(GTextAlignmentCenter,
                                                         GTextAlignmentLeft));
  layer_add_child(root, text_layer_get_layer(layer));
  return layer;
}

static void prv_window_load(Window *window) {
  StatusWindow *state = window_get_user_data(window);
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);

  const int16_t inset = PBL_IF_ROUND_ELSE(24, 6);
  const int16_t width = bounds.size.w - inset * 2;

  // Laid out from the bottom, and the cover takes whatever is left above. That
  // way the text keeps its size on both screens and only the image adapts.
  const int16_t state_y = bounds.size.h - TEXT_STATE_H - PBL_IF_ROUND_ELSE(20, 2);
  const int16_t title_y = state_y - TEXT_TITLE_H;
  const int16_t artist_y = title_y - TEXT_ARTIST_H;

  state->artist_layer = prv_make_label(root, GRect(inset, artist_y, width, TEXT_ARTIST_H),
                                       FONT_KEY_GOTHIC_24, UI_COLOR_FOREGROUND);
  state->title_layer = prv_make_label(root, GRect(inset, title_y, width, TEXT_TITLE_H),
                                      FONT_KEY_GOTHIC_28_BOLD, UI_COLOR_FOREGROUND);
  state->state_layer = prv_make_label(root, GRect(inset, state_y, width, TEXT_STATE_H),
                                      FONT_KEY_GOTHIC_24, UI_COLOR_FOREGROUND);

  text_layer_set_overflow_mode(state->title_layer, GTextOverflowModeTrailingEllipsis);
  text_layer_set_overflow_mode(state->artist_layer, GTextOverflowModeTrailingEllipsis);

  // Full width, flush to the top, and only as tall as the space above the text.
  // A sleeve is square, so the phone crops the bottom off rather than sending
  // rows that would be clipped here anyway.
  state->cover_w = bounds.size.w;
  state->cover_h = artist_y > 0 ? artist_y : 0;

  state->cover_layer = bitmap_layer_create(
      GRect(0, 0, state->cover_w, state->cover_h));
  bitmap_layer_set_background_color(state->cover_layer, GColorClear);
  // ARGB2222 carries alpha, and GCompOpSet is what honours it.
  bitmap_layer_set_compositing_mode(state->cover_layer, GCompOpSet);
  layer_add_child(root, bitmap_layer_get_layer(state->cover_layer));

  strncpy(state->title_text, state->player_name, sizeof(state->title_text) - 1);
  strncpy(state->state_text, "Loading...", sizeof(state->state_text) - 1);
  text_layer_set_text(state->title_layer, state->title_text);
  text_layer_set_text(state->state_layer, state->state_text);
  text_layer_set_text(state->artist_layer, state->artist_text);
}

static void prv_window_appear(Window *window) {
  StatusWindow *state = window_get_user_data(window);
  // Only while this screen is on top: the touch service is global, and a
  // subscription that outlived the window would steer a player the user has
  // already left.
  touch_service_subscribe(prv_on_touch, state);
  comm_set_image_handler(prv_on_cover_chunk, state);
  prv_request_status(state);
}

static void prv_window_disappear(Window *window) {
  StatusWindow *state = window_get_user_data(window);
  touch_service_unsubscribe();
  comm_set_image_handler(NULL, NULL);
  // No polling behind the LMS menu or the player list.
  if (state->refresh_timer) {
    app_timer_cancel(state->refresh_timer);
    state->refresh_timer = NULL;
  }
}

static void prv_window_unload(Window *window) {
  StatusWindow *state = window_get_user_data(window);
  lms_cancel(state);
  if (state->refresh_timer)
    app_timer_cancel(state->refresh_timer);
  prv_drop_cover(state);
  bitmap_layer_destroy(state->cover_layer);
  text_layer_destroy(state->artist_layer);
  text_layer_destroy(state->title_layer);
  text_layer_destroy(state->state_layer);
  window_destroy(state->window);
  free(state);
  s_state = NULL;
}

void status_window_push(const char *player_id, const char *player_name) {
  if (s_state)
    return;

  s_state = malloc(sizeof(StatusWindow));
  if (!s_state)
    return;
  memset(s_state, 0, sizeof(StatusWindow));

  strncpy(s_state->player_id, player_id, sizeof(s_state->player_id) - 1);
  strncpy(s_state->player_name, player_name, sizeof(s_state->player_name) - 1);

  s_state->window = window_create();
  window_set_user_data(s_state->window, s_state);
  window_set_background_color(s_state->window, UI_COLOR_BACKGROUND);
  window_set_click_config_provider_with_context(s_state->window,
                                                prv_click_config, s_state);
  window_set_window_handlers(s_state->window, (WindowHandlers){
      .load = prv_window_load,
      .appear = prv_window_appear,
      .disappear = prv_window_disappear,
      .unload = prv_window_unload,
  });
  window_stack_push(s_state->window, true);
}
