#include "pch.h"
#include "SNES/HdPacks/SnesHdVideoFilter.h"
#include "SNES/SnesConsole.h"
#include "Shared/Emulator.h"
#include "Shared/EmuSettings.h"
#include "Shared/Video/BaseVideoFilter.h"
#include "Shared/ColorUtilities.h"
#include "Shared/MessageManager.h"
#include <unordered_set>
#include <algorithm>
#include <cstdlib>

// Build version — logged in diagnostics so test PC can verify correct code is running.
#define SNES_HD_BUILD_VERSION "P3.10"

// ---------------------------------------------------------------------------
// DiagLog — writes to both Mesen's log window AND a persistent text file.
// File is created once per session at %USERPROFILE%\Downloads\snes_hd_diag.txt
// (or $HOME/Downloads/ on non-Windows). Flushed after every write so crash
// won't lose data.
// ---------------------------------------------------------------------------
static void DiagLog(const char* msg)
{
	MessageManager::Log(msg);

	static FILE* diagFile = nullptr;
	static bool diagFileAttempted = false;

	if(!diagFileAttempted) {
		diagFileAttempted = true;
		const char* home = getenv("USERPROFILE");
		if(!home) home = getenv("HOME");
		if(home) {
			char path[512];
#ifdef _WIN32
			snprintf(path, sizeof(path), "%s\\Downloads\\snes_hd_diag.txt", home);
#else
			snprintf(path, sizeof(path), "%s/Downloads/snes_hd_diag.txt", home);
#endif
			diagFile = fopen(path, "w");
			if(diagFile) {
				fprintf(diagFile, "=== SNES HD Pack Diagnostics (build " SNES_HD_BUILD_VERSION ") ===\n");
				fprintf(diagFile, "Log file: %s\n\n", path);
				fflush(diagFile);
			}
		}
	}

	if(diagFile) {
		fprintf(diagFile, "%s\n", msg);
		fflush(diagFile);
	}
}

// ---------------------------------------------------------------------------
// ContextLog — separate file for per-level PPU register snapshots.
// Written to %USERPROFILE%\Downloads\snes_hd_context.txt
// One entry per context change — stays small and readable.
// ---------------------------------------------------------------------------
static void ContextLog(const char* msg)
{
	static FILE* ctxFile = nullptr;
	static bool ctxFileAttempted = false;

	if(!ctxFileAttempted) {
		ctxFileAttempted = true;
		const char* home = getenv("USERPROFILE");
		if(!home) home = getenv("HOME");
		if(home) {
			char path[512];
#ifdef _WIN32
			snprintf(path, sizeof(path), "%s\\Downloads\\snes_hd_context.txt", home);
#else
			snprintf(path, sizeof(path), "%s/Downloads/snes_hd_context.txt", home);
#endif
			ctxFile = fopen(path, "w");
			if(ctxFile) {
				fprintf(ctxFile, "=== SNES HD Context Log (build " SNES_HD_BUILD_VERSION ") ===\n");
				fprintf(ctxFile, "One entry per level/context switch. PPU register snapshot from scanline 120.\n\n");
				fflush(ctxFile);
			}
		}
	}

	if(ctxFile) {
		fprintf(ctxFile, "%s\n", msg);
		fflush(ctxFile);
	}
}

SnesHdVideoFilter::SnesHdVideoFilter(Emulator* emu, SnesConsole* console, SnesHdPackData* hdData) : BaseVideoFilter(emu)
{
	_hdData = hdData;
	_console = console;
	_hdScale = hdData->Scale;
	InitLookupTable();
}

void SnesHdVideoFilter::InitLookupTable()
{
	VideoConfig config = _emu->GetSettings()->GetVideoConfig();
	InitConversionMatrix(config.Hue, config.Saturation);

	for(int rgb555 = 0; rgb555 < 0x8000; rgb555++) {
		uint8_t r = ColorUtilities::Convert5BitTo8Bit(rgb555 & 0x1F);
		uint8_t g = ColorUtilities::Convert5BitTo8Bit((rgb555 >> 5) & 0x1F);
		uint8_t b = ColorUtilities::Convert5BitTo8Bit((rgb555 >> 10) & 0x1F);

		if(config.Hue != 0 || config.Saturation != 0 || config.Brightness != 0 || config.Contrast != 0) {
			ApplyColorOptions(r, g, b, config.Brightness, config.Contrast);
		}
		_calculatedPalette[rgb555] = 0xFF000000 | (r << 16) | (g << 8) | b;
	}
}

void SnesHdVideoFilter::OnBeforeApplyFilter()
{
	VideoConfig config = _emu->GetSettings()->GetVideoConfig();
	if(config.Hue != _lastHue || config.Saturation != _lastSaturation ||
	   config.Brightness != _lastBrightness || config.Contrast != _lastContrast) {
		_lastHue = config.Hue;
		_lastSaturation = config.Saturation;
		_lastBrightness = config.Brightness;
		_lastContrast = config.Contrast;
		InitLookupTable();
	}
}

FrameInfo SnesHdVideoFilter::GetFrameInfo()
{
	OverscanDimensions overscan = GetOverscan();
	uint32_t baseWidth = 256;
	uint32_t baseHeight = 239;

	return {
		(baseWidth - overscan.Left - overscan.Right) * _hdScale,
		(baseHeight - overscan.Top - overscan.Bottom) * _hdScale
	};
}

OverscanDimensions SnesHdVideoFilter::GetOverscan()
{
	return BaseVideoFilter::GetOverscan();
}

