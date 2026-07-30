#include "pch.h"
#include "SNES/HdPacks/SnesHdPackLoader.h"
#include "SNES/HdPacks/SnesHdData.h"
#include "Utilities/FolderUtilities.h"
#include "Utilities/PNGHelper.h"
#include "Shared/MessageManager.h"
#include "Utilities/HexUtilities.h"

// ============================================================================
// SnesHdPackLoader — Loads SNES HD texture packs
//
// Pack directory structure (in {HdPackFolder}/{romName}/):
//   manifest.json          — metadata (scale, version)
//   bg/bg1/*.png           — BG1 tiles: {vramAddr}_P{palette}.png
//   bg/bg2/*.png           — BG2 tiles
//   bg/bg3/*.png           — BG3 tiles
//   bg/bg4/*.png           — BG4 tiles
//   bg/bgN/gfxset_XX/*.png — per-gfxset tiles (XX is DECIMAL, e.g. gfxset_37 = 0x25)
//   bg/bgN/global/*.png    — tiles that match in ANY gfxset (the text font); stored
//                            with GfxsetIndex 0xFF, which GetMatchingTile lets past
//                            the strict scoping check
//   cmFg/gfxset_XX/*.png   — Color-math foreground overlay, loaded as BG1 (layer 0)
//   sprites/*.png          — Sprite tiles
//
// Filename formats (without extension):
//   "4000_P03"             — VRAM address 0x4000, palette 3 (BG, address-keyed)
//   "h{16 hex}_P{pal}"     — S6b BG CHR-animation frame, keyed by CONTENT HASH.
//                            An animation streams several different tiles through
//                            the same VRAM address, so the address cannot identify
//                            the art. Lives in the same bg/bgN/gfxset_XX/ folders.
//   "{16 hex}_P{pal}"      — S3 sprite tile, keyed by content hash (sprites/ only)
// Each PNG is 32x32 pixels (4x scale of 8x8 SNES tile)
// ============================================================================

bool SnesHdPackLoader::LoadHdSnesPack(const string& romName, SnesHdPackData& outData)
{
	SnesHdPackLoader loader;
	if(!loader.InitializeLoader(romName, &outData)) {
		return false;
	}
	return loader.LoadPack();
}

bool SnesHdPackLoader::InitializeLoader(const string& romName, SnesHdPackData* data)
{
	_data = data;

	// Look for HD pack in {HdPackFolder}/{romName}/
	string hdPackRoot = FolderUtilities::GetHdPackFolder();
	_hdPackFolder = FolderUtilities::CombinePath(hdPackRoot, romName);

	// Verify the pack directory exists
	vector<string> entries;
	try {
		entries = FolderUtilities::GetFolders(_hdPackFolder);
	} catch(...) {}

	// Also check for any files directly
	std::unordered_set<string> anyExt = { ".png", ".json" };
	vector<string> files;
	try {
		files = FolderUtilities::GetFilesInFolder(_hdPackFolder, anyExt, false);
	} catch(...) {}

	if(entries.empty() && files.empty()) {
		return false;
	}

	return true;
}

