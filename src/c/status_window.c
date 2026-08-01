#include "ui.h"

/**
 * Screen 2: what the selected player is doing, and the controls for it.
 *
 * Per navigation_concept.md the touchscreen carries playback control, which
 * frees the hardware buttons for navigation: Select opens the LMS menu, Up and
 * Down double as volume, Back returns to the player list.
 *
 * Cover art belongs at the top of this screen. It is not here yet -- the space
 * above the artist line is where it goes.
 */

// How far a finger has to travel before a touch counts as a swipe rather than
// a tap, and how still it has to stay to count as a tap at all.
#define SWIPE_MIN_PX 30
#define TAP_MAX_PX 10

// slim.request words, not numbers, because that is what the relay forwards.
#define VOLUME_UP   "mixer volume +5"
#define VOLUME_DOWN "mixer volume -5"

// LMS needs a moment to act on a command before `status` reports the result.
#define REFRESH_DELAY_MS 600

typedef struct {
  Window *window;
  TextLayer *artist_layer;
  TextLayer *title_layer;
  TextLayer *state_layer;

  char player_id[LMS_ID_LEN];
  char player_name[LMS_NAME_LEN];

  char artist_text[LMS_TRACK_LEN];
  char title_text[LMS_TRACK_LEN];
  char state_text[40];

  int16_t touch_x;
  int16_t touch_y;
  bool touch_down;

  AppTimer *refresh_timer;
} StatusWindow;

static StatusWindow *s_state;

static void prv_request_status(StatusWindow *state);

static void prv_on_status(const char *err, const LMSStatus *status, void *ctx) {
  StatusWindow *state = ctx;

  if (err) {
    snprintf(state->state_text, sizeof(state->state_text), "Error: %s", err);
    text_layer_set_text(state->state_layer, state->state_text);
    return;
  }

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
}

static void prv_on_refresh_timer(void *ctx) {
  StatusWindow *state = ctx;
  state->refresh_timer = NULL;
  prv_request_status(state);
}

static void prv_request_status(StatusWindow *state) {
  lms_status(state->player_id, prv_on_status, state);
}

/** Re-reads the status shortly after a command, once LMS has acted on it. */
static void prv_refresh_soon(StatusWindow *state) {
  if (state->refresh_timer)
    app_timer_cancel(state->refresh_timer);
  state->refresh_timer =
      app_timer_register(REFRESH_DELAY_MS, prv_on_refresh_timer, state);
}

static void prv_on_command(const char *err, void *ctx) {
  StatusWindow *state = ctx;
  if (err) {
    snprintf(state->state_text, sizeof(state->state_text), "Error: %s", err);
    text_layer_set_text(state->state_layer, state->state_text);
    return;
  }
  prv_refresh_soon(state);
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
    // Swiping left moves forward, the way a page turns.
    prv_send(state, dx < 0 ? "button jump_fwd" : "button jump_rew");
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

  // Laid out from the bottom so the free space at the top -- where cover art
  // goes later -- grows and shrinks with the screen rather than the text.
  const int16_t state_h = 24;
  const int16_t title_h = 56;
  const int16_t artist_h = 24;
  const int16_t state_y = bounds.size.h - state_h - PBL_IF_ROUND_ELSE(14, 4);
  const int16_t title_y = state_y - title_h;
  const int16_t artist_y = title_y - artist_h;

  state->artist_layer = prv_make_label(root, GRect(inset, artist_y, width, artist_h),
                                       FONT_KEY_GOTHIC_18, UI_COLOR_MUTED);
  state->title_layer = prv_make_label(root, GRect(inset, title_y, width, title_h),
                                      FONT_KEY_GOTHIC_28_BOLD, UI_COLOR_FOREGROUND);
  state->state_layer = prv_make_label(root, GRect(inset, state_y, width, state_h),
                                      FONT_KEY_GOTHIC_18, UI_COLOR_HIGHLIGHT);

  text_layer_set_overflow_mode(state->title_layer, GTextOverflowModeTrailingEllipsis);
  text_layer_set_overflow_mode(state->artist_layer, GTextOverflowModeTrailingEllipsis);

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
  prv_request_status(state);
}

static void prv_window_disappear(Window *window) {
  touch_service_unsubscribe();
}

static void prv_window_unload(Window *window) {
  StatusWindow *state = window_get_user_data(window);
  lms_cancel(state);
  if (state->refresh_timer)
    app_timer_cancel(state->refresh_timer);
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
