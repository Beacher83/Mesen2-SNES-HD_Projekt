# HD Compositing Engine — Architecture

> Branch: `feature/hd-compositing-engine`
> Predecessor: `v0.1-sonderfall-m5.19` (tag) — per-level-type special cases

## Why This Exists

The original Mesen2 SNES HD pack system inherited the NES design: find a matching
tile by hash, replace the pixel, done. But SNES has complex post-tile compositing
that NES doesn't:

- **Color Math** (ADD/SUB/HALF) — fog, honey glow, lava, ice
- **Color Window** (Clip/Prevent) — fog boundaries, spotlights
- **Sub-Screen blending** — Hot-Head Hop BG1 on sub-screen
- **HDMA-animated registers** — per-scanline fog gradients, window movement
- **Brightness** — fade-in/out transitions

The old code tried to reverse-engineer these effects from limited per-pixel info
using heuristics (e.g., `frameBg3FogSkip == 0` means beehive, `> 0` means fog).
Each new level type needed a new heuristic, and fixes for one type caused
regressions in others (M5.17→M5.19 beehive regression from fog fix).

**The new approach:** Forward the PPU's own register state to the HD filter and
let it reproduce the compositing pipeline at HD resolution. No heuristics, no
special cases — one general-purpose engine handles all level types.

---

## Data Flow

```
PPU Register Writes ($2100-$2133, HDMA-animated per scanline)
    |
    v
SnesPpuState (_state) — all registers decoded, updated by HDMA at HBlank
    |
    v
RenderScanline()
  |-- RenderModeX() / RenderSprites() / RenderBgColor()
  |     -> fills _mainScreenBuffer[], _subScreenBuffer[], _mainScreenFlags[]
  |     -> populates SnesHdScreenInfo->ScreenTiles[] (per-pixel tile data for ALL layers)
  |
  |-- NEW: Snapshot _state into SnesHdScreenInfo->ScanlineInfo[scanline]
  |     -> per-scanline register state (color math, windows, brightness)
  |
  |-- ApplyColorMath() + ApplyBrightness()
  |     -> modifies buffers in-place (native-res compositing)
  |
  v
SendFrame()
  -> attaches SnesHdScreenInfo to RenderedFrame (double-buffered swap)
    |
    v
SnesHdVideoFilter::ApplyFilter()
  -> receives ppuOutputBuffer (post-math, native-res fallback)
  -> receives SnesHdScreenInfo (all HD data + scanline register state)
  -> For each pixel:
       1. Look up HD tiles for ALL layers (not just compositing winner)
       2. Composite HD layers in correct priority order
       3. Apply color math at HD resolution using ScanlineInfo
       4. Apply color window at HD resolution
       5. Apply brightness
  -> produces ARGB8888 output at HD scale
```

---

## Key Data Structures

### SnesHdScanlineInfo (NEW — Phase 1)

**File:** `Core/SNES/HdPacks/SnesHdData.h`

Per-scanline snapshot of PPU registers relevant to HD compositing.
Captured in `SnesPpu::RenderScanline()` before `ApplyColorMath()`.

| Field | SNES Register | Purpose |
|-------|---------------|---------|
| `ColorMathEnabled` | $2131 bits 0-5 | Which layers get color math |
| `ColorMathSubtractMode` | $2131 bit 7 | ADD or SUB |
| `ColorMathHalveResult` | $2131 bit 6 | Halve the result |
| `ColorMathAddSubscreen` | $2130 bit 1 | Sub-screen or fixed color source |
| `ColorMathClipMode` | $2130 bits 6-7 | Clip color to black (window) |
| `ColorMathPreventMode` | $2130 bits 4-5 | Prevent color math (window) |
| `FixedColor` | $2132 | BGR555, HDMA-animated (fog gradient) |
| `ScreenBrightness` | $2100 bits 0-3 | 0-15 brightness multiplier |
| `MainScreenLayers` | $212C | Layer visibility bitmask (main) |
| `SubScreenLayers` | $212D | Layer visibility bitmask (sub) |
| `Window1Left/Right` | $2126/$2127 | Window 1 boundaries |
| `Window2Left/Right` | $2128/$2129 | Window 2 boundaries |
| `ColorWindowActive[2]` | $2123-$2125 | Window 1/2 active for color layer |
| `ColorWindowInverted[2]` | $2123-$2125 | Window 1/2 inverted for color layer |
| `ColorWindowMaskLogic` | $2125 | How to combine Window 1+2 |

### SnesHdScreenInfo (EXTENDED — Phase 1)

**File:** `Core/SNES/HdPacks/SnesHdData.h`

Now contains:
- `ScreenTiles[256*239]` — per-pixel tile data (existing, unchanged)
- `ScanlineInfo[239]` — **NEW** per-scanline register snapshot
- `Vram` pointer, `FrameNumber` (existing, unchanged)

Cleared via `memset` in `SendFrame()` when swapping double buffers.

### SnesHdPpuPixelInfo (EXISTING — unchanged in Phase 1)

Per-pixel data already available for HD compositing:
- `BgTiles[4]` — tile info for ALL 4 BG layers (not just winner)
- `BgLayerMask` — which layers have non-transparent pixels
- `BgWinnerLayer` — which BG layer won PPU compositing
- `MainScreenColor` — pre-color-math BGR555 (brightness-scaled)
- `SubScreenColor` — sub-screen pixel
- `MainScreenFlags` — AllowColorMath, IsSubtract, IsSpritePixel
- `Sprites[4]` — sprite tile data

---

## PPU Code Map (Recherche-Ergebnisse)

Detaillierte Analyse des Mesen2 SNES PPU Codes. Referenz für alle Phasen.

### Rendering-Pipeline (SnesPpu.cpp)

#### Haupteinstieg: RenderScanline() — Zeile 874

Wird pro Scanline aufgerufen (und bei mid-scanline Register-Writes für
Teilbereiche). Ablauf:

```
RenderScanline()
 ├── Zeile 878-883: Sprite Evaluation (EvaluateNextLineSprites)
 ├── Zeile 886-892: Tile Data Fetch (FetchTileData)
 ├── Zeile 896-950: Pixel Rendering
 │    ├── Zeile 899-902: Forced Blank → schwarze Pixel
 │    ├── Zeile 904-912: RenderModeX() nach BgMode (0-7)
 │    ├── Zeile 914: RenderBgColor() — Backdrop füllen
 │    ├── Zeile 918: hdScanline berechnen (Overscan-Offset)
 │    ├── Zeile 920-953: *** NEU: ScanlineInfo Snapshot ***
 │    ├── Zeile 959-970: HD Pre-Math: MainScreenColor speichern (brightness-scaled)
 │    ├── Zeile 972: ApplyColorMath()
 │    ├── Zeile 973: ApplyBrightness<true>()
 │    ├── Zeile 974: ApplyHiResMode()
 │    └── Zeile 977-983: HD Post-Math: MainScreenFlags + SubScreenColor speichern
 └── Zeile 953+: Sprite CHR Fetch
```

