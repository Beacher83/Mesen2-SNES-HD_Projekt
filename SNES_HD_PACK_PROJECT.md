# SNES HD Pack Support for Mesen2 — Project Documentation

## Overview

Adding SNES HD texture pack support to Mesen2, modeled after the existing NES HD Packs system. Allows replacing SNES tiles at render time with high-resolution PNG replacements.

## Current Status

**Stand: 2026-06-12**  
**M5.3 — Viewer-Patches vorbereitet, Mesen Fingerprint-System implementiert.**

**Mesen-Seite: ✅ FERTIG** (4 Dateien geändert, noch nicht gebaut)
- `SnesHdData.h`: Fingerprint-System, Gfxset-Scoping, DetectActiveGfxset()
  - **Bugfix (Review):** `GetMatchingTile()` Gfxset-Filter war fehlerhaft — wenn ActiveGfxset==-1
    (kein Gfxset erkannt, z.B. Worldmap), wurden gfxset-scoped Tiles nicht blockiert.
    Fix: `HasFingerprints() && GfxsetIndex != 0xFF` prüft jetzt korrekt alle Fälle.
- `SnesHdPackLoader.cpp`: LoadFingerprints(), GfxsetIndex-Zuweisung
- `SnesHdPackLoader.h`: LoadFingerprints() Deklaration
- `SnesHdVideoFilter.cpp`: DetectActiveGfxset() Aufruf in ApplyFilter()

**Viewer-Seite: Patch-Spezifikation FERTIG** (siehe `VIEWER_PATCH_M53.md`)
- 11 Änderungsbereiche dokumentiert mit exaktem Vorher/Nachher-Code
- Änderung 1: BG1 Hash-Fix (`chrRawData` → `vramSnapshot`)
- Änderung 2: BG1 vramWordAddr `& 0x7FFF` Maske
- Änderungen 3-6: BG3-Support in Container (Save/Refresh/Export/Import)
- Änderung 7: BG3 Tile-PNG-Export (2bpp, layer=2)
- Änderung 8: BG3 Hash-Berechnung in hashes.bin
- Änderung 9: fingerprints.bin Generierung (nur 4bpp Referenz-Tiles)
- Änderungen 10-11: Manifest + Alert-Update

**PPU-Verifikation: ✅ BG3 2bpp end-to-end kompatibel**
- PPU: `RenderTilemap<2,2,...>()` → `ComputeTileContentHash(vram, addr, 8)` → 16 bytes
- Viewer: `fnv1a_64(vram, addr*2, 16)` → 16 bytes
- Loader: `bg/bg3/gfxset_XX/` → layer=2 → korrekt
- Fingerprints: beschränkt auf 4bpp (BG1/BG2) wegen Default-wordCount=16

**Nächste Schritte:**
1. **Viewer-Patches anwenden** — `VIEWER_PATCH_M53.md` auf DKC2-HD-Tools anwenden
2. **Mesen bauen** — Fingerprint-Code kompilieren (benötigt VS)
3. **Container aktualisieren** — ROM laden, Refresh Metadata, BG3-Daten aufnehmen
4. **Re-Export + Test** — Pack neu exportieren, Level 2 + Worldmap testen
5. Bug #5 (BG2 Animation Flicker) — Known Limitation, geparkt
6. M6: HD-Tiles im Tile-Viewer-Debugger

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

### Multi-Gfxset-Architektur — IMPLEMENTIERT, ABGELÖST durch M5.2

**Commits:** Mesen2 `16dadecf`, Viewer `4331822`

> **Hinweis:** Diese Architektur (per-gfxset Unterordner + checksums.bin) war ein Zwischenschritt.
> M5.2 (Content-Hash-System) löst das Multi-Gfxset-Problem sauberer: statt VRAM-Checksummen zur
> Laufzeit-Disambiguation wird der Tile-Inhalt direkt als Hash-Key verwendet. Die Ordnerstruktur
> (`gfxset_XX/`) bleibt bestehen, aber `hashes.bin` ersetzt `checksums.bin` als primäres
> Identifikationsmittel.

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

### M5.2 — Content-Hash-System (2026-06-10, implementiert, Build/Test ausstehend)

**Problem:** VramAddress-basierte Tile-Keys versagen bei Multi-Gfxset-Spielen wie DKC2.
Verschiedene Levels laden verschiedene Grafiken an dieselbe VRAM-Adresse via DMA.
Die M5.1-Lösung (checksums.bin) erforderte Laufzeit-Prüfung gegen VRAM-Inhalt bei
jedem Tile-Lookup — funktionierte, aber war konzeptionell fragil.

