# SNES HD Pack Support for Mesen2 — Project Documentation

## Overview

Adding SNES HD texture pack support to Mesen2, modeled after the existing NES HD Packs system. Allows replacing SNES tiles at render time with high-resolution PNG replacements.

## Current Status

**Stand: 2026-06-08**  
**Bug #4 verifiziert** (Mesen2: `75589f83`, Viewer: `1628677`).  
**Multi-Gfxset-Architektur implementiert, aber noch nicht gebaut/getestet** (Mesen2: `16dadecf`, Viewer: `4331822`).

**Nächste Schritte (Priorität):**
1. Mesen2 neu bauen + Multi-Gfxset-Pack testen (Level 1 + Worldmap)
2. Level 2 korrekt exportieren (richtigen Gfxset-Index laden, neu speichern)
3. Weitere Gfxsets für vollständige Spielabdeckung
4. Bug #5 (BG2 Animation Flicker) — Known Limitation, Diagnose offen
5. M6: HD-Tiles im Tile-Viewer-Debugger

## Milestone History

### M1 — Project Setup & Analysis (implicit)
- Analyse der bestehenden NES HdPacks-Architektur
- Planung der SNES-Adaption (8x8 tiles, BG layers, 8bpp palettes)

### M2 — Data Structures & Loader (`5681eb22`)
**Files:**
- `Core/SNES/HdPacks/SnesHdData.h` — Datenstrukturen
- `Core/SNES/HdPacks/SnesHdPackLoader.cpp/.h` — Pack-Loader

**Key Design:**
- Tile Key: VRAM address + palette index + layer index
- Pack-Verzeichnis: `HdPacks/{romName}/manifest.json` + PNG tiles in `bg/bg1-4/` und `sprites/`
- PNG loading via `PNGHelper::ReadPNG<uint32_t>` (ARGB)

### M3 — PPU Integration & Video Filter (`9063b80c`)
**Files:**
- `Core/SNES/SnesPpu.cpp/.h` — Double-buffered `SnesHdScreenInfo`, Tile-Info-Sammlung im Pixel-Loop
- `Core/SNES/SnesConsole.cpp/.h` — `LoadHdPack()` bei ROM-Load, `SetHdData()` auf PPU, HD Video Filter in `GetVideoFilter()`
- `Core/SNES/HdPacks/SnesHdVideoFilter.cpp/.h` — Palette LUT (BGR555->ARGB), HD Tile Lookup mit premultiplied alpha blending, hi-res aware fallback

### M3.1 — Code Cleanup & Bugfixes (2026-05-29, uncommitted)

**Fixes applied:**

1. **Indentation fix** (`SnesHdVideoFilter.cpp:128-131`): Premultiplied alpha blend lines were misaligned inside the `else if(alpha > 0)` block. Fixed to correct indentation level.

2. **Removed dead `IsSprite` field** (`SnesHdData.h`, `SnesHdPackLoader.cpp`, `SnesPpu.cpp`): `SnesHdTileKey::IsSprite` was never used in `operator==` or `GetHashCode()`, making it redundant since `LayerIndex=4` already identifies sprites. Removed the field and all assignments.

3. **`InitializeLoader` now validates directory** (`SnesHdPackLoader.cpp:33-58`): Previously always returned `true`. Now checks for actual subdirectories or files in the pack folder before proceeding, eliminating unnecessary log spam when no pack exists.

4. **`LoadTilesFromDirectory` return value fix** (`SnesHdPackLoader.cpp`): Previously returned `!files.empty()` which was true even if all PNGs were invalid. Now tracks `loadedCount` and returns `loadedCount > 0`.

5. **HD Filter respects `EnableHdPacks` setting** (`SnesConsole.cpp:303`): `GetVideoFilter()` now checks `_settings->GetSnesConfig().EnableHdPacks` in addition to `_hdData != nullptr`, allowing users to toggle HD packs on/off at runtime via the UI.

6. **Palette LUT caching** (`SnesHdVideoFilter.cpp/.h`): `OnBeforeApplyFilter()` no longer recalculates the 32K-entry palette LUT every frame. Instead tracks `_lastHue/_lastSaturation/_lastBrightness/_lastContrast` and only rebuilds when settings change.

7. **Improved comment on `SnesHdBitmapInfo::Init()`** (`SnesHdData.h`): Clarified that `Init()` is for deferred async PNG decoding via `LoadAsync()`.

