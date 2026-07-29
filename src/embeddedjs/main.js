/**
 * LMS Controller -- Alloy/Piu version for emery (Pebble Time 2) and
 * gabbro (Pebble Round 2).
 *
 * Piu quirks that this file deliberately works around:
 *  - `import {} from "piu/MC"` installs Application, Skin, Label, ... as
 *    globals. A named import (`import { Application } from ...`) yields
 *    undefined and the app exits with no error message.
 *  - The default export is the Application INSTANCE, not a factory.
 *  - Buttons are dispatched starting from the application focus. Without
 *    `.focus()` on the active container, onPressUp/Down/Select/Back never fire.
 */
import {} from "piu/MC";
import { skins, styles, metrics } from "theme";
import * as lms from "lms";

function row(text, selected) {
	return new Label(null, {
		left: metrics.inset,
		right: metrics.inset,
		height: metrics.rowHeight,
		string: text,
		skin: selected ? skins.highlight : skins.background,
		style: selected ? styles.itemSelected : styles.item,
	});
}

/** Replaces a container's contents with lines of text. */
function fill(container, texts, selectedIndex) {
	container.empty();
	for (let i = 0; i < texts.length; i++)
		container.add(row(texts[i], i === selectedIndex));
}

class PlayerListBehavior extends Behavior {
	onCreate(column) {
		this.players = [];
		this.selected = 0;
		this.view = "list";
	}

	onDisplaying(column) {
		column.focus();
		fill(column, ["loading players..."], -1);
		this.loadPlayers(column);
	}

	loadPlayers(column) {
		lms.players()
			.then((list) => {
				this.players = list;
				this.selected = 0;
				this.view = "list";
				this.paint(column);
			})
			.catch((e) => {
				this.players = [];
				this.view = "error";
				fill(column, ["No LMS", String(e.message || e).slice(0, 24)], -1);
			});
	}

	paint(column) {
		if (!this.players.length) {
			fill(column, ["No players"], -1);
			return;
		}
		const texts = this.players.map((p) => (p.playing ? "> " : "") + p.name);
		fill(column, texts, this.selected);
	}

	move(column, delta) {
		if (this.view !== "list" || !this.players.length)
			return;
		const n = this.players.length;
		this.selected = (this.selected + delta + n) % n;
		this.paint(column);
	}

	onPressUp(column) {
		this.move(column, -1);
		return true;
	}

	onPressDown(column) {
		this.move(column, +1);
		return true;
	}

	onPressSelect(column) {
		if (this.view === "list" && this.players.length) {
			const player = this.players[this.selected];
			this.view = "status";
			fill(column, [player.name, "loading..."], -1);
			lms.status(player.id)
				.then((s) => {
					fill(column, [
						s.artist || "(no artist)",
						s.title || "(no title)",
						(s.playing ? "playing" : "paused") + "  Vol " + s.volume,
					], -1);
				})
				.catch((e) => fill(column, ["Error", String(e.message || e).slice(0, 24)], -1));
		}
		return true;
	}

	onPressBack(column) {
		if (this.view !== "list") {
			this.view = "list";
			this.paint(column);
			return true; // swallow Back so the app does not close
		}
		return false; // in the list, Back closes the app as usual
	}
}

const LMSApplication = Application.template(($) => ({
	skin: skins.background,
	style: styles.item,
	contents: [
		Column($, {
			top: 0,
			bottom: 0,
			left: 0,
			right: 0,
			active: true,
			Behavior: PlayerListBehavior,
			contents: [],
		}),
	],
}));

export default new LMSApplication(null, {
	touchCount: 0,
	pixels: screen.width * 4,
});