bool SnesHdPackLoader::LoadPack()
{
	MessageManager::Log("[SNES HD Pack] Loading pack from: " + _hdPackFolder);

	// Try to load manifest for scale info
	LoadManifest();
	// Try content hash mode first (hashes.bin), fall back to legacy checksum mode (checksums.bin)
	_useContentHash = LoadHashes();
	if(!_useContentHash) {
		LoadChecksums();
	}
	_data->UseContentHash = _useContentHash;

	// Load gfxset fingerprints for active-gfxset detection (optional)
	LoadFingerprints();

	// R3: load reference BG palettes for the CGRAM-diff transform (optional)
	LoadPalettes();

	bool anyLoaded = false;

	// Load BG tiles for each layer (bg1 through bg4).
	// New format: bg/bg1/gfxset_07/*.png  (one subdir per gfxset)
	// Legacy format: bg/bg1/*.png          (flat, no gfxset)
	for(uint8_t layer = 0; layer < 4; layer++) {
		string layerDir = FolderUtilities::CombinePath(
			FolderUtilities::CombinePath(_hdPackFolder, "bg"),
			"bg" + std::to_string(layer + 1)
		);

		// Scan for gfxset_XX subdirectories
		bool foundGfxsetDirs = false;
		vector<string> subdirs;
		try { subdirs = FolderUtilities::GetFolders(layerDir); } catch(...) {}

		for(const string& subdir : subdirs) {
			// Extract last path component to get dir name
			size_t sep = subdir.find_last_of("/\\");
			string dirName = (sep != string::npos) ? subdir.substr(sep + 1) : subdir;

			uint8_t gfxsetIdx = 0;
			if(ParseGfxsetDirName(dirName, gfxsetIdx)) {
				if(LoadTilesFromDirectory(subdir, layer, false, gfxsetIdx)) {
					anyLoaded = true;
				}
				foundGfxsetDirs = true;
			} else if(dirName == "global") {
				// Art that belongs to no single gfxset: the text font is the same
				// ~90 glyphs in every shop, on the world map and in the menus, so
				// scoping it to one set would leave it native everywhere else.
				// 0xFF is the "unscoped" marker GetMatchingTile already honours —
				// the same exemption sprites use.
				if(LoadTilesFromDirectory(subdir, layer, false, 0xFF)) {
					anyLoaded = true;
				}
				foundGfxsetDirs = true;
			}
		}

		// Fall back to flat layout if no gfxset subdirs found
		if(!foundGfxsetDirs) {
			if(LoadTilesFromDirectory(layerDir, layer, false)) {
				anyLoaded = true;
			}
		}
	}

	// Also check for tiles directly in bg/ (layer-agnostic, assigned to all layers)
	string bgDir = FolderUtilities::CombinePath(_hdPackFolder, "bg");
	// We'll skip this for now — per-layer is cleaner

	// S12: color-math foreground overlay — cmFg/gfxset_XX/*.png
	//
	// Levels like Rambi Rumble draw a foreground overlay (the honey) from a SECOND
	// BG1 chrBase and blend it over the scene. The viewer exports those tiles into
	// their own folder rather than bg/bg1/ so the overlay can be removed for testing
	// without touching the terrain art — until now nothing ever read it back.
	//
	// They are ordinary address-keyed BG1 tiles: same "{addr}_P{pal}.png" naming,
	// and the exporter already writes their content hashes into hashes.bin under
	// layer 0, deduplicated against the terrain entries. So loading them as layer 0
	// needs no new key type and no renderer change — they simply become matchable.
	{
		string cmFgDir = FolderUtilities::CombinePath(_hdPackFolder, "cmFg");
		vector<string> cmFgDirs;
		try { cmFgDirs = FolderUtilities::GetFolders(cmFgDir); } catch(...) {}
		for(const string& subdir : cmFgDirs) {
			size_t sep = subdir.find_last_of("/\\");
			string dirName = (sep != string::npos) ? subdir.substr(sep + 1) : subdir;

			uint8_t gfxsetIdx = 0;
			if(ParseGfxsetDirName(dirName, gfxsetIdx)) {
				if(LoadTilesFromDirectory(subdir, 0, false, gfxsetIdx)) {
					anyLoaded = true;
				}
			}
		}
	}

	// Load sprite tiles
	string spriteDir = FolderUtilities::CombinePath(_hdPackFolder, "sprites");
	if(LoadTilesFromDirectory(spriteDir, 4, true)) {
		anyLoaded = true;
	}

	if(anyLoaded) {
		MessageManager::Log("[SNES HD Pack] Loaded " + std::to_string(_data->Tiles.size()) + " HD tiles (scale: " + std::to_string(_data->Scale) + "x)");
	} else {
		MessageManager::Log("[SNES HD Pack] No HD tiles found in: " + _hdPackFolder);
	}

	return anyLoaded;
}

bool SnesHdPackLoader::LoadManifest()
{
	// For now, just set defaults. Full JSON parsing can be added later
	// using a lightweight JSON parser (Mesen doesn't have one built-in).
	_data->Scale = 4;
	_data->Version = 1;

	// TODO: Parse manifest.json for scale, version, game info
	// For the initial implementation, we hardcode scale=4

	return true;
}