### M4.1 — Sprite Priority Fix (2026-05-30, noch nicht gebaut)

**Problem:** Diddy (Sprite) lief hinter den HD BG1-Tiles, weil der HD-Filter jedes Pixel mit einem BG1-Tile-Key ersetzte — auch wenn ein Sprite den Pixel gewonnen hatte.

**Root Cause:** `RenderTilemap()` sammelt Tile-Infos für alle BG-Pixel unabhängig davon, ob das Tile den Main Screen gewonnen hat. `ApplyFilter()` ersetzte blind alle Pixel, bei denen ein HD-Tile gefunden wurde.

**Fix (3 Dateien):**

1. **`Core/SNES/SnesPpuTypes.h`**: `IsSpritePixel = 0x40` zu `PixelFlags` hinzugefügt (Bit 6, bisher unbenutzt).

2. **`Core/SNES/SnesPpu.cpp`**:
   - `RenderSprites()`: Wenn Sprite Main Screen gewinnt → `_mainScreenFlags[x] |= PixelFlags::IsSpritePixel`
   - `RenderScanline()`: Nach `ApplyHiResMode()` → `_mainScreenFlags[x]` in `pixelInfo.MainScreenFlags` kopieren (für den aktuellen Chunk `[_drawStartX, _drawEndX]`)
   - Wenn danach ein BG-Tile den Sprite überschreibt, löscht `DrawMainPixel()` das Bit automatisch

3. **`Core/SNES/HdPacks/SnesHdVideoFilter.cpp`**:
   - `#include "SNES/SnesPpuTypes.h"` hinzugefügt
   - `ApplyFilter()`: `spriteOnTop = pixelInfo.MainScreenFlags & PixelFlags::IsSpritePixel` — wenn gesetzt, nativen SNES-Pixel verwenden statt HD-Tile

### M4 — End-to-End Test ✓ (2026-05-29/30)
- Solide Test-PNGs (32×32, 9 Adressen × 8 Paletten) für DKC2 BG1 generiert
- Tiles in-game sichtbar: grün, blau, gelb, cyan, orange, lila — Matching bestätigt
- `EnableHdPacks`-Checkbox im SNES General-Tab ergänzt
- `ForceFilterUpdate()` bei Toggle → sofortige Reaktion ohne ROM-Reload
- VS 2025 Insiders (v145-Toolset, MSVC 14.51) Build-Fixes:
  - `_Openprot` entfernt aus `UTF8Util.h`
  - Pfade mit Leerzeichen in `UI.csproj` gequotet
  - HD-Pack-Dateien in `Core.vcxproj` eingetragen
  - `GetCartName()` auf public gestellt, `EnableHdPacks` zu `SnesConfig` hinzugefügt

### M4.2 — HD-Tile Priority Fix (2026-05-30, noch nicht gebaut)

**Problem:** Zwei Bugs nach M4.1:
1. Tiles clippten teilweise durch Diddy (besonders bei Bewegung)
2. Foreground-BG-Layer (BG2/BG3 Seile, Pflanzen) verschwanden hinter den HD-BG1-Tiles

**Root Cause beider Bugs:** `RenderTilemap` sammelte HD-Tile-Info UNCONDITIONALLY für alle BG-Layer, egal ob sie den Pixel gewonnen haben oder nicht. Der Filter iterierte dann durch alle BgTiles[] und fand BG1-Tiles auch an Pixeln, wo BG2/Sprite eigentlich gewonnen hatte.

**Fix (2 Dateien):**

1. **`Core/SNES/SnesPpu.cpp` — `RenderTilemap()`:**
   - `bool winsMain = drawMain && (_mainScreenFlags[x] & 0x0F) < priority && ...` extrahiert
   - HD-Tile-Info-Sammlung nun INNERHALB von `if(color > 0)` und `if(winsMain)`
   - Schreibt immer auf `BgTiles[0]` (höheres Layer überschreibt ggf.); `BgTileCount = 1`
   - `BgTileCount = 0` signalisiert: Sprite oder Backdrop hat gewonnen — kein HD-Tile

2. **`Core/SNES/HdPacks/SnesHdVideoFilter.cpp` — `ApplyFilter()`:**
   - Loop entfernt, direkt `BgTiles[0]` prüfen wenn `BgTileCount > 0`
   - `spriteOnTop`/`IsSpritePixel`-Check entfernt (durch `BgTileCount=0` implizit gehandhabt)
   - `#include "SNES/SnesPpuTypes.h"` entfernt (nicht mehr benötigt)

