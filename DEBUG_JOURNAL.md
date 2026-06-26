# Debug Journal — SNES HD Pack (Mesen2 / DKC2)

Stand: 2026-06-23 | Mesen Build: M5.10 (tested)

---

## Open Issues

| # | Issue | Status | Seit | Letzer Test |
|---|-------|--------|------|-------------|
| A | BG3 native tiles transparent in Level 1 (Regression) | FIXED (M5.8) — verified M5.9+M5.10 | ~M5.5b | M5.10 OK |
| B | BG1/BG2 HD tiles fehlen in Level 2 (gfxset_25/0x37dez) | M5.10: BG1+BG2 OK, fog etwas zu dunkel | M5.2 | M5.10: BG1+BG2 sichtbar, fog 75/25 noch zu dunkel |
| C | Worldmap Tile-Kontamination | CLOSED | M5.1 | M5.7 (gefixt via vramSig-Sperre) |
| D | Performance Level 1 leicht schlechter | OFFEN — Optimierung geplant | M5.10 | M5.10: spielbar aber spürbar |

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
- WRAM-Adresse `$003E` (`!RAM_DKC2_Global_CurrentLevelLo` aus p4plus2 Disassembly)
- **NOCH NICHT VERIFIZIERT** — muss in-game getestet werden
- Falls falsch: Adresse im Script anpassen (Zeile `readCurrentLevelId()`)

**Output pro Gfxset (im Script Data Folder):**
- `gfxset_XX_vram.bin` — Viewer-Import-kompatibel
- `gfxset_XX_cgram.bin` — Paletten-Daten
- `gfxset_XX_state.txt` — PPU-Register + Metadaten
- `gfxset_XX.savestate` — für zukünftiges Batch-Re-Dumping

**Voraussetzungen:**
- Mesen Script Settings: "Allow I/O" aktivieren
- 102% Save-File (.srm) für Level-Zugang (oder Level-Select-Cheat)

**Offene Punkte nach erstem Test:**
- [ ] WRAM $003E verifizieren (zeigt HUD korrekte Level-ID?)
- [ ] ROM-Analyse: wie viele unique Gfxsets werden gefunden?
- [ ] Dump-Output prüfen: stimmt VRAM-Größe, sind Dateien valide?
- [ ] NMI-Adresse verifizieren (Savestate-Capture funktioniert?)
- [ ] Performance: Ist der Byte-für-Byte-Dump schnell genug?

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
