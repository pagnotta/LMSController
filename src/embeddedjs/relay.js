/**
 * Relay transport: watch <-> phone over AppMessage.
 *
 * Why not on-device fetch()? See docs/networking.md -- the required
 * @moddable/pebbleproxy 0.1.8 has a header parsing bug that breaks every
 * request carrying headers. The relay bypasses that bridge entirely: HTTP
 * happens in src/pkjs/, and only finished, compact responses arrive here.
 * That also keeps memory pressure away from the mod.
 *
 * Protocol (keys are declared under messageKeys in package.json):
 *   watch -> phone: RQ_ID (number), RQ_OP (string), RQ_ARG (string)
 *   phone -> watch: RS_ID (number), RS_ERR (string, empty = ok), RS_DATA (string)
 */
import Message from "pebble/message";

// Separators for the compact response encoding: record and field.
// Deliberately not JSON on the watch -- that saves a parser and memory.
export const RECORD = "\u001e";
export const FIELD = "\u001f";

const KEYS = ["RQ_ID", "RQ_OP", "RQ_ARG", "RS_ID", "RS_ERR", "RS_DATA"];

let message;
let nextId = 1;
const pending = new Map(); // RQ_ID -> { resolve, reject }
const outbox = [];

const stats = { writableEvents: 0, writes: 0, reads: 0, lastError: "" };

/** Transport state, for on-device diagnosis. */
export function debug() {
	return "w" + stats.writableEvents + " s" + stats.writes +
		" r" + stats.reads + " q" + outbox.length +
		(stats.lastError ? " " + stats.lastError : "");
}

let retryTimer = 0;

/**
 * Writes the next queued message.
 *
 * Important: on Pebble, onWritable does not fire on its own until something has
 * been written -- waiting for it waits forever (observed: w0 s0 r0 q1). So we
 * write directly and retry on failure.
 */
function pump() {
	if (!outbox.length)
		return;

	try {
		message.write(outbox[0]);
		outbox.shift();
		stats.writes++;
		stats.lastError = "";
	} catch (e) {
		stats.lastError = "W:" + String(e.message || e).slice(0, 12);
		if (!retryTimer) {
			retryTimer = setTimeout(() => {
				retryTimer = 0;
				pump();
			}, 250);
		}
	}
}

export function init() {
	if (message)
		return;

	message = new Message({
		keys: KEYS,
		// Without input/output, Message opens the channel with maximum buffers:
		// 8200 bytes in plus 8200 out is 16 KB taken from the XS mod, which
		// caused "Alloy fatal error, memory full". This protocol only needs
		// RS_DATA (512 max) plus overhead.
		input: 1024,
		output: 512,
		onReadable() {
			stats.reads++;
			const msg = this.read();
			const id = msg.get("RS_ID");
			const waiter = pending.get(id);
			if (!waiter)
				return; // unknown id, or the phone's handshake -- ignore

			pending.delete(id);
			const err = msg.get("RS_ERR");
			if (err)
				waiter.reject(new Error(String(err)));
			else
				waiter.resolve(String(msg.get("RS_DATA") ?? ""));
		},
		onWritable() {
			stats.writableEvents++;
			pump();
		},
		onSuspend() {},
	});
}

/**
 * Sends a request to the phone and resolves with the response string.
 * @param {string} op   operation, see src/pkjs/index.js
 * @param {string} arg  argument, fields separated by FIELD
 * @returns {Promise<string>}
 */
export function call(op, arg = "") {
	return new Promise((resolve, reject) => {
		const id = nextId++;
		
		// Add a timeout to prevent memory leaks if the phone never replies
		const timer = setTimeout(() => {
			if (pending.has(id)) {
				pending.delete(id);
				reject(new Error("Timeout waiting for phone"));
			}
		}, 10000);
		
		pending.set(id, { 
			resolve: (res) => { clearTimeout(timer); resolve(res); },
			reject: (err) => { clearTimeout(timer); reject(err); }
		});

		const m = new Map();
		m.set("RQ_ID", id);
		m.set("RQ_OP", op);
		m.set("RQ_ARG", String(arg));
		outbox.push(m);
		pump();
	});
}

// The AppMessage channel is opened when this module loads, not on the first
// call(). Constructed inside a Piu display callback, onWritable never fired and
// the app exited -- the official hellomessage example also creates Message at
// module scope.
init();
