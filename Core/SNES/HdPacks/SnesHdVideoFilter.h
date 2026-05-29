#pragma once
#include "pch.h"
#include "Shared/Video/BaseVideoFilter.h"
#include "SNES/HdPacks/SnesHdData.h"

class Emulator;
class SnesConsole;

class SnesHdVideoFilter : public BaseVideoFilter
{
private:
	SnesHdPackData* _hdData;
	SnesConsole* _console;
	uint32_t _hdScale;

	// Palette lookup (BGR555 → ARGB8888), same as SnesDefaultVideoFilter
	uint32_t _calculatedPalette[0x8000] = {};
	void InitLookupTable();

public:
	SnesHdVideoFilter(Emulator* emu, SnesConsole* console, SnesHdPackData* hdData);
	virtual ~SnesHdVideoFilter() = default;

	void ApplyFilter(uint16_t* ppuOutputBuffer) override;
	FrameInfo GetFrameInfo() override;
	OverscanDimensions GetOverscan() override;

protected:
	void OnBeforeApplyFilter() override;
};
