#include "ui.h"

/**
 * Screen 3: the LMS menu tree.
 *
 * One window per level, pushed on top of each other, so Back steps out of a
 * folder without any history of our own. The deepest sensible LMS path is far
 * short of MAX_LEVELS.
 *
 * Paging: MenuLayer is told the level's true row count, taken from the server,
 * while only part of the level is in memory. Deducing the end of a list from a
 * short page instead would be wrong -- the phone cuts pages on a record
 * boundary to fit one AppMessage, so a full level can arrive short.
 *
 * What is in memory is a contiguous run of items that slides as you scroll, not
 * a single page. Holding one page meant every fetch threw away the rows just
 * read, so scrolling left a trail of placeholders both ahead of and behind the
 * cursor. Several pages cost a few kilobytes, which this app has.
 *
 * On top of that the next page is fetched before the cursor reaches it, so in
 * steady scrolling the placeholder is never seen at all.
 */

#define MAX_LEVELS 10

/**
 * Items kept per level. Three pages is enough that the rows behind the cursor
 * survive a fetch ahead of it; at 100 bytes an item that is 3 KB per level.
 */
#define CACHE_PAGES 3
#define CACHE_ITEMS (LMS_PAGE * CACHE_PAGES)

/** How close the cursor may come to an unloaded edge before we fetch. */
#define PREFETCH_MARGIN 4

/**
 * Levels dropped from the saved path when playback closes the menu.
 *
 * Playback is started from deep inside -- a track inside an album inside a
 * list of albums -- and what is wanted next is another album, which sits two
 * levels above the track. Tuned by use rather than derived: one level up still
 * left a folder to re-enter every time.
 *
 * Nothing is lost by dropping them. Each remaining level keeps the selection it
 * had, so the way back down is already under the cursor.
 */
#define PATH_DROP_LEVELS 2

typedef struct BrowseWindow {
  Window *window;
  MenuLayer *menu;

  char player_id[LMS_ID_LEN];
  char req_id[LMS_ID_LEN];
  bool node;                 //!< request type for this level
  char title[LMS_TEXT_LEN];

  LMSItem cache[CACHE_ITEMS];
  int cache_start;           //!< index of cache[0] within the level
  int cache_count;
  int total;                 //!< rows at this level, from the server

  int requested_start;       //!< start of the fetch in flight
  uint16_t restore_row;      //!< selection to reinstate once the level loads
  bool restored;             //!< came from the saved path, not from a tap
  bool have_total;
  bool loading;
  char message[40];          //!< shown while the level has no rows yet
} BrowseWindow;

static BrowseWindow *s_levels[MAX_LEVELS];
static int s_level_count;

/**
 * Where the user was when playback took them away.
 *
 * Starting a track closes the whole menu, and coming back at the root meant
 * walking the tree again just to reach the album next to the one that was
 * picked -- which is the commonest thing to want.
 *
 * Only remembered across that jump, not across leaving the menu by hand: if
 * the user walked out level by level, they meant to leave. And only within one
 * run of the app -- these ids are keys into cachedMenu on the phone, which is
 * torn down with the app, so a path from a previous run would resolve to
 * nothing. A level that fails to load throws the path away and leaves the user
 * where they are.
 */
typedef struct {
  char req_id[LMS_ID_LEN];
  char title[LMS_TEXT_LEN];
  bool node;
  uint16_t selected;
} SavedLevel;

static SavedLevel s_saved[MAX_LEVELS];
static int s_saved_depth;
static char s_saved_player[LMS_ID_LEN];

// Set while browse_close_all() is unwinding, so the unload handler can tell a
// jump to the player from the user backing out of the root by hand.
static bool s_closing;

/**
 * Set while a saved path is being pushed back on.
 *
 * Levels normally fetch their first page as they load. Restoring pushes the
 * whole path at once, so that fired one request per level simultaneously and
 * ran the relay out of slots -- four, between lms.c and comm.c -- which came
 * back as "busy" on reopening. Only the level the user ends up looking at is
 * fetched now; the ones underneath load if and when Back reaches them.
 */
static bool s_restoring;

static void prv_push(const char *player_id, const char *req_id, bool node,
                     const char *title, uint16_t selected, bool restored);
static void prv_load_page(BrowseWindow *state, int start);

static void prv_forget_path(void) {
  s_saved_depth = 0;
  s_saved_player[0] = '\0';
}

