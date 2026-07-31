/**
 * Appearance for emery (200x228, rectangular) and gabbro (260x260, round).
 *
 * On the font format: Piu builds the Pebble font name from family plus weight,
 * so you write "24px Gothic" or "bold 24px Gothic" -- NOT "24px Gothic-Regular".
 * The latter becomes "Gothic-Regular-Regular" internally, matches no font, and
 * kills the app with an xsURIError.
 *
 * Available families and sizes (from xs/platforms/pebble/xsHost.c):
 *   Gothic 9/14/18/24/28/36, Bitham, Roboto, DroidSerif, Leco
 */
import {} from "piu/MC";

// gabbro is square and round; emery is taller than it is wide.
export const round = screen.width === screen.height;

export const skins = {
	background: new Skin({ fill: "black" }),
	highlight: new Skin({ fill: "blue" }),
	bar: new Skin({ fill: "#222222" }),
};

export const styles = {
	item: new Style({
		font: "28px Gothic",
		color: "white",
		horizontal: round ? "center" : "left",
		vertical: "middle",
	}),
	itemSelected: new Style({
		font: "bold 28px Gothic",
		color: "white",
		horizontal: round ? "center" : "left",
		vertical: "middle",
	}),
	title: new Style({
		font: "bold 28px Gothic",
		color: "white",
		horizontal: "center",
		vertical: "middle",
	}),
	hint: new Style({
		font: "28px Gothic",
		color: "gray",
		horizontal: "center",
		vertical: "middle",
	}),
	// Memory overlay, see diag.js. Smallest font available, so the four
	// counters fit on one line.
	diag: new Style({
		font: "14px Gothic",
		color: "yellow",
		horizontal: "center",
		vertical: "middle",
	}),
};

export const metrics = {
	rowHeight: round ? 48 : 42,
	// On a round display rows have to move inward so they are not clipped by
	// the curvature.
	inset: round ? 30 : 4,
	titleHeight: 28,
	diagHeight: 18,
};