#### Wo wird RenderScanline() aufgerufen?

| Zeile | Kontext |
|-------|---------|
| **413** | `ProcessEndOfScanline()` — Hauptaufruf am Ende jeder sichtbaren Scanline |
| **1650** | `DebugSendFrame()` — erzwingt Render wenn Scanline < VBlank |
| **1831** | Mid-Frame Register-Write Handler (rendert Teilscanline VOR Write) |
| **2003** | Weiterer Mid-Frame Register-Write Handler |

#### HD-Scanline-Offset-Formel

```cpp
uint16_t hdScanline = _overscanFrame ? (_scanline - 1) : (_scanline + 6);
```
Identisch an allen Stellen verwendet. Gültigkeitsbereich: `0 .. ScreenHeight-1` (0..238).

### Per-Pixel HD-Daten: Wo befüllt?

#### BgTiles[], BgLayerMask, BgWinnerLayer — RenderTilemap() Zeile 1122-1153

Innerhalb der Tilemap-Pixel-Schleife, für JEDEN nicht-transparenten BG-Pixel
auf JEDEM aktiven Layer (nicht nur der Compositing-Winner):

```cpp
// Zeile 1122-1153 (vereinfacht):
if((drawMain || drawSub) && hdValid && x < ScreenWidth) {
    SnesHdPpuPixelInfo& pixelInfo = _hdActiveScreen->ScreenTiles[...];
    SnesHdPpuTileInfo& tileInfo = pixelInfo.BgTiles[layerIndex];  // ← ALLE Layer
    tileInfo.Key.VramAddress = vramAddr;
    tileInfo.Key.ContentHash = ComputeTileContentHash(vram, ...);
    tileInfo.Key.PaletteIndex = palette;
    tileInfo.Key.LayerIndex = layerIndex;
    tileInfo.OffsetX = offsetX;
    tileInfo.OffsetY = offsetY;
    tileInfo.HorizontalMirror = hMirror;
    tileInfo.VerticalMirror = vMirror;
    tileInfo.Priority = priority;    // ← Tilemap Bit 13
    tileInfo.TilemapData = tilemapData;
    tileInfo.VramWordAddr = vramAddr;
    pixelInfo.BgLayerMask |= (1 << layerIndex);   // ← Bitmask
    if(winsMain) {
        pixelInfo.BgWinnerLayer = layerIndex;       // ← Winner
    }
}
```

**Wichtig:** `BgTiles[4]` enthält Daten für ALLE Layer die Content haben,
nicht nur den Winner. Das ist die Grundlage für Phase 2 Multi-Layer-Lookup.

#### MainScreenColor — Zeile 959-970 (HD Pre-Math Block)

Gespeichert VOR `ApplyColorMath()`, mit Brightness bereits eingerechnet:
```cpp
uint16_t color = _mainScreenBuffer[x];
if(_state.ScreenBrightness != 15) {
    // BGR555 Brightness-Skalierung pro Kanal
    color = r | (g << 5) | (b << 10);
}
pixelInfo.MainScreenColor = color;
```

#### MainScreenFlags, SubScreenColor — Zeile 977-983 (HD Post-Math Block)

Gespeichert NACH `ApplyColorMath()` und `ApplyBrightness()`:
```cpp
px.MainScreenFlags = _mainScreenFlags[x];   // AllowColorMath | IsSubtract | IsSpritePixel
px.SubScreenColor = _subScreenBuffer[x];
```

#### SubScreenHasSprite — RenderSprites() Zeile 1016-1019

```cpp
pixelInfo.SubScreenHasSprite = true;  // gesetzt wenn Sprite Sub-Screen gewinnt
```

### Color Math Implementierung (Referenz für Phase 3)

#### ApplyColorMath() — Zeile 1369

Aufgerufen aus `RenderScanline()` Zeile 972. Schleife über `_drawStartX`
bis `_drawEndX`, ruft `ApplyColorMathToPixel()` pro Pixel.

#### ApplyColorMathToPixel() — Zeile 1399-1476

Die zentrale Funktion die wir in Phase 3 auf HD-Auflösung reimplementieren.
Schritt-für-Schritt:

```
1. Zeile 1401: halfShift = _state.ColorMathHalveResult (0 oder 1)

2. Zeile 1404-1423: CLIP TO BLACK
   switch(_state.ColorMathClipMode):
     Never    → kein Clip
     Always   → pixelA = 0 (schwarz), halfShift = 0
     Inside   → if(isInsideWindow) pixelA = 0, halfShift = 0
     Outside  → if(!isInsideWindow) pixelA = 0, halfShift = 0

3. Zeile 1425-1428: ALLOW CHECK
   if(!(mainScreenFlags & AllowColorMath)) → return (kein Color Math)

4. Zeile 1431-1448: PREVENT COLOR MATH
   switch(_state.ColorMathPreventMode):
     Never    → Color Math erlaubt
     Always   → return (immer verhindern)
     Inside   → if(isInsideWindow) return
     Outside  → if(!isInsideWindow) return

5. Zeile 1451-1461: ZWEITEN OPERANDEN BESTIMMEN
   if(_state.ColorMathAddSubscreen):
     otherPixel = _subScreenBuffer[x]
     if(subscreen == 0 → kein Sub-Pixel) otherPixel = 0, halfShift = 0
   else:
     otherPixel = _state.FixedColor

6. Zeile 1464-1476: ARITHMETIK
   if(_state.ColorMathSubtractMode):
     R = max(0, R_main - R_other) >> halfShift
     G = max(0, G_main - G_other) >> halfShift
     B = max(0, B_main - B_other) >> halfShift
   else:
     R = min(31, R_main + R_other) >> halfShift
     G = min(31, G_main + G_other) >> halfShift
     B = min(31, B_main + B_other) >> halfShift
   pixelA = R | (G << 5) | (B << 10)
```

#### ApplyBrightness() — Zeile 1480-1488

Template-Funktion, aufgerufen NACH Color Math:
```cpp
if(_state.ScreenBrightness != 15) {
    for each pixel:
        R = R * brightness / 15
        G = G * brightness / 15
        B = B * brightness / 15
}
```

### Wo die PPU-Register gelesen werden

#### ColorMathEnabled ($2131 bits 0-5)