/** True when `row` is one of the items currently in memory. */
static bool prv_row_loaded(const BrowseWindow *state, int row) {
  return state->have_total && row >= state->cache_start &&
         row < state->cache_start + state->cache_count;
}

static int prv_cache_end(const BrowseWindow *state) {
  return state->cache_start + state->cache_count;
}

static void prv_cache_reset(BrowseWindow *state, int start,
                            const LMSItem *items, int count) {
  memcpy(state->cache, items, sizeof(LMSItem) * count);
  state->cache_start = start;
  state->cache_count = count;
}

/**
 * Folds an arriving page into the cached run.
 *
 * Only a page that touches the run can extend it; anything else is a jump, and
 * the run restarts there. Overflow is dropped from the far end, so the items
 * nearest the cursor are the ones that survive.
 */
static void prv_cache_merge(BrowseWindow *state, int start,
                            const LMSItem *items, int count) {
  if (count <= 0)
    return;
  if (state->cache_count == 0 || count >= CACHE_ITEMS) {
    prv_cache_reset(state, start, items, count);
    return;
  }

  if (start == prv_cache_end(state)) {
    const int overflow = state->cache_count + count - CACHE_ITEMS;
    if (overflow > 0) {
      memmove(state->cache, state->cache + overflow,
              sizeof(LMSItem) * (state->cache_count - overflow));
      state->cache_start += overflow;
      state->cache_count -= overflow;
    }
    memcpy(state->cache + state->cache_count, items, sizeof(LMSItem) * count);
    state->cache_count += count;
    return;
  }

  if (start + count == state->cache_start) {
    const int overflow = state->cache_count + count - CACHE_ITEMS;
    if (overflow > 0)
      state->cache_count -= overflow;  // drop the far end
    memmove(state->cache + count, state->cache,
            sizeof(LMSItem) * state->cache_count);
    memcpy(state->cache, items, sizeof(LMSItem) * count);
    state->cache_start = start;
    state->cache_count += count;
    return;
  }

  if (start >= state->cache_start && start + count <= prv_cache_end(state)) {
    memcpy(state->cache + (start - state->cache_start), items,
           sizeof(LMSItem) * count);
    return;
  }

  prv_cache_reset(state, start, items, count);
}

/**
 * Keeps the run around the cursor filled.
 *
 * Three jobs: fetch the page the cursor landed on if it is not there, and pull
 * in the next or previous page once the cursor comes within PREFETCH_MARGIN of
 * an edge. Called when the selection moves and again after a page lands --
 * scrolling past a pending request would otherwise leave the cursor parked on
 * rows nobody ever asked for.
 *
 * The two prefetch directions are split at the middle of the run so they cannot
 * take turns evicting each other's items on a full cache.
 */
static void prv_ensure_selection_loaded(BrowseWindow *state) {
  if (state->loading || !state->have_total || state->total == 0)
    return;

  const int row = menu_layer_get_selected_index(state->menu).row;

  if (!prv_row_loaded(state, row)) {
    prv_load_page(state, (row / LMS_PAGE) * LMS_PAGE);
    return;
  }

  const int end = prv_cache_end(state);
  const int middle = state->cache_start + state->cache_count / 2;

  if (row >= middle) {
    if (row + PREFETCH_MARGIN >= end && end < state->total)
      prv_load_page(state, end);
    return;
  }

  if (row - PREFETCH_MARGIN < state->cache_start && state->cache_start > 0) {
    const int start = state->cache_start - LMS_PAGE;
    prv_load_page(state, start < 0 ? 0 : start);
  }
}

static void prv_on_page(const char *err, const LMSPage *page, void *ctx) {
  BrowseWindow *state = ctx;
  state->loading = false;

  if (err) {
    snprintf(state->message, sizeof(state->message), "Error: %s", err);
    if (!state->have_total)
      menu_layer_reload_data(state->menu);
    // A restored level that will not load means the phone no longer knows
    // these ids. Keeping the path would fail the same way every time.
    if (state->restored)
      prv_forget_path();
    return;
  }

  // Committed only now: a failed fetch must not make the cached items claim
  // the range it asked for.
  prv_cache_merge(state, state->requested_start, page->items, page->count);
  state->total = page->total;
  state->have_total = true;
  if (page->total == 0)
    strncpy(state->message, "Empty", sizeof(state->message) - 1);

  menu_layer_reload_data(state->menu);

  // Reinstating the row has to wait for the total: before that the list has one
  // message row and any index would be clamped to it.
  if (state->restore_row) {
    menu_layer_set_selected_index(state->menu,
                                  MenuIndex(0, state->restore_row), MenuRowAlignCenter,
                                  false);
    state->restore_row = 0;
  }

  prv_ensure_selection_loaded(state);
}