bool SnesHdPackLoader::LoadHashes()
{
	// hashes.bin format (all little-endian):
	//   uint32_t count
	//   count x { uint16_t vramAddr, uint8_t layerIndex, uint8_t gfxsetIndex, uint64_t contentHash }
	//   Entry size: 12 bytes per entry
	string hashPath = FolderUtilities::CombinePath(_hdPackFolder, "hashes.bin");

	ifstream file(hashPath, std::ios::binary);
	if(!file) {
		return false;
	}

	uint32_t count = 0;
	file.read((char*)&count, 4);
	if(file.fail() || count == 0 || count > 65536) {
		return false;
	}

	for(uint32_t i = 0; i < count; i++) {
		uint16_t vramAddr = 0;
		uint8_t layerIdx = 0;
		uint8_t gfxsetIdx = 0;
		uint64_t contentHash = 0;

		file.read((char*)&vramAddr, 2);
		file.read((char*)&layerIdx, 1);
		file.read((char*)&gfxsetIdx, 1);
		file.read((char*)&contentHash, 8);

		if(file.fail()) break;

		uint32_t key = ((uint32_t)gfxsetIdx << 24) | ((uint32_t)layerIdx << 16) | vramAddr;
		_hashMap[key] = contentHash;
	}

	if(!_hashMap.empty()) {
		// Log per-gfxset breakdown
		std::unordered_map<uint8_t, int> gfxsetCounts;
		for(auto& kv : _hashMap) {
			uint8_t gs = (kv.first >> 24) & 0xFF;
			gfxsetCounts[gs]++;
		}
		string detail;
		for(auto& gc : gfxsetCounts) {
			if(!detail.empty()) detail += ", ";
			detail += "gfxset_" + std::to_string(gc.first) + "=" + std::to_string(gc.second);
		}
		MessageManager::Log("[SNES HD Pack] Loaded " + std::to_string(_hashMap.size()) + " content hashes from hashes.bin (" + detail + ")");

		// DIAGNOSTIC: Dump first 5 hash values per gfxset for comparison with runtime misses
		std::unordered_map<uint8_t, int> sampleCounts;
		for(auto& kv : _hashMap) {
			uint8_t gs = (kv.first >> 24) & 0xFF;
			if(sampleCounts[gs]++ < 5) {
				uint16_t va = kv.first & 0xFFFF;
				uint8_t ly = (kv.first >> 16) & 0xFF;
				char buf[128];
				snprintf(buf, sizeof(buf), "[SNES HD diag] LOADED gfxset=%d vram=0x%04X layer=%d hash=%016llX",
					gs, va, ly, (unsigned long long)kv.second);
				MessageManager::Log(buf);
			}
		}
	}
	return !_hashMap.empty();
}

bool SnesHdPackLoader::LoadChecksums()
{
	// checksums.bin format (all little-endian):
	//   uint32_t count
	//   count × { uint16_t vramAddr, uint8_t layerIndex, uint8_t reserved, uint32_t checksum }
	string checksumPath = FolderUtilities::CombinePath(_hdPackFolder, "checksums.bin");

	ifstream file(checksumPath, std::ios::binary);
	if(!file) {
		return false;  // No checksum file — pack was exported without checksums, all tiles match
	}

	uint32_t count = 0;
	file.read((char*)&count, 4);
	if(file.fail() || count == 0 || count > 65536) {
		return false;
	}

	for(uint32_t i = 0; i < count; i++) {
		uint16_t vramAddr = 0;
		uint8_t layerIdx = 0;
		uint8_t gfxsetIdx = 0;
		uint32_t checksum = 0;

		file.read((char*)&vramAddr, 2);
		file.read((char*)&layerIdx, 1);
		file.read((char*)&gfxsetIdx, 1);
		file.read((char*)&checksum, 4);

		if(file.fail()) break;

		// Key = (gfxset << 24) | (layer << 16) | vramAddr
		uint32_t key = ((uint32_t)gfxsetIdx << 24) | ((uint32_t)layerIdx << 16) | vramAddr;
		_checksumMap[key] = checksum;
	}

	if(!_checksumMap.empty()) {
		MessageManager::Log("[SNES HD Pack] Loaded " + std::to_string(_checksumMap.size()) + " VRAM checksums for content verification");
	}
	return !_checksumMap.empty();
}