| Zeile | Kontext | Verwendung |
|-------|---------|------------|
| **966** | `RenderBgColor()` | Backdrop: `(_state.ColorMathEnabled & 0x20)` → AllowColorMath Flag |
| **1004** | `RenderSprites()` | Sprites: `(_state.ColorMathEnabled & 0x10) && palette > 3` |
| **1049** | `RenderTilemap()` | BG Layer: `(_state.ColorMathEnabled >> layerIndex) & 0x01` |
| **1287** | `RenderTilemapMode7()` | Mode 7: gleich wie 1049 |

Bit-Zuordnung: `bit0=BG1, bit1=BG2, bit2=BG3, bit3=BG4, bit4=OBJ, bit5=Backdrop`

#### FixedColor ($2132)

| Zeile | Verwendung |
|-------|------------|
| **1456** | Fallback wenn Sub-Screen leer (kein Pixel) |
| **1460** | Explizit gewählt wenn `ColorMathAddSubscreen == false` |

BGR555 Format. HDMA-animiert in DKC2 für Nebelgradienten (pro Scanline anderer Wert).

#### Weitere Register

| Register | Feld | Gelesen in |
|----------|------|------------|
| $2130 bit 1 | `ColorMathAddSubscreen` | Zeile 1451 |
| $2131 bit 6 | `ColorMathHalveResult` | Zeile 1401 |
| $2131 bit 7 | `ColorMathSubtractMode` | Zeile 966, 1004, 1049, 1287, 1464 |
| $2130 bits 6-7 | `ColorMathClipMode` | Zeile 1404 |
| $2130 bits 4-5 | `ColorMathPreventMode` | Zeile 1431 |
| $2100 bits 0-3 | `ScreenBrightness` | Zeile 927-931 (HD pre-math), 1482-1488 (post) |

### SendFrame() und Double-Buffering — Zeile 1597-1645

```
SendFrame()
 ├── Zeile 1618: RenderedFrame erstellen
 ├── Zeile 1621-1638: HD-Daten anhängen
 │    ├── Zeile 1625-1630: Blanked Rows nullen (top 7, bottom 8)
 │    ├── Zeile 1632: FrameNumber setzen
 │    ├── Zeile 1633: frame.Data = _hdActiveScreen  ← HD-Daten an Frame anhängen
 │    ├── Zeile 1635: Buffer-Swap (nächster Frame schreibt in anderen Buffer)
 │    ├── Zeile 1637: ScreenTiles des neuen Buffers nullen
 │    └── Zeile 1672: *** NEU: ScanlineInfo des neuen Buffers nullen ***
 └── Zeile 1640: VideoDecoder::UpdateFrame() — Frame an Renderer übergeben
```

Die HD-Daten werden via `frame.Data` (void*) an den VideoDecoder übergeben,
der sie an `SnesHdVideoFilter::ApplyFilter()` weiterreicht.

### PPU-Effekte: Was handled die PPU bereits vs. was muss die HD-Engine reproduzieren?

#### Bereits von der PPU gehandled (transparent für HD-Engine)

Diese Effekte sind in den Tile-Daten (`BgTiles[]`) bereits korrekt abgebildet:

- **Scrolling** — BG Layer Scroll ($210D-$2114), HDMA-animiert für Parallax
- **Tile Flip** — Horizontal/Vertical Mirror (Tilemap Bits 14-15)
- **BG Layer Windowing** — Tile wird nur gerendert wenn Window erlaubt
- **Mosaic** — Blockige Vergrößerung (selten in DKC2)
- **Palette Lookup** — CGRAM Index → BGR555 Farbe
- **Sprite Compositing** — OAM Priority, Sprite-Layer-Interaktion
- **Offset-Per-Tile** — Mode 2/4/6 BG3-gesteuerte Tile-Offsets

#### Muss die HD-Engine reproduzieren (Phase 2-4)

Diese Effekte passieren NACH der Tile-Auswahl und sind im `BgTiles[]`-Array
nicht abgebildet — nur im finalen `MainScreenColor`/`SubScreenColor`:

1. **Color Math (ADD/SUB/HALF)** — $2131, $2132
   - Alle 6 ppuConfig-Typen verwenden dies
   - Fog, Honey, Lava, Ice, Ghost, Canopy
2. **Color Math Source** — $2130 bit 1
   - Sub-Screen (ppuCfg $03: BG1 auf Sub-Screen) vs. Fixed Color
3. **Color Window (Clip/Prevent)** — $2130 bits 4-7, $2125-29
   - Nebel-Grenzen, Spotlights, Dampf-Bereiche
4. **Brightness** — $2100
   - Fade-In/Out bei Level-Übergang
5. **Layer Priority** — $2105, Tilemap Bit 13
   - BG3 vor/hinter Sprites (Mode1Bg3Priority)
6. **Main/Sub Screen Designation** — $212C, $212D
   - Welche Layer auf Main/Sub Screen sichtbar

#### Nicht relevant (Mode 1 / DKC2)

- Mode 7 (SNES Mode 7 Rotation/Skalierung) — DKC2 nutzt Mode 1
- Hi-Res Mode (512px) — nicht in DKC2
- Direct Color Mode — nicht in Mode 1
- EXTBG — nur Mode 7
- OBJ Interlace — nicht in DKC2

### PPU State Struct (SnesPpuTypes.h, Zeile 127-190)

Vollständige `SnesPpuState` Struktur — alle dekodierten PPU-Register:

```cpp
struct SnesPpuState {
    // Timing
    uint16_t Cycle, Scanline, HClock;
    uint32_t FrameCount;
    bool ForcedBlank;
    uint8_t ScreenBrightness;          // $2100 [0-15]

    // BG Config
    uint8_t BgMode;                     // $2105 [0-7]
    bool Mode1Bg3Priority;              // $2105 bit 3
    uint8_t MainScreenLayers;           // $212C
    uint8_t SubScreenLayers;            // $212D
    LayerConfig Layers[4];              // $2107-$210A (tilemap), $210B-$210C (chr)

    // Windows
    WindowConfig Window[2];             // $2126-$2129 (positions), $2123-$2125 (enable)
    WindowMaskLogic MaskLogic[6];       // $212A-$212B (BG1-4, OBJ, Color)
    bool WindowMaskMain[5];             // $212E
    bool WindowMaskSub[5];             // $212F

    // Color Math
    ColorWindowMode ColorMathClipMode;      // $2130 bits 6-7
    ColorWindowMode ColorMathPreventMode;   // $2130 bits 4-5
    bool ColorMathAddSubscreen;             // $2130 bit 1
    uint8_t ColorMathEnabled;               // $2131 bits 0-5
    bool ColorMathSubtractMode;             // $2131 bit 7
    bool ColorMathHalveResult;              // $2131 bit 6
    uint16_t FixedColor;                    // $2132 BGR555

    // ... (VRAM, OAM, CGRAM, Mode 7 — nicht relevant für HD-Engine)
};
```

