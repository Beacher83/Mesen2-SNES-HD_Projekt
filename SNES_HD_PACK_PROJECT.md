# SNES HD Pack Support for Mesen2 — Project Documentation

## Overview

Adding SNES HD texture pack support to Mesen2, modeled after the existing NES HD Packs system. Allows replacing SNES tiles at render time with high-resolution PNG replacements.

## Current Status

**Build:** Noch nicht neu gebaut (M4.2-Änderungen ausstehend).  
**Status: M4.2 implementiert — Rebuild erforderlich.** Fundamentaler Fix für HD-Tile-Priorität: nur der Pixel-Gewinner-BG-Layer wird in `BgTiles[0]` gespeichert.  
**Next Step (nach Rebuild-Verifikation):** M5 — DKC2 HD Viewer Tool-Exports als echte Tile-Ersetzungen integrieren.

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

### M5 — Echte HD-Tiles (NEXT, nach M4.2-Verifikation)
- **Rebuild + Test M4.1:** Core + UI neu bauen, DKC2 starten, verifizieren dass Diddy VOR den farbigen Test-Tiles läuft
- DKC2 HD Viewer Tool-Exports in `HdPacks/Donkey Kong Country 2/bg/bg1/` einspielen
- Export-Pfad im Viewer von `textures/DONKEY_KONG_2/` auf `HdPacks/{romName}/` anpassen
- Manifest-Parsing implementieren (aktuell Scale hardcoded auf 4)
- Vollständiges Level mit HD-Tiles verifizieren

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

The `romName` in Mesen2 comes from `_cart->GetCartName()` — for DKC2 this is likely `DONKEY KONG 2` or similar (from the SNES ROM header at offset $FFC0). Verify with a test ROM.

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