bool SnesHdPackLoader::ParseGfxsetDirName(const string& dirName, uint8_t& gfxsetIndex)
{
	// Expects "gfxset_07", "gfxset_8", etc.
	if(dirName.size() < 8 || dirName.substr(0, 7) != "gfxset_") return false;
	try {
		unsigned long idx = std::stoul(dirName.substr(7), nullptr, 10);
		if(idx > 254) return false;  // 0xFF reserved for legacy (no gfxset)
		gfxsetIndex = (uint8_t)idx;
		return true;
	} catch(...) {
		return false;
	}
}

// S6b: BG CHR-animation frames are hash-keyed in the filename, like sprites:
// "h{16 hex FNV}_P{decPalette}.png" (e.g. "h55138607A3B1C916_P02").
//
// A CHR animation streams several DIFFERENT tiles through the SAME VRAM address
// (Hot Head's lava, Gusty's foliage, Mainbrace's flag), so the address-based
// naming ParseTileFilename() expects cannot identify the art — only the content
// can. The leading 'h' marks that, and is unambiguous: a plain tile name starts
// with a hex VRAM address, and 'h' is not a hex digit.
//
// File-local on purpose so SnesHdPackLoader.h stays untouched (a header change
// would force a much wider rebuild for what is a filename-parsing detail).
static bool ParseAnimTileFilename(const string& filename, uint64_t& contentHash, uint8_t& paletteIndex)
{
	if(filename.size() < 18 || (filename[0] != 'h' && filename[0] != 'H')) {
		return false;
	}
	if(filename[17] != '_') {
		return false;
	}

	string hashStr = filename.substr(1, 16);
	for(char c : hashStr) {
		bool isHex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
		if(!isHex) return false;
	}
	try {
		contentHash = std::stoull(hashStr, nullptr, 16);
	} catch(...) {
		return false;
	}
	if(contentHash == 0) {
		return false;  // 0 is the "no hash" sentinel in SnesHdTileKey
	}

	string palStr = filename.substr(18);
	if(palStr.empty() || (palStr[0] != 'P' && palStr[0] != 'p')) {
		return false;
	}
	try {
		unsigned long pal = std::stoul(palStr.substr(1), nullptr, 10);
		if(pal > 7) return false;
		paletteIndex = (uint8_t)pal;
	} catch(...) {
		return false;
	}
	return true;
}