**Warum das BgTileCount=1 Modell korrekt ist:**
- Sprites rendern VOR BG-Layern (Mode 1: `RenderSprites` → `RenderTilemap BG1/2/3`)
- Wenn Sprite Priority 3 (Wert 10) gewinnt, setzt er `_mainScreenFlags[x] = 10|IsSpritePixel`
- BG1 high (Wert 9) prüft `10 < 9` = false → BG1 gewinnt nicht → kein Info gesammelt
- Wenn BG2 high (Wert 8) nach BG1 low (Wert 6) gewinnt, überschreibt BG2 `BgTiles[0]`
- Ergebnis: `BgTiles[0]` enthält immer den echten Gewinner-BG-Layer

### M5 — Echte HD-Tiles ✓ (2026-06-01, verifiziert)

**HdPacks-Pfad (bestätigt):**
```
C:\Users\beach\OneDrive\Dokumente\Mesen2\HdPacks\Donkey Kong Country 2\bg\bg1\
```
ROM-Name: `Donkey Kong Country 2` (verifiziert aus SNES ROM-Header offset 0xFFC0, HiROM + 512-Byte SMC-Header → 0x101C0; ROM: `Donkey Kong Country 2 - Diddy's Kong Quest.smc`)

**Workflow (via DKC2-HD-Tools Viewer):**
1. Container im Viewer laden (`Alt v1.3`)
2. "Texture Pack"-Button → `Alt v1.3_mesen2_hdpack.zip` (918 unique BG1 Sub-Tiles)
3. ZIP nach `C:\Users\beach\OneDrive\Dokumente\Mesen2\` entpacken
4. DKC2 in Mesen neu laden (Pack wird beim ROM-Load eingelesen)

**Ergebnis:** BG1-Tiles (Piratenschiff, gfxset_07) korrekt in HD dargestellt, Spiel spielbar.

**Viewer-Änderungen (DKC2-HD-Tools):**
- `exportContainerAsZip`: speichert `ppu_config.json` + `tile_arrangement.bin` pro Level-Set
- `importContainerFromZip`: stellt diese Felder beim Import wieder her
- `exportAsTexturePack`: Ausgabepfad auf `HdPacks/Donkey Kong Country 2/bg/bg1/` korrigiert; `OffscreenCanvas.convertToBlob()` durch `HTMLCanvasElement.toDataURL()` ersetzt (Browser-Kompatibilität); Tile-ID-Lookup-Bug gefixt (`"7_0"` Format statt Integer)

**Noch offen → M5.1:**
- Manifest-Parsing (`manifest.json` für Scale, Version) — aktuell Scale hardcoded auf 4
- Flip-Handling in `ApplyFilter()` (BG1-Tiles werden gespiegelt falsch dargestellt → Ghosting-Glitches)
- BG2-Export im Viewer (Hintergründe noch nativ)
- VRAM-Kollision auf Worldmap (siehe Known Limitations)

### M5.1 — Bugfixes nach erstem Praxistest ✓ (2026-06-08)

**Bug 1: Grafische Glitches bei gespiegelten Tiles ✓ (Commit 2209d5f3)**
- Ursache: H/V-Flip-Bits wurden im HD-Filter nicht angewendet → gespiegelte Tiles zeigten un-gespiegelte HD-Version → Ghosting.
- Fix: Flip-Flags aus `SnesHdPpuPixelInfo` auslesen, HD-Pixel in `ApplyFilter()` entsprechend spiegeln.

**Bug 2: HD-Tiles 6 Pixel nach oben verschoben → Diddy im Boden, Seile falsch ✓ (Commit abd2da7c)**
- Ursache: `ApplyHiResMode()` schreibt `_scanline N` in PPU-Buffer-Zeile `N+6` (non-overscan). ScreenTiles wurde aber bei Index `_scanline` beschrieben → Filter las Tile-Info von Scanline N+6 mit Pixeldaten von Scanline N.
- Fix: `hdScanline = _overscanFrame ? (_scanline-1) : (_scanline+6)` in `RenderTilemap()` und `RenderScanline()` — gleiche Formel wie `ApplyHiResMode()`.
- Gleichzeitig: `ScreenHeight` 240→239 (Constant-Fix), Flip-Refactor PPU→Filter, Alpha-Blend auf SubScreenColor umgestellt.

**Bug 3: BG2 (Hintergründe) nicht in HD ✓ (Viewer Commits 62a1135, 9eb3be3, 142d6df)**
- Dynamische Tile-Größe, Live-Catalog-Fallback, BG2-Tilemap-Extraktion implementiert.
- BG2-Tiles werden korrekt exportiert und in Mesen dargestellt.

**Bug 4: Level-Tiles auf Worldmap (VRAM-Kollision) ✓ GEFIXT**
- Ursache: DKC2 lädt verschiedene Gfxsets an dieselben VRAM-Adressen. `{vramAddr}_P{pal}` reicht nicht zur eindeutigen Identifikation.
- Fix: VRAM-Checksum-Verifikation. Der Viewer berechnet beim Speichern für jede exportierte Tile eine Prüfsumme (Summe der 16 VRAM-Wörter = 32 Bytes) und speichert sie in `checksums.bin`. Der Loader (`LoadChecksums()`) liest die Datei und hängt die Checksummen an `SnesHdPackTileInfo`. Der Filter (`ApplyFilter()`) prüft via `ComputeVramTileChecksum()` ob das aktuelle VRAM noch stimmt — bei Abweichung kein HD-Tile.
- Workflow: Level im Viewer laden → Container speichern (berechnet Checksummen) → Texture Pack exportieren (schreibt `checksums.bin` in ZIP).
- Rückwärtskompatibel: Packs ohne `checksums.bin` verhalten sich wie bisher (kein `HasChecksum` gesetzt → immer matchen).

**Bug 5: BG2 Tile-Animation Flicker — Known Limitation (geparkt 2026-06-08)**

*Symptom:* Ozean-Animation in "Pirate Panic" (BG2) wechselt zwischen HD (4×) und nativem Bild wenn Kamera nach oben scrollt (unterer Ozeanbereich sichtbar).

*Diagnose:* HD/nativ-Wechsel (nicht bloße Farbverschiebung) → Tile-Keys fehlen im HD-Pack für bestimmte Animationszustände. Genaue Ursache unklar — wahrscheinlich eine Kombination aus:
- **Tilemap-Animation (Mechanismus 2):** Spiel wechselt tileNum-Einträge per Frame; unser Export erfasst nur einen Animationsframe
- **Palette-Mismatch:** Einige Tiles (speziell unterer Tilemap-Bereich) verwenden andere Palette-Rows als erwartet
- **Sonderproblem unterster Tilemap-Rows:** Beim Tilemap-Render erschienen die untersten ~4 Tilemap-Zeilen als grau/bräunlich (möglicher Sub-Screen-Blend-Effekt im Spiel, der ohne Farb-Math-Unterstützung falsch aussieht)

*Was versucht wurde (beides hat nicht funktioniert):*
1. **Tilemap-Render (Original):** bg2.png = Tilemap gerendert → gute Upscaling-Qualität, aber nur ein Animations-Frame → Flicker; graue Tiles im unteren Bereich → Farbverschiebung bei Kamera-Scroll
2. **Raw-VRAM-Grid:** bg2.png = alle 512 VRAM-Tiles ab chrBase sequenziell → alle Frames vorhanden, aber: (a) schlechtere Upscaling-Qualität (kein Szenen-Kontext), (b) Palette-Mismatch für Tiles die nicht im aktuellen Tilemap-Frame waren → Flicker schlimmer als zuvor. Revert auf Commit `142d6df`.

*Nächster Diagnose-Schritt (wenn wieder aufgegriffen):*
- Hit/Miss-Logging im `SnesHdVideoFilter` aktivieren → sehen welche konkreten Keys fehlen
- Klären ob Mechanismus 2 (Tilemap-Switching) oder 3 (VRAM-Overwrite / CHR-Animation)
- Mesen2: Sub-Screen Color-Math für BG2 HD-Tiles prüfen

### Multi-Gfxset-Architektur — IMPLEMENTIERT, Test ausstehend

**Commits:** Mesen2 `16dadecf`, Viewer `4331822`

**Neues Exportformat (Viewer):**
```
bg/bg1/gfxset_07/2000_P03.png    (Pirate Panic)
bg/bg1/gfxset_08/2000_P03.png    (anderer Level, gleiche Adresse, anderer Inhalt)
```
- Deduplizierung jetzt **per Gfxset** (nicht mehr global)
- `checksums.bin` Byte 3 = gfxsetIndex (war `reserved=0`)
- tileChecksums-Einträge enthalten `gfxset`-Feld (aus `currentStyle.graphics`)

**Mesen2 Loader:**
- `LoadPack()` sucht `gfxset_XX/`-Unterordner je Layer; Fallback auf Flat-Layout (rückwärtskompatibel)
- `ParseGfxsetDirName()`: parst `"gfxset_07"` → Index 7
- `_checksumMap`-Key: `(gfxset << 24) | (layer << 16) | vramAddr`

**Mesen2 Filter:**
- `GetMatchingTile(key, vram)` iteriert Kandidaten-Vector → gibt Tile mit passendem VRAM-Checksum zurück
- Multi-Gfxset-Auflösung direkt in `GetMatchingTile`, nicht mehr in `ApplyFilter`

**Workflow:**
1. Gfxset N laden → upscalen → Save to Container
2. Nächsten Gfxset laden → upscalen → Save
3. Export → ein ZIP deckt alle Gfxsets ab
4. DKC2-Levels die denselben Gfxset nutzen, profitieren automatisch

**Noch offen / nächste Session:**
- Build + Test: Level 1 + Worldmap mit neuem Pack prüfen
- Level 2 korrekt exportieren (richtiger Gfxset-Index)
- Optional: VRAM-Snapshot beim Laden sichern (Robustheit wenn User Gfxset wechselt bevor er speichert)

### M6 — Tile Viewer Integration (niedrigere Priorität)
- HD-Tiles im Tile-Viewer-Debugger anzeigen (wenn EnableHdPacks aktiv)
- `SnesPpuTools.cpp` + `TileViewerViewModel.cs` anpassen

## Architecture

```
SnesConsole
  ├── LoadHdPack()          → SnesHdPackLoader::LoadHdSnesPack()
  ├── _hdData (shared_ptr)  → SnesHdPackData
  └── GetVideoFilter()      → SnesHdVideoFilter (when HD active + EnableHdPacks)

