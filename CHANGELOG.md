# Changelog — Mesen2 SNES HD Fork (Emulator C++ Code)

Dieses Changelog dokumentiert Änderungen am Mesen2-Fork (C++ Emulator-Code).
Für Änderungen am DKC2-HD-Tools Viewer siehe `DKC2-HD-Tools/CHANGELOG.md`.
Für die Architektur der Compositing Engine siehe `ARCHITECTURE.md`.

---

## ═══════════════════════════════════════════════════════════════
## HD Compositing Engine (v2)
## ═══════════════════════════════════════════════════════════════
##
## Branch: feature/hd-compositing-engine
## Vorgänger: v0.1-sonderfall-m5.19 (Tag) — M5.1 bis M5.19
##
## Architekturwechsel: Statt pro-Level-Typ Sonderfälle (bg3FogBlend,
## bg1OverlayBlend, colorMathDelta etc.) wird die PPU-Compositing-
## Pipeline (Color Math, Windowing, Brightness) generisch auf HD-
## Auflösung repliziert. Ein Code-Pfad für alle Level-Typen.
## ═══════════════════════════════════════════════════════════════

---

## [2026-07-13] — Phase 3.10: Targeted BG3 Overlay Swap (P3.10)

### BG3+Mode1Bg3Priority Overlay Swap + Lockjaw Fix

**Datei:** `Core/SNES/HdPacks/SnesHdVideoFilter.cpp`

**Problem P3.9:** `useHdSubPixel` (HD-Bottom als CM-Operand) verursachte zwei
Regressionen:
- Lockjaw: BG1 Terrain halb-transparent statt solid (CM ADD mit voller 8-bit
  HD-Background-Farbe → zu starke Farbveränderung)
- Mainbrace (ohne Nebel): leichte blaue Tönung auf BG1/BG2 Tiles in Bereichen
  wo kein Nebel ist (useHdSubPixel feuerte für alle sole-on-main Pixel)

**Lösung P3.10:** `useHdSubPixel` komplett entfernt. Stattdessen:

1. **Gezielter BG3-Overlay-Swap:** Nur wenn ALLE drei PPU-Bedingungen erfüllt:
   - `winLayer == 2` (BG3 ist der Compositing-Winner)
   - `Mode1Bg3Priority` (Spiel hat BG3 priorisiert → Overlay-Pattern)
   - `(MainScreenLayers & 0x0F) == 0x04` (BG3 ist sole BG auf Main)

   → Swap: HD Content (BG1/BG2) wird Primary, Nebel-Tint aus nativem PPU-Output.

2. **Alle anderen CM-Fälle:** Winner HD wird solid gerendert mit nativem
   `SubScreenColor` als CM-Operand → leichte Tönung, kein Halbe-Transparenz.

**Ergebnis — drei unabhängige Pfade ohne Heuristic:**

| Level | Winner | Swap? | Rendering |
|-------|--------|-------|-----------|
| **Mainbrace (Nebel)** | BG3+Mode1Prio+sole | **JA** | HD Content + Nebel-Tint |
| **Mainbrace (kein Nebel)** | BG1/BG2 | Nein | HD Winner + native CM |
| **Lockjaw (unter Wasser)** | BG1 sole | Nein | HD Terrain + native Wasser-Tint |
| **Rambi** | BG1 (entfernt) | Nein | Step 3 Overlay-Fallback |
| **Normal** | BG1/BG2 | Nein | HD Winner, kein CM |

---

## [2026-07-13] — Phase 3.9: HD Sub-Screen CM Operand (P3.9) — REVERTED in P3.10

### Full-HD Color Math: CM(HD_main, HD_sub) statt CM(HD_main, native_sub)

**Datei:** `Core/SNES/HdPacks/SnesHdVideoFilter.cpp`

**Problem P3.8:** Der Overlay-Swap-Ansatz (Winner verwerfen → Bottom als Primary)
war nicht generisch genug. Er funktionierte für Mainbrace (Nebel=Overlay) aber
verursachte Lockjaw-Regression (BG1=Terrain fälschlicherweise verworfen, da BG1
ebenfalls sole-on-main + CM + AddSubscreen).