bool SnesHdPackLoader::LoadTilesFromDirectory(const string& dirPath, uint8_t layerIndex, bool isSprite, uint8_t gfxsetIndex)
{
	std::unordered_set<string> extensions = { ".png" };
	vector<string> files;

	try {
		files = FolderUtilities::GetFilesInFolder(dirPath, extensions, false);
	} catch(...) {
		// Directory doesn't exist or can't be read
		return false;
	}

	if(files.empty()) {
		return false;
	}

	int loadedCount = 0;
	int animCount = 0;

	for(const string& filePath : files) {
		string filename = FolderUtilities::GetFilename(filePath, false);

		uint16_t vramAddr = 0;
		uint8_t paletteIndex = 0;
		uint64_t spriteHash = 0;
		uint64_t animHash = 0;
		bool isAnimTile = false;

		// S3: sprite tiles are hash-keyed directly in the filename
		// ("{16-hex-FNV}_P{pal}.png") — OBJ tiles are streamed to varying
		// VRAM addresses at runtime, so address-based naming cannot work.
		if(isSprite) {
			if(!ParseSpriteFilename(filename, spriteHash, paletteIndex)) {
				MessageManager::Log("[SNES HD Pack] Skipping invalid sprite filename: " + filename);
				continue;
			}
		} else if(ParseAnimTileFilename(filename, animHash, paletteIndex)) {
			// S6b: CHR-animation frame, keyed by content hash (see ParseAnimTileFilename).
			// Tried before ParseTileFilename so the 'h' prefix wins; a normal tile name
			// can never start with 'h', so this cannot shadow one.
			if(!_useContentHash) {
				MessageManager::Log("[SNES HD Pack] Anim tile needs content-hash mode (hashes.bin missing?) - skipping: " + filename);
				continue;
			}
			isAnimTile = true;
		} else if(!ParseTileFilename(filename, vramAddr, paletteIndex)) {
			MessageManager::Log("[SNES HD Pack] Skipping invalid filename: " + filename);
			continue;
		}

		// Create bitmap info and load the PNG
		auto bitmap = std::make_unique<SnesHdBitmapInfo>();
		bitmap->PngName = filePath;

		if(!LoadPngFile(filePath, *bitmap)) {
			MessageManager::Log("[SNES HD Pack] Failed to load PNG: " + filePath);
			continue;
		}

		// Validate dimensions match expected scale
		uint32_t expectedSize = 8 * _data->Scale;
		if(bitmap->Width != expectedSize || bitmap->Height != expectedSize) {
			MessageManager::Log("[SNES HD Pack] Wrong tile size " + std::to_string(bitmap->Width) + "x" + std::to_string(bitmap->Height)
				+ " (expected " + std::to_string(expectedSize) + "x" + std::to_string(expectedSize) + "): " + filename);
			continue;
		}

		// Create tile info
		auto tile = std::make_unique<SnesHdPackTileInfo>();
		tile->Key.PaletteIndex = paletteIndex;
		tile->Key.LayerIndex = layerIndex;

		// Assign tile identity based on lookup mode
		if(isSprite) {
			// S3: hash from the filename. Requires content-hash mode at runtime
			// (the sprite key carries ContentHash; packs always ship hashes.bin).
			tile->Key.ContentHash = spriteHash;
		} else if(isAnimTile) {
			// S6b: hash from the filename, no hashes.bin lookup — the animation's
			// frames are not in hashes.bin at all (that maps vramAddr -> the ONE tile
			// present in the snapshot). GfxsetIndex below still scopes them.
			tile->Key.ContentHash = animHash;
		} else if(_useContentHash && gfxsetIndex != 0xFF) {
			// Content hash mode: look up hash from hashes.bin
			uint32_t hashKey = ((uint32_t)gfxsetIndex << 24) | ((uint32_t)layerIndex << 16) | vramAddr;
			auto hashIt = _hashMap.find(hashKey);
			if(hashIt != _hashMap.end()) {
				tile->Key.ContentHash = hashIt->second;
			} else {
				MessageManager::Log("[SNES HD Pack] No content hash for tile " + filename + " in gfxset " + std::to_string(gfxsetIndex) + " - skipping");
				continue;
			}
		} else {
			// Legacy VramAddress mode
			tile->Key.VramAddress = vramAddr;
		}

		tile->X = 0;
		tile->Y = 0;
		tile->Width = bitmap->Width;
		tile->Height = bitmap->Height;
		tile->BitmapIndex = (uint32_t)_data->ImageFileData.size();
		tile->Brightness = 255;
		tile->GfxsetIndex = gfxsetIndex;  // Track which gfxset this tile belongs to (0xFF = unscoped)

		// Copy pixel data directly into tile (since each PNG = one tile)
		tile->HdTileData = bitmap->PixelData;
		tile->UpdateFlags();

		// Legacy: Assign VRAM checksum if available (only for legacy mode with gfxset tiles)
		if(!_useContentHash && gfxsetIndex != 0xFF) {
			uint32_t csKey = ((uint32_t)gfxsetIndex << 24) | ((uint32_t)layerIndex << 16) | vramAddr;
			auto csIt = _checksumMap.find(csKey);
			if(csIt != _checksumMap.end()) {
				tile->VramChecksum = csIt->second;
				tile->HasChecksum = true;
			}
		}

		// Register in lookup map
		_data->TileByKey[tile->Key].push_back(tile.get());

		_data->Tiles.push_back(std::move(tile));
		_data->ImageFileData.push_back(std::move(bitmap));
		loadedCount++;
		if(isAnimTile) animCount++;
	}

	if(loadedCount > 0 || !files.empty()) {
		MessageManager::Log("[SNES HD Pack] gfxset_" + std::to_string(gfxsetIndex) + " layer " + std::to_string(layerIndex)
			+ ": loaded " + std::to_string(loadedCount) + "/" + std::to_string(files.size()) + " tiles"
			+ (animCount > 0 ? " (" + std::to_string(animCount) + " anim frames)" : string()));
	}

	return loadedCount > 0;
}

