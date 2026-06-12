# DKC2-HD-Tools Viewer Patches — M5.3

Alle Änderungen für `dkc2-viewer/index.html` im Repo `Beacher83/DKC2-HD-Tools`.

**Ziel:** BG1-Hash-Bug fixen, BG3-Layer-Support hinzufügen, fingerprints.bin generieren.

---

## Übersicht der Änderungen

| # | Bereich | Problem | Fix |
|---|---------|---------|-----|
| 1 | BG1 Hash (Export) | `chrRawData` produziert falsche Hashes für gfxset ≠ 07 | → `vramSnapshot` verwenden (wie BG2) |
| 2 | BG1 PNG Export | `vramWordAddr` fehlt `& 0x7FFF` Maske | → Maske hinzufügen (wie BG2) |
| 3 | Container Save | Kein `bg3TilemapData`, kein `bg3TilesW/H` in ppuConfig | → BG3-Daten extrahieren |
| 4 | Container Refresh | `refreshContainerSetMetadata` ignoriert BG3 | → BG3-Extraktion hinzufügen |
| 5 | Container ZIP Export | Kein `bg3_tilemap.bin` | → Export hinzufügen |
| 6 | Container ZIP Import | Kein `bg3_tilemap.bin` Import | → Import hinzufügen |
| 7 | Mesen2 Export: BG3 PNGs | Keine BG3-Tile-Dekomposition | → BG3-Tiles wie BG2 exportieren |
| 8 | Mesen2 Export: BG3 Hashes | Keine BG3-Hashes in hashes.bin | → BG3-Hashes berechnen |
| 9 | Mesen2 Export: fingerprints.bin | Nicht existent | → Generierung hinzufügen |
| 10 | Mesen2 Export: manifest.json | Kein BG3 tile count | → Hinzufügen |

---

## Änderung 1: BG1 Hash-Datenquelle Fix

**Datei:** `dkc2-viewer/index.html`  
**Bereich:** `exportAsTexturePack()` → Hash-Berechnung (ca. Zeile 7192-7212)  
**Bug:** BG1-Hashes werden aus `chrRawData` (ROM-Dekomprimierungspuffer) berechnet. Mesen berechnet aus `_vram[]` (VRAM-Bytes). Für gfxset_07 stimmt es zufällig, für andere nicht.

### VORHER:
```javascript
      // BG1 hashes from stored chrRawData
      if (set.chrRawData && set.chrRawData.length > 0 && set.tileArrangementData) {
        const chrData = new Uint8Array(set.chrRawData);
        const tileArr = new Uint8Array(set.tileArrangementData);
        const chrBase = ppuCfg.bg1ChrBase || 0;

        for (let i = 0; i * 2 + 1 < tileArr.length; i++) {
          const tileInfo = tileArr[i * 2] | (tileArr[i * 2 + 1] << 8);
          const gfxIndex = tileInfo & 0x03FF;
          const vramWordAddr = (chrBase + gfxIndex * wordsPerTile) & 0x7FFF;
          const dk = `${gfxset}_0_${vramWordAddr}`;
          if (dedupKey.has(dk)) continue;
          dedupKey.add(dk);

          const byteOff = gfxIndex * bytesPerTile;
          if (byteOff + bytesPerTile <= chrData.length) {
            const hash = fnv1a_64(chrData, byteOff, bytesPerTile);
            hashEntries.push({ vramAddr: vramWordAddr, layer: 0, gfxset, hash });
          }
        }
      }
```

### NACHHER:
```javascript
      // BG1 hashes from vramSnapshot (same approach as BG2)
      // IMPORTANT: Must use vramSnapshot, NOT chrRawData! chrRawData is the ROM
      // decompression buffer which may have different byte layout than VRAM.
      // Mesen computes hashes from live VRAM bytes — we must match that.
      if (set.vramSnapshot && set.tileArrangementData) {
        const vram = set.vramSnapshot instanceof Uint8Array
          ? set.vramSnapshot : new Uint8Array(set.vramSnapshot);
        const tileArr = new Uint8Array(set.tileArrangementData);
        const chrBase = ppuCfg.bg1ChrBase || 0;

        for (let i = 0; i * 2 + 1 < tileArr.length; i++) {
          const tileInfo = tileArr[i * 2] | (tileArr[i * 2 + 1] << 8);
          const gfxIndex = tileInfo & 0x03FF;
          const vramWordAddr = (chrBase + gfxIndex * wordsPerTile) & 0x7FFF;
          const dk = `${gfxset}_0_${vramWordAddr}`;
          if (dedupKey.has(dk)) continue;
          dedupKey.add(dk);

          const byteOff = vramWordAddr * 2;
          if (byteOff + bytesPerTile <= vram.length) {
            const hash = fnv1a_64(vram, byteOff, bytesPerTile);
            hashEntries.push({ vramAddr: vramWordAddr, layer: 0, gfxset, hash });
          }
        }
      }
```

