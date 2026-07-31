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
import * as diag from "diag";

// Items per menu request. One page has to survive the trip in a single
// AppMessage (MAX_DATA in src/pkjs/index.js), so this is a transport limit
// rather than a display one -- the visible window is usually smaller.
const PAGE = 10;

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

/** Replaces a container's contents with lines of text, reusing components. */
function fill(container, texts, selectedIndex) {
	let comp = container.first;
	for (let i = 0; i < texts.length; i++) {
		let isSelected = i === selectedIndex;
		let text = texts[i];
		if (comp) {
			comp.string = text;
			comp.skin = isSelected ? skins.highlight : skins.background;
			comp.style = isSelected ? styles.itemSelected : styles.item;
			comp.visible = true;
			comp = comp.next;
		} else {
			container.add(row(text, isSelected));
		}
	}
	while (comp) {
			comp.visible = false;
			comp = comp.next;
		}
}

class PlayerListBehavior extends Behavior {
	onCreate(column) {
		this.players = [];
		this.selected = 0;
		this.view = "list";
	}

	onDisplaying(column) {
		column.focus();
		column.interval = 250;
		column.start();
		if (diag.probeOnStart) {
			diag.probe((text) => fill(column, [text], -1));
			return;
		}
		fill(column, ["loading players..."], -1);
		this.loadPlayers(column);
	}