bool SnesHdPackLoader::LoadPngFile(const string& filePath, SnesHdBitmapInfo& bitmap)
{
	// Read the file into memory, then use the template ReadPNG<uint32_t>
	// which decodes PNG and converts ABGR→ARGB automatically.
	ifstream pngFile(filePath, std::ios::in | std::ios::binary);
	if(!pngFile) {
		return false;
	}

	pngFile.seekg(0, std::ios::end);
	size_t fileSize = (size_t)pngFile.tellg();
	pngFile.seekg(0, std::ios::beg);

	vector<uint8_t> fileData(fileSize, 0);
	pngFile.read((char*)fileData.data(), fileData.size());

	if(PNGHelper::ReadPNG(fileData, bitmap.PixelData, bitmap.Width, bitmap.Height)) {
		bitmap.PremultiplyAlpha();
		return true;
	}
	return false;
}

bool SnesHdPackLoader::ParseSpriteFilename(const string& filename, uint64_t& contentHash, uint8_t& paletteIndex)
{
	// S3 sprite format: "{16 hex chars}_P{decPalette}" (without extension)
	// Example: "9A4ED6AC608A1BCD_P5" → contentHash=0x9A4ED6AC608A1BCD, palette=5
	size_t underscorePos = filename.find('_');
	if(underscorePos != 16) {
		return false;
	}

	string hashStr = filename.substr(0, 16);
	try {
		contentHash = std::stoull(hashStr, nullptr, 16);
	} catch(...) {
		return false;
	}
	if(contentHash == 0) {
		return false;  // 0 is the "no hash" sentinel in SnesHdTileKey
	}

	string palStr = filename.substr(underscorePos + 1);
	if(palStr.empty() || (palStr[0] != 'P' && palStr[0] != 'p')) {
		return false;
	}
	try {
		unsigned long pal = std::stoul(palStr.substr(1), nullptr, 10);
		if(pal > 7) {
			return false;
		}
		paletteIndex = (uint8_t)pal;
	} catch(...) {
		return false;
	}
	return true;
}

bool SnesHdPackLoader::ParseTileFilename(const string& filename, uint16_t& vramAddr, uint8_t& paletteIndex)
{
	// Expected format: "4000_P03" (without extension)
	// Parts: {hexVramAddr}_P{decPalette}
	//
	// Example: "4000_P03" → vramAddr=0x4000, palette=3

	size_t underscorePos = filename.find('_');
	if(underscorePos == string::npos || underscorePos == 0) {
		return false;
	}

	// Parse hex VRAM address
	string addrStr = filename.substr(0, underscorePos);
	try {
		unsigned long addr = std::stoul(addrStr, nullptr, 16);
		if(addr > 0x7FFF) {
			return false;
		}
		vramAddr = (uint16_t)addr;
	} catch(...) {
		return false;
	}

	// Parse palette: expect "P" followed by decimal number
	string palStr = filename.substr(underscorePos + 1);
	if(palStr.empty() || (palStr[0] != 'P' && palStr[0] != 'p')) {
		return false;
	}

	try {
		unsigned long pal = std::stoul(palStr.substr(1), nullptr, 10);
		if(pal > 7) {
			return false;
		}
		paletteIndex = (uint8_t)pal;
	} catch(...) {
		return false;
	}

	return true;
}

