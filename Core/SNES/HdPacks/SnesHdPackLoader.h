#pragma once
#include "pch.h"
#include "SNES/HdPacks/SnesHdData.h"

class SnesHdPackLoader
{
public:
	// Load an HD pack for the given ROM name.
	// Looks in {HdPackFolder}/{romName}/ for manifest.json + PNG files.
	// Returns true if a valid pack was loaded.
	static bool LoadHdSnesPack(const string& romName, SnesHdPackData& outData);

private:
	SnesHdPackLoader() = default;

	SnesHdPackData* _data = nullptr;
	string _hdPackFolder;

	// Checksum map: key = (gfxsetIndex << 24) | (layerIndex << 16) | vramAddr → checksum value
	// Populated from checksums.bin; used by GetMatchingTile to pick the right gfxset tile.
	std::unordered_map<uint32_t, uint32_t> _checksumMap;

	bool InitializeLoader(const string& romName, SnesHdPackData* data);
	bool LoadPack();
	bool LoadManifest();
	bool LoadChecksums();
	bool LoadTilesFromDirectory(const string& dirPath, uint8_t layerIndex, bool isSprite, uint8_t gfxsetIndex = 0xFF);
	bool LoadPngFile(const string& filePath, SnesHdBitmapInfo& bitmap);
	bool ParseTileFilename(const string& filename, uint16_t& vramAddr, uint8_t& paletteIndex);
	bool ParseGfxsetDirName(const string& dirName, uint8_t& gfxsetIndex);
};