**Lösung P3.9:** P3.8-Swap komplett entfernt. Stattdessen neuer Ansatz:
Wenn BEIDE Layer (Winner + Bottom) HD-Tiles haben UND CM + AddSubscreen aktiv,
wird der HD-Bottom-Pixel (raw, pre-brightness) als CM-Operand verwendet statt
des nativen SubScreenColor. Ergebnis: `CM(HD_main, HD_sub)` — beide Seiten HD.

**Rendering-Flow:**
1. Bottom HD Tile → als Hintergrund gerendert (mit Brightness)
2. Top HD Tile (Winner) → CM angewendet mit HD-Bottom als Operand → über Hintergrund
3. Brightness auf CM-Ergebnis angewendet

**Per-Subpixel Override:** `pxCmR/pxCmG/pxCmB` ersetzt `cmR/cmG/cmB` wenn
`useHdSubPixel=true`. Wenn Bottom-Pixel transparent → Fallback auf native SubScreenColor.

**Generische Wirkung — kein Heuristic, kein Sonderfall:**
- Mainbrace: `CM(HD_fog, HD_content)` → nebliger HD-Content ✓
- Lockjaw:   `CM(HD_terrain, HD_background)` → Wasser-getöntes HD-Terrain ✓
- Rambi:     `CM(HD_honey, HD_terrain)` → Honig-Glow auf HD-Terrain ✓
- Normal:    Kein CM → Winner HD normal gerendert ✓
- Overlay-Fallback (Step 3): Unverändert — greift wenn Winner KEIN HD-Tile hat

---

## [2026-07-13] — Phase 3.8: CM+AddSubscreen Overlay Swap (P3.8) — REVERTED in P3.9

### Winner found + Color Math + AddSubscreen → overlay mode

**Datei:** `Core/SNES/HdPacks/SnesHdVideoFilter.cpp`

**Problem P3.7:** Wenn der Winner-Layer (z.B. BG3 Nebel in Mainbrace) ein
HD-Tile hat UND Color Math mit AddSubscreen aktiv ist, wurde der Nebel
opak gerendert und hat den darunter liegenden HD-Content (BG1/BG2)
verdeckt. Der Overlay-Fallback (Step 3) griff nur wenn KEIN HD-Tile
für den Winner gefunden wurde.

**Lösung P3.8:** Nach der Bottom-Layer-Suche in Step 2 wird geprüft:
wenn `cmActive && AddSubscreen && hdTileBot` gefunden, wird in den
Overlay-Modus gewechselt — das Sub-Screen HD-Tile wird zum Primary,
der Nebel-Tint wird aus dem nativen PPU-Output extrahiert. So wird
HD-Content DURCH den Nebel-Effekt hindurch angezeigt.

**Betroffene Level:** Mainbrace (BG3 Nebel über BG1/BG2 Content),
potenziell alle Level mit Overlay + Color Math + AddSubscreen.

---

## [2026-07-13] — Phase 3.5: Enhanced HDMA Diagnostics (P3.5)

### Per-Scanline HDMA Dump + Expanded Split Detection

**Datei:** `Core/SNES/HdPacks/SnesHdVideoFilter.cpp`

**Analyse-Ergebnis P3.4:** Lockjaw hdBG1=0 ist KEIN Regression — BG1-Tiles
bei VRAM $2000-$2340 sind DMA-animiert und wurden in keinem Build (auch P3.3)
jemals gefunden. Die MISS-Hashes sind zwischen P3.3 und P3.4 identisch.

**Änderungen P3.5:**

1. **HDMA Split Detection erweitert:** Erkennt jetzt auch Änderungen in
   SubScreenLayers, ColorMathEnabled, FixedColor und ScreenBrightness
   (nicht mehr nur MainScreenLayers). → Gangplank Galleon wird jetzt als
   HDMA-aktiv erkannt falls HDMA FixedColor/CM per Scanline ändert.

