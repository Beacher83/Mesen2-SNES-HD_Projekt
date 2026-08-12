#pragma once
#include "pch.h"
#include "Utilities/SimpleLock.h"
#include "SNES/SnesPpuTypes.h"

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

// S20: PaletteIndex value that means "this sprite tile fits any OBJ palette row".
//
// For sprites the PaletteIndex is the OAM palette SLOT, and a slot carries no
// identity: DKC2's allocator (CODE_BB8A6F) hands out whichever of the eight
// slots is free, so the same character lands in different rows from one level
// to the next — and the pack had to ship the same art once per slot it had ever
// been recorded in. A tile that carries a reference palette does not need that:
// it is recoloured from its reference to whatever row is live, so one copy is
// correct everywhere. Those tiles are stored under this wildcard, and
// GetMatchingTile falls back to it when the exact slot has no art.
//
// Tiles WITHOUT a reference keep their recorded slot: they render with their
// baked-in colours, so letting them match a foreign row would be a new way to
// paint wrong colours.
static constexpr uint8_t SnesHdSpriteAnyPalette = 0xFF;

struct SnesHdTileKey
{
	uint64_t ContentHash = 0;   // FNV-1a 64-bit hash of tile VRAM bytes (0 = legacy VramAddress mode)
	uint16_t VramAddress = 0;   // VRAM word address (legacy fallback, used when ContentHash == 0)
	uint8_t PaletteIndex = 0;   // Palette group (0-7), or SnesHdSpriteAnyPalette for slot-free sprite art
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
	uint16_t VramWordAddr = 0;  // VRAM word address used for content hash (diagnostic: detect DMA range)
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
	bool SubScreenHasSprite = false;     // Sprite wins sub-screen at this pixel (set by RenderSprites)

	// P4.0: sub-screen winner identity — lets the HD filter sample the color
	// math operand from an HD tile instead of the flat native SubScreenColor.
	// Encoded +1 so the memset(0) buffer clear means "no BG won the sub screen".
	uint8_t SubScreenWinnerPlus1 = 0;    // 0 = none/backdrop, 1-4 = BG1-BG4 won the sub screen
	// P4.0: true when _subScreenPriority[x] == 0 after rendering — the PPU's
	// color math then uses FixedColor instead of the sub-screen color AND
	// disables the halve operation (SnesPpu::ApplyColorMathToPixel special case).
	bool SubScreenEmpty = false;

	// S1: OBJ tile identity of the composited sprite pixel, captured in
	// RenderSprites from the fetch-time line buffer. Sprites[0] = tile that
	// won the MAIN screen, Sprites[1] = tile that won the SUB screen.
	// OffsetX/OffsetY are SCREEN-SPACE tile coordinates with mirror flags set
	// — the same convention as BgTiles, so HdTileSampler works unchanged.
	// LayerIndex = 4. NOTE: sprites render before the tilemaps —
	// check the final winner (IsSpritePixel / SubScreenHasSprite) before use.
	SnesHdPpuTileInfo Sprites[4] = {};
	uint8_t SpriteCount = 0;             // Validity bitmask: bit0 = Sprites[0], bit1 = Sprites[1]

	uint16_t MainScreenColor = 0;        // P4.0: pre-color-math, pre-brightness main screen color (BGR555)
	uint16_t SubScreenColor = 0;
	uint8_t MainScreenFlags = 0;
	uint8_t MainScreenPriority = 0;
};

// ---------------------------------------------------------------------------
// SnesHdScanlineInfo — Per-scanline PPU register snapshot for HD compositing
// ---------------------------------------------------------------------------
// Captured once per scanline from SnesPpuState during RenderScanline().
// Contains all register state needed to reproduce SNES post-tile compositing
// (color math, windowing, brightness) at HD resolution.
//
// Why per-scanline: HDMA updates registers between scanlines (at HBlank).
// DKC2 uses HDMA to animate FixedColor (fog gradient), toggle color math
// layers, and move window boundaries — all between scanlines, never mid-line.
//
// Used by Phase 2-4 of the HD compositing engine to replace the old
// per-level-type special cases (bg3FogBlend, bg1OverlayBlend, etc.)

struct SnesHdScanlineInfo
{
	// --- BG Mode ($2105) ---
	uint8_t BgMode = 0;                    // $2105 bits 0-2: BG mode (0-7)
	bool Mode1Bg3Priority = false;         // $2105 bit 3: BG3 priority=1 above all (Mode 1 only)

