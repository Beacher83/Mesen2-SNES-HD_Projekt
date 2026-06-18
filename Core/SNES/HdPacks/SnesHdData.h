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
// FNV-1a 64-bit hash — content-based tile identification
// ---------------------------------------------------------------------------
// Must match the JavaScript fnv1a_64() in the DKC2 viewer exactly.
// Hashes the raw VRAM bytes of a tile's CHR data.
// On x86 little-endian: (uint8_t*)(vram + wordAddr) gives bytes in the
// same order as the viewer's Uint8Array VRAM snapshot.

inline uint64_t ComputeTileContentHash(const uint16_t* vram, uint16_t wordAddress, uint16_t wordCount = 16)
{
	// Bounds check: don't read past end of VRAM (0x8000 words)
	if(wordAddress + wordCount > 0x8000) {
		wordCount = 0x8000 - wordAddress;
	}

	const uint8_t* bytes = reinterpret_cast<const uint8_t*>(vram + wordAddress);
	uint32_t byteCount = (uint32_t)wordCount * 2;

	uint64_t hash = 0xcbf29ce484222325ULL;
	constexpr uint64_t prime = 0x100000001b3ULL;

	for(uint32_t i = 0; i < byteCount; i++) {
		hash ^= bytes[i];
		hash *= prime;
	}
	return hash;
}

// ---------------------------------------------------------------------------
// SnesHdTileKey — Uniquely identifies a SNES tile for HD replacement lookup
// ---------------------------------------------------------------------------
// Content hash mode (new, for multi-gfxset packs):
//   Key = FNV-1a 64-bit hash of tile VRAM bytes + palette + layer
//   The hash uniquely identifies tile content regardless of VRAM address.
//   Different gfxsets that load different tiles to the same VRAM address
//   produce different ContentHash values -> unique keys.
//
// VramAddress mode (legacy, for single-gfxset packs):
//   Key = VRAM word address + palette + layer
//   Original approach; breaks when multiple gfxsets share VRAM addresses.

struct SnesHdTileKey
{
	uint64_t ContentHash = 0;   // FNV-1a 64-bit hash of tile VRAM bytes (0 = legacy VramAddress mode)
	uint16_t VramAddress = 0;   // VRAM word address (legacy fallback, used when ContentHash == 0)
	uint8_t PaletteIndex = 0;   // Palette group (0-7)
	uint8_t LayerIndex = 0;     // 0-3 = BG1-BG4, 4 = Sprites

	size_t GetHashCode() const
	{
		if(ContentHash != 0) {
			// Mix both halves of the 64-bit content hash with palette + layer
			uint32_t lo = (uint32_t)(ContentHash & 0xFFFFFFFF);
			uint32_t hi = (uint32_t)(ContentHash >> 32);
			return (size_t)(lo ^ hi) ^ ((size_t)LayerIndex << 4) ^ (size_t)PaletteIndex;
		}
		// Legacy address-based hash
		return ((size_t)VramAddress << 8) | ((size_t)LayerIndex << 4) | (size_t)PaletteIndex;
	}

	size_t operator()(const SnesHdTileKey& tile) const
	{
		return tile.GetHashCode();
	}