2. **Per-Scanline HDMA Dump:** Bei HDMA-aktiven Kontexten wird einmalig
   pro Kontext ein kompakter Dump ausgegeben der Scanline-Bereiche mit
   gleichen Registerwerten zusammenfasst:
   ```
   HDMA SCANLINE DUMP (first HDMA frame):
     Y   1- 95: Main=$13 Sub=$14 CM=$23 AddSub=1 Fixed=$0000 Br=15
     Y  96-224: Main=$01 Sub=$14 CM=$23 AddSub=1 Fixed=$0000 Br=15
   ```
   Damit können wir Gangplank/Lockjaw HDMA-Effekte verstehen.

---

## [2026-07-13] — Phase 3.4: Overlay Tint Extraction (P3.4)

### Winner-Only bleibt Top-Layer, Multi-Layer nur für Bottom

**Datei:** `Core/SNES/HdPacks/SnesHdVideoFilter.cpp`

P3.0 war rein Winner-Only. P3.1 erweitert das sicher um Bottom-Layer-
Compositing für HD Tiles mit Transparenz:

**Ansatz (P2.0-Bug-sicher):**
1. PPU-Winner ist IMMER der Top-Layer (keine Priority-Walk-Ersetzung!)
2. HD Lookup für Winner (+BG1↔BG2 Retry) — wie bisher
3. Kein HD-Match für Winner → natives Pixel (Winner ist opak, fertig)
4. HD-Match gefunden → Top-Layer rendern
5. Wenn Top-Layer transparente Pixel hat: Nächsten Layer darunter
   (Mode 1 Prioritätsreihenfolge) als Bottom-Layer suchen
6. Bottom-Layer wird hinter transparenten Top-Pixeln compositet

**Sicherheitsgarantien:**
- Winner ohne HD Tile → natives Pixel (kein Lower-Priority-Tile-Durchscheinen)
- Bottom-Layer-Suche filtert nach MainScreenLayers (Sub-Screen ausgeschlossen)
- Sprites → natives Pixel (spriteWon unverändert)

**Änderungen gegenüber P3.0:**
- Prioritätsreihenfolge wird NUR für Bottom-Layer-Suche berechnet (nicht für Top)
- Neuer Diagnosezähler `multi=` zeigt Pixel mit Bottom-Layer-Match
- MainScreenLayers-Check verhindert Sub-Screen-Layer als Bottom
- Build-Version: `P3.1`

**Erwartetes Verhalten:**
- Pirate Panic: BG2 gewinnt (Winner) → HD Tile. BG3 als Bottom falls transparent.
- Mainbrace: BG3 gewinnt (Nebel) → HD Tile + Color Math (SUBTRACT FixedColor)
- NPC-Text: Sprite gewinnt → natives Pixel → kein Bug
- BG3 vor BG1: Unmöglich, da Winner=BG1 (PPU korrekt)

---

## [2026-07-10] — Phase 3: HD Color Math (P3.0)

### CM-Skip entfernt, echte HD Color Math implementiert

**Datei:** `Core/SNES/HdPacks/SnesHdVideoFilter.cpp`

Phase 2.1 übersprang HD-Tile-Rendering komplett wenn `AllowColorMath` aktiv war
(CM-Skip). Das war zu aggressiv — in HDMA-Leveln (Mainbrace, Hot Head Hop, Rambi
Rumble) war AllowColorMath auf ALLEN Pixeln gesetzt → 100% native Fallback.

**Phase 3 ersetzt CM-Skip durch echte HD Color Math:**

1. HD-Tile-Lookup erfolgt IMMER, unabhängig von AllowColorMath
2. Wenn HD-Tile gefunden UND AllowColorMath aktiv → Color Math auf HD-Pixel anwenden
3. Color Math Quelle: `FixedColor` (aus ScanlineInfo) oder `SubScreenColor` (per-pixel),
   abhängig von `ColorMathAddSubscreen`
4. Operationen: ADD oder SUBTRACT, mit optionalem HALVE
5. Brightness (0-15) wird NACH Color Math angewendet (wie die PPU)
6. Gilt für opaque und semi-transparent Alpha-Pfade