	// --- Color Math ($2130, $2131) ---
	uint8_t ColorMathEnabled = 0;          // $2131 bits 0-5: bitmask of layers that participate
	                                       //   bit 0 = BG1, 1 = BG2, 2 = BG3, 3 = BG4,
	                                       //   4 = OBJ (sprites with palette 4-7), 5 = Backdrop
	bool ColorMathSubtractMode = false;    // $2131 bit 7: true = subtract, false = add
	bool ColorMathHalveResult = false;     // $2131 bit 6: halve the add/sub result
	bool ColorMathAddSubscreen = false;    // $2130 bit 1: true = blend with sub-screen pixel,
	                                       //              false = blend with FixedColor
	ColorWindowMode ColorMathClipMode = ColorWindowMode::Never;    // $2130 bits 6-7: clip color to black
	ColorWindowMode ColorMathPreventMode = ColorWindowMode::Never; // $2130 bits 4-5: prevent color math

	// --- Fixed Color ($2132) ---
	uint16_t FixedColor = 0;               // BGR555, HDMA-animated per scanline (fog/lava/ice gradient)

	// --- Brightness ($2100) ---
	uint8_t ScreenBrightness = 0;          // 0-15 (0 = black, 15 = full)

	// --- Layer Designation ($212C, $212D) ---
	uint8_t MainScreenLayers = 0;          // $212C: bitmask of layers on main screen
	uint8_t SubScreenLayers = 0;           // $212D: bitmask of layers on sub screen

	// --- Window Positions ($2126-$2129) ---
	// Needed to re-evaluate color window boundaries at HD resolution.
	// At 4x scale, SNES window boundary at x=100 → HD boundary at x=400.
	uint8_t Window1Left = 0;               // $2126
	uint8_t Window1Right = 0;              // $2127
	uint8_t Window2Left = 0;               // $2128
	uint8_t Window2Right = 0;              // $2129

	// --- Color Window Configuration ---
	// The SNES has 2 windows. For the color math layer (index 5 in PPU terms),
	// we need to know: is each window active? is it inverted? how are they combined?
	bool ColorWindowActive[2] = {};        // Window 1/2 active for color math (layer 5)
	bool ColorWindowInverted[2] = {};      // Window 1/2 inverted for color math (layer 5)
	WindowMaskLogic ColorWindowMaskLogic = WindowMaskLogic::Or;  // How to combine W1+W2 for color
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
	SnesHdScanlineInfo ScanlineInfo[ScreenHeight];  // Per-scanline PPU register snapshot (filled by PPU)
	uint16_t Cgram[256] = {};   // P4.0: CGRAM snapshot at SendFrame — basis for palette-shift transforms (R3)
	uint16_t* Vram = nullptr;   // Pointer to PPU VRAM (word-addressed, 0x8000 entries); set by PPU at init
	uint32_t FrameNumber = 0;
	uint16_t Scanline = 0;

	// S17: OAM, so a sprite OBJECT can be recorded as the game defined it.
	// The screen capture in ScreenTiles knows which tile produced a pixel but
	// not what belongs together — that has to be guessed from adjacency, and the
	// guess is what made whole objects come and go depending on grouping
	// settings. OAM carries the answer: one entry IS one object (up to 64x64),
	// with its own position, size, palette and mirror flags.
	uint8_t* Oam = nullptr;     // 544 bytes: 128 x 4-byte entries + 32-byte high table
	uint16_t OamBaseAddress = 0;    // OBSEL name base (word address)
	uint16_t OamAddressOffset = 0;  // OBSEL name select — offset of the second tile table
	uint8_t OamMode = 0;            // OBSEL size mode 0-7 (which two sprite sizes are active)

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

	// R3: reference BG palettes per gfxset (palettes.bin) — the CGRAM state the
	// HD tiles were exported under. 128 entries = 8 BG palette rows × 16 colors
	// (BGR555, CGRAM 0-127). At runtime the filter diffs live CGRAM against this
	// reference to derive a per-palette-row color transform, making the HD tiles
	// follow CGRAM effects (underwater darkening, sunset HDMA, palette cycling).
	// Empty = no transform (HD tiles keep their baked-in colors).
	unordered_map<uint8_t, vector<uint16_t>> GfxsetPalettes;