**Lösung:** Tile-Identifikation durch **FNV-1a 64-bit Content-Hash** der VRAM-Bytes.
Inspiriert vom NES HD Pack CHR RAM-Modus (`HdData.h`), wo der Tile-Inhalt selbst der
Schlüssel ist (nicht die Adresse).

**Algorithmus (identisch in C++ und JavaScript):**
```
hash = 0xcbf29ce484222325 (FNV offset basis)
prime = 0x100000001b3
for each byte in tile VRAM data:
    hash = (hash XOR byte) * prime, masked to 64 bits
```

**hashes.bin Format (ersetzt checksums.bin):**
```
uint32_t count                    (4 Bytes, little-endian)
count × {
    uint16_t vramAddr             (2 Bytes) — für Zuordnung zum PNG-Dateinamen
    uint8_t  layerIndex           (1 Byte)
    uint8_t  gfxsetIndex          (1 Byte)
    uint64_t contentHash          (8 Bytes) — FNV-1a 64-bit
}
```
Entry size: 12 Bytes pro Eintrag.

**Geänderte Dateien (Mesen2):**

1. **`SnesHdData.h`:**
   - `ComputeTileContentHash()` (L26-44): Inline-Funktion, FNV-1a 64-bit über VRAM-Bytes
   - `SnesHdTileKey`: Neues Feld `ContentHash` (uint64_t), dual-mode `GetHashCode()` und `operator==`
   - `SnesHdPackData::UseContentHash`: Flag für Content-Hash-Modus
   - `GetMatchingTile()`: Fast-Path für Content-Hash (kein VRAM-Zugriff nötig)

2. **`SnesHdPackLoader.h`:**
   - `_hashMap` (uint32_t → uint64_t): Content-Hashes aus hashes.bin
   - `_useContentHash` Flag, `LoadHashes()` Deklaration

3. **`SnesHdPackLoader.cpp`:**
   - `LoadPack()`: Versucht `LoadHashes()`, Fallback auf `LoadChecksums()`
   - `LoadHashes()`: Parst hashes.bin (12 Bytes/Eintrag)
   - `LoadTilesFromDirectory()`: Content-Hash-Modus setzt `tile->Key.ContentHash` statt `VramAddress`

4. **`SnesPpu.cpp`:**
   - `RenderTilemap()`: Berechnet Content-Hash live via `ComputeTileContentHash(_vram, addr, 4*bpp)`

**Geänderte Dateien (Viewer — dkc2-viewer/index.html):**
- `fnv1a_64()`: FNV-1a 64-bit Hash-Funktion (BigInt)
- `saveCurrentHDToContainer()`: Speichert `chrRawData` + `vramSnapshot`
- `exportAsTexturePack()`: Generiert `hashes.bin` statt `checksums.bin`
- `refreshContainerSetMetadata()`: Aktualisiert Container-Metadaten ohne Tile-Re-Upload

**Rückwärtskompatibilität:**
- `hashes.bin` vorhanden → Content-Hash-Modus
- Nur `checksums.bin` → Legacy-Checksum-Modus (wie M5.1)
- Keines vorhanden → Legacy VramAddress-Modus (first-match)
- Kein Crash bei fehlenden Dateien

### M5.3 — Tiefenanalyse: Warum M5.2 bei Level 2 und Worldmap scheitert (2026-06-12)

**Kontext:** M5.2 (Content-Hash) funktionierte für Level 1 (gfxset_07), aber komplett
nicht für Level 2 (gfxset_37) und zeigte falsche Tiles auf der Worldmap. Die Analyse
deckte drei voneinander unabhängige Probleme auf und erforderte einen detaillierten
Vergleich der NES- und SNES-Architekturen.

#### Problem 1: BG1-Hash-Datenquelle im Viewer (chrRawData ≠ VRAM)

**Symptom:** hash_miss.log zeigt 0/60 MISS-Hashes aus gfxset_37 in hashes.bin gefunden.

**Root Cause:** Der Viewer berechnet BG1-Hashes aus `chrRawData[gfxIndex * 32]` — einem
sequentiellen Tile-Buffer aus ROM-Decompression. Mesen berechnet Hashes aus
`_vram[vramWordAddr]` — den tatsächlichen VRAM-Bytes an der Hardware-Adresse.

Für gfxset_07 stimmen diese zufällig überein (Tiles werden in gleicher Reihenfolge an
gleiche Adressen geladen). Für gfxset_37 weichen die Bytes vollständig ab →
verschiedene Hashes → 0% Match-Rate.

**Beweis:** hash_load.log und hash_miss.log zeigen null Überlappung zwischen
Viewer-berechneten und Mesen-berechneten Hashes für gfxset_37.

**Fix (Viewer):** BG1-Hash-Berechnung von `chrRawData` auf `vramSnapshot` umstellen.
BG2 verwendet bereits `vramSnapshot` und funktioniert korrekt — BG1 muss denselben
Ansatz verwenden.

