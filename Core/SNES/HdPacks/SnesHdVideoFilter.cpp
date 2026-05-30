#include "pch.h"
#include "SNES/HdPacks/SnesHdVideoFilter.h"
#include "SNES/SnesConsole.h"
#include "Shared/Emulator.h"
#include "Shared/EmuSettings.h"
#include "Shared/Video/BaseVideoFilter.h"
#include "Shared/ColorUtilities.h"

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

	uint32_t hdScale = _hdScale;
	uint32_t baseWidth = 256;
	
	// The PPU output buffer width depends on hi-res mode
	uint32_t ppuWidth = _baseFrameInfo.Width;
	bool isHiRes = (ppuWidth == 512);

	// For each pixel in the original SNES frame (always 256x239 for HD info)
	for(uint32_t y = overscan.Top; y < 239 - overscan.Bottom; y++) {
		for(uint32_t x = overscan.Left; x < baseWidth - overscan.Right; x++) {
			uint32_t srcIndex = y * SnesHdScreenInfo::ScreenWidth + x;
			SnesHdPpuPixelInfo& pixelInfo = hdScreen->ScreenTiles[srcIndex];

			// Index into ppuOutputBuffer (accounts for hi-res doubling)
			uint32_t ppuIndex = isHiRes ? (y * 2 * ppuWidth + x * 2) : (y * ppuWidth + x);

			// BgTileCount is 0 (sprite/backdrop won) or 1 (the winning BG layer's tile).
			// Only apply HD tile when a BG layer won the pixel.
			SnesHdPackTileInfo* hdTile = nullptr;
			SnesHdPpuTileInfo* tileInfo = nullptr;

			if(pixelInfo.BgTileCount > 0) {
				hdTile = _hdData->GetMatchingTile(pixelInfo.BgTiles[0].Key);
				if(hdTile) {
					tileInfo = &pixelInfo.BgTiles[0];
				}
			}

			uint32_t outX = (x - overscan.Left) * hdScale;
			uint32_t outY = (y - overscan.Top) * hdScale;

			if(hdTile && tileInfo && !hdTile->HdTileData.empty()) {
				// Draw HD tile pixels for this source pixel's sub-pixels
				uint8_t srcTileX = tileInfo->OffsetX;
				uint8_t srcTileY = tileInfo->OffsetY;

				for(uint32_t dy = 0; dy < hdScale; dy++) {
					for(uint32_t dx = 0; dx < hdScale; dx++) {
						uint32_t hdPixelX = srcTileX * hdScale + dx;
						uint32_t hdPixelY = srcTileY * hdScale + dy;

						if(hdPixelX < hdTile->Width && hdPixelY < hdTile->Height) {
							uint32_t hdColor = hdTile->HdTileData[hdPixelY * hdTile->Width + hdPixelX];
							uint32_t outIndex = (outY + dy) * frameInfo.Width + (outX + dx);

							if(outIndex < frameInfo.Width * frameInfo.Height) {
								uint8_t alpha = (hdColor >> 24) & 0xFF;
								if(alpha == 0xFF) {
									outputBuffer[outIndex] = hdColor;
								} else if(alpha > 0) {
									// Alpha blend with fallback SNES color
									uint32_t bgColor = _calculatedPalette[ppuOutputBuffer[ppuIndex] & 0x7FFF];
									uint8_t srcR = (bgColor >> 16) & 0xFF;
									uint8_t srcG = (bgColor >> 8) & 0xFF;
									uint8_t srcB = bgColor & 0xFF;
									uint8_t hdR = (hdColor >> 16) & 0xFF;
									uint8_t hdG = (hdColor >> 8) & 0xFF;
									uint8_t hdB = hdColor & 0xFF;
									// HD tile data is premultiplied alpha, so blend accordingly
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
		}
	}
}