	// Reference OBJ palettes per sprite tile (sprite_palettes.bin) — the 16 colors
	// the HD art was baked under, keyed by the tile's content hash.
	//
	// The BG mechanism above cannot serve sprites. DKC2 allocates OBJ palettes
	// dynamically: a sprite's init script names a palette (command $8D, table
	// DATA_FD5FEE) and the allocator (bank BB, CODE_BB8A6F) hands it one of 8
	// refcounted CGRAM slots — whichever happens to be free. That slot number is
	// what reaches us in the OAM attribute bits, and it carries no color identity:
	// the same crate sits in different slots in different levels, and the LEVEL
	// decides which palette it gets. So the reference cannot be derived from
	// anything we see at runtime and has to ship with the art.
	//
	// With it the filter recolors reference -> live CGRAM, which is how one piece
	// of art per sprite ends up correctly tinted everywhere. Empty = no recoloring
	// (HD sprites keep their baked-in colors).
	vector<uint16_t> SpritePaletteData;                       // paletteCount x 16, BGR555
	unordered_map<uint64_t, uint32_t> SpritePaletteByHash;    // contentHash -> palette index

	// nullptr when the tile has no reference (runtime-captured tiles, older packs).
	const uint16_t* GetSpriteRefPalette(uint64_t contentHash) const
	{
		auto it = SpritePaletteByHash.find(contentHash);
		if(it == SpritePaletteByHash.end()) {
			return nullptr;
		}
		return SpritePaletteData.data() + (size_t)it->second * 16;
	}

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
	// Exact match on (hash + palette + layer). No palette fallback — cross-palette matching
	// causes worldmap contamination when Level 2 tile hashes coincide with worldmap tiles
	// at different palette indices. palFB=0 in diagnostics confirmed it never helps Level 2 anyway.
	SnesHdPackTileInfo* GetMatchingTile(const SnesHdTileKey& key, const uint16_t* vram = nullptr)
	{
		if(UseContentHash && key.ContentHash != 0) {
			// P4.2 strict gfxset scoping: when fingerprints are loaded, only tiles
			// of the currently detected gfxset may match; ActiveGfxset == -1
			// (worldmap, unknown screen, NPC shop) blocks HD lookups entirely.
			// Cross-gfxset hash+palette coincidences painted foreign tiles into
			// unrelated scenes (Gusty Glade's blue squares: foreign BG3 tiles
			// ADDed via the sub-operand path, sHd=3723 with ~0% real coverage).
			// Unscoped tiles (GfxsetIndex 0xFF) stay matchable so packs without
			// scoping info keep working. Known accepted loss: NPC shop (sig
			// F88DC3…, 32% match, gfx=-1) until a shop fingerprint set exists.
			// S3: sprites (LayerIndex 4) are exempt — characters exist across
			// all levels/worldmap; their hash-keyed art is inherently exact.
			const bool strictScope = HasFingerprints() && key.LayerIndex != 4;
			if(strictScope && ActiveGfxset < 0) {
				return nullptr;
			}
			auto it = TileByKey.find(key);
			if(it != TileByKey.end()) {
				for(SnesHdPackTileInfo* tile : it->second) {
					if(tile->IsFullyTransparent) continue;
					if(strictScope && tile->GfxsetIndex != 0xFF && (int16_t)tile->GfxsetIndex != ActiveGfxset) continue;
					return tile;
				}
			}

			// S20: sprite art that carries a reference palette is stored slot-free
			// (see SnesHdSpriteAnyPalette). The exact slot is tried first so a pack
			// that still ships per-slot art keeps behaving exactly as before.
			if(key.LayerIndex == 4 && key.PaletteIndex != SnesHdSpriteAnyPalette) {
				SnesHdTileKey anyKey = key;
				anyKey.PaletteIndex = SnesHdSpriteAnyPalette;
				auto anyIt = TileByKey.find(anyKey);
				if(anyIt != TileByKey.end()) {
					for(SnesHdPackTileInfo* tile : anyIt->second) {
						if(tile->IsFullyTransparent) continue;
						return tile;
					}
				}
			}
			return nullptr;
		}

		// Legacy VramAddress mode: exact match with optional checksum
		auto it = TileByKey.find(key);
		if(it == TileByKey.end()) return nullptr;

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