// =========================================================================
// Phase 3.3: Generic CM Overlay + Winner-First HD Compositing
// =========================================================================
// P3.4: Fix overlay CM operand (extract tint via undo-brightness + subtract sub).
// Extends P3.2 with generic overlay detection (replaces BG3-only check):
//
//   1. PPU winner is ALWAYS the top layer (avoids P2.0 bugs)
//   2. CM Overlay: if winner is sole BG on main screen + CM + AddSubscreen,
//      skip overlay (fog/honey/water) → search sub-screen layers for HD
//   3. Normal: look up HD tile for winner layer (+BG1↔BG2 retry)
//   4. If winner has NO HD tile → native pixel (winner is opaque, done)
//   5. If winner HAS HD tile → render it as top layer
//   6. If top HD tile has transparency: find bottom layer for compositing
//   7. Apply Color Math if AllowColorMath set for the pixel
//   8. Apply Brightness after Color Math
//
// CM Overlay covers: Mainbrace fog (BG3), Rambi honey (BG1), Lockjaw water (BG1)
//
// Safety guarantees:
//   - spriteWon → native pixel (sprites have absolute compositing priority)
//   - Winner has no HD tile → native pixel (no lower-priority tile leakage)
//   - Layers filtered by MainScreenLayers|SubScreenLayers
// =========================================================================