**Was ändert sich:**
1. Bedingung: `set.chrRawData && set.chrRawData.length > 0` → `set.vramSnapshot`
2. Datenquelle: `chrData` (chrRawData) → `vram` (vramSnapshot)
3. Byte-Offset: `gfxIndex * bytesPerTile` → `vramWordAddr * 2`

---

## Änderung 2: BG1 PNG Export — vramWordAddr Maske

**Bereich:** `exportAsTexturePack()` → BG1 sub-tile export (ca. Zeile 7024)  
**Bug:** BG1 wendet `& 0x7FFF` nicht an, BG2 schon. Inkonsistenz.

### VORHER (Zeile ~7024):
```javascript
        const vramWordAddr = chrBase + gfxIndex * wordsPerTile;
```

### NACHHER:
```javascript
        const vramWordAddr = (chrBase + gfxIndex * wordsPerTile) & 0x7FFF;
```

---

## Änderung 3: Container Save — BG3-Daten hinzufügen

**Bereich:** `saveCurrentHDToContainer()` (ca. Zeile 6625-6768)

### 3a: ppuConfig um BG3-Dimensionen erweitern

**VORHER** (ca. Zeile 6668):
```javascript
    ppuConfig = {
      bgMode: ppu.bgMode,
      bg1ChrBase: ppu.bg1 ? ppu.bg1.chrBase : 0,
      bg2ChrBase: ppu.bg2 ? ppu.bg2.chrBase : 0,
      bg3ChrBase: ppu.bg3 ? ppu.bg3.chrBase : 0,
      bg2TilesW,
      bg2TilesH,
    };
```

**NACHHER:**
```javascript
    const bg3TilesW = ppu.bg3 && ppu.bg3.wide ? 64 : 32;
    const bg3TilesH = ppu.bg3 && ppu.bg3.tall ? 64 : 32;
    ppuConfig = {
      bgMode: ppu.bgMode,
      bg1ChrBase: ppu.bg1 ? ppu.bg1.chrBase : 0,
      bg2ChrBase: ppu.bg2 ? ppu.bg2.chrBase : 0,
      bg3ChrBase: ppu.bg3 ? ppu.bg3.chrBase : 0,
      bg2TilesW,
      bg2TilesH,
      bg3TilesW,
      bg3TilesH,
    };
```

### 3b: BG3-Tilemap extrahieren (nach bg2TilemapData-Block einfügen)

**NEU einfügen NACH dem `bg2TilemapData`-Block** (nach ca. Zeile 6700):
```javascript
  // Extract raw BG3 tilemap from VRAM (same approach as BG2)
  let bg3TilemapData = null;
  if (currentBgData && currentBgData.vram && currentBgData.ppu &&
      currentBgData.ppu.bg3 && currentBgData.ppu.bg3.enabled && ppuConfig) {
    const ppu = currentBgData.ppu;
    const vram = currentBgData.vram;
    const tmByteOffset = ppu.bg3.tilemapBase * 2;
    const tmByteLength = ppuConfig.bg3TilesW * ppuConfig.bg3TilesH * 2;
    if (tmByteOffset >= 0 && tmByteOffset + tmByteLength <= vram.length) {
      bg3TilemapData = [];
      for (let i = 0; i < tmByteLength; i++) bg3TilemapData.push(vram[tmByteOffset + i] || 0);
    }
  } else if (catalogData && catalogData.bg3TilemapData) {
    bg3TilemapData = catalogData.bg3TilemapData;
  }
```

### 3c: BG3-Checksums hinzufügen (im tileChecksums-Block)