#### Problem 2: Palette-Mismatch im Viewer-Export

**Symptom:** Selbst wenn die Hashes übereinstimmen würden, passen die Paletten nicht.

**Root Cause:** 
- hash_load.log: gfxset_37 BG1-Tiles exportiert mit P06/P04
- hash_miss.log: Mesen sucht P02 bei Level-2-Start
- Der Lookup-Key ist `(Hash, Palette, Layer)` → `(Hash, P06, layer0)` ≠ `(Hash, P02, layer0)`

**Ursache im Viewer:** Der Dedup-Key `${gfxset}_0_${vramWordAddr}` enthält keine Palette →
nur die erste gefundene Palette-Variante wird exportiert. Tiles die in verschiedenen
Levels mit verschiedenen Paletten verwendet werden, fehlen.

**Fix (Viewer):** Palette-Index in den Dedup-Key aufnehmen:
`${gfxset}_0_${vramWordAddr}_P${paletteIdx}`. Zusätzlich: Tilemaps ALLER Levels eines
Gfxsets iterieren, um alle vorkommenden Palette-Varianten zu erfassen.

#### Problem 3: Worldmap False Positives (kein Gfxset-Kontext)

**Symptom:** Worldmap zeigt Level-HD-Tiles an falschen Stellen.

**Root Cause:** Der Content-Hash-Lookup ist gfxset-agnostisch: Key = `(ContentHash,
Palette, Layer)` ohne Gfxset-Scoping. Wenn ein Worldmap-VRAM-Tile zufällig denselben
Byte-Inhalt hat wie ein Level-Tile, wird das Level-HD-Tile eingesetzt.

**Warum das ein Architektur-Problem ist:** Content-Hash identifiziert Tiles korrekt
INNERHALB eines Gfxsets. Aber verschiedene Gfxsets können Tiles mit identischem Inhalt
an verschiedenen Stellen haben, die semantisch verschiedene Dinge darstellen (z.B.
ein Worldmap-Tile vs. ein Level-Hintergrund-Tile).

**Fix (Mesen + Viewer): Gfxset-Fingerprint-System** (siehe Lösungsarchitektur unten)

#### NES vs SNES Architekturvergleich (Kernerkenntniss)

Die Wahl von `chrRawData` als BG1-Hash-Quelle basierte auf einem Missverständnis
der NES HD Pack-Architektur:

| Aspekt | NES CHR ROM | NES CHR RAM | SNES |
|--------|-------------|-------------|------|
| Speicher | ROM, unveränderlich | RAM, DMA-befüllt | VRAM, DMA-befüllt |
| Tile-Key | Absoluter ROM-Offset / 16 | **16 Bytes Raw-Tile-Inhalt** | ? |
| Datenquelle | `chrPagePointers[]` | `CopyChrTile()` → `_chrRam[]` | `_vram[]` |
| Content-Check | Nicht nötig (ROM) | **JA** (TileData[16]) | **JA** |

**Kritische Erkenntnis:** SNES ist zu 100% ein CHR-RAM-Fall. Es gibt kein CHR ROM.
Die korrekte Analogie ist der NES CHR RAM-Pfad in `HdData.h`:

```cpp
// NES HdData.h — CHR RAM Modus:
struct HdPackTileInfo {
    int32_t TileIndex = -1;       // -1 = CHR RAM mode
    uint8_t TileData[16];         // Die 16 RAW BYTES des Tiles
    // ...
};
// Matching in HdNesPack.cpp:
// memcmp(tileData, info.TileData, 16)
```

Der NES-Code liest die Tile-Daten aus `CopyChrTile()` → `memcpy(_chrRam + address, ...)`
— dem **live CHR RAM**, nicht ROM-decomprimierten Daten. Genau dies entspricht
`_vram[]` auf der SNES-Seite und `vramSnapshot` auf der Viewer-Seite.

`chrRawData` (ROM-Decompression-Buffer) hat **kein NES-Äquivalent** und war die
falsche Wahl.

#### VRAM-Byte-Ordering-Verifikation

Bestätigt: Die Byte-Reihenfolge ist identisch zwischen Mesen und Viewer.

```
DKC2 DMA (Mode 1):
  B0 → $2118 (VRAM Low)
  B1 → $2119 (VRAM High)
  → _vram[addr] = (B1 << 8) | B0

Mesen (C++, x86 little-endian):
  reinterpret_cast<uint8_t*>(_vram + addr) → B0, B1, B2, B3, ...

Viewer (JavaScript):
  vramSnapshot (Uint8Array) → B0, B1, B2, B3, ...
```

Byte-Reihenfolge ist identisch → Hashes sind kompatibel. Dies erklärt auch,
warum BG2 (bereits auf vramSnapshot basierend) korrekt funktioniert.