static void prv_load_page(BrowseWindow *state, int start) {
  state->loading = true;
  state->requested_start = start;
  lms_menu(state->player_id, start, state->node, state->req_id, prv_on_page,
           state);
}

// --- menu layer ------------------------------------------------------------

static uint16_t prv_num_rows(MenuLayer *menu, uint16_t section, void *ctx) {
  BrowseWindow *state = ctx;
  if (!state->have_total || state->total == 0)
    return 1;  // the message row
  return state->total;
}

static int16_t prv_header_height(MenuLayer *menu, uint16_t section, void *ctx) {
  return ui_menu_header_height();
}

static int16_t prv_cell_height(MenuLayer *menu, MenuIndex *index, void *ctx) {
  const MenuIndex selected = menu_layer_get_selected_index(menu);
  return ui_menu_cell_height(menu_index_compare(&selected, index) == 0);
}

static void prv_draw_header(GContext *gctx, const Layer *cell, uint16_t section,
                            void *ctx) {
  BrowseWindow *state = ctx;
  ui_draw_menu_header(gctx, cell, state->title);
}

static void prv_draw_row(GContext *gctx, const Layer *cell, MenuIndex *index,
                         void *ctx) {
  BrowseWindow *state = ctx;

  if (!state->have_total || state->total == 0) {
    ui_draw_menu_row(gctx, cell, state->message);
    return;
  }
  if (!prv_row_loaded(state, index->row)) {
    ui_draw_menu_row(gctx, cell, "...");
    return;
  }

  // "> " marks a folder, so a list mixing albums and tracks reads at a glance.
  const LMSItem *item = &state->cache[index->row - state->cache_start];
  const bool folder = item->kind == LMSItemNode || item->kind == LMSItemCmd;
  char line[LMS_TEXT_LEN + 3];
  snprintf(line, sizeof(line), "%s%s", folder ? "> " : "", item->text);
  ui_draw_menu_row(gctx, cell, line);
}

static void prv_on_selection_changed(MenuLayer *menu, MenuIndex new_index,
                                     MenuIndex old_index, void *ctx) {
  ui_marquee_reset();
  prv_ensure_selection_loaded(ctx);
}

static void prv_on_played(const char *err, void *ctx) {
  BrowseWindow *state = ctx;
  if (err) {
    snprintf(state->message, sizeof(state->message), "Error: %s", err);
    menu_layer_reload_data(state->menu);
    return;
  }
  // Playback started -- the interesting screen is now the player, not the
  // folder the track happened to live in.
  browse_close_all();
}

static void prv_select(MenuLayer *menu, MenuIndex *index, void *ctx) {
  BrowseWindow *state = ctx;
  if (!prv_row_loaded(state, index->row))
    return;

  const LMSItem *item = &state->cache[index->row - state->cache_start];

  switch (item->kind) {
    case LMSItemNode:
    case LMSItemCmd:
      prv_push(state->player_id, item->id, item->kind == LMSItemNode,
               item->text, 0, false);
      break;
    case LMSItemPlay:
      lms_menu_go(state->player_id, item->id, prv_on_played, state);
      break;
    case LMSItemNone:
      break;
  }
}

// --- window ----------------------------------------------------------------

static void prv_window_load(Window *window) {
  BrowseWindow *state = window_get_user_data(window);
  Layer *root = window_get_root_layer(window);

  state->menu = menu_layer_create(layer_get_bounds(root));
  menu_layer_set_callbacks(state->menu, state, (MenuLayerCallbacks){
      .get_num_rows = prv_num_rows,
      .get_cell_height = prv_cell_height,
      .get_header_height = prv_header_height,
      .draw_header = prv_draw_header,
      .draw_row = prv_draw_row,
      .select_click = prv_select,
      .selection_changed = prv_on_selection_changed,
  });
  ui_style_menu_layer(state->menu);
  menu_layer_set_click_config_onto_window(state->menu, window);
  layer_add_child(root, menu_layer_get_layer(state->menu));

  if (!s_restoring)
    prv_load_page(state, 0);
}

static void prv_window_appear(Window *window) {
  BrowseWindow *state = window_get_user_data(window);
  ui_marquee_set_menu(state->menu);

  // A restored level below the top has nothing yet; this is where it gets it,
  // once Back actually brings the user to it.
  if (!s_restoring && !state->have_total && !state->loading)
    prv_load_page(state, 0);
}

