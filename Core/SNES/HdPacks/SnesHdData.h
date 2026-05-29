#pragma once
#include "pch.h"
#include "Utilities/SimpleLock.h"

// ============================================================================
// SNES HD Pack Data Structures
// Adapted from Core/NES/HdPacks/HdData.h for SNES tile architecture.
//
// Key differences from NES:
// - SNES tiles are always in VRAM (no CHR ROM/RAM distinction)
// - SNES has 2bpp/4bpp/8bpp tiles (NES is always 2bpp)
// - SNES has 4 BG layers + 1 sprite layer (NES has 1 BG + sprites)
// - SNES palette indices: BG = 0-7 (from tilemap bits 10-12),
//                         Sprites = 0-7 (from OAM palette bits)
// - Tile VRAM address = ChrAddress + tileIndex * (4 * bpp)
// ============================================================================

// ---------------------------------------------------------------------------
// SnesHdTileKey — Uniquely identifies a SNES tile for HD replacement lookup
// ---------------------------------------------------------------------------
// Key = VRAM word address of tile + palette group index
// This matches the bsnes-hd texture pack format: "{vramAddr}_P{palette}.png"
//
// For BG tiles: vramAddr = LayerConfig.ChrAddress + (tilemapData & 0x3FF) * wordsPerTile
//               palette  = (tilemapData >> 10) & 0x07
//
// For Sprites:  vramAddr = computed from OAM + name table base
//               palette  = OAM palette (0-7)
//               IsSprite flag set to distinguish from BG tiles at same address

struct SnesHdTileKey
{
	uint16_t VramAddress = 0;   // VRAM word address of the tile's CHR data
	uint8_t PaletteIndex = 0;   // Palette group (0-7)
	uint8_t LayerIndex = 0;     // 0-3 = BG1-BG4, 4 = Sprites

	uint32_t GetHashCode() const
	{
		// Combine address + palette + layer into a single hash
		// VramAddress is 15 bits (0x0000-0x7FFF), palette is 3 bits, layer is 3 bits
		return ((uint32_t)VramAddress << 8) | ((uint32_t)LayerIndex << 4) | PaletteIndex;
	}

	size_t operator()(const SnesHdTileKey& tile) const
	{
		return tile.GetHashCode();
	}

	bool operator==(const SnesHdTileKey& other) const
	{
		return VramAddress == other.VramAddress
			&& PaletteIndex == other.PaletteIndex
			&& LayerIndex == other.LayerIndex;
	}
};

namespace std {
	template<> struct hash<SnesHdTileKey>
	{
		size_t operator()(const SnesHdTileKey& x) const
		{
			return x.GetHashCode();
		}
	};
}

// ---------------------------------------------------------------------------
// SnesHdPpuTileInfo — Tile identification data collected during PPU Pass 1
// ---------------------------------------------------------------------------
// The PPU collects this per-pixel (or per-tile-column) during rendering.
// Used by the HD rendering pass to look up replacement tiles.

struct SnesHdPpuTileInfo
{
	SnesHdTileKey Key;

	uint8_t OffsetX = 0;        // Pixel X offset within the 8x8 tile (0-7)
	uint8_t OffsetY = 0;        // Pixel Y offset within the 8x8 tile (0-7)
	bool HorizontalMirror = false;
	bool VerticalMirror = false;
	uint8_t Priority = 0;       // SNES tile priority (from tilemap bit 13)

	uint16_t TilemapData = 0;   // Raw 16-bit tilemap word (for conditions/debugging)
};

// ---------------------------------------------------------------------------
// SnesHdPpuPixelInfo — Per-pixel info collected by PPU for HD rendering
// ---------------------------------------------------------------------------
// Two-pass approach (like NES):
//   Pass 1: PPU fills SnesHdScreenInfo with tile identification per pixel
//   Pass 2: HD renderer looks up replacement tiles and draws scaled output

struct SnesHdPpuPixelInfo
{
	SnesHdPpuTileInfo BgTiles[4] = {};   // BG1-BG4 tile info at this pixel
	uint8_t BgTileCount = 0;             // How many BG layers contributed

	SnesHdPpuTileInfo Sprites[4] = {};   // Up to 4 sprite tiles at this pixel
	uint8_t SpriteCount = 0;

	uint16_t MainScreenColor = 0;        // Original SNES BGR555 color (for fallback)
	uint16_t SubScreenColor = 0;
	uint8_t MainScreenFlags = 0;
	uint8_t MainScreenPriority = 0;
};

// ---------------------------------------------------------------------------
// SnesHdScreenInfo — Full frame of pixel info for HD rendering
// ---------------------------------------------------------------------------

struct SnesHdScreenInfo
{
	static constexpr int ScreenWidth = 256;
	static constexpr int ScreenHeight = 240;   // Max visible (including overscan)
	static constexpr int ScreenPixelCount = ScreenWidth * ScreenHeight;