	loadPlayers(column) {
		lms.players((err, list) => {
			if (err) {
				this.players = [];
				this.view = "error";
				fill(column, ["No LMS", String(err).slice(0, 24)], -1);
				return;
			}
			this.players = list;
			this.selected = 0;
			this.view = "list";
			this.paint(column);
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
		if (this.view === "status") {
			this.loadPlayers(column);
		} else if (this.view === "menu") {
			this.moveMenu(column, -1);
		}
	}

	onPressDown(column) {
		this.move(column, 1);
		if (this.view === "menu") {
			this.moveMenu(column, +1);
		}
	}

	/**
	 * Moves the menu cursor, fetching the neighbouring page when it steps off
	 * the loaded one.
	 *
	 * The end of the list comes from the server's total, not from a short page.
	 * Deducing it from "fewer than PAGE items came back" was wrong whenever a
	 * page had been cut down to fit one AppMessage: the list then looked
	 * finished while items were still left, and scrolling stopped dead.
	 */
	moveMenu(column, delta) {
		if (this.loadingMenu || !this.menuItems.length)
			return;
		const next = this.menuSelected + delta;
		if (next < 0 || next >= this.menuTotal)
			return;

		this.menuSelected = next;
		if (next >= this.menuStart && next < this.menuStart + this.menuItems.length) {
			this.paintMenu(column);
			return;
		}
		this.paintMenu(column);
		this.loadMenu(column,
			delta < 0 ? Math.max(0, this.menuStart - PAGE) : this.menuStart + this.menuItems.length,
			this.currentReqType, this.currentReqId);
	}

	onPressSelect(column) {
		if (this.view === "list" && this.players.length) {
			const player = this.players[this.selected];
			this.currentPlayerId = player.id;
			this.view = "status";
			fill(column, [player.name, "loading..."], -1);
			this.refreshStatus(column);
		} else if (this.view === "status") {
			// Select Button in status view opens the LMS Menu
			this.menuHistory = [];
			this.menuStart = 0;
			this.menuItems = [];
			this.menuSelected = 0;
			this.view = "menu";
			this.loadMenu(column, 0, "node", "home");
		} else if (this.view === "menu") {
			const item = this.menuItems[this.menuSelected - this.menuStart];
			if (item && !this.loadingMenu) {
				if (item.isFolder) {
					this.menuHistory.push({
						start: this.menuStart,
						selected: this.menuSelected,
						items: this.menuItems,
						total: this.menuTotal,
						reqType: this.currentReqType,
						reqId: this.currentReqId,
					});
					this.menuStart = 0;
					this.menuSelected = 0;
					this.menuItems = [];
					fill(column, ["loading..."], -1);
					this.loadMenu(column, 0, item.kind, item.id);
				} else {
					fill(column, ["playing..."], -1);
					lms.menuGo(this.currentPlayerId, item.id, (err) => {
						if (err)
							return this.showError(column, err);
						this.view = "status";
						this.refreshStatus(column);
					});
				}
			}
		}
		return true;
	}

	/** Shows a failed request without letting it take the app down. */
	showError(column, err) {
		fill(column, ["Error", String(err).slice(0, 24)], -1);
	}

	refreshStatus(column) {
		if (this.view !== "status") return;
		lms.status(this.currentPlayerId, (err, s) => {
			if (err)
				return this.showError(column, err);
			fill(column, [
				s.artist || "(no artist)",
				s.title || "(no title)",
				(s.playing ? "playing" : "paused") + "  Vol " + s.volume,
			], -1);
		});
	}

	loadMenu(column, start, reqType, reqId) {
		this.currentReqType = reqType;
		this.currentReqId = reqId;
		this.loadingMenu = true;
		lms.menu(this.currentPlayerId, start, PAGE, reqType, reqId, (err, page) => {
			this.loadingMenu = false;
			if (err) {
				fill(column, ["Menu Error", String(err).slice(0, 24)], -1);
				return;
			}
			this.menuStart = start;
			this.menuItems = page.items;
			this.menuTotal = page.total;
			// Keep the cursor inside the page that just arrived.
			if (this.menuSelected < start)
				this.menuSelected = start;
			const lastIndex = start + page.items.length - 1;
			if (this.menuSelected > lastIndex)
				this.menuSelected = Math.max(start, lastIndex);
			this.paintMenu(column);
		});
	}

	paintMenu(column) {
		if (this.view !== "menu") return;
		if (!this.menuItems.length) {
			fill(column, ["Empty"], -1);
			return;
		}
		// How many rows actually fit. This used to read metrics.pixels, which
		// theme.js never defined, so the expression was NaN and always fell back
		// to 6 -- one row more than emery's 228 px can show at rowHeight 42. The
		// sixth entry sat below the edge of the screen and the window never
		// scrolled to reveal it. Measuring the laid-out container instead is
		// right for both emery and gabbro, and accounts for the diag bar.
		const windowSize = Math.max(1, Math.floor(column.height / metrics.rowHeight));

		// The window has to stay inside the loaded chunk: indices outside it have
		// no item and would paint as gaps.
		const first = this.menuStart;
		const last = this.menuStart + this.menuItems.length;
		let startIdx = this.menuSelected - (windowSize >> 1);
		if (startIdx > last - windowSize)
			startIdx = last - windowSize;
		if (startIdx < first)
			startIdx = first;
		const endIdx = Math.min(last, startIdx + windowSize);

		let comp = column.first;
		for (let i = startIdx; i < endIdx; i++) {
			let isSelected = i === this.menuSelected;
			let item = this.menuItems[i - this.menuStart];
			if (!item) continue;
			let text = (item.isFolder ? "> " : "") + item.text;
			if (text.length > 14) text = text.substring(0, 14);
			
			if (comp) {
				comp.string = text;
				comp.skin = isSelected ? skins.highlight : skins.background;
				comp.style = isSelected ? styles.itemSelected : styles.item;
				comp.visible = true;
				comp = comp.next;
			} else {
				column.add(row(text, isSelected));
			}
		}
		while (comp) {
			comp.visible = false;
			comp = comp.next;
		}
	}

	onTouchBegan(column, id, x, y, ticks) {
		this.startX = x;
		this.startY = y;
	}

	onTouchEnded(column, id, x, y, ticks) {
		if (this.view !== "status") return;
		const dx = x - this.startX;
		const dy = y - this.startY;
		
		let cmd = null;
		if (Math.abs(dx) > Math.abs(dy) && Math.abs(dx) > 30) {
			cmd = (dx > 0) ? "button jump_rew" : "button jump_fwd";
		} else if (Math.abs(dy) > Math.abs(dx) && Math.abs(dy) > 30) {
			cmd = (dy > 0) ? "mixer volume -5" : "mixer volume +5";
		} else if (Math.abs(dx) < 10 && Math.abs(dy) < 10) {
			cmd = "pause"; // Tap
		}

		if (cmd) {
			lms.command(this.currentPlayerId, cmd, (err) => {
				if (err)
					return this.showError(column, err);
				this.statusRefreshTime = Date.now() + 500;
			});
		}
	}

	onTimeChanged(column) {
		if (this.statusRefreshTime && Date.now() >= this.statusRefreshTime) {
			this.statusRefreshTime = 0;
			this.refreshStatus(column);
		}
		if (diag.enabled) {
			// Once a second is enough; the overlay itself allocates a string.
			this.diagTick = (this.diagTick || 0) + 1;
			if (this.diagTick >= 4) {
				this.diagTick = 0;
				column.next.string = diag.line();
			}
		}
	}

	onPressBack(column) {
		if (this.view === "menu") {
			if (this.menuHistory.length > 0) {
				const state = this.menuHistory.pop();
				this.menuStart = state.start;
				this.menuSelected = state.selected;
				this.menuItems = state.items;
				this.menuTotal = state.total;
				this.currentReqType = state.reqType;
				this.currentReqId = state.reqId;
				this.paintMenu(column);
			} else {
				this.view = "status";
				this.refreshStatus(column);
			}
			return true;
		} else if (this.view === "status") {
			this.view = "list";
			this.paint(column);
			return true; 
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
			bottom: diag.enabled ? metrics.diagHeight : 0,
			left: 0,
			right: 0,
			active: true,
			Behavior: PlayerListBehavior,
			contents: [],
		}),
		// The behavior reaches this via `column.next`. Kept outside the Column
		// so fill() and paintMenu(), which walk every child, leave it alone.
		Label($, {
			left: 0,
			right: 0,
			bottom: 0,
			height: diag.enabled ? metrics.diagHeight : 0,
			skin: skins.bar,
			style: styles.diag,
			string: "",
		}),
	],
}));

export default new LMSApplication(null, {
	touchCount: 1,
	pixels: screen.width * 4,
});