### Window-Evaluation (SnesPpuTypes.h, Zeile 101-125)

```cpp
struct WindowConfig {
    bool ActiveLayers[6];    // 0-3: BG1-4, 4: OBJ, 5: Color
    bool InvertedLayers[6];
    uint8_t Left, Right;     // Window-Grenzen (inklusive)

    template<uint8_t layerIndex>
    bool PixelNeedsMasking(int x) {
        if(InvertedLayers[layerIndex]) {
            if(Left > Right) return true;          // Window "disabled"
            return x < Left || x > Right;          // außerhalb
        } else {
            if(Left > Right) return false;         // Window "disabled"
            return x >= Left && x <= Right;        // innerhalb
        }
    }
};
```

Für die HD-Engine (Phase 4): Die gleiche Logik, aber `x` ist die SNES-Koordinate
(HD x-Koordinate / Scale). Keine Sub-Pixel-Window-Auswertung nötig.

---

## DKC2 ppuConfig Values

All 6 special screen blend configurations in DKC2, with the PPU register
values that `SnesHdScanlineInfo` captures:

| ppuCfg | Effect | Key Registers | Levels |
|--------|--------|----------------|--------|
| $03 | Honey glow (BG1 on sub-screen, ADD) | ColorMathAddSubscreen=1, SubScreenLayers has BG1 | 13 beehive variants |
| $24 | Mine fog (BG3 ADD with fixed color) | ColorMathEnabled bit 2, FixedColor animated | 9 mine variants |
| $29 | Steam haze (windowed ADD) | ColorWindow active, FixedColor | Red-Hot Ride |
| $2C | Ghost overlay (windowed ADD) | ColorWindow active, FixedColor | 5 haunted variants |
| $31 | Ice fog (BG3 ADD with fixed color) | ColorMathEnabled bit 2, FixedColor animated | 4 ice variants |
| $35 | Forest canopy (windowed ADD) | ColorWindow active, FixedColor | 3 forest variants |

The new engine handles ALL of these with the same code path — no per-ppuConfig
special cases needed.

### Detailed PPU Analysis: Overlay Levels (from DKC2 disassembly)

Source: `p4plus2/DKC2-disassembly`, tables at `DATA_FD79E2` (58 PPU profiles),
HDMA dispatch at `$BB9358` via `DATA_BB95BC` (27 effects indexed by `$0519`).

#### Mainbrace Mayhem (Level ID $00, PPU Profile 1, HDMA $0519=$00)

**Profile 1 initial state** (DATA_FD7A86):
- $2105=$09 (Mode 1, BG3 priority)
- $212C=$16 (Main: BG2+BG3+OBJ), $212D=$00 (Sub: none)
- $2130=$00 (no AddSubscreen, no clip), $2131=$26 (ADD on BG2+BG3+Backdrop)
- $2132=$00 (FixedColor = black)

**Runtime state** (observed in ScanlineInfo diag log):
- $212C=$04 (Main: BG3 only), $212D=$13 (Sub: BG1+BG2+OBJ)
- $2130=$02 (AddSubscreen), $2131=$24 (ADD on BG3+Backdrop)

**Transition:** Game code (not just HDMA) reconfigures PPU after profile load,
switching from "BG2+BG3 + FixedColor ADD" to "BG3 overlay + Subscreen ADD".
This creates the fog effect: BG3 = fog tiles (semi-transparent), Sub = terrain.

**HDMA effect 0** (`CODE_BB9E6A`): Basic single-channel HDMA. Likely animates
$2132 or window registers for subtle per-scanline fog density variation.

**Overlay detection in our code:** BG3 sole on main ($04) + AddSubscreen →
overlay path fires. Searches BG1/BG2 on sub-screen for HD content tiles.
Formula: `HD_result = HD_terrain + fog_tint` where
`fog_tint = undoBrightness(ppuOutput) - SubScreenColor`.

#### Rambi Rumble (Level ID $02, PPU Profile 3, HDMA $0519=$10)

**Profile 3 state** (DATA_FD7ADF):
- $2105=$09 (Mode 1, BG3 priority)
- $212C=$01 (Main: BG1 only), $212D=$16 (Sub: BG2+BG3+OBJ)
- $2130=$02 (AddSubscreen), $2131=$21 (ADD on BG1+Backdrop)
- $2132=$00 (FixedColor = black, but HDMA animates this)

**Runtime behavior:** Profile 3 values apply directly (no game-code override).
BG1 = honey/amber overlay tiles, Sub = BG2 terrain + BG3 detail.
PPU output = BG1_honey + SubScreen_terrain (additive blend).

**HDMA effect $10** (`CODE_BBA031`): Multi-channel HDMA (channels 2+5).
Animates $2132 (FixedColor) per scanline — creates vertical honey gradient.
Since AddSubscreen is active, FixedColor doesn't directly affect CM output,
but HDMA may conditionally switch $2130 per-scanline for gradient zones.

**Overlay detection in our code:** BG1 sole on main ($01) + AddSubscreen →
overlay path fires. Searches BG2/BG3 on sub-screen for HD content tiles.
Formula: `HD_result = HD_terrain + honey_tint`.

#### Lockjaw's Locker (Level ID $15, PPU Profile 4, HDMA $0519=$11)

**Profile 4 state** (previously researched):
- $212C=$01 (Main: BG1 only), $212D=$06 (Sub: BG2+BG3)
- $2130=$02 (AddSubscreen), $2131=$23 (ADD on BG1+BG2+Backdrop)

**HDMA effect $11** (`CODE_BB95F2`): Per-scanline MainScreenLayers override.
- Above water line: $212C=$13 (BG1+BG2+OBJ on main) → normal terrain
- Below water line: $212C=$01 (BG1 only on main) → overlay mode

**Overlay detection:** Below water: BG1 sole on main + AddSubscreen →
overlay path. Above water: BG1+BG2 both on main → normal winner path.
HDMA creates the split; our per-scanline ScanlineInfo captures it correctly.

#### Summary: All Three Overlay Levels

| Level | Overlay Layer | Content Layers | CM Mode | HDMA Role |
|-------|--------------|----------------|---------|-----------|
| Mainbrace | BG3 (fog) | BG1+BG2 (terrain) | ADD Sub | Reconfigured by game code |
| Rambi | BG1 (honey) | BG2+BG3 (terrain) | ADD Sub | Animates FixedColor gradient |
| Lockjaw | BG1 (water) | BG2+BG3 (terrain) | ADD Sub | Splits screen via MainScreenLayers |

