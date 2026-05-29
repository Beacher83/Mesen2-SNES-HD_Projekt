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

	bool InitializeLoader(const string& romName, SnesHdPackData* data);
	bool LoadPack();
	bool LoadManifest();
	bool LoadTilesFromDirectory(const string& dirPath, uint8_t layerIndex, bool isSprite);
	bool LoadPngFile(const string& filePath, SnesHdBitmapInfo& bitmap);
	bool ParseTileFilename(const string& filename, uint16_t& vramAddr, uint8_t& paletteIndex);
};
