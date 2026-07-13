# Debug Journal — SNES HD Pack (Mesen2 / DKC2)

Stand: 2026-07-13 | Mesen Build: P3.12 (BUILD+TEST PENDING) | Mainbrace: WORKING | Rambi Rumble: WORKING | Lockjaw: P3.12 palette tint fix pending test | Phase 4 Color Window: NOT STARTED

---

## P3.12 — Palette Tint: Multiplicative Color Correction for HDMA Palette Shifts (2026-07-13)

### Problem
Lockjaw's Locker: BG2 (natively rendered) shows correct blue water tint underwater, but BG1 HD tiles render WITHOUT the blue tint. User confirmed this is NOT a Color Math issue — the blue comes from **HDMA palette manipulation** (CGRAM entries are shifted to blue below the water line).

### Root Cause Analysis
The overlay tint extraction formula `tint = undoBrightness(ppuOutput) - SubScreenColor` produces **zero** for Lockjaw underwater pixels:
- Main screen = backdrop (black, $0000) or BG3 (mostly transparent)
- Sub screen = BG1/BG2 (level content with blue-shifted palette)
- CM: backdrop(0) + SubScreenColor = SubScreenColor
- undoBrightness(ppuOutput) ≈ SubScreenColor → tint = 0

The HD tiles were pre-rendered with the ORIGINAL (non-blue) palette. Since the blue tint is in the PALETTE COLOR itself (not in a separate overlay layer), the additive overlay extraction correctly finds zero — there IS no overlay. The color difference is purely palette-based.

### Fix: P3.12 Multiplicative Palette Tint
When the overlay tint extraction produces zero (all channels ≤ 1 in 5-bit space), switch to **multiplicative palette tint mode**:

1. **Detection**: `tintR5 ≤ 1 && tintG5 ≤ 1 && tintB5 ≤ 1` after overlay extraction
2. **Reference computation**: Get the HD tile's center pixel at this native position → this approximates the ORIGINAL palette color
3. **Tint ratio**: `ratio = SubScreenColor / HD_center_pixel` (per channel, fixed-point 8.8)
4. **Application**: `HD_pixel = HD_pixel * ratio` (multiplicative, preserves HD detail)
5. **HalveResult skip**: Palette tint skips the CM halve step (the ratio already represents the correct target color)

### Generic Properties
- No level-specific detection — works for ANY palette-based tint
- Additive overlay tint (Mainbrace fog, Rambi honey) unaffected — those have non-zero overlay tint
- BG3 overlay swap path unchanged (uses separate tint computation)

### Diagnostics Added
- `palTint` counter in FRAME summary — shows how many pixels used palette tint mode
- `PALTINT-SAMPLE` log entries — first 10 palette tint candidates per context with ppuOutput, SubScreenColor, MainLayers, SubLayers

### Expected Lockjaw Result
- Underwater BG1 HD tiles should now have blue-shifted colors matching native BG2
- `palTint` should show ~20k+ pixels per underwater frame
- Mainbrace fog and Rambi honey should be unaffected (non-zero overlay tint → normal path)

---

## P3.11 — Context Key Fix: PPU Registers in Context Identity (2026-07-13)

### Problem
Lockjaw's Locker never appeared in diagnostic logs despite user entering the level. The v4 diag file (5609 lines, 107 context changes) contained ONLY Mainbrace Mayhem data — all LEVEL ANALYSIS blocks showed Main=$04/Sub=$13/CM=$24. Zero trace of Lockjaw's register pattern (Main=$01/Sub=$16/CM=$21).

### Root Cause Analysis
Context detection was based ONLY on VRAM signature hash. While VRAM content should differ between level types, the diagnostic system had no PPU register info in context change lines or frame summaries, making it impossible to verify what level was actually running.

### Fix
1. **Combined context key**: `contextKey = vramSig XOR (ppuConfigKey * golden_ratio)` — ensures levels with different PPU configs are ALWAYS separate contexts, even if VRAM hashes were to collide
2. **PPU registers in CONTEXT CHANGE lines**: Now shows `Main=$XX Sub=$XX CM=$XX AddSub=X Br=X`
3. **PPU registers in FRAME summary lines**: Every frame now includes `Main=$XX Sub=$XX CM=$XX`
4. Context detection moved from VRAM-only to VRAM+PPU combined key

### Next Step
User needs to test again — enter Lockjaw's Locker and provide new P3.11 logs. The enhanced context detection should now capture Lockjaw as a distinct context with full PPU register visibility.

---

## ShipDeck BG3 Virtual Tilemap Pipeline (2026-07-02)

### Problem
ShipDeck levels (mapId=3) verwenden ein dynamisches Metatile-Scroll-System für BG3 (Schiffstaue,
Vordergrund-Dekor). Die VRAM-Tilemap bei $7800 ist zur Ladezeit komplett leer — nur die
Scroll-Engine füllt sie zur Laufzeit Stück für Stück. Die Export-Pipeline las diese leere
Tilemap und konnte daher keine BG3 per-tile PNGs oder Hashes generieren.

### Lösung: expandShipDeckMetatilesToTilemap()
Neue Helper-Funktion, die alle 49 Metatile-Definitionen (ROM 0x352087) und die 40×16 Metatile-Map
(ROM 0x3526A7) in eine flache 160×64 "virtuelle" Tilemap expandiert. Jedes Metatile wird zu 4×4
Tiles aufgelöst, mit korrekten H/V-Flip-Bits (XOR zwischen Metatile-Flip und Tile-Flip).

### Pipeline-Integration (4 Stellen)
| Stelle | Erkennung | Override |
|--------|-----------|----------|
| `saveCurrentHDToContainer()` | `currentStyle?.mapId === 3` | bg3TilemapData + ppuConfig.bg3TilesW/H |
| `buildCatalogByGfxSet()` | `style.mapId === 3` | catalogBg3TilemapData + catalogPpuConfig |
| `refreshContainerSetMetadata()` | `currentStyle?.mapId === 3` | bg3TilemapData + ppuConfig.bg3TilesW/H |
| `expandShipDeckMetatilesToTilemap()` | (die Funktion selbst) | Liest ROM, gibt {tilemapData, tilesW, tilesH} zurück |

### Kompatibilitäts-Check
- Per-tile PNG: srcTileSize = 1280/160 = 8 (1x) oder 5120/160 = 32 (4x HD) ✓
- Hashes: CHR-Daten in VRAM vorhanden (von gfxSet DMA geladen), nur Tilemap fehlte ✓
- Dedup: Tile/Palette-Key funktioniert unverändert ✓
- Un-flip: flipH/flipV korrekt aus dem virtuellen tileWord gelesen ✓

### Test-Plan
1. ShipDeck-Level laden (Pirate Panic, gfxSet 0x07)
2. Im Katalog öffnen → BG3-Bild kontrollieren (1280×512, Schiffstaue sichtbar?)
3. Container speichern → Console prüfen: `[save] ShipDeck BG3: replaced empty VRAM tilemap...`
4. HD-Pack exportieren → bg/bg3/gfxset_07/ prüfen: per-tile PNGs vorhanden?
5. hashes.bin: BG3 hash entries für gfxset 7 vorhanden?

---

## Open Issues

| # | Issue | Status | Seit | Letzer Test |
|---|-------|--------|------|-------------|
| A | BG3 native tiles transparent in Level 1 (Regression) | FIXED (M5.8) — verified M5.9+M5.10 | ~M5.5b | M5.10 OK |
| B | BG1/BG2 HD tiles fehlen in Level 2 (gfxset_25/0x37dez) | M5.11: fog 80/20 (war 75/25) | M5.2 | M5.10: BG1+BG2 sichtbar, fog 75/25 noch zu dunkel |
| C | Worldmap Tile-Kontamination | CLOSED | M5.1 | M5.7 (gefixt via vramSig-Sperre) |
| D | Performance Level 1 leicht schlechter | OFFEN — Optimierung geplant | M5.10 | M5.10: spielbar aber spürbar |
| E | Hot-Head Hop: Lava-Glow Color-Math (BG3 subtract) | M5.11: VERIFIED ✓ — Lava-Glow auf HD Tiles sichtbar | 2026-06-29 | M5.11 OK |
| F | Hot-Head Hop: Bubble-Animation (Frame-Mismatch) | Known Limitation (wie Piratenflagge, Beehive-Bienen/Larven) | 2026-06-29 | Erste Beobachtung |
| G | Glimmer's Galleon (ppuConfig 0x28): BG2 Sub-Screen + HDMA-Scroll-Effekt | Viewer-Limitation, Laufzeit teilweise OK (3D-Hintergrund fehlt) | 2026-06-29 | Erste Beobachtung |
| H | Hot-Head Hop: Unteres Fünftel — Layer Index Mismatch (BG2 runtime, BG1 in pack) | M5.13: Layer-agnostic retry — M5.14: Fallback-Retry entfernt (Bubble-Regression) | 2026-06-30 | M5.13: unteres 1/5 HD ✓, aber Blasen-Löcher im oberen Bereich |
| I | Rambi Rumble BG3: Honig opak, Bienenstock verdeckt (Sub-Screen-Blend) | v3 BESTÄTIGT — Layer-Reihenfolge korrekt. Feintuning (Alpha, Catalog) nächste Session | 2026-07-02 | v3 getestet 2026-07-03: Waben=BG, Honig=FG ✓ |

---

## Issue A: BG3 Native Tiles Transparent (Level 1)

### Symptom
Level 1 (Pirate Panic, gfxset_07): BG3-Layer-Elemente (Schiffstaue, Masten, Vordergrund-Dekor)
sind unsichtbar. Diese sind sporadische Elemente — KEIN ganzflächiger Fog wie in Level 2.
BG1 HD-Tiles werden angezeigt, BG2 ebenfalls — aber die BG3-Vordergrund-Elemente fehlen komplett.
War **vor M5.5** sichtbar (Regression).

### Verdacht (Code-Analyse 2026-06-22)

**Wahrscheinliche Ursache: Fallback-Logik ersetzt BG3-gewonnene Pixel mit opakem BG1-HD-Tile.**

Code-Flow in `SnesHdVideoFilter.cpp:249-292`:
1. BG3 (Schiffstau etc.) gewinnt PPU-Compositing → `BgWinnerLayer = 2`
2. Schritt 1: Versuche HD-Tile für BG3 → keines vorhanden (kein BG3-HD-Pack)
3. Schritt 2 (Fallback): Iteriere andere Layer → findet BG1 HD-Tile (layer 0)
4. BG1 HD-Tile wird mit `alpha=0xFF` gerendert → kompletter Pixel-Replace
5. Ergebnis: BG3-Elemente verschwunden (wurden durch opakes BG1-Bild ersetzt)

**Eingeführt durch:** `08fd3216` (multi-layer BG fallback) oder `3a369ba9` (stop-at-first)

### Versuchs-Log

| Datum | Hypothese | Test | Build | Ergebnis | Schlussfolgerung |
|-------|-----------|------|-------|----------|------------------|
| 2026-06-22 | Code-Analyse: Fallback überschreibt BG3 | Diag-Log M5.7: fb==BG3 count (100% Fallback) | M5.7 | BESTÄTIGT: alle BG-Pixel via Fallback, BG3 invisible | Root cause confirmed |
| 2026-06-22 | Fix: Gate fallback on `winLayer != 2` | Code-Change in M5.8 | M5.8 | PENDING TEST | Expected: BG3 sichtbar, HD BG1 dort nicht gerendert |

### Fix (M5.8) — Implemented 2026-06-22

**Change:** `SnesHdVideoFilter.cpp:280` — Fallback gated on `winLayer != 2`

```cpp
// Before (M5.7):
if(!hdTile) { /* fallback loop */ }

// After (M5.8):
if(!hdTile && winLayer != 2) { /* fallback loop */ }
```

**Additional changes:**
- New counter `frameBg3FogNative` tracks pixels where BG3 won → native rendering
- New `else if(winLayer == 2)` branch: BG3-gewonnene Pixel nicht als Miss gezählt
- Diag summary zeigt jetzt `fogNat=N` Feld

**Erwartetes Verhalten (Level 1, M5.8):**
- BG3-Elemente (Schiffstaue, Masten) SICHTBAR auf dem Bildschirm
- An Pixeln wo BG3 aktiv ist: native PPU-Compositing (BG1 + BG3 überlagert)
- An Pixeln wo BG1 direkt gewinnt (kein BG3): HD-Tile wird gerendert
- `fogNat` im Log = Anzahl Pixel wo BG3 gewonnen hat

**Kompromiss:** An Pixeln wo BG3 aktiv ist, werden KEINE HD BG1 Tiles gerendert.
Stattdessen wird das native PPU-Bild angezeigt (BG1 nativ + BG3 darüber).
Für HD+BG3 gleichzeitig bräuchte man compositor-aware Alpha Blending → **M5.9 löst dies.**

### Ruled Out
*(noch nichts definitiv ausgeschlossen)*

---

## Issue B — Sub-Problem: BG3 Fog blockiert HD BG1

### Symptom (entdeckt nach M5.8 + chrRawData-Fix)
Level 2 (Mainbrace Mayhem): Hash-Matching funktioniert (BG1miss=0, notInPack=0),
aber HD BG1 Tiles werden nur angezeigt wo KEIN BG3-Fog vorhanden ist (~10% des Screens).
Überall wo BG3-Fog aktiv ist (~90% = fogNat=51881), wird das native PPU-Bild gezeigt.
Der Nebel ist dabei "komplett transparent" anstatt nebelartig.

### Root Cause
M5.8 Fix (Issue A) gated fallback auf `winLayer != 2` — blockiert ALLE HD-Rendering
wenn BG3 gewinnt. Korrekt für opake BG3-Elemente (Level 1 Schiffstaue), aber
FALSCH für semi-transparenten Fog (Level 2, Bienenstock-Honig, Unterwasser-Effekte).

### Fix (M5.9) — Implemented 2026-06-23

**Erkenntnis:** `AllowColorMath` Flag (0x80 in MainScreenFlags) unterscheidet zuverlässig:
- Opakes BG3 (Level 1: Taue/Masten): `AllowColorMath = 0` → skip fallback (M5.8 behavior)
- Semi-transparentes BG3 (Level 2: Fog): `AllowColorMath = 0x80` → fog-blend path

**DKC2 Effekte die alle BG3 + Color Math nutzen:**
- Fog (Rigging-Levels wie Mainbrace Mayhem)
- Honig (Bienenstock-Levels)
- Wasser-Effekte (Unterwasser-Levels)

Alle diese Effekte nutzen SNES Color Math (Register $2131 CGADSUB, Bit 2 = BG3 enabled).
Die AllowColorMath-Erkennung behandelt sie alle korrekt.

**Changes (2 Dateien):**

1. **`Core/SNES/SnesPpu.cpp`** (PPU-Seite):
   - Speichert `_mainScreenBuffer[x]` in `px.MainScreenColor` VOR `ApplyColorMath()`
   - Gibt dem HD-Filter den rohen BG3-Fog-Farbwert (pre-math, pre-brightness)

2. **`Core/SNES/HdPacks/SnesHdVideoFilter.cpp`** (HD-Filter):
   - Neuer Pfad (Zeile ~303): `if(!hdTile && winLayer == 2 && (MainScreenFlags & 0x80))`
     → Versuche BG1 HD-Tile zu finden (layer 0 unter dem Fog)
   - Neues Flag `bg3FogBlend` markiert diese Pixel für spezielle Rendering
   - Pixel-Output: Repliziert PPU's Half-Addition Color Math mit HD-Tile:
     `output = (BG3_fog_color + HD_BG1_pixel) / 2` pro Kanal (8-bit)
   - Neuer Diagnose-Counter `fogB=` (fog-blend matches) im Frame-Summary

```cpp
// Fog-Blend Rendering (half-addition):
uint8_t outR = (fogR + hdR) >> 1;
uint8_t outG = (fogG + hdG) >> 1;
uint8_t outB = (fogB + hdB) >> 1;
```

**Erwartetes Verhalten (Level 2, M5.9):**
- HD BG1 Tiles ÜBERALL sichtbar (auch unter Fog)
- Fog-Effekt als halbtransparente Überlagerung auf HD-Tiles
- Diag: `fogB=` zeigt Anzahl fog-blend Pixel (erwartet: ~50k statt fogNat)
- `fogNat=` nur noch für BG3 pixels wo KEIN BG1 content darunter ist

**Erwartetes Verhalten (Level 1, M5.9 = M5.8 unchanged):**
- BG3-Elemente weiterhin sichtbar (AllowColorMath nicht gesetzt → opaker Pfad)
- Kein Fog-Blend angewendet

**Mögliche Einschränkungen:**
- Hardcoded Half-Addition (`>> 1`). Falls ein Level Full-Addition oder Subtraction
  nutzt, wäre das Ergebnis leicht falsch. Für DKC2 Fog ist Half-Addition Standard.
- Brightness < 15 (Fade-Effekte): MainScreenColor ist pre-brightness, HD-Tile ist
  full-brightness → leichte Diskrepanz während Fades (akzeptabel, kurz).

---

## Issue B: BG1 HD Tiles fehlen in Level 2

### Symptom
Level 2 (Mainbrace Mayhem, gfxset 37 dez / 0x25 hex): BG1 zeigt nur native Tiles.
BG2-Tiles werden korrekt in HD dargestellt. Level 1 BG1 funktioniert.

### Kern-Problem (zusammengefasst)
Der Container für gfxset_25 speichert `bg1ChrBase = $2000` (falsch). Korrekt wäre `$6000`
(aus PPU-Register BG12NBA = $26). Der Hash-Export berechnet daher BG1-Hashes aus den
falschen VRAM-Bytes (BG2-Daten statt BG1-Daten) → 0% Match-Rate in Mesen.

### Versuchs-Log

| Datum | Hypothese | Test | Build/Viewer | Ergebnis | Schlussfolgerung |
|-------|-----------|------|--------------|----------|------------------|
| ~06-12 | chrRawData als Hash-Quelle falsch | Hash-Vergleich Viewer vs Mesen | M5.2 | 0/60 Hashes matchen | BESTÄTIGT: chrRawData ≠ VRAM |
| ~06-12 | Fix: vramSnapshot statt chrRawData | Viewer-Patch (Änderung 1 in M5.3) | M5.3 Viewer | Hashes sollten matchen | IMPLEMENTED, aber Level 2 funktioniert trotzdem nicht |
| ~06-13 | Viewer's vramSnapshot weicht von Mesen-VRAM ab (DMA Type 13) | VramCompare2 Analyse | M5.5 | Statische Tiles (31+) matchen, VBlank-Tiles (1-30) nicht | BESTÄTIGT: vramSnapshot pre-DMA ≠ post-DMA |
| ~06-15 | VBlank DMA Injection (Frame 0 ins Snapshot injizieren) | Viewer Commits 885c867, a831c5d | M5.5 Viewer | Noch nicht getestet gegen Mesen-Runtime | OFFEN |
| ~06-15 | Palette-Mismatch (Viewer P04/P06, Runtime P02) | Debug-Log Analyse | M5.5 | MISS-Hashes mit pal=2, LOADED mit P04/P06 | Palette Expansion (1b774a6) implementiert |
| ~06-17 | Palette-Fallback in GetMatchingTile | Build M5.5f (42719d1b) | M5.5f | palFB=0 im Diag → hat NIE gegriffen | REMOVED in M5.6: verursachte Worldmap-Contamination |
| ~06-20 | Sub-screen sprite guard + VramAddress fix | Build M5.6 (0b57bcbe) | M5.6 | Level 2 BG1 immer noch nicht da | Sprite-Guard war korrekt aber nicht die Ursache |
| 2026-06-22 | ? | Neuer Mesen-Build M5.7 | M5.7 | BG1 Level 2 fehlt weiterhin | Nächster Schritt: Diag-Log auswerten |
| 2026-06-22 | PPU-Register: bg1ChrBase=$6000, Container hat $2000 | ROM-Analyse + VRAM-Dump + Diag-Log | M5.7 + ROM | ROOT CAUSE: Container speichert falsche chrBase | Fix: Refresh oder VRAM-Import |
| 2026-06-22 | Container-Refresh behebt chrBase? | SHA256 Vergleich: hashes.bin vor/nach Refresh byte-identisch | Viewer | NEIN — Refresh ändert chrBase NICHT | Refresh ist KEIN Fix-Pfad |
| 2026-06-22 | Multi-chrBase: levels[0] hat andere ppuConfig als Level 2 | Viewer-Code-Analyse: buildCatalogByGfxSet nutzt levels[0] | Viewer | BESTÄTIGT: levels[0] gibt chrBase=$2000, Level 2 braucht $6000 | Fix: Multi-chrBase Export |
| 2026-06-22 | Multi-chrBase Export → Cross-Layer Collision | User-Test: beide Levels visuell korrupt, BG2 verschwunden | Viewer + M5.8 | BESTÄTIGT: 5 chrBases exportiert, Hash-Kollisionen BG1↔BG2 | Fix: Single-Best Detection (nur 1 chrBase) |
| 2026-06-22 | Single-Best v2a: bestCount → $3000 statt $6000 | Console: "overridden → $3000"; $3000=BG2-Region | Viewer | FALSCH: $3000 inside BG2 CHR, wrapping penalized $6000 | Fix v2b: density scoring, no wrapping, all indices |
| 2026-06-22 | Single-Best v2b: density scoring → $3000 (88.1%) | Console: $3000=88.1%, $6000=44.9% (511/760 valid) | Viewer | FALSCH: vramSnapshot ist Viewer-Simulation, hat BG1-Daten NICHT korrekt bei $6000 | Fix v3: chrRawData cross-reference |
| 2026-06-23 | Fix v3: chrRawData byte-for-byte cross-reference | Committed `24d9df5`. Vergleicht decompressed chrRawData gegen vramSnapshot-Bytes an jedem chrBase-Kandidaten | Viewer | PENDING TEST | Nächster Schritt: Mesen VRAM-Dump importieren → Export → Console prüfen |

### Definitiv Ausgeschlossen (Ruled Out)

| Ursache | Beweis | Datum |
|---------|--------|-------|
| chrRawData als Hash-Quelle | M5.3 Viewer-Patch auf vramSnapshot umgestellt, BG2 (gleiche Quelle) funktioniert | 06-12 |
| Palette-Fallback (cross-palette matching) | palFB=0 in Diag, hat nie geholfen; verursachte Worldmap-Bug | 06-17 |
| Byte-Ordering-Differenz Viewer↔Mesen | Verifiziert: beide little-endian, DMA Mode 1 → identische Reihenfolge | 06-12 |
| FNV-1a Algorithmus-Unterschied | Identischer Code in C++ und JS bestätigt via manuelle Stichprobe | 06-12 |
| Gfxset-Scoping (Fingerprint-System) | Deaktiviert seit M5.4 (32bd50b9), kein Effekt | 06-15 |
| Container-Refresh als Fix | hashes.bin SHA256 vor/nach Refresh identisch — ppuConfig wird nicht aktualisiert | 06-22 |
| Multi-chrBase Export (alle Bases >50%) | Verursacht Cross-Layer Hash-Kollisionen: BG2/BG3-Daten als BG1 exportiert → falsche HD-Tiles geladen | 06-22 |
| Density-only chrBase Detection (ohne chrRawData) | vramSnapshot (Viewer-Sim) hat BG1-Daten NICHT bei $6000; $3000 (BG2-Region) gewinnt immer | 06-22 |