**Common pattern:** One BG layer acts as overlay (sole layer on main screen),
content layers are on sub-screen, AddSubscreen blends them. Our generic
overlay detection (`isSoleBgOnMain && ColorMathAddSubscreen`) catches all three
without any level-specific code.

---

## 2026-07-14 — Architecture Review & Refactor Plan (P4.0)

### Befund (Review über Filter, PPU-Capture, Loader, Issue-Katalog)

Die Richtung des P3.x-Umbaus (Register statt Heuristiken) ist korrekt, aber der
Filter **rekonstruierte** das PPU-Compositing aus unvollständigen Daten, statt es
mit vollständigen Daten **nachzurechnen**. Überall wo Information fehlte, wurde
zurückgeraten (Tint-Extraktion P3.4, Overlay-Swaps P3.8/P3.10, Palette-Ratio-
Sampling P3.12/P3.13) — jede Raterei mit eigenen Fehlerfällen. Das war die
Ursache der Per-Level-Bug-Serie. Drei Informationslücken erzeugten alle offenen Bugs:

| Lücke | Was fehlte | Bug-Familie |
|-------|-----------|-------------|
| 1. Sub-Screen nur als fertige Farbe | `SubScreenColor` ist ein komponierter Einzelwert; Sub-Winner-Tile + "Sub leer?" gingen verloren | Mainbrace/Rambi/Lockjaw-Overlay-Saga, alle Swap/Tint-Hacks |
| 2. Palette in HD-Tiles eingebacken | CGRAM-Änderungen (Unterwasser-DMA, Sunset-HDMA, Paletten-Zyklus) unsichtbar für den Filter | Lockjaw-Verdunklung+Wellen-Animation, Gangplank, (vermutl.) Pirate Panic |
| 3. Color Math nicht exakt | Leer-Subscreen→FixedColor+Halve-off-Sonderfall (`SnesPpu.cpp:1488-1496`) fehlte; Windows fehlten komplett | Pirate-Panic-Tönung, Gusty Glade |

**Nebenfunde:**
- P3.13-Ratio-Rauschen hatte eine Zusatzursache: der Loader **premultipliziert Alpha**
  (`SnesHdPackLoader.cpp:392`) — Center-Pixel-Sampling las vorgemultiplizierte
  (alpha-verdunkelte) Werte → Ratio-Ausreißer bis zum 512-Clamp.
- `DetectActiveGfxset()` wird **nirgends aufgerufen** — Fingerprints sind geladen
  aber tot. Das `isWorldmap`-Sig-Gate im Filter ist deshalb tragend und bleibt
  vorerst (Tech-Debt: Fingerprints verdrahten, dann Gate entfernen).
- Diagnose-`static`s in `ApplyFilter()` sind nicht threadsicher (Tech-Debt).
- HD-**Sprites** sind komplett unbenutzt: `Sprites[4]`/`SpriteCount` werden von der
  PPU erfasst, der Filter liest sie nie (Sprite-Pixel → immer nativ).

### Refactor-Plan (R-Serie)

| Schritt | Inhalt | Status |
|---------|--------|--------|
| **R1** | Capture-Lücken schließen: `SubScreenWinnerPlus1` + `SubScreenEmpty` pro Pixel, CGRAM-Snapshot pro Frame, `MainScreenColor` ohne Brightness | **DONE (P4.0)** |
| **R2** | Filter-Kern = PPU-Composite @ HD: ein generischer Pfad, exakte `ApplyColorMathToPixel`-Portierung inkl. Leer-Sub-Sonderfall, Clip/Prevent-Windows, Sub-Operand aus HD-Tile. Löscht: Overlay-Erkennung, Swaps, Tint-Extraktion, Palette-Ratio | **DONE (P4.0)** |
| **R3** | Palettendynamische HD-Tiles: Referenz-Paletten ins Pack (`palettes.bin`), Laufzeit-Diff Live-CGRAM vs. Referenz → stabiler Transform pro Palette-Zeile (statt Pixel-Sampling) | offen (nächster Schritt; Viewer + Mesen) |
| **R4** | Color Window @ HD | **Kern in P4.0 mitgeliefert** (Clip/Prevent + W1/W2-Masklogik im CM-Port); Gusty-Glade-Test ausstehend |
| **R5** | Golden-Frame-Regressionsharness (Savestates pro Level + Bilddiff) | offen |
| **R6** | HD-Sprites, Performance (Tile-Cache, Issue D), Cleanup (Worldmap-Gate→Fingerprints, Statics) | offen |

### P4.0 Unified Algorithm — Definitive Reference

```
Pro Pixel (Lookup):
  1. Main-Winner-HD-Tile suchen (+ BG1↔BG2-Retry)      [wie bisher]
  2. Falls gefunden: Bottom-Layer-Tile für Soft-Alpha    [wie bisher]
  3. NEU: Sub-Screen-Operand — unabhängig von 1./2.:
     cmActive && AddSubscreen && !SubScreenEmpty && !SubScreenHasSprite
     → HD-Tile für BgTiles[SubScreenWinnerPlus1-1] suchen (+Retry)

Pro Sub-Pixel (Rendering, wenn Main-HD ODER Sub-HD existiert):
  m = blend(TopHD über BottomHD über natives MainScreenColor[pre-math])
  ColorMath (exakter Port von SnesPpu::ApplyColorMathToPixel):
    - Clip-to-Black nach ClipMode+Window (auch ohne AllowColorMath!)
    - AllowColorMath-Flag (per-Pixel) + PreventMode+Window
    - Operand: AddSubscreen
        ? (SubScreenEmpty ? FixedColor + halve=0            ← PPU-Sonderfall!
                          : blend(SubHD über natives SubScreenColor))
        : FixedColor
    - ADD: min(255, m+o)>>halve   SUB: max(0, m-o)>>halve
  Brightness danach (wie PPU)
Sonst: nativer Fallback (unverändert)
```

**Warum das jedes Level abdeckt:** Overlay-Effekte (Nebel/Honig/Wasser) sind nur
noch der Fall "Main ohne HD-Tile, Sub mit HD-Tile" — kein Erkennungscode mehr.
Pirate Panic bekommt durch den Leer-Sub-Sonderfall erstmals den korrekten
FixedColor-Grünton ($0180). Erwartete Rest-Lücke nach P4.0: Lockjaws
Unterwasser-**Verdunklung** (dunklere CGRAM-Palette) — das ist Familie 2 und
kommt erst mit R3.

**Log-Format-Änderung P4.0:** FRAME-Zeile: `overlay=`/`palTint=` ersetzt durch
`mNat=` (Main nativ + Sub-HD gerendert), `sHd=` (Sub-Operand aus HD-Tile),
`sFix=` (Leer-Sub→FixedColor-Fall). Neues `SUBOP-SAMPLE`-Log (erste 10
Operand-Entscheidungen pro Kontext). PALTINT/PALRATIO/CM-MISSING entfernt.

