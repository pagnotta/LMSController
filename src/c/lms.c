#include "lms.h"
#include "comm.h"

// In-flight requests. Each holds the typed handler and the caller's context;
// the address of the slot is what comm.c sees, so lms_cancel() can find and
// drop exactly the requests belonging to one window.
#define MAX_INFLIGHT 4

typedef enum {
  KindPlayers,
  KindStatus,
  KindPage,
  KindDone,
  KindCover,
} RequestKind;

typedef struct {
  bool used;
  RequestKind kind;
  void *user_context;
  union {
    LMSPlayersHandler players;
    LMSStatusHandler status;
    LMSPageHandler page;
    LMSDoneHandler done;
    LMSCoverHandler cover;
  } handler;
} Request;

static Request s_requests[MAX_INFLIGHT];

static Request *prv_take_slot(void) {
  for (int i = 0; i < MAX_INFLIGHT; i++)
    if (!s_requests[i].used) {
      s_requests[i].used = true;
      return &s_requests[i];
    }
  return NULL;
}

/**
 * Cuts the next field out of a mutable buffer.
 *
 * Returns the field, NUL-terminated in place, and leaves *cursor on the
 * character after the separator -- or NULL once the buffer is spent, which is
 * what ends the caller's loop.
 */
static char *prv_take(char **cursor, char separator) {
  char *start = *cursor;
  if (!start)
    return NULL;

  char *p = start;
  while (*p && *p != separator)
    p++;

  if (*p == separator) {
    *p = '\0';
    *cursor = p + 1;
  } else {
    *cursor = NULL;  // last field; the next call ends the loop
  }
  return start;
}

static void prv_copy(char *dst, size_t size, const char *src) {
  if (!src)
    src = "";
  strncpy(dst, src, size - 1);
  dst[size - 1] = '\0';
}

static LMSItemKind prv_kind(const char *text) {
  if (!text)
    return LMSItemNone;
  if (strcmp(text, "node") == 0)
    return LMSItemNode;
  if (strcmp(text, "cmd") == 0)
    return LMSItemCmd;
  if (strcmp(text, "play") == 0)
    return LMSItemPlay;
  return LMSItemNone;
}

/** Releases the slot before the handler runs, so handlers may start requests. */
static void prv_finish_players(Request *r, const char *err,
                               const LMSPlayer *players, int count) {
  LMSPlayersHandler handler = r->handler.players;
  void *context = r->user_context;
  r->used = false;
  if (handler)
    handler(err, players, count, context);
}

static void prv_on_players(const char *err, const char *data, void *context) {
  Request *r = context;
  if (err) {
    prv_finish_players(r, err, NULL, 0);
    return;
  }

  char buffer[COMM_MAX_DATA + 1];
  prv_copy(buffer, sizeof(buffer), data);

  LMSPlayer players[LMS_MAX_PLAYERS];
  int count = 0;
  char *cursor = buffer;

  while (cursor && *cursor && count < LMS_MAX_PLAYERS) {
    char *record = prv_take(&cursor, LMS_RECORD);
    if (!record || !*record)
      continue;

    char *field = record;
    prv_copy(players[count].name, LMS_NAME_LEN, prv_take(&field, LMS_FIELD));
    prv_copy(players[count].id, LMS_ID_LEN, prv_take(&field, LMS_FIELD));
    const char *playing = prv_take(&field, LMS_FIELD);
    players[count].playing = playing && playing[0] == '1';
    count++;
  }

  prv_finish_players(r, NULL, players, count);
}

static void prv_on_status(const char *err, const char *data, void *context) {
  Request *r = context;
  LMSStatusHandler handler = r->handler.status;
  void *user_context = r->user_context;
  r->used = false;

  if (err) {
    if (handler)
      handler(err, NULL, user_context);
    return;
  }

  char buffer[COMM_MAX_DATA + 1];
  prv_copy(buffer, sizeof(buffer), data);

  LMSStatus status;
  char *field = buffer;
  prv_copy(status.artist, LMS_TRACK_LEN, prv_take(&field, LMS_FIELD));
  prv_copy(status.title, LMS_TRACK_LEN, prv_take(&field, LMS_FIELD));
  const char *volume = prv_take(&field, LMS_FIELD);
  const char *playing = prv_take(&field, LMS_FIELD);
  status.volume = volume ? atoi(volume) : 0;
  status.playing = playing && playing[0] == '1';

  if (handler)
    handler(NULL, &status, user_context);
}

static void prv_on_page(const char *err, const char *data, void *context) {
  Request *r = context;
  LMSPageHandler handler = r->handler.page;
  void *user_context = r->user_context;
  r->used = false;

  if (err) {
    if (handler)
      handler(err, NULL, user_context);
    return;
  }

  char buffer[COMM_MAX_DATA + 1];
  prv_copy(buffer, sizeof(buffer), data);

  LMSPage page;
  page.count = 0;
  char *cursor = buffer;

  // The first record is the level's total, not an item. Without it a page cut
  // short to fit one AppMessage is indistinguishable from the end of the list.
  const char *total = prv_take(&cursor, LMS_RECORD);
  page.total = total ? atoi(total) : 0;

  while (cursor && *cursor && page.count < LMS_PAGE) {
    char *record = prv_take(&cursor, LMS_RECORD);
    if (!record || !*record)
      continue;

    LMSItem *item = &page.items[page.count];
    char *field = record;
    prv_copy(item->text, LMS_TEXT_LEN, prv_take(&field, LMS_FIELD));
    prv_copy(item->id, LMS_ID_LEN, prv_take(&field, LMS_FIELD));
    item->kind = prv_kind(prv_take(&field, LMS_FIELD));
    page.count++;
  }

  if (handler)
    handler(NULL, &page, user_context);
}