#### Viewer-Datenfluss-Analyse

Alle Viewer-Daten sind **level-abhängig:**
- `loadLevel()` erzeugt frisches VRAM, Paletten, Tilemaps pro Level
- Container-Save speichert den Zustand des aktuell geladenen Levels
- `vramSnapshot` und `chrRawData` stammen aus dem aktuell geladenen Level

**ABER:** CHR-Tile-Bytes im VRAM sind **gfxset-abhängig, nicht level-abhängig.**
Alle Levels mit identischem `style.graphics`-Wert bekommen identische CHR-Daten
ins VRAM. Unterschiede liegen in Tilemaps und Paletten.

`buildCatalogByGfxSet()` erzeugt VRAM lokal, aktualisiert aber NICHT das globale
`currentBgData`/`currentTileRawData` — was ein Bug-Risiko darstellt wenn man
zwischen Catalog- und Level-Modus wechselt.

#### Lösungsarchitektur

**Phase 1 — Level-2-Fix (nur Viewer-Änderungen):**
1. BG1-Hash-Berechnung: `chrRawData` → `vramSnapshot` (analog zu BG2)
2. Dedup-Key um Palette-Index erweitern
3. ALLE Level-Tilemaps pro Gfxset iterieren für vollständige Palette-Abdeckung

**Phase 2 — Worldmap-Fix (Mesen + Viewer):**
1. **Gfxset-Fingerprint-System:** Pro Gfxset 4-8 Reference-Tile-Hashes mit
   einzigartigem Inhalt, gespeichert in `fingerprints.bin`
2. Mesen erkennt den aktiven Gfxset einmal pro Frame durch Prüfung der
   Reference-Tile-Hashes gegen live VRAM
3. Tile-Lookup nur innerhalb des aktiven Gfxsets → keine False Positives
4. Trennung: "Welcher Tile-Satz ist geladen?" (Fingerprint) vs.
   "Welches Tile ersetzen?" (Content-Hash innerhalb des aktiven Gfxsets)

**Vergleich der drei Identifikationsansätze:**

| Merkmal | Checksum (M5.1) | Content-Hash (M5.2) | Fingerprint (Phase 2) |
|---------|-----------------|---------------------|-----------------------|
| Prinzip | Sekundärfilter auf Adress-Key | Inhalt = Identität | Gfxset erkennen, dann scoped Lookup |
| Gfxset-Kontext | Nein (alle im selben Key-Space) | Nein (gleiche Keys) | Ja (explizite Erkennung) |
| False Positives | Möglich (Byte-Summenkollision) | Möglich (cross-gfxset) | Ausgeschlossen |
| VRAM-Zugriff/Frame | Jeder Tile-Lookup | Jeder Tile-Lookup | Nur Fingerprint-Check (4-8 Tiles) |
| Datenquelle-Bug | Viewer: Byte/Word-Indexierung | Viewer: chrRawData statt VRAM | Nicht betroffen |

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
                               Content-Hash mode: ComputeTileContentHash() → Key.ContentHash
                               Legacy mode: Key.VramAddress

SnesHdVideoFilter
  ├── _paletteLut[]         → BGR555 → ARGB conversion (cached, rebuilds on settings change)
  └── ApplyFilter()         → tile lookup + alpha blend + fallback
                               Calls GetMatchingTile(key) — O(1) hash map lookup

SnesHdPackData
  ├── TileByKey             → unordered_map<SnesHdTileKey, vector<TileInfo*>>
  ├── UseContentHash        → true = hashes.bin loaded, false = legacy mode
  └── GetMatchingTile()     → Content-Hash: direct match (no VRAM access needed)
                               Legacy: checksum disambiguation against live VRAM
```

### Tile Identification: Dual-Mode Key

```
SnesHdTileKey {
    ContentHash (uint64_t)  ← FNV-1a 64-bit of tile VRAM bytes (0 = legacy)
    VramAddress (uint16_t)  ← VRAM word address (used when ContentHash == 0)
    PaletteIndex (uint8_t)  ← Palette group 0-7
    LayerIndex (uint8_t)    ← 0-3 = BG1-BG4, 4 = Sprites
}
```

**Data flow (content hash mode):**
1. Viewer: tile CHR bytes → `fnv1a_64()` → hash stored in `hashes.bin`
2. Loader: reads `hashes.bin` → `_hashMap[(gfxset,layer,addr)] = hash`
3. Loader: for each PNG, looks up hash → sets `tile->Key.ContentHash`
4. PPU runtime: `ComputeTileContentHash(_vram, addr, wordCount)` → `key.ContentHash`
5. Filter: `TileByKey.find(key)` → O(1) lookup, no VRAM comparison needed

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