	bool operator==(const SnesHdTileKey& other) const
	{
		if(ContentHash != 0 || other.ContentHash != 0) {
			return ContentHash == other.ContentHash
				&& PaletteIndex == other.PaletteIndex
				&& LayerIndex == other.LayerIndex;
		}
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
	SnesHdPpuTileInfo BgTiles[4] = {};   // BG1-BG4 tile info at this pixel (indexed by layerIndex 0-3)
	uint8_t BgLayerMask = 0;             // Bitmask: bit N set = BgTiles[N] has a non-transparent tile
	uint8_t BgWinnerLayer = 0xFF;        // Which BG layer won compositing (0-3), or 0xFF = sprite/backdrop

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
	static constexpr int ScreenHeight = 239;   // Full PPU frame height (matches SnesPpu::SendFrame height)
	static constexpr int ScreenPixelCount = ScreenWidth * ScreenHeight;

	SnesHdPpuPixelInfo* ScreenTiles;
	uint16_t* Vram = nullptr;   // Pointer to PPU VRAM (word-addressed, 0x8000 entries); set by PPU at init
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

	uint32_t VramChecksum = 0;   // Sum of VRAM words for this tile's CHR region (0 = not verified)
	bool HasChecksum = false;    // If false, tile was exported without checksum data (always match)

	uint8_t GfxsetIndex = 0xFF;  // Gfxset this tile belongs to (0xFF = unscoped/legacy)

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
// Gfxset Fingerprint — identifies which graphics set is currently in VRAM
// ---------------------------------------------------------------------------
// Each gfxset has a set of "reference tiles" whose content hashes uniquely
// identify that gfxset. At runtime, Mesen checks these reference tiles
// against live VRAM once per frame. If all reference tiles match, that
// gfxset is considered active. Only tiles from the active gfxset are used
// for HD replacement — preventing cross-gfxset false positives (e.g.
// worldmap tiles incorrectly matching level tiles with same byte content).

struct GfxsetFingerprintEntry
{
	uint16_t VramWordAddr;   // Where to check in VRAM
	uint64_t ExpectedHash;   // FNV-1a 64-bit hash the tile should have
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
	bool UseContentHash = false; // true = content hash mode (hashes.bin), false = VramAddress mode (legacy)

	// All loaded PNG files
	vector<unique_ptr<SnesHdBitmapInfo>> ImageFileData;

	// All tile replacements
	vector<unique_ptr<SnesHdPackTileInfo>> Tiles;

	// Fast lookup: key → HD tile info
	// Multiple entries per key are possible (for conditional replacements in future)
	unordered_map<SnesHdTileKey, vector<SnesHdPackTileInfo*>> TileByKey;

	// Gfxset fingerprints for active-gfxset detection.
	// Key: gfxsetIndex, Value: reference tiles whose content identifies that gfxset.
	// Populated from fingerprints.bin. Empty = no scoping (all tiles match any context).
	unordered_map<uint8_t, vector<GfxsetFingerprintEntry>> GfxsetFingerprints;

	// Currently detected active gfxset.
	// -1 = no gfxset detected (no fingerprints loaded, or no match found)
	int16_t ActiveGfxset = -1;

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

	bool HasFingerprints() const { return !GfxsetFingerprints.empty(); }

	// Detect which gfxset is currently loaded in VRAM by checking reference
	// tile hashes. Called once per frame by the video filter.
	// Sets ActiveGfxset to the matching gfxset index, or -1 if none matches.
	//
	// NOTE: Uses default wordCount=16 (4bpp = 32 bytes per tile).
	// Fingerprint reference tiles MUST be 4bpp (BG1/BG2 in Mode 1).
	// The viewer's fingerprint selection only picks layer 0/1 tiles for this reason.
	void DetectActiveGfxset(const uint16_t* vram)
	{
		if(!HasFingerprints() || vram == nullptr) {
			ActiveGfxset = -1;
			return;
		}

		for(const auto& [gfxsetIdx, entries] : GfxsetFingerprints) {
			bool allMatch = true;
			for(const auto& entry : entries) {
				uint64_t liveHash = ComputeTileContentHash(vram, entry.VramWordAddr);
				if(liveHash != entry.ExpectedHash) {
					allMatch = false;
					break;
				}
			}
			if(allMatch) {
				ActiveGfxset = (int16_t)gfxsetIdx;
				return;
			}
		}
		ActiveGfxset = -1;  // No gfxset matched — worldmap or unknown context
	}

	// Look up an HD tile replacement.
	// Content hash mode: direct lookup — the key's ContentHash uniquely identifies tile content.
	//   With fingerprints: only returns tiles from the active gfxset (prevents false positives).
	//   Without fingerprints: returns any matching tile (original behavior).
	// Legacy mode: when vram is provided, selects among multiple candidates by checksum.
	SnesHdPackTileInfo* GetMatchingTile(const SnesHdTileKey& key, const uint16_t* vram = nullptr)
	{
		auto it = TileByKey.find(key);
		if(it == TileByKey.end()) return nullptr;

		if(UseContentHash) {
			for(SnesHdPackTileInfo* tile : it->second) {
				if(tile->IsFullyTransparent) continue;

				// DISABLED (Phase 1 fix): Fingerprint-based gfxset scoping does not work
				// for DKC2. VBlank DMA overwrites VRAM regions 0x2000-0x21FF and
				// 0x6000-0x61FF every frame with shared tiles, so fingerprint reference
				// hashes (computed from pre-DMA VRAM) never match live VRAM → ActiveGfxset
				// is always -1 → ALL gfxset-scoped tiles get blocked.
				// Content-hash keys already provide correct tile identity (like NES CHR-RAM
				// mode). Cross-context issues (worldmap) are cosmetic and should be solved
				// via a condition system in a future milestone, not via fingerprints.
				//
				// if(HasFingerprints() && tile->GfxsetIndex != 0xFF) {
				// 	if(ActiveGfxset < 0 || tile->GfxsetIndex != (uint8_t)ActiveGfxset) {
				// 		continue;
				// 	}
				// }
				return tile;
			}
			return nullptr;
		}

		// Legacy VramAddress mode: use checksum disambiguation
		for(SnesHdPackTileInfo* tile : it->second) {
			if(tile->IsFullyTransparent) continue;
			if(!tile->HasChecksum || vram == nullptr) return tile;
			uint32_t sum = 0;
			for(int i = 0; i < 16; i++) sum += vram[key.VramAddress + i];
			if(sum == tile->VramChecksum) return tile;
		}
		return nullptr;
	}
};