**NEU einfügen NACH dem BG2-Checksums-Block** (nach ca. Zeile 6740):
```javascript
    // BG3: iterate saved tilemap (2bpp — wordsPerTile may differ!)
    if (bg3TilemapData && bg3TilemapData.length > 1 && ppuConfig.bg3ChrBase !== undefined) {
      const chrBase = ppuConfig.bg3ChrBase;
      const bg3Is4bpp = (ppuConfig.bgMode !== 1);  // Mode 1: BG3 is 2bpp; other modes may differ
      const bg3WordsPerTile = bg3Is4bpp ? 16 : 8;
      const seen = new Set();
      for (let i = 0; i + 1 < bg3TilemapData.length; i += 2) {
        const entry = bg3TilemapData[i] | (bg3TilemapData[i + 1] << 8);
        const tileNum = entry & 0x3FF;
        const vramWordAddr = (chrBase + tileNum * bg3WordsPerTile) & 0x7FFF;
        if (seen.has(vramWordAddr)) continue;
        seen.add(vramWordAddr);
        tileChecksums.push({ layer: 2, vramAddr: vramWordAddr, gfxset, checksum: computeVramChecksum(vram, vramWordAddr, bg3WordsPerTile) });
      }
    }
```

### 3d: bg3TilemapData im hdSaveSet-Aufruf mitgeben

**VORHER** (ca. Zeile 6754):
```javascript
  await hdSaveSet(containerName, 'level', setId, {
    scaleFactor: hdPack.scaleFactor,
    tileCount: tiles.length,
    tiles, clusters, bg2Blob, bg3Blob,
    tileArrangementData: tileArrangementData ? Array.from(tileArrangementData) : null,
    tileArrangementTileCount: currentTileRawData ? currentTileRawData.tileCount : 0,
    gfxCount,
    ppuConfig,
    bg2TilemapData,
    tileChecksums,
    chrRawData,
    vramSnapshot,
    gfxSetIndex: currentStyle?.graphics || null,
    savedAt: Date.now()
  });
```

**NACHHER:**
```javascript
  await hdSaveSet(containerName, 'level', setId, {
    scaleFactor: hdPack.scaleFactor,
    tileCount: tiles.length,
    tiles, clusters, bg2Blob, bg3Blob,
    tileArrangementData: tileArrangementData ? Array.from(tileArrangementData) : null,
    tileArrangementTileCount: currentTileRawData ? currentTileRawData.tileCount : 0,
    gfxCount,
    ppuConfig,
    bg2TilemapData,
    bg3TilemapData,
    tileChecksums,
    chrRawData,
    vramSnapshot,
    gfxSetIndex: currentStyle?.graphics || null,
    savedAt: Date.now()
  });
```

---

## Änderung 4: Container Refresh — BG3-Daten

**Bereich:** `refreshContainerSetMetadata()` (ca. Zeile 7503-7635)

### 4a: ppuConfig um BG3-Dimensionen erweitern

**VORHER** (ca. Zeile 7546):
```javascript
    ppuConfig = {
      bgMode: ppu.bgMode,
      bg1ChrBase: ppu.bg1 ? ppu.bg1.chrBase : 0,
      bg2ChrBase: ppu.bg2 ? ppu.bg2.chrBase : 0,
      bg3ChrBase: ppu.bg3 ? ppu.bg3.chrBase : 0,
      bg2TilesW: ppu.bg2 && ppu.bg2.wide ? 64 : 32,
      bg2TilesH: ppu.bg2 && ppu.bg2.tall ? 64 : 32,
    };
```

**NACHHER:**
```javascript
    ppuConfig = {
      bgMode: ppu.bgMode,
      bg1ChrBase: ppu.bg1 ? ppu.bg1.chrBase : 0,
      bg2ChrBase: ppu.bg2 ? ppu.bg2.chrBase : 0,
      bg3ChrBase: ppu.bg3 ? ppu.bg3.chrBase : 0,
      bg2TilesW: ppu.bg2 && ppu.bg2.wide ? 64 : 32,
      bg2TilesH: ppu.bg2 && ppu.bg2.tall ? 64 : 32,
      bg3TilesW: ppu.bg3 && ppu.bg3.wide ? 64 : 32,
      bg3TilesH: ppu.bg3 && ppu.bg3.tall ? 64 : 32,
    };
```

### 4b: BG3-Tilemap-Extraktion hinzufügen

**NACH dem bg2TilemapData-Block einfügen** (nach ca. Zeile 7575):
```javascript
  let bg3TilemapData = existing.bg3TilemapData;
  if (currentBgData && currentBgData.vram && currentBgData.ppu &&
      currentBgData.ppu.bg3 && currentBgData.ppu.bg3.enabled && ppuConfig) {
    const ppu = currentBgData.ppu;
    const vram = currentBgData.vram;
    const tmByteOffset = ppu.bg3.tilemapBase * 2;
    const tmByteLength = (ppuConfig.bg3TilesW || 32) * (ppuConfig.bg3TilesH || 32) * 2;
    if (tmByteOffset >= 0 && tmByteOffset + tmByteLength <= vram.length) {
      bg3TilemapData = [];
      for (let i = 0; i < tmByteLength; i++) bg3TilemapData.push(vram[tmByteOffset + i] || 0);
    }
  } else if (!bg3TilemapData && catalogData && catalogData.bg3TilemapData) {
    bg3TilemapData = catalogData.bg3TilemapData;
  }
```