void SnesHdVideoFilter::ApplyFilter(uint16_t* ppuOutputBuffer)
{
	if(_frameData == nullptr) {
		return;
	}

	SnesHdScreenInfo* hdScreen = (SnesHdScreenInfo*)_frameData;
	uint32_t* outputBuffer = GetOutputBuffer();
	FrameInfo frameInfo = _frameInfo;
	OverscanDimensions overscan = GetOverscan();

	uint32_t hdScale = _hdScale;
	uint32_t baseWidth = 256;

	// The PPU output buffer width depends on hi-res mode
	uint32_t ppuWidth = _baseFrameInfo.Width;
	bool isHiRes = (ppuWidth == 512);

	// =====================================================================
	// DIAGNOSTIC: Context-aware logging with VRAM-based level/worldmap detection
	// =====================================================================
	static uint64_t diagPrevVramSig = 0;
	static int diagFrameCount = 0;
	static int diagBgFrameCount = 0;
	static int diagMissCount = 0;
	static int diagMatchCount = 0;
	static std::unordered_set<uint64_t> diagLoggedHashes;
	static bool hdmaDumped = false;

	// Compute VRAM context signature from two stable reference tiles
	uint64_t sigA = 0, sigB = 0;
	bool isLevel2 = false;
	bool isWorldmap = false;
	if(hdScreen->Vram) {
		sigA = ComputeTileContentHash(hdScreen->Vram, 0x32E0);
		sigB = ComputeTileContentHash(hdScreen->Vram, 0x2080);
		isLevel2 = (sigA == 0x1585855B0633F405ULL);
	}
	uint64_t vramSig = sigA ^ (sigB << 1);
	isWorldmap = (vramSig == 0xDBF342F9932FD251ULL);

	// Detect context change (level transition) → reset diagnostic counters
	if(vramSig != diagPrevVramSig && diagPrevVramSig != 0) {
		const char* ctxLabel = isWorldmap ? "WORLDMAP" : (isLevel2 ? "LEVEL2" : "other");
		char buf[256];
		snprintf(buf, sizeof(buf),
			"[SNES HD diag] CONTEXT CHANGE (build=" SNES_HD_BUILD_VERSION "): sig %016llX -> %016llX (%s)",
			(unsigned long long)diagPrevVramSig, (unsigned long long)vramSig, ctxLabel);
		DiagLog(buf);
		diagFrameCount = 0;
		diagBgFrameCount = 0;
		diagMissCount = 0;
		diagMatchCount = 0;
		diagLoggedHashes.clear();
		hdmaDumped = false;
	}
	// Build a compact PPU config key from the registers that matter for rendering.
	// Context log fires only when this key changes (not on every VRAM sig change,
	// which cycles rapidly in animated levels like Lockjaw).
	static uint64_t diagPrevPpuConfigKey = 0;
	static bool diagPendingContextLog = false;  // deferred until first BG-content frame
	static uint64_t diagPendingVramSig = 0;

	SnesHdScanlineInfo& slCtx = hdScreen->ScanlineInfo[120];
	uint64_t ppuConfigKey =
		((uint64_t)slCtx.MainScreenLayers) |
		((uint64_t)slCtx.SubScreenLayers << 8) |
		((uint64_t)slCtx.ColorMathEnabled << 16) |
		((uint64_t)(slCtx.ColorMathAddSubscreen ? 1 : 0) << 24) |
		((uint64_t)(slCtx.ColorMathSubtractMode ? 1 : 0) << 25) |
		((uint64_t)(slCtx.ColorMathHalveResult ? 1 : 0) << 26) |
		((uint64_t)(slCtx.Mode1Bg3Priority ? 1 : 0) << 27) |
		((uint64_t)slCtx.BgMode << 28) |
		((uint64_t)(slCtx.FixedColor & 0x7FFF) << 32) |
		((uint64_t)(slCtx.ScreenBrightness == 15 ? 15 : 0) << 48);

	// On VRAM sig change, always update sig tracking
	if(vramSig != diagPrevVramSig) {
		diagPendingVramSig = vramSig;
	}

	// On PPU config change OR first frame, mark context log as pending
	// (will fire once we see a frame with actual BG content, avoiding transition zeros)
	if(ppuConfigKey != diagPrevPpuConfigKey || diagPrevVramSig == 0) {
		diagPendingContextLog = true;
		diagPendingVramSig = vramSig;
		diagPrevPpuConfigKey = ppuConfigKey;
	}

	diagPrevVramSig = vramSig;

	// DIAGNOSTIC: On very first frame, unconditionally log what we see
	static bool diagFirstFrameLogged = false;
	if(!diagFirstFrameLogged) {
		diagFirstFrameLogged = true;
		SnesHdScanlineInfo& slFirst = hdScreen->ScanlineInfo[120];
		char buf[512];
		snprintf(buf, sizeof(buf),
			"[SNES HD diag] STARTUP: sig=%016llX Main=$%02X Sub=$%02X CM=$%02X "
			"AddSub=%d Mode=%d Bg3Pri=%d Br=%d",
			(unsigned long long)vramSig, slFirst.MainScreenLayers, slFirst.SubScreenLayers,
			slFirst.ColorMathEnabled, slFirst.ColorMathAddSubscreen ? 1 : 0,
			slFirst.BgMode, slFirst.Mode1Bg3Priority ? 1 : 0, slFirst.ScreenBrightness);
		DiagLog(buf);
	}

	// Per-frame counters
	uint32_t frameTotalPixels = 0;
	uint32_t frameBgPixels = 0;
	uint32_t frameHdMatch = 0;       // pixels where HD tile found for winner layer
	uint32_t frameHdMiss = 0;        // BG pixels where NO HD tile found for winner
	uint32_t frameHdCm = 0;         // HD pixels that had color math applied
	uint32_t frameOverlay = 0;      // HD pixels via overlay path (P3.4)
	uint32_t frameLayerRetry = 0;    // BG1↔BG2 layer-agnostic retry matches
	uint32_t frameSpriteWon = 0;     // pixels where sprite won (HD BG skipped)
	uint32_t frameMaskZero = 0;      // non-sprite pixels with BgLayerMask == 0
	uint32_t frameLayerBits[4] = {}; // per-layer: pixels where layer has content
	uint32_t frameWin[4] = {};       // per-layer: pixels where layer wins compositing
	uint32_t frameHdLayers[4] = {};  // per-layer: HD tile found count
	uint32_t frameMultiLayer = 0;   // pixels where bottom HD layer also found
	uint32_t frameHdmaSplit = 0;    // scanlines where MainScreenLayers differs from previous

	// =====================================================================
	// Main pixel loop
	// =====================================================================
	for(uint32_t y = overscan.Top; y < 239 - overscan.Bottom; y++) {

		// HDMA split detection: track per-scanline register changes
		if(y > overscan.Top) {
			SnesHdScanlineInfo& prev = hdScreen->ScanlineInfo[y - 1];
			SnesHdScanlineInfo& cur = hdScreen->ScanlineInfo[y];
			if(cur.MainScreenLayers != prev.MainScreenLayers ||
			   cur.SubScreenLayers != prev.SubScreenLayers ||
			   cur.ColorMathEnabled != prev.ColorMathEnabled ||
			   cur.FixedColor != prev.FixedColor ||
			   cur.ScreenBrightness != prev.ScreenBrightness)
				frameHdmaSplit++;
		}

		for(uint32_t x = overscan.Left; x < baseWidth - overscan.Right; x++) {
			uint32_t srcIndex = y * SnesHdScreenInfo::ScreenWidth + x;
			SnesHdPpuPixelInfo& pixelInfo = hdScreen->ScreenTiles[srcIndex];

			// Index into ppuOutputBuffer (accounts for hi-res doubling)
			uint32_t ppuIndex = isHiRes ? (y * 2 * ppuWidth + x * 2) : (y * ppuWidth + x);

			frameTotalPixels++;

			// Sprite detection: sprite won main screen or sub-screen
			bool spriteWon = (pixelInfo.MainScreenFlags & 0x40) != 0 || pixelInfo.SubScreenHasSprite;
			if(spriteWon) {
				frameSpriteWon++;
			} else if(pixelInfo.BgLayerMask == 0) {
				frameMaskZero++;
			}

			// =============================================================
			// Phase 3.1: Winner-first with bottom-layer enhancement
			// =============================================================
			// The PPU winner is ALWAYS the top layer (avoids P2.0 bugs).
			// If the winner has no HD tile → native pixel (winner is opaque).
			// If the winner HAS an HD tile with transparency → find bottom
			// layer below in priority order for compositing behind it.

			SnesHdPackTileInfo* hdTile = nullptr;
			SnesHdPpuTileInfo* hdTileInfo = nullptr;
			SnesHdPackTileInfo* hdTileBot = nullptr;
			SnesHdPpuTileInfo* hdTileInfoBot = nullptr;
			bool applyColorMath = false;
			bool isOverlayPixel = false;  // true when CM overlay path found HD content
			bool cmActive = false;  // hoisted so the rendering section (below) can see it too
			uint8_t winLayer = 0xFF;  // hoisted so the rendering section (below) can see it too

			if(pixelInfo.BgLayerMask != 0 && !spriteWon && !isWorldmap) {
				frameBgPixels++;

				winLayer = pixelInfo.BgWinnerLayer;
				if(winLayer < 4) frameWin[winLayer]++;

				// Count per-layer content bits (diagnostic only)
				for(int li = 0; li < 4; li++) {
					if(pixelInfo.BgLayerMask & (1 << li)) frameLayerBits[li]++;
				}

				// Check if Color Math is active for this pixel
				cmActive = (pixelInfo.MainScreenFlags & 0x80) != 0;

				// ---------------------------------------------------------
				// Unified HD tile lookup algorithm (P3.7):
				//
				// 1. Try winner layer for HD tile
				// 2. If found → render with CM (if active), find bottom layer
				// 3. If NOT found + CM active + AddSubscreen → winner is likely
				//    a semi-transparent overlay (fog/honey/water) whose tile
				//    was removed from the HD pack. Search other layers for
				//    HD content and apply overlay tint extracted from native.
				// 4. If NOT found + no CM fallback → show native (miss)
				//
				// No level-specific detection. Works generically for:
				//   Mainbrace fog, Rambi honey, Lockjaw water, normal levels.
				// ---------------------------------------------------------
				SnesHdScanlineInfo& sl = hdScreen->ScanlineInfo[y];

				// --- Step 1: Try winner layer ---
				if(winLayer < 4 && (pixelInfo.BgLayerMask & (1 << winLayer))) {
					hdTile = _hdData->GetMatchingTile(
						pixelInfo.BgTiles[winLayer].Key, hdScreen->Vram);

					// BG1↔BG2 layer retry
					if(!hdTile && (winLayer == 0 || winLayer == 1)) {
						SnesHdTileKey altKey = pixelInfo.BgTiles[winLayer].Key;
						altKey.LayerIndex = (winLayer == 0) ? 1 : 0;
						hdTile = _hdData->GetMatchingTile(altKey, hdScreen->Vram);
						if(hdTile) frameLayerRetry++;
					}
				}

				if(hdTile) {
					// --- Step 2: Winner found → render with CM + bottom layer ---
					hdTileInfo = &pixelInfo.BgTiles[winLayer];
					frameHdMatch++;
					frameHdLayers[winLayer]++;
					if(cmActive) {
						applyColorMath = true;
						frameHdCm++;
					}

					// DIAGNOSTIC: Log first 5 unique matches per context
					if(diagMatchCount < 5) {
						auto& key = pixelInfo.BgTiles[winLayer].Key;
						if(key.ContentHash != 0
							&& diagLoggedHashes.find(key.ContentHash) == diagLoggedHashes.end()) {
							diagLoggedHashes.insert(key.ContentHash);
							char buf[320];
							snprintf(buf, sizeof(buf),
								"[SNES HD diag] MATCH hash=%016llX pal=%d layer=%d win=%d cm=%d",
								(unsigned long long)key.ContentHash, key.PaletteIndex,
								key.LayerIndex, winLayer, cmActive ? 1 : 0);
							DiagLog(buf);
							diagMatchCount++;
						}
					}

	// DIAGNOSTIC: Lockjaw scenario — BG1 sole on main + CM + AddSubscreen
					// Log SubScreenColor to understand why water tint may be missing
					static int diagLockjawCount = 0;
					if(diagFrameCount == 0) diagLockjawCount = 0; // reset on context change
					if(diagLockjawCount < 20
						&& winLayer == 0
						&& (sl.MainScreenLayers & 0x0F) == 0x01
						&& cmActive && sl.ColorMathAddSubscreen) {
						uint16_t ssc = pixelInfo.SubScreenColor;
						char buf[400];
						snprintf(buf, sizeof(buf),
							"[SNES HD diag] LOCKJAW-CM win=BG1 SubScreenColor=0x%04X "
							"(R=%d G=%d B=%d) SubLayers=0x%02X MainLayers=0x%02X "
							"CMEnabled=0x%02X HalfMath=%d x=%d y=%d",
							ssc, ssc & 0x1F, (ssc >> 5) & 0x1F, (ssc >> 10) & 0x1F,
							sl.SubScreenLayers, sl.MainScreenLayers,
							sl.ColorMathEnabled, sl.ColorMathHalveResult ? 1 : 0,
							x, y);
						DiagLog(buf);
						diagLockjawCount++;
					}
					// DIAGNOSTIC: BG1-only on main but CM NOT active per-pixel — why?
					static int diagLockjawNoCmCount = 0;
					if(diagFrameCount == 0) diagLockjawNoCmCount = 0;
					if(diagLockjawNoCmCount < 10
						&& winLayer == 0
						&& (sl.MainScreenLayers & 0x0F) == 0x01
						&& sl.ColorMathAddSubscreen
						&& (sl.ColorMathEnabled & 0x01)  // CM enabled for BG1 in register
						&& !cmActive) {  // but per-pixel flag says NO
						char buf[400];
						snprintf(buf, sizeof(buf),
							"[SNES HD diag] LOCKJAW-NOCM win=BG1 MainFlags=0x%02X "
							"CMEnabled=0x%02X MainLayers=0x%02X SubLayers=0x%02X x=%d y=%d",
							pixelInfo.MainScreenFlags, sl.ColorMathEnabled,
							sl.MainScreenLayers, sl.SubScreenLayers, x, y);
						DiagLog(buf);
						diagLockjawNoCmCount++;
					}

					// Find bottom layer (for transparency compositing)
					uint8_t prioOrder[6];
					int prioCount = 0;

					uint8_t bg1P = pixelInfo.BgTiles[0].Priority;
					uint8_t bg2P = pixelInfo.BgTiles[1].Priority;
					uint8_t bg3P = pixelInfo.BgTiles[2].Priority;

					if(sl.Mode1Bg3Priority && bg3P) prioOrder[prioCount++] = 2;
					if(bg1P) prioOrder[prioCount++] = 0;
					if(bg2P) prioOrder[prioCount++] = 1;
					if(!bg1P) prioOrder[prioCount++] = 0;
					if(!bg2P) prioOrder[prioCount++] = 1;
					if(!sl.Mode1Bg3Priority && bg3P) prioOrder[prioCount++] = 2;
					if(!bg3P) prioOrder[prioCount++] = 2;

					bool pastWinner = false;
					for(int pi = 0; pi < prioCount && !hdTileBot; pi++) {
						uint8_t layer = prioOrder[pi];
						if(layer == winLayer) {
							pastWinner = true;
							continue;
						}
						if(!pastWinner) continue;
						if(!(pixelInfo.BgLayerMask & (1 << layer))) continue;
						if(!((sl.MainScreenLayers | sl.SubScreenLayers) & (1 << layer))) continue;

						SnesHdPackTileInfo* tile2 = _hdData->GetMatchingTile(
							pixelInfo.BgTiles[layer].Key, hdScreen->Vram);
						if(!tile2 && (layer == 0 || layer == 1)) {
							SnesHdTileKey altKey2 = pixelInfo.BgTiles[layer].Key;
							altKey2.LayerIndex = (layer == 0) ? 1 : 0;
							tile2 = _hdData->GetMatchingTile(altKey2, hdScreen->Vram);
						}
						if(tile2) {
							hdTileBot = tile2;
							hdTileInfoBot = &pixelInfo.BgTiles[layer];
							frameMultiLayer++;
						}
					}
				}
				// --- Step 3: Winner NOT found + CM + AddSubscreen → overlay fallback ---
				else if(cmActive && sl.ColorMathAddSubscreen) {
					// Winner tile missing from HD pack — likely a semi-transparent
					// overlay (fog, honey, water). Search other layers for HD content.
					for(int tryLayer = 0; tryLayer < 3 && !hdTile; tryLayer++) {
						if(tryLayer == winLayer) continue;
						if(!(pixelInfo.BgLayerMask & (1 << tryLayer))) continue;

						hdTile = _hdData->GetMatchingTile(
							pixelInfo.BgTiles[tryLayer].Key, hdScreen->Vram);
						if(!hdTile && (tryLayer == 0 || tryLayer == 1)) {
							SnesHdTileKey altKey = pixelInfo.BgTiles[tryLayer].Key;
							altKey.LayerIndex = (tryLayer == 0) ? 1 : 0;
							hdTile = _hdData->GetMatchingTile(altKey, hdScreen->Vram);
							if(hdTile) frameLayerRetry++;
						}
						if(hdTile) {
							hdTileInfo = &pixelInfo.BgTiles[tryLayer];
							frameHdMatch++;
							frameHdLayers[tryLayer]++;
							applyColorMath = true;
							isOverlayPixel = true;
							frameHdCm++;
							frameOverlay++;
						}
					}
					if(!hdTile) {
						frameHdMiss++;

						// DIAGNOSTIC: Log first 60 unique misses per context
						if(diagMissCount < 60) {
							SnesHdPpuTileInfo* missInfo = &pixelInfo.BgTiles[winLayer];
							if(missInfo->Key.ContentHash != 0
								&& diagLoggedHashes.find(missInfo->Key.ContentHash) == diagLoggedHashes.end()) {
								diagLoggedHashes.insert(missInfo->Key.ContentHash);
								bool inDmaRange = (missInfo->VramWordAddr >= 0x2000
									&& missInfo->VramWordAddr <= 0x21D0);
								char buf[512];
								snprintf(buf, sizeof(buf),
									"[SNES HD diag] MISS hash=%016llX pal=%d layer=%d vram=0x%04X%s "
									"mask=0x%02X win=%d",
									(unsigned long long)missInfo->Key.ContentHash,
									missInfo->Key.PaletteIndex, missInfo->Key.LayerIndex,
									missInfo->VramWordAddr, inDmaRange ? " [DMA_RANGE]" : "",
									pixelInfo.BgLayerMask, winLayer);
								DiagLog(buf);
								diagMissCount++;
							}
						}
					}
				} else {
					// Winner is sprite/backdrop (0xFF) or winner layer not in mask
					frameHdMiss++;
				}
			}

			// =============================================================
			// Rendering: Multi-layer compositing with HD Color Math
			// =============================================================
			uint32_t outX = (x - overscan.Left) * hdScale;
			uint32_t outY = (y - overscan.Top) * hdScale;

			if(hdTile && hdTileInfo && !hdTile->HdTileData.empty()) {
				// Get scanline info for color math parameters
				SnesHdScanlineInfo& sl = hdScreen->ScanlineInfo[y];

			// Pre-compute color math second operand (BGR555 → RGB888)
				uint8_t cmR = 0, cmG = 0, cmB = 0;
				if(applyColorMath) {
					if(isOverlayPixel) {
						// Overlay mode: extract the overlay tint from PPU output.
						// PPU output = content + overlay (with brightness applied).
						// SubScreenColor = raw content color (no brightness).
						// overlay_color = undoBrightness(ppuOutput) - SubScreenColor
						uint16_t ppuNative = ppuOutputBuffer[ppuIndex] & 0x7FFF;
						uint8_t br = sl.ScreenBrightness;
						int rawR, rawG, rawB;
						if(br > 0) {
							rawR = (int)(ppuNative & 0x1F) * 15 / br;
							rawG = (int)((ppuNative >> 5) & 0x1F) * 15 / br;
							rawB = (int)((ppuNative >> 10) & 0x1F) * 15 / br;
						} else {
							rawR = rawG = rawB = 0;
						}
						int subR = (int)(pixelInfo.SubScreenColor & 0x1F);
						int subG = (int)((pixelInfo.SubScreenColor >> 5) & 0x1F);
						int subB = (int)((pixelInfo.SubScreenColor >> 10) & 0x1F);
						cmR = ColorUtilities::Convert5BitTo8Bit((uint8_t)std::max(0, std::min(31, rawR - subR)));
						cmG = ColorUtilities::Convert5BitTo8Bit((uint8_t)std::max(0, std::min(31, rawG - subG)));
						cmB = ColorUtilities::Convert5BitTo8Bit((uint8_t)std::max(0, std::min(31, rawB - subB)));
					} else if(sl.ColorMathAddSubscreen) {
						uint16_t cmColor = pixelInfo.SubScreenColor;
						cmR = ColorUtilities::Convert5BitTo8Bit(cmColor & 0x1F);
						cmG = ColorUtilities::Convert5BitTo8Bit((cmColor >> 5) & 0x1F);
						cmB = ColorUtilities::Convert5BitTo8Bit((cmColor >> 10) & 0x1F);
					} else {
						uint16_t cmColor = sl.FixedColor;
						cmR = ColorUtilities::Convert5BitTo8Bit(cmColor & 0x1F);
						cmG = ColorUtilities::Convert5BitTo8Bit((cmColor >> 5) & 0x1F);
						cmB = ColorUtilities::Convert5BitTo8Bit((cmColor >> 10) & 0x1F);
					}

				// P3.10: BG3+Mode1Bg3Priority overlay swap
					// When BG3 wins with Mode1Bg3Priority AND is the sole BG on
					// main screen, BG3 is a fullscreen overlay (fog, etc.).
					// In this case, swap: render sub-screen HD content as primary
					// with overlay tint from native PPU output.
					//
					// PPU condition (not a heuristic):
					//   Mode1Bg3Priority = game promoted BG3 above all other layers
					//   Sole BG on main = BG3 is the only compositing source
					//   Both conditions together = BG3 is a fullscreen overlay
					//
					// Does NOT fire for:
					//   Lockjaw: winner=BG1 (not BG3) → normal CM path
					//   Rambi:   winner=BG1 (not BG3), honey tiles removed → Step 3
					//   Normal:  no CM or multi-BG on main → normal path
					if(cmActive && sl.ColorMathAddSubscreen && hdTileBot
						&& winLayer == 2 && sl.Mode1Bg3Priority
						&& (sl.MainScreenLayers & 0x0F) == (1u << 2)) {
						// Swap: bottom HD (content) becomes primary
						hdTile = hdTileBot;
						hdTileInfo = hdTileInfoBot;
						hdTileBot = nullptr;
						hdTileInfoBot = nullptr;
						isOverlayPixel = true;
						// applyColorMath already true
						frameOverlay++;
					}
				}

				// Brightness from scanline info (0-15, applied after color math)
				uint8_t brightness = sl.ScreenBrightness;

				// Render HD tile (multi-layer: top over bottom over native)
				for(uint32_t dy = 0; dy < hdScale; dy++) {
					for(uint32_t dx = 0; dx < hdScale; dx++) {
						uint32_t outIndex = (outY + dy) * frameInfo.Width + (outX + dx);
						if(outIndex >= frameInfo.Width * frameInfo.Height) continue;

						// Base: native PPU pixel
						uint32_t result = _calculatedPalette[ppuOutputBuffer[ppuIndex] & 0x7FFF];

						// --- Bottom layer (if available) ---
						if(hdTileBot && hdTileInfoBot && !hdTileBot->HdTileData.empty()) {
							uint8_t rawX2 = hdTileInfoBot->OffsetX;
							uint8_t rawY2 = hdTileInfoBot->OffsetY;
							bool hFlip2 = hdTileInfoBot->HorizontalMirror;
							bool vFlip2 = hdTileInfoBot->VerticalMirror;
							uint8_t srcTileX2 = hFlip2 ? (7 - rawX2) : rawX2;
							uint8_t srcTileY2 = vFlip2 ? (7 - rawY2) : rawY2;
							uint32_t hdPX2 = srcTileX2 * hdScale + (hFlip2 ? (hdScale - 1 - dx) : dx);
							uint32_t hdPY2 = srcTileY2 * hdScale + (vFlip2 ? (hdScale - 1 - dy) : dy);

							if(hdPX2 < hdTileBot->Width && hdPY2 < hdTileBot->Height) {
								uint32_t botColor = hdTileBot->HdTileData[hdPY2 * hdTileBot->Width + hdPX2];
								uint8_t botAlpha = (botColor >> 24) & 0xFF;
								if(botAlpha > 0) {
									uint8_t bR = (botColor >> 16) & 0xFF;
									uint8_t bG = (botColor >> 8) & 0xFF;
									uint8_t bB = botColor & 0xFF;
									// Apply brightness to bottom layer
									if(brightness < 15) {
										bR = (uint8_t)(bR * brightness / 15);
										bG = (uint8_t)(bG * brightness / 15);
										bB = (uint8_t)(bB * brightness / 15);
									}
									if(botAlpha == 0xFF) {
										result = 0xFF000000 | (bR << 16) | (bG << 8) | bB;
									} else {
										uint8_t baseR = (result >> 16) & 0xFF;
										uint8_t baseG = (result >> 8) & 0xFF;
										uint8_t baseB = result & 0xFF;
										result = 0xFF000000
											| ((uint8_t)(bR + ((baseR * (255 - botAlpha)) / 255)) << 16)
											| ((uint8_t)(bG + ((baseG * (255 - botAlpha)) / 255)) << 8)
											| (uint8_t)(bB + ((baseB * (255 - botAlpha)) / 255));
									}
								}
							}
						}

						// --- Top layer ---
						uint8_t rawX = hdTileInfo->OffsetX;
						uint8_t rawY = hdTileInfo->OffsetY;
						bool hFlip = hdTileInfo->HorizontalMirror;
						bool vFlip = hdTileInfo->VerticalMirror;
						uint8_t srcTileX = hFlip ? (7 - rawX) : rawX;
						uint8_t srcTileY = vFlip ? (7 - rawY) : rawY;
						uint32_t hdPX = srcTileX * hdScale + (hFlip ? (hdScale - 1 - dx) : dx);
						uint32_t hdPY = srcTileY * hdScale + (vFlip ? (hdScale - 1 - dy) : dy);

						if(hdPX >= hdTile->Width || hdPY >= hdTile->Height) continue;

						uint32_t hdColor = hdTile->HdTileData[hdPY * hdTile->Width + hdPX];
						uint8_t alpha = (hdColor >> 24) & 0xFF;

						if(alpha > 0) {
							uint8_t hdR = (hdColor >> 16) & 0xFF;
							uint8_t hdG = (hdColor >> 8) & 0xFF;
							uint8_t hdB = hdColor & 0xFF;

						// Apply color math to HD pixel
							if(applyColorMath) {
								if(sl.ColorMathSubtractMode) {
									hdR = (hdR > cmR) ? (hdR - cmR) : 0;
									hdG = (hdG > cmG) ? (hdG - cmG) : 0;
									hdB = (hdB > cmB) ? (hdB - cmB) : 0;
								} else {
									hdR = (uint8_t)std::min(255, (int)hdR + (int)cmR);
									hdG = (uint8_t)std::min(255, (int)hdG + (int)cmG);
									hdB = (uint8_t)std::min(255, (int)hdB + (int)cmB);
								}
								if(sl.ColorMathHalveResult) {
									hdR >>= 1;
									hdG >>= 1;
									hdB >>= 1;
								}
							}

							// Apply brightness
							if(brightness < 15) {
								hdR = (uint8_t)(hdR * brightness / 15);
								hdG = (uint8_t)(hdG * brightness / 15);
								hdB = (uint8_t)(hdB * brightness / 15);
							}

							if(alpha == 0xFF) {
								result = 0xFF000000 | (hdR << 16) | (hdG << 8) | hdB;
							} else {
								// Alpha blend top over current result
								uint8_t srcR = (result >> 16) & 0xFF;
								uint8_t srcG = (result >> 8) & 0xFF;
								uint8_t srcB = result & 0xFF;
								uint8_t blR = hdR + ((srcR * (255 - alpha)) / 255);
								uint8_t blG = hdG + ((srcG * (255 - alpha)) / 255);
								uint8_t blB = hdB + ((srcB * (255 - alpha)) / 255);
								result = 0xFF000000 | (blR << 16) | (blG << 8) | blB;
							}
						}
						// alpha == 0: transparent — result (bottom/native) shows through

						outputBuffer[outIndex] = result;
					}
				}
			} else {
				// No HD replacement — use original SNES color, scaled up
				uint32_t color = _calculatedPalette[ppuOutputBuffer[ppuIndex] & 0x7FFF];
				for(uint32_t dy = 0; dy < hdScale; dy++) {
					for(uint32_t dx = 0; dx < hdScale; dx++) {
						uint32_t outIndex = (outY + dy) * frameInfo.Width + (outX + dx);
						if(outIndex < frameInfo.Width * frameInfo.Height) {
							outputBuffer[outIndex] = color;
						}
					}
				}
			}
		} // end for(x)
	} // end for(y)

	// =====================================================================
	// DIAGNOSTIC: Deferred context log — emitted on first frame with BG content
	// after a PPU config change (avoids capturing transition-frame zeros)
	// =====================================================================
	if(diagPendingContextLog && frameBgPixels > 0) {
		diagPendingContextLog = false;

		// Re-read scanline 120 (now guaranteed to have real data)
		SnesHdScanlineInfo& slLog = hdScreen->ScanlineInfo[120];
		const char* ctxLabel = isWorldmap ? "WORLDMAP" : (isLevel2 ? "LEVEL2" : "other");

		const char* renderMode = "NORMAL";
		if(slLog.Mode1Bg3Priority && (slLog.ColorMathEnabled & 0x04)) {
			renderMode = "BG3_OVERLAY";
		} else if(slLog.ColorMathAddSubscreen && (slLog.SubScreenLayers & 0x04)) {
			renderMode = "SUBSCREEN_BLEND";
		} else if(!slLog.ColorMathAddSubscreen && slLog.FixedColor != 0x0000) {
			renderMode = "FIXED_COLOR";
		}

		char ctxBuf[1024];
		snprintf(ctxBuf, sizeof(ctxBuf),
			"--- CONTEXT: sig=%016llX (%s) ---\n"
			"  BgMode=%d  Mode1Bg3Pri=%d\n"
			"  MainScreenLayers=$%02X  SubScreenLayers=$%02X\n"
			"  CMEnabled=$%02X (BG1=%d BG2=%d BG3=%d BG4=%d OBJ=%d BDrop=%d)\n"
			"  AddSubscreen=%d  SubtractMode=%d  HalveResult=%d\n"
			"  FixedColor=$%04X  ScreenBrightness=%d\n"
			"  ClipMode=%d  PreventMode=%d\n"
			"  Window1: L=%d R=%d  Window2: L=%d R=%d\n"
			"  ColorWin: active=[%d,%d] inv=[%d,%d] logic=%d\n"
			"  -> Detected mode: %s\n",
			(unsigned long long)diagPendingVramSig, ctxLabel,
			slLog.BgMode, slLog.Mode1Bg3Priority ? 1 : 0,
			slLog.MainScreenLayers, slLog.SubScreenLayers,
			slLog.ColorMathEnabled,
			(slLog.ColorMathEnabled & 0x01) ? 1 : 0,
			(slLog.ColorMathEnabled & 0x02) ? 1 : 0,
			(slLog.ColorMathEnabled & 0x04) ? 1 : 0,
			(slLog.ColorMathEnabled & 0x08) ? 1 : 0,
			(slLog.ColorMathEnabled & 0x10) ? 1 : 0,
			(slLog.ColorMathEnabled & 0x20) ? 1 : 0,
			slLog.ColorMathAddSubscreen ? 1 : 0,
			slLog.ColorMathSubtractMode ? 1 : 0,
			slLog.ColorMathHalveResult ? 1 : 0,
			slLog.FixedColor, slLog.ScreenBrightness,
			(int)slLog.ColorMathClipMode, (int)slLog.ColorMathPreventMode,
			slLog.Window1Left, slLog.Window1Right,
			slLog.Window2Left, slLog.Window2Right,
			slLog.ColorWindowActive[0] ? 1 : 0, slLog.ColorWindowActive[1] ? 1 : 0,
			slLog.ColorWindowInverted[0] ? 1 : 0, slLog.ColorWindowInverted[1] ? 1 : 0,
			(int)slLog.ColorWindowMaskLogic,
			renderMode);
		ContextLog(ctxBuf);
		DiagLog(ctxBuf);
	}

	// =====================================================================
	// DIAGNOSTIC: Per-frame summary
	// =====================================================================
	bool logThisFrame = frameTotalPixels > 0 &&
		(diagFrameCount < 10 || (frameBgPixels > 0 && diagBgFrameCount < 60));
	if(logThisFrame) {
		const char* ctxLabel = isWorldmap ? "WORLDMAP" : (isLevel2 ? "LEVEL2" : "other");
		char buf[1024];
		snprintf(buf, sizeof(buf),
			"[SNES HD diag] FRAME %d/%d [%s] build=" SNES_HD_BUILD_VERSION
			": total=%u bg=%u match=%u miss=%u hdCm=%u overlay=%u lRetry=%u multi=%u"
			" sprWon=%u mask0=%u hdmaSplit=%u"
			" BG1=%u BG2=%u BG3=%u BG4=%u"
			" hdBG1=%u hdBG2=%u hdBG3=%u hdBG4=%u"
			" wn0=%u wn1=%u wn2=%u wn3=%u"
			" (TileByKey=%zu, sig=%016llX)",
			diagFrameCount, diagBgFrameCount, ctxLabel,
			frameTotalPixels, frameBgPixels, frameHdMatch,
			frameHdMiss, frameHdCm, frameOverlay, frameLayerRetry, frameMultiLayer,
			frameSpriteWon, frameMaskZero, frameHdmaSplit,
			frameLayerBits[0], frameLayerBits[1], frameLayerBits[2], frameLayerBits[3],
			frameHdLayers[0], frameHdLayers[1], frameHdLayers[2], frameHdLayers[3],
			frameWin[0], frameWin[1], frameWin[2], frameWin[3],
			_hdData->TileByKey.size(),
			(unsigned long long)vramSig);
		DiagLog(buf);
		diagFrameCount++;
		if(frameBgPixels > 0) {
			diagBgFrameCount++;
		}
	}

	// =====================================================================
	// DIAGNOSTIC: Per-scanline HDMA dump (once per context, first BG frame)
	// Shows scanline ranges where MainScreenLayers/Sub/CM/FixedColor change.
	// =====================================================================
	if(logThisFrame && frameBgPixels > 0 && frameHdmaSplit > 0 && !hdmaDumped) {
		hdmaDumped = true;
		DiagLog("[SNES HD diag] HDMA SCANLINE DUMP (first HDMA frame):");
		uint8_t prevMain = 0xFF, prevSub = 0xFF, prevCM = 0xFF;
		uint16_t prevFixed = 0xFFFF;
		uint8_t prevBr = 0xFF;
		uint32_t rangeStart = overscan.Top;
		for(uint32_t sy = overscan.Top; sy <= 239 - overscan.Bottom; sy++) {
			SnesHdScanlineInfo& sli = hdScreen->ScanlineInfo[sy];
			bool last = (sy == 239 - overscan.Bottom);
			bool changed = (sli.MainScreenLayers != prevMain ||
				sli.SubScreenLayers != prevSub ||
				sli.ColorMathEnabled != prevCM ||
				sli.FixedColor != prevFixed ||
				sli.ScreenBrightness != prevBr);
			if((changed || last) && prevMain != 0xFF) {
				uint32_t rangeEnd = changed ? sy - 1 : sy;
				char slBuf[256];
				snprintf(slBuf, sizeof(slBuf),
					"  Y %3u-%3u: Main=$%02X Sub=$%02X CM=$%02X AddSub=%d Fixed=$%04X Br=%d",
					rangeStart, rangeEnd, prevMain, prevSub, prevCM,
					hdScreen->ScanlineInfo[rangeStart].ColorMathAddSubscreen ? 1 : 0,
					prevFixed, prevBr);
				DiagLog(slBuf);
				rangeStart = sy;
			}
			prevMain = sli.MainScreenLayers;
			prevSub = sli.SubScreenLayers;
			prevCM = sli.ColorMathEnabled;
			prevFixed = sli.FixedColor;
			prevBr = sli.ScreenBrightness;
		}
	}
	// Reset HDMA dump flag on context change (handled by diagLoggedHashes clear)
	if(logThisFrame && frameHdmaSplit == 0) {
		hdmaDumped = false;
	}

	// Log build version once at startup
	static bool buildVersionLogged = false;
	if(!buildVersionLogged) {
		char buf[128];
		snprintf(buf, sizeof(buf), "[SNES HD diag] Build version: " SNES_HD_BUILD_VERSION);
		DiagLog(buf);
		buildVersionLogged = true;
	}
}
