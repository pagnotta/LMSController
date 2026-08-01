#include "comm.h"

// Requests waiting for a reply. Four is generous: the UI issues one at a time
// and at most a status refresh overlaps a menu fetch.
#define MAX_PENDING 4

// Requests not yet handed to AppMessage, because a send is still in flight or
// the session is not up yet.
#define MAX_QUEUED 4

#define MAX_OP 16
#define MAX_ARG 200

// How long a request may go unanswered. The phone gives XMLHttpRequest 8 s, so
// this has to sit above that or a slow LMS looks like a dropped message.
#define REQUEST_TIMEOUT_MS 10000

// Retry delay after a send fails. Mostly this is the session not being up yet,
// which resolves as soon as the phone's first message lands.
#define RETRY_MS 300

typedef struct {
  int32_t id;
  CommHandler handler;
  void *context;
  AppTimer *timeout;
  bool used;
} Pending;

typedef struct {
  int32_t id;
  char op[MAX_OP];
  char arg[MAX_ARG];
  bool used;
} Queued;

static Pending s_pending[MAX_PENDING];
static Queued s_queue[MAX_QUEUED];
static int32_t s_next_id = 1;
static bool s_sending;
static AppTimer *s_retry;

static void prv_pump(void);

static Pending *prv_find_pending(int32_t id) {
  for (int i = 0; i < MAX_PENDING; i++)
    if (s_pending[i].used && s_pending[i].id == id)
      return &s_pending[i];
  return NULL;
}

static void prv_release(Pending *p) {
  if (p->timeout) {
    app_timer_cancel(p->timeout);
    p->timeout = NULL;
  }
  p->used = false;
  p->handler = NULL;
  p->context = NULL;
}

/**
 * Finishes a request and hands the result to its owner.
 *
 * The slot is released *before* the handler runs: handlers routinely start the
 * next request, and a still-occupied slot would shrink the table under them.
 */
static void prv_complete(Pending *p, const char *err, const char *data) {
  CommHandler handler = p->handler;
  void *context = p->context;
  prv_release(p);
  if (handler)
    handler(err, data ? data : "", context);
}

static void prv_on_timeout(void *ctx) {
  Pending *p = ctx;
  if (!p->used)
    return;
  p->timeout = NULL;  // it just fired; cancelling it would be wrong
  prv_complete(p, "timeout", NULL);
}

static void prv_drop_queued(int32_t id) {
  for (int i = 0; i < MAX_QUEUED; i++)
    if (s_queue[i].used && s_queue[i].id == id)
      s_queue[i].used = false;
}

static void prv_retry(void *ctx) {
  s_retry = NULL;
  prv_pump();
}

/** Hands the oldest queued request to AppMessage, one at a time. */
static void prv_pump(void) {
  if (s_sending)
    return;

  Queued *q = NULL;
  for (int i = 0; i < MAX_QUEUED; i++) {
    if (s_queue[i].used) {
      q = &s_queue[i];
      break;
    }
  }
  if (!q)
    return;

  DictionaryIterator *out;
  AppMessageResult result = app_message_outbox_begin(&out);
  if (result != APP_MSG_OK) {
    if (!s_retry)
      s_retry = app_timer_register(RETRY_MS, prv_retry, NULL);
    return;
  }

  dict_write_int32(out, MESSAGE_KEY_RQ_ID, q->id);
  dict_write_cstring(out, MESSAGE_KEY_RQ_OP, q->op);
  dict_write_cstring(out, MESSAGE_KEY_RQ_ARG, q->arg);

  result = app_message_outbox_send();
  if (result != APP_MSG_OK) {
    if (!s_retry)
      s_retry = app_timer_register(RETRY_MS, prv_retry, NULL);
    return;
  }

  q->used = false;
  s_sending = true;
}