### Root Cause (identifiziert 2026-06-22)

**PPU-Register-Analyse für Level 2 (VBlank Type 13, DATA_FD7C80):**
- `BG12NBA ($210B) = $26` → BG1 chrBase = `(0x26 & 0x0F) * 0x1000` = **$6000**
- BG2 chrBase = `((0x26 >> 4) & 0x0F) * 0x1000` = **$2000**
- `BG34NBA ($210C) = $07` → BG3 chrBase = **$7000**

**Gfxset_25 VRAM-Destinations (ROM DATA_FD8537, nach Stripping von bit 15 = compressed flag):**
1. `DATA_DFD537` → VRAM $2000 (compressed) — BG2 CHR
2. `DATA_EA121C` → VRAM $5000 (compressed)
3. `DATA_CAFABE` → VRAM $7000 (compressed) — BG3 CHR
4. `DATA_EA8D3C` → VRAM $6000 (compressed) — **BG1 CHR!**
5. `DATA_D1F971` → VRAM $6C00 (compressed) — BG1 CHR (oberer Teil)

**VRAM-Dump-Verifikation (Byte-Offsets = Word-Addr × 2):**
- BG1 CHR ($C000–$DFFF): 7175/8192 non-zero bytes — dicht belegt, valide 4bpp Tile-Daten
- BG2 CHR ($4000–$9FFF): 18979/24576 non-zero bytes — korrekt
- BG3 CHR ($E000–$FFFF): 5755/8192 non-zero bytes — korrekt

**Root Cause:** Der Container für gfxset_25 wurde mit einer **älteren Viewer-Version** gespeichert,
die `bg1ChrBase` falsch berechnet oder gar nicht gespeichert hat:
- Alte Version verwendete hardcodierten "3-case DMA switch" (Kommentar Zeile 2068)
- Oder speicherte `bg1ChrBase: 0` (bzw. `undefined`) → Viewer-Default `0x2000` (Zeile 1962)
- **Aktueller Viewer-Code (Zeile 1313) berechnet korrekt: `(0x26 & 0x0F) * 0x1000 = $6000`**

**Beweis-Kette:**
1. Container hat `bg1ChrBase = $2000` (oder 0/undefined → Fallback $2000)
2. Hash-Export (Zeile 7549): `const chrBase = ppuCfg.bg1ChrBase || 0` → liest $2000 aus Container
3. Hashes werden aus VRAM-Region $2000 berechnet (= BG2 Daten!)
4. Mesen berechnet Hashes korrekt aus VRAM $6000 (echte BG1 Daten)
5. → 0% Match weil BG2-Daten-Hashes ≠ BG1-Daten-Hashes

**Fix-Pfad:**
- ~~Container-Refresh für gfxset_25~~ → **FUNKTIONIERT NICHT** (hashes.bin identisch vor/nach)
- ~~VRAM-Dump-Import (umgeht das Problem)~~ → Umgeht nur das VRAM-Problem, nicht das chrBase-Problem
- **IMPLEMENTIERT: Multi-chrBase Export** → Viewer scannt alle Levels im Gfxset, sammelt alle
  einzigartigen bg1ChrBase-Werte, generiert Hash-Einträge und PNGs für JEDE chrBase

### Warum Refresh nicht funktioniert (22.06.2026)

**Befund:** SHA256 der hashes.bin vor und nach Container-Refresh sind BYTE-IDENTISCH:
`dc1ed5ee...cd20` (beide Packs).

**Ursache:** `refreshContainerSetMetadata()` (Zeile 8112) aktualisiert ppuConfig nur wenn
`currentBgData && currentBgData.ppu` wahr ist (Zeile 8153). Im Katalog-Modus ist
`currentBgData` nicht gesetzt → ppuConfig bleibt auf dem gespeicherten Wert ($2000).
Selbst wenn es aktualisiert würde: `buildCatalogByGfxSet()` nutzt `levels[0]` als Referenz,
und `levels[0]` hat möglicherweise ppuConfig mit chrBase=$2000.

### Warum levels[0] die falsche chrBase hat

`scanGraphicsSets()` (Zeile 4099) iteriert alle 192 Level und gruppiert nach `style.graphics`.
Für gfxset 37 (0x25) gibt es mehrere Level mit unterschiedlichen `ppuConfig`-Indizes.
`levels[0]` (das erste gefundene Level) hat einen ppuConfig mit `BG12NBA` Low-Nibble = 2
→ `bg1ChrBase = $2000`. Level 2 (Mainbrace Mayhem) hat `BG12NBA = $26`
→ `bg1ChrBase = $6000`.

### Multi-chrBase Fix (implementiert 22.06.2026)

**Änderungen in `index.html` (Viewer `exportAsTexturePack`):**

**1. ChrBase-Sammlung (nach Zeile 7181):**
Für jeden Gfxset werden alle Level gescannt → `readPpuConfig()` für jedes Level →
alle einzigartigen `bg1ChrBase`-Werte in `allBg1ChrBases` (Set) gesammelt.

**2. PNG-Export (Zeile 7247ff):**
Für jedes 8x8 Sub-Tile wird das PNG einmal erstellt, dann unter ALLEN chrBase-abgeleiteten
VRAM-Adressen ins ZIP geschrieben. Content-Hash als Gatekeeper: falsche chrBase-PNGs
matchen nie, da die VRAM-Inhalte an verschiedenen Adressen verschieden sind.

**3. Palette-Varianten (Zeile 7303ff):**
`collectBG1PaletteVariantsFromROM()` wird jetzt pro chrBase aufgerufen (statt einmal).
Die Funktion überspringt intern Level mit nicht-matchender chrBase → korrekte Zuordnung.

**4. Hash-Export (Zeile 7575ff):**
`hashChrBases` (Set) sammelt alle chrBases. Für jede chrBase werden Hash-Einträge
aus dem vramSnapshot generiert. Der FNV-1a Hash wird aus den echten VRAM-Bytes an der
jeweiligen Adresse berechnet → nur korrekte Einträge matchen zur Runtime.

**Warum das sicher ist:**
- Für Level 2 (chrBase=$6000): Emulator hasht VRAM[$C020:$C03F] → findet Hash-Eintrag
  bei vramAddr=$6010 → lädt PNG `6010_P06.png` → KORREKT
- Für Bonus-Level (chrBase=$2000): Emulator hasht VRAM[$4020:$403F] → findet Hash-Eintrag
  bei vramAddr=$2010 → lädt PNG `2010_P06.png` → Andere VRAM-Daten, anderer Hash →
  Content-Hash matcht NICHT → PNG wird ignoriert (kein Schaden)

**PROBLEM (erkannt 22.06.2026 Abend):** Multi-chrBase erzeugt Cross-Layer-Kollisionen!
Das Scannen ALLER chrBases mit >50% non-zero Tiles führt dazu, dass BG2/BG3-Daten an
$3000/$4000/$5000 fälschlicherweise als BG1-Tile-Daten interpretiert werden.
Mesen's TileByKey nutzt {ContentHash, PaletteIndex, LayerIndex} — NICHT vramAddr.
Wenn ein BG2-Tile-ContentHash zufällig einem (fälschlicherweise exportierten) BG1-Eintrag
entspricht, wird das falsche HD-Bild geladen → visuelle Korruption in beiden Leveln.

### Single-Best-ChrBase Fix v2 (implementiert 2026-06-22)

**Status: SUPERSEDED by Fix v3 (chrRawData cross-reference)**

**Änderung:** `detectChrBasesFromVRAM()` gibt NUR NOCH EINE chrBase zurück — die mit
dem HÖCHSTEN non-zero Tile-Count (vorher: ALLE mit >50%).

**PROBLEM v2a (22.06 Abend):** Erster Versuch mit "best count" wählte $3000 statt $6000!
- $3000 (word) = byte $6000 = mitten in der BG2 CHR Region ($4000-$9FFF)
- BG2-Grafik-Daten sind dicht belegt → hoher Score
- $6000 (word) = byte $C000 = korrekte BG1 Region, aber:
  - Hohe Tile-Indices (>256) wrappen via `& 0x7FFF` auf Adressen $0000-$2000 (leer/sparse)
  - Das Boolean-Scoring ("has ANY non-zero word") war zu grob — auch Tilemap-Daten zählen

**Fix v2b: Drei Verbesserungen:**
1. **Kein Address-Wrapping:** `rawAddr > 0x7FFF` → skip (statt `& 0x7FFF`)
   - Verhindert, dass hohe chrBases ($5000+) durch Wrap-Around benachteiligt werden
2. **Density-Scoring:** Zählt ALLE non-zero Words pro Tile (nicht nur "mindestens 1")
   - Echte Tile-Grafik: ~87% Word-Density (4 Bitplanes, dicht belegt)
   - Tilemap-Daten an falscher Adresse: niedrigere Density
3. **Alle Indices:** Kein 50-Sample-Limit mehr, nutzt ALLE unique Tile-Indices
   - Repräsentativeres Scoring, kein Sampling-Bias

**Diagnostic Logging hinzugefügt:** Zeigt Score für ALLE 8 chrBases im Console-Log.

**Calling-Code geändert (PNG-Export + Hash-Export):**
- Alte Logik: `allBg1ChrBases.add(stored); allBg1ChrBases.add(...detected); allBg1ChrBases.add(...romScan)`
- Neue Logik: Wenn VRAM-Detection einen Treffer hat → NUR diesen verwenden (ersetzt stored).
  Wenn Detection fehlschlägt → Fallback auf stored chrBase. ROM-Scan ENTFERNT (lieferte
  für gfxset 37 immer $2000 weil alle Levels im ROM denselben ppuConfig-Index haben).

**Erwartetes Ergebnis:**
- Gfxset 7 (Level 1): Detection findet $2000 (korrekt, == stored) → 1 chrBase → KEINE Kollisionen
- Gfxset 37 (Level 2): Detection findet $6000 (höchste Density) → 1 chrBase → BG1 korrekt,
  BG2/BG3 nicht fälschlich als BG1 exportiert → KEINE Cross-Layer-Kollisionen

### Noch Offen / Nicht Abschliessend Getestet

| Frage | Status | Nächster Schritt |
|-------|--------|------------------|
| Container-Refresh behebt bg1ChrBase? | **NEIN** — hashes.bin identisch vor/nach | Refresh ist kein Fix-Pfad |
| Multi-chrBase Export behebt BG1 Level 2? | **NEIN** — verursacht Cross-Layer-Kollisionen | Ersetzt durch Single-Best Fix v2 |
| Single-Best v2b (density only) behebt Level 2? | **NEIN** — vramSnapshot hat BG1 nicht bei $6000 | Density-only kann $6000 NICHT finden |
| Fix v3: chrRawData cross-ref behebt Level 2? | READY TO TEST | Import Mesen VRAM → Export → Console Log prüfen |
| Level 1 BG1 Anordnung korrekt nach Fix v3? | READY TO TEST | Prüfen ob keine falsch zugeordneten Tiles |
| Sind alle BG1-Tiles betroffen oder nur bestimmte? | BEANTWORTET: ALLE | M5.7 Diag: BG1notInPack==BG1miss für 100% of BG1 tiles |
| vramSnapshot nach Refresh == Viewer-Simulation? | **JA** — Refresh überschreibt importierten Dump | WARNUNG: nicht Refreshen nach VRAM-Import! |

### VRAM Dump/Import System (IMPLEMENTED in Viewer)
Das System umgeht die Viewer-VRAM-Simulation komplett:
1. Mesen: VRAM dumpen wenn Level 2 geladen ✓ (Dump vorhanden)
2. Viewer: Dump importieren → ersetzt vramSnapshot ✓ (UI + Code committed)
3. Export: Hashes basieren auf echtem Mesen-VRAM ✓ (uses vramSnapshot)

**Status:** Viewer-Import-UI ist committed und funktional (`importVramForSet()`, line 8046).
User muss den vorhandenen 64KB VRAM-Dump für gfxset_25 importieren und dann neu exportieren.
Verifiziert: VRAM-Dump Hashes == Mesen-Runtime Hashes (FNV-1a Vergleich am 22.06).

**WARNUNG:** `refreshContainerSetMetadata()` (Zeile 8391-8397) ÜBERSCHREIBT importierte
VRAM-Dumps mit der Viewer-Simulation! Nach VRAM-Import NICHT Refreshen vor dem Export!

**Workflow für jeden neuen Gfxset:**
1. Level in Mesen laden → Tools > Dump VRAM to File
2. Viewer → Container Manager → VRAM-Button für den Set klicken → .bin auswählen
3. **NICHT Refreshen!** (überschreibt den importierten Dump)
4. "Texture Pack" exportieren → hashes.bin enthält korrekte Hashes

### ChrRawData Cross-Reference Fix v3 (implementiert 2026-06-23)

**Commit:** `24d9df5` (DKC2-HD-Tools, branch `main`)

**Konzept:** Deterministische chrBase-Erkennung durch Byte-Vergleich.
- `chrRawData` enthält die dekomprimierten Tile-Grafiken sequentiell (Tile N bei Byte N*32)
- Für jeden chrBase-Kandidaten ($0000..$7000 in $1000-Schritten):
  - Vergleiche chrRawData[N*32..N*32+31] mit vramSnapshot[(chrBase+N*16)*2..(chrBase+N*16)*2+31]
  - Zähle byte-for-byte Matches
- Der chrBase mit den meisten Matches ist definitiv korrekt (>50% Threshold)
- Falls chrRawData unavailable oder inconclusive: Fallback auf Density-Scoring

**Sampling-Strategie:**
- 40 non-empty Tiles mit Index >= 30 (VBlank-DMA-Zone vermeiden)
- "Non-empty" = mindestens 1 Byte != 0 in chrRawData

**Kritischer Punkt:** Die Methode funktioniert NUR wenn vramSnapshot die chrRawData-Bytes
tatsächlich an der richtigen Adresse enthält. Zwei Szenarien:

| vramSnapshot Quelle | chrRawData gefunden bei | Korrekt? |
|---------------------|------------------------|----------|
| Viewer-Simulation   | $2000 (wo Viewer BG1 hinlegt) | FALSCH — Mesen nutzt $6000 |
| Mesen VRAM-Dump     | $6000 (echte Runtime-Adresse) | KORREKT |

**Deshalb:** Für gfxset_25 MUSS der Mesen-VRAM-Dump importiert sein (nicht Refresh-Simulation)!

**Erwarteter Console Output (bei korrektem Dump):**
```
[detectChrBase] chrRawData cross-ref (40 tiles): $0=0/40, $1000=0/40, $2000=0/40,
  $3000=0/40, $4000=0/40, $5000=0/40, $6000=38/40, $7000=0/40
[detectChrBase] → best (chrRawData match): $6000 (38/40)
[export] gfxset 25: stored chrBase $2000 overridden by VRAM detection → $6000
```

---

## Issue C: Worldmap Tile-Kontamination (CLOSED)

### Symptom (historisch)
Worldmap zeigte Level-HD-Tiles an falschen Stellen.

### Fix
M5.7 (`c0d0ec17`): `isWorldmap`-Erkennung via `vramSig == 0xDBF342F9932FD251`.
HD-Tile-Lookup komplett gesperrt wenn `isWorldmap` → native Rendering.

### Status: GEFIXT und VERIFIZIERT (User-Test 2026-06-22)

---

## Erkenntnisse aus Diagnostik-Log + VRAM-Dump (22.06.2026)

### Was wir gemacht haben

1. **Mesen M5.7 gebaut** mit umfangreicher Diagnostik (loggt pro Frame: match/miss/fallback/layer-Daten)
2. **Diag-Log aufgezeichnet** für Level 1 und Level 2 (`snes_hd_diag2206v2.txt`)
3. **VRAM-Dump erstellt** von Level 2 (64KB, via Mesen Tools > Dump VRAM)
4. **VRAM-Dump gegen Log verifiziert** (8 Adressen geprüft: identische FNV-1a Hashes)

### Was das Diagnostic-Log bewiesen hat

#### Level 1 (Pirate Panic, sig=1DF33CEAA50F7F08):
- **100% Match-Rate** — ALLE BG-Pixel finden ein HD-Tile
- **100% via Fallback** — `fb` (Fallback-Zähler) == `BG3` (BG3-Pixel-Zähler)
- **0 Miss** — kein einziger Pixel ohne HD-Treffer
- **Bedeutung:** Überall wo BG3-Inhalt vorhanden ist, gewinnt BG3 das PPU-Compositing.
  Da kein HD-Pack für BG3 existiert, springt der Fallback an und findet BG1-HD-Tiles.
  Diese werden opak gerendert → BG3-Elemente (Taue, Masten) komplett überdeckt.
- **Beweis für Issue A:** Der Fallback ist 100% schuld an den unsichtbaren BG3-Elementen.

#### Level 2 (Mainbrace Mayhem, sig=BD2C76B73C545997):
- **BG1: 0% Match** — `BG1miss=16403, BG1notInPack=16403`
- **Alle Treffer sind BG2** — `match=40941`, alle via Fallback (BG3 fog → BG2 gefunden)
- **Kein Palette-Problem** — `palMis=0` (Runtime-Palette == Pack-Palette)
- **Kein Layer-Problem** — `layerMis=0` (Runtime-Layer == Pack-Layer)
- **Alle BG1-Hashes komplett unbekannt** — "notInPack" = Hash existiert nirgends im Pack
- **MISS-Adressen: 0x2530 – 0x48B0** — gesamter BG1-CHR-Bereich, nicht nur VBlank-Zone
- **Bedeutung:** Die Viewer-VRAM-Simulation für gfxset_25 erzeugt komplett andere
  Byte-Inhalte an den BG1-VRAM-Adressen als das echte Spiel.

### Was der VRAM-Dump bewiesen hat

- Wir haben den **64KB VRAM-Dump** (aus Mesen, Level 2 geladen) genommen
- **8 VRAM-Adressen aus dem MISS-Log** gegen den Dump geprüft (FNV-1a Hash berechnet)
- **Ergebnis: 100% Übereinstimmung** — Dump-Hashes == Log-Hashes
- **Bedeutung:**
  - Mesen's Runtime-VRAM ist korrekt und konsistent (kein Timing-Problem, kein Race)
  - Der Dump ist eine verlässliche Ground-Truth-Quelle
  - Das Problem liegt **ausschließlich** im Viewer: dessen `loadLevelBackground()`
    simuliert den VRAM-Inhalt für gfxset_25 falsch (andere Bytes als das echte Spiel)

### Architektur-Erkenntnis

| Gfxset | Viewer-Simulation | Mesen-Runtime | Ergebnis |
|--------|-------------------|---------------|----------|
| gfxset_07 (Level 1) | ✓ Korrekt | ✓ | 100% Match |
| gfxset_25 (Level 2) | ✗ Falsche Bytes | ✓ | 0% BG1 Match |

**Root Cause:** Der Viewer's VRAM-Simulator ist für manche Gfxsets unzuverlässig.
DKC2 verwendet komplexe DMA-Operationen (Type 7, 8, 13, 19) und Level-spezifische
CHR-Lade-Routinen die der Viewer nicht alle korrekt nachbildet.

**Lösung:** VRAM-Dumps als Ground Truth verwenden statt Simulation. Das Import-System
im Viewer existiert bereits und ist getestet. Pro Gfxset einmal den VRAM dumpen,
importieren, und alle zukünftigen Exports basieren auf echten Daten.

---

## Fix M5.10 — BG2 Fog-Blend + Lighter Blend Weight (2026-06-23)

### Problem (aus M5.9 Test-Log)

M5.9 hat BG1-Tiles unter BG3-Fog zum ersten Mal sichtbar gemacht ✓  
Aber zwei Probleme blieben:

1. **BG2 fehlt:** In Level 2 besteht ~100% des Screens aus BG2 (Parallax-Hintergrund, ~55760 px/frame).
   Wenn BG1 transparent ist (~38000 px/frame = 69% des Screens), sieht man KEINEN HD-Tile, weil
   der Fog-Blend-Pfad nur `BgLayerMask & 0x01` (BG1) prüfte.
2. **Fog zu dunkel:** Die 50/50 Formel `(fog + hd) >> 1` ist zu aggressiv. HD-Tiles sind heller
   gestaltet als native SNES-Pixel, daher sieht die 1:1 Mischung unnatürlich dunkel aus.

### Analyse (M5.9 Diagnostik `snes_hd_diag 2306.txt`)

```
Pixel-Budget pro Frame (Level 2):
  BG1   ~17.449 px  (31% des Screens — Vordergrund-Elemente)
  BG2   ~55.760 px  (≈100% — Parallax Hintergrund)
  fogB  ~15.000 px  (BG1 unter Fog → fog-blend in M5.9)
  fogNat ~34.000 px (BG2 unter Fog → NOT blended → blieb native)
```

**Root Cause BG2:** Der fog-blend Pfad (Schritt 3) versuchte nur Layer 0 (BG1).
Für ~34000 px pro Frame, wo BG2 existiert aber BG1 transparent ist, wurde kein HD-Tile gesucht.

**Root Cause Fog-Helligkeit:** Original PPU nutzt half-addition `(a+b)>>1` auf **native** 
Pixel-Farben. HD-Tiles haben hellere, detailreichere Farben → gleiches 50/50 ergibt sichtbar
dunkleres Ergebnis als intended.

### Fix (M5.10) — Implemented 2026-06-23

**Datei:** `Core/SNES/HdPacks/SnesHdVideoFilter.cpp`

**Änderung 1: BG2 Fallback im Fog-Blend-Pfad (Zeile ~303-322)**

```cpp
// Try BG1 (layer 0) first — highest priority background
if(pixelInfo.BgLayerMask & 0x01) {
    hdTile = _hdData->GetMatchingTile(pixelInfo.BgTiles[0].Key, hdScreen->Vram);
    if(hdTile) {
        tileInfo = &pixelInfo.BgTiles[0];
        bg3FogBlend = true;
    }
}
// If no BG1 HD tile, try BG2 (layer 1) — parallax background
if(!hdTile && (pixelInfo.BgLayerMask & 0x02)) {
    hdTile = _hdData->GetMatchingTile(pixelInfo.BgTiles[1].Key, hdScreen->Vram);
    if(hdTile) {
        tileInfo = &pixelInfo.BgTiles[1];
        bg3FogBlend = true;
    }
}
```

**Änderung 2: Blend-Gewichtung 75% HD / 25% Fog (Zeile ~532+560)**

```cpp
// Vorher (M5.9): 50/50
uint8_t outR = (fogR + hdR) >> 1;

// Nachher (M5.10): 75/25
uint8_t outR = (uint8_t)((hdR * 3 + fogR) >> 2);
```

Gleiches Muster für alpha-blend + fog Pfad.

### Erwartetes Ergebnis