### 4c: BG3-Checksums hinzufügen

**Im tileChecksums-Block, NACH dem BG2-Checksums-Block:**
```javascript
    // BG3 checksums
    if (bg3TilemapData && bg3TilemapData.length > 1 && ppuConfig.bg3ChrBase !== undefined) {
      const chrBase = ppuConfig.bg3ChrBase;
      const bg3Is4bpp = (ppuConfig.bgMode !== 1);
      const bg3WordsPerTile = bg3Is4bpp ? 16 : 8;
      const seen = new Set();
      for (let i = 0; i + 1 < bg3TilemapData.length; i += 2) {
        const entry = bg3TilemapData[i] | (bg3TilemapData[i + 1] << 8);
        const tileNum = entry & 0x3FF;
        const vramWordAddr = (chrBase + tileNum * bg3WordsPerTile) & 0x7FFF;
        if (seen.has(vramWordAddr)) continue;
        seen.add(vramWordAddr);
        tileChecksums.push({ layer: 2, vramAddr: vramWordAddr, gfxset, checksum: computeVramChecksum(vram, vramWordAddr, bg3WordsPerTile) });
      }
    }
```

### 4d: bg3TilemapData im hdSaveSet-Aufruf

**VORHER** (ca. Zeile 7610):
```javascript
    bg2TilemapData,
    tileChecksums,
```

**NACHHER:**
```javascript
    bg2TilemapData,
    bg3TilemapData,
    tileChecksums,
```

---

## Änderung 5: Container ZIP Export — bg3_tilemap.bin

**Bereich:** `exportContainerAsZip()` (ca. Zeile 6875-6953)

**NEU einfügen NACH dem bg2_tilemap.bin-Block** (nach ca. Zeile 6918):
```javascript
      if (set.bg3TilemapData && set.bg3TilemapData.length > 0) {
        folder.file('bg3_tilemap.bin', new Uint8Array(set.bg3TilemapData));
        summary.totalFiles++;
      }
```

---

## Änderung 6: Container ZIP Import — bg3_tilemap.bin

**Bereich:** `importContainerFromZip()` (ca. Zeile 7319-7431)

### 6a: bg3TilemapData laden

**NEU einfügen NACH dem bg2TilemapFile-Block** (nach ca. Zeile 7393):
```javascript
      let bg3TilemapData = null;
      const bg3TilemapFile = zip.file(`${setPath}bg3_tilemap.bin`);
      if (bg3TilemapFile) {
        const buf = await bg3TilemapFile.async('arraybuffer');
        bg3TilemapData = Array.from(new Uint8Array(buf));
      }
```

### 6b: bg3TilemapData im hdSaveSet-Aufruf mitgeben

**VORHER** (ca. Zeile 7401):
```javascript
        ppuConfig, tileArrangementData, bg2TilemapData,
```

**NACHHER:**
```javascript
        ppuConfig, tileArrangementData, bg2TilemapData, bg3TilemapData,
```

---

## Änderung 7: Mesen2 Export — BG3 Tile PNG Export

**Bereich:** `exportAsTexturePack()` — NACH dem BG2-Export-Block einfügen (nach ca. Zeile 7171)

### 7a: setsWithBg3 sammeln und BG3-Tiles exportieren

**NEU einfügen NACH `}` das den BG2-Export-Block schließt (Zeile ~7171):**

