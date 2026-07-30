# Navigations- und Bedienungskonzept

Um eine nahtlose, intuitive und absturzfreie Bedienung auf der Pebble (sowohl Touch als auch Tasten) zu gewährleisten, strukturieren wir die App in drei klare Hierarchie-Ebenen. Da wir nun Touch-Gesten für die Player-Steuerung haben, können wir die Hardware-Tasten clever für die Menü-Navigation nutzen.

## 1. Ebene: Player-Auswahl (Startbildschirm)
Hier startet die App und listet alle im Netzwerk gefundenen LMS-Player auf.
*   **Darstellung:** Liste der Player (Namen).
*   **Bedienung (Touch & Tasten):** Hoch/Runter scrollen und einen Player per *Tap* oder *Select-Taste* auswählen.
*   **Back-Taste:** Schließt die App.

## 2. Ebene: Player Ansicht (Now Playing)
Das ist das Herzstück. Hier sehen wir (später) das Cover Art, Artist, Titel und den Status.
Da wir Touch-Gesten nutzen, lagern wir die reine Musiksteuerung auf das Display aus. Das macht die Tasten frei für die Navigation!

*   **Touch-Gesten (Musiksteuerung):**
    *   `Tap` (Tippen): Play / Pause
    *   `Wischen Links/Rechts`: Nächster / Vorheriger Titel
    *   `Wischen Oben/Unten`: Lautstärke anpassen
*   **Hardware-Tasten:**
    *   `Select (Mitte)`: **Öffnet das LMS Navigationsmenü!** (Der Ersatz für den alten "Long Press", da dieser im neuen Framework schwerer abbildbar ist und wir die Taste nun frei haben).
    *   `Up / Down`: Alternativ zur Touch-Geste für Lautstärke (+ / -).
*   **Back-Taste:** Geht zurück zur *Player-Auswahl* (Ebene 1).

## 3. Ebene: LMS Navigationsmenü (Browsing)
Hier durchsuchst du "Eigene Musik", "Radio", Ordner, Alben etc. für den gerade aktiven Player.
*   **Darstellung:** Eine paginierte Liste (Lazy Loading in 10er/20er Schritten), um den Arbeitsspeicher zu schonen und Abstürze bei großen Ordnern zu verhindern.
*   **Bedienung:**
    *   `Scrollen` (Wischen Oben/Unten oder Up/Down Tasten): Blättert durch die Liste. Am Ende der Liste wird automatisch der nächste Chunk geladen.
    *   `Auswählen` (Tap oder Select-Taste): Öffnet den Ordner oder spielt den Song ab.
*   **Back-Taste:** 
    *   Geht eine Ordner-Ebene nach oben. 
    *   Befindet man sich auf der obersten Menü-Ebene (Root), führt die Back-Taste zurück zur *Player Ansicht* (Ebene 2).


---

### Zusammenfassung des Workflows:
1. App Start -> **[Player Liste]**
2. Klick auf Player -> **[Player Ansicht]** (Cover, Musiksteuerung per Wischen/Tippen)
3. Klick auf mittlere Hardware-Taste -> **[LMS Menü]** (Ordner durchsuchen, speicherschonend)
4. Back-Taste -> springt jeweils eine Ebene zurück.