| Test | Erwartung |
|------|-----------|
| Level 2 BG1 unter Fog | Weiterhin sichtbar (wie M5.9) |
| Level 2 BG2 unter Fog | **NEU: HD-Tiles sichtbar** (Parallax-Hintergrund) |
| Level 2 Fog-Helligkeit | Leichter/natürlicher als M5.9 |
| Level 1 (BG3 Taue) | Keine Regression (winLayer==2 Gate bleibt) |
| Worldmap | Keine Regression (kein BG3-fog dort) |

### Risiko-Analyse

- BG2-Lookup kostet zusätzliche Hash-Suche pro Pixel → Performance-Impact möglich
  (nur für Pixel wo BG1 transparent UND BG3 aktiv = ~34k px/frame = ~6% von 240*256*scale)
- 75/25 Gewichtung könnte zu hell sein (Fog kaum sichtbar) → ggf. auf 2/3+1/3 anpassen
- BG2-Tiles könnten höhere miss-rate haben (ungetestet) → Diagnostik prüfen

### Test-Ergebnis (2026-06-23 abends, Log: snes_hd_diag2306abends.txt)

**Level 2 (Mainbrace Mayhem) — ERFOLG:**

| Metrik | M5.9 | M5.10 | Bewertung |
|--------|------|-------|-----------|
| `fogB` | ~15.000 | **~50.300** | BG1+BG2 unter Fog jetzt alle HD |
| `fogNat` | ~34.000 | **0** | ELIMINIERT — kein native fallback unter Fog |
| `BG2` | n/a | **~55.700** | BG2 vollständig populiert |
| `miss` | 0 | **0** | Perfekte Hash-Rate beibehalten |
| `BG1miss` | 0 | **0** | Perfekt |
| `fb` | ~3.200 | **~3.300** | Fallback (non-fog) stabil |
| `mask0` | 0 | **0** | Kein Pixel ohne Layer |

- BG1 HD Tiles sichtbar unter Fog ✓
- BG2 HD Tiles sichtbar unter Fog ✓ (NEU in M5.10)
- Fog-Helligkeit deutlich besser als M5.9, **aber noch etwas zu dunkel**
  → Nächster Schritt: Gewichtung von 75/25 auf ~80/20 oder 5/6+1/6 anpassen
- Performance Level 2: gut, kein spürbarer Unterschied zu M5.9

**Level 1 (Pirate Panic) — OK mit Performance-Regression:**
- BG1 + BG2 HD korrekt ✓
- BG3 Taue/Masten sichtbar ✓ (keine Regression)
- **Performance etwas schlechter als früher** (war mal flüssiger)
  → Vermutlich kumulativer Effekt der zusätzlichen per-pixel checks
  → Tile-Level Caching als Optimierung geplant (siehe unten)

**Worldmap — OK:** Sauber, keine Regression ✓

### Offene Punkte nach M5.10

1. **Fog noch etwas zu dunkel** — Gewichtung anpassen (80/20 oder 5:1)
2. **Performance Level 1** — Tile-Level Caching implementieren (siehe Optimierungs-Sektion)

---

## Geplante Optimierung: Tile-Level Caching

### Motivation
Aktuell wird `GetMatchingTile()` für JEDEN Pixel aufgerufen. Innerhalb eines 8x8 nativen
Tiles ist der Hash-Key identisch — die selbe Suche wird 64× wiederholt pro Tile.

### Idee
Pro Scanline oder pro Tile-Zeile den Hash-Lookup einmal durchführen und das Ergebnis
(`HdPackTileInfo*`) für alle 8 Pixel der Tile-Zeile cachen.

**Erwarteter Gewinn:**
- Hash-Lookups reduziert um Faktor ~8 (pro Zeile) bis ~64 (pro ganzes Tile)
- Betrifft ALLE HD-Rendering-Pfade (nicht nur Fog), also globale Verbesserung
- Besonders wirksam in Fog-Levels wo zusätzliche BG2-Lookups stattfinden

**Risiken:**
- Cache-Invalidierung muss korrekt sein (VRAM-Änderungen mid-frame sind selten aber möglich)
- Implementierungs-Aufwand: mittel (lokaler Cache pro Scanline reicht)

**Status:** Geplant, nicht priorisiert. Wird relevant wenn mehr Levels HD-Packs haben.

---

## Geplantes Projekt: VRAM-Dump-Pipeline für alle Gfxsets

### Problem
Der Viewer simuliert den VRAM-Inhalt pro Gfxset über `loadLevelBackground()`.
Diese Simulation ist unzuverlässig — DKC2 nutzt komplexe DMA-Operationen (Type 7, 8, 13, 19)
und level-spezifische CHR-Lade-Routinen die der Viewer nicht korrekt nachbildet.
Ergebnis: gfxset_07 (Level 1) funktioniert zufällig, gfxset_25 (Level 2) hatte 0% Match.

### Lösung: Lua-Script VRAM-Dump + Viewer-Integration

**Phase 1: Savestates anlegen (einmalig, manuell)**
- DKC2 Level-Select-Cheat nutzen um jedes Gfxset zu erreichen
- Pro Gfxset einen Mesen-Savestate speichern
- DKC2 hat ~40 Gfxsets, viele Levels teilen sich eins
- Geschätzter Aufwand: ~30-60 Min mit Cheat

**Phase 2: Lua-Script (einmalig entwickeln, beliebig oft ausführbar)**
- Script lädt Savestates der Reihe nach
- Wartet 1 Frame (VRAM vollständig befüllt nach VBlank-DMA)
- Dumpt 64KB VRAM (`emu.memType.snesVideoRam`) + 512B CGRAM + PPU-Register
- Speichert als binäre Datei pro Gfxset
- Mesen2 API bestätigt (recherchiert 2026-06-23):
  - `emu.read(addr, emu.memType.snesVideoRam)` — VRAM lesen ✓
  - `emu.read(addr, emu.memType.snesCgRam)` — Paletten lesen ✓
  - `emu.loadSavestate(state)` — Savestate laden ✓
  - `io.open()` — Dateien schreiben ✓ (I/O-Zugriff in Settings aktivieren)
  - `emu.getState()` — PPU-Register auslesen ✓

**Phase 3: Viewer-Integration (einmalig)**
- VRAM-Dumps als Ground-Truth-Datenquelle im Viewer hinterlegen
- `loadLevelBackground()` / Hash-Berechnung nutzt Dump-Daten statt Simulation
- Pro Gfxset: eine binäre Datei mit den korrekten CHR-Bytes
- Ergebnis: **Korrekte Hashes für alle Gfxsets, ohne Simulation**

### Workflow nach Integration

```
Neues HD-Grafikset erstellt → Viewer öffnen → Gfxset auswählen
→ Viewer kennt korrekte VRAM-Daten (aus Dump, hardcoded)
→ Hash-Berechnung stimmt automatisch
→ HD Pack exportieren → in Mesen laden → funktioniert
```

Kein erneuter Dump nötig. Die VRAM-Daten sind ROM-determiniert und ändern sich nicht.
Nur die HD-Grafiken (PNGs) ändern sich — die Hash-Seite bleibt stabil.

### Warum nicht den Viewer-DMA-Simulator fixen?
- Mesen emuliert DMA als Teil der CPU/PPU-Schleife (Register, Timing, VBlank-abhängig)
- Den Simulator korrekt nachbauen = halben SNES emulieren
- Auch nach Fix: keine 100% Garantie für alle Gfxsets
- Dump-Ansatz ist einfacher, schneller, und garantiert korrekt

**Status:** ~~Geplant als nächstes größeres Projekt.~~ → **Phase 2 implementiert (26.06.2026).**

### Implementierung: dkc2_vram_dump.lua (26.06.2026)

**Lua-Script gebaut** (`dkc2_vram_dump.lua` im Projekt-Root), implementiert Phase 2 der Pipeline:

**ROM-Analyse beim Start (kein Gameplay nötig):**
- Scannt alle 192 Level-IDs über ROM-Pointer-Chain:
  `$3D:0000` → Property-Table → Style-Pointer → `graphics`-Byte (Offset 13)
- Findet alle unique Gfxsets und zeigt "Einkaufsliste" im Log
  (welches Level pro Gfxset besucht werden muss)
- NPC-Shops (Cranky 0x08, Funky 0x09, Wrinkly 0x0A, Swanky 0x0B, Klubba 0x0C)
  als Hardcoded-Override (Property-Type 0x0004/0x0005 hat keine Style-Daten)

**Interaktiver Dump-Modus:**
- HUD-Overlay: aktuelles Level, erkanntes Gfxset, Fortschritt (X/N)
- `[F2]` = Dump VRAM (64KB) + CGRAM (512B) + PPU-State + Savestate
- `[F3]` = Zeigt verbleibende Gfxsets im Log
- Duplikat-Erkennung (bereits gedumpte Gfxsets werden übersprungen)
- Savestate-Capture über NMI-Exec-Callback (Constraint: `emu.createSavestate()`
  funktioniert nur in Exec-Callbacks)

**Level-ID-Erkennung:**
- WRAM-Adresse `$003E` war **FALSCH** (zeigte 0x28 in Pirate Panic, 0x48 in Mainbrace Mayhem)
- Korrekte Adresse: **`$0539`** — verifiziert 2026-06-29 per `dkc2_find_level_addr.lua`
  - Pirate Panic (gfxset_07): `$0539 = 0x07` ✓
  - Mainbrace Mayhem (gfxset_25): `$0539 = 0x25` ✓
- Script wurde korrigiert: `readCurrentGfxset()` liest `$0539` direkt
  (Commit `84eb8b66`)

**Output pro Gfxset (im Script Data Folder):**
- `gfxset_XX_vram.bin` — 65536 Bytes, Viewer-Import-kompatibel
- `gfxset_XX_cgram.bin` — 512 Bytes Paletten-Daten
- `gfxset_XX_state.txt` — PPU-Register + Metadaten
- `gfxset_XX.savestate` — für zukünftiges Batch-Re-Dumping

**Voraussetzungen:**
- Mesen Script Settings: "Allow I/O" aktivieren
- Completionist SRM für Level-Zugang (siehe SRM/RGD-Fix unten)

---

### Phase 2 Ergebnis: 25 Gfxsets gedumpt (2026-06-29)

**Dump-Ordner:** `C:\Users\beach\OneDrive\Dokumente\Mesen2\LuaScriptData\dkc2_vram_dump`

**Alle 25 gedumpten Gfxsets:**

| GfxSet | Level-Beispiel |
|--------|----------------|
| gfxset_02 | Worldmap (Krok) |
| gfxset_03 | Worldmap (Krem Quay) |
| gfxset_04 | Worldmap (Krazy Kremland) |
| gfxset_07 | Pirate Panic |
| gfxset_1D | Gusty Glade |
| gfxset_1E | Castle Crush |
| gfxset_20 | Krazy Kremland (Roller Coaster) |
| gfxset_21 | Topsail Trouble |
| gfxset_22 | Web Woods |
| gfxset_24 | Lava Lagoon |
| gfxset_25 | Mainbrace Mayhem |
| gfxset_26 | Barrel Bayou |
| gfxset_27 | Krow's Nest |
| gfxset_28 | Hot Head Hop / Red Hot Ride |
| gfxset_29 | Slime Climb (Note: Palette differs from gfxset_25, gleiche CHR) |
| gfxset_2A | ~~Rambi Rumble~~ (Falsch — Rambi Rumble = gfxSet 0x04, Level 0x02) |
| gfxset_2B | Bramble Scramble |
| gfxset_2C | Snakey Chantey Bonus |
| gfxset_2D | Slime Climb |
| gfxset_2E | Hornet Hole |
| gfxset_2F | Target Terror |
| gfxset_30 | Klobber Karnage / Ghoul Galley |
| gfxset_31 | Animal Barrel (Enguarde/Squawks) |
| gfxset_33 | K. Rool's Keep Snowfields |
| gfxset_34 | Krocodile Kore Boss |

**Nicht dumpbar — NPC-Shops (kein Gfxset-Register):**
- gfxset_08 Cranky's Hut, gfxset_09 Funky's, gfxset_0A Wrinkly's,
  gfxset_0B Swanky's, gfxset_0C Klubba's Toll
- `$0539 = 0` in diesen Screens — statische VRAM-Hintergründe, kein DMA-Gfxset

**ROM-Phantom-Gfxsets (in ROM-Pointer-Tabelle, nie zur Laufzeit aktiv):**
- `0x23`, `0x32` — erscheinen in ROM-Scan, `$0539` zeigt sie nie

---

### SRM/RGD-Fix (2026-06-28)

**Problem:** Completionist-SRM wurde von Mesen ignoriert — Mesen lud stattdessen
seinen eingebetteten Savestate aus der `.rgd`-Datei (`RecentGames/*.rgd`, ~1.8MB).

**Root Cause:** `RecentGames\Donkey Kong Country 2 - Diddy's Kong Quest.rgd` enthielt
einen eingebetteten Auto-Savestate, der bei jedem Start geladen wurde — überschrieb SRM.

**Fix:**
1. `.rgd` → `.rgd.bak` umbenannt (nicht löschen — Mesen erstellt neue .rgd nach erstem Start)
2. Completionist-SRM nach `Mesen2\Saves\` kopiert
3. Mesen neu starten → SRM wird geladen → alle Levels zugänglich

**WICHTIG:** Wenn Mesen per Reset (statt Fenster-X) beendet wird, flusht es den
In-Memory-SRAM auf die Disk und überschreibt die SRM. Immer Fenster schließen.

---

### Phase 3: Ground-Truth-Integration im Viewer (2026-06-29)

**Ansatz:** Statt manuellem VRAM-Import pro Session, werden alle 25 Dumps direkt
im Viewer hardecodiert als JavaScript-Konstante.

**Dateien:**
- `C:\DEV Claude\DKC2-HD-Tools\generate_vram_groundtruth.py`
  - Liest alle `gfxset_XX_vram.bin` (je 65536B) aus dem Dump-Ordner
  - Base64-kodiert jeden Dump, schreibt `vram_groundtruth.js`
  - Re-run nötig wenn neue Dumps hinzukommen: `python generate_vram_groundtruth.py`
- `C:\DEV Claude\DKC2-HD-Tools\dkc2-viewer\vram_groundtruth.js` (auto-generiert)
  - `const VRAM_GROUND_TRUTH = { "gfxset_07": "<base64>", ... };`
  - 25 Gfxsets × 64KB = ~2.1MB base64
  - **Nicht manuell bearbeiten** — immer per Python neu generieren

**Viewer-Integration (Commit `882f23a`):**
- `index.html`: `<script src="vram_groundtruth.js"></script>` eingebunden
- `_b64ToUint8Array(b64)` — Hilfsfunktion (Base64 → Uint8Array, file://-kompatibel)
- `applyGroundTruthVram(sets)` — iteriert alle Container-Sets, setzt `vramSnapshot`
  aus VRAM_GROUND_TRUTH wenn kein manueller Snapshot vorhanden
- Hook in `loadContainerToHDPack()` → `applyGroundTruthVram(sets)` nach `hdGetContainerSets()`
- Safety-Fallback in `exportAsTexturePack()` vor `if (set.vramSnapshot)` Block

**Workflow jetzt:**
```
Viewer öffnen → Container laden → vramSnapshot auto-apply aus Ground-Truth
→ "Texture Pack" exportieren → Hashes stimmen ohne manuelle VRAM-Dumps
```

---

## Issue E: Hot-Head Hop — Lava-Glow Color Math (2026-06-29)

### Symptom
- Ca. 1/5 des Bildschirms (unten): kein HD tile angezeigt, nur nativer Pixel
- Die anderen 4/5 (oben): HD BG1 tiles korrekt — aber OHNE den Lava-Glow-Effekt (falsch)
- Der Effekt betrifft im Original alle BG1 tiles im Level

### PPU-Konfiguration (aus gfxset_20_state.txt)
```
bgMode = 1
colorMathEnabled    = 6     → Bit1+Bit2 → BG2+BG3 Color Math aktiv
colorMathSubtractMode = true → SUBTRACT (nicht ADD wie Level 2 Nebel!)
colorMathAddSubscreen = false → Fixed Color (kein Sub-Screen)
fixedColor          = 4228  → RGB(4,4,4) ≈ sehr dunkel
subScreenLayers     = 0     → kein Sub-Screen
mainScreenLayers    = 23    → BG1+BG2+BG3+OBJ alle auf Main Screen
```

### Ursache (revidiert 2026-06-30)
DKC2 Hot-Head Hop nutzt **DREI separate Effekt-Mechanismen** gleichzeitig:

1. **HDMA auf $2131 (ColorMathSelectAndEnable) pro Scanline:**
   - Ändert welche Layer Color Math haben — per Scanline!
   - In ASM: HDMA Channel 3 schreibt $2131 kontinuierlich
   - Auf bestimmten Scanlines wird BG1 (Bit 0) in Color Math eingeschlossen
   - → PPU subtrahiert fixedColor von BG1 Pixeln auf diesen Scanlines
   - → HD-Filter zeigte bisher HD-Tile OHNE diesen Subtract → "kein Glow"

2. **HDMA auf $2132 (FixedColorData) pro Scanline:**
   - Animiert fixedColor-Wert → Lava-Pulseffekt über den ganzen Bildschirm
   - Verschiedene Scanlines haben verschiedene Abdunkelungsstärken

3. **Palette-Cycling (VBlank):**
   - 8-Phasen Lava-Palette (CGRAM 5/6/7) via DATA_80BA51
   - Tiles mit diesen Palette-Rows → Farben animiert (im Viewer als "animated" markiert)
   - HD-Tiles sind statische PNGs → Palette-Cycling nicht sichtbar
   - Separates Problem (nicht in M5.11 adressiert)

### Fix: M5.11 — Winner Color Math Delta

**Ansatz:** Delta-Berechnung aus PPU pre-math vs post-math Farben.

```
MainScreenColor = rohe BG1-Farbe (vor Color Math, BGR555)
ppuOutputBuffer = finale Farbe (nach Color Math, BGR555)
delta = postMath - preMath  (pro 5-bit Komponente, ×8 → 8-bit)
HD_output = HD_pixel + delta  (clamped 0-255)
```

**Implementierung** (`SnesHdVideoFilter.cpp`):
- Zeilen 532-556: Delta pre-computation (nur wenn `!bg3FogBlend && AllowColorMath`)
- Zeilen 581-591: Delta-Anwendung auf opake HD-Pixel
- Zeilen 614-618: Delta-Anwendung auf alpha-geblendete Pixel
- Diagnostik: `cmDelta` Counter in Frame-Summary

**Vorteile:**
- Automatisch korrekt für add UND subtract Modi
- Erfasst HDMA pro-Scanline Änderungen (ppuOutput hat bereits HDMA-beeinflussten Wert)
- Kein Struct-Umbau nötig — nutzt bestehende MainScreenColor + ppuOutputBuffer
- Bestehender Fog-Blend (Level 2 Nebel) bleibt unberührt

**Gleichzeitig:** Fog-Blend von 75/25 auf 80/20 angepasst (Issue B).

**Status:** Test ausstehend. Erwartung: obere 4/5 mit Lava-Glow-Effekt, unteres 1/5 unverändert (siehe Issue H).

---

## Issue H: Hot-Head Hop — Unteres Fünftel ohne HD-Tiles (2026-06-30)

### Symptom
- Unteres ~1/5 des Bildschirms zeigt nur native Tiles (kein HD)
- Stalaktiten (BG1 Vordergrund) WERDEN als HD angezeigt, auch im unteren Bereich
- Andere BG1-Tiles auf gleicher Höhe werden NICHT als HD angezeigt

### Beobachtete Layer-Reihenfolge (Viewer + Mesen)
| Level | Vorne → Hinten | BG3 Rolle |
|-------|----------------|-----------|
| Pirate Panic | BG3, BG1, BG2 | Vordergrund (Taue, Masten) |
| Hot-Head Hop | BG1, BG1, BG3 | Hintergrund (Lava-Flüsse, Berge) |

Hot-Head Hop: kein BG2 exportiert im Viewer, BG3 = Hintergrund-Lava.

### Initiale Hypothese (WIDERLEGT durch M5.12 Test — siehe unten)
SNES Mode 1 Tile-Prioritäten:
```
BG1 Priority 1 (hoch)  →  über BG3  → Stalaktiten → HD ✓
BG3                     →  dazwischen → Lava-Hintergrund
BG1 Priority 0 (niedrig) →  unter BG3  → Terrain → BG3 gewinnt
```

Wenn BG3 im unteren Bereich Compositing gewinnt UND HDMA dort `colorMathEnabled`
BG3-Bit abschaltet → AllowColorMath = false → Fog-Blend-Gate blockiert →
kein Fallback auf BG1 → native Pixel.

Der bisherige Fog-Blend-Gate (`winLayer == 2 && AllowColorMath`) wurde in M5.8
eingeführt um Issue A zu verhindern (Pirate Panic: BG3-Taue sollen nicht von
BG1 überschrieben werden). Der Gate unterscheidet aber nicht zwischen:
- BG3 = opaker Vordergrund (Pirate Panic → Gate korrekt)
- BG3 = Hintergrund-Overlay (Hot-Head Hop → Gate zu restriktiv)

### Recherche-Ergebnis (2026-06-30)

**ROM-Analyse:** Alle 44 ppuConfig-Einträge (`DATA_FD79E2`) systematisch geparst.
Validiertes Kriterium basierend auf `$212C` (mainScreenLayers):

```
BG1_on_main = ($212C & 0x01) != 0
BG3_on_main = ($212C & 0x04) != 0

BG3 FOREGROUND: BG3_on_main && !BG1_on_main  (7 Konfigurationen)
BG3 BACKGROUND: BG3_on_main && BG1_on_main   (19 Konfigurationen)
```

| Level | $212C | BG1 main | BG3 main | Typ |
|-------|-------|----------|----------|-----|
| Hot-Head Hop | $17 | ja | ja | BACKGROUND → Fallback erlaubt |
| Pirate Panic | $17 | ja | ja | BACKGROUND (aber BG1 hat kein color math → Flag false) |
| Lockjaw's Locker | $04 | nein | ja | FOREGROUND → Gate blockiert |

**Aber:** $212C allein reicht nicht! Pirate Panic und Hot-Head Hop haben identischen
$212C=$17. Unterscheidung gelingt über **BG1 Color Math Detection:**

### Fix: M5.12 — `frameHasBg1ColorMath` Progressive Detection (hat Issue H NICHT gelöst)

**Ansatz:** Frame-Level Flag das prüft ob BG1 jemals `AllowColorMath` hatte.

```
frameHasBg1ColorMath = false

// Für jeden Pixel (top-to-bottom, left-to-right):
if (winLayer == 0 && AllowColorMath):
    frameHasBg1ColorMath = true

// BG3 background fallback:
if (winLayer == 2 && !AllowColorMath && frameHasBg1ColorMath):
    → Fall back to BG1/BG2 HD tile (plain, ohne Fog/Delta)