```javascript
  // ---- BG3 tile export ----
  // BG3 in Mode 1 is 2bpp. Extracts unique 8x8 tiles from the composed bg3.png
  // using the saved tilemap, scales them 4x to 32x32, stores un-flipped.
  let bg3ExportedTiles = 0;

  const setsWithBg3 = levelSets.filter(s => {
    if (!s.bg3Blob) return false;
    const hasPpu  = !!(s.ppuConfig || (livePpu && s.gfxSetIndex === catalogData?.gfxSetIndex));
    const hasTmap = !!(s.bg3TilemapData && s.bg3TilemapData.length > 0 ||
                      (catalogData && catalogData.bg3TilemapData && catalogData.bg3TilemapData.length > 0
                       && s.gfxSetIndex === catalogData.gfxSetIndex));
    return hasPpu && hasTmap;
  });

  for (const set of setsWithBg3) {
    const ppuCfg   = set.ppuConfig || livePpu;
    const tilemapRaw = (set.bg3TilemapData && set.bg3TilemapData.length > 0)
      ? set.bg3TilemapData
      : (catalogData && catalogData.bg3TilemapData ? catalogData.bg3TilemapData : null);
    if (!tilemapRaw || !ppuCfg) continue;

    // Per-gfxset subfolder: bg/bg3/gfxset_07/
    const bg3GfxIdx = set.gfxSetIndex != null ? set.gfxSetIndex : 0;
    const bg3GfxIdStr = String(bg3GfxIdx).padStart(2, '0');
    const bg3Folder = root.folder(`bg/bg3/gfxset_${bg3GfxIdStr}`);
    const bg3ExportedKeys = new Set();

    const chrBase = ppuCfg.bg3ChrBase || 0;
    // BG3 in Mode 1 is 2bpp; in other modes it may be 4bpp
    const bg3Is4bpp = (ppuCfg.bgMode !== 1);
    const bg3WordsPerTile = bg3Is4bpp ? 16 : 8;
    const tilesW = ppuCfg.bg3TilesW || 32;
    const tilesH = ppuCfg.bg3TilesH || 32;

    // Load composed BG3 image
    const bg3Bitmap = await createImageBitmap(set.bg3Blob);
    const srcTileSize = Math.round(bg3Bitmap.width / tilesW);

    // Read tilemap as 16-bit LE words
    const tilemapWords = [];
    const td = tilemapRaw;
    for (let i = 0; i + 1 < td.length; i += 2) tilemapWords.push(td[i] | (td[i+1] << 8));

    for (let tileY = 0; tileY < tilesH; tileY++) {
      for (let tileX = 0; tileX < tilesW; tileX++) {
        const entry = tilemapWords[tileY * tilesW + tileX];
        if (entry === undefined) continue;
        const tileNum = entry & 0x3FF;
        const palRow  = (entry >> 10) & 7;
        const flipH   = !!(entry & 0x4000);
        const flipV   = !!(entry & 0x8000);

        const vramWordAddr = (chrBase + tileNum * bg3WordsPerTile) & 0x7FFF;
        const addrHex = vramWordAddr.toString(16).padStart(4, '0');
        const palStr  = 'P' + String(palRow).padStart(2, '0');
        const fileKey = `${addrHex}_${palStr}`;

        if (bg3ExportedKeys.has(fileKey)) continue;
        bg3ExportedKeys.add(fileKey);

        // Extract tile, scale to 32x32, un-flip
        const srcX = tileX * srcTileSize, srcY = tileY * srcTileSize;
        const outCanvas = document.createElement('canvas');
        outCanvas.width = 32; outCanvas.height = 32;
        const ctx = outCanvas.getContext('2d');
        ctx.imageSmoothingEnabled = false;

        if (flipH || flipV) {
          ctx.save();
          if (flipH) { ctx.scale(-1, 1); ctx.translate(-32, 0); }
          if (flipV) { ctx.scale(1, -1); ctx.translate(0, -32); }
        }
        ctx.drawImage(bg3Bitmap, srcX, srcY, srcTileSize, srcTileSize, 0, 0, 32, 32);
        if (flipH || flipV) ctx.restore();

        const dataUrl = outCanvas.toDataURL('image/png');
        const b64 = dataUrl.slice(dataUrl.indexOf(',') + 1);
        const raw = atob(b64);
        const arr = new Uint8Array(raw.length);
        for (let i = 0; i < raw.length; i++) arr[i] = raw.charCodeAt(i);
        bg3Folder.file(`${fileKey}.png`, new Blob([arr], { type: 'image/png' }));
        bg3ExportedTiles++;
      }
    }
  }
```

---

## Änderung 8: Mesen2 Export — BG3 Hashes

**Bereich:** `exportAsTexturePack()` → Hash-Block (ca. Zeile 7173-7235)

### 8a: setsWithBg3 in die Hash-Iteration einbeziehen

**VORHER** (ca. Zeile 7185):
```javascript
    for (const set of [...setsWithArrangement, ...setsWithBg2]) {
```

**NACHHER:**
```javascript
    for (const set of [...setsWithArrangement, ...setsWithBg2, ...setsWithBg3]) {
```

