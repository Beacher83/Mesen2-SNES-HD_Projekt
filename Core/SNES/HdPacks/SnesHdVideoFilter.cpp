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
#define SNES_HD_BUILD_VERSION "P2.1"

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
// Phase 2.1: Winner-Only HD Compositing with CM-Skip
// =========================================================================
// Replaces the M5.19 heuristic cascade (fog-blend, overlay-blend,
// colorMathDelta, bg3-fallback) with a PPU register-driven approach:
//
//   1. Identify the PPU compositing winner (BgWinnerLayer)
//   2. If the winner has AllowColorMath → skip HD, use native pixel
//   3. Otherwise, look up HD tile for the winner layer only
//   4. Render single HD tile over native PPU pixel as base
//
// Winner-only: without Color Math, the winner fully occludes lower layers,
// so only the winner's HD tile matters. Fixes P2.0 bugs where lower-
// priority HD tiles composited over higher-priority non-HD content (e.g.
// shop text, BG3 beehives rendering in foreground).
//
// CM-skip: pixels with Color Math use the native PPU pixel (which already
// has correct color math applied). Phase 3 will apply HD color math.
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
	}
	diagPrevVramSig = vramSig;

	// Per-frame counters
	uint32_t frameTotalPixels = 0;
	uint32_t frameBgPixels = 0;
	uint32_t frameHdMatch = 0;       // pixels where HD tile found for winner layer
	uint32_t frameHdMiss = 0;        // BG pixels where NO HD tile found for winner
	uint32_t frameCmSkip = 0;        // pixels where Color Math skipped HD lookup
	uint32_t frameLayerRetry = 0;    // BG1↔BG2 layer-agnostic retry matches
	uint32_t frameSpriteWon = 0;     // pixels where sprite won (HD BG skipped)
	uint32_t frameMaskZero = 0;      // non-sprite pixels with BgLayerMask == 0
	uint32_t frameLayerBits[4] = {}; // per-layer: pixels where layer has content
	uint32_t frameWin[4] = {};       // per-layer: pixels where layer wins compositing
	uint32_t frameHdLayers[4] = {};  // per-layer: HD tile found count

	// =====================================================================
	// Main pixel loop
	// =====================================================================
	for(uint32_t y = overscan.Top; y < 239 - overscan.Bottom; y++) {

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
			// Phase 2.1: Winner-only HD tile lookup with CM-skip
			// =============================================================
			// Only look up HD tile for the PPU compositing winner.
			// Without Color Math, the winner fully occludes lower layers.
			// CM-skip: if winner has AllowColorMath, use native pixel.
			// BG1↔BG2 layer retry is structural (not a heuristic).

			SnesHdPackTileInfo* hdTile = nullptr;
			SnesHdPpuTileInfo* hdTileInfo = nullptr;

			if(pixelInfo.BgLayerMask != 0 && !spriteWon && !isWorldmap) {
				frameBgPixels++;

				uint8_t winLayer = pixelInfo.BgWinnerLayer;
				if(winLayer < 4) frameWin[winLayer]++;

				// Count per-layer content bits (diagnostic only)
				for(int li = 0; li < 4; li++) {
					if(pixelInfo.BgLayerMask & (1 << li)) frameLayerBits[li]++;
				}

				// CM-skip: if the winner pixel has AllowColorMath set, the
				// native PPU pixel already has correct color math applied.
				// Skip HD lookup entirely → native pixel used.
				// Phase 3 will implement HD color math to replace this.
				bool cmActive = (pixelInfo.MainScreenFlags & 0x80) != 0;

				if(cmActive) {
					frameCmSkip++;
					// No HD lookup — use native pixel
				} else if(winLayer < 4 && (pixelInfo.BgLayerMask & (1 << winLayer))) {
					// Look up HD tile for the winner layer only
					hdTile = _hdData->GetMatchingTile(
						pixelInfo.BgTiles[winLayer].Key, hdScreen->Vram);

					// BG1↔BG2 layer retry: same tile content may be exported
					// as layer 0 but appear on layer 1 at runtime (or vice versa).
					// Safe because BG1 and BG2 are both 4bpp in Mode 1.
					if(!hdTile && (winLayer == 0 || winLayer == 1)) {
						SnesHdTileKey altKey = pixelInfo.BgTiles[winLayer].Key;
						altKey.LayerIndex = (winLayer == 0) ? 1 : 0;
						hdTile = _hdData->GetMatchingTile(altKey, hdScreen->Vram);
						if(hdTile) frameLayerRetry++;
					}

					if(hdTile) {
						hdTileInfo = &pixelInfo.BgTiles[winLayer];
						frameHdMatch++;
						frameHdLayers[winLayer]++;

						// DIAGNOSTIC: Log first 5 unique matches per context
						if(diagMatchCount < 5) {
							auto& key = pixelInfo.BgTiles[winLayer].Key;
							if(key.ContentHash != 0
								&& diagLoggedHashes.find(key.ContentHash) == diagLoggedHashes.end()) {
								diagLoggedHashes.insert(key.ContentHash);
								char buf[320];
								snprintf(buf, sizeof(buf),
									"[SNES HD diag] MATCH hash=%016llX pal=%d layer=%d win=%d",
									(unsigned long long)key.ContentHash, key.PaletteIndex,
									key.LayerIndex, winLayer);
								DiagLog(buf);
								diagMatchCount++;
							}
						}
					} else {
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
			// Rendering
			// =============================================================
			uint32_t outX = (x - overscan.Left) * hdScale;
			uint32_t outY = (y - overscan.Top) * hdScale;

			if(hdTile && hdTileInfo && !hdTile->HdTileData.empty()) {
				// Render single HD tile over native PPU pixel base
				for(uint32_t dy = 0; dy < hdScale; dy++) {
					for(uint32_t dx = 0; dx < hdScale; dx++) {
						uint32_t outIndex = (outY + dy) * frameInfo.Width + (outX + dx);
						if(outIndex >= frameInfo.Width * frameInfo.Height) continue;

						// Start with native PPU pixel as base
						uint32_t result = _calculatedPalette[ppuOutputBuffer[ppuIndex] & 0x7FFF];

						// Compute HD sub-pixel coordinates within tile
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

						if(alpha == 0xFF) {
							// Fully opaque — replace
							result = hdColor;
						} else if(alpha > 0) {
							// Semi-transparent — alpha blend over native base
							uint8_t srcR = (result >> 16) & 0xFF;
							uint8_t srcG = (result >> 8) & 0xFF;
							uint8_t srcB = result & 0xFF;
							uint8_t hdR = (hdColor >> 16) & 0xFF;
							uint8_t hdG = (hdColor >> 8) & 0xFF;
							uint8_t hdB = hdColor & 0xFF;
							uint8_t blR = hdR + ((srcR * (255 - alpha)) / 255);
							uint8_t blG = hdG + ((srcG * (255 - alpha)) / 255);
							uint8_t blB = hdB + ((srcB * (255 - alpha)) / 255);
							result = 0xFF000000 | (blR << 16) | (blG << 8) | blB;
						}
						// alpha == 0: transparent — native pixel shows through

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
	// DIAGNOSTIC: Per-frame summary
	// =====================================================================
	bool logThisFrame = frameTotalPixels > 0 &&
		(diagFrameCount < 10 || (frameBgPixels > 0 && diagBgFrameCount < 60));
	if(logThisFrame) {
		const char* ctxLabel = isWorldmap ? "WORLDMAP" : (isLevel2 ? "LEVEL2" : "other");
		char buf[1024];
		snprintf(buf, sizeof(buf),
			"[SNES HD diag] FRAME %d/%d [%s] build=" SNES_HD_BUILD_VERSION
			": total=%u bg=%u match=%u miss=%u cmSkip=%u lRetry=%u"
			" sprWon=%u mask0=%u"
			" BG1=%u BG2=%u BG3=%u BG4=%u"
			" hdBG1=%u hdBG2=%u hdBG3=%u hdBG4=%u"
			" wn0=%u wn1=%u wn2=%u wn3=%u"
			" (TileByKey=%zu, sig=%016llX)",
			diagFrameCount, diagBgFrameCount, ctxLabel,
			frameTotalPixels, frameBgPixels, frameHdMatch,
			frameHdMiss, frameCmSkip, frameLayerRetry,
			frameSpriteWon, frameMaskZero,
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

	// Log build version once at startup
	static bool buildVersionLogged = false;
	if(!buildVersionLogged) {
		char buf[128];
		snprintf(buf, sizeof(buf), "[SNES HD diag] Build version: " SNES_HD_BUILD_VERSION);
		DiagLog(buf);
		buildVersionLogged = true;
	}
}