static void prv_on_cover(const char *err, const char *data, void *context) {
  Request *r = context;
  LMSCoverHandler handler = r->handler.cover;
  void *user_context = r->user_context;
  r->used = false;

  if (err) {
    if (handler)
      handler(err, NULL, user_context);
    return;
  }

  char buffer[COMM_MAX_DATA + 1];
  prv_copy(buffer, sizeof(buffer), data);

  LMSCover cover;
  char *field = buffer;
  const char *w = prv_take(&field, LMS_FIELD);
  const char *h = prv_take(&field, LMS_FIELD);
  const char *length = prv_take(&field, LMS_FIELD);
  const char *chunks = prv_take(&field, LMS_FIELD);

  cover.width = w ? atoi(w) : 0;
  cover.height = h ? atoi(h) : 0;
  cover.byte_length = length ? (uint32_t)atoi(length) : 0;
  cover.chunks = chunks ? atoi(chunks) : 0;

  if (!handler)
    return;
  if (cover.width <= 0 || cover.height <= 0 || cover.byte_length == 0)
    handler("no cover", NULL, user_context);
  else
    handler(NULL, &cover, user_context);
}

static void prv_on_done(const char *err, const char *data, void *context) {
  Request *r = context;
  LMSDoneHandler handler = r->handler.done;
  void *user_context = r->user_context;
  r->used = false;
  if (handler)
    handler(err, user_context);
}

void lms_players(LMSPlayersHandler handler, void *context) {
  Request *r = prv_take_slot();
  if (!r) {
    handler("busy", NULL, 0, context);
    return;
  }
  r->kind = KindPlayers;
  r->handler.players = handler;
  r->user_context = context;

  if (!comm_request("players", "", prv_on_players, r))
    prv_finish_players(r, "busy", NULL, 0);
}

void lms_status(const char *player_id, LMSStatusHandler handler,
                void *context) {
  Request *r = prv_take_slot();
  if (!r) {
    handler("busy", NULL, context);
    return;
  }
  r->kind = KindStatus;
  r->handler.status = handler;
  r->user_context = context;

  if (!comm_request("status", player_id, prv_on_status, r)) {
    r->used = false;
    handler("busy", NULL, context);
  }
}

void lms_command(const char *player_id, const char *command,
                 LMSDoneHandler handler, void *context) {
  Request *r = prv_take_slot();
  if (!r) {
    if (handler)
      handler("busy", context);
    return;
  }
  r->kind = KindDone;
  r->handler.done = handler;
  r->user_context = context;

  char arg[160];
  snprintf(arg, sizeof(arg), "%s%c%s", player_id, LMS_FIELD, command);

  if (!comm_request("cmd", arg, prv_on_done, r)) {
    r->used = false;
    if (handler)
      handler("busy", context);
  }
}

void lms_menu(const char *player_id, int start, bool node, const char *req_id,
              LMSPageHandler handler, void *context) {
  Request *r = prv_take_slot();
  if (!r) {
    handler("busy", NULL, context);
    return;
  }
  r->kind = KindPage;
  r->handler.page = handler;
  r->user_context = context;

  char arg[160];
  snprintf(arg, sizeof(arg), "%s%c%d%c%d%c%s%c%s", player_id, LMS_FIELD, start,
           LMS_FIELD, LMS_PAGE, LMS_FIELD, node ? "node" : "cmd", LMS_FIELD,
           req_id);

  if (!comm_request("menu", arg, prv_on_page, r)) {
    r->used = false;
    handler("busy", NULL, context);
  }
}

void lms_menu_go(const char *player_id, const char *item_id,
                 LMSDoneHandler handler, void *context) {
  Request *r = prv_take_slot();
  if (!r) {
    if (handler)
      handler("busy", context);
    return;
  }
  r->kind = KindDone;
  r->handler.done = handler;
  r->user_context = context;

  char arg[160];
  snprintf(arg, sizeof(arg), "%s%c%s", player_id, LMS_FIELD, item_id);

  if (!comm_request("menu_go", arg, prv_on_done, r)) {
    r->used = false;
    if (handler)
      handler("busy", context);
  }
}

void lms_cover(const char *player_id, int edge, LMSCoverHandler handler,
               void *context) {
  Request *r = prv_take_slot();
  if (!r) {
    handler("busy", NULL, context);
    return;
  }
  r->kind = KindCover;
  r->handler.cover = handler;
  r->user_context = context;

  char arg[160];
  snprintf(arg, sizeof(arg), "%s%c%d", player_id, LMS_FIELD, edge);

  if (!comm_request("cover", arg, prv_on_cover, r)) {
    r->used = false;
    handler("busy", NULL, context);
  }
}

void lms_cancel(void *context) {
  for (int i = 0; i < MAX_INFLIGHT; i++) {
    if (s_requests[i].used && s_requests[i].user_context == context) {
      comm_cancel(&s_requests[i]);
      s_requests[i].used = false;
    }
  }
}
