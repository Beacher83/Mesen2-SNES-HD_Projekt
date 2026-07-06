# Changelog — Mesen2 SNES HD Fork (Emulator C++ Code)

Dieses Changelog dokumentiert Änderungen am Mesen2-Fork (C++ Emulator-Code).
Für Änderungen am DKC2-HD-Tools Viewer siehe `DKC2-HD-Tools/CHANGELOG.md`.

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