---

## P3.10 Unified Algorithm — SUPERSEDED (durch P4.0, siehe oben)

**Historische Referenz der P3.x-Architektur. Nicht mehr der aktive Algorithmus.**

### Tile Lookup (Steps 1-3)

```
Step 1: Try winner layer for HD tile (+ BG1↔BG2 retry)
        → hdTile = winner HD, hdTileInfo = winner tile info

Step 2: If found → search for bottom HD tile (below winner in priority)
        → hdTileBot = bottom HD (sub-screen content, background, etc.)
        
        P3.10 BG3 Overlay Swap:
        If ALL of: cmActive + AddSubscreen + hdTileBot +
                   winLayer==2 + Mode1Bg3Priority + BG3 sole on main
        → Swap: bottom becomes primary, isOverlayPixel=true

Step 3: If NOT found + cmActive + AddSubscreen → overlay fallback
        Search other layers for HD content tile.
        → hdTile = content HD, isOverlayPixel = true
        → CM operand = overlay tint extracted from native PPU output
```

### CM Operand Selection

| Condition | CM Operand Source |
|-----------|------------------|
| `isOverlayPixel` (Step 3 or BG3 swap) | Extracted from native: `undoBrightness(ppuOutput) - SubScreenColor` |
| `AddSubscreen` (normal path) | Native `SubScreenColor` (BGR555→RGB888) |
| `!AddSubscreen` (FixedColor) | `FixedColor` from scanline info |

### Rendering (per sub-pixel dx,dy)

```
1. result = native PPU pixel (fallback base)
2. If hdTileBot: render bottom HD pixel (with brightness) over result
3. Top HD pixel: apply CM(top, cm) → apply brightness → render over result
```

### BG3 Overlay Swap — PPU Condition (not a heuristic)

The swap fires when three PPU conditions are ALL true simultaneously:
1. `winLayer == 2` — BG3 won the compositing
2. `Mode1Bg3Priority` — Game set $2105 bit 3 (BG3 promoted above BG1/BG2)
3. `(MainScreenLayers & 0x0F) == 0x04` — BG3 is the SOLE BG on main screen

This pattern identifies: **BG3 is a fullscreen overlay** (fog, etc.).
It is NOT a level-specific check — any level with this PPU state will trigger it.

Levels where this fires: Mainbrace Mayhem (fog zones)
Levels where this does NOT fire:
- Lockjaw (winner=BG1, not BG3)
- Rambi (winner=BG1, not BG3; honey tiles removed → Step 3)
- Normal levels (no CM, or multi-BG on main)
- Mainbrace without fog (Main=$17, multi-BG → BG3 not sole)

### Why Each Level Works

| Level | Winner | BG3 Swap? | Path | Result |
|-------|--------|-----------|------|--------|
| **Mainbrace (fog)** | BG3 | YES | Swap → overlay tint | HD content through fog |
| **Mainbrace (no fog)** | BG1/BG2 | NO | Normal CM | HD terrain, normal colors |
| **Lockjaw (underwater)** | backdrop/BG3 | NO | Step 3 → P3.12 palette tint | HD terrain + blue water tint |
| **Lockjaw (above water)** | BG1/BG2 | NO | Normal (no CM or multi-BG) | HD terrain, no tint |
| **Rambi** | (none) | NO | Step 3 overlay fallback | HD terrain + honey tint |
| **Hot-Head Hop** | varies | NO | Normal CM or no CM | HD terrain ± lava glow |
| **Normal** | BG1/BG2 | NO | No CM | HD terrain, clean |

---

## Implementation Phases

| Phase | What | Status | Risk |
|-------|------|--------|------|
| **1** | `SnesHdScanlineInfo` struct + PPU fills per scanline | **DONE** | Low |
| **2** | Winner-only HD compositing with CM-skip | **DONE (P2.1)** | Medium |
| **3** | General color math on HD pixels (ADD/SUB/HALF) + Multi-Layer | **SUPERSEDED durch P4.0/R2** (P3.x-Serie war Rekonstruktions-Ansatz) | High |
| **4** | Color window + brightness at HD resolution | **Kern DONE in P4.0** (Clip/Prevent/Masklogik im CM-Port) | Medium |
| **5** | Remove old special-case paths, update diagnostics | **Größtenteils DONE in P4.0** (Swaps/Tint/Ratio entfernt); Rest siehe R6 | Low |

### Phase 1: Scanline Register Snapshot (DONE)

**Changes:**
- `SnesHdData.h`: Added `SnesHdScanlineInfo` struct, `#include "SNES/SnesPpuTypes.h"`
- `SnesHdData.h`: Added `ScanlineInfo[ScreenHeight]` to `SnesHdScreenInfo`
- `SnesPpu.cpp`: Snapshot `_state` into `ScanlineInfo[hdScanline]` in `RenderScanline()`
- `SnesPpu.cpp`: Clear `ScanlineInfo` in `SendFrame()` buffer swap

**No behavioral change** — existing filter continues to work unchanged.
The new data is available but not yet consumed.

### Phase 2: Winner-Only HD Compositing with CM-Skip (P2.1 CODE DONE — BUILD+TEST PENDING)

**P2.0 (2026-07-07):** Full rewrite of `ApplyFilter()` from ~947 lines of
heuristic cascade to ~320 lines of multi-layer compositing. Tested — 6 bugs found.

**P2.1 (2026-07-07):** Fixes P2.0 bugs by switching from multi-layer to
winner-only compositing with Color Math skip. ~280 lines of clean code.

**Decision:** Old color math heuristics (fog-blend, overlay-blend, colorMathDelta)
are REMOVED, not kept as temporary bridge. Expected visual regression on color-math
levels until Phase 3. ("nein lass uns das nicht mitschleppen")

**P2.0 test results (6 bugs found):**
1. NPC Shop text invisible — lower-priority HD tiles over higher-priority text
2. Rambi Rumble — BG3 beehives in foreground (BG3 Bg3Priority=true)
3. Hot Head Hop — BG3 lava background in foreground (same mechanism)
4. Mainbrace Mayhem — opaque HD fog (no Color Math → HD tile rendered solid)
5. Pirate Panic — sea color missing (colorMathDelta removed, expected)
6. Lockjaw's Locker — water clipping (minor, low priority)

**P2.1 fix — winner-only + CM-skip:**
- **Winner-only:** Only look up HD tile for `BgWinnerLayer` (the PPU compositing
  winner). Without Color Math, the winner fully occludes lower layers, so only
  the winner's HD tile matters. Fixes bugs 1-3.
