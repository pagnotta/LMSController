/**
 * LMS interface for the watch.
 *
 * Deliberately narrow: the entire network path lives in relay.js and
 * src/pkjs/index.js. If on-device fetch() ever becomes usable (see
 * docs/networking.md), only the plumbing underneath changes -- the UI keeps
 * calling players(), status() and command().
 *
 * All responses arrive as compact, field-separated strings. The LMS JSON is
 * reduced on the phone and never transferred to the watch.
 *
 * Every function takes a callback of the shape (err, result), where err is a
 * string or null. See relay.js for why this is not promise-based.
 */
import { call, RECORD, FIELD } from "relay";

/**
 * List of players.
 * @param {(err: string|null, list?: Array<{name: string, id: string, playing: boolean}>) => void} onDone
 */
export function players(onDone) {
	call("players", "", (err, data) => {
		if (err)
			return onDone(err);
		if (!data)
			return onDone(null, []);
		const list = [];
		for (const record of data.split(RECORD)) {
			const [name, id, playing] = record.split(FIELD);
			list.push({ name, id, playing: playing === "1" });
		}
		onDone(null, list);
	});
}

/**
 * Current state of one player.
 * @param {string} id  player MAC
 * @param {(err: string|null, status?: {artist: string, title: string, volume: number, playing: boolean}) => void} onDone
 */
export function status(id, onDone) {
	call("status", id, (err, data) => {
		if (err)
			return onDone(err);
		const [artist, title, volume, playing] = data.split(FIELD);
		onDone(null, {
			artist: artist ?? "",
			title: title ?? "",
			volume: parseInt(volume ?? "0") || 0,
			playing: playing === "1",
		});
	});
}

/**
 * Sends a control command to a player.
 * @param {string} id   player MAC
 * @param {string} cmd  LMS command, words separated by spaces,
 *                      e.g. "pause", "playlist jump +1", "mixer volume +5"
 * @param {(err: string|null) => void} onDone
 */
export function command(id, cmd, onDone) {
	call("cmd", id + FIELD + cmd, onDone);
}

/**
 * Fetches a page of menu items.
 *
 * The reply starts with a header record carrying the total number of items the
 * server holds for this level, so the caller can tell "this page is short
 * because the list ends here" from "this page is short because it did not fit
 * into one AppMessage". Guessing that from the page size was wrong whenever a
 * page was cut for size, and the list then appeared to end early.
 *
 * @param {string} id player MAC
 * @param {number} start index
 * @param {number} limit max items
 * @param {string} reqType "node" or "cmd"
 * @param {string} reqId e.g. "home" or an item id
 * @param {(err: string|null, page?: {total: number, items: Array<{text: string, id: string, kind: string, isFolder: boolean}>}) => void} onDone
 */
export function menu(id, start, limit, reqType, reqId, onDone) {
	call("menu", [id, start, limit, reqType, reqId].join(FIELD), (err, data) => {
		if (err)
			return onDone(err);
		if (!data)
			return onDone(null, { total: 0, items: [] });

		const records = data.split(RECORD);
		const total = parseInt(records[0]) || 0;
		const items = [];
		for (let r = 1; r < records.length; r++) {
			const [text, itemId, kind] = records[r].split(FIELD);
			if (undefined === kind)
				continue; // truncated record, see MAX_DATA in src/pkjs/index.js
			items.push({
				text,
				id: itemId,
				kind,
				// "node" and "cmd" open a new list; "play" and "0" do not.
				isFolder: kind === "node" || kind === "cmd",
			});
		}
		onDone(null, { total, items });
	});
}

/**
 * Runs a menu item's action -- for a playable item, that starts playback.
 * @param {string} playerId
 * @param {string} itemId
 * @param {(err: string|null) => void} onDone
 */
export function menuGo(playerId, itemId, onDone) {
	call("menu_go", playerId + FIELD + itemId, onDone);
}