```

**Warum sicher gegen Issue A (Pirate Panic):**
- Pirate Panic: `$2131 = $02` → BG1 Bit 0 = 0 → BG1 hat NIE color math
- `frameHasBg1ColorMath` bleibt false → neuer Fallback-Pfad triggert nicht
- Pirate Panic Verhalten: unverändert ✓

**Warum Hot-Head Hop funktioniert:**
- HDMA aktiviert BG1 color math auf oberen Scanlines
- Obere Scanlines werden zuerst verarbeitet → Flag wird true
- Untere Scanlines (BG3 gewinnt ohne color math): Flag ist true → Fallback erlaubt
- BG1 HD Tile wird plain gerendert (kein Fog, kein Delta — Delta wäre ohnehin 0)

**Level 2 Nebel (Mainbrace Mayhem):**
- `$2131 = $44` → BG1 Bit 0 = 0 → kein BG1 color math
- `frameHasBg1ColorMath` bleibt false → bestehender Fog-Blend-Pfad unberührt ✓

**Implementierung** (`SnesHdVideoFilter.cpp`):
- Zeile ~207: `frameHasBg1ColorMath` Flag + `frameBg3BgFallback` Counter
- Zeile ~271: Flag-Tracking nach `winLayer` Bestimmung
- Zeilen ~339-367: Neuer Fallback-Pfad (4) nach Fog-Blend-Pfad (3)
- Rendering: Keine Änderung nötig — plain HD tile automatisch korrekt
  (bg3FogBlend=false, applyColorMathDelta=false → `outputBuffer = hdColor`)
- Diagnostik: `bgFb` Counter in Frame-Summary

**Status:** Test ausstehend. Erwartung: unteres Fünftel zeigt jetzt BG1 HD Tiles.

### M5.12 Test-Ergebnis (2026-06-30)

**Ergebnis:** `bgFb=0` — der frameHasBg1ColorMath-Fallback hat NICHT getriggert.
Hypothese war falsch: BG3 gewinnt im unteren Bereich NICHT das Compositing.

**Diagnostik-Log-Analyse (snes_hd_diag 3006.txt):**
```
match=28395  bgFb=0  fogNat=1312  layerMis=13573  BG1=28779 BG2=50957 BG3=25823
BG1miss=0  BG1notInPack=0
```

**Entscheidende Erkenntnisse:**
- `BG1miss=0`: Jeder BG1-Tile matched perfekt — BG1 Lookup funktioniert 100%
- `layerMis=13573`: 13.573 Pixel haben Layer-Mismatch — dominanter Fehler-Modus
- `BG2=50957`: BG2 hat die MEISTEN Pixel (nicht BG3 wie vermutet)
- `bgFb=0`: Der M5.12 bg3BgFallback-Pfad hat nie getriggert
- MISS-Einträge: `mask=0x06` (BG2+BG3, kein BG1) im unteren Bereich

**LAYER_MISMATCH Log-Einträge:**
```
hash=455B87A7E14411BE pal=5 runtime_layer=1 pack_layer=0
                            (BG2 runtime)    (BG1 in HD pack)