	SnesHdPpuPixelInfo* ScreenTiles;
	uint32_t FrameNumber = 0;
	uint16_t Scanline = 0;

	SnesHdScreenInfo(const SnesHdScreenInfo&) = delete;
	SnesHdScreenInfo& operator=(const SnesHdScreenInfo&) = delete;

	SnesHdScreenInfo()
	{
		ScreenTiles = new SnesHdPpuPixelInfo[ScreenPixelCount]();
	}

	~SnesHdScreenInfo()
	{
		delete[] ScreenTiles;
	}
};

// ---------------------------------------------------------------------------
// SnesHdPackTileInfo — A loaded HD replacement tile (PNG pixel data)
// ---------------------------------------------------------------------------

struct SnesHdPackTileInfo
{
	SnesHdTileKey Key;

	uint32_t BitmapIndex = 0;   // Index into SnesHdPackData::ImageFileData
	uint32_t X = 0;             // X offset in the source PNG atlas
	uint32_t Y = 0;             // Y offset in the source PNG atlas
	uint32_t Width = 0;         // HD tile width (e.g. 32 for 4x scale)
	uint32_t Height = 0;        // HD tile height

	int Brightness = 255;       // Brightness adjustment (0-255)
	bool Blank = false;         // All pixels are the same color
	bool HasTransparentPixels = false;
	bool IsFullyTransparent = false;

	vector<uint32_t> HdTileData; // Pre-loaded RGBA pixel data (Width * Height)

	void UpdateFlags()
	{
		Blank = true;
		HasTransparentPixels = false;
		IsFullyTransparent = true;
		for(size_t i = 0; i < HdTileData.size(); i++) {
			if(HdTileData[i] != HdTileData[0]) {
				Blank = false;
			}
			if((HdTileData[i] & 0xFF000000) != 0xFF000000) {
				HasTransparentPixels = true;
			}
			if(HdTileData[i] & 0xFF000000) {
				IsFullyTransparent = false;
			}
		}
	}
};

// ---------------------------------------------------------------------------
// SnesHdBitmapInfo — A loaded PNG file (shared by multiple tiles in an atlas)
// ---------------------------------------------------------------------------

struct SnesHdBitmapInfo
{
private:
	bool _initDone = false;
	SimpleLock _lock;

public:
	string PngName;
	vector<uint8_t> FileData;
	vector<uint32_t> PixelData;
	uint32_t Width = 0;
	uint32_t Height = 0;

	void Init();  // Deferred PNG decode for async loading (used by LoadAsync)

	void PremultiplyAlpha()
	{
		for(size_t i = 0; i < PixelData.size(); i++) {
			if(PixelData[i] < 0xFF000000) {
				uint8_t* output = (uint8_t*)(PixelData.data() + i);
				uint8_t alpha = output[3] + 1;
				output[0] = (uint8_t)((alpha * output[0]) >> 8);
				output[1] = (uint8_t)((alpha * output[1]) >> 8);
				output[2] = (uint8_t)((alpha * output[2]) >> 8);
			}
		}
	}
};

// ---------------------------------------------------------------------------
// SnesHdPackData — All loaded HD pack data for a game
// ---------------------------------------------------------------------------

struct SnesHdPackData
{
private:
	bool _cancelLoad = false;

public:
	uint32_t Scale = 1;         // HD scale factor (e.g. 4 for 4x)
	uint32_t Version = 0;       // Pack format version

	// All loaded PNG files
	vector<unique_ptr<SnesHdBitmapInfo>> ImageFileData;

	// All tile replacements
	vector<unique_ptr<SnesHdPackTileInfo>> Tiles;

	// Fast lookup: key → HD tile info
	// Multiple entries per key are possible (for conditional replacements in future)
	unordered_map<SnesHdTileKey, vector<SnesHdPackTileInfo*>> TileByKey;

	SnesHdPackData() {}
	~SnesHdPackData() {}

	SnesHdPackData(const SnesHdPackData&) = delete;
	SnesHdPackData& operator=(const SnesHdPackData&) = delete;

	void CancelLoad() { _cancelLoad = true; }

	void LoadAsync()
	{
		for(auto& bitmap : ImageFileData) {
			bitmap->Init();
			if(_cancelLoad) return;
		}
	}

	// Look up an HD tile replacement. Returns nullptr if no replacement exists.
	SnesHdPackTileInfo* GetMatchingTile(const SnesHdTileKey& key)
	{
		auto it = TileByKey.find(key);
		if(it != TileByKey.end() && !it->second.empty()) {
			// For now, return the first match (no conditions yet)
			SnesHdPackTileInfo* tile = it->second[0];
			if(tile->IsFullyTransparent) {
				return nullptr;
			}
			return tile;
		}
		return nullptr;
	}
};