### 8b: BG3 Hash-Berechnung hinzufügen (nach dem BG2-Hash-Block)

**NEU einfügen NACH dem BG2-Hash-Block** (nach dem `}` das den BG2-`if`-Block schließt, ca. Zeile 7235):

```javascript
      // BG3 hashes from vramSnapshot (2bpp in Mode 1)
      if (set.vramSnapshot && set.bg3TilemapData && set.bg3TilemapData.length > 1) {
        const vram = set.vramSnapshot instanceof Uint8Array
          ? set.vramSnapshot : new Uint8Array(set.vramSnapshot);
        const chrBase = ppuCfg.bg3ChrBase || 0;
        const bg3Is4bpp = (ppuCfg.bgMode !== 1);
        const bg3WordsPerTile = bg3Is4bpp ? 16 : 8;
        const bg3BytesPerTile = bg3WordsPerTile * 2;  // 16 bytes for 2bpp, 32 for 4bpp
        const td = set.bg3TilemapData;

        for (let i = 0; i + 1 < td.length; i += 2) {
          const entry = td[i] | (td[i + 1] << 8);
          const tileNum = entry & 0x3FF;
          const vramWordAddr = (chrBase + tileNum * bg3WordsPerTile) & 0x7FFF;
          const dk = `${gfxset}_2_${vramWordAddr}`;
          if (dedupKey.has(dk)) continue;
          dedupKey.add(dk);

          const byteOff = vramWordAddr * 2;
          if (byteOff + bg3BytesPerTile <= vram.length) {
            const hash = fnv1a_64(vram, byteOff, bg3BytesPerTile);
            hashEntries.push({ vramAddr: vramWordAddr, layer: 2, gfxset, hash });
          }
        }
      }
```

**Hinweis:** `layer: 2` = BG3 (0=BG1, 1=BG2, 2=BG3, 3=BG4, 4=Sprites).

---

## Änderung 9: fingerprints.bin Generierung

**Bereich:** `exportAsTexturePack()` — NACH dem hashes.bin/checksums.bin-Block einfügen, VOR dem manifest.json-Block

**Wichtig:** `fpEntries` muss AUSSERHALB des `{ }` Blocks deklariert werden (neben `usesContentHash`), damit es im `manifest.json`-Block sichtbar ist.

**Variable-Deklaration einfügen** (bei ca. Zeile 7179, neben `let usesContentHash`):
```javascript
  let usesContentHash = false;
  let fpEntries = null;   // ← NEU: muss außerhalb des { } Blocks stehen
```

**NEU einfügen** INNERHALB des `{ }` Blocks, nach dem hashes.bin/checksums.bin Code (nach ca. Zeile 7277):

