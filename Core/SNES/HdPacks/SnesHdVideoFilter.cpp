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
#define SNES_HD_BUILD_VERSION "M5.5f"

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
	// DIAGNOSTIC: Context-aware logging with VRAM-based level detection
	// =====================================================================
	// Problem: Mesen auto-loads last save state (could be worldmap).
	// Static counters would log worldmap data and exhaust limits before
	// the user enters Level 2. Solution: detect context changes via
	// VRAM content hash at stable reference addresses (outside VBlank DMA).
	//
	// Reference tiles (from VramCompare2.cs):
	//   gfxset_37 (Level 2): VRAM 0x32E0 → hash 0x1585855B0633F405
	//   gfxset_07 (Level 1): VRAM 0x2080 → hash 0xF33C58BA8611DF5D
	// Both are outside VBlank DMA range (0x2000-0x21D0), so stable.
	// =====================================================================

	static uint64_t diagPrevVramSig = 0;
	static int diagFrameCount = 0;
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
	if(hdScreen->Vram) {
		sigA = ComputeTileContentHash(hdScreen->Vram, 0x32E0);  // gfxset_37 ref tile
		sigB = ComputeTileContentHash(hdScreen->Vram, 0x2080);  // gfxset_07 ref tile
		isLevel2 = (sigA == 0x1585855B0633F405ULL);
	}
	uint64_t vramSig = sigA ^ (sigB << 1);  // simple combined signature

	// Detect context change (level transition) → reset all diagnostic counters
	if(vramSig != diagPrevVramSig && diagPrevVramSig != 0) {
		char buf[256];
		snprintf(buf, sizeof(buf),
			"[SNES HD diag] CONTEXT CHANGE (build=" SNES_HD_BUILD_VERSION "): sig %016llX -> %016llX (isLevel2=%s)",
			(unsigned long long)diagPrevVramSig, (unsigned long long)vramSig,
			isLevel2 ? "YES" : "no");
		DiagLog(buf);
		diagFrameCount = 0;
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
	uint32_t framePalFallbackMatch = 0;    // Matches via palette fallback (Stage 2)
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
			int fallbackStopLayer = -1;  // Which layer stopped the fallback loop (for miss diagnostics)

			// Only attempt HD BG tile replacement when:
			// 1) At least one BG layer has a non-transparent tile at this pixel
			// 2) A sprite did NOT win compositing (IsSpritePixel flag in MainScreenFlags)
			// Without the sprite guard, BG tile info stored by the PPU (for fallback)
			// would incorrectly replace sprite pixels with BG HD tiles.
			bool spriteWon = (pixelInfo.MainScreenFlags & 0x40) != 0;

			// Track pixel classification for diagnostics
			if(spriteWon) {
				frameSpriteWon++;
			} else if(pixelInfo.BgLayerMask == 0) {
				frameMaskZero++;
			}

			if(pixelInfo.BgLayerMask != 0 && !spriteWon) {
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
				//    IMPORTANT: stop at the FIRST layer that has tile data
				//    (BgLayerMask bit set), regardless of whether an HD tile
				//    exists.  This prevents lower-priority layers (e.g. BG2
				//    far-background) from replacing native composited pixels
				//    that contain higher-priority content (BG1 level graphics
				//    + BG3 fog blend + sprites).
				if(!hdTile) {
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

				if(hdTile) {
					frameHdMatch++;
					if(usedFallbackLayer) {
						frameFallback++;
					}

					// Detect palette fallback: GetMatchingTile Stage 2 returned
					// a tile whose pack palette differs from runtime palette
					bool palFallback = (hdTile->Key.PaletteIndex != tileInfo->Key.PaletteIndex);
					if(palFallback) {
						framePalFallbackMatch++;
					}

					// DIAGNOSTIC: Log first 5 unique MATCHES + first 10 palette fallbacks
					if(diagMatchCount < 5 || (palFallback && diagPalFallbackCount < 10)) {
						auto& key = tileInfo->Key;
						if(key.ContentHash != 0 && diagLoggedHashes.find(key.ContentHash) == diagLoggedHashes.end()) {
							diagLoggedHashes.insert(key.ContentHash);
							char buf[320];
							if(palFallback) {
								snprintf(buf, sizeof(buf),
									"[SNES HD diag] PAL_FALLBACK_MATCH hash=%016llX runtime_pal=%d "
									"pack_pal=%d layer=%d vram=0x%04X%s",
									(unsigned long long)key.ContentHash, key.PaletteIndex,
									hdTile->Key.PaletteIndex, key.LayerIndex,
									tileInfo->VramWordAddr,
									usedFallbackLayer ? " (FALLBACK_LAYER)" : "");
								DiagLog(buf);
								diagPalFallbackCount++;
							} else {
								snprintf(buf, sizeof(buf),
									"[SNES HD diag] MATCH hash=%016llX pal=%d layer=%d%s",
									(unsigned long long)key.ContentHash, key.PaletteIndex, key.LayerIndex,
									usedFallbackLayer ? " (FALLBACK)" : "");
								DiagLog(buf);
							}
							diagMatchCount++;
						}
					}
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
									outputBuffer[outIndex] = hdColor;
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
									uint8_t outR = hdR + ((srcR * (255 - alpha)) / 255);
									uint8_t outG = hdG + ((srcG * (255 - alpha)) / 255);
									uint8_t outB = hdB + ((srcB * (255 - alpha)) / 255);
									outputBuffer[outIndex] = 0xFF000000 | (outR << 16) | (outG << 8) | outB;
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

	// DIAGNOSTIC: Log per-frame summary for first 10 frames
	// Also log if frameBgPixels==0 (catches stale build where BgLayerMask is never set)
	if(frameTotalPixels > 0 && diagFrameCount < 10) {
		char buf[1024];
		snprintf(buf, sizeof(buf),
			"[SNES HD diag] FRAME %d [%s] build=" SNES_HD_BUILD_VERSION
			": total=%u bg=%u match=%u (fb=%u palFB=%u) miss=%u palMis=%u layerMis=%u notInPack=%u"
			" sprWon=%u mask0=%u BG1=%u BG2=%u BG3=%u BG4=%u"
			" BG1miss=%u BG1notInPack=%u (TileByKey=%zu, sig=%016llX)",
			diagFrameCount,
			isLevel2 ? "LEVEL2" : "other",
			frameTotalPixels, frameBgPixels, frameHdMatch, frameFallback, framePalFallbackMatch,
			frameHdMiss,
			framePalMismatch, frameLayerMismatch, frameNotInPack,
			frameSpriteWon, frameMaskZero,
			frameLayerBits[0], frameLayerBits[1], frameLayerBits[2], frameLayerBits[3],
			frameBg1MissPixels, frameBg1NotInPack,
			_hdData->TileByKey.size(),
			(unsigned long long)vramSig);
		DiagLog(buf);
		diagFrameCount++;
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