- **CM-skip:** If `MainScreenFlags & 0x80` (AllowColorMath), skip HD lookup
  entirely → native PPU pixel used. Native pixel already has correct color math.
  Fixes bug 4, handles bug 5 correctly.
- **Removed:** Multi-layer loop, priority sorting, `frameMultiLayer` counter
- **Added:** `frameCmSkip` counter
- **Kept:** BG1↔BG2 layer retry (structural, not heuristic)

**What changed across P2.0+P2.1 (3 files):**

1. **`SnesHdData.h`:** Added `BgMode` + `Mode1Bg3Priority` fields to
   `SnesHdScanlineInfo` (Phase 1 extension for Phase 2).

2. **`SnesPpu.cpp`:** Two new lines in scanline snapshot block (~line 929-930)
   populating `sl.BgMode` and `sl.Mode1Bg3Priority`.

3. **`SnesHdVideoFilter.cpp`:** Complete rewrite of `ApplyFilter()`:
   - **Winner-only lookup** (~line 229-315): Checks CM-skip, then looks up
     HD tile for `BgWinnerLayer` only. BG1↔BG2 layer retry kept.
   - **Single-tile rendering** (~line 323-380): Native PPU pixel as base,
     single HD tile composited over with alpha blending.
   - **Diagnostics:** `frameCmSkip` counter. Build version `P2.1`.

4. **Removed code:**
   - `bg3FogBlend` / `bg3FogSkip` / `frameBg3FogSkip`
   - `bg1OverlayBlend` / `bg1OverlayWinner`
   - `colorMathDelta` / `colorMathRatio`
   - `bg3BgFallback` / `frameHasBg1ColorMath`
   - Multi-layer arrays (`layerHdTiles[4]`, `layerTileInfos[4]`, `layerHdCount`)
   - Priority sorting (`LayerEntry`, `order[]`, insertion sort)
   - `mode1Bg3Prio` per-scanline read (not needed without priority sorting)
   - `frameMultiLayer` counter
   - All old diagnostic counters

**Build version:** `P2.1`

### Phase 3: HD Color Math (IMPLEMENTED — P3.0)

Re-implement `ApplyColorMathToPixel()` logic but operating on HD-resolution
ARGB8888 pixels instead of native BGR555. Uses `ScanlineInfo` to determine:
- Which layer's pixel gets color math
- ADD/SUB/HALF mode
- Sub-screen pixel or FixedColor as second operand

**Konkrete Schritte:**

1. Neue Hilfsfunktion `ApplyHdColorMath()` in SnesHdVideoFilter erstellen:
   - Input: HD ARGB8888 Pixel, `SnesHdScanlineInfo`, Layer-Index
   - Output: ARGB8888 Pixel nach Color Math

2. Für jeden Pixel prüfen:
   - Ist Color Math für den Winner-Layer aktiviert? (`ColorMathEnabled & (1 << layer)`)
   - Ist `AllowColorMath` Flag gesetzt? (aus `MainScreenFlags`)

3. Zweiten Operanden bestimmen:
   - Wenn `ColorMathAddSubscreen` → Sub-Screen-Pixel verwenden
   - Sonst → `FixedColor` aus `ScanlineInfo` verwenden
   - FixedColor von BGR555 nach ARGB8888 konvertieren

4. ADD/SUB berechnen:
   - ADD: `R = min(255, R_main + R_sub)` (pro Kanal, clamped)
   - SUB: `R = max(0, R_main - R_sub)` (pro Kanal, clamped)
   - HALF: Ergebnis durch 2 teilen (nach ADD/SUB)

5. Clip/Prevent-Modi aus Color Window berücksichtigen (vorbereitet,
   aber volle Window-Auswertung erst in Phase 4).

**Dateien:** `SnesHdVideoFilter.cpp`, `SnesHdVideoFilter.h`

**Risiko:** Hoch — muss pixelgenau mit der PPU übereinstimmen. Fehler
sind sofort sichtbar (falsche Nebeltönung, fehlende Honig-Overlay etc.)

**Testfälle (alle 6 ppuConfigs):**
- $03: Beehive — Honig-Glow (BG1 ADD Sub-Screen)
- $24: Mine — Nebel (BG3 ADD FixedColor, HDMA-animiert)
- $29: Red-Hot Ride — Dampf (windowed ADD)
- $2C: Haunted — Geister-Overlay (windowed ADD)
- $31: Ice — Eis-Nebel (BG3 ADD FixedColor)
- $35: Forest — Wald-Baldachin (windowed ADD)

### Phase 4: HD Color Window + Brightness

Scale window boundaries to HD resolution. Evaluate inside/outside per HD pixel.
Apply clip-to-black and prevent-color-math. Apply brightness scaling.

**Konkrete Schritte:**

1. Neue Hilfsfunktion `IsInsideColorWindow()`:
   - Input: SNES x-Koordinate (0-255), `SnesHdScanlineInfo`
   - Evaluiert Window 1 + Window 2 mit MaskLogic (Or/And/Xor/Xnor)
   - Berücksichtigt Active/Inverted-Flags
   - Gibt true/false zurück

2. HD-Skalierung der Window-Grenzen:
   - SNES Window bei x=100 → HD bei x=400..403 (4x Scale)
   - Wichtig: Window-Grenzen sind inklusive (Left <= x <= Right)
   - Für HD: `hdX / scale` gibt die SNES-Koordinate → Window-Auswertung
     auf SNES-Ebene, nicht HD-Ebene (keine Sub-Pixel-Windows nötig)

3. In `ApplyHdColorMath()` (aus Phase 3):
   - Clip-to-black: `ColorMathClipMode` + Window-State → Pixel = 0
   - Prevent: `ColorMathPreventMode` + Window-State → Color Math überspringen

4. Brightness: `ScreenBrightness` aus `ScanlineInfo`:
   - `R = R * brightness / 15` (pro Kanal)
   - Auf HD-Pixel anwenden NACH Color Math (wie die PPU es macht)

**Dateien:** `SnesHdVideoFilter.cpp`, `SnesHdVideoFilter.h`

**Risiko:** Mittel — Window-Logik ist komplex (2 Windows, 4 Kombinations-
modi), aber die Referenz-Implementierung existiert bereits in der PPU.

**Testfälle:**
- Gusty Glade: Window-Grenzen für Nebel-Bereich
- Red-Hot Ride: Dampf-Window
- Fade-In/Out bei Level-Start: Brightness von 0→15

### Phase 5: Cleanup + Diagnostik

Remove old special-case paths and update diagnostics.

**Konkrete Schritte:**