```javascript
  // ---- fingerprints.bin generation ----
  // Selects distinctive "reference tiles" per gfxset whose content hashes uniquely
  // identify that gfxset at runtime. Mesen checks these against live VRAM once per
  // frame to detect which gfxset is active and scope tile lookups accordingly.
  // This prevents cross-gfxset false positives (e.g. worldmap tiles matching level tiles).
  //
  // Format: [uint8 gfxsetCount] × [uint8 gfxsetIdx, uint8 refCount, refCount × {uint16 vramAddr, uint64 hash}]
  if (usesContentHash && hashEntries.length > 0) {
    // Group hash entries by gfxset
    const gfxsetMap = new Map();  // gfxsetIdx → [{vramAddr, hash}]
    for (const e of hashEntries) {
      if (!gfxsetMap.has(e.gfxset)) gfxsetMap.set(e.gfxset, []);
      gfxsetMap.get(e.gfxset).push({ vramAddr: e.vramAddr, hash: e.hash, layer: e.layer });
    }

    // Build set of all hashes per gfxset (for uniqueness check)
    const allHashSets = new Map();  // gfxsetIdx → Set<hash as string>
    for (const [gs, entries] of gfxsetMap) {
      allHashSets.set(gs, new Set(entries.map(e => e.hash.toString())));
    }

    // For each gfxset, find tiles whose hash is UNIQUE to that gfxset
    // (not appearing in any other gfxset).
    // IMPORTANT: Only use 4bpp tiles (BG1/BG2 in Mode 1, layer 0 or 1) as reference tiles!
    // DetectActiveGfxset() in Mesen uses the default wordCount=16 (4bpp = 32 bytes).
    // 2bpp tiles (BG3, layer 2) would hash 16 bytes, causing a mismatch.
    fpEntries = new Map();  // gfxsetIdx → [{vramAddr, hash}]  (declared outside block)
    const MAX_REF_TILES = 8;
    const MIN_REF_TILES = 4;

    for (const [gs, entries] of gfxsetMap) {
      const candidates = [];
      for (const e of entries) {
        // Skip BG3/BG4 tiles (2bpp) — fingerprint detection uses 4bpp wordCount
        if (e.layer >= 2) continue;
        const hashStr = e.hash.toString();
        // Check if this hash appears in any OTHER gfxset
        let isUnique = true;
        for (const [otherGs, otherHashes] of allHashSets) {
          if (otherGs === gs) continue;
          if (otherHashes.has(hashStr)) { isUnique = false; break; }
        }
        if (isUnique) {
          // Prefer non-trivial tiles (hash != the "all zeros" hash)
          const zeroHash = fnv1a_64(new Uint8Array(32), 0, 32);  // hash of 32 zero bytes
          const isTrivial = (e.hash === zeroHash);
          candidates.push({ ...e, isTrivial });
        }
      }

      // Sort: non-trivial first, then BG1 (layer 0) first
      candidates.sort((a, b) => {
        if (a.isTrivial !== b.isTrivial) return a.isTrivial ? 1 : -1;
        if (a.layer !== b.layer) return a.layer - b.layer;
        return 0;
      });

      // Take top N candidates
      const selected = candidates.slice(0, MAX_REF_TILES);
      if (selected.length >= MIN_REF_TILES) {
        fpEntries.set(gs, selected.map(c => ({ vramAddr: c.vramAddr, hash: c.hash })));
      } else if (selected.length > 0) {
        // If less than MIN, still include them (better than nothing)
        fpEntries.set(gs, selected.map(c => ({ vramAddr: c.vramAddr, hash: c.hash })));
        console.warn(`[fingerprints] gfxset ${gs}: only ${selected.length} unique ref tiles (${MIN_REF_TILES} preferred)`);
      } else {
        console.warn(`[fingerprints] gfxset ${gs}: NO unique ref tiles found — cannot create fingerprint`);
      }
    }

    if (fpEntries.size > 0) {
      // Calculate buffer size: 1 + sum(2 + refCount * 10)
      let totalSize = 1;  // gfxsetCount byte
      for (const [gs, refs] of fpEntries) {
        totalSize += 2 + refs.length * 10;  // gfxsetIdx(1) + refCount(1) + refs*(vramAddr(2) + hash(8))
      }
      const fpBuf = new ArrayBuffer(totalSize);
      const fpView = new DataView(fpBuf);
      let offset = 0;

      fpView.setUint8(offset, fpEntries.size); offset += 1;

      for (const [gs, refs] of fpEntries) {
        fpView.setUint8(offset, gs); offset += 1;
        fpView.setUint8(offset, refs.length); offset += 1;
        for (const ref of refs) {
          fpView.setUint16(offset, ref.vramAddr, true); offset += 2;
          // Write uint64 LE as two uint32 halves
          fpView.setUint32(offset, Number(ref.hash & 0xFFFFFFFFn), true); offset += 4;
          fpView.setUint32(offset, Number((ref.hash >> 32n) & 0xFFFFFFFFn), true); offset += 4;
        }
      }

      root.file('fingerprints.bin', new Uint8Array(fpBuf));
      console.log(`[fingerprints] Generated fingerprints for ${fpEntries.size} gfxsets`);
    }
  }
```

**Algorithmus:**
1. Gruppiere alle Hash-Einträge nach Gfxset
2. Für jeden Gfxset, finde Tiles deren Hash in KEINEM anderen Gfxset vorkommt
3. Bevorzuge nicht-triviale Tiles (nicht alle Nullen) und BG1-Tiles
4. Wähle die Top 4-8 als Referenz-Tiles
5. Schreibe das binäre Format

---

## Änderung 10: manifest.json — BG3 Tile-Count

**VORHER** (ca. Zeile 7280):
```javascript
  root.file('manifest.json', JSON.stringify({
    format_version: 2,
    game: {
      rom_header_title: 'Donkey Kong Country 2',
      rom_sha256: ''
    },
    scale: scale,
    tile_size_px: 8,
    hd_tile_size_px: subtileSize,
    lookup_mode: usesContentHash ? 'content_hash' : 'tile_address_palette',
    flip_handling: 'runtime',
    total_bg1_tiles: exportedTiles,
    total_bg2_tiles: bg2ExportedTiles,
    notes: `Exported from DKC2 Viewer container "${containerName}"`
  }, null, 2));
```

