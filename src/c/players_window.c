#include "ui.h"

/**
 * Screen 1: the list of players the LMS server knows about.
 *
 * Also the app's landing screen, so it carries the "talking to the server"
 * states -- there is nowhere else to show them yet.
 */

typedef struct {
  Window *window;
  MenuLayer *menu;
  LMSPlayer players[LMS_MAX_PLAYERS];
  int count;
  bool loading;
  char message[40];  //!< shown instead of the list while empty
} PlayersWindow;

static PlayersWindow *s_state;

static uint16_t prv_num_rows(MenuLayer *menu, uint16_t section, void *ctx) {
  PlayersWindow *state = ctx;
  return state->count > 0 ? state->count : 1;
}

static int16_t prv_cell_height(MenuLayer *menu, MenuIndex *index, void *ctx) {
  const MenuIndex selected = menu_layer_get_selected_index(menu);
  return ui_menu_cell_height(menu_index_compare(&selected, index) == 0);
}

static void prv_draw_row(GContext *gctx, const Layer *cell, MenuIndex *index,
                         void *ctx) {
  PlayersWindow *state = ctx;

  if (state->count == 0) {
    ui_draw_menu_row(gctx, cell, state->message);
    return;
  }

  // One line per row, so a player that is playing is marked in the text --
  // the same "> " the Alloy version used, rather than a subtitle that would
  // halve the type size again.
  const LMSPlayer *player = &state->players[index->row];
  char line[LMS_NAME_LEN + 3];
  snprintf(line, sizeof(line), "%s%s", player->playing ? "> " : "",
           player->name);
  ui_draw_menu_row(gctx, cell, line);
}

static void prv_on_selection_changed(MenuLayer *menu, MenuIndex new_index,
                                     MenuIndex old_index, void *ctx) {
  ui_marquee_reset();
}

static void prv_select(MenuLayer *menu, MenuIndex *index, void *ctx) {
  PlayersWindow *state = ctx;
  if (state->count == 0)
    return;

  const LMSPlayer *player = &state->players[index->row];
  status_window_push(player->id, player->name);
}

static void prv_on_players(const char *err, const LMSPlayer *players, int count,
                           void *ctx) {
  PlayersWindow *state = ctx;
  state->loading = false;

  if (err) {
    state->count = 0;
    snprintf(state->message, sizeof(state->message), "No LMS: %s", err);
  } else if (count == 0) {
    state->count = 0;
    strncpy(state->message, "No players", sizeof(state->message) - 1);
  } else {
    memcpy(state->players, players, sizeof(LMSPlayer) * count);
    state->count = count;
  }

  menu_layer_reload_data(state->menu);
}

static void prv_load_players(PlayersWindow *state) {
  if (state->loading)
    return;
  state->loading = true;
  state->count = 0;
  strncpy(state->message, "Loading players...", sizeof(state->message) - 1);
  menu_layer_reload_data(state->menu);
  lms_players(prv_on_players, state);
}

static void prv_window_load(Window *window) {
  PlayersWindow *state = window_get_user_data(window);
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);

  state->menu = menu_layer_create(bounds);
  menu_layer_set_callbacks(state->menu, state, (MenuLayerCallbacks){
      .get_num_rows = prv_num_rows,
      .get_cell_height = prv_cell_height,
      .draw_row = prv_draw_row,
      .select_click = prv_select,
      .selection_changed = prv_on_selection_changed,
  });
  ui_style_menu_layer(state->menu);
  menu_layer_set_click_config_onto_window(state->menu, window);
  layer_add_child(root, menu_layer_get_layer(state->menu));

  prv_load_players(state);
}

static void prv_window_unload(Window *window) {
  PlayersWindow *state = window_get_user_data(window);
  lms_cancel(state);
  menu_layer_destroy(state->menu);
  window_destroy(state->window);
  free(state);
  s_state = NULL;
}

/**
 * Refreshes on every return from the status screen, so a player that started
 * playing elsewhere shows up as such without the user reopening the app.
 */
static void prv_window_appear(Window *window) {
  PlayersWindow *state = window_get_user_data(window);
  ui_marquee_set_menu(state->menu);
  if (state->count > 0)
    lms_players(prv_on_players, state);
}

static void prv_window_disappear(Window *window) {
  ui_marquee_set_menu(NULL);
}

void players_window_push(void) {
  if (s_state)
    return;

  s_state = malloc(sizeof(PlayersWindow));
  if (!s_state)
    return;
  memset(s_state, 0, sizeof(PlayersWindow));

  s_state->window = window_create();
  window_set_user_data(s_state->window, s_state);
  window_set_background_color(s_state->window, UI_COLOR_BACKGROUND);
  window_set_window_handlers(s_state->window, (WindowHandlers){
      .load = prv_window_load,
      .appear = prv_window_appear,
      .disappear = prv_window_disappear,
      .unload = prv_window_unload,
  });
  window_stack_push(s_state->window, true);
}