bool SnesHdPackLoader::LoadFingerprints()
{
	// fingerprints.bin format (all little-endian):
	//   uint8_t gfxsetCount
	//   gfxsetCount × {
	//     uint8_t gfxsetIndex
	//     uint8_t refTileCount
	//     refTileCount × { uint16_t vramWordAddr (2 bytes), uint64_t expectedHash (8 bytes) }
	//   }
	string fpPath = FolderUtilities::CombinePath(_hdPackFolder, "fingerprints.bin");

	ifstream file(fpPath, std::ios::binary);
	if(!file) {
		return false;  // No fingerprints file — no gfxset scoping (all tiles match any context)
	}

	uint8_t gfxsetCount = 0;
	file.read((char*)&gfxsetCount, 1);
	if(file.fail() || gfxsetCount == 0 || gfxsetCount > 128) {
		return false;
	}

	for(uint8_t g = 0; g < gfxsetCount; g++) {
		uint8_t gfxsetIdx = 0;
		uint8_t refCount = 0;
		file.read((char*)&gfxsetIdx, 1);
		file.read((char*)&refCount, 1);
		if(file.fail() || refCount == 0) break;

		vector<GfxsetFingerprintEntry> entries;
		for(uint8_t r = 0; r < refCount; r++) {
			GfxsetFingerprintEntry entry;
			file.read((char*)&entry.VramWordAddr, 2);
			file.read((char*)&entry.ExpectedHash, 8);
			if(file.fail()) break;
			entries.push_back(entry);
		}

		if(!entries.empty()) {
			_data->GfxsetFingerprints[gfxsetIdx] = std::move(entries);
		}
	}

	if(!_data->GfxsetFingerprints.empty()) {
		uint32_t totalRefs = 0;
		for(const auto& [idx, entries] : _data->GfxsetFingerprints) {
			totalRefs += (uint32_t)entries.size();
		}
		MessageManager::Log("[SNES HD Pack] Loaded fingerprints for " + std::to_string(_data->GfxsetFingerprints.size())
			+ " gfxsets (" + std::to_string(totalRefs) + " reference tiles)");
	}
	return !_data->GfxsetFingerprints.empty();
}

bool SnesHdPackLoader::LoadPalettes()
{
	// palettes.bin format (all little-endian):
	//   uint8_t gfxsetCount
	//   gfxsetCount × {
	//     uint8_t gfxsetIndex
	//     128 × uint16_t bgr555   (8 BG palette rows × 16 colors, CGRAM 0-127
	//                              as captured at export time in the viewer)
	//   }
	string palPath = FolderUtilities::CombinePath(_hdPackFolder, "palettes.bin");

	ifstream file(palPath, std::ios::binary);
	if(!file) {
		return false;  // No palettes file — no CGRAM-diff transform (HD colors stay baked in)
	}

	uint8_t gfxsetCount = 0;
	file.read((char*)&gfxsetCount, 1);
	if(file.fail() || gfxsetCount == 0 || gfxsetCount > 128) {
		return false;
	}

	for(uint8_t g = 0; g < gfxsetCount; g++) {
		uint8_t gfxsetIdx = 0;
		file.read((char*)&gfxsetIdx, 1);
		if(file.fail()) break;

		vector<uint16_t> palette(128);
		file.read((char*)palette.data(), 128 * sizeof(uint16_t));
		if(file.fail()) break;

		_data->GfxsetPalettes[gfxsetIdx] = std::move(palette);
	}

	if(!_data->GfxsetPalettes.empty()) {
		MessageManager::Log("[SNES HD Pack] Loaded reference palettes for "
			+ std::to_string(_data->GfxsetPalettes.size()) + " gfxsets");
	}
	return !_data->GfxsetPalettes.empty();
}

// Implement SnesHdBitmapInfo::Init (declared in SnesHdData.h)
void SnesHdBitmapInfo::Init()
{
	if(!FileData.empty() && PixelData.empty()) {
		if(PNGHelper::ReadPNG(FileData, PixelData, Width, Height)) {
			PremultiplyAlpha();
		} else {
			MessageManager::Log("[SNES HD Pack] Failed to decode PNG: " + PngName);
		}
		FileData.clear();
	}
}