**Build-Version:** `P3.0`

**Diagnostik:** `frameCmSkip` Counter umbenannt zu `frameHdCm` (zählt Pixel mit
angewandter HD Color Math statt übersprungener)

---

## [2026-07-06] — Phase 1: Per-Scanline PPU Register Snapshot

### Neue Datenstruktur: SnesHdScanlineInfo

**Datei:** `Core/SNES/HdPacks/SnesHdData.h`

Neue Struktur `SnesHdScanlineInfo` — Snapshot aller PPU-Register die für
HD-Compositing relevant sind, erfasst pro Scanline:

- **Color Math:** `ColorMathEnabled`, `SubtractMode`, `HalveResult`,
  `AddSubscreen`, `ClipMode`, `PreventMode` ($2130, $2131)
- **Fixed Color:** `FixedColor` ($2132) — HDMA-animiert pro Scanline
  (Nebelgradient, Lavatönung, Eiseffekt)
- **Brightness:** `ScreenBrightness` ($2100) — Fade-In/Out
- **Layer-Zuordnung:** `MainScreenLayers`, `SubScreenLayers` ($212C, $212D)
- **Window-Positionen:** `Window1Left/Right`, `Window2Left/Right` ($2126-$2129)
- **Color-Window-Konfiguration:** `ColorWindowActive[2]`, `ColorWindowInverted[2]`,
  `ColorWindowMaskLogic` — für Color-Window-Auswertung auf HD-Auflösung

### SnesHdScreenInfo erweitert

`ScanlineInfo[239]` Array hinzugefügt. Wird in `SendFrame()` beim
Double-Buffer-Swap via `memset` gecleared.

### PPU befüllt ScanlineInfo

**Datei:** `Core/SNES/SnesPpu.cpp`

In `RenderScanline()`, direkt nach `hdScanline`-Berechnung und VOR
`ApplyColorMath()`, wird `_state` in `ScanlineInfo[hdScanline]` kopiert.
So hat der HD-Filter den Register-Zustand *vor* der nativen Color Math
und kann sie selbst auf HD-Pixeln neu berechnen.

### Include-Abhängigkeit

`SnesHdData.h` inkludiert jetzt `SNES/SnesPpuTypes.h` für die Enum-Typen
`ColorWindowMode` und `WindowMaskLogic`. Keine zirkuläre Abhängigkeit
(SnesPpuTypes.h ist ein reines Type-Definition-Header).

### Kein Verhaltensunterschied

Phase 1 fügt nur Daten hinzu — die werden noch nicht konsumiert.
Der bestehende HD-Filter arbeitet exakt wie M5.19 weiter.

---

## ═══════════════════════════════════════════════════════════════
## v1 — Per-Level-Typ Sonderfälle (M5.1 bis M5.19)
## Gesichert als Tag: v0.1-sonderfall-m5.19
## ═══════════════════════════════════════════════════════════════

---

## [2026-07-06f] — M5.19: Beehive Regression Fix — frameBg3FogSkip statt BgLayerMask (Issue O)

### Fix: bg1OverlayWinner Kriterium korrigiert

**Problem:** M5.17 fügte `BgLayerMask & 0x04` als Bedingung für `bg1OverlayWinner` hinzu,
um Beehive-Overlay (BG1 über BG3) von Mainbrace-Terrain (BG1 ohne BG3) zu unterscheiden.
Aber BG3 deckt in Beehive-Leveln nur ~56% der Pixel ab (31636/56056). Die restlichen 44%
(24420 Pixel) wurden fälschlich als nicht-overlay klassifiziert → Fallback auf native
Rendering → Regression ("wilder Mix aus HD und nativem Muster").

**Root Cause:** Per-Pixel BG3-Masken-Check ist zu granular. Nicht alle Beehive-Pixel haben
BG3 (transparente Bereiche), aber ALLE brauchen Overlay-Blend-Rendering.