1. Alte Sonderfall-Variablen entfernen:
   - `bg3FogBlend` / `bg3FogSkip` / `frameBg3FogSkip`
   - `bg1OverlayBlend` / `bg1OverlayWinner`
   - `colorMathDelta` / `colorMathRatio`
   - `bg3BgFallback`
   - `frameBg1OvBg2` / `frameBg1OvBg3` / `frameBg1OvMiss`
   - `layerRetryHd` / `layerRetryNative` / `layerRetryMiss`

2. Alte Diagnostik-Logzeilen anpassen:
   - WINNERS-Zeile: alte Counter entfernen
   - Neue Counter für Compositing Engine: Layer-Hits pro Layer,
     Color-Math-Anwendungen, Window-Clips

3. DEBUG_JOURNAL.md: Issues als CLOSED markieren:
   - Issue L (Mainbrace fog): gelöst durch generische Color Math
   - Issue O (Beehive regression): gelöst durch Multi-Layer-Compositing
   - Issue J, K, M, N, P: prüfen welche durch Engine gelöst sind

4. `cmFg`-Hack im Viewer entfernen (DKC2-HD-Tools, separate Aufgabe):
   - Honig-Tiles brauchen keinen separaten Ordner mehr
   - Layer-Swap-Workaround für ppuConfig $03 vereinfachen

5. Optional: Alte .ps1-Temp-Scripts aufräumen (14 Dateien im Repo-Root)

**Dateien:** `SnesHdVideoFilter.cpp`, `SnesHdVideoFilter.h`, `DEBUG_JOURNAL.md`

**Risiko:** Niedrig — nur Code entfernen und Doku aktualisieren.

---

## What Stays Unchanged

- **Hash generation** — `ComputeTileContentHash()`, `SnesHdTileKey`
- **HD pack loading** — `SnesHdPackLoader`, `SnesHdPackData`, `hashes.bin` format
- **Gfxset detection** — `DetectActiveGfxset()`, `fingerprints.bin`
- **PNG atlas system** — `SnesHdBitmapInfo`, `SnesHdPackTileInfo`
- **Viewer tool** — DKC2-HD-Tools (~95% unchanged)
- **Tile matching** — `GetMatchingTile()` lookup

---

## Files Modified

| File | Phase | Change |
|------|-------|--------|
| `Core/SNES/HdPacks/SnesHdData.h` | 1+2 | `SnesHdScanlineInfo` struct + `BgMode`/`Mode1Bg3Priority` fields |
| `Core/SNES/SnesPpu.cpp` | 1+2 | Scanline snapshot + memset clearing + BgMode/Mode1Bg3Priority |
| `Core/SNES/HdPacks/SnesHdVideoFilter.cpp` | 2-5 | `ApplyFilter()` rewrite (Phase 2 DONE) |
| `Core/SNES/HdPacks/SnesHdVideoFilter.h` | 3-5 | New helper methods (Phase 3+) |

---

## Design Principles

1. **"Erst korrekt, dann schnell"** — Performance optimization (tile-level caching)
   is NOT part of this refactor. Get the pipeline correct first.
2. **No heuristics** — Every compositing decision uses PPU register data, not
   guesses based on observed pixel patterns.
3. **PPU is the source of truth** — We don't interpret game behavior; we replicate
   what the PPU already computed, just at higher resolution.
4. **Backwards compatible** — HD packs, hashes.bin, fingerprints.bin all unchanged.
   The same pack works with old and new engine.

---

## Checkliste: Nächste Session

### Phase 2: Build + Test

**P2.0 (tested, 6 bugs found → fixed in P2.1):**
- [x] In Visual Studio bauen — Kompiliert ohne Fehler
- [x] DKC2 starten, Pirate Panic — Meer-Farbe fehlt (expected, colorMathDelta entfernt)
- [x] Mainbrace Mayhem — HD Fog opak statt semi-transparent (BUG → P2.1 fix)
- [x] Rambi Rumble — BG3 Bienenstöcke im Vordergrund (BUG → P2.1 fix)
- [x] Hot Head Hop — BG3 Lava-Hintergrund im Vordergrund (BUG → P2.1 fix)
- [x] NPC Shop Text unsichtbar (BUG → P2.1 fix)

**P2.1 (BUILD+TEST PENDING):**
- [ ] In Visual Studio bauen (Windows, x64)
- [ ] DKC2, **Pirate Panic** — HD tiles + native Meer-Farbe (CM-skip auf Meer-Pixels)
- [ ] DKC2, **NPC Shop** (Cranky/Funky) — Text sichtbar
- [ ] DKC2, **Mainbrace Mayhem** — Nativer Fog (korrekt semi-transparent)
- [ ] DKC2, **Rambi Rumble** — BG3 Bienenstöcke im HINTERGRUND
- [ ] DKC2, **Hot Head Hop** — HD wo kein CM, nativ wo CM (mixed)
- [ ] Diagnostik prüfen: `cmSkip=` Counter im Diag-Log sichtbar
- [ ] Git commit nach erfolgreichem Test

### Phase 2 → Phase 3 Übergang

- [ ] Phase 2 commit erstellen
- [ ] Phase 3 starten: `ApplyHdColorMath()` Hilfsfunktion implementieren
- [ ] Color Math auf HD ARGB8888 Pixel anwenden (ADD/SUB/HALF)
- [ ] FixedColor (BGR555→ARGB8888) als zweiten Operanden verwenden
- [ ] Sub-Screen als alternativen zweiten Operanden unterstützen
- [ ] Testen: alle 6 ppuConfig-Typen ($03, $24, $29, $2C, $31, $35)

### Offene Punkte (nicht vergessen)

- [ ] DEBUG_JOURNAL Issues J, K, M, N, P: Status prüfen nach Phase 3/4
- [ ] 14 alte .ps1 Temp-Scripts im Repo-Root aufräumen (Phase 5)
- [ ] Viewer: `cmFg`-Hack entfernen nach Phase 5
- [ ] Performance-Optimierung (Tile-Level Caching) — nach Phase 5

### Wichtige Referenzen

- **PPU Color Math Implementierung:** `SnesPpu.cpp` Zeile 1399-1476
  (`ApplyColorMathToPixel()`) — Referenz für Phase 3 HD-Reimplementierung
- **PPU Window Evaluation:** `SnesPpuTypes.h` Zeile 108-124
  (`WindowConfig::PixelNeedsMasking()`) — Referenz für Phase 4
- **Phase 2 HD Filter:** `SnesHdVideoFilter.cpp` ~280 Zeilen —
  `ApplyFilter()` winner-only + CM-skip (Phase 2.1). Build version `P2.1`.
- **Tag für Rollback:** `v0.1-sonderfall-m5.19` — falls die neue Engine
  Probleme macht, kann jederzeit auf den alten Ansatz zurückgewechselt werden
