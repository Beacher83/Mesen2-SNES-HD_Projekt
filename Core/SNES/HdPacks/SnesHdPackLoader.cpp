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

	// Check if the pack directory exists by looking for any PNG files
	string manifestPath = FolderUtilities::CombinePath(_hdPackFolder, "manifest.json");

	// Simple existence check: try to find bg/ or sprites/ subdirectories
	// We'll check for PNGs in LoadPack
	return true;
}

bool SnesHdPackLoader::LoadPack()
{
	MessageManager::Log("[SNES HD Pack] Loading pack from: " + _hdPackFolder);

	// Try to load manifest for scale info
	LoadManifest();

	bool anyLoaded = false;

	// Load BG tiles for each layer (bg1 through bg4)
	for(uint8_t layer = 0; layer < 4; layer++) {
		string layerDir = FolderUtilities::CombinePath(
			FolderUtilities::CombinePath(_hdPackFolder, "bg"),
			"bg" + std::to_string(layer + 1)
		);
		if(LoadTilesFromDirectory(layerDir, layer, false)) {
			anyLoaded = true;
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

bool SnesHdPackLoader::LoadTilesFromDirectory(const string& dirPath, uint8_t layerIndex, bool isSprite)
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

	for(const string& filePath : files) {
		string filename = FolderUtilities::GetFilename(filePath, false);

		uint16_t vramAddr = 0;
		uint8_t paletteIndex = 0;

		if(!ParseTileFilename(filename, vramAddr, paletteIndex)) {
			MessageManager::Log("[SNES HD Pack] Skipping invalid filename: " + filename);
			continue;
		}

		// Create bitmap info and load the PNG
		auto bitmap = make_unique<SnesHdBitmapInfo>();
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
		auto tile = make_unique<SnesHdPackTileInfo>();
		tile->Key.VramAddress = vramAddr;
		tile->Key.PaletteIndex = paletteIndex;
		tile->Key.LayerIndex = layerIndex;
		tile->Key.IsSprite = isSprite;
		tile->X = 0;
		tile->Y = 0;
		tile->Width = bitmap->Width;
		tile->Height = bitmap->Height;
		tile->BitmapIndex = (uint32_t)_data->ImageFileData.size();
		tile->Brightness = 255;

		// Copy pixel data directly into tile (since each PNG = one tile)
		tile->HdTileData = bitmap->PixelData;
		tile->UpdateFlags();

		// Register in lookup map
		_data->TileByKey[tile->Key].push_back(tile.get());

		_data->Tiles.push_back(std::move(tile));
		_data->ImageFileData.push_back(std::move(bitmap));
	}

	return !files.empty();
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