**Lösung:** Frame-Level-Entscheidung statt Per-Pixel-Mask:
```cpp
// M5.17-M5.18 (broken): Per-Pixel BG3 check
bool bg1OverlayWinner = (!hdTile && winLayer == 0 && (pixelInfo.MainScreenFlags & 0x80)
                         && (pixelInfo.BgLayerMask & 0x04));
// M5.19 (fixed): Frame-Level Fog-Erkennung
bool bg1OverlayWinner = (!hdTile && winLayer == 0 && (pixelInfo.MainScreenFlags & 0x80)
                         && frameBg3FogSkip == 0);
```

- **Beehive:** `frameBg3FogSkip == 0` (kein Fog-Gate feuert jemals) → Overlay auf ALLEN Pixeln ✓
- **Mainbrace:** `frameBg3FogSkip > 0` (tausende Fog-Pixel ab erster Scanline) → kein Overlay ✓

### Diagnostik-Erweiterung: Overlay-Blend Aufschlüsselung

Neue Counter im WINNERS-Log: `ovBg2=X ovBg3=Y ovMiss=Z`
- `ovBg2`: Overlay-Pixel die BG2-Terrain via Layer-Retry fanden (korrekt)
- `ovBg3`: Overlay-Pixel die BG3-Hintergrund als Fallback fanden (kein BG2 vorhanden)
- `ovMiss`: Overlay-eligible Pixel die weder BG2 noch BG3 fanden

---

## [2026-07-06e] — M5.18: Fog Contour Fix — ADD Color Math für BG3 Fog (Issue L)

### Fix: Native ADD statt gewichtetem Blend für BG3-Fog

**Problem:** Die M5.17 Fog-Blend-Formel `(hdR * 4 + fogR) / 5` (80% HD + 20% Fog)
erzeugte einen uniformen dunklen Schleier statt sichtbarer Nebelkonturen/Wisps.
- Schwarzer Fog (0x0000): `hdR * 0.80` → 20% Verdunklung (FALSCH — nativ: keine Änderung)
- Heller Fog (0x2108): `hdR * 0.85` → nur 5% Variation (FALSCH — nativ: deutlicher Kontrast)
- Ergebnis: kaum sichtbarer Fog-Gradient trotz HDMA-Animation der Fog-Dichte

**Root Cause:** Mainbrace Mayhem nutzt ADD Color Math (MSFlags=0x81, Bit 5=0).
Die native PPU addiert die BG3-Fog-Farbe (HDMA-animiert, 0x0000–0x2108 Graustufen)
auf das Terrain. Die 80/20-Gewichtung ignoriert den ADD-Modus komplett.

**Lösung:** Separater Blend-Pfad für `bg3FogBlend` vs `bg1OverlayBlend`:
- **bg3FogBlend (Mainbrace Fog):** `result = min(255, hdPixel + fogColor)` — nativer ADD.
  HDMA variiert fogColor pro Scanline → Contour/Wisps werden sichtbar.
  Half-Add (MSFlags Bit 6) wird ebenfalls unterstützt: `fog/2` statt vollem `fog`.
- **bg1OverlayBlend (Beehive Honey):** Unverändert `(hdR * 4 + fogR) / 5` — keine Regression.

Beide Rendering-Pfade betroffen: alpha=255 (opak) UND alpha>0 (semi-transparent).

---

## [2026-07-06d] — M5.17: BG3 Fog-Winner Gate + BG1 Overlay Fix (Issue L — Mainbrace Mayhem)

### Fix 1: BG3 Fog-Winner Gate (Step 1)

**Problem:** In Mainbrace Mayhem liegen BG3-Fog-HD-Tiles im Pack (`bg/bg3/`). Step 1 fand
diese Tiles und renderte sie opak — der Nebel war sichtbar, aber das Terrain darunter
komplett verdeckt. Der bestehende `bg3FogBlend`-Pfad (Step 3) wurde nie erreicht, weil
`!hdTile` bereits false war.

**Root Cause:** Step 1 macht keinen Unterschied zwischen BG3-Fog-Winnern (semi-transparent,
AllowColorMath aktiv) und normalen BG3-Winnern. BG3-Fog-HD-Tiles wurden wie normale
opake Tiles behandelt.