SnesPpu
  ├── _hdData*              → pointer to loaded pack data
  ├── _hdScreenInfo[2]      → double-buffered per-pixel tile info
  └── RenderTilemap()       → fills SnesHdPpuPixelInfo per pixel

SnesHdVideoFilter
  ├── _paletteLut[]         → BGR555 → ARGB conversion (cached, rebuilds on settings change)
  └── ApplyFilter()         → tile lookup + alpha blend + fallback
```

## File Listing

| File | Purpose |
|------|---------|
| `Core/SNES/HdPacks/SnesHdData.h` | All data structures (tile key, pixel info, screen info, pack data) |
| `Core/SNES/HdPacks/SnesHdPackLoader.cpp/.h` | Loads manifest.json + PNG tiles from disk |
| `Core/SNES/HdPacks/SnesHdVideoFilter.cpp/.h` | Runtime video filter: tile replacement + blending |
| `Core/SNES/SnesPpu.cpp/.h` | PPU modifications for tile info collection |
| `Core/SNES/SnesConsole.cpp/.h` | Console-level integration (load, wire-up, filter selection) |

## Pack Directory Structure (Mesen2 Loader)

```
HdPacks/
  {romName}/
    manifest.json
    bg/
      bg1/ bg2/ bg3/ bg4/
        {vramAddr}_P{paletteIdx}.png      e.g. "4000_P03.png"
    sprites/
        {vramAddr}_P{paletteIdx}.png
