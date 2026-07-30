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
			comp = comp.next;
		} else {
			container.add(row(text, isSelected));
		}
	}
	while (container.last && container.length > texts.length) {
		container.remove(container.last);
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
		if (this.view === "list") {
			this.move(column, -1);
		} else if (this.view === "status") {
			lms.command(this.currentPlayerId, "mixer volume +5").then(() => this.refreshStatus(column));
		} else if (this.view === "menu") {
			if (this.menuSelected > 0) {
				this.menuSelected--;
				this.marqueeTick = 0;
				this.lastMarqueeOffset = -1;
				this.paintMenu(column);
			}
		}
		return true;
	}

	onPressDown(column) {
		if (this.view === "list") {
			this.move(column, +1);
		} else if (this.view === "status") {
			lms.command(this.currentPlayerId, "mixer volume -5").then(() => this.refreshStatus(column));
		} else if (this.view === "menu") {
			if (this.menuSelected < this.menuItems.length - 1) {
				this.menuSelected++;
				this.marqueeTick = 0;
				this.lastMarqueeOffset = -1;
				this.paintMenu(column);
				// Lazy load next chunk if we approach the end
				if (this.menuSelected >= this.menuItems.length - 2) {
					if (!this.loadingMenu) {
						this.loadMenu(column, this.menuItems.length, this.currentMenuParams);
					}
				}
			}
		}
		return true;
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
			this.loadMenu(column, 0, "direct:1");
		} else if (this.view === "menu") {
			const item = this.menuItems[this.menuSelected];
			if (item) {
				if (item.isFolder) {
					this.menuHistory.push({ start: this.menuStart, selected: this.menuSelected, items: this.menuItems, params: this.currentMenuParams });
					this.menuStart = 0;
					this.menuSelected = 0;
					this.menuItems = [];
					this.marqueeTick = 0;
					this.lastMarqueeOffset = -1;
					fill(column, ["loading..."], -1);
					lms.menuGo(this.currentPlayerId, item.id).then(() => {
						this.loadMenu(column, 0, "item_id:" + item.id);
					});
				} else {
					// Execute Play action
					fill(column, ["playing..."], -1);
					lms.menuGo(this.currentPlayerId, item.id).then(() => {
						this.view = "status";
						this.refreshStatus(column);
					});
				}
			}
		}
		return true;
	}

	refreshStatus(column) {
		if (this.view !== "status") return;
		lms.status(this.currentPlayerId)
			.then((s) => {
				fill(column, [
					s.artist || "(no artist)",
					s.title || "(no title)",
					(s.playing ? "playing" : "paused") + "  Vol " + s.volume,
				], -1);
			})
			.catch((e) => fill(column, ["Error", String(e.message || e).slice(0, 24)], -1));
	}

	loadMenu(column, start, params) {
		this.currentMenuParams = params;
		this.loadingMenu = true;
		lms.menu(this.currentPlayerId, start, 10, params).then(items => {
			this.loadingMenu = false;
			if (start === 0) {
				this.menuItems = items;
			} else if (items.length > 0) {
				this.menuItems = this.menuItems.concat(items);
			}
			this.lastMarqueeOffset = -1;
			this.paintMenu(column);
		});
	}

	paintMenu(column) {
		if (this.view !== "menu") return;
		if (!this.menuItems.length) {
			fill(column, ["Empty"], -1);
			return;
		}
		const windowSize = Math.floor(metrics.pixels / metrics.rowHeight) || 6;
		let startIdx = Math.max(0, this.menuSelected - Math.floor(windowSize / 2));
		let endIdx = Math.min(this.menuItems.length, startIdx + windowSize);
		if (endIdx - startIdx < windowSize) {
			startIdx = Math.max(0, endIdx - windowSize);
		}
		
		const texts = [];
		for (let i = startIdx; i < endIdx; i++) {
			const item = this.menuItems[i];
			let text = (item.isFolder ? "> " : "") + item.text;
			// Only truncate here, animation happens in updateMarquee
			if (i === this.menuSelected && text.length > 14) {
				text = text.substring(0, 14);
			}
			texts.push(text);
		}
		fill(column, texts, this.menuSelected - startIdx);
	}

	updateMarquee(column) {
		if (this.view !== "menu") return;
		const item = this.menuItems[this.menuSelected];
		if (!item) return;
		
		let text = (item.isFolder ? "> " : "") + item.text;
		if (text.length <= 14) return;
		
		const over = text.length - 14;
		const tick = Math.floor(this.marqueeTick || 0);
		const phase = tick % (over * 2 + 8);
		let offset = 0;
		if (phase > 4 && phase <= 4 + over) {
			offset = phase - 4;
		} else if (phase > 4 + over && phase <= 4 + over * 2) {
			offset = (4 + over * 2) - phase;
		}
		
		if (this.lastMarqueeOffset === offset) return;
		this.lastMarqueeOffset = offset;
		
		text = text.substring(offset);
		
		const windowSize = Math.floor(metrics.pixels / metrics.rowHeight) || 6;
		let startIdx = Math.max(0, this.menuSelected - Math.floor(windowSize / 2));
		let endIdx = Math.min(this.menuItems.length, startIdx + windowSize);
		if (endIdx - startIdx < windowSize) {
			startIdx = Math.max(0, endIdx - windowSize);
		}
		
		const selectedIndexInColumn = this.menuSelected - startIdx;
		let comp = column.first;
		for (let i = 0; i < selectedIndexInColumn; i++) {
			if (comp) comp = comp.next;
		}
		if (comp) {
			comp.string = text;
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
			lms.command(this.currentPlayerId, cmd).then(() => {
				this.statusRefreshTime = Date.now() + 500;
			});
		}
	}

	onTimeChanged(column) {
		if (this.statusRefreshTime && Date.now() >= this.statusRefreshTime) {
			this.statusRefreshTime = 0;
			this.refreshStatus(column);
		}
		
		if (this.view === "menu" && this.menuItems.length) {
			this.marqueeTick = (this.marqueeTick || 0) + 1;
			this.updateMarquee(column);
		}
	}

	onPressBack(column) {
		if (this.view === "menu") {
			if (this.menuHistory.length > 0) {
				const state = this.menuHistory.pop();
				this.menuStart = state.start;
				this.menuSelected = state.selected;
				this.menuItems = state.items;
				this.currentMenuParams = state.params;
				this.marqueeTick = 0;
				this.lastMarqueeOffset = -1;
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
	touchCount: 1,
	pixels: screen.width * 4,
});