**Lösung:** `bg3FogWinner`-Gate in Step 1 (~Zeile 315): Wenn `winLayer == 2` UND
`AllowColorMath` gesetzt ist, wird der HD-Tile-Lookup für BG3 übersprungen. Step 3
(`bg3FogBlend`) findet stattdessen BG1/BG2-Terrain-HD-Tiles und rendert sie mit
Nebel-Tint (80% HD + 20% Fog-Farbe).

**Neuer Diagnostik-Counter:** `fogSkip=%u` im FRAME-Log — zählt übersprungene
BG3-Fog-Winner-Lookups pro Frame (erwarteter Wert: ~49800 in Mainbrace).

### Fix 2: BG1 Overlay-Winner Einschränkung

**Problem:** Die `bg1OverlayWinner`-Erkennung (M5.16) war zu breit. In Mainbrace Mayhem
feuerte sie ~3914 Mal pro Frame — BG1 gewann in Nebellücken (wo BG3 keinen Tile hat),
wurde aber als "Overlay" fehlinterpretiert. Das Ergebnis: BG2-Hintergrund mit
BG1-Terrain-Tint (falsche Darstellung).

**Root Cause:** Die Bedingung `!hdTile && winLayer == 0 && AllowColorMath` unterschied
nicht zwischen echtem Overlay (BG1 über BG3, Beehive) und Terrain in Nebellücken
(BG1 wo BG3 fehlt, Mainbrace).

**Lösung:** Zusätzliche Bedingung `(pixelInfo.BgLayerMask & 0x04)` (~Zeile 359):
BG1 ist nur Overlay, wenn BG3 am selben Pixel AUCH einen Tile hat.
- **Beehive:** BG1 gewinnt ÜBER BG3 (beide vorhanden) → `0x04` gesetzt → Overlay ✓
- **Mainbrace:** BG1 gewinnt WO BG3 FEHLT (Nebellücke) → `0x04` = 0 → Terrain ✓

**Erwartete Log-Werte für Mainbrace nach Fix:**
- `fogSkip` ≈ 49800 (alle BG3-Fog-Pixel übersprungen)
- `fogB` ≈ 49800 (Step 3 findet Terrain unter Fog)
- `ovBlend` = 0 (kein BG1-Overlay in diesem Level)

---

## [2026-07-06c] — M5.16: BG1 Overlay-Blend (Issue O — Rambi Rumble Beehive HD)

### Neue Feature: BG1 Overlay-Blend Rendering-Pfad

**Problem:** In Beehive-Leveln (ppuConfig $03) liegt die semi-transparente Honig-Overlay
auf BG1 (layer=0). BG1 gewinnt das PPU-Compositing auf 100% der Pixel (wn0=55140,
acm0=55140). Die Honig-HD-Tiles wurden gerendert und deckten den BG2-Terrain komplett ab.
BG2 Terrain-HD-Tiles wurden nie nachgeschlagen → kein HD-Terrain sichtbar.

**Root Cause:** Exakt das gleiche Problem wie BG3-Fog in Mainbrace Mayhem, aber auf BG1
statt BG3. Der bestehende `bg3FogBlend`-Pfad triggert nur für `winLayer == 2`. Kein
äquivalenter Pfad für `winLayer == 0` (BG1 overlay).

**Lösung: `bg1OverlayBlend`-Pfad in `SnesHdVideoFilter.cpp`:**

1. **Erkennung** (~Zeile 333): `bg1OverlayWinner` = Pixel wo BG1 gewinnt, kein HD-Tile
   für BG1 vorhanden, und AllowColorMath aktiv → Honig-Overlay erkannt
2. **Step 2 Skip** (~Zeile 347): Overlay-Winner überspringen Palette-Vergleichs-Logik
3. **Step 3b** (~Zeile 423-462): Neuer BG1 Overlay-Blend Pfad — sucht HD-Tile auf
   BG2 (Terrain) und BG3 (Hintergrund), inkl. Layer-Retry
4. **Fog-Color Berechnung** (~Zeile 707): `bg3FogBlend || bg1OverlayBlend` →
   MainScreenColor als Overlay-Tint (80% HD + 20% Honig-Farbe)
