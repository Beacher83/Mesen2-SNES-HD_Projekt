#include "pch.h"
#include "SNES/HdPacks/SnesHdVideoFilter.h"
#include "SNES/SnesConsole.h"
#include "Shared/Emulator.h"
#include "Shared/EmuSettings.h"
#include "Shared/Video/BaseVideoFilter.h"
#include "Shared/ColorUtilities.h"
#include "Shared/MessageManager.h"
#include <unordered_set>
#include <cstdlib>    // getenv (for DiagLog file path)

// Build version — logged in diagnostics so test PC can verify correct code is running.
// Increment this on every push to catch stale-build issues.
#define SNES_HD_BUILD_VERSION "M5.10"

// ---------------------------------------------------------------------------
// DiagLog — writes to both Mesen's log window AND a persistent text file.
// File is created once per session at %USERPROFILE%\Downloads\snes_hd_diag.txt
// (or $HOME/Downloads/ on non-Windows). Flushed after every write so crash
// won't lose data. The user can open the file after the session.
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
	// Output is scaled by HD pack scale factor
	// Base SNES resolution: 256x239 (no overscan adjustments for now)
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

void SnesHdVideoFilter::ApplyFilter(uint16_t* ppuOutputBuffer)
{
	if(_frameData == nullptr) {
		return;
	}

	SnesHdScreenInfo* hdScreen = (SnesHdScreenInfo*)_frameData;
	uint32_t* outputBuffer = GetOutputBuffer();
	FrameInfo frameInfo = _frameInfo;
	OverscanDimensions overscan = GetOverscan();

	// DISABLED (Phase 1 fix): DetectActiveGfxset() is fundamentally broken for
	// DKC2 because VBlank DMA overwrites the VRAM regions where fingerprint
	// reference tiles were stored (pre-DMA snapshot). Result: ActiveGfxset is
	// always -1, blocking all gfxset-scoped HD tiles.
	// The corresponding scoping check in GetMatchingTile() is also disabled.
	// See SnesHdData.h for full explanation.
	//
	// _hdData->DetectActiveGfxset(hdScreen->Vram);

	uint32_t hdScale = _hdScale;
	uint32_t baseWidth = 256;
	
	// The PPU output buffer width depends on hi-res mode
	uint32_t ppuWidth = _baseFrameInfo.Width;
	bool isHiRes = (ppuWidth == 512);

	// =====================================================================
	// DIAGNOSTIC: Context-aware logging with VRAM-based level/worldmap detection
	// =========================================================================
	// Reference tiles (stable, outside VBlank DMA range 0x2000-0x21D0):
	//   gfxset_37 (Level 2): VRAM 0x32E0 → hash 0x1585855B0633F405
	//   gfxset_07 (Level 1): VRAM 0x2080 → hash 0xF33C58BA8611DF5D
	// Combined vramSig = sigA ^ (sigB << 1) — unique per screen context.
	// Worldmap: stable sig 0xDBF342F9932FD251 (verified across multiple sessions).
	// =========================================================================

	static uint64_t diagPrevVramSig = 0;
	static int diagFrameCount = 0;     // all frames (incl. loading, limit 10)
	static int diagBgFrameCount = 0;   // gameplay frames only (bg>0, limit 60)
	static int diagMissCount = 0;
	static int diagPalMismatchCount = 0;
	static int diagLayerMismatchCount = 0;
	static int diagMatchCount = 0;
	static int diagPalFallbackCount = 0;
	static int diagDetailCount = 0;
	static std::unordered_set<uint64_t> diagLoggedHashes;

	// Compute VRAM context signature from two stable reference tiles
	uint64_t sigA = 0, sigB = 0;
	bool isLevel2 = false;
	bool isWorldmap = false;
	if(hdScreen->Vram) {
		sigA = ComputeTileContentHash(hdScreen->Vram, 0x32E0);  // gfxset_37 ref tile
		sigB = ComputeTileContentHash(hdScreen->Vram, 0x2080);  // gfxset_07 ref tile
		isLevel2  = (sigA == 0x1585855B0633F405ULL);
	}
	uint64_t vramSig = sigA ^ (sigB << 1);
	isWorldmap = (vramSig == 0xDBF342F9932FD251ULL);

	// Detect context change (level transition) → reset all diagnostic counters
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
		diagPalMismatchCount = 0;
		diagLayerMismatchCount = 0;
		diagMatchCount = 0;
		diagPalFallbackCount = 0;
		diagDetailCount = 0;
		diagLoggedHashes.clear();
	}
	diagPrevVramSig = vramSig;

	// Per-frame counters (reset each frame, not static)
	uint32_t frameBgPixels = 0;
	uint32_t frameHdMatch = 0;
	uint32_t frameHdMiss = 0;
	uint32_t framePalMismatch = 0;
	uint32_t frameLayerMismatch = 0;
	uint32_t frameNotInPack = 0;           // Hashes not found with ANY pal×layer combo
	uint32_t frameBg1MissPixels = 0;       // BG1 miss pixel count
	uint32_t frameBg1NotInPack = 0;        // BG1 unique hashes not in pack at all
	uint32_t frameFallback = 0;
	uint32_t frameBg3FogBlend = 0;         // BG3 fog + color math → HD BG1 rendered with fog blend
	uint32_t frameBg3FogNative = 0;        // BG3 fog won → native pixel preserved (no fallback)
	uint32_t frameSpriteWon = 0;           // Pixels where sprite won (HD BG skipped)
	uint32_t frameMaskZero = 0;            // Non-sprite pixels with BgLayerMask == 0
	uint32_t frameLayerBits[4] = {};       // Per-layer pixel counts (bit N set in mask)
	uint32_t frameTotalPixels = 0;         // Total pixels iterated

	// For each pixel in the original SNES frame (always 256x239 for HD info)
	for(uint32_t y = overscan.Top; y < 239 - overscan.Bottom; y++) {
		for(uint32_t x = overscan.Left; x < baseWidth - overscan.Right; x++) {
			uint32_t srcIndex = y * SnesHdScreenInfo::ScreenWidth + x;
			SnesHdPpuPixelInfo& pixelInfo = hdScreen->ScreenTiles[srcIndex];

			// Index into ppuOutputBuffer (accounts for hi-res doubling)
			uint32_t ppuIndex = isHiRes ? (y * 2 * ppuWidth + x * 2) : (y * ppuWidth + x);

			frameTotalPixels++;

			// -----------------------------------------------------------------
			// Multi-layer HD tile lookup with fallback
			// -----------------------------------------------------------------
			// BgLayerMask is a bitmask of which BG layers have non-transparent
			// tiles at this pixel. BgWinnerLayer is the compositing winner.
			// Strategy: try the winner layer first. If no HD tile exists for
			// the winner, try other layers (e.g. BG1 under BG3 fog in DKC2).
			SnesHdPackTileInfo* hdTile = nullptr;
			SnesHdPpuTileInfo* tileInfo = nullptr;
			bool usedFallbackLayer = false;
			bool bg3FogBlend = false;  // true when rendering HD BG1 under semi-transparent BG3 fog
			int fallbackStopLayer = -1;  // Which layer stopped the fallback loop (for miss diagnostics)

			// Only attempt HD BG tile replacement when:
			// 1) At least one BG layer has a non-transparent tile at this pixel
			// 2) A sprite did NOT win compositing
			// 3) We are NOT on the Worldmap — Worldmap tiles are unique to that
			//    context but share gfxset_7 CHR data with Level 1, causing false
			//    matches. Gate off entirely so native rendering is always used there.
			// Two cases for sprite: wins main screen (IsSpritePixel in MainScreenFlags),
			// or wins sub-screen only (SubScreenHasSprite). In DKC2 Level 2,
			// BG3 fog wins main screen while sprites contribute via color math from
			// sub-screen — IsSpritePixel is never set there, but SubScreenHasSprite is.
			bool spriteWon = (pixelInfo.MainScreenFlags & 0x40) != 0 || pixelInfo.SubScreenHasSprite;

			// Track pixel classification for diagnostics
			if(spriteWon) {
				frameSpriteWon++;
			} else if(pixelInfo.BgLayerMask == 0) {
				frameMaskZero++;
			}

			if(pixelInfo.BgLayerMask != 0 && !spriteWon && !isWorldmap) {
				frameBgPixels++;

				// Count which layers are present at BG pixels
				for(int li = 0; li < 4; li++) {
					if(pixelInfo.BgLayerMask & (1 << li)) frameLayerBits[li]++;
				}

				// 1) Try the compositing winner first
				uint8_t winLayer = pixelInfo.BgWinnerLayer;
				if(winLayer < 4 && (pixelInfo.BgLayerMask & (1 << winLayer))) {
					hdTile = _hdData->GetMatchingTile(pixelInfo.BgTiles[winLayer].Key, hdScreen->Vram);
					if(hdTile) {
						tileInfo = &pixelInfo.BgTiles[winLayer];
					}
				}

				// 2) Fallback: try other layers if winner has no HD tile
				//    GATE: Skip fallback when BG3 (layer 2) wins compositing
				//    WITHOUT color math (opaque foreground — Issue A fix).
				//    When BG3 wins WITH color math (AllowColorMath set), it's a
				//    semi-transparent effect (fog, honey, water) — fall through
				//    to the fog-blend path below instead.
				//
				//    IMPORTANT: stop at the FIRST layer that has tile data
				//    (BgLayerMask bit set), regardless of whether an HD tile
				//    exists.  This prevents lower-priority layers (e.g. BG2
				//    far-background) from replacing native composited pixels
				//    that contain higher-priority content (BG1 level graphics
				//    + BG3 fog blend + sprites).
				if(!hdTile && winLayer != 2) {
					for(int i = 0; i < 4; i++) {
						if(i == (int)winLayer) continue;
						if(pixelInfo.BgLayerMask & (1 << i)) {
							hdTile = _hdData->GetMatchingTile(pixelInfo.BgTiles[i].Key, hdScreen->Vram);
							if(hdTile) {
								tileInfo = &pixelInfo.BgTiles[i];
								usedFallbackLayer = true;
							} else {
								fallbackStopLayer = i;  // Track for miss diagnostics
							}
							// Stop here: this layer has data. Either we found
							// an HD tile and will use it, or we didn't and the
							// pixel should fall through to native rendering.
							// Continuing would let a lower-priority layer's HD
							// tile incorrectly cover this pixel.
							break;
						}
					}
				}

				// 3) BG3 fog-blend path: BG3 won compositing WITH color math
				//    (semi-transparent effect like fog/honey/water in DKC2).
				//    Try to find an HD tile on BG1 or BG2 underneath and render
				//    it with the BG3 color blended on top (replicating color math).
				if(!hdTile && winLayer == 2 && (pixelInfo.MainScreenFlags & 0x80)) {
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
				}

				if(hdTile) {
					frameHdMatch++;
					if(usedFallbackLayer) {
						frameFallback++;
					}
					if(bg3FogBlend) {
						frameBg3FogBlend++;
					}

					// DIAGNOSTIC: Log first 5 unique MATCHES
					if(diagMatchCount < 5) {
						auto& key = tileInfo->Key;
						if(key.ContentHash != 0 && diagLoggedHashes.find(key.ContentHash) == diagLoggedHashes.end()) {
							diagLoggedHashes.insert(key.ContentHash);
							char buf[320];
							{
								snprintf(buf, sizeof(buf),
									"[SNES HD diag] MATCH hash=%016llX pal=%d layer=%d%s%s",
									(unsigned long long)key.ContentHash, key.PaletteIndex, key.LayerIndex,
									usedFallbackLayer ? " (FALLBACK)" : "",
									bg3FogBlend ? " (FOG-BLEND)" : "");
								DiagLog(buf);
							}
							diagMatchCount++;
						}
					}
				} else if(winLayer == 2) {
					// BG3 fog won compositing — no HD tile for fog (expected).
					// The native composited pixel is correct (contains underlying
					// BG layers + BG3 fog blend).  Do NOT count as a miss.
					frameBg3FogNative++;
				} else {
					frameHdMiss++;

					// DIAGNOSTIC: Enhanced miss analysis with layer-mismatch and VRAM address
					//
					// KEY FIX (M5.5f): When the fallback loop stopped at a layer
					// (has data but no HD tile), analyze THAT layer's tile info.
					// Previously we always analyzed the winner layer, which meant
					// BG3 fog (winner) was checked instead of BG1 (fallback stop).
					// This made BG1miss always 0 and hid palette mismatches.
					//
					// Priority: fallbackStopLayer > winner > first layer with data
					SnesHdPpuTileInfo* missLayerInfo = nullptr;
					const char* missSource = "unknown";

					if(fallbackStopLayer >= 0 && fallbackStopLayer < 4) {
						// Fallback loop stopped at this layer — it's the actual
						// failing lookup we care about (e.g. BG1 under BG3 fog)
						missLayerInfo = &pixelInfo.BgTiles[fallbackStopLayer];
						missSource = "fallback";
					} else if(winLayer < 4 && (pixelInfo.BgLayerMask & (1 << winLayer))) {
						// No fallback layers had data — only the winner missed
						missLayerInfo = &pixelInfo.BgTiles[winLayer];
						missSource = "winner";
					} else {
						for(int mi = 0; mi < 4; mi++) {
							if(pixelInfo.BgLayerMask & (1 << mi)) {
								missLayerInfo = &pixelInfo.BgTiles[mi];
								missSource = "first";
								break;
							}
						}
					}

					if(missLayerInfo && missLayerInfo->Key.ContentHash != 0) {
						auto& key = missLayerInfo->Key;
						bool isBg1Miss = (key.LayerIndex == 0);
						if(isBg1Miss) frameBg1MissPixels++;

						// Exhaustive search: check all palette × layer combinations
						bool foundWithOtherPal = false;
						bool foundWithOtherLayer = false;
						uint8_t foundPal = 0;
						uint8_t foundLayer = 0;

						for(int tryLayer = 0; tryLayer < 4; tryLayer++) {
							for(int tryPal = 0; tryPal < 8; tryPal++) {
								if(tryPal == key.PaletteIndex && tryLayer == key.LayerIndex) continue;
								SnesHdTileKey tryKey;
								tryKey.ContentHash = key.ContentHash;
								tryKey.PaletteIndex = (uint8_t)tryPal;
								tryKey.LayerIndex = (uint8_t)tryLayer;
								auto it = _hdData->TileByKey.find(tryKey);
								if(it != _hdData->TileByKey.end()) {
									foundPal = (uint8_t)tryPal;
									foundLayer = (uint8_t)tryLayer;
									if(tryPal != key.PaletteIndex) {
										foundWithOtherPal = true;
									} else {
										foundWithOtherLayer = true;
									}
									goto foundMatch;
								}
							}
						}
						foundMatch:

						if(foundWithOtherPal) {
							framePalMismatch++;
							if(diagPalMismatchCount < 20) {
								char buf[320];
								snprintf(buf, sizeof(buf),
									"[SNES HD diag] PAL MISMATCH hash=%016llX runtime_pal=%d pack_pal=%d "
									"runtime_layer=%d pack_layer=%d vram=0x%04X",
									(unsigned long long)key.ContentHash, key.PaletteIndex, foundPal,
									key.LayerIndex, foundLayer, missLayerInfo->VramWordAddr);
								DiagLog(buf);
								diagPalMismatchCount++;
							}
						} else if(foundWithOtherLayer) {
							frameLayerMismatch++;
							if(diagLayerMismatchCount < 20) {
								char buf[320];
								snprintf(buf, sizeof(buf),
									"[SNES HD diag] LAYER MISMATCH hash=%016llX pal=%d runtime_layer=%d "
									"pack_layer=%d vram=0x%04X",
									(unsigned long long)key.ContentHash, key.PaletteIndex,
									key.LayerIndex, foundLayer, missLayerInfo->VramWordAddr);
								DiagLog(buf);
								diagLayerMismatchCount++;
							}
						} else {
							// Hash not found in pack with ANY key combination
							frameNotInPack++;
							if(isBg1Miss) frameBg1NotInPack++;

							if(diagMissCount < 60) {
								if(diagLoggedHashes.find(key.ContentHash) == diagLoggedHashes.end()) {
									diagLoggedHashes.insert(key.ContentHash);
									bool inDmaRange = (missLayerInfo->VramWordAddr >= 0x2000
										&& missLayerInfo->VramWordAddr <= 0x21D0);
									char buf[512];
									snprintf(buf, sizeof(buf),
										"[SNES HD diag] MISS hash=%016llX pal=%d layer=%d vram=0x%04X%s "
										"mask=0x%02X win=%d src=%s",
										(unsigned long long)key.ContentHash, key.PaletteIndex,
										key.LayerIndex, missLayerInfo->VramWordAddr,
										inDmaRange ? " [DMA_RANGE]" : "",
										pixelInfo.BgLayerMask, pixelInfo.BgWinnerLayer,
										missSource);
									DiagLog(buf);
									diagMissCount++;

									// For first 5 misses, dump all layer hashes with VRAM addr
									if(diagDetailCount < 5) {
										for(int dli = 0; dli < 4; dli++) {
											if(pixelInfo.BgLayerMask & (1 << dli)) {
												auto& dk = pixelInfo.BgTiles[dli].Key;
												bool dInDma = (pixelInfo.BgTiles[dli].VramWordAddr >= 0x2000
													&& pixelInfo.BgTiles[dli].VramWordAddr <= 0x21D0);
												snprintf(buf, sizeof(buf),
													"[SNES HD diag]   -> BgTiles[%d]: hash=%016llX pal=%d "
													"layer=%d vram=0x%04X%s",
													dli, (unsigned long long)dk.ContentHash, dk.PaletteIndex,
													dk.LayerIndex, pixelInfo.BgTiles[dli].VramWordAddr,
													dInDma ? " [DMA_RANGE]" : "");
												DiagLog(buf);
											}
										}
										diagDetailCount++;
									}
								}
							}
						}
					}
				}
			} // end if(pixelInfo.BgLayerMask != 0)

			uint32_t outX = (x - overscan.Left) * hdScale;
			uint32_t outY = (y - overscan.Top) * hdScale;

			if(hdTile && tileInfo && !hdTile->HdTileData.empty()) {
				uint8_t rawX = tileInfo->OffsetX;
				uint8_t rawY = tileInfo->OffsetY;
				bool hFlip = tileInfo->HorizontalMirror;
				bool vFlip = tileInfo->VerticalMirror;
				uint8_t srcTileX = hFlip ? (7 - rawX) : rawX;
				uint8_t srcTileY = vFlip ? (7 - rawY) : rawY;

				// Pre-compute fog color if this pixel uses BG3 fog blending.
				// MainScreenColor holds the raw BG3 layer color captured before
				// the PPU applied color math.
				// Blend weight: 75% HD tile + 25% fog color.  The original PPU
				// uses 50/50 half-addition, but HD tiles are painted brighter than
				// native tiles, so a lighter fog weight preserves art quality while
				// still conveying the atmospheric effect.
				uint32_t fogColorRGB = 0;
				uint8_t fogR = 0, fogG = 0, fogB = 0;
				if(bg3FogBlend) {
					fogColorRGB = _calculatedPalette[pixelInfo.MainScreenColor & 0x7FFF];
					fogR = (fogColorRGB >> 16) & 0xFF;
					fogG = (fogColorRGB >> 8) & 0xFF;
					fogB = fogColorRGB & 0xFF;
				}

				for(uint32_t dy = 0; dy < hdScale; dy++) {
					for(uint32_t dx = 0; dx < hdScale; dx++) {
						uint32_t hdPixelX = srcTileX * hdScale + (hFlip ? (hdScale - 1 - dx) : dx);
						uint32_t hdPixelY = srcTileY * hdScale + (vFlip ? (hdScale - 1 - dy) : dy);

						if(hdPixelX < hdTile->Width && hdPixelY < hdTile->Height) {
							uint32_t hdColor = hdTile->HdTileData[hdPixelY * hdTile->Width + hdPixelX];
							uint32_t outIndex = (outY + dy) * frameInfo.Width + (outX + dx);

							if(outIndex < frameInfo.Width * frameInfo.Height) {
								uint8_t alpha = (hdColor >> 24) & 0xFF;
								if(alpha == 0xFF) {
									if(bg3FogBlend) {
										// BG3 fog-blend: render HD tile with atmospheric
										// fog tint.  75% HD + 25% fog keeps art visible
										// while conveying the fog effect.
										uint8_t hdR = (hdColor >> 16) & 0xFF;
										uint8_t hdG = (hdColor >> 8) & 0xFF;
										uint8_t hdB = hdColor & 0xFF;
										uint8_t outR = (uint8_t)((hdR * 3 + fogR) >> 2);
										uint8_t outG = (uint8_t)((hdG * 3 + fogG) >> 2);
										uint8_t outB = (uint8_t)((hdB * 3 + fogB) >> 2);
										outputBuffer[outIndex] = 0xFF000000 | (outR << 16) | (outG << 8) | outB;
									} else {
										outputBuffer[outIndex] = hdColor;
									}
								} else if(alpha > 0) {
									// Alpha blend with sub-screen (BG2/backdrop) as background.
									// Using main-screen would blend HD tile with native BG1 itself,
									// creating a doubled appearance for semi-transparent tiles.
									uint32_t bgColor = _calculatedPalette[pixelInfo.SubScreenColor & 0x7FFF];
									uint8_t srcR = (bgColor >> 16) & 0xFF;
									uint8_t srcG = (bgColor >> 8) & 0xFF;
									uint8_t srcB = bgColor & 0xFF;
									uint8_t hdR = (hdColor >> 16) & 0xFF;
									uint8_t hdG = (hdColor >> 8) & 0xFF;
									uint8_t hdB = hdColor & 0xFF;
									uint8_t blendR = hdR + ((srcR * (255 - alpha)) / 255);
									uint8_t blendG = hdG + ((srcG * (255 - alpha)) / 255);
									uint8_t blendB = hdB + ((srcB * (255 - alpha)) / 255);
									if(bg3FogBlend) {
										// Apply fog on top of the alpha-blended result (75/25)
										blendR = (uint8_t)((blendR * 3 + fogR) >> 2);
										blendG = (uint8_t)((blendG * 3 + fogG) >> 2);
										blendB = (uint8_t)((blendB * 3 + fogB) >> 2);
									}
									outputBuffer[outIndex] = 0xFF000000 | (blendR << 16) | (blendG << 8) | blendB;
								} else {
									outputBuffer[outIndex] = _calculatedPalette[ppuOutputBuffer[ppuIndex] & 0x7FFF];
								}
							}
						}
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

	// DIAGNOSTIC: Log per-frame summary.
	// Always log first 10 frames (loading screens included) — catches stale builds.
	// Additionally log up to 60 gameplay frames (frameBgPixels > 0) per context.
	bool logThisFrame = frameTotalPixels > 0 &&
		(diagFrameCount < 10 || (frameBgPixels > 0 && diagBgFrameCount < 60));
	if(logThisFrame) {
		const char* ctxLabel = isWorldmap ? "WORLDMAP" : (isLevel2 ? "LEVEL2" : "other");
		char buf[1024];
		snprintf(buf, sizeof(buf),
			"[SNES HD diag] FRAME %d/%d [%s] build=" SNES_HD_BUILD_VERSION
			": total=%u bg=%u match=%u (fb=%u fogB=%u) miss=%u fogNat=%u palMis=%u layerMis=%u notInPack=%u"
			" sprWon=%u mask0=%u BG1=%u BG2=%u BG3=%u BG4=%u"
			" BG1miss=%u BG1notInPack=%u (TileByKey=%zu, sig=%016llX)",
			diagFrameCount, diagBgFrameCount,
			ctxLabel,
			frameTotalPixels, frameBgPixels, frameHdMatch, frameFallback, frameBg3FogBlend,
			frameHdMiss, frameBg3FogNative,
			framePalMismatch, frameLayerMismatch, frameNotInPack,
			frameSpriteWon, frameMaskZero,
			frameLayerBits[0], frameLayerBits[1], frameLayerBits[2], frameLayerBits[3],
			frameBg1MissPixels, frameBg1NotInPack,
			_hdData->TileByKey.size(),
			(unsigned long long)vramSig);
		DiagLog(buf);
		diagFrameCount++;
		if(frameBgPixels > 0) {
			diagBgFrameCount++;
		}
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
