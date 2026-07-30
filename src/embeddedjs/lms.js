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
 */
import { call, RECORD, FIELD } from "relay";

/**
 * List of players.
 * @returns {Promise<Array<{name: string, id: string, playing: boolean}>>}
 */
export function players() {
	return call("players").then((data) => {
		if (!data)
			return [];
		return data.split(RECORD).map((record) => {
			const [name, id, playing] = record.split(FIELD);
			return { name, id, playing: playing === "1" };
		});
	});
}

/**
 * Current state of one player.
 * @param {string} id  player MAC
 * @returns {Promise<{artist: string, title: string, volume: number, playing: boolean}>}
 */
export function status(id) {
	return call("status", id).then((data) => {
		const [artist, title, volume, playing] = data.split(FIELD);
		return {
			artist: artist ?? "",
			title: title ?? "",
			volume: parseInt(volume ?? "0") || 0,
			playing: playing === "1",
		};
	});
}

/**
 * Sends a control command to a player.
 * @param {string} id   player MAC
 * @param {string} cmd  LMS command, words separated by spaces,
 *                      e.g. "pause", "playlist jump +1", "mixer volume +5"
 * @returns {Promise<string>}
 */
export function command(id, cmd) {
	return call("cmd", id + FIELD + cmd);
}
/**
 * Fetch a chunk of menu items.
 * @param {string} id player MAC
 * @param {number} start index
 * @param {number} limit max items
 * @param {string} params optional string e.g. "direct:1"
 * @returns {Promise<Array<{text: string, id: string, isFolder: boolean}>>}
 */
export function menu(id, start, limit, params = "") {
	return call("menu", [id, start, limit, params].join(FIELD)).then((data) => {
		if (!data) return [];
		return data.split(RECORD).map((record) => {
			const [text, itemId, isFolder] = record.split(FIELD);
			return { text, id: itemId, isFolder: isFolder === "1" };
		});
	});
}

/**
 * Execute a menu action (drill down or play).
 * @param {string} playerId
 * @param {string} itemId
 * @returns {Promise<string>}
 */
export function menuGo(playerId, itemId) {
	return call("menu_go", playerId + FIELD + itemId);
}