5. **Color Math Delta Guard** (~Zeile 727): Overlay-Blend Pixel überspringen
   cmDelta (falsche Berechnung für diesen Fall)
6. **Rendering** (~Zeile 759, 808): Fog-Blend auf opaque und alpha-blended Pixel
   wird für Overlay-Blend identisch angewendet
7. **Diagnostik**: `ovBlend=%u` im FRAME-Log, `(OV-BLEND)` Tag bei MATCH-Einträgen,
   `frameBg1OverlayBlend` Counter

**Betroffene Level (alle ppuConfig $03, $2131=$21):**
Rambi Rumble (0x02), Hornet Hole (0x11), Rambi Scene (0x12),
Parrot Chute Panic (0x13), Shortcut (0x26), King Zing Sting (0x60),
plus 7 Bonus-Räume — insgesamt 13 Level-Varianten.

**Dateien:**
- `Core/SNES/HdPacks/SnesHdVideoFilter.cpp` — BG1 overlay-blend Erkennung,
  Rendering-Pfad, Diagnostik (Build M5.15 → M5.16)

---

## [2026-07-06b] — M5.15: Per-Layer Diagnostik + Issue O/P Journal-Update

### Diagnostik-Erweiterung (M5.15)

**Neue Features in `SnesHdVideoFilter.cpp`:**
- `frameWin[4]` / `frameWinACM[4]` — Per-Layer Winner- und AllowColorMath-Pixelzähler
- `BG3WIN sample` Logging — Erste 3 BG3-gewinnende Pixel pro Context (MSFlags, MSColor, ppuOut, Tile-Info)
- `WINNERS` Summary-Zeile pro Frame: `wn0/wn1/wn2/wn3 | acm0/acm1/acm2/acm3`
- Build-Version: M5.14 → M5.15

### Journal-Update

**Issue O (Rambi Rumble):** Post-Log-Analyse (36.931 Zeilen) dokumentiert —
Layer-Fix von vorheriger Session war FALSCH (reverted im Viewer), Palette pal=6 korrekt,
Content-Hash-Mismatch bleibt als offenes Problem.

**Issue P (Gusty Glade):** Pre-Log-Analyse zeigt cmDelta=0 in JEDEM Gameplay-Frame —
unser Subtract-Mode-Fix (9d97024c) wird nie ausgelöst, blaue Quadrate haben eine
völlig andere Ursache. M5.15 WINNERS-Daten sollen die echte Quelle identifizieren.

---

## [2026-07-06] — Issue P Fix: Blaue Quadrate (Color Math Subtract) + Brightness + Delta-Skalierung

### Fix 1: Multiplikative Color Math für Subtract-Modus (Issue P — Blaue Quadrate)

**Problem:** Gusty Glade HD-Tiles zeigten blaue Quadrate. Color Math SUBTRACT
subtrahiert eine FixedColor (hoher R/G, niedriger B) — das additive cmDelta-System
clampte R/G auf 0 während B unverändert blieb → asymmetrisches Clamping → blaues
Erscheinungsbild.

**Lösung:** Neuer `IsSubtractMode`-Flag (Bit 0x20) im `PixelFlags`-Enum, gesetzt in
`SnesPpu.cpp` überall wo `AllowColorMath` gesetzt wird. `SnesHdVideoFilter.cpp` nutzt
multiplikative Skalierung (`hdR * cmPostR / cmPreR`) für Subtract-Modus — bewahrt
Farbverhältnisse statt absoluter Offsets. Additiver Pfad beibehalten für Add-Modus
(Nebel, Glow-Effekte).

**Dateien:**
- `Core/SNES/SnesPpuTypes.h` — `IsSubtractMode = 0x20` zu `PixelFlags` Enum
- `Core/SNES/SnesPpu.cpp` — Flag an 4 Stellen gesetzt (RenderBgColor, RenderSprites,
  BG-Layer-Template, Mode 7)