```

**Root Cause:** Identischer Tile-Content (gleicher Hash, gleiche Palette) erscheint
auf BG1 im oberen Bereich (matched) und auf BG2 im unteren Bereich (Layer-Index-
Mismatch → kein Match). Der TileByKey-Lookup erfordert ContentHash + PaletteIndex +
LayerIndex — alle drei müssen übereinstimmen.

**Positive Seiteneffekte von M5.12:**
- Pirate Panic BG2 Wasser: Jetzt korrekt blau-grün (Color Math Delta auf BG2)
- Hot-Head Hop Bubbles: Dunkler Rahmen weg, Animation flüssiger

### Fix: M5.13 — Layer-Agnostic Retry für BG1↔BG2 (4bpp)

**Ansatz:** Nach jedem fehlgeschlagenen HD-Tile-Lookup für BG1 oder BG2 wird
ein Retry mit dem jeweils anderen Layer-Index durchgeführt.

```
// Wenn Winner BG2 ist und kein HD Tile gefunden:
altKey = winner.Key
altKey.LayerIndex = 0  // versuche mit BG1-Index
hdTile = GetMatchingTile(altKey, Vram)
// (und umgekehrt für BG1 → BG2)
```

**Sicherheit:**
- BG1 und BG2 sind beide 4bpp in Mode 1 — identische Tile-Daten mit gleicher
  Palette erzeugen identische Pixel. Layer-Index-Austausch ist visuell korrekt.
- BG3 (2bpp) ist ausgeschlossen — kein Interchange mit 4bpp-Layern.
- Retry wird in ALLEN 4 Lookup-Pfaden angewendet: Winner, Fallback-Loop,
  Fog-Blend, BG3-Background-Fallback.

**Implementierung** (`SnesHdVideoFilter.cpp`):
- `frameLayerRetry` Counter für Diagnostik
- Nach Schritt 1 (Winner Lookup): Retry mit alternativem LayerIndex
- In Schritt 2 (Fallback Loop): Retry für 4bpp-Fallback-Layer
- In Schritt 3 (Fog-Blend): Retry bei BG1/BG2-Lookup unter BG3
- In Schritt 4 (BG3 Bg Fallback): Retry bei BG1/BG2-Lookup unter BG3
- Diagnostik: `lRetry` Counter in Frame-Summary

**Erwartung:** `layerMis` Zähler sinkt auf ~0, `lRetry` steigt auf ~13573.
Unteres Fünftel zeigt HD Tiles. Pirate Panic + Level 2 unverändert.

### M5.13 Test-Ergebnis (2026-07-01)

**Ergebnis:** Unteres 1/5 zeigt HD-Tiles ✓ — Layer-Retry hat funktioniert.
```
layerMis=0  lRetry≈26600  notInPack≈666  BG1miss=0  BG1notInPack=0
```

**ABER: Neue Regression entdeckt — Lava-Blasen im oberen Bereich als "Löcher"**

- Oberer Bereich: Blasen-Positionen zeigen Level-Hintergrund statt Blasen-Animation
- Unterer 1/5 Bereich: Blasen rendern korrekt als native Pixels
- Gelegentlich ein HD-Frame sichtbar (der dem statischen Export entspricht)
- Identisches Phänomen wie Issue F (Piratenflagge in Level 2)

**Root Cause Analyse:**

M5.13 hat Layer-Retry auch im Fallback-Loop (Schritt 2, Zeilen 322-327) eingebaut.
Ablauf bei DMA-Blasen-Pixel im oberen Bereich:

1. BG2 DMA-Blase gewinnt → Hash einzigartig (DMA) → kein Match
2. Layer-Retry auf Winner → gleicher Hash, anderer Layer → kein Match
3. Fallback-Loop: versucht BG1 (i=0) → BG1 hat Tile-Daten an Lava-Position
4. BG1 direkt → z.B. mit layer=0 exportiert → kein Match
5. **Fallback Layer-Retry: BG1 mit layer=1 → MATCH!** (BG1-Tile war als BG2 exportiert)
6. Rendert BG1s statisches HD-Tile an Blasen-Position → "Loch" im Lava-Bereich

**Warum unteres 1/5 nicht betroffen:**
Dort gewinnt BG3 → Pfade 3/4 (Fog-Blend / BG3-Bg-Fallback). Diese haben keinen
generischen Fallback-Loop. BG1 hat dort keine Tile-Daten (mask bit not set).

**Warum M5.12 nicht betroffen war:**
M5.12 hatte keinen Layer-Retry im Fallback-Loop. BG1 an Lava-Positionen matchte
nicht direkt (layer=0 ≠ layer=1 im Pack) → native Rendering → Blasen sichtbar.

### Fix: M5.14 — Layer-Retry aus Fallback-Loop entfernt

**Änderung:** 6 Zeilen Layer-Retry im Fallback-Loop (Schritt 2) auskommentiert.
Layer-Retry bleibt NUR im Winner-Lookup (Schritt 1b) und in den BG3-Pfaden (3/4).

**Rationale:**
- Fallback-Loop versucht ANDERE Layer als den Winner
- Layer-Retry im Fallback erzeugt doppelte Indirektion: falscher Layer + falscher Index
- Dies matched vollständig unrelated Tiles (BG1-Hintergrund statt BG2-DMA-Blase)
- Ohne Retry: Fallback nur bei exaktem Layer-Index-Match (selten, weniger schädlich)

**Code (`SnesHdVideoFilter.cpp` Zeilen ~322-335):**
```cpp
// REMOVED (M5.14): Layer retry was here but caused bubble regression.
// The commented-out code can be restored if needed. See comment in source.
```

**Erwartung:**
- Blasen im oberen Bereich: native Rendering (wie M5.12) ✓
- Unteres 1/5: weiterhin HD (Layer-Retry in Schritt 1b unverändert) ✓
- Level 2 Fog: unverändert ✓
- Pirate Panic: unverändert ✓
- `lRetry` Counter sinkt leicht (nur noch Winner + Pfade 3/4 zählen)

**Revert-Anleitung:** Kommentierte Zeilen in Schritt 2 wieder einkommentieren
(4 Zeilen ab "if(!hdTile && (i == 0 || i == 1))").

---

## Issue F: Hot-Head Hop — Bubble-Animation Frame-Mismatch (2026-06-29)

### Symptom
- Animierte Blasen (Luftblasen, aufsteigend) erscheinen nur wenn Animation-Frame = Dump-Frame
- Andere Frames: native Pixel (falsche/keine Bubbles)
- Außerdem: dunklerer Rahmen um das HD-Tile (visueller Mismatch zur native Version)

### Ursache
Identisch mit Level 2 Piratenflagge:
- VBlank-DMA schreibt Bubble-Tiles in VRAM jeden Frame neu
- VRAM-Dump erfasst nur einen Frame → nur dieser Frame matched
- Darker border: vermutlich ein Zeichenfehler im HD-Asset (kein System-Bug)

### Status
Known Limitation — gleicher Root Cause wie Piratenflagge (Multi-Frame-Animation).
Mögliche Langzeitlösung: Alle Animations-Frames dumpen und als separate HD-Tiles einbinden.

---

## Issue G: Lockjaw's Locker — Sub-Screen BG2 + HDMA-Parallax (2026-06-29)

### Symptom
- BG2 (Hintergrund) zeigt "Fill-Tiles" (Kacheln wirken zu flach/falsch)
- Kein echter 3D-Tiefeneffekt wie im Original
- Fehler auch im Viewer sichtbar (kein Laufzeit-spezifisches Problem)

### PPU-Konfiguration (aus gfxset_28_state.txt)
```
bgMode              = 1
mainScreenLayers    = 4  → nur BG3 auf Main Screen
subScreenLayers     = 19 → BG1, BG2, OBJ auf Sub Screen
colorMathEnabled    = 36 → Bit2+Bit5 → BG3 + Backdrop Color Math aktiv
colorMathSubtractMode = true → subtract
colorMathAddSubscreen = true → Sub-Screen-Mischung (nicht fixedColor)
fixedColor          = 0
window[0].activeLayers[2] = true, invertedLayers[2] = true → BG3 nur außerhalb Window 0
```

### Architektur
- BG3 = Unterwasser-Tint-Overlay (Main Screen, mit Window geclippt)
- BG1 = Spielfiguren / Vordergrund (Sub Screen)
- BG2 = Hintergrund-Tiles (Sub Screen, chrBase=$4000, Tilemap $7000)
- Color Math: BG3 - Sub(BG1/BG2/OBJ) pro Pixel

### Warum Fill-Tiles
1. **Viewer zeigt kein HDMA**: BG2 nutzt HDMA-Scroll (Scroll ändert sich pro Scanline → Parallax/Wellen-Effekt). Viewer zeigt statischen Flat-Tilestrip.
2. **Viele identische Tiles**: Unterwasser-Hintergrund besteht aus vielen gleichen Tiles (gleiches VRAM-Byte-Pattern → gleicher Hash → ein PNG). Im Viewer: wiederholtes Kacheln.
3. **Laufzeit**: Mesen wendet HDMA korrekt an → echte Parallax-Optik vorhanden.

### HD-Tile-Darstellung in Mesen (User-Feedback 2026-06-30)
- **Laufzeit grundsätzlich OK:** BG1 und BG2 HD tiles werden korrekt angezeigt
- **3D-Parallax-Hintergrund fehlt:** Der HDMA-animierte Hintergrund (Tiefeneffekt)
  wird im Mesen-Runtime nicht als HD dargestellt — vermutlich weil der Viewer diesen
  Hintergrund nicht vollständig exportiert (HDMA-Scroll wird nicht simuliert)
- **Viewer-Export-Limitation:** Viewer exportiert nicht alle Hintergrund-Elemente;
  nur die statisch sichtbaren Tiles werden in den Container aufgenommen

### Status
Viewer-Limitation bestätigt. Laufzeit funktioniert bis auf 3D-Hintergrund.
Kein Code-Fix im HD-Filter nötig — Problem liegt im Viewer-Export.

### Sub-Investigation: Decompressor-Truncation-Hypothese (2026-06-30 / 2026-07-01)

**Hypothese:** `rareDecompress()` bricht zu früh ab → fehlende BG-Tiles.

**Untersuchung:**
1. Diagnostik-Instrumentierung eingebaut (Trace Ring-Buffer, Exit-Logging, Size-Vergleich)
2. BG1 CHR (VRAM $2000): dekomprimiert $3000 Bytes, GFX-Tabelle sagt $3400 → "Truncation" detektiert
3. BG2 CHR (VRAM $4000): dekomprimiert $5480, Tabelle $5480 → exakt gleich
4. Alle 32 Dekompressions-Commands Zeile für Zeile gegen p4plus2/DKC2-disassembly `compression.asm` verifiziert — keine Logik-Fehler
5. Carry-Flag-Analyse für alle ADC-Operationen: carry=0 in allen Fällen korrekt

**Resolution: KEIN BUG — False Alarm.**
Analyse des `VRAM_payload_handler` im DKC2-Disassembly ($BB8CB5) beweist:
```asm
PHX               ; Table-Index sichern VOR Dekompression
JSL decompress    ; dekomprimiert nach $7F0000, gibt Anzahl in X zurück
PLX               ; ÜBERSCHREIBT X sofort — Decompressor-Rückgabewert wird VERWORFEN
LDA FD819F,x      ; liest DMA-Größe aus TABELLE (Bytes 5-6 des 7-Byte-Eintrags)
STA DMA[0].size   ; DMA transferiert Tabellen-spezifizierte Bytes aus $7F0000
```
Das `size`-Feld in der GFX-Set-Tabelle ist die **DMA-Transfer-Größe**, NICHT die erwartete
dekomprimierte Größe. Wenn der Decompressor weniger Bytes produziert als die Tabelle sagt,
kommen die restlichen Bytes aus dem vor-gelöschten $7F0000-Buffer (Nullen).
Unser `new Uint8Array(0x20000)` ist ebenfalls vor-genullt → identisches Verhalten.

**Cleanup (2026-07-01):**
- `expectedSize` Parameter aus `rareDecompress()` entfernt
- Trace Ring-Buffer und False-Alarm-Warnings entfernt
- Echte Fehler-Warnings (unknown opcode, MAX_ITER) bleiben erhalten

**Ruled Out:** Decompressor-Truncation als Ursache für fehlende BG-Tiles.

---

## Issue H: Fehlende Schiffswand (Ship Wall) — Swapped VRAM Layouts

**Datum:** 2026-07-01  
**Status:** FIX ANGEWENDET — Wartet auf User-Testing  
**Betroffene Level:** Rambi Rumble (ppuConfig 0x03), potenziell Glimmer's Galleon (ppuConfig 0x28)

**WICHTIG: Level-ID-Korrektur (2026-07-01):**  
Die ursprüngliche Analyse bezog sich auf "Lockjaw's Locker (ppuConfig 0x03)" — das war FALSCH.
- ppuConfig 0x03 ist **Rambi Rumble** (Level ID 0x02), NICHT Lockjaw's Locker
- Die echte **Lockjaw's Locker** (Level ID 0x15) hat **ppuConfig 0x02** (Standard-Layout)
- Für Lockjaw's Locker funktioniert der BG2-Rendering-Pfad über bg2TmLoaded bereits korrekt
- Die BG1-Rendering-Fixes (H.2) sind trotzdem korrekt — sie lösen das Problem für Rambi Rumble

### Problem

Im Viewer fehlt die Schiffswand mit Laternen/Fackeln, die im Spiel als Hintergrund der Ship-Hold-Level sichtbar ist. Der Export zeigt nur den dynamischen Terrain-Layer (Plattformen), aber nicht den statischen Hintergrund.

### Root-Cause-Analyse

**1. loadLevelBackground() renderte BG1 nie (vor Fix):**  
Die Funktion renderte nur BG2 und BG3 als Hintergrund-Layer. Für ppuConfig 0x03 (Rambi Rumble) liegt die Schiffswand/Scenery aber auf BG1 (statischer Hintergrund), während BG2 der dynamische Terrain-Layer ist. Das VRAM-Layout ist gegenüber dem Standard vertauscht:

| BG | chrBase | tilemapBase | Rolle (ppuConfig 0x03 = Rambi Rumble) |
|---|---|---|---|
| BG1 | $5000 | $6C00 | **Statischer Hintergrund (Scenery)** |
| BG2 | $2000 | $7800 | **Dynamisches Terrain** |
| BG3 | $7000 | $6800 | **Statischer Hintergrund** |

**2. DMA-Truncation-Bug:**  
Der Viewer kopiert ALLE dekomprimierten Bytes in VRAM, unabhängig von der DMA-Transfer-Größe (`entry.size`). Auf echter SNES-Hardware überträgt DMA nur `entry.size` Bytes. Overflow in angrenzende VRAM-Regionen verfälscht CHR-Daten, besonders bei gfxSet 0x04 wo Entry 3 ($5300) und Entry 4 ($2000) kontingent sind.

**3. loadTileParts() benutzt falschen chrBase:**  
Für ppuConfig 0x03 ist BG2 der Terrain-Layer (chrBase=$2000), aber `loadTileParts()` benutzt immer `ppu.bg1.chrBase` ($5000). Map32 Tile-Indices referenzieren Tiles relativ zum Terrain-BG chrBase → falsche CHR-Daten.

### Tilemap-Loading-Heuristik

Zuverlässige Methode um statische BGs von dynamischem Terrain zu unterscheiden:
- **gfxSet-Entry schreibt an tilemapBase** → statischer Hintergrund (Tilemap aus ROM geladen)
- **Kein Entry schreibt an tilemapBase** → dynamisches Terrain (Tilemap wird zur Laufzeit vom Scroll-Engine gefüllt)

Verifikation über bekannte Configs:

| Level | ppuConfig | BG1 tm geladen? | BG2 tm geladen? | BG3 tm geladen? |
|---|---|---|---|---|
| Lockjaw's Locker | 0x03 | **JA** ($6C00) | NEIN ($7800) | JA ($6800) |
| Lava Lagoon | 0x02 | NEIN ($3800) | JA ($7000) | JA ($6C00) |
| Glimmer's Galleon | 0x28 | NEIN ($3800) | JA ($7000) | NEIN ($6C00) |

### Angewendete Fixes (index.html)

**Fix H.1 — DMA Truncation in loadLevelBackground() (~line 1452):**
```javascript
if (data.length > entry.size) {
    data = data.slice(0, entry.size);
}
```

**Fix H.2 — Tilemap-Erkennung und BG1-Rendering in loadLevelBackground() (~line 1475-1571):**
- `bg1TmLoaded` / `bg2TmLoaded` / `bg3TmLoaded` Flags basierend auf Tilemap-Heuristik
- `primaryBg` Auswahl bevorzugt BGs mit geladenen Tilemaps
- BG3/BG2 Rendering nur wenn Tilemap geladen (`bg3TmLoaded`, `bg2TmLoaded`)
- Neuer BG1-Rendering-Block für statische Hintergründe

**Fix H.3 — DMA Truncation in loadTileParts() (~line 2140):**
Gleiche Truncation wie in loadLevelBackground(), verhindert VRAM-Overflow.

**Fix H.4 — DMA Truncation in loadOverworldLevel() NPC shops (~line 1697):**
Gleiche Truncation für NPC-Shop VRAM-Loading.

**Fix H.5 — Terrain chrBase Fix in loadTileParts() (~line 2119-2171):**
Verwendet Tilemap-Heuristik um den korrekten Terrain-BG zu ermitteln:
```javascript
const bg1TmLoaded = entries.some(e => e.vram === ppu.bg1.tilemapBase);
const terrainBg = bg1TmLoaded ? ppu.bg2 : ppu.bg1;
const terrainChrBase = terrainBg.chrBase;
```
Alle Referenzen (`gfxSize`-Tracking, `gfxData`-Extraktion, `injectAnimatedTiles`) verwenden jetzt `terrainChrBase` statt `bg1ChrBase`.

### Offene Punkte

- **User-Testing nötig:** Rambi Rumble mit ppuConfig 0x03 prüfen (BG1 als Hintergrund), Terrain-Tiles verifizieren
- **User-Testing nötig:** Rambi Rumble HD-Pack exportieren → in Mesen laden → Terrain-Tile-Hashes verifizieren (terrainChrBase-Fix validieren)
- **Lockjaw's Locker (ppuConfig 0x02):** BG2-Schiffswand sollte bereits korrekt angezeigt werden (bg2TmLoaded greift, verifiziert 2026-07-01)
- ~~**HD-Pack-Export:** Die Export-Funktionen (lines 6817, 6894, 7371, 7779, 8465) verwenden noch `bg1ChrBase` für Tile-Hash-Berechnung → müssen ebenfalls den terrainBg-Fix bekommen~~ **ERLEDIGT (2026-07-02):** `terrainChrBase` in allen 6 Export-Stellen eingebaut (lines ~6903, 7380, 7788, 7898, 8478 + ppuConfig-Konstruktion an 3 Stellen). Siehe Fix H.6 unten.
- **Glimmer's Galleon (ppuConfig 0x28):** BG3 Tilemap wird NICHT von gfxSet geladen. Die Schiffswand könnte auf BG2 liegen (das IST geladen) → Viewer-Export prüfen
- **BG2 Fill-Tile Export:** Niedrigere Priorität, HDMA-Parallax-Regionen werden falsch exportiert

### Fix H.6 — terrainChrBase in HD-Pack-Export-Funktionen (2026-07-02)

**Problem:** Alle Export- und Hash-Funktionen verwendeten `bg1ChrBase` zur Berechnung der
VRAM-Adressen für Terrain-Tile-Arrangements. Bei geschwappten Layouts (ppuConfig 0x03,
Rambi Rumble: Terrain auf BG2) war `bg1ChrBase` FALSCH → exportierte Hashes stimmten
nicht mit Mesens Runtime-Hashes überein → 0% Match.

**Fix:** `terrainChrBase` wird jetzt durch die gesamte Datenpipeline propagiert:
- `loadLevelBackground()` gibt `bg1TmLoaded` zurück (line ~1639)
- `loadTileParts()` gibt `terrainChrBase` zurück (line ~2228)
- `currentTileRawData` enthält `terrainChrBase` (line ~4922)
- Alle ppuConfig-Konstruktionen enthalten `terrainChrBase` (3 Stellen)
- 6 Export/Hash-Code-Stellen verwenden `terrainChrBase` (mit `bg1ChrBase`-Fallback)

**Betroffene Funktionen:**
- `saveCurrentHDToContainer()` — Terrain-Checksum (line ~6903)
- `exportAsTexturePack()` — chrBase für Hash-Berechnung (line ~7380)
- Hash-Computation — storedChrBase (line ~7788)
- VRAM-Fingerprint-Diagnostik (line ~7898)
- `refreshCurrentHDInContainer()` — Refresh-Checksum (line ~8478)

**Zusätzlich korrigiert:**
- Irreführende Code-Kommentare: "Lockjaw's Locker" → "Rambi Rumble" für ppuConfig 0x03 (lines ~1565, 2121)
- `read_gfxset.ps1` line 26: falsche Level-ID 0x02 → 0x15 für Lockjaw's Locker

---

## Disassembly-Analyse: Level-Init und VRAM-Loading (2026-07-01)

### VRAM_payload_handler_global ($BB80B0)

Benutzt **exakt die gleiche Tabelle** wie der Viewer (`DATA_FD819A` = ROM 0x3D819A).
Nimmt eine Payload-ID als Index, liest 7-Byte-Einträge (Bank, Addr, VRAM-Ziel, DMA-Größe),
dekomprimiert/kopiert die Daten nach $7F:0000, DMA'd sie zum VRAM. Mehrere Chunks bis Terminator (Bank=0).

**Kritische Erkenntnis:** Der Viewer's `readGfxSetEntries()` implementiert exakt die gleiche
Logik. Es gibt KEINE versteckte zweite Datenquelle.

### DATA_BB9592 — Level-Init Handler-Tabelle (21 Einträge)

Handler 0x0011 (`CODE_BB95F2`) — benutzt von Lockjaw, Lava Lagoon, Mainbrace, Rambi Rumble:
```asm
LDA #$0001                      ; VRAM Payload #1 (gemeinsame Sprite-Tiles)
JSL VRAM_payload_handler_global
LDA $0539                       ; Level-spezifischer gfxSet-Index
JSL VRAM_payload_handler_global
JSR CODE_BB94B6                  ; Palette/Misc Init
LDA $0537                       ; PPU Register Config ID
JSL set_PPU_registers_global
```

### VRAM Payload #1 (Common/Shared)

Nur **ein** Eintrag: 1664 Bytes raw data nach VRAM word 0x1CC0 (= OBJ Sprite-Tiles 204-255
der zweiten Sprite-Nametable). Enthält gemeinsame Sprites (DK-Barrel, Bananen etc.).
**Keine Hintergrunddaten!**

### $0537 und $0539 Herkunft

Diese RAM-Adressen werden in `bank_BB.asm` nur GELESEN, nie geschrieben.
Sie werden während der Style-Data-Parsing (CODE_BBAF0F) gesetzt:
- $0537 = ppuConfig (Style Data Offset 12)
- $0539 = gfxSet (Style Data Offset 13)

### BG2 Tilemap-Verifikation (2026-07-01)

VRAM-Dump-Analyse von Lava Lagoon (ppuConfig 0x02, gfxSet 3) bestätigt:
- BG2 Tilemap (word 0x7000-0x77FF): **1456 von 2048 Einträgen non-zero**
- Klares Schiffswand-Muster: sequenzielle Tile-Indices 0x001-0x0CB, symmetrische Struktur
- BG2 CHR (word 0x4000): vollständig geladen (21632 Bytes, 676 Tiles)
- Die gfxSet-3 Entries 1+2 laden die BG2 Tilemap korrekt nach word 0x7000/0x7400
- bg2TmLoaded-Check: `entries.some(e => e.vram === 0x7000)` → TRUE (verifiziert per ROM-Readout)
- BG2 Rendering-Pfad in loadLevelBackground() wird korrekt ausgeführt

**~~"Unknown" gfxSet 3 Entries ($7800, $7C00):~~ KORRIGIERT (siehe unten)** — Sind NICHT Scratch-Daten,
sondern die **Schiffswand-Tilemaps** (BG2 Screens 2+3). Siehe Abschnitt "Schiffswand-Analyse".

### Korrekte Level-ID-Zuordnungen (VERIFIED)

| Level ID | Level Name | ppuConfig | gfxSet | vblankType |
|---|---|---|---|---|
| 0x00 | Mainbrace Mayhem | 0x01 | 0x02 | 0x01 |
| 0x01 | Glimmer's Galleon | 0x28 | 0x28 | 0x12 |
| 0x02 | Rambi Rumble | 0x03 | 0x04 | 0x03 |
| 0x14 | Lava Lagoon | 0x02 | 0x03 | 0x18 |
| 0x15 | Lockjaw's Locker | 0x02 | 0x03 | 0x02 |

---

## Schiffswand-Analyse und Fix (2026-07-01 / 2026-07-02)

### Problem

In Schiffs-Levels (Lockjaw's Locker, Lava Lagoon, Glimmer's Galleon) war die Schiffswand
im Viewer unsichtbar. Die BG2-Schiffsdecke und BG3-Wasser wurden korrekt dargestellt,
aber der untere Bereich (Holzplanken-Wand) fehlte komplett.

### Root-Cause-Analyse

**VRAM-Dump-Analyse** (gfxset_03_vram.bin + gfxset_28_vram.bin) ergab:

Die gfxSet-Entries 3+4 laden Tilemap-Daten nach VRAM-Adressen **außerhalb** des normalen
BG2 64×32 Tilemap-Bereichs:

| Entry | VRAM Word | Inhalt | Größe |
|---|---|---|---|
| 3 | 0x7800 | Wall Tilemap Screen 2 (links) | 768 Bytes = 12×32 Tiles |
| 4 | 0x7C00 | Wall Tilemap Screen 3 (rechts) | 768 Bytes = 12×32 Tiles |

Diese Adressen liegen bei word 0x7800-0x7FFF, also Screens 2+3 einer konzeptuellen 64×64 Tilemap.
Die PPU-Register zeigen `doubleHeight=false` (64×32), aber das Spiel nutzt **HDMA** um
BG2 vscroll mid-frame zwischen Decke und Wand umzuschalten.

**BG2 CHR enthält DREI Tile-Gruppen (676 Tiles gesamt):**

| Tile-Indices | Zweck |
|---|---|
| 1-577 (577 Tiles) | Schiffsdecke (BG2 Tilemap Rows 0-23) |
| 578-652 (75 Tiles) | Schiffswand (referenziert von Entries 3+4) |
| 653-675 (23 Tiles) | Übergangsstreifen Decke→Wand (Rows 24-27) |

**Cross-Validation:** gfxset_03 (Lava Lagoon) und gfxset_28 (Glimmer's Galleon) haben
byte-identische Wall-Tilemap-Daten.

### Implementierung (5 Fixes)

**Fix 1 — Wall-Erkennung in `loadLevelBackground()` (~Zeile 1480)**
- Erkennt Wall-Tilemap-Entries an Adresse `bg2.tilemapBase + 0x800` / `+ 0xC00`
- Rendert Wall als separates `wallData` Image mit synthetischer bgConfig
- Rückgabeobjekt enthält jetzt `wallData`

**Fix 2 — `buildCatalog()` (~Zeile 5455)**
- Erstellt `wallImage` Canvas aus `currentBgData.wallData`
- Return-Objekt enthält `wallImage`

**Fix 3 — Katalog-Anzeige (~Zeile 5611)**
- Rendert Wall-Sektion nach BG2 mit rotem Rahmen (#e57373) und Label "BG2 Wall (Schiffswand)"

**Fix 4 — `buildCatalogByGfxSet()` Return (~Zeile 4411)**
- `wallImage: null` zum Return hinzugefügt für Konsistenz

**Fix 5 — `buildBgImageFromCurrentLevel()` (~Zeile 5297)**
- Unterstützt jetzt `layer === 'wall'` als Parameter
- Nutzt `currentBgData.wallData` statt `ppu[layer]`

**Fix 6 — ZIP-Export (~Zeile 6050)**
- Wall wird automatisch als `bg2_wall.png` exportiert wenn BG2-Checkbox aktiviert
- `manifest.bg2_wall` im Manifest

### Status

- Viewer-Fixes: implementiert, **Test ausstehend** (Browser-Test mit Lockjaw/Lava Lagoon)
- HD-Pack-Export: Wall-PNG wird exportiert, **Hash-Integration ABGESCHLOSSEN (2026-07-02)**
  - Wall-Hashes in hashes.bin (Layer 1, gleicher chrBase wie BG2-Decke)
  - Wall per-tile PNGs in bg/bg2/gfxset_XX/ (32×32px, 4× skaliert, un-flipped)
  - Container-Pipeline komplett: save, load, ZIP export/import, refresh
- HDMA-basierte Darstellung in Mesen (mid-frame vscroll) ist eine eigene Aufgabe

### Pipeline-Integration Details (2026-07-02)

**Batch 4 (importContainerFromZip):** wallTilemapData aus wall_tilemap.bin geladen,
wallBlob + wallTilemapData in hdSaveSet()-Aufruf eingefügt.

**Batch 5 (refreshContainerSetMetadata):** wallTilemapData Refresh aus VRAM mit
ppuConfig.wallTilemapBase, wallBlob + wallTilemapData + Wall-Checksums in hdSaveSet().

**Batch 6 (Hash-Generierung):** Wall-Hash-Loop nach BG2-Hashes in exportAsTexturePack().
Layer 1, gleicher chrBase, tileNum===0 Skip, Dedup-Key ${gfxset}_1_${vramWordAddr}.

**Batch 7 (Per-Tile PNG Export):** Wall-Tiles aus wallBlob extrahiert, in gleichen
bg/bg2/gfxset_XX/ Ordner wie Decken-Tiles. Keine Filename-Kollision (verschiedene
vramWordAddrs wegen verschiedener Tile-Indices 578-652 vs 1-577).

**User-Test (2026-07-02):** Wall-Pipeline getestet — Lockjaw + Lava Lagoon zeigen
beide BG2-Bilder (Decke + Wand) korrekt im Viewer.

---

## GfxSet-Katalog Palette-Bug (2026-07-02)

### Symptom

GfxSet-Ansicht für gfxSet 0x03 zeigte immer Lava Lagoons rötlichen Farbfilter,
auch wenn Lockjaw's Locker geladen war. Rein visueller Viewer-Bug — exportierte
Tiles im ZIP waren korrekt (ohne Rot-Filter).

### Root Cause

`buildCatalogByGfxSet()` Zeile 4257 nahm immer `setInfo.levels[0]` als Referenz-Level.
`scanGraphicsSets()` iteriert Level-IDs aufsteigend (0-191), daher wird für gfxSet 0x03
Lava Lagoon (ID 0x14) vor Lockjaw's Locker (ID 0x15) eingefügt → `levels[0]` = Lava Lagoon.

Lava Lagoons `routine1=0x14` triggert in `loadTileParts()` Zeile 2237-2242 eine **komplette
Palette-Ersetzung** aus ROM 0x3D1610 (rötliche Palette). Die CHR-Grafikdaten sind zwar
identisch für alle Levels im gleichen gfxSet, aber Palette (routine1), ppuConfig (Color Math),
und vblankType (animierte Tiles) sind **per-Level Eigenschaften**.

### Fix

1. **`buildCatalogByGfxSet()`:** Wenn `currentLevelId` im selben gfxSet enthalten ist,
   wird dessen Level als Referenz benutzt statt `levels[0]`.
2. **`toggleCatalogView()`:** Cache-Invalidierung erweitert — prüft jetzt auch
   `catalogData.refLevelId !== currentLevelId`.
3. **catalogData Return:** Neues Feld `refLevelId` für Cache-Vergleich.

### Status

**GETESTET UND BESTÄTIGT** — User hat Lockjaw/Lava Lagoon Wechsel geprüft, sieht sauber aus.

---

## Prozess-Regeln

### Vor jedem neuen Fix/Test:
1. Dieses Journal lesen
2. Prüfen ob Hypothese bereits getestet wurde (Versuchs-Log)
3. Prüfen ob Ursache bereits ausgeschlossen wurde (Ruled Out)
4. Klaren Test-Plan formulieren mit erwartetem Ergebnis

### Nach jedem Test:
1. Ergebnis in Versuchs-Log eintragen
2. Bei definitiver Erkenntnis → Ruled Out oder Fix-Sektion aktualisieren
3. Status der Issues aktualisieren

### Diagnose-Workflow:
1. Mesen-Build starten, Level laden
2. `snes_hd_diag.txt` aus Downloads lesen
3. Key-Metriken extrahieren: match/miss/palMis/notInPack Ratio
4. Spezifische MISS-Hashes gegen hashes.bin prüfen
5. Ergebnis dokumentieren BEVOR nächster Fix implementiert wird

---

## BG3 Foreground-Architektur — Forschung (2026-07-02)

### Kontext

BG3-Vordergrund-Elemente (Seile, Masten, Dekor) in Pirate Panic sind im Viewer unsichtbar.
Die Analyse ergab, dass dies KEIN Bug ist — die BG3-Tilemap für ShipDeck-Level wird
zur Laufzeit dynamisch vom Scroll-Engine befüllt und ist nach gfxSet-Loading tatsächlich leer.

### Forschungsmethodik

Umfangreiche Analyse folgender Quellen:
- p4plus2 DKC2-Disassembly: `bank_B5.asm` (~330KB), `bank_80.asm` (~380KB), `vram.asm`, `structs.asm`
- Lokale `DKC2_Routine_Macros.asm` (175K Zeilen)
- Cross-Referenzierung von RAM-Adressen, Dispatch-Tabellen, Tilemap-Tabellen

### Ergebnisse

Alle Forschungsergebnisse wurden in `SNES_HD_PACK_PROJECT.md` → Abschnitt "BG3 Foreground-
Architektur (Forschung 2026-07-02)" dokumentiert. Hier die Kurzfassung:

**Root Cause:** BG3 ShipDeck-Tilemap wird dynamisch vom Scroll-Engine aus `DATA_F52087`
(Layer3_ShipDeck.bin, unkomprimiert, Bank $F5) spaltenweise gestreamt. Die gfxSet-DMA
lädt nur CHR-Daten (Tile-Grafiken), NICHT die Tilemap.

**Zwei Mechanismen:**
- Statisch (Forest, Water, etc.): Tilemap via gfxSet DMA direkt ins VRAM
- Dynamisch (ShipDeck, evtl. andere): Tilemap spaltenweise vom Scroll-Engine gestreamt

**Key ROM/Code Referenzen:**
- `DATA_F52087` — Layer3_ShipDeck.bin Tilemap-Daten
- `CODE_B5A95B` — Horizontales Scroll/Streaming
- `CODE_B5AB0B` — Vertikales Scroll/Streaming
- `CODE_B5BCA8` — Zentrales Map/Tileset-Loading (nimmt Map Type ID)
- 8 parallele Tabellen bei `DATA_B5BAEF`..`DATA_B5BC7E` (indiziert durch Map Type $0523)
- 15 Layer-3-Tilemap-Dateien im ROM identifiziert

**Widerlegte Hypothesen:**
- $0046/$0048 sind KEINE VRAM-Adressen → Sprite-Feld-Offsets in der Sprite-Config-Engine

### Nächste Schritte

1. ~~Deep-Dive: Scroll-Engine Streaming-Format von `CODE_B5A95B` / `CODE_B5AB0B` verstehen~~ **DONE**
2. ~~Datenformat von `DATA_F52087` entschlüsseln (Metatile-Struktur → 8×8 Tiles)~~ **DONE**
3. ~~`loadBg3ForegroundMap()` im Viewer implementieren~~ **DONE** (als `loadBg3ShipDeckMap()`)
4. Komprimierte Layer-3-Tilemaps für andere Level-Typen (Rare-Kompression)

---

## BG3 ShipDeck Viewer-Implementation (2026-07-02)

### Änderungen in index.html

**Neue Funktion: `loadBg3ShipDeckMap()` (~Zeile 1427-1565)**
- Liest Metatile-Definitionen aus ROM offset `0x352087` (DATA_F52087, 49 × 32 Bytes)
- Liest Metatile-Map aus ROM offset `0x3526A7` (DATA_F526A7, 40 × 16 Words)
- Expandiert 4×4 Metatiles zu individuellen 8×8 SNES-Tiles
- Rendert 2bpp Tiles mit korrekter Palette (identische Logik wie `renderBgLayer()`)
- Metatile-Flip-Transforms: H-flip, V-flip, HV-flip (reversed Sub-Tile-Reihenfolge + XOR $4000/$8000)
- Output: 1280×512px ImageData (40 Rows=horizontal × 16 Cols=vertikal)
- Parameter `priorityFilter`: null=alle, 0=nur pri-0, 1=nur pri-1

**Hook in `loadLevelBackground()` (~Zeile 1616-1622):**
- Erkennung: `style.mapId === 3` → ShipDeck-Level
- Aufruf mit `priorityFilter=null` (alle Tiles)
- Ergebnis in `bg3ShipDeckData` gespeichert

**BG3 Empty-Check Erweiterung (~Zeile 1767-1779):**
- Neue Priorität: wenn ShipDeck-Daten vorhanden UND `bg3TmNonZero === 0` → fgData aus Metatile-Map
- `fgData.mode = 'shipdeck'`, `colorMathAlpha = 1.0`
- Sonst: Fallback auf bestehende Empty-Check-Logik

**Return-Value erweitert:**
- `bg3ShipDeckData` als neues Feld im Return-Objekt

**buildCatalog() BG3-Sektion (~Zeile 5631-5656):**
- Prüft `currentBgData.bg3ShipDeckData` vor VRAM-basiertem Rendering
- ShipDeck: Verwendet Metatile-Image für bg3Image-Canvas

**buildBgImageFromCurrentLevel() (~Zeile 5494-5499):**
- Neuer Early-Return für `layer === 'bg3'` + ShipDeck-Daten vorhanden

### ROM-Adressen (HiROM, ohne SMC-Header)

| Daten | Adresse | Größe | SNES-Label |
|-------|---------|-------|------------|
| Metatile-Definitionen | `0x352087` | 0x620 (1568B) | DATA_F52087 |
| Metatile-Map | `0x3526A7` | 0x500 (1280B) | DATA_F526A7 |
| BG3 CHR (Grafik) | `0x352BA7` | variabel | DATA_F52BA7 |

### Erwartetes Console-Log bei Pirate Panic

```
BG3 ShipDeck: X metatiles, Y tiles rendered (1280x512px, pri0=N, pri1=M, filter=all)
BG3 foreground: ShipDeck metatile map (1280x512px, Y tiles, pri0=N, pri1=M)
```

### Offene Punkte

- Priority-Trennung: Aktuell werden ALLE Tiles als Foreground-Overlay gerendert.
  Falls pri-0 Tiles (Background) vorhanden sind, könnten diese fälschlicherweise
  VOR dem Terrain erscheinen statt dahinter. → Test zeigt ob relevant.
- HD Pack Export: ShipDeck BG3 Tiles sind noch nicht in der Hash-/Export-Pipeline.
- Andere Level-Typen: Komprimierte Layer-3-Tilemaps (Forest, Water, etc.) noch nicht unterstützt.

---

## Issue I: Rambi Rumble BG3 — Honig opak, Bienenstock verdeckt (Sub-Screen-Blend)

**Datum:** 2026-07-02  
**Status:** v3 BESTÄTIGT (2026-07-03) — Layer-Reihenfolge korrekt, Feintuning ausstehend  
**Betroffene Level:** Rambi Rumble (Level 0x02, ppuConfig 0x03, gfxSet 0x04)  
**Betrifft:** Viewer `index.html` — `loadLevelBackground()` BG3 Foreground-Detection

### Symptom

- **Erwartet:** Bienenstock-Muster (BG1) als Hintergrund sichtbar, Honig-Tropfen (BG3) als semi-transparentes Foreground-Overlay darüber
- **Tatsächlich:** Bienenstock VERDECKT, Honig als voll-opakes Foreground-Overlay über dem gesamten Canvas

### Verifizierte ROM-Daten (via PowerShell ROM-Trace)

**PPU Config 0x03:**
| Register | Wert | Bedeutung |
|----------|------|-----------|
| $2105 | 0x09 | Mode 1, BG3 Priority Bit gesetzt |
| BG1 | tm=0x6C00, chr=0x5000 | 32×32 tiles, Bienenstock-Muster |
| BG2 | tm=0x7800, chr=0x2000 | 64×32 tiles, Terrain (dynamisch) |
| BG3 | tm=0x6800, chr=0x7000 | 32×32 tiles, Honig-Overlay |
| mainScreen | 0x01 | **Nur BG1** auf Main Screen |
| subScreen | 0x16 | BG2 + BG3 + OBJ auf Sub Screen |
| cgwsel | 0x02 | useSubScreen=true |
| cgadsub | 0x21 | Color Math: BG1 + Backdrop, ADD, full |

**GfxSet 0x04 DMA Entries:**
| # | VRAM | Ziel | Größe |
|---|------|------|-------|
| 0 | 0x6800 | BG3 Tilemap | 2048 (compressed) |
| 1 | 0x6C00 | BG1 Tilemap | 2048 (compressed) |
| 2 | 0x7000 | BG3 CHR | 4096 (compressed) |
| 3 | 0x5300 | BG1 CHR (ab Tile 48) | 10752 (compressed) |
| 4 | 0x2000 | BG2 CHR | 26112 (compressed) |

**Viewer Detection Ergebnisse:**
- `bg1TmLoaded = true` (Entry 1 → 0x6C00)
- `bg2TmLoaded = false` (kein Entry bei 0x7800 — Terrain dynamisch)
- `bg3TmLoaded = true` (Entry 0 → 0x6800)

### SNES Hardware-Verhalten (ppuConfig 0x03)

Dies ist ein **Sub-Screen-Blend-Muster** — grundlegend anders als der Mainbrace-Nebel:

1. **Main Screen:** Nur BG1 (Bienenstock) wird angezeigt
2. **Sub Screen:** BG2 (Terrain) + BG3 (Honig) sind die Blend-Quellen
3. **Color Math:** `finalPixel = BG1_pixel + SubScreen_pixel` (ADD, full intensity)
4. **Effekt:** Der Honig (BG3 auf Sub Screen) wird additiv in den Bienenstock (BG1 auf Main Screen) eingeblendet

**Vergleich der zwei Color-Math-Muster:**

| Eigenschaft | Mainbrace-Nebel | Rambi-Rumble-Honig |
|-------------|-----------------|---------------------|
| BG3 auf | Main Screen | **Sub Screen** |
| Color Math Target | BG3 (subtract) | **BG1 (add)** |
| useSubScreen | false (fixed color) | **true** |
| Viewer-Approximation | BG3 als halbtransparentes Overlay | BG3 als additives Sub-Screen-Overlay |

### Root-Cause: 2 Bugs in loadLevelBackground()

**Bug 1: `hasPri1` blockiert Color Math (index.html:1837)**

```javascript
// Line 1829-1840:
const fgImgPri1 = new ImageData(fgW, fgH);
renderBgLayer(vram, ppu.bg3, palette, false, fgImgPri1, fgW, fgH, 1);
// ...
if (hasPri1) {
    // OPAQUE foreground (alpha=1.0) — VERDECKT den Bienenstock!
    fgData = { imgData: fgImgPri1, ..., colorMathAlpha: 1.0, mode: 'priority' };
}
```

Wenn BG3 Priority-1 Tiles hat (Honig-Tropfen mit Mode 1 BG3 Priority), werden diese als voll-opakes Foreground-Overlay gerendert. Die Color-Math-Analyse in der `else`-Branch wird **nie erreicht**.

**Bug 2: Color Math prüft nur Main Screen (index.html:1868)**

```javascript
// Line 1864-1868:
const bg3OnMain = !!(ppu.mainScreen & 0x04);  // = false! BG3 ist auf SUB Screen
// ...
if (bg3OnMain && hasColorMath) { ... }  // SCHLÄGT FEHL — bg3OnMain=false
```

Selbst wenn `hasPri1=false` wäre: Die Color-Math-Detection prüft `bg3OnMain`, aber BG3 ist auf dem **Sub Screen** (0x16 & 0x04 = true). Der Check `bg3OnMain` ist hier der falsche Test.

### Fix-Strategie (3 Änderungen in loadLevelBackground)

**1. `isSubScreenBlend` Flag berechnen (nach Zeile 1718):**
```javascript
const bg3OnSub = !!(ppu.subScreen & 0x04);
const bg3OnMain = !!(ppu.mainScreen & 0x04);
const isSubScreenBlend = bg3OnSub && !bg3OnMain && ppu.colorMath.useSubScreen 
                         && (ppu.colorMath.bg1 || ppu.colorMath.backdrop);
