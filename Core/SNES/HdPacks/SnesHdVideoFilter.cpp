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
#define SNES_HD_BUILD_VERSION "M5.5b"

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
	static int diagMatchCount = 0;
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
		diagMatchCount = 0;
		diagDetailCount = 0;
		diagLoggedHashes.clear();
	}
	diagPrevVramSig = vramSig;

	// Per-frame counters (reset each frame, not static)
	uint32_t frameBgPixels = 0;
	uint32_t frameHdMatch = 0;
	uint32_t frameHdMiss = 0;
	uint32_t framePalMismatch = 0;
	uint32_t frameFallback = 0;
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
				if(!hdTile) {
					for(int i = 0; i < 4; i++) {
						if(i == (int)winLayer) continue;
						if(pixelInfo.BgLayerMask & (1 << i)) {
							hdTile = _hdData->GetMatchingTile(pixelInfo.BgTiles[i].Key, hdScreen->Vram);
							if(hdTile) {
								tileInfo = &pixelInfo.BgTiles[i];
								usedFallbackLayer = true;
								break;
							}
						}
					}
				}

				if(hdTile) {
					frameHdMatch++;
					if(usedFallbackLayer) {
						frameFallback++;
					}

					// DIAGNOSTIC: Log first 5 unique MATCHES
					if(diagMatchCount < 5) {
						auto& key = tileInfo->Key;
						if(key.ContentHash != 0 && diagLoggedHashes.find(key.ContentHash) == diagLoggedHashes.end()) {
							char buf[256];
							snprintf(buf, sizeof(buf),
								"[SNES HD diag] MATCH hash=%016llX pal=%d layer=%d%s",
								(unsigned long long)key.ContentHash, key.PaletteIndex, key.LayerIndex,
								usedFallbackLayer ? " (FALLBACK)" : "");
							DiagLog(buf);
							diagMatchCount++;
						}
					}
				} else {
					frameHdMiss++;

					// DIAGNOSTIC: Log misses for the winner layer
					auto& key = (winLayer < 4 && (pixelInfo.BgLayerMask & (1 << winLayer)))
						? pixelInfo.BgTiles[winLayer].Key
						: pixelInfo.BgTiles[0].Key;
					if(key.ContentHash != 0) {
						bool foundWithOtherPal = false;
						for(int tryPal = 0; tryPal < 8; tryPal++) {
							if(tryPal == key.PaletteIndex) continue;
							SnesHdTileKey tryKey;
							tryKey.ContentHash = key.ContentHash;
							tryKey.PaletteIndex = (uint8_t)tryPal;
							tryKey.LayerIndex = key.LayerIndex;
							auto it = _hdData->TileByKey.find(tryKey);
							if(it != _hdData->TileByKey.end()) {
								foundWithOtherPal = true;
								framePalMismatch++;
								if(diagPalMismatchCount < 20) {
									char buf[256];
									snprintf(buf, sizeof(buf),
										"[SNES HD diag] PAL MISMATCH hash=%016llX runtime_pal=%d pack_pal=%d layer=%d",
										(unsigned long long)key.ContentHash, key.PaletteIndex, tryPal, key.LayerIndex);
									DiagLog(buf);
									diagPalMismatchCount++;
								}
								break;
							}
						}

						if(!foundWithOtherPal && diagMissCount < 60) {
							if(diagLoggedHashes.find(key.ContentHash) == diagLoggedHashes.end()) {
								diagLoggedHashes.insert(key.ContentHash);
								char buf[512];
								snprintf(buf, sizeof(buf),
									"[SNES HD diag] MISS hash=%016llX pal=%d layer=%d mask=0x%02X win=%d",
									(unsigned long long)key.ContentHash, key.PaletteIndex, key.LayerIndex,
									pixelInfo.BgLayerMask, pixelInfo.BgWinnerLayer);
								DiagLog(buf);
								diagMissCount++;

								// For first 5 misses, also dump all layer hashes for this pixel
								if(diagDetailCount < 5) {
									for(int dli = 0; dli < 4; dli++) {
										if(pixelInfo.BgLayerMask & (1 << dli)) {
											auto& dk = pixelInfo.BgTiles[dli].Key;
											snprintf(buf, sizeof(buf),
												"[SNES HD diag]   -> BgTiles[%d]: hash=%016llX pal=%d layer=%d",
												dli, (unsigned long long)dk.ContentHash, dk.PaletteIndex, dk.LayerIndex);
											DiagLog(buf);
										}
									}
									diagDetailCount++;
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
		char buf[768];
		snprintf(buf, sizeof(buf),
			"[SNES HD diag] FRAME %d [%s] build=" SNES_HD_BUILD_VERSION
			": total=%u bg=%u match=%u (fb=%u) miss=%u palMis=%u sprWon=%u mask0=%u"
			" BG1=%u BG2=%u BG3=%u BG4=%u (TileByKey=%zu, sig=%016llX)",
			diagFrameCount,
			isLevel2 ? "LEVEL2" : "other",
			frameTotalPixels, frameBgPixels, frameHdMatch, frameFallback, frameHdMiss, framePalMismatch,
			frameSpriteWon, frameMaskZero,
			frameLayerBits[0], frameLayerBits[1], frameLayerBits[2], frameLayerBits[3],
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