static void prv_window_disappear(Window *window) {
  ui_marquee_set_menu(NULL);
}

static void prv_window_unload(Window *window) {
  BrowseWindow *state = window_get_user_data(window);
  lms_cancel(state);

  for (int i = 0; i < s_level_count; i++) {
    if (s_levels[i] == state) {
      for (int j = i; j < s_level_count - 1; j++)
        s_levels[j] = s_levels[j + 1];
      s_level_count--;
      break;
    }
  }

  // Backing out of the root by hand means the user meant to leave; only the
  // jump to the player is worth resuming.
  if (!s_closing && s_level_count == 0)
    prv_forget_path();

  menu_layer_destroy(state->menu);
  window_destroy(state->window);
  free(state);
}

static void prv_push(const char *player_id, const char *req_id, bool node,
                     const char *title, uint16_t selected, bool restored) {
  if (s_level_count >= MAX_LEVELS)
    return;

  BrowseWindow *state = malloc(sizeof(BrowseWindow));
  if (!state)
    return;
  memset(state, 0, sizeof(BrowseWindow));

  strncpy(state->player_id, player_id, sizeof(state->player_id) - 1);
  strncpy(state->req_id, req_id, sizeof(state->req_id) - 1);
  strncpy(state->title, title, sizeof(state->title) - 1);
  strncpy(state->message, "Loading...", sizeof(state->message) - 1);
  state->node = node;
  state->restore_row = selected;
  state->restored = restored;

  state->window = window_create();
  window_set_user_data(state->window, state);
  window_set_background_color(state->window, UI_COLOR_BACKGROUND);
  window_set_window_handlers(state->window, (WindowHandlers){
      .load = prv_window_load,
      .appear = prv_window_appear,
      .disappear = prv_window_disappear,
      .unload = prv_window_unload,
  });

  s_levels[s_level_count++] = state;
  window_stack_push(state->window, true);
}

void browse_window_push_root(const char *player_id) {
  // A path belongs to the player it was walked for.
  if (s_saved_depth > 0 && strcmp(s_saved_player, player_id) != 0)
    prv_forget_path();

  if (s_saved_depth == 0) {
    prv_push(player_id, "home", true, "LMS", 0, false);
    return;
  }

  // Rebuilt from the root up, so Back still walks out the way it came in.
  const int depth = s_saved_depth;
  SavedLevel path[MAX_LEVELS];
  memcpy(path, s_saved, sizeof(SavedLevel) * depth);
  prv_forget_path();

  s_restoring = true;
  for (int i = 0; i < depth; i++)
    prv_push(player_id, path[i].req_id, path[i].node, path[i].title,
             path[i].selected, true);
  s_restoring = false;

  if (s_level_count > 0)
    prv_load_page(s_levels[s_level_count - 1], 0);
}

void browse_close_all(void) {
  // Snapshot before unwinding: this is the jump to the player, and coming back
  // at the root is exactly what makes picking a second album tedious.
  //
  // See PATH_DROP_LEVELS for why the last levels are left out.
  s_saved_depth = 0;
  for (int i = 0; i < s_level_count - PATH_DROP_LEVELS && i < MAX_LEVELS; i++) {
    BrowseWindow *level = s_levels[i];
    strncpy(s_saved[i].req_id, level->req_id, sizeof(s_saved[i].req_id) - 1);
    s_saved[i].req_id[sizeof(s_saved[i].req_id) - 1] = '\0';
    strncpy(s_saved[i].title, level->title, sizeof(s_saved[i].title) - 1);
    s_saved[i].title[sizeof(s_saved[i].title) - 1] = '\0';
    s_saved[i].node = level->node;
    s_saved[i].selected = menu_layer_get_selected_index(level->menu).row;
    s_saved_depth++;
  }
  if (s_saved_depth > 0) {
    strncpy(s_saved_player, s_levels[0]->player_id, sizeof(s_saved_player) - 1);
    s_saved_player[sizeof(s_saved_player) - 1] = '\0';
  }

  // Each removal unloads its window, which takes the level back out of
  // s_levels -- hence the loop condition rather than an index walk.
  s_closing = true;
  int guard = MAX_LEVELS + 1;
  while (s_level_count > 0 && guard-- > 0)
    window_stack_remove(s_levels[s_level_count - 1]->window, false);
  s_closing = false;
}