**NACHHER:**
```javascript
  root.file('manifest.json', JSON.stringify({
    format_version: 3,
    game: {
      rom_header_title: 'Donkey Kong Country 2',
      rom_sha256: ''
    },
    scale: scale,
    tile_size_px: 8,
    hd_tile_size_px: subtileSize,
    lookup_mode: usesContentHash ? 'content_hash' : 'tile_address_palette',
    flip_handling: 'runtime',
    total_bg1_tiles: exportedTiles,
    total_bg2_tiles: bg2ExportedTiles,
    total_bg3_tiles: bg3ExportedTiles,
    has_fingerprints: fpEntries && fpEntries.size > 0,
    notes: `Exported from DKC2 Viewer container "${containerName}"`
  }, null, 2));
```

---

## Änderung 11: Alert-Nachricht am Ende

**VORHER** (ca. Zeile 7305):
```javascript
  const bg2Msg = bg2ExportedTiles > 0 ? `\n${bg2ExportedTiles} unique BG2-Tiles (32×32px, native 4×)` : '\nKeine BG2-Daten (Level neu speichern für BG2-Export)';
  alert(`Mesen2 HD Pack exportiert!\n\n${exportedTiles} unique BG1-Tiles (32×32px HD)${bg2Msg}\naus ${setsWithArrangement.length} Level-Sets.\n\nZIP direkt nach Mesen2-Ordner entpacken:\nHdPacks\\Donkey Kong Country 2\\ wird automatisch erstellt.`);
  return { exportedTiles, bg2ExportedTiles, sets: setsWithArrangement.length };
```

**NACHHER:**
```javascript
  const bg2Msg = bg2ExportedTiles > 0 ? `\n${bg2ExportedTiles} unique BG2-Tiles` : '';
  const bg3Msg = bg3ExportedTiles > 0 ? `\n${bg3ExportedTiles} unique BG3-Tiles` : '';
  const fpMsg = (typeof fpEntries !== 'undefined' && fpEntries && fpEntries.size > 0)
    ? `\n${fpEntries.size} Gfxset-Fingerprints` : '';
  alert(`Mesen2 HD Pack exportiert!\n\n${exportedTiles} unique BG1-Tiles (32×32px HD)${bg2Msg}${bg3Msg}${fpMsg}\naus ${setsWithArrangement.length} Level-Sets.\n\nZIP direkt nach Mesen2-Ordner entpacken:\nHdPacks\\Donkey Kong Country 2\\ wird automatisch erstellt.`);
  return { exportedTiles, bg2ExportedTiles, bg3ExportedTiles, sets: setsWithArrangement.length };
```

---

## Zusammenfassung der Mesen-seitigen Anpassungen (bereits erledigt)

Die Mesen-Seite unterstützt bereits:
- **BG3 (layer=2):** `SnesHdPackLoader.cpp` sucht in `bg/bg3/gfxset_XX/` nach PNGs
- **fingerprints.bin:** `LoadFingerprints()` liest das Format und `DetectActiveGfxset()` prüft pro Frame
- **Gfxset-Scoping:** `GetMatchingTile()` filtert nach `ActiveGfxset`

Keine weiteren Mesen-Änderungen nötig für BG3/Fingerprints.

---

## Reihenfolge der Anwendung

1. Änderungen 3-6 (Container) → ermöglicht BG3-Daten in der DB
2. Container aktualisieren (Refresh-Button) → BG3-Daten werden aus ROM geladen
3. Änderungen 1-2 (Hash-Fix) → korrekte BG1-Hashes
4. Änderung 7 (BG3 PNG-Export) → BG3-Tiles werden exportiert
5. Änderung 8 (BG3 Hashes) → BG3-Hashes in hashes.bin
6. Änderung 9 (fingerprints.bin) → Gfxset-Erkennung
7. Änderungen 10-11 (Manifest/Alert) → kosmetisch

**Kritischer Workflow für bestehende Container:**
```
1. Viewer-Code aktualisieren
2. ROM laden, Gfxset öffnen
3. "Container → Refresh Metadata" klicken → speichert bg3TilemapData + korrekten vramSnapshot
4. "Export as Mesen2 HD Pack" → erzeugt hashes.bin (BG1 fix!), bg3-Tiles, fingerprints.bin
5. ZIP entpacken in Mesen2 HdPacks-Ordner
6. Mesen2 starten → Level 2 + Worldmap testen
```