static void prv_inbox_received(DictionaryIterator *iter, void *ctx) {
  Tuple *id_tuple = dict_find(iter, MESSAGE_KEY_RS_ID);
  if (!id_tuple)
    return;

  // The phone opens the session with RS_ID 0. Nothing waits on it.
  Pending *p = prv_find_pending(id_tuple->value->int32);
  if (!p)
    return;

  Tuple *err_tuple = dict_find(iter, MESSAGE_KEY_RS_ERR);
  Tuple *data_tuple = dict_find(iter, MESSAGE_KEY_RS_DATA);

  // `length` counts the terminator, so anything above 1 is a real message.
  // Reading cstring[0] instead trips -Wzero-length-bounds on the SDK's
  // flexible array member.
  const char *err = (err_tuple && err_tuple->length > 1)
      ? err_tuple->value->cstring : NULL;
  const char *data = data_tuple ? data_tuple->value->cstring : "";

  prv_complete(p, err, data);
}

static void prv_inbox_dropped(AppMessageResult reason, void *ctx) {
  APP_LOG(APP_LOG_LEVEL_WARNING, "inbox dropped: %d", (int)reason);
}

static void prv_outbox_sent(DictionaryIterator *iter, void *ctx) {
  s_sending = false;
  prv_pump();
}

/**
 * A send that never left the watch. The reply will never come, so the request
 * is failed straight away rather than left to time out ten seconds later.
 */
static void prv_outbox_failed(DictionaryIterator *iter, AppMessageResult reason,
                              void *ctx) {
  s_sending = false;

  Tuple *id_tuple = iter ? dict_find(iter, MESSAGE_KEY_RQ_ID) : NULL;
  if (id_tuple) {
    Pending *p = prv_find_pending(id_tuple->value->int32);
    if (p)
      prv_complete(p, "send failed", NULL);
  }
  prv_pump();
}

void comm_init(void) {
  app_message_register_inbox_received(prv_inbox_received);
  app_message_register_inbox_dropped(prv_inbox_dropped);
  app_message_register_outbox_sent(prv_outbox_sent);
  app_message_register_outbox_failed(prv_outbox_failed);

  // The inbox has to hold RS_DATA (512 B) plus the other keys and dictionary
  // overhead; the outbox only ever carries an id and two short strings.
  app_message_open(1024, 256);
}

void comm_deinit(void) {
  if (s_retry) {
    app_timer_cancel(s_retry);
    s_retry = NULL;
  }
  for (int i = 0; i < MAX_PENDING; i++)
    if (s_pending[i].used)
      prv_release(&s_pending[i]);
  app_message_deregister_callbacks();
}

bool comm_request(const char *op, const char *arg, CommHandler handler,
                  void *context) {
  Pending *p = NULL;
  for (int i = 0; i < MAX_PENDING; i++) {
    if (!s_pending[i].used) {
      p = &s_pending[i];
      break;
    }
  }
  Queued *q = NULL;
  for (int i = 0; i < MAX_QUEUED; i++) {
    if (!s_queue[i].used) {
      q = &s_queue[i];
      break;
    }
  }
  if (!p || !q)
    return false;

  const int32_t id = s_next_id++;

  p->id = id;
  p->handler = handler;
  p->context = context;
  p->used = true;
  p->timeout = app_timer_register(REQUEST_TIMEOUT_MS, prv_on_timeout, p);

  q->id = id;
  strncpy(q->op, op, sizeof(q->op) - 1);
  q->op[sizeof(q->op) - 1] = '\0';
  strncpy(q->arg, arg ? arg : "", sizeof(q->arg) - 1);
  q->arg[sizeof(q->arg) - 1] = '\0';
  q->used = true;

  prv_pump();
  return true;
}

void comm_cancel(void *context) {
  for (int i = 0; i < MAX_PENDING; i++) {
    if (s_pending[i].used && s_pending[i].context == context) {
      prv_drop_queued(s_pending[i].id);
      prv_release(&s_pending[i]);
    }
  }
}