```

**Filename format:** `{hexVramAddr}_P{decPalette}.png`
- `vramAddr`: 4-digit hex VRAM word address (0x0000-0x7FFF)
- `paletteIdx`: decimal palette group (0-7)
- Each PNG: 32x32 px (4x scale of 8x8 SNES tile)

## DKC2 Viewer Tool Compatibility

**Viewer:** [github.com/Beacher83/DKC2-HD-Tools](https://github.com/Beacher83/DKC2-HD-Tools)

The viewer's `exportAsTexturePack()` function exports tiles in a compatible format:

| Aspect | Viewer Export | Mesen2 Loader | Compatible? |
|--------|--------------|---------------|-------------|
| Filename | `{vramAddr}_P{pal}.png` | `{vramAddr}_P{pal}.png` | YES |
| Tile size | 32x32 (4x scale) | 32x32 (4x scale) | YES |
| VRAM addr | hex, from tilemap word bits 0-9 + chrBase | hex, parsed from filename | YES |
| Palette | `(tilemapData >> 10) & 7` | decimal 0-7 | YES |
| Directory | `textures/DONKEY_KONG_2/bg/bg1/` | `HdPacks/{romName}/bg/bg1/` | NEEDS RENAME |
| Flip handling | Exported un-flipped (canonical) | Emulator applies at runtime | YES |
| manifest.json | Full metadata with scale, game info | Not parsed yet (scale hardcoded 4) | OK for now |

### Action needed for test:
The viewer exports to `textures/DONKEY_KONG_2/bg/bg1/` but Mesen2 looks in `HdPacks/{romName}/bg/bg1/`.
To test: either rename the export directory, or update the viewer's export path.

The `romName` in Mesen2 comes from `_cart->GetCartName()` — für DKC2 ist das **`Donkey Kong Country 2`** (verifiziert aus SNES ROM-Header, offset 0xFFC0 / HiROM+SMC = 0x101C0).

## Upstream-Situation: MesenCE

**Stand: 2026-05-30**

Mesen2 (unsere Fork-Basis) ist seit Juli 2025 eingefroren. Die Community hat unter `nesdev-org/MesenCE` einen aktiven Fork gestartet — mit Beteiligung des Originalautors Sour (aktive Commits bis heute). Lizenz: GPL v3, identisch zu Mesen2.

**Relevanz für unser Projekt:**

- Unser SNES HD Pack-Code ist einzigartig — MesenCE hat keine SNES HD Packs
- Neue Bug-Fixes und Genauigkeitsverbesserungen kommen nur noch von MesenCE, nicht von Mesen2
- MesenCE hat am 26.05.2026 auf VS2022 v143-Toolset zurückgewechselt; unser v145-Build-Fix existiert dort nicht
- Je länger wir auf Mesen2-Basis bleiben, desto aufwändiger wird ein späterer Merge von MesenCE-Fixes

**Empfehlung (nach M5):** Fork-Basis von Mesen2 auf MesenCE wechseln (Rebase). Unser SNES HD-Code ist sauber isoliert und sollte konfliktarm übertragbar sein. MesenCE wäre auch das richtige Ziel für eine spätere Upstream-Contribution.

**Referenz:** https://github.com/nesdev-org/MesenCE

---

## Known Limitations & Design Notes

### Transparente Sprite-Pixel — "Clipping"-Effekt mit Test-Tiles

**Symptom:** HD BG-Tiles scheinen durch den Spieler-Sprite zu "clippen", besonders bei Bewegung.

**Ursache:** SNES Sprite-Tiles haben transparente Pixel (Color Index 0 = kein Sprite). In `FetchSpriteTile()` (`SnesPpu.cpp`) wird `_spritePriorityCopy[x]` nur gesetzt wenn `color != 0`. Transparente Sprite-Pixel lassen `_spritePriorityCopy[x] = 0xFF` (kein Sprite). An diesen Positionen gewinnt der BG-Layer korrekt — der HD-Filter wendet das HD-Tile an.

**Mit Original-SNES-Grafik:** Die nativen 8×8-BG-Tiles zeigen durch die transparenten Sprite-Lücken → sieht aus wie der Hintergrund → kaum sichtbar.  
**Mit Test-Tiles (Farbblöcke):** Die großen 32×32 Farbblöcke in den Sprite-Lücken → sieht aus wie ein Bug, ist aber korrektes SNES-Verhalten.

**"Bei Bewegung stärker":** Verschiedene Animations-Frames haben unterschiedliche Positionen transparenter Pixel, die mehr oder weniger mit HD-Tiles überlappen.

**Fix:** Kein Code-Fix nötig. Das verschwindet automatisch mit echten HD-Tiles (M5), weil dann durch die Sprite-Lücken der korrekte Hintergrund sichtbar ist statt eines Farbblocks. Langfristig: HD-Sprites für den Spielercharakter (future milestone).

---

### Allgemeine Hinweise

- Scale factor is defined in `manifest.json` (currently hardcoded to 4 in loader)
- Alpha blending is premultiplied for performance
- Fallback renders native resolution when no HD tile matches
- Hi-res modes (512px) are handled in the video filter
- `EnableHdPacks` setting in `SnesConfig` (default: `true`) controls whether HD filter is used
- `SnesHdTileKey` uses `LayerIndex=4` for sprites (no separate `IsSprite` flag)
- BG-Tile-Info wird nur für den Pixel-Gewinner-Layer gespeichert (`BgTiles[0]`, `BgTileCount` = 0 oder 1). `BgTileCount=0` = Sprite oder Backdrop hat gewonnen → Filter verwendet nativen SNES-Pixel.
