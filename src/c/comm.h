#pragma once

#include <pebble.h>

/**
 * Watch end of the relay to the phone.
 *
 * All LMS traffic goes through src/pkjs/index.js, which does the HTTP and hands
 * back compact, field-separated strings. The watch never sees JSON. See
 * docs/networking.md for why the phone does the talking.
 *
 * Wire format, unchanged from the Alloy version:
 *   request   RQ_ID (int32), RQ_OP (string), RQ_ARG (string)
 *   response  RS_ID (int32), RS_ERR (string), RS_DATA (string)
 *
 * The phone must send the first message -- until the AppMessage session exists,
 * the outbox on this side is not writable. It opens with RS_ID 0, which has no
 * waiter here and is dropped.
 */

/** Separators inside RQ_ARG and RS_DATA. Must match src/pkjs/index.js. */
#define LMS_FIELD  '\x1f'
#define LMS_RECORD '\x1e'

/** Longest RS_DATA the phone will send (MAX_DATA in src/pkjs/index.js). */
#define COMM_MAX_DATA 512

/**
 * Called once per request, on the app's own event loop.
 * `err` is NULL on success and a short message otherwise; `data` is valid only
 * for the duration of the call.
 */
typedef void (*CommHandler)(const char *err, const char *data, void *context);

void comm_init(void);
void comm_deinit(void);

/**
 * Queues a request. `op` and `arg` are copied.
 * Returns false if the queue is full, in which case the handler never runs.
 */
bool comm_request(const char *op, const char *arg, CommHandler handler, void *context);

/**
 * Forgets every request made with this context, without waiting for a reply.
 *
 * A window must call this as it unloads: a reply that arrives afterwards would
 * otherwise run a handler against freed state.
 */
void comm_cancel(void *context);
