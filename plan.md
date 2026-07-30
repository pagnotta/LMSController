## Ziel (Goal Description)
Die Portierung der LMS Controller App auf das neue Piu/Alloy-Gerüst (Moddable) ist in einem frühen Stadium. Neben der technischen Portierung planen wir nun ein **komplettes Redesign** der über 10 Jahre alten App:
- **Moderne UI:** Neue Farben, Schriften (Fonts) und Integration von **Cover Art**.
- **Intuitive Bedienung:** Umsetzung einer nativen Touch-Steuerung für neuere Modelle sowie durchdachte Tasten-Belegung.
- **LMS API Research:** Die aktuelle Squeezebox / Logitech Media Server (LMS) API soll noch einmal recherchiert werden, um Cover Art und erweiterte Steuerungen optimal und performant in die Pebble App zu übertragen.

Zudem wurde gerade erfolgreich ein **Clay Konfigurations-Gerüst** implementiert, womit Server-IP und Zugangsdaten nun via Pebble-App konfigurierbar sind!

## Steuerungskonzept & UI-Design
**Farben & Schrift:**
- Wir verwenden gut lesbare, kontrastreiche Farbkombinationen, da ein reiner schwarzer Hintergrund keinen Akku spart (Memory LCD) - aber wir nutzen ihn ggf. dort, wo es optisch am besten aussieht (z.B. Cover Art Kontraste).
- Die Schriftart wird auf optimale Lesbarkeit angepasst (z.B. große Roboto-Schrift).

**Steuerung:**
- **Status/Player Ansicht:** 
  - *Touch:* Tap für Play/Pause, horizontales Wischen (Swipe) für Vor/Zurück, vertikales Wischen für Lautstärke.
  - *Knöpfe:* Ein dedizierter Button (z.B. Select oder Back) führt ins "Navigationsmenü", die anderen können redundant zur Touch-Steuerung belegt werden.
- **LMS Navigationsmenü:**
  - Eine separate Ansicht, um durch den LMS (Interpreten, Alben, Playlists) zu browsen.
  - *Bedienung:* Tasten (Up/Down/Select) sowie Touch (Wischen zum Scrollen, Tap zum Auswählen).

## LMS API Integration
Die Recherche der LMS API (JSON-RPC via `slim.request`) ergab folgende Möglichkeiten für die Umsetzung:
1. **Cover Art:** Bild-URLs können via `/music/current/cover.jpg` abgerufen werden. Das Handy (PebbleKit JS) muss diese laden, in das 64-Farben Pebble-Format konvertieren und blockweise an die Uhr schicken (asynchron mit tolerierbarer Ladezeit).
2. **Browsing & Pagination (Große Verzeichnisse):** Befehle wie `["artists", 0, 10]`, `["albums", 0, 10, "artist_id:123"]` erlauben durch die Start- und End-Parameter ein **Chunking (Paginierung)**. Große Listen (z. B. 200 Ordner) werden niemals komplett geladen, sondern immer nur in kleinen Blöcken (z.B. 10 Stück) zur Uhr geschickt. Beim Scrollen wird dynamisch nachgeladen. So wird der sehr begrenzte Arbeitsspeicher der Pebble nicht gesprengt!
3. **Playback:** Über `["playlistcontrol", "cmd:load", "album_id:123"]` kann Musik direkt vom Handgelenk aus gestartet werden.

## Proposed Changes

### `src/embeddedjs/main.js`
Erweiterung der Navigation und Integration von `lms.command(...)` in die Detail-Ansicht (`status` View).

#### [MODIFY] `src/embeddedjs/main.js`
Ich plane, die App so umzubauen, dass der "status" View nicht nur passiv Daten anzeigt, sondern auf Tastendrücke reagiert.
Wenn wir uns für direkte Tasten-Bindings im Status-View entscheiden, sähe der Code in etwa so aus:

```javascript
	onPressUp(column) {
		if (this.view === "list") {
			this.move(column, -1);
		} else if (this.view === "status") {
            // Lauter
            const player = this.players[this.selected];
            lms.command(player.id, "mixer volume +5").then(() => this.refreshStatus(column));
        }
		return true;
	}

	onPressDown(column) {
		if (this.view === "list") {
			this.move(column, +1);
		} else if (this.view === "status") {
            // Leiser
            const player = this.players[this.selected];
            lms.command(player.id, "mixer volume -5").then(() => this.refreshStatus(column));
        }
		return true;
	}

	onPressSelect(column) {
		if (this.view === "list" && this.players.length) {
			const player = this.players[this.selected];
			this.view = "status";
			this.refreshStatus(column);
		} else if (this.view === "status") {
            // Play / Pause Toggle
            const player = this.players[this.selected];
            lms.command(player.id, "pause").then(() => this.refreshStatus(column));
        }
		return true;
	}
```

Dazu kommt eine Methode `refreshStatus(column)` in `PlayerListBehavior`, die den Status abruft (`lms.status(id)`) und den Bildschirm mit Titel, Künstler und aktuellem Status aktualisiert.

### `src/pkjs/index.js`
Diese Datei hat bereits die passenden Endpunkte für `cmd`, `status` und `players`, ist also prinzipiell bereit. Keine direkten Änderungen erforderlich, solange die Befehle vom Standard abgedeckt werden ("pause", "mixer volume +5" etc.).

## Verification Plan
### Automated Tests
Pebble SDK hat keine umfangreichen integrierten Unit-Tests für Piu/JS. Ich werde den Build (für Emery und Gabbro) mit `pebble build` auf Fehler prüfen.

### Manual Verification
1. Die App wird im Pebble Emulator (Emery) installiert und ausgeführt: `pebble install --emulator emery`
2. Es wird geprüft, ob die App den LMS Server erreicht (falls vom Emulator aus 192.168.178.85 erreichbar ist).
3. Du kannst die generierte `build/LMSController.pbw` auf dein Gerät (Rebble) laden und prüfen, ob die Aktionen ausgeführt werden.
