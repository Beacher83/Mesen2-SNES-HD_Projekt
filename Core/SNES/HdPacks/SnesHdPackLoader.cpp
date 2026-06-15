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
//   sprites/*.png          — Sprite tiles
//
// Filename format: "4000_P03.png" = VRAM address 0x4000, palette 3
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

	for(const string& filePath : files) {
		string filename = FolderUtilities::GetFilename(filePath, false);

		uint16_t vramAddr = 0;
		uint8_t paletteIndex = 0;

		if(!ParseTileFilename(filename, vramAddr, paletteIndex)) {
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
		if(_useContentHash && gfxsetIndex != 0xFF) {
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
	}

	if(loadedCount > 0 || !files.empty()) {
		MessageManager::Log("[SNES HD Pack] gfxset_" + std::to_string(gfxsetIndex) + " layer " + std::to_string(layerIndex)
			+ ": loaded " + std::to_string(loadedCount) + "/" + std::to_string(files.size()) + " tiles");
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