- `Core/SNES/HdPacks/SnesHdVideoFilter.cpp` — Neuer `if(cmSubtractMode)` Branch
  für opaque und alpha-blend Pfade

### Fix 2: Brightness-Kontamination bei MainScreenColor

**Problem:** `MainScreenColor` wurde VOR `ApplyBrightness()` erfasst, während
`ppuOutputBuffer` (Post-Math) NACH Brightness erfasst wurde. Die cmDelta-Ratio
enthielt dadurch den Brightness-Faktor → HD-Tiles wurden bei Fades über-verdunkelt.

**Lösung:** Brightness inline auf `MainScreenColor` anwenden mit
`channel * ScreenBrightness / 15` (gleiche Formel wie `ApplyBrightness()`).
Bei ScreenBrightness=15 (normales Gameplay) ist dies ein No-Op.

**Datei:** `Core/SNES/SnesPpu.cpp` (MainScreenColor-Capture-Block)

### Fix 3: 5-bit→8-bit Delta-Skalierung

**Problem:** Konvertierung nutzte `val*8` statt `val*8 + val/4` (≈ `val*255/31`).
3% Unter-Anwendung des Color-Math-Effekts.

**Lösung:** Akkurate Konvertierung `val*8 + val/4` für beide Seiten (pre und post).

**Datei:** `Core/SNES/HdPacks/SnesHdVideoFilter.cpp` (cmDelta-Berechnung)

### Verweis
- Issue P dokumentiert in `DEBUG_JOURNAL.md` (ab Zeile 2150)
- Sub-Issues P(b) Layer-Mismatch und P(c) Blätter-Tiles weiterhin OFFEN

---

## [2026-07-05] — M5.14: Layer Retry Bubble-Regression Fix

- Fallback-Loop in `SnesHdVideoFilter.cpp` entfernt die fälschlicherweise Layer-Retry
  auf ALLE Tiles anwendete (nicht nur BG1↔BG2). Verursachte Bubble-Artefakte in
  Hot Head Hop (Issue N Regression).

## [2026-07-03] — M5.13: Layer-Agnostic Retry BG1↔BG2

- Layer-Retry-Mechanismus für Issue H: Wenn BG1-Tile-Lookup fehlschlägt, wird mit
  BG2-Layer-Index nachgeschlagen (und umgekehrt). Behebt Layer-Index-Mismatch
  zwischen Viewer-Export und Mesen-Runtime.
- BG3-Fog-Blend-Pfad korrekt von Retry ausgenommen.

## [2026-06-28] — M5.12: Issue H BG3 Background Fallback

- `frameHasBg1ColorMath` Flag für korrektes BG3-Rendering bei Levels mit Color Math.

## [2026-06-26] — M5.11: Color Math Delta für HDMA Lava-Glow

- Initiale cmDelta-Implementierung: Differenz zwischen Pre- und Post-Color-Math
  MainScreenColor wird als additiver Offset auf HD-Tiles angewendet.
- BG3-Fog-Blend (80/20 Mischung) für Sub-Screen-Nebel-Compositing.

## [2026-06-17] — M5.4: Enhanced MISS Diagnostik

- Erweiterte Diagnose-Logs: MATCH, PAL MISMATCH, MISS, FRAME, CONTEXT CHANGE.
- VRAM-Signatur-basierte Level-Erkennung mit automatischem Counter-Reset.
- MISS-Limit von 30 auf 60 erhöht.

## [2026-06-12] — M5.3: Content Hash Matching

- FNV-1a 64-bit Content Hash System (`hashes.bin` Loader).
- Fingerprint-System (`fingerprints.bin`) für Gfxset-Identifikation.
- BG3 2bpp Tile-Support (Layer 2).

## [2026-06-11] — M5.2: Content Hash Export

- `SnesHdPackLoader.cpp`: Laden von `hashes.bin` Content-Hash-Einträgen.
- Ablösung des adressbasierten Tile-Matchings durch Content-basiertes.

## [2026-06-10] — M5.1: Checksum-Based Tile Matching

- VRAM-Checksums (`checksums.bin`) für Tile-Kollisionserkennung.