```

**2. BG3 pri-0 Background gaten (Zeile 1797):**
```javascript
if (bg3TmLoaded && ppu.bg3.enabled && !isSubScreenBlend) {
```
BG3 darf NICHT als Hintergrund-Layer hinter BG1 gerendert werden, wenn es ein Sub-Screen-Blend ist.

**3. Neue Foreground-Branch VOR pri-1 Check (Zeile 1823+):**
Wenn `isSubScreenBlend` aktiv: alle BG3 Tiles (pri-0 + pri-1) als semi-transparentes Overlay rendern, mit `mode='colormath'` und passendem Alpha.

### Level-Scan (2026-07-02): Sub-Screen-Blend ist weit verbreitet!

**ppuConfig 0x03:** 13 Levels (alle gfxSet=0x04)
- mapId=0x02 (BeeHive): Level 0x02, 0x11, 0x12, 0x26, 0xAE-0xB2
- mapId=0x07 (IceCave): Level 0x13, 0x60, 0xB3-0xB4

**Sub-Screen-Blend insgesamt: 35 Levels, 6 ppuConfig-Werte**

| ppuCfg | mainScreen | subScreen | cgadsub | CM-Target | Levels |
|--------|-----------|-----------|---------|-----------|--------|
| 0x03 | 0x01 (BG1) | 0x16 (BG2+BG3+OBJ) | 0x21 | BG1+backdrop ADD full | 13 |
| 0x24 | 0x13 (BG1+BG2+OBJ) | 0x04 (BG3) | 0x22 | BG2+backdrop ADD full | 10+ |
| 0x29 | 0x13 | 0x04 (BG3) | 0x43 | BG1+BG2 ADD **half** | 2 |
| 0x2C | 0x02 (BG2) | 0x15 (BG1+BG3+OBJ) | 0x22 | BG2+backdrop ADD full | 3 |
| 0x31 | 0x13 | 0x14 (BG3+OBJ) | 0x01 | BG1 ADD full | 4 |
| 0x35 | 0x01 (BG1) | 0x16 | 0x31 | BG1+OBJ+backdrop ADD full | 3 |

**Erkennung (generisch):**
```javascript
const isSubScreenBlend = bg3OnSub && !bg3OnMain && ppu.colorMath.useSubScreen
    && (ppu.colorMath.bg1 || ppu.colorMath.bg2 || ppu.colorMath.backdrop);
```
Deckt alle 6 ppuConfig-Werte ab. Die Fix-Strategie bleibt gleich (3 Änderungen), 
ist aber jetzt verifiziert generisch statt nur für ppuConfig 0x03.

### Alpha-Wert-Strategie
- `half` in cgadsub (z.B. ppuCfg 0x29) → alpha=0.5
- `full` (kein half-bit) → alpha=0.7 als Approximation (echtes additives Blending wäre `globalCompositeOperation='lighter'`, aber die bestehende Pipeline nutzt Alpha)
- `subtract` → `globalCompositeOperation='difference'` (bestehend)

### Implementierung (2026-07-02)

Alle 3 Änderungen in `loadLevelBackground()` umgesetzt:

**Change 1 — `isSubScreenBlend` Flag (Zeile 1720-1731):**
```javascript
const bg3OnSub = !!(ppu.subScreen & 0x04);
const bg3NotOnMain = !(ppu.mainScreen & 0x04);
const isSubScreenBlend = ppu.bg3.enabled && bg3OnSub && bg3NotOnMain
    && ppu.colorMath.useSubScreen
    && (ppu.colorMath.bg1 || ppu.colorMath.bg2 || ppu.colorMath.backdrop);
```

**Change 2 — BG3 Background gated (Zeile 1811):**
```javascript
if (bg3TmLoaded && ppu.bg3.enabled && !isSubScreenBlend) {
```
Verhindert, dass BG3 als separater pri-0-Hintergrund unter BG1 gerendert wird.

**Change 3 — Per-Pixel Additive Blend (Zeile 1843-1876):**

**Erster Versuch (v1):** BG3 als semi-transparentes fgData-Overlay (alpha=0.7).
**Problem:** fgData wird in `renderLevel()` ÜBER dem Terrain getiled (Zeile 2769-2793).
Auf der SNES passiert der Sub-Screen-Blend aber HINTER dem Terrain (BG3 auf Sub Screen
wird additiv in BG1 auf Main Screen eingeblendet, beides hinter BG2/Terrain).
**Resultat:** Elemente visuell vertauscht — BG3 lag im Vordergrund über dem Terrain.

**Zweiter Versuch (v2):** Per-Pixel additives Compositing direkt in imgData.
BG3 wird in temporäres ImageData gerendert, dann pixelweise in imgData (das BG1 enthält) 
eingeblendet: `pixel = BG1 + BG3 * factor`. Factor=1.0 (full) oder 0.5 (half, ppuCfg 0x29).
Kein separates fgData — das kombinierte BG1+BG3 Bild liegt korrekt hinter dem Terrain.
**Problem:** User-Test zeigt: BEIDE Elemente sind jetzt hinter den Terrain-Tiles verborgen.
Die Honig-Effekte (BG1) sollen aber als Foreground-Overlay ÜBER dem Terrain sichtbar sein.

**Dritter Versuch (v3 — aktuell):** Layer-Swap.
Aus v1-Feedback identifiziert: BG3 = Bienenwaben (Hintergrund), BG1 = Honig (Vordergrund).
- BG3 wird als Hintergrund auf imgData gerendert (alle Priorities, Zeile 1811-1815)
- BG1 wird NICHT auf imgData gerendert (gated mit `!isSubScreenBlend`, Zeile 1834)
- BG1 wird als semi-transparentes fgData-Overlay gerendert (alpha=0.7/0.5, Zeile 1851-1876)
- fgData wird in renderLevel() über dem Terrain getiled → Honig-Overlay über dem Level

**Noch zu testen:**
- Rambi Rumble (Level 0x02, ppuCfg 0x03): Bienenwaben als Hintergrund, Honig als Overlay
- Andere ppuCfg-Werte (0x24, 0x29, 0x2C, 0x31, 0x35): korrekte Erkennung + Layer-Zuordnung
- Keine Regression bei Main-Screen-ColorMath Leveln (Mainbrace Mayhem Nebel etc.)
- Half-Intensity Levels (ppuCfg 0x29): alpha=0.5 Blending korrekt?

### v3 Test-Ergebnis (2026-07-03)

**BESTÄTIGT — Layer-Reihenfolge korrekt:**
- Honigwaben (BG3) als Hintergrund hinter dem Terrain ✓
- BG1 in der Mitte ✓
- Honig-Effekt (BG1) als Overlay ganz vorne ✓

**Verbleibende Issues (nächste Session):**

1. **Honig-Overlay zu dunkel:** Alpha=0.7 ist noch zu intensiv/dunkel, muss transparenter
   werden. Nächster Schritt: Alpha auf ~0.4-0.5 reduzieren oder `globalCompositeOperation='lighter'`
   (echtes additives Blending) testen.

2. **Catalog-View zeigt kein Foreground-Element:** Im Katalog-View werden nur BG1 und
   BG3-Honigwaben-Hintergrund angezeigt. Es gibt kein separates Fenster/Canvas für das
   BG1-Foreground-Overlay. Nächster Schritt: `buildCatalog()` und `buildCatalogByGfxSet()`
   müssen ein drittes Bild-Feld für das SSB-Foreground-Element erzeugen.

3. **Andere ppuConfig-Werte testen:** 0x24, 0x29, 0x2C, 0x31, 0x35 — Erkennung verifiziert
   (generische `isSubScreenBlend`-Formel deckt alle ab), aber visuelles Ergebnis noch
   nicht pro Level-Typ geprüft. Besonders ppuCfg 0x29 (half-intensity, alpha=0.5).

4. **Regressions-Check:** Nicht-SSB-Level wie Mainbrace Mayhem (Nebel) auf Regression prüfen.

---

# Test-Session 2026-07-05 — Neue Issues (J bis P)

**Build:** M5.14  
**Diagnoselog:** `snes_hd_diag05072026 abends.txt` (35.376 Zeilen)  
**Viewer-Stand:** Commit `a4351fa` (3 neue Commits seit `a105510`)  
**Getestete Level:** Pirate Panic, Mainbrace Mayhem, Gangplank Galleon, Lockjaw's Locker, Hot Head Hop, Rambi Rumble, Gusty Glade

---

## Issue J: BG3 Foreground Bleed-Through — Pirate Panic (2026-07-05)

### Symptom
- BG3 Seile/Takelage werden in HD gerendert
- **ABER:** native BG3-Pixel scheinen durch, wenn Charakter-Sprites auf derselben Y-Position sind
- Effekt: Hinter/um die Spielfigur herum sind kurzzeitig native Low-Res-Pixel sichtbar,
  die das HD-Rendering durchbrechen

### Log-Analyse (sig=1DF33CEAA50F7F08, context [other])
```
Typischer Frame (Zeile 47):
  bg=55074 match=55074 miss=0 notInPack=0 sprWon=2270
  BG1=26994 BG2=55074 BG3=2561 cmDelta=26597

Beobachtung:
  - match=bg → 100% Match-Rate, kein einziger MISS
  - sprWon=2270 → ca. 2270 Pixel von Sprites "gewonnen" (OBJ über BG)
  - cmDelta=26597..3000 → Color-Math-Fade (Einblend-Animation)
  - BG3=2561 → BG3-Tiles sind vorhanden und werden gematched
```

### Vermutete Ursache
Kein Tile-Matching-Problem — alle Tiles matchen zu 100%. Das Problem liegt im **Compositing/Rendering**:
- Der HD-Filter zeichnet BG3 als Background-Layer (Priorität 0)
- Wenn Sprites (OBJ) denselben Bildschirmbereich belegen, gewinnt der Sprite (`sprWon`)
- **ABER:** die SNES-PPU rendert OBJ ÜBER BG1/BG2, nicht über BG3 in allen Fällen
- Der HD-Filter könnte die Layer-Priority falsch auflösen: BG3 wird transparent,
  statt den nativen Pixel zeigt der HD-Filter das raw PPU-Output, und die nativen
  BG3-Pixel bluten durch

### Nächste Schritte
1. `SnesHdVideoFilter.cpp`: Prüfen wie BG3-Pixel in Kombination mit sprWon behandelt werden
2. Testen ob das Bleed-Through nur bei bestimmten BG3-Priorities auftritt
3. Vergleich: natives PPU-Output vs. HD-Output am gleichen Pixel (BG3 + OBJ Overlap)

---

## Issue K: Gangplank Galleon — Sunset-Effekt fehlt (2026-07-05)

### Symptom
- Gangplank Galleon nutzt **gfxset 07** (identisch mit Pirate Panic)
- Im Original hat der Hintergrund einen Sunset/Dämmerungseffekt:
  warme Orange-/Rottöne statt des Standard-Tageslicht-Hintergrunds
- Mit HD-Pack: Die HD-Tiles zeigen den normalen Tageslicht-Hintergrund,
  der Sunset-Farbeffekt fehlt komplett

### Vermutete Ursache
DKC2 erzeugt den Sunset-Effekt über **HDMA-Palette-Manipulation**:
- HDMA-Kanäle schreiben pro VBlank neue Palette-Werte in CGRAM ($2121/$2122)
- Die BG-Tiles haben identische VRAM-Daten wie in Pirate Panic (gleicher gfxset)
- Zur Laufzeit werden die Paletten-Farben "warm" verschoben (mehr Rot, weniger Blau)
- HD-Tiles sind statische PNGs mit der Export-Palette → keine HDMA-Farbanpassung

### Verwandte Issues
- **Issue E** (Hot Head Hop Lava Glow): Ähnliches Problem, gelöst mit cmDelta-Mechanismus.
  cmDelta erfasst die Color-Math-Differenz (pre-math vs. post-math), nicht Palette-Änderungen.
- Der Sunset-Effekt ist eine **Palette-Änderung**, kein Color-Math-Effekt.
  cmDelta greift hier nicht.

### Nächste Schritte
1. Prüfen ob der Sunset-Effekt über $2121/$2122 (Direct CGRAM) oder über $2132 (FixedColor) läuft
2. Falls CGRAM: Runtime-Palette vs. Export-Palette vergleichen, Delta berechnen
3. Neuer Mechanismus nötig: "Palette Delta" analog zu cmDelta, aber basierend auf
   CGRAM-Differenz statt Color-Math-Differenz

---

## Issue L: Mainbrace Mayhem — BG3-Nebel-Interaktion (2026-07-05)

**Status: M5.17 VERIFIZIERT ✓ — M5.18 IMPLEMENTIERT (Fog Contour Fix) — TEST AUSSTEHEND**

### Symptom
Komplementäres Verhalten je nachdem ob BG3-HD-Tiles im Pack sind:

**(a) MIT BG3-HD-Tiles im Pack:**
- HD-Elemente nur dort sichtbar, wo der Nebel NICHT ist
- Wo Nebel ist → HD-Tiles verschwinden, native Pixel erscheinen

**(b) OHNE BG3-Tiles im Container:**
- HD-Elemente sichtbar, ABER native Pixel bluten durch wo der Nebel NICHT ist
- Inverses Problem zu (a)

### Log-Analyse (M5.16, mit BG3-HD-Tiles)

Diagnostik-Log: `snes_hd_diag MM mit BG3 HD tiles.txt` (~3220 Zeilen)

```
Typischer Steady-State Frame (sig 26F9149D83DDEF29):
  bg=55228 match=55228 fogB=0 ovBlend=3914 cmDelta=42699
  BG1=17413 BG2=55228 BG3=49858
  WINNERS: wn0=5370 wn1=0 wn2=49858 wn3=0 | acm0=5370 acm1=0 acm2=49858 acm3=0
```

**Schlüsselerkenntnisse:**
1. **wn0 + wn2 = bg** (5370 + 49858 = 55228): Nur BG1 und BG3 gewinnen jemals. BG2 gewinnt NIE.
2. **acm = wn (100%)**: AllowColorMath auf ALLEN Gewinner-Pixeln — BG1 UND BG3.
3. **BG3 hat höhere Priorität als BG1**: BG1=17413 Tiles, aber nur wn0=5370 gewinnen.
4. **ovBlend=3914**: bg1OverlayBlend feuerte FALSCH — BG1-Terrain als Overlay interpretiert.
5. **fogB=0**: bg3FogBlend nie erreicht (BG3-HD-Tiles in Step 1 gefunden).
6. **match=bg**: 100% Match-Rate. Kein einziger Miss.
7. **BG3WIN MSFlags=0x81**: Sub-Screen-Blend aktiv auf BG3-Nebel.
8. **8 rotierende Signaturen**: HDMA-Parallax (26F9→BD2C→989D→14AD→975E...).

### Root Cause (2 Probleme identifiziert)

**Problem 1: BG3-Fog-HD-Tiles blockieren bg3FogBlend**
- Step 1 findet BG3-HD-Tile → rendert opak → Nebel sichtbar, aber Terrain verdeckt
- Der `bg3FogBlend`-Pfad (Step 3) triggert nur bei `!hdTile` → wird nie erreicht

**Problem 2: bg1OverlayWinner feuert falsch in Mainbrace**
- Bedingung `!hdTile && winLayer == 0 && AllowColorMath` zu breit
- In Mainbrace: BG1 gewinnt in Nebellücken (5370 px), ~3914 ohne BG1-HD-Tile
- bg1OverlayBlend rendert BG2-Hintergrund mit BG1-Terrain-Tint → FALSCH

**Unterscheidungskriterium:** BgLayerMask Bit 2 (BG3-Präsenz):
- Beehive: BG1 gewinnt ÜBER BG3 (beide vorhanden → `mask & 0x04 == true` → Overlay) ✓
- Mainbrace: BG1 gewinnt WO BG3 FEHLT (Nebellücke → `mask & 0x04 == false` → Terrain) ✓

### Fix (M5.17)

**Fix 1: bg3FogWinner Gate (SnesHdVideoFilter.cpp ~Zeile 315)**
```cpp
bool bg3FogWinner = (winLayer == 2 && (pixelInfo.MainScreenFlags & 0x80));
if(!bg3FogWinner) {
    hdTile = _hdData->GetMatchingTile(pixelInfo.BgTiles[winLayer].Key, hdScreen->Vram);
    if(hdTile) { tileInfo = &pixelInfo.BgTiles[winLayer]; }
} else {
    frameBg3FogSkip++;
}
```
Wenn BG3 mit AllowColorMath gewinnt, wird der HD-Tile-Lookup übersprungen.
Step 3 (bg3FogBlend) findet stattdessen BG1/BG2-Terrain unter dem Nebel.

**Fix 2: bg1OverlayWinner Einschränkung (~Zeile 359)**
```cpp
bool bg1OverlayWinner = (!hdTile && winLayer == 0 && (pixelInfo.MainScreenFlags & 0x80)
                         && (pixelInfo.BgLayerMask & 0x04));
```
BG1 ist nur Overlay wenn BG3 am selben Pixel AUCH vorhanden ist.

**Neuer Diagnostik-Counter:** `fogSkip=%u` im FRAME-Log.

**Erwartete Log-Werte nach Fix:**
- `fogSkip` ≈ 49800 (BG3-Fog-Winner übersprungen)
- `fogB` ≈ 49800 (Step 3 findet Terrain unter Fog)
- `ovBlend` = 0 (kein BG1-Overlay in Mainbrace)
- `match` sollte sinken (BG3-Fog-Tiles nicht mehr gematched)

### M5.17 Verifizierung (2026-07-06)

Diagnostik-Log: `snes_hd_diag MM neu.txt` (~1310 Zeilen, Mainbrace Mayhem ohne BG3-HD-Tiles)

**Log-Counter M5.16 → M5.17:**

| Counter | M5.16 | M5.17 | Status |
|---------|-------|-------|--------|
| fogSkip | N/A | ~49876 | ✓ NEU — BG3-Fog-Winner übersprungen |
| fogB | 0 | ~49876 | ✓ FIXED — Fog-Blend feuert jetzt |
| ovBlend | 3914 | 0 | ✓ FIXED — kein falsches BG1-Overlay |
| miss | 0 | 0 | ✓ |
| cmDelta | 42699 | ~4616 | ✓ Deutlich weniger (nur BG1-Gap-Pixel) |

**BG2 wird in HD gerendert:** Alle MATCH-Samples zeigen `layer=1 (FOG-BLEND)` = BG2-Tiles unter Fog.

**User-Feedback:** "Geht in die richtige Richtung. Das seltsame Verhalten an den Stellen,
wo kein Nebel ist, ist weg." — Struktureller Fix funktioniert, aber Nebel erscheint als
uniformer dunkler Schleier statt sichtbarer Fog-Wisps/Contour.

### Fog Contour Problem — Root Cause Analyse

**BG3WIN MSColor Samples (HDMA-animiert):**
```
MSColor=0x0000 → ppuOut=0x0000  (Loading-Frame)
MSColor=0x0000 → ppuOut=0x1863
MSColor=0x0421 → ppuOut=0x3107
MSColor=0x1084 → ppuOut=0x5A0E
MSColor=0x1CE7 → ppuOut=0x6E2E
MSColor=0x14A5 → ppuOut=0x7E4D
MSColor=0x0842 → ppuOut=0x7DEA
MSColor=0x18C6 → ppuOut=0x7E6E
```

Alle MSColor-Werte sind **Graustufen** (R=G=B), Bereich 0–8 in 5-Bit. HDMA animiert
die Fog-Dichte pro Scanline.

**ADD-Modus empirisch bestätigt:**
- MSColor=0x0000 + Terrain → ppuOut ≈ Terrain (Fog addiert 0) ✓
- MSColor=0x14A5 (R=5) + Terrain(R≈8) = ppuOut(R=13) → 5+8=13 ✓ (FULL ADD, nicht Half)
- MSFlags=0x81 → Bit 5 (0x20) NICHT gesetzt → ADD (nicht Subtract)
- MSFlags=0x81 → Bit 6 (0x40) NICHT gesetzt → Full ADD (nicht Half)

**Problem der M5.17-Formel `(hdR * 4 + fogR) / 5`:**
- Schwarzer Fog (0x0000): `hdR × 0.80` → 20% Verdunklung (FALSCH — nativ: keine Änderung)
- Heller Fog (0x2108): `hdR × 0.85` → nur 5% Variation über gesamten HDMA-Bereich
- → Uniformer dunkler Schleier, kein Fog-Gradient sichtbar

### Fix (M5.18) — Fog Contour: ADD Color Math

**Änderung:** Getrennter Blend-Pfad für `bg3FogBlend` vs `bg1OverlayBlend`:

**bg3FogBlend (Mainbrace Fog)** — Native ADD Color Math:
```cpp
bool halfAdd = (pixelInfo.MainScreenFlags & 0x40) != 0;
int fR = halfAdd ? fogR / 2 : fogR;
uint8_t outR = (uint8_t)std::min(255, (int)hdR + fR);
```
- Schwarzer Fog: HD + 0 = HD unverändert (korrekt — kein Fog-Effekt)
- Heller Fog: HD + 66 = aufgehellt/neblig (korrekt — Fog sichtbar)
- HDMA-Gradient → fogR variiert pro Scanline → Contour/Wisps werden sichtbar

**bg1OverlayBlend (Beehive Honey)** — Unverändert:
```cpp
uint8_t outR = (uint8_t)((hdR * 4 + fogR) / 5);  // 80/20 wie bisher
```

**Betroffene Stellen in SnesHdVideoFilter.cpp:**
- Kommentar-Block (~Zeile 714): Erklärung der zwei Modi
- Alpha=255 Rendering (~Zeile 779): `if(bg3FogBlend)` ADD, `else if(bg1OverlayBlend)` gewichtet
- Alpha>0 Rendering (~Zeile 842): Gleicher Split

### Regressions-Check: Beehive-Level
Die Änderungen dürfen Issue O (Rambi Rumble) NICHT brechen:
- Fix 1 (bg3FogWinner): Beehive hat `winLayer == 0` (BG1 gewinnt, nicht BG3) → Gate greift nicht ✓
- Fix 2 (bg1OverlayWinner M5.17): `mask & 0x04` — **REGRESSED** (BG3 nur 56% Coverage) ⛔
- Fix 3 (bg1OverlayWinner M5.19): `frameBg3FogSkip == 0` — Fix für Regression, TEST PENDING

### Verwandte Issues
- **Issue B** (BG3 Fog blockiert HD BG1): Gleicher Grundmechanismus
- **Issue O** (Rambi Rumble BG1 Overlay): bg1OverlayBlend-Pfad, jetzt verfeinert

---

## Issue M: Lockjaw's Locker — Multi-Issue (2026-07-05)

### Symptom
Drei separate visuelle Probleme in einem Level:

**(a) Wand-Hintergrund falsch positioniert + nur nativ:**
- Die Schiffswand im Hintergrund ist horizontal verschoben
- Nur native Pixel, keine HD-Tiles für den Wand-Bereich

**(b) Deckeneffekt — Scroll-Diskontinuitäten:**
- Die Decke über dem Spielfeld zeigt Sprünge/Versätze beim Scrollen
- Tiles "springen" statt flüssig zu scrollen

**(c) Wasseroberfläche — falsche Farbe:**
- HD-Tiles für die Wasseroberfläche vorhanden, ABER Farbe ist falsch
- Zu transparent/blass, sollte blau-getönt sein

### Log-Analyse (sig=E10E4686511EB716, context [other])
```
Frame 10 (erste Spielframe, Zeile 1180):
  bg=57266 match=54280 miss=2986 notInPack=2986
  fb=20960 cmDelta=51743 BG1=31585 BG2=55552 BG3=1861

Ab Frame 11 (Zeile 1182):
  bg=55802 match=53840 miss=1962 notInPack=1962
  BG1=31532 BG2=54074 BG3=0 cmDelta=0

MISS-Detail (Zeilen 1132-1177):
  Alle MISSes auf layer=1 (BG2), src=fallback oder src=winner
  VRAM-Adressen: 0x4300-0x6990 (BG2 Tilemap-Bereich)
  Alle pal=1
```

### Analyse
- **~1962 notInPack pro Frame:** BG2-Tiles, die nicht im HD-Pack sind
  → Diese BG2-Tiles wurden beim Export nicht erfasst (fehlende Tiles im Viewer-Container)
- **BG3=1861 (Frame 10) → BG3=0 (ab Frame 11):** BG3 nur im ersten Frame aktiv,
  danach deaktiviert. Das könnte der Wasseroberflächen-Effekt sein (kurzes BG3-Window)
- **cmDelta=51743 (Frame 10) → cmDelta=0 (ab Frame 11):** Color-Math nur im
  Einblend-Frame, danach aus. Die falsche Wasserfarbe könnte mit fehlendem cmDelta
  zusammenhängen
- **fb=20960 (Frame 10):** Extrem viele Fallback-Pixel im ersten Frame — Übergang
- **BG2-MISS auf fallback-Quelle:** Tiles die über den Fallback-Pfad (nicht Winner)
  gesucht wurden → VRAM-Layout-Mismatch zwischen Export und Runtime

### Nächste Schritte
1. **(a)** Viewer: VRAM Ground Truth für Lockjaw's Locker gfxset prüfen — ist die
   Wand überhaupt im Export enthalten? Tilemap-Offset prüfen
2. **(b)** HDMA-Parallax-Effekt auf BG2-Scroll-Register prüfen (DKC2 nutzt HDMA
   auf $210D/$210E für Parallax-Scrolling in Höhlen-Leveln)
3. **(c)** BG3-Wassereffekt: Sub-Screen-Blend oder Window-Effekt? ppuConfig analysieren

---

## Issue N: Hot Head Hop — Bubble-Artefakte + Tile Seams (2026-07-05)

### Symptom
**(a) Bubble-Artefakte im unteren Bildschirmfünftel:**
- Derselbe Bereich, der in M5.14 (Issue H) gefixt wurde
- Trotz Fix noch vereinzelte Artefakte sichtbar: falsche Tiles oder Glitches
  in den untersten Scanlines

**(b) Sehr sichtbare Tile Seams:**
- HD-Tiles schließen nicht nahtlos aneinander
- Sichtbare Kanten/Linien zwischen benachbarten HD-Tiles
- Besonders auffällig bei großflächigen Hintergrund-Elementen

**(c) Lava-Animation statisch (= Issue F):**
- Bereits als Issue F dokumentiert: Palette-Cycling erzeugt Lava-Animation im Original
- HD-Tiles sind statische PNGs → kein Palette-Cycling → statische Lava

### Log-Analyse (sig=02D047A001E155B7, context [other])
```
PAL MISMATCH (Zeilen 1390-1409, wiederholt):
  hash=8A1303A809810035 runtime_pal=6 pack_pal=2
  runtime_layer=1 pack_layer=0 vram=0x5000

  → Tile im Pack mit pal=2, layer=0 exportiert
  → Zur Laufzeit PPU nutzt pal=6, layer=1
  → DOPPELTER Mismatch: Palette UND Layer falsch
```

### Analyse
- **PAL MISMATCH runtime_pal=6 vs pack_pal=2:** Das Tile wurde beim Export mit einer
  anderen Palette-Row gespeichert als die PPU zur Laufzeit verwendet. Hot Head Hop
  nutzt HDMA-Palette-Cycling ($2121/$2122), das die Palette-Zuordnung dynamisch ändert.
  Der Export-Zeitpunkt hat eine andere Phase des Palette-Cyclings erfasst.
- **Layer Mismatch (runtime=1, pack=0):** Das Tile wurde als BG1 (layer=0) exportiert,
  wird aber zur Laufzeit auf BG2 (layer=1) verwendet. Mögliches Tilemap-Sharing
  zwischen Layern.
- **Bubble-Artefakte (a):** Wahrscheinlich restliches Problem aus Issue H — der
  HDMA-Scanline-Bereich im unteren Fünftel ändert Color-Math-Register, und die
  Tile-Zuordnung in diesem Bereich ist nicht stabil über alle Frames.

### Verwandte Issues
- **Issue E:** Lava-Glow Color Math → cmDelta-Fix in M5.11 (teilweise gelöst)
- **Issue F:** Bubble-Animation Frame-Mismatch → noch offen
- **Issue H:** Unteres Fünftel ohne HD-Tiles → M5.14 Fix (teilweise)

### Nächste Schritte
1. **(a)** HDMA-Scanline-Grenze im unteren Fünftel genauer analysieren:
   Welche Scanlines sind betroffen? Ändert sich $2131 dort?
2. **(b)** Tile Seams: Cluster-Padding-Mechanismus prüfen — wurde das Padding
   nach Viewer-Commit `585858e` (Revert Cluster Padding Ring) korrekt angewendet?
   Ggf. ist individuelles Tile-Padding zu klein
3. **(c)** Issue F bleibt offen (Palette-Cycling, benötigt Animationsframe-Support)

---

## Issue O: Rambi Rumble — 0% Match Rate (2026-07-05)

### Symptom
- **Nichts** wird in HD gerendert — kompletter Fallback auf native Pixel
- Das gesamte Level zeigt nur das originale SNES-Bild
- Kein sichtbarer HD-Effekt obwohl Tiles für gfxSet 0x04 (Beehive SSB) vorhanden sein sollten
- Level 0x02 (Rambi Rumble), ppuConfig 0x03, gfxSet 0x04

### Log-Analyse — KORRIGIERT (sig=02D047A001E155B7)

Erste Analyse (sig=AD7DD09D...) war irreführend (falsches Level/Context).
Korrekte Analyse nach gezieltem Rambi-Rumble-Test:

```
Frame 10 (erster Render):
  bg=57240 match=0 palMis=22168 notInPack=35072 BG1miss=0

Frame 12+ (steady state):
  bg=53024 match=176 palMis=0 notInPack=52848 BG1miss=1152

PAL MISMATCH (Frame 10, Zeilen 1390-1409):
  hash=8A1303A809810035 runtime_pal=6 pack_pal=2
  runtime_layer=1 pack_layer=0 vram=0x5000
  → Content Hash MATCHT, aber Layer UND Palette sind falsch

MISS-Detail (steady state):
  layer=1 pal=6 vram=$5010-$5380 (BG2 honey tiles)
  layer=2 vram=$7000+ (BG3)
```

### Root Cause — Dreifach-Problem im cmFg SSB-Export (Viewer)

**Root Cause 1: Layer-Mismatch (cmFg)**
DKC2 reprogrammiert PPU-Register $210B zur Laufzeit und **tauscht die chrBases**:
- ROM ppuConfig: $210B=$25 → BG1.chr=$5000 (Honig), BG2.chr=$2000 (Terrain)
- Runtime:       $210B=$52 → BG1.chr=$2000 (Terrain), BG2.chr=$5000 (Honig)

Die Honig-Overlay-Tiles bei $5000 werden zur Laufzeit von **BG2 (layer=1)** gerendert,
aber der Viewer exportierte sie als **BG1 (layer=0)** → Folder `bg/bg1/` → Mesen
ordnet layer=0 zu → runtime_layer=1 ≠ pack_layer=0 → kein Match.

**Root Cause 2: Palette-Mismatch**
Der cmFg-Export las die Palette aus der **BG1-Tilemap** (bg1TilemapData aus simuliertem
VRAM bei ppu.bg1.tilemapBase). Diese DMA-geladene Tilemap hatte **pal=2**.
Zur Laufzeit liest BG2 seine Tilemap von **bg2TilemapBase** — im Ground Truth VRAM bei
Wort-Adresse **$6C00** liegt eine reine pal=6-Tilemap (1024/1024 Einträge = pal=6).
Ergebnis: pack_pal=2, runtime_pal=6 → kein Match.

**Root Cause 3: Content Hash (animierte Tiles, niedrige Priorität)**
BG1miss=1152 steady-state → ~18 Terrain-Tiles bei $2000 matchen nicht (Content Hash
unterschiedlich). Vermutlich VBlank-DMA-animierte Tiles (Honigtropf-Effekte), die der
Ground Truth nur in einem Frame erfasst.

### Frame 10→12 Transition
- Frame 10: palMis=22168 → Content Hashes matchen noch (frisch aus DMA geladen),
  aber Layer+Palette falsch → palMis
- Frame 12: palMis=0 → VRAM-Inhalt bei $5000 hat sich per VBlank-DMA geändert,
  Content Hashes matchen nicht mehr → alles wird notInPack statt palMis

### Fix (2026-07-06) — DKC2-HD-Tools Viewer — **LAYER-FIX WAR FALSCH (revertiert 2026-07-06b)**

**Fix 1: Layer cmFg 0→1** (index.html) — **FALSCH, REVERTIERT**
- Hash-Export: `layer: 0` → `layer: 1`, Dedup-Key `_0_` → `_1_` (Zeile ~9226/9233)
- PNG-Export: Folder `bg/bg1/gfxset_XX` → `bg/bg2/gfxset_XX` (Zeile ~8935)
- Mesen Loader ordnet `bg/bg2/` → layer=1 zu → matcht runtime BG2
- **REVERTIERT** weil Diagnostic-Log zeigt: BG1 (layer=0) rendert Honig, nicht BG2!

**Fix 2: Palette aus Ground Truth BG2-Tilemap** (index.html) — **KORREKT, BEIBEHALTEN**
- `bg2TilemapBase` zu ppuConfig hinzugefügt (Zeile ~7683)
- cmFg PNG-Export: Extrahiert BG2-Tilemap aus Ground Truth VRAM bei `bg2TilemapBase`
  und überschreibt die Palette aus der BG1-Tilemap (Zeile ~8944-8997)
- Für gfxSet 0x04: bg2TilemapBase=$6C00 → pal=6 für alle Honig-Tiles
- Fallback: Wenn kein Ground Truth verfügbar, bleibt BG1-Palette erhalten

### Update (2026-07-06b) — Korrigierte Analyse nach Diagnostic-Log

**Post-Log-Analyse** (36.931 Zeilen, Rambi Rumble sig CE539ABFD210DBD0):

| Layer | ChrBase | VRAM-Bereich | Palette | Inhalt |
|-------|---------|-------------|---------|--------|
| 0 (BG1) | $5000 | $5xxx-$6xxx | pal=6 | Honig-Tiles (cmFg) |
| 1 (BG2) | $2000 | $2xxx-$4xxx | pal=3 | Terrain-Tiles |
| 2 (BG3) | $7000 | $7xxx | — | Hintergrund (9684 FALLBACK matches) |

**Korrekte Root Cause Analyse:**
- $210B bleibt $25 (BG1.chr=$5000, BG2.chr=$2000) — KEIN Swap in diesen Frames
- Layer 0→1 Änderung war FALSCH (Runtime rendert Honig auf BG1/layer=0)
- Palette 2→6 Änderung war KORREKT (BG2 Tilemap hat pal=6)
- 14.130 MATCH insgesamt — ALLE sind BG3 FALLBACK, KEIN exakter BG1/BG2 Match
- 4 Gameplay-Kontext-Signaturen rotieren im 6er-Zyklus (HDMA?)

**Offenes Problem:** Trotz korrektem Layer + Palette + chrBase sind fast alle Honig-Tiles
MISS (Content Hash nicht im Pack). Mögliche Ursache: Ground Truth VRAM Snapshot weicht
vom Runtime-VRAM ab. Vergleich der exportierten vs. Runtime-Hashes für spezifische
Tile-Adressen erforderlich.

### Status: **PALETTE-FIX KORREKT, LAYER REVERTIERT — CONTENT HASH MISMATCH OFFEN**

### Update (2026-07-06c) — Echte Root Cause + BG1 Overlay-Blend Fix (M5.16)

**Neue Erkenntnis: Content Hash Mismatch war NICHT das Hauptproblem.**

Die M5.15 WINNERS-Diagnostik zeigte: `wn0=55140, acm0=55140` — BG1 gewinnt das
PPU-Compositing auf ALLEN Pixeln. Die Honig-HD-Tiles (im `bg/bg1/` Ordner) wurden
gefunden und gerendert — aber sie deckten den BG2-Terrain komplett ab. BG2 Terrain-
HD-Tiles wurden nie nachgeschlagen.

**Dies ist exakt das gleiche Problem wie BG3-Fog in Mainbrace Mayhem (Issue L),
nur auf BG1 statt BG3.**

**Layer-Struktur Beehive-Level (ppuConfig $03):**

| Layer | ChrBase | Inhalt | Rolle |
|-------|---------|--------|-------|
| 0 (BG1) | $5000 | Honig-Overlay | Semi-transparent, gewinnt Compositing |
| 1 (BG2) | $2000 | Terrain (Brambles, Plattformen) | Darunter verborgen |
| 2 (BG3) | $7000 | Dunkler Hintergrund | Hinterste Schicht |

**Alle betroffenen Level (bestätigt via ASM-Disassembly, alle ppuConfig $03):**
Rambi Rumble (0x02), Hornet Hole (0x11), Rambi Scene (0x12),
Parrot Chute Panic (0x13), Shortcut (0x26), King Zing Sting (0x60),
plus 7 Bonus-Räume — 13 Varianten total.

**Bramble-Level (ppuConfig $27) sind NICHT betroffen** — kein Color Math, kein Overlay.

**Fix (2-teilig):**

1. **Viewer** (`dkc2-viewer/index.html`, Zeile ~8935):
   cmFg PNG-Export-Ordner `bg/bg1/gfxset_XX` → `cmFg/gfxset_XX`.
   Damit werden die Honig-Tiles nicht mehr als BG1-HD-Tiles geladen.
   - **PNGs werden weiterhin exportiert** — nur in den `cmFg/`-Ordner statt `bg/bg1/`.
     Mesen's Loader scannt nur `bg/bg1-4/` und `sprites/`, NICHT `cmFg/` →
     die Tiles sind auf der Festplatte vorhanden, aber zur Laufzeit unsichtbar.
   - **Hashes bleiben in `hashes.bin`** mit `layer: 0` — werden zu harmlosen Orphans
     (Hash existiert, aber kein PNG wird dafür geladen). Kein Einfluss auf Matching.
   - **Zweck:** PNGs werden aufbewahrt für zukünftige Erweiterung (eigener `cmFg/`
     Loader-Pfad für echte HD-Honig-Overlays statt nur Farb-Tint).

2. **C++ Emulator** (`SnesHdVideoFilter.cpp`, Build M5.16):
   Neuer `bg1OverlayBlend`-Pfad — wenn BG1 das Compositing gewinnt und
   AllowColorMath aktiv ist, aber kein HD-Tile für BG1 existiert:
   → Suche HD-Tile auf BG2 (Terrain) und BG3 (Hintergrund)
   → Rendere mit 80% HD + 20% Honig-Tint (identisch zu BG3-Fog-Blend)
   → Color Math Delta wird übersprungen (falsches Ergebnis für Overlay)
   - **Tint-Quelle:** PPU `MainScreenColor` (= native BG1-Honig-Farbe), NICHT die
     cmFg-PNG-Dateien. Die Farbe wird direkt aus dem PPU-Output genommen.

**Erwartetes Ergebnis:** HD-Terrain sichtbar durch semi-transparenten Honig-Schleier.

### Status: **M5.16 VERIFIED** — Alle Layer in HD sichtbar, nativer Honig-Overlay vorhanden.

**Verbleibende Feintuning-Punkte:**
- Honig-Tint könnte etwas kräftiger sein (aktuell 80/20 HD/Overlay-Ratio). Niedrige Prio.
- Animierte Hintergrundelemente (kleine Bienen, Larven) zeigen dieselben Darstellungsfehler
  wie Bubbles in Hot-Head Hop (→ Issue F: Frame-Mismatch bei VBlank-DMA-animierten Tiles).
  Bekannte Limitation, betrifft alle Level mit animierten BG-Sprites.

### Update (2026-07-06d) — M5.17 REGRESSION + M5.19 Fix

**Regression in M5.17:** Die `BgLayerMask & 0x04`-Prüfung (M5.17) war zu restriktiv:

```
M5.18 Log (Beehive frame):
  bg=56056 match=31636 ovBlend=31636 miss=24420
  BG1=56056 BG2=39084 BG3=31636
  lRetry=21527
  → BG3 deckt nur 56% der Pixel ab (31636/56056)
  → 24420 BG1-Pixel ohne BG3 fallen auf nativen Pfad zurück → Regression
```

**Analyse:** Die `mask & 0x04`-Prüfung in `bg1OverlayWinner` erforderte BG3-Präsenz an
jedem Pixel, um Overlay von Terrain zu unterscheiden. Aber BG3 deckt nicht die gesamte
Beehive-Szene ab — nur ~56%. Pixel ohne BG3 (24420) wurden als nicht-overlay klassifiziert
und fielen durch Step 2 (fallback) → MISS (kein Layer-Retry dort).

**Verteilung der 31636 overlay-eligiblen Pixel (Step 3b):**
- `lRetry=21527` → BG2-Terrain via Layer-Retry gefunden (LayerIndex 1→0) ✓
- 10109 → BG3-Hintergrund als Fallback (kein BG2 an diesen Pixeln — transparent areas) ✓
- BG2-Retry funktioniert korrekt — Fix 2 (BG2 retry investigation) ist NICHT nötig

**M5.19 Fix:** `BgLayerMask & 0x04` ersetzt durch `frameBg3FogSkip == 0`:
```cpp
// M5.18 (broken):
bool bg1OverlayWinner = (!hdTile && winLayer == 0 && (pixelInfo.MainScreenFlags & 0x80)
                         && (pixelInfo.BgLayerMask & 0x04));
// M5.19 (fixed):
bool bg1OverlayWinner = (!hdTile && winLayer == 0 && (pixelInfo.MainScreenFlags & 0x80)
                         && frameBg3FogSkip == 0);
```

**Warum das funktioniert:**
- `frameBg3FogSkip` zählt BG3-Fog-Pixel im aktuellen Frame (Step 1 Gate, M5.17)
- **Beehive:** `frameBg3FogSkip == 0` immer (kein Fog-Gate feuert) → Overlay auf ALLEN BG1 ✓
- **Mainbrace:** `frameBg3FogSkip >> 0` (tausende Fog-Pixel) → kein Overlay → korrekt ✓
- Frame-Level-Entscheidung statt Per-Pixel-Mask → keine Coverage-Lücken

**Neue Diagnostik (M5.19):** WINNERS-Log enthält jetzt `ovBg2=X ovBg3=Y ovMiss=Z`
für detaillierte BG2/BG3/Miss-Aufschlüsselung im Overlay-Blend-Pfad.

### Status: **M5.19 — TEST PENDING**

---

## Issue P: Gusty Glade — Blaue Quadrate + Partieller Match (2026-07-05)

### Symptom
**(a) Blaue Quadrate auf einigen BG1-Tiles:**
- Einzelne HD-Tiles rendern als einfarbige blaue Rechtecke
- Statt des erwarteten HD-Bilds wird ein "Platzhalter-Blau" angezeigt
- Betrifft spezifische Tile-Positionen, nicht den ganzen Bildschirm

**(b) HD-Hintergrund funktioniert teilweise:**
- Einige Hintergrund-Tiles werden korrekt in HD gerendert
- Andere Bereiche sind nativ oder blau

**(c) Vordergrund-Blätter nur nativ:**
- Die animierten Blätter/Laub im Vordergrund haben keine HD-Tiles
- Nur native Pixel sichtbar

### Log-Analyse (rotierende Signaturen ab Zeile 1731, context [other])
```
Erste Rotation sig=02DABED78E3BD21D (Zeile 1731):
  bg=55633 match=55633 miss=0 notInPack=0
  BG1=28779 BG2=50957 BG3=25823 lRetry=25542 cmDelta=41906

Zweite Rotation sig=F80DE612F5578AB7 (Zeile 1761):
  bg=52966 match=52649 miss=317 layerMis=317 notInPack=0
  BG1=26449 BG2=48646 BG3=25112 lRetry=25289

Spätere Rotationen (Zeilen 1897-1987):
  miss=256-363 layerMis=256-363 notInPack=0
  lRetry=25243-25254

Beobachtungen:
  - 8 rotierende Signaturen (HDMA-Parallax-Effekt)
  - lRetry≈25000 pro Frame → ~25000 Pixel nutzen Layer-Retry-Mechanismus
  - layerMis=256-363 → Tiles mit falschem Layer-Attribut
  - notInPack=0 → Alle Tiles sind im Pack (Hash bekannt)
  - Erste Rotation: 100% match. Folgende: 99.4% match
  - BG1≈27000, BG2≈49000-51000, BG3≈25000 → alle drei Layer aktiv
```

### Analyse
- **Blaue Quadrate (a):** `notInPack=0` bedeutet alle Tile-Hashes sind im Pack.
  Die blauen Quadrate könnten von **beschädigten/leeren PNG-Daten** kommen: der Hash
  matcht, aber die zugehörige PNG-Datei enthält ungültige oder leere Pixel (Fallback-Blau).
  Alternativ: falsche HD-Tile-Zuordnung — ein Hash zeigt auf das falsche PNG.
- **layerMis=256-363:** Tiles die im Pack mit einem anderen Layer exportiert wurden
  als die PPU zur Laufzeit nutzt. Diese Tiles werden nicht gematched (→ native Pixel).
  Konsistent mit "teilweiser HD-Hintergrund" — die layer-mismatch-Tiles fallen auf nativ zurück.
- **lRetry=25000:** Der Layer-Retry-Mechanismus greift massiv. Dies bedeutet, der
  HD-Filter versucht Tiles zunächst mit dem "Winner"-Layer, scheitert, und probiert
  den Fallback-Layer. ~25000 Pixel brauchen diesen zweiten Versuch.
  → Performance-Impact (Issue D)
- **Vordergrund-Blätter (c):** Möglicherweise BG3-Tiles (BG3≈25000 aktiv), die als
  Foreground-Overlay gerendert werden, aber im Container fehlen oder als falscher
  Layer exportiert wurden

### Verbleibende Sub-Issues (b, c)
1. **(b)** layerMis=256-363 → Tiles mit falschem Layer-Attribut. Export mit
   erweitertem Layer-Matching oder layer-agnostischem Matching testen
2. **(c)** BG3-Tiles im Container für Gusty Glade gfxset prüfen —
   sind die Blätter-Tiles überhaupt enthalten?
3. **lRetry-Optimierung:** 25000 Retries pro Frame ist sehr hoch —
   direktes Layer-agnostisches Matching statt Retry könnte Performance verbessern

### Root Cause (a) — Blaue Quadrate: GEFUNDEN UND BEHOBEN (2026-07-06)

Die blauen Quadrate entstanden durch **asymmetrisches Clamping bei Color Math
Subtract** im HD-Filter (`SnesHdVideoFilter.cpp`).

**Mechanismus:**
- Gusty Glade nutzt Color Math **SUBTRACT** Modus ($2131 bit 7)
- Die SNES subtrahiert eine FixedColor (hoher R/G-Wert, niedriger/null B-Wert)
  → erzeugt Sturm-Verdunkelung
- Das additive cmDelta-System wendete dies als absolute Offsets auf HD-Pixel an
- Ergebnis: R- und G-Kanäle wurden auf 0 geclampt, B blieb unverändert → blaues Erscheinungsbild

**Beispiel (5-bit Werte):**
```
MainScreenColor (pre):  R=18 G=20 B=12
FixedColor subtract:    R=-15 G=-15 B=-2
ppuOutput (post):       R=3  G=5  B=10
cmDelta (additive):     R=-15 G=-15 B=-2  (als 8-bit: -120, -120, -16)

HD-Pixel z.B.:          R=200 G=180 B=50
Nach additivem Delta:   R=80  G=60  B=34   ← akzeptabel

Aber bei HD-Pixel:      R=100 G=80  B=50
Nach additivem Delta:   R=0*  G=0*  B=34   ← BLAU! (* = geclampt)
```

**Fix: 3 Änderungen in 3 Dateien**

1. **`SnesPpuTypes.h`** — Neues Flag `IsSubtractMode = 0x20` im `PixelFlags` Enum.
   Nutzt freies Bit zwischen Priority (0x0F) und IsSpritePixel (0x40).

2. **`SnesPpu.cpp`** — `IsSubtractMode` Flag wird an 4 Stellen gesetzt wo auch
   `AllowColorMath` gesetzt wird (RenderBgColor, RenderSprites, BG-Layer-Template,
   Mode 7). Zusätzlich: **Brightness-Inline** — `MainScreenColor` wird jetzt mit
   `channel * ScreenBrightness / 15` skaliert bevor es gespeichert wird, damit
   Pre- und Post-Math-Werte die gleiche Brightness-Basis haben (verhindert dass
   der Brightness-Faktor die cmDelta-Ratio kontaminiert).

3. **`SnesHdVideoFilter.cpp`** — Neuer `if(cmSubtractMode)` Branch:
   - **Multiplikative Skalierung** für Subtract: `hdR * cmPostR / cmPreR`
     → bewahrt Farbverhältnisse, kein asymmetrisches Clamping
   - **Additives Delta** beibehalten für Add-Modus (Nebel, Glow-Effekte)
   - Beide Pfade (opaque + alpha-blend) aktualisiert
   - **Delta-Skalierung** korrigiert: `val*8 + val/4` (= val*255/31) statt nur `val*8`
     (3% Unter-Anwendung eliminiert)

**Status: (a) BEHOBEN — (b, c) weiterhin OFFEN**

---

## Zusammenfassung: Issue-Status nach Fix-Session 2026-07-06

| Issue | Level | Schwere | Kategorie | Status |
|-------|-------|---------|-----------|--------|
| J | Pirate Panic | Mittel | Compositing (BG3+OBJ) | OFFEN |
| K | Gangplank Galleon | Mittel | HDMA Palette | OFFEN |
| L | Mainbrace Mayhem | Hoch | Sub-Screen BG3 Fog | OFFEN |
| M | Lockjaw's Locker | Hoch | Multi (Tilemap+Scroll+Color) | OFFEN |
| N | Hot Head Hop | Mittel | Artefakte+Seams | OFFEN (teilw. E/F/H) |
| O | Rambi Rumble | Kritisch | BG1 Overlay verdeckt BG2 Terrain (Honig blockiert HD-Tiles) | **M5.16 VERIFIED** — HD-Terrain + Honig-Tint sichtbar. Feintuning (Tint-Stärke) niedrige Prio. Animierte BG-Elemente (Bienen/Larven) → Issue F |
| P(a) | Gusty Glade | Hoch | Blaue Quadrate — Color Math NICHT die Ursache (acm=0), PNG-Inhalt verdächtig | C++ Subtract-Fix irrelevant, **PNG-Dateien untersuchen** |
| P(b,c) | Gusty Glade | Mittel | Layer-Mismatch+Blätter | OFFEN |
| D | Alle Level | Mittel | Performance | OFFEN |

### Priorisierung (empfohlen)
1. ~~**Issue O** (Rambi Rumble): M5.16 BG1 Overlay-Blend implementiert — Test ausstehend~~ **VERIFIED**
2. **Issue L** (Mainbrace Mayhem Fog): Sehr sichtbar, BG3-Nebel-Compositing
3. **Issue M** (Lockjaw's Locker): Drei Sub-Issues, teilweise fehlende Tiles
4. **Issue J** (Pirate Panic BG3): Subtil, nur bei Sprite-Überlappung
5. **Issue K** (Gangplank Galleon): Kosmetisch, Sunset-Palette-Effekt
6. **Issue N** (Hot Head Hop): Artefakte/Seams, teilweise bereits adressiert (E/F/H)
7. **Issue P(b,c)** (Gusty Glade Rest): Layer-Mismatch + fehlende Blätter-Tiles
8. **Issue D** (Performance): Tile-Level-Caching noch nicht implementiert

---

## Phase 2: HD Compositing Engine — Multi-Layer Compositing (2026-07-07)

### Architektur-Entscheidung

**Alle alten Heuristiken entfernt.** Die M5.1–M5.19 Sonderfall-Kaskade (fog-blend,
overlay-blend, colorMathDelta, bg3FogSkip, frameHasBg1ColorMath, etc.) wurde komplett
durch einen generischen PPU-Register-gesteuerten Multi-Layer-Compositing-Ansatz ersetzt.

**Entscheidung des Users:** "Nein, lass uns das nicht mitschleppen" — alte Heuristiken
sind NICHT als Fallback/Bridge beibehalten. Erwartete visuelle Regression auf allen
Color-Math-Leveln bis Phase 3 (HD Color Math) implementiert ist.

### Was geändert wurde (3 Dateien)

**1. `SnesHdData.h`** — 2 neue Felder in `SnesHdScanlineInfo`:
- `BgMode` (uint8_t) — SNES BG Mode (0-7), für Prioritätssortierung
- `Mode1Bg3Priority` (bool) — $2105 bit 3, für BG3-Prioritätsposition

**2. `SnesPpu.cpp`** — 2 Zeilen im Scanline-Snapshot-Block (~Zeile 929-930):
```cpp
sl.BgMode = _state.BgMode;
sl.Mode1Bg3Priority = _state.Mode1Bg3Priority;
```

**3. `SnesHdVideoFilter.cpp`** — Komplette Neufassung von `ApplyFilter()`:

| Sektion | Zeilen | Beschreibung |
|---------|--------|-------------|
| Multi-Layer Lookup | ~182-250 | Iteriert `BgLayerMask`, `GetMatchingTile()` pro Layer |
| BG1↔BG2 Layer Retry | ~210-230 | Strukturell (nicht heuristisch), für Layer-Index-Mismatch |
| Priority Sorting | ~258-295 | SNES Mode 1 Prioritätsreihenfolge (back-to-front) |
| Compositing | ~300-340 | Native PPU als Base, HD-Layer darüber mit Alpha |
| Diagnostik | ~350-380 | `frameMultiLayer`, `frameHdLayers[4]` |

**Entfernte Variablen/Counter (~15):**
`bg3FogBlend`, `bg3FogSkip`, `frameBg3FogSkip`, `bg1OverlayBlend`, `bg1OverlayWinner`,
`colorMathDelta`, `colorMathRatio`, `bg3BgFallback`, `frameHasBg1ColorMath`,
`frameBg1OvBg2`, `frameBg1OvBg3`, `frameBg1OvMiss`, `layerRetryHd`, `layerRetryNative`,
`layerRetryMiss`, `fogB`, `fogNat`, `ovBlend`, `cmDelta`

### Erwartete Test-Ergebnisse

| Level | Erwartung | Warum |
|-------|-----------|-------|
| Pirate Panic | Identisch zu M5.19 | Kein Color Math → neuer Code produziert gleichen Output |
| Mainbrace Mayhem | Layering korrekt, Fog FEHLT | Color Math nicht implementiert (Phase 3) |
| Rambi Rumble | BG2 Terrain über BG3, Honig FEHLT | Color Math nicht implementiert |
| Hot Head Hop | BG1 HD sichtbar, Lava-Glow FEHLT | Color Math nicht implementiert |
| Gusty Glade | Layering korrekt, Verdunkelung FEHLT | Color Math nicht implementiert |

### Impact auf offene Issues

| Issue | Erwarteter Status nach Phase 2 |
|-------|-------------------------------|
| J (BG3 Bleed-Through) | Möglicherweise gelöst durch korrektes Multi-Layer-Compositing |
| K (Sunset) | Unverändert (Palette-Problem, nicht Color-Math) |
| L (Mainbrace Fog) | Regression erwartet (Fog-Blend entfernt) → Phase 3 löst |
| M (Lockjaw) | Teilweise gelöst (Multi-Layer), Color Math fehlt |
| N (Hot Head Artefakte) | Teilweise (Seams + Bubbles bleiben) |
| O (Rambi Rumble) | Regression erwartet (Overlay-Blend entfernt) → Phase 3 löst |
| P (Gusty Glade Blau) | Gelöst (Color Math Delta entfernt, war Root Cause) |

### Status: **P2.0 — TESTED, 6 BUGS → FIXED IN P2.1**

---

## Phase 2.1: Winner-Only HD Compositing + CM-Skip (2026-07-07)

### P2.0 Test-Ergebnisse (User getestet auf separatem PC)

Build kompilierte sauber. 6 Issues gefunden:

| # | Level | Problem | Root Cause | Fix |
|---|-------|---------|------------|-----|
| 1 | NPC Shops | Text komplett unsichtbar | Lower-prio HD tile composited OVER higher-prio text (kein HD match) | Winner-only |
| 2 | Rambi Rumble | BG3 Bienenstöcke im Vordergrund | BG3 prio-1 tiles mit Bg3Priority=true sortieren ganz nach vorne | Winner-only |
| 3 | Hot Head Hop | BG3 Lava-Hintergrund im Vordergrund | Gleicher Mechanismus wie #2 | Winner-only |
| 4 | Mainbrace Mayhem | HD Fog opak (dunkel, nicht durchsichtig) | BG3 fog HD tiles gefunden aber opak gerendert (kein Color Math) | CM-skip |
| 5 | Pirate Panic | Meer-Farbe fehlt | colorMathDelta entfernt (expected) | CM-skip |
| 6 | Lockjaw's Locker | Wasser-Höhe/Clipping | Minor, low priority | — |

### Root Cause: Fundamentaler Multi-Layer Bug

Back-to-front multi-layer compositing hat grundlegenden Fehler ohne Color Math:

**Wenn ein higher-priority Layer Content hat (BgLayerMask) aber KEIN HD tile,
compositen lower-priority HD tiles ÜBER den nativen PPU Pixel und VERDECKEN
den higher-priority Layer.** Der native PPU Pixel enthält korrekt den higher-
priority Layer, aber der lower-priority HD tile ersetzt ihn.

Beispiel: BG1 (Text, prio 5) gewinnt Compositing. BG2 (Background, prio 4) hat
HD tile. Compositing: BG2 HD tile ersetzt native Base → BG1 hat kein HD tile →
composited nicht → Text weg.

### P2.1 Fix

**Teil 1: Winner-only rendering.** Statt ALLE Layer zu compositen, nur den
HD tile für den PPU Compositing Winner (`BgWinnerLayer`) rendern. Ohne Color
Math verdeckt der Winner alle lower-priority layers vollständig. Fixt Bugs 1-3.

**Teil 2: CM-skip.** Wenn der Winner `AllowColorMath` Flag hat
(`MainScreenFlags & 0x80`), HD lookup komplett skippen → nativer PPU Pixel.
Nativer Pixel hat bereits korrektes Color Math. Fixt Bug 4, handhabt Bug 5.

### Code-Änderungen (nur `SnesHdVideoFilter.cpp`)

| Aktion | Was |
|--------|-----|
| Entfernt | Multi-Layer Arrays (`layerHdTiles[4]`, `layerTileInfos[4]`, `layerHdCount`) |
| Entfernt | Priority Sorting (`LayerEntry`, `order[]`, insertion sort) |
| Entfernt | `mode1Bg3Prio` per-scanline read |
| Entfernt | `frameMultiLayer` counter |
| Hinzugefügt | CM-skip Check (`MainScreenFlags & 0x80`) |
| Hinzugefügt | `frameCmSkip` counter |
| Geändert | Multi-layer loop → single winner lookup |
| Geändert | Multi-tile compositing → single-tile rendering |
| Beibehalten | BG1↔BG2 layer retry (strukturell) |
| Beibehalten | Diagnostik (MATCH/MISS logs, angepasst für winner-only) |

### Erwartete P2.1 Test-Ergebnisse

| Level | Erwartung |
|-------|-----------|
| Pirate Panic | HD tiles + native Meer-Farbe (CM-skip auf Meer-Pixels) |
| NPC Shops | Text sichtbar (Winner hat kein HD match → nativ) |
| Mainbrace Mayhem | Nativer Fog (korrekt semi-transparent, kein HD) |
| Rambi Rumble | Nativ (BG1 Honig gewinnt mit CM → nativ) |
| Hot Head Hop | HD wo kein CM, nativ wo CM (mixed) |

### Status: **P2.1 — TESTED, VERIFIED → Basis für Phase 3**

---

## Phase 3: HD Color Math Engine (2026-07-07 — 2026-07-13)

### Ziel

Den CM-skip aus P2.1 durch echtes HD Color Math ersetzen: HD-Tiles rendern UND
PPU Color Math (ADD/SUB, Sub-Screen/FixedColor, Half-Intensity, Brightness) korrekt
darauf anwenden. EIN generischer Algorithmus für ALLE Level — keine Heuristiken.

### Architektur-Überblick (5-Phasen-Plan, siehe ARCHITECTURE.md)

| Phase | Thema | Status |
|-------|-------|--------|
| P3.0 | Winner + Layer Retry (kein CM) | Done |
| P3.1-P3.5 | Iterative CM-Integration | Done |
| P3.6-P3.7 | Unified Algorithm + Diagnostik | Done |
| P3.8 | BG3 Swap (zu breit) | Done, teilw. revertiert |
| P3.9 | useHdSubPixel (revertiert) | Done, revertiert |
| P3.10 | BG3+Mode1Bg3Priority targeted swap | **CURRENT (3a1b31ae)** |

### P3.10 Algorithm (Current — committed `3a1b31ae`, pushed)

Three-step algorithm:

**Step 1:** Try HD tile for PPU compositing winner layer (+ BG1↔BG2 retry for 4bpp layers)

**Step 2 (winner HD tile found):**
- Search for bottom HD tile below winner in priority order
- **BG3 Overlay Swap:** If ALL conditions met:
  - `cmActive` (AllowColorMath on this pixel)
  - `AddSubscreen` (sub-screen blend mode)
  - `hdTileBot` exists (bottom layer HD tile found)
  - `winLayer == 2` (BG3 is winner)
  - `Mode1Bg3Priority` ($2105 bit 3 set)
  - `(MainScreenLayers & 0x0F) == 0x04` (only BG3 on main screen)
  → Swap bottom to primary, set `isOverlayPixel=true`
- Otherwise: render winner HD tile with CM using native `SubScreenColor`

**Step 3 (winner HD NOT found + cmActive + AddSubscreen):** Overlay fallback —
search other layers for HD content, apply overlay tint extracted from native PPU output

### PPU Data Available Per-Pixel (from `SnesPpu.cpp`)

| Field | Source | Description |
|-------|--------|-------------|
| `BgWinnerLayer` | PPU compositing | Which BG won (0-3) |
| `BgLayerMask` | PPU per-pixel | Which layers have non-transparent content (MAIN screen) |
| `MainScreenFlags` | PPU | Bit 7=AllowColorMath, Bit 5=IsSubtract, Bit 6=HalfResult |
| `SubScreenColor` | PPU sub-screen buffer | BGR555, post-compositing |
| `MainScreenColor` | PPU main-screen buffer | BGR555, pre-Color-Math, post-Brightness |
| `BgTiles[0..3].Key` | PPU | ContentHash + PaletteIndex + LayerIndex per layer |

### Per-Scanline Data (from `SnesHdScanlineInfo`)

| Field | Description |
|-------|-------------|
| `MainScreenLayers` | $212C — which layers on main screen |
| `SubScreenLayers` | $212D — which layers on sub screen |
| `CMEnabled` | $2131 low 6 bits — which layers have CM enabled |
| `AddSubscreen` | bool — use sub-screen (vs fixed color) |
| `Mode1Bg3Priority` | bool — $2105 bit 3 |
| `ScreenBrightness` | 0-15 — $2100 |
| `ColorMathHalveResult` | bool — half-intensity flag |

### Test Results (P3.10)

| Level | Result | Notes |
|-------|--------|-------|
| Mainbrace Mayhem | ✓ Working | BG3 swap fires, terrain visible through fog |
| Rambi Rumble | ✓ Working | BG1 overlay tint visible on terrain |
| Pirate Panic | ✓ Working | No regression |
| Lockjaw's Locker | ✗ Water tint missing | BG1 HD tiles found via retry, but no blue tint |
| Hot Head Hop | ? | Not yet tested in P3.10 |
| Gusty Glade | ? | Not yet tested (Phase 4: Color Window needed) |

---

## Lockjaw's Locker — P3.10 Diagnostic Investigation (2026-07-13)

### PPU Configuration (confirmed from context log)

```
Signature: 213CE0452E0D6AB0 (fade-in) / CE539ABFD210DBD0 (gameplay)
Main=$01 (only BG1)
Sub=$16 (BG2+BG3+OBJ)
CMEnabled=$21 (BG1+Backdrop)
AddSubscreen=1
Mode1Bg3Priority=1
ScreenBrightness=15 (after fade-in)
No windows active (ClipMode=0, PreventMode=0)
```

### P3.7 Diagnostic Data Analysis

From `snes_hd_diag (1).txt` (Lockjaw frames):

```
wn0=55700  — ALL pixels have winner=BG1 (only BG1 on main screen)
hdBG1=0    — NO BG1 HD tiles found directly
hdBG2=38400, hdBG3=9900 — HD content found via Layer-Retry/Overlay
lRetry=38500 — almost all via BG1→BG2 layer retry
overlay=48300 — everything goes through overlay path
miss=7300  — BG1 tiles that completely miss
```

**MISS entries:** All show `mask=0x01` (only BG1 present), pal=6, VRAM $5320-$6340

### What This Means for P3.10

1. BG1 has NO HD tiles in the pack (user confirmed: removed from pack for Rambi/Beehive levels)
2. Step 1 layer retry (BG1→BG2) finds HD tiles → Step 2 fires
3. Step 2 applies CM with `SubScreenColor` (native PPU sub-screen value)
4. **Key question:** What is `SubScreenColor` at BG1 terrain positions?
   - Sub-screen has BG2+BG3+OBJ → BG3 water tiles cover those positions
   - If SubScreenColor is blue → CM ADD should produce blue tint on HD tiles
   - If SubScreenColor is 0/black → ADD with 0 = no visible tint

### Diagnostic Code Added (this session)

Added diagnostic logging after line 394 in `SnesHdVideoFilter.cpp`:
- Logs `SubScreenColor`, `SubScreenLayers`, `MainScreenLayers`, `CMEnabled`,
  `ColorMathHalveResult`, x/y position
- Limited to 10 entries where: `winLayer==0 && MainScreenLayers==0x01 && cmActive && AddSubscreen`
- Purpose: Confirm whether SubScreenColor is actually blue (non-zero) at Lockjaw terrain pixels

### Next Steps

1. **User builds P3.10 with diagnostic**, runs Lockjaw, shares log
2. **If SubScreenColor IS blue:** Bug is in CM application math (rendering stage)
3. **If SubScreenColor IS 0/black:** Bug is in PPU capture (sub-screen not composited correctly at those positions)
4. Fix based on findings

### Status: **DIAGNOSTIC ADDED — AWAITING USER BUILD + TEST**

---

## DKC2 Level PPU Configurations (Reference Table)

| Level | Sig | Main | Sub | CM ($2131) | AddSub | Mode1Bg3Pri |
|-------|-----|------|-----|------------|--------|-------------|
| Mainbrace (fog) | BD2C76B73C545997 | $04 (BG3) | $13 (BG1+BG2+OBJ) | $24 (BG3+Backdrop) | Yes | Yes |
| Mainbrace (no fog) | E10E4686511EB716 | $17→$04 (HDMA) | $13 | $24 | Yes | Yes |
| Lockjaw (underwater) | 213CE0452E0D6AB0 | $01 (BG1) | $16 (BG2+BG3+OBJ) | $21 (BG1+Backdrop) | Yes | Yes |
| Lockjaw (gameplay) | CE539ABFD210DBD0 | $01 | $16 | $21 | Yes | Yes |
| Pirate Panic | 1DF33CEAA50F7F08 | $17 | $10 (OBJ) | $02 (BG2 only) | Yes | Yes |
| Rambi Rumble | CE539ABFD210DBD0 | $01 | $16 | $21 | Yes | Yes |
| Gusty Glade | 02DABED78E3BD21D | rotates | — | — | — | — |
| Hot Head Hop | 02D047A001E155B7 | $17 | $00 | $06 (BG2+BG3) | No (fixed) | Yes |
