/**
 * Memory diagnostics for the watch.
 *
 * Two independent routes, because neither is guaranteed to work:
 *
 * 1. `Instrumentation` -- the Alloy host preloads Moddable's instrumentation
 *    module (see build/devices/pebble/host/manifest.json), which exposes the XS
 *    engine's own counters. In the QEMU emulator these resolve by name but
 *    always read 0: the firmware compiles the counter table but never installs
 *    the getter callbacks. On a physical watch they may well work, so the
 *    overlay shows them.
 *
 * 2. `probe()` -- allocates 1 KB at a time and reports the running total to the
 *    phone after every step. When the machine dies, the last number in
 *    `pebble logs` is the chunk heap capacity in KB. This needs no firmware
 *    support at all and is the fallback when route 1 reads 0.
 *
 * Why this indirection: `console.log` from src/embeddedjs/ never reaches
 * `pebble logs` (only the pkjs side is logged), and an XS out-of-memory is a
 * hard abort that JavaScript cannot catch, so the capacity cannot be found by
 * trial inside a try/catch.
 */
import Instrumentation from "instrumentation";
import { call } from "relay";

/**
 * Shows the counter overlay at the bottom of the screen.
 *
 * Off by default, and not because it is expensive to look at: on a physical
 * watch the firmware already logs an "instruments:" line every second all by
 * itself, which carries the same numbers. The overlay only adds noise to them.
 * Building its line costs about 240 bytes of chunk heap per second -- measured,
 * that is exactly the idle growth it produced -- so leaving it on means
 * measuring the instrument rather than the app.
 */
export const enabled = false;

/**
 * Runs the allocation probe at startup instead of the normal UI.
 * Leave false for normal use -- it deliberately crashes the app.
 */
export const probeOnStart = false;

const SLOT = Instrumentation.map("XS Slot Heap Used");
const CHUNK = Instrumentation.map("XS Chunk Heap Used");
const FREE = Instrumentation.map("System Free Memory");
const GC = Instrumentation.map("XS Garbage Collection Count");

/**
 * One compact line: slot heap, chunk heap, free system memory, GC runs.
 * Bytes except g, which is a count. "?" means the counter does not exist,
 * "0" across the board means the firmware does not maintain them.
 */
export function line() {
	return "S" + get(SLOT) + " C" + get(CHUNK) +
		" F" + get(FREE) + " g" + get(GC);
}

function get(what) {
	if (undefined === what)
		return "?";
	const value = Instrumentation.get(what);
	return (undefined === value) ? "?" : value;
}

/**
 * How much the bounded probe tries to hold, in KB.
 * Only used when probeLimitKB > 0; set it to 0 for the stepping probe.
 */
export const probeLimitKB = 0;

const held = [];

/**
 * Measures the free chunk heap by holding 1 KB buffers that the collector
 * cannot reclaim.
 *
 * Two modes, because the emulator and a physical watch fail differently:
 *
 * - probeLimitKB > 0: allocate exactly that much, then stop and display it.
 *   Surviving proves the heap holds at least that much. Use this in the
 *   emulator, where an XS abort wedges QEMU so badly that neither
 *   `pebble screenshot` nor `pebble logs` can retrieve anything afterwards --
 *   raise the limit across runs to find the boundary.
 *
 * - probeLimitKB === 0: step upward, reporting every step to the phone, until
 *   the app dies. On a physical watch the phone survives the crash, so the last
 *   "watch: probe N" line in `pebble logs` is the answer in one run.
 *
 * @param {(text: string) => void} show  on-screen reporter
 */
export function probe(show) {
	if (probeLimitKB > 0) {
		for (let i = 0; i < probeLimitKB; i++)
			held.push(new ArrayBuffer(1024));
		show("held " + held.length + " KB");
		call("log", "probe survived " + held.length + " KB", () => {});
		return;
	}

	held.push(new ArrayBuffer(1024));
	show("probe " + held.length + " KB");
	// Chained through the phone rather than a timer: it paces the steps, and
	// the acknowledgement proves the number was logged before the next
	// allocation can kill the machine.
	call("log", "probe " + held.length, (err) => {
		if (err)
			return show("probe stopped at " + held.length + " KB");
		probe(show);
	});
}
