#include "pch.h"
#include "SNES/HdPacks/SnesHdVideoFilter.h"
#include "SNES/SnesConsole.h"
#include "Shared/Emulator.h"
#include "Shared/EmuSettings.h"
#include "Shared/Video/BaseVideoFilter.h"
#include "Shared/ColorUtilities.h"
#include "Shared/MessageManager.h"
#include <unordered_set>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

// Build version — logged in diagnostics so test PC can verify correct code is running.
#define SNES_HD_BUILD_VERSION "S19"

// ---------------------------------------------------------------------------
// DiagLog — writes to both Mesen's log window AND a persistent text file.
// File is created once per session at %USERPROFILE%\Downloads\snes_hd_diag.txt
// (or $HOME/Downloads/ on non-Windows). Flushed after every write so crash
// won't lose data.
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// OpenRecorder — one place where every recorder file is opened.
//
// All of them append across sessions, which is right (a capture spans several
// sittings) and was also a trap: with no marker in the file there is no way to
// tell this session's lines from last month's. Analysing snes_hd_bgcap.txt cost
// an afternoon twice for exactly that reason — 21 886 lines from many runs, and
// "the last block" was a guess.
//
// Every open now stamps a session line, so the newest run is a search rather
// than an estimate:
//     === SESSION 2026-08-05 22:41:07 build=S18 ===
// ---------------------------------------------------------------------------
static bool RecorderPath(const char* fileName, char* out, size_t outSize)
{
	const char* home = getenv("USERPROFILE");
	if(!home) home = getenv("HOME");
	if(!home) return false;
#ifdef _WIN32
	snprintf(out, outSize, "%s\\Downloads\\%s", home, fileName);
#else
	snprintf(out, outSize, "%s/Downloads/%s", home, fileName);
#endif
	return true;
}

// ---------------------------------------------------------------------------
// SeedRecorderSet — carry a recorder's dedup across sessions.
//
// OpenRecorder appends, which is right: a capture spans several sittings. But
// the dedup sets were per-process statics, so every Mesen start re-recorded
// what earlier runs had already written and appended it a second, third, ...
// time. Measured on 2026-08-10: 11 sessions had grown snes_hd_spritecap.txt to
// 75 MB and snes_hd_spritemiss.txt to 38 MB for a few tens of thousands of
// distinct tiles. The comment "consumers dedup across sessions" was true and
// still left the files to grow without bound.
//
// Reading the keys back when the file is opened makes the append design mean
// what it says: a restart adds only what is genuinely new. Costs one sequential
// pass over the file at open, once per session, on the thread that was about to
// write to it anyway.
// ---------------------------------------------------------------------------
static void SeedRecorderSet(const char* fileName, std::unordered_set<uint64_t>& seen,
	bool (*parseKey)(const char*, uint64_t&))
{
	char path[512];
	if(!RecorderPath(fileName, path, sizeof(path))) {
		return;
	}
	FILE* f = fopen(path, "r");
	if(!f) {
		return;   // first run — nothing recorded yet
	}
	char line[4096];
	size_t before = seen.size();
	while(fgets(line, sizeof(line), f)) {
		uint64_t key;
		if(parseKey(line, key)) {
			seen.insert(key);
		}
	}
	fclose(f);
	char msg[256];
	snprintf(msg, sizeof(msg), "[SNES HD diag] %s: %zu keys from earlier sessions — only new lines get appended",
		fileName, seen.size() - before);
	MessageManager::Log(msg);
}

// Key parsers — each must reproduce EXACTLY the key its writer builds, or the
// recorder starts duplicating again without any visible symptom.
// Writer: "SPR %016llX P%d T..."
static bool ParseSpriteCapKey(const char* line, uint64_t& key)
{
	unsigned long long h; int pal;
	if(sscanf(line, "SPR %16llX P%d", &h, &pal) != 2) return false;
	key = (uint64_t)h ^ ((uint64_t)pal * 0x9E3779B97F4A7C15ULL);
	return true;
}

// Writer: "BGA G%d L%d P%d A%04X H%016llX T..."
static bool ParseBgCapKey(const char* line, uint64_t& key)
{
	int g, layer, pal; unsigned addr; unsigned long long h;
	if(sscanf(line, "BGA G%d L%d P%d A%4X H%16llX", &g, &layer, &pal, &addr, &h) != 5) return false;
	key = (uint64_t)h ^ ((uint64_t)pal * 0x9E3779B97F4A7C15ULL)
		^ ((uint64_t)layer * 0xC2B2AE3D27D4EB4FULL);
	return true;
}

// Writer: "SPRMISS %c P%d O%d G%d H%016llX T..."  ('M' = main slot 0, 'S' = sub slot 1)
static bool ParseSprMissKey(const char* line, uint64_t& key)
{
	char c; int pal, ov, g; unsigned long long h;
	if(sscanf(line, "SPRMISS %c P%d O%d G%d H%16llX", &c, &pal, &ov, &g, &h) != 5) return false;
	key = (uint64_t)h ^ ((uint64_t)pal * 0x9E3779B97F4A7C15ULL)
		^ ((c == 'M' ? 0ULL : 1ULL) * 0xC2B2AE3D27D4EB4FULL);
	return true;
}

// OAM is deduplicated on an object's COMPOSITION, not on the frame it appeared
// in — see the recorder below. Both writer and seeder hash the same substring of
// the same rendered line, starting at " W", so they cannot drift apart: position
// and OAM index sit before that point and are excluded, everything that defines
// what the object IS sits after it.
static uint64_t OamCompositionSig(const char* line)
{
	const char* tail = strstr(line, " W");
	if(!tail) return 0;
	uint64_t h = 0xCBF29CE484222325ULL;
	for(const char* p = tail; *p && *p != '\n' && *p != '\r'; p++) {
		h = (h ^ (uint8_t)*p) * 0x100000001B3ULL;
	}
	return h;
}

// Dedup happens per FRAME, folding the composition of every entry in it. Two
// alternatives were worse: keeping the frame gate on position (what the file
// grew to 399 MB with, since something on screen moves every frame), and
// dropping individual known entries from a frame (which would leave partial
// blocks behind and change what an OAMF block means to the viewer). Folding the
// whole frame keeps every written block complete and self-consistent, and a
// frame whose object set merely MOVED is recognised as one we already have.
static void SeedOamFrameSigs(std::unordered_set<uint64_t>& seen)
{
	char path[512];
	if(!RecorderPath("snes_hd_oam.txt", path, sizeof(path))) {
		return;
	}
	FILE* f = fopen(path, "r");
	if(!f) {
		return;
	}
	char line[4096];
	uint64_t cur = 0;
	bool inFrame = false;
	size_t before = seen.size();
	while(fgets(line, sizeof(line), f)) {
		if(strncmp(line, "OAMF ", 5) == 0) {
			if(inFrame && cur) {
				seen.insert(cur);
			}
			cur = 0xCBF29CE484222325ULL;
			inFrame = true;
		} else if(inFrame && strncmp(line, "OAM I", 5) == 0) {
			cur = (cur ^ OamCompositionSig(line)) * 0x100000001B3ULL;
		}
	}
	if(inFrame && cur) {
		seen.insert(cur);
	}
	fclose(f);
	char msg[256];
	snprintf(msg, sizeof(msg), "[SNES HD diag] snes_hd_oam.txt: %zu distinct object sets from earlier sessions",
		seen.size() - before);
	MessageManager::Log(msg);
}

static FILE* OpenRecorder(const char* fileName)
{
	char path[512];
	if(!RecorderPath(fileName, path, sizeof(path))) return nullptr;
	FILE* f = fopen(path, "a");
	if(f) {
		time_t now = time(nullptr);
		struct tm lt {};
#ifdef _WIN32
		localtime_s(&lt, &now);
#else
		localtime_r(&now, &lt);
#endif
		fprintf(f, "=== SESSION %04d-%02d-%02d %02d:%02d:%02d build=" SNES_HD_BUILD_VERSION " ===\n",
			lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, lt.tm_hour, lt.tm_min, lt.tm_sec);
		fflush(f);
	}
	return f;
}

static void DiagLog(const char* msg)
{
	MessageManager::Log(msg);

	static FILE* diagFile = nullptr;
	static bool diagFileAttempted = false;

	if(!diagFileAttempted) {
		diagFileAttempted = true;
		const char* home = getenv("USERPROFILE");
		if(!home) home = getenv("HOME");
		if(home) {
			char path[512];
#ifdef _WIN32
			snprintf(path, sizeof(path), "%s\\Downloads\\snes_hd_diag.txt", home);
#else
			snprintf(path, sizeof(path), "%s/Downloads/snes_hd_diag.txt", home);
#endif
			diagFile = fopen(path, "w");
			if(diagFile) {
				fprintf(diagFile, "=== SNES HD Pack Diagnostics (build " SNES_HD_BUILD_VERSION ") ===\n");
				fprintf(diagFile, "Log file: %s\n\n", path);
				fflush(diagFile);
			}
		}
	}

	if(diagFile) {
		fprintf(diagFile, "%s\n", msg);
		fflush(diagFile);
	}
}

// ---------------------------------------------------------------------------
// ContextLog — separate file for per-level PPU register snapshots.
// Written to %USERPROFILE%\Downloads\snes_hd_context.txt
// One entry per context change — stays small and readable.
// ---------------------------------------------------------------------------
static void ContextLog(const char* msg)
{
	static FILE* ctxFile = nullptr;
	static bool ctxFileAttempted = false;

	if(!ctxFileAttempted) {
		ctxFileAttempted = true;
		const char* home = getenv("USERPROFILE");
		if(!home) home = getenv("HOME");
		if(home) {
			char path[512];
#ifdef _WIN32
			snprintf(path, sizeof(path), "%s\\Downloads\\snes_hd_context.txt", home);
#else
			snprintf(path, sizeof(path), "%s/Downloads/snes_hd_context.txt", home);
#endif
			ctxFile = fopen(path, "w");
			if(ctxFile) {
				fprintf(ctxFile, "=== SNES HD Context Log (build " SNES_HD_BUILD_VERSION ") ===\n");
				fprintf(ctxFile, "One entry per level/context switch. PPU register snapshot from scanline 120.\n\n");
				fflush(ctxFile);
			}
		}
	}

	if(ctxFile) {
		fprintf(ctxFile, "%s\n", msg);
		fflush(ctxFile);
	}
}

// ---------------------------------------------------------------------------
// P4.0 helpers — HD tile sampling and PPU color-window evaluation
// ---------------------------------------------------------------------------

// Samples HD sub-pixels from a tile, honoring the tile's flip flags.
// Pixel data is alpha-PREMULTIPLIED (done at pack load time).
//
// P4.1c perf: Init() resolves the tile-local base pointer and flip-aware
// row/column steps once per native pixel; Sample() inside the hdScale×hdScale
// sub-pixel loop is then pure pointer arithmetic — no per-sub-pixel coordinate
// math or bounds checks. Init fails (valid=false) when the native tile cell
// lies outside the HD bitmap; callers must skip sampling then.
struct HdTileSampler
{
	const uint32_t* base = nullptr;
	int rowStep = 0;
	int colStep = 0;
	bool valid = false;

	inline void Init(const SnesHdPackTileInfo* tile, const SnesHdPpuTileInfo* info, uint32_t hdScale)
	{
		valid = false;
		if(!tile || !info || tile->HdTileData.empty()) {
			return;
		}
		uint8_t srcTX = info->HorizontalMirror ? (7 - info->OffsetX) : info->OffsetX;
		uint8_t srcTY = info->VerticalMirror ? (7 - info->OffsetY) : info->OffsetY;
		// The sub-pixel loop covers px in [srcTX*s, srcTX*s + s-1], same for py —
		// one bounds check for the whole cell replaces one per sub-pixel.
		if(srcTX * hdScale + hdScale > tile->Width || srcTY * hdScale + hdScale > tile->Height) {
			return;
		}
		uint32_t px0 = srcTX * hdScale + (info->HorizontalMirror ? (hdScale - 1) : 0);
		uint32_t py0 = srcTY * hdScale + (info->VerticalMirror ? (hdScale - 1) : 0);
		base = tile->HdTileData.data() + py0 * tile->Width + px0;
		rowStep = info->VerticalMirror ? -(int)tile->Width : (int)tile->Width;
		colStep = info->HorizontalMirror ? -1 : 1;
		valid = true;
	}

	inline uint32_t Sample(uint32_t dx, uint32_t dy) const
	{
		return base[(int)dy * rowStep + (int)dx * colStep];
	}
};

// Sprite recoloring: map an HD sprite's baked-in reference palette onto the live
// OBJ palette the game loaded for this level. See SnesHdData.h for why sprites
// need this and BG tiles do not -- OBJ palette slots are dynamically allocated,
// so the slot we see says nothing about the colors, and the level decides them.
//
// Per texel: find the nearest reference color and carry the texel's offset from
// it over to the live color. Anti-aliased texels sit between two palette entries,
// and keeping the offset preserves that gradient instead of snapping it to one
// side. When live == reference every delta is zero and Apply() is the exact
// identity -- a level whose sprites already look right cannot change.
struct HdSpriteRecolor
{
	bool valid = false;
	uint8_t refR[16] = {}, refG[16] = {}, refB[16] = {};
	int16_t dR[16] = {}, dG[16] = {}, dB[16] = {};

	void Init(const uint16_t* refPal, const uint16_t* liveRow)
	{
		valid = false;
		if(!refPal || !liveRow) {
			return;
		}
		bool anyDelta = false;
		for(int i = 0; i < 16; i++) {
			uint16_t rc = refPal[i] & 0x7FFF;
			uint16_t lc = liveRow[i] & 0x7FFF;
			refR[i] = ColorUtilities::Convert5BitTo8Bit(rc & 0x1F);
			refG[i] = ColorUtilities::Convert5BitTo8Bit((rc >> 5) & 0x1F);
			refB[i] = ColorUtilities::Convert5BitTo8Bit((rc >> 10) & 0x1F);
			dR[i] = (int16_t)((int)ColorUtilities::Convert5BitTo8Bit(lc & 0x1F) - (int)refR[i]);
			dG[i] = (int16_t)((int)ColorUtilities::Convert5BitTo8Bit((lc >> 5) & 0x1F) - (int)refG[i]);
			dB[i] = (int16_t)((int)ColorUtilities::Convert5BitTo8Bit((lc >> 10) & 0x1F) - (int)refB[i]);
			// Index 0 is transparent and never drawn, so a difference there is not
			// a reason to run the whole recolor.
			if(i > 0 && (dR[i] || dG[i] || dB[i])) {
				anyDelta = true;
			}
		}
		valid = anyDelta;
	}

	inline uint32_t Apply(uint32_t c) const
	{
		uint32_t a = c >> 24;
		if(a == 0) {
			return c;
		}
		int r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
		// HdTileData is premultiplied; the nearest-color search only makes sense
		// against the straight color, so undo and redo the premultiply on edges.
		if(a < 255) {
			r = std::min(255, r * 255 / (int)a);
			g = std::min(255, g * 255 / (int)a);
			b = std::min(255, b * 255 / (int)a);
		}
		int best = 1, bestD = 0x7FFFFFFF;
		for(int i = 1; i < 16; i++) {
			int er = r - (int)refR[i], eg = g - (int)refG[i], eb = b - (int)refB[i];
			int d = er * er + eg * eg + eb * eb;
			if(d < bestD) {
				bestD = d;
				best = i;
				if(d == 0) {
					break;
				}
			}
		}
		r = std::min(255, std::max(0, r + (int)dR[best]));
		g = std::min(255, std::max(0, g + (int)dG[best]));
		b = std::min(255, std::max(0, b + (int)dB[best]));
		if(a < 255) {
			r = r * (int)a / 255;
			g = g * (int)a / 255;
			b = b * (int)a / 255;
		}
		return (a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
	}
};

// P4.1c perf: memoized tile lookup.
//
// Neighboring pixels share the same 8x8 tile, so the same (hash, palette,
// layer) key hits the unordered_map up to 8 times per row — and each pixel
// performs up to 5 lookups (winner, winner-retry, bottom layer, sub-op,
// sub-op-retry). A small direct-mapped cache in front of GetMatchingTile
// removes ~7/8 of the map traffic. Negative results (nullptr) are cached
// too — misses dominate in Lockjaw (BG3 water) and Gangplank (BG1 waves).
// Keys are content hashes, so entries stay valid for the whole frame.
struct TileLookupEntry
{
	uint64_t hash = 0;
	uint8_t pal = 0xFF;
	uint8_t layer = 0xFF;
	SnesHdPackTileInfo* tile = nullptr;
};

static inline SnesHdPackTileInfo* CachedGetMatchingTile(SnesHdPackData* hdData, const uint16_t* vram,
	TileLookupEntry* cache, const SnesHdTileKey& key)
{
	if(!hdData->UseContentHash || key.ContentHash == 0) {
		return hdData->GetMatchingTile(key, vram);
	}
	TileLookupEntry& e = cache[(key.ContentHash ^ key.PaletteIndex ^ ((uint64_t)key.LayerIndex << 2)) & 15];
	if(e.hash == key.ContentHash && e.pal == key.PaletteIndex && e.layer == key.LayerIndex) {
		return e.tile;
	}
	SnesHdPackTileInfo* t = hdData->GetMatchingTile(key, vram);
	e.hash = key.ContentHash;
	e.pal = key.PaletteIndex;
	e.layer = key.LayerIndex;
	e.tile = t;
	return t;
}

// Port of WindowConfig::PixelNeedsMasking (SnesPpuTypes.h) using the
// per-scanline snapshot values instead of live PPU state.
static inline bool WindowNeedsMasking(uint8_t left, uint8_t right, bool inverted, int x)
{
	if(inverted) {
		if(left > right) {
			return true;
		}
		return x < left || x > right;
	} else {
		if(left > right) {
			return false;
		}
		return x >= left && x <= right;
	}
}

// Port of SnesPpu::ProcessMaskWindow<ColorWindowIndex> — evaluates the color
// math window state for a native x coordinate from the scanline snapshot.
static inline bool IsInsideColorWindow(const SnesHdScanlineInfo& sl, int x)
{
	uint8_t activeCount = (sl.ColorWindowActive[0] ? 1 : 0) + (sl.ColorWindowActive[1] ? 1 : 0);
	switch(activeCount) {
		case 1:
			if(sl.ColorWindowActive[0]) {
				return WindowNeedsMasking(sl.Window1Left, sl.Window1Right, sl.ColorWindowInverted[0], x);
			}
			return WindowNeedsMasking(sl.Window2Left, sl.Window2Right, sl.ColorWindowInverted[1], x);

		case 2: {
			bool w1 = WindowNeedsMasking(sl.Window1Left, sl.Window1Right, sl.ColorWindowInverted[0], x);
			bool w2 = WindowNeedsMasking(sl.Window2Left, sl.Window2Right, sl.ColorWindowInverted[1], x);
			switch(sl.ColorWindowMaskLogic) {
				default:
				case WindowMaskLogic::Or: return w1 | w2;
				case WindowMaskLogic::And: return w1 & w2;
				case WindowMaskLogic::Xor: return w1 ^ w2;
				case WindowMaskLogic::Xnor: return !(w1 ^ w2);
			}
		}
	}
	return false;
}

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

// =========================================================================
// P4.0: PPU composite reproduced at HD resolution (see ARCHITECTURE.md)
// =========================================================================
// One generic path — no overlay detection, no swaps, no tint extraction:
//
//   1. Main winner HD tile lookup (+BG1↔BG2 retry)
//   2. If found: bottom-layer HD tile for soft-alpha compositing
//   3. Sub-screen CM operand: HD tile for the sub-screen WINNER
//      (SubScreenWinnerPlus1, captured by the PPU) — independent of 1./2.
//   4. Per sub-pixel: m = blend(top over bottom over native pre-math color)
//   5. Color math = exact port of SnesPpu::ApplyColorMathToPixel:
//      clip/prevent windows, AllowColorMath flag, operand =
//      empty-sub→FixedColor+halve-off | blend(subHD over SubScreenColor)
//      | FixedColor, then ADD/SUB with clamp and halve
//   6. Brightness after color math (like the PPU)
//
// Overlay levels (Mainbrace fog, Rambi honey, Lockjaw water) are simply the
// case "main winner has no HD tile, sub winner has one" — same code path.
//
// Safety guarantees:
//   - Sprite won MAIN screen → native pixel (no HD sprites yet)
//   - Sprite on SUB screen → native SubScreenColor as operand (correct colors)
//   - No HD anywhere → native pixel (PPU output is already correct)
// =========================================================================

// ===========================================================================
// R6: multithreaded filter.
//
// The pixel loop reads only immutable per-frame data (ScreenTiles,
// ScanlineInfo, VRAM, CGRAM LUTs, tile map) and writes disjoint output rows,
// so rows render in parallel. Work is handed out dynamically in small row
// chunks (HD content clusters vertically -- static bands balance poorly).
// Per-frame counters accumulate per thread and are summed after the join,
// so FRAME-log values stay identical to the single-threaded build. In-loop
// diagnostic sampling (capped per context) synchronizes on s_diagMutex --
// cold once the caps fill.
//
// MSVC note: everything lives at file level -- local structs/lambdas inside
// ApplyFilter trigger ICE C1001 (see P4.1d journal entry).
// ===========================================================================

// Diagnostic sampling state shared by the render threads. Reset in
// ApplyFilter on context change (no render threads running then); all
// in-loop access takes s_diagMutex after a cheap unsynchronized cap check.
static std::mutex s_diagMutex;
static int diagMissCount = 0;
static int diagMatchCount = 0;
static int diagCmSampleCount = 0;    // generic CM+AddSubscreen pixel samples
static int diagSubOpSampleCount = 0; // P4.0: sub-screen operand decision samples
static int diagSprSampleCount = 0;   // P4.1e: sub-screen SPRITE pixel samples
static int diagS9SampleCount = 0;    // S9: packed sub-sprite that did NOT render HD (window-SD hunt)
static std::unordered_set<uint64_t> diagLoggedHashes;

// Read-only per-frame state shared by all render threads.
struct HdFilterFrameCtx
{
	SnesHdScreenInfo* hdScreen = nullptr;
	SnesHdPackData* hdData = nullptr;
	uint32_t* outputBuffer = nullptr;
	uint16_t* ppuOutput = nullptr;
	const uint32_t* calculatedPalette = nullptr;
	OverscanDimensions overscan = {};
	uint32_t frameWidth = 0;
	uint32_t frameHeight = 0;
	uint32_t hdScale = 1;
	uint32_t ppuWidth = 256;
	bool isHiRes = false;
	bool isWorldmap = false;
	// M5.7's worldmap lockout, narrowed. Blocking BG HD on the worldmap was only
	// ever a stand-in for scoping: back then any level tile could match a map tile
	// by hash alone. P4.2's strict gfxset scoping does that job properly, and the
	// maps now ship their own gfxsets and fingerprints, so the lockout would just
	// keep their own art from rendering. It still applies when a pack has no
	// fingerprints at all, where strict scoping cannot help.
	bool blockBgHd = false;
	uint64_t vramSig = 0;
	bool anyPalTransform = false;
	const bool* palRowActive = nullptr;        // [8]
	const uint8_t (*palLut)[3][256] = nullptr; // [8][3][256]
};

// Per-thread frame counters, summed after all threads joined.
struct HdFilterFrameStats
{
	uint32_t TotalPixels = 0;
	uint32_t BgPixels = 0;
	uint32_t HdMatch = 0;       // pixels where HD tile found for winner layer
	uint32_t HdMiss = 0;        // BG pixels where NO HD tile found for winner
	uint32_t HdCm = 0;          // HD pixels that had color math applied
	uint32_t LayerRetry = 0;    // BG1<->BG2 layer-agnostic retry matches
	uint32_t SpriteWon = 0;     // pixels where sprite won the MAIN screen
	uint32_t MaskZero = 0;      // non-sprite pixels with BgLayerMask == 0
	uint32_t LayerBits[4] = {}; // per-layer: pixels where layer has content
	uint32_t Win[4] = {};       // per-layer: pixels where layer wins compositing
	uint32_t HdLayers[4] = {};  // per-layer: HD tile found count
	uint32_t MultiLayer = 0;    // pixels where bottom HD layer also found
	uint32_t HdmaSplit = 0;     // scanlines whose registers differ from previous line
	uint32_t SubOpHd = 0;       // P4.0: CM operand sampled from a sub-screen HD tile
	uint32_t SubOpFixed = 0;    // P4.0: empty sub-screen -> FixedColor operand
	uint32_t MainNatHd = 0;     // P4.0: native main color + HD sub operand (overlay case)
	uint32_t SprRecolor = 0;    // sprite pixels whose HD art was recolored to the live OBJ palette
	uint32_t SprRecolorNoRef = 0; // sprite pixels with HD art but no reference palette in the pack
	uint32_t SprSub = 0;        // P4.1e: sprite is the final sub-screen winner
	uint32_t SprSubMainHd = 0;  // P4.1e: of those, pixels with a main-winner HD match
	uint32_t SprHd = 0;         // S4: sprite-won pixels rendered via an HD sprite tile
	uint32_t SprSubHd = 0;      // S7: sub-screen sprite rendered via an HD sprite tile (fog/water levels)
};

static void AddFilterStats(HdFilterFrameStats& dst, const HdFilterFrameStats& src)
{
	dst.TotalPixels += src.TotalPixels;
	dst.BgPixels += src.BgPixels;
	dst.HdMatch += src.HdMatch;
	dst.HdMiss += src.HdMiss;
	dst.HdCm += src.HdCm;
	dst.LayerRetry += src.LayerRetry;
	dst.SpriteWon += src.SpriteWon;
	dst.MaskZero += src.MaskZero;
	dst.MultiLayer += src.MultiLayer;
	dst.HdmaSplit += src.HdmaSplit;
	dst.SubOpHd += src.SubOpHd;
	dst.SubOpFixed += src.SubOpFixed;
	dst.MainNatHd += src.MainNatHd;
	dst.SprRecolor += src.SprRecolor;
	dst.SprRecolorNoRef += src.SprRecolorNoRef;
	dst.SprSub += src.SprSub;
	dst.SprSubMainHd += src.SprSubMainHd;
	dst.SprHd += src.SprHd;
	dst.SprSubHd += src.SprSubHd;
	for(int i = 0; i < 4; i++) {
		dst.LayerBits[i] += src.LayerBits[i];
		dst.Win[i] += src.Win[i];
		dst.HdLayers[i] += src.HdLayers[i];
	}
}

// Renders native rows [yStart, yEnd) at HD scale -- the P4.0 pixel loop,
// semantics unchanged, extracted from ApplyFilter for row-parallel execution.
static void RenderHdRows(const HdFilterFrameCtx& ctx, uint32_t yStart, uint32_t yEnd,
	HdFilterFrameStats& st, TileLookupEntry* tileLookupCache);

// Persistent worker pool. An atomic row counter hands out ChunkRows-sized
// row chunks to the workers AND the calling thread until the frame is
// drained; RunFrame returns only after every chunk completed.
struct HdFilterWorkPool
{
	static constexpr uint32_t ChunkRows = 4;
	static constexpr int MaxWorkers = 7;

	std::vector<std::thread> threads;
	std::mutex mtx;
	std::condition_variable cvStart;
	std::condition_variable cvDone;

	const HdFilterFrameCtx* ctx = nullptr;
	HdFilterFrameStats* workerStats = nullptr;
	std::atomic<uint32_t> nextRow{ 0 };
	uint32_t rowEnd = 0;
	uint64_t generation = 0;
	uint32_t pending = 0;

	void EnsureStarted()
	{
		if(!threads.empty()) {
			return;
		}
		unsigned cores = std::thread::hardware_concurrency();
		// Leave headroom for the emulation thread; the calling (video decode)
		// thread renders too, so spawn cores-2 workers, at least 1, capped.
		int workers = cores > 3 ? (int)cores - 2 : 1;
		if(workers > MaxWorkers) {
			workers = MaxWorkers;
		}
		for(int i = 0; i < workers; i++) {
			threads.emplace_back(&HdFilterWorkPool::WorkerLoop, this, i);
		}
	}

	void WorkerLoop(int index)
	{
		uint64_t seenGeneration = 0;
		for(;;) {
			{
				std::unique_lock<std::mutex> lock(mtx);
				while(generation == seenGeneration) {
					cvStart.wait(lock);
				}
				seenGeneration = generation;
			}
			DrainRows(workerStats[index]);
			{
				std::lock_guard<std::mutex> lock(mtx);
				pending--;
				if(pending == 0) {
					cvDone.notify_one();
				}
			}
		}
	}

	void DrainRows(HdFilterFrameStats& st)
	{
		// Tile-lookup memo lives per thread per frame -- content-hash keys
		// stay valid for the whole frame (P4.1c).
		TileLookupEntry tileLookupCache[16];
		for(;;) {
			uint32_t y0 = nextRow.fetch_add(ChunkRows);
			if(y0 >= rowEnd) {
				break;
			}
			uint32_t y1 = std::min(y0 + ChunkRows, rowEnd);
			RenderHdRows(*ctx, y0, y1, st, tileLookupCache);
		}
	}

	void RunFrame(const HdFilterFrameCtx& frameCtx, uint32_t yStart, uint32_t yEndArg,
		HdFilterFrameStats* statsSlots, HdFilterFrameStats& callerStats)
	{
		EnsureStarted();
		{
			std::lock_guard<std::mutex> lock(mtx);
			ctx = &frameCtx;
			workerStats = statsSlots;
			nextRow.store(yStart);
			rowEnd = yEndArg;
			pending = (uint32_t)threads.size();
			generation++;
		}
		cvStart.notify_all();
		DrainRows(callerStats);
		{
			std::unique_lock<std::mutex> lock(mtx);
			while(pending != 0) {
				cvDone.wait(lock);
			}
		}
	}
};

// Intentionally leaked singleton: joining threads from a static destructor
// would run during DLL unload (loader lock held) and can deadlock -- the OS
// reclaims the worker threads at process exit instead.
static HdFilterWorkPool& GetHdFilterPool()
{
	static HdFilterWorkPool* pool = new HdFilterWorkPool();
	return *pool;
}

static void RenderHdRows(const HdFilterFrameCtx& ctx, uint32_t yStart, uint32_t yEnd,
	HdFilterFrameStats& st, TileLookupEntry* tileLookupCache)
{
	SnesHdScreenInfo* hdScreen = ctx.hdScreen;
	SnesHdPackData* hdData = ctx.hdData;
	uint32_t* outputBuffer = ctx.outputBuffer;
	uint16_t* ppuOutputBuffer = ctx.ppuOutput;
	const uint32_t* calculatedPalette = ctx.calculatedPalette;
	const OverscanDimensions& overscan = ctx.overscan;
	const uint32_t frameWidth = ctx.frameWidth;
	const uint32_t frameHeight = ctx.frameHeight;
	const uint32_t hdScale = ctx.hdScale;
	const uint32_t ppuWidth = ctx.ppuWidth;
	const bool isHiRes = ctx.isHiRes;
	const bool isWorldmap = ctx.isWorldmap;
	const bool blockBgHd = ctx.blockBgHd;
	const uint64_t vramSig = ctx.vramSig;
	const bool anyPalTransform = ctx.anyPalTransform;
	const bool* palRowActive = ctx.palRowActive;
	const uint8_t (*palLut)[3][256] = ctx.palLut;
	const uint32_t baseWidth = 256;

	for(uint32_t y = yStart; y < yEnd; y++) {

		// HDMA split detection: track per-scanline register changes
		if(y > overscan.Top) {
			SnesHdScanlineInfo& prev = hdScreen->ScanlineInfo[y - 1];
			SnesHdScanlineInfo& cur = hdScreen->ScanlineInfo[y];
			if(cur.MainScreenLayers != prev.MainScreenLayers ||
			   cur.SubScreenLayers != prev.SubScreenLayers ||
			   cur.ColorMathEnabled != prev.ColorMathEnabled ||
			   cur.FixedColor != prev.FixedColor ||
			   cur.ScreenBrightness != prev.ScreenBrightness)
				st.HdmaSplit++;
		}

		for(uint32_t x = overscan.Left; x < baseWidth - overscan.Right; x++) {
			uint32_t srcIndex = y * SnesHdScreenInfo::ScreenWidth + x;
			SnesHdPpuPixelInfo& pixelInfo = hdScreen->ScreenTiles[srcIndex];

			// Index into ppuOutputBuffer (accounts for hi-res doubling)
			uint32_t ppuIndex = isHiRes ? (y * 2 * ppuWidth + x * 2) : (y * ppuWidth + x);

			st.TotalPixels++;

			// Sprite detection (P4.0): only a sprite winning the MAIN screen forces
			// the native path (no HD sprites yet). A sprite on the SUB screen no
			// longer blocks HD — its color is correctly included in the native
			// SubScreenColor used as the color math operand.
			bool spriteWon = (pixelInfo.MainScreenFlags & 0x40) != 0;
			if(spriteWon) {
				st.SpriteWon++;
			} else if(pixelInfo.BgLayerMask == 0) {
				st.MaskZero++;
			}

			// =============================================================
			// Phase 3.1: Winner-first with bottom-layer enhancement
			// =============================================================
			// The PPU winner is ALWAYS the top layer (avoids P2.0 bugs).
			// If the winner has no HD tile → native pixel (winner is opaque).
			// If the winner HAS an HD tile with transparency → find bottom
			// layer below in priority order for compositing behind it.

			SnesHdPackTileInfo* hdTile = nullptr;
			SnesHdPpuTileInfo* hdTileInfo = nullptr;
			SnesHdPackTileInfo* hdTileBot = nullptr;
			SnesHdPpuTileInfo* hdTileInfoBot = nullptr;
			SnesHdPackTileInfo* subTile = nullptr;     // P4.0: sub-screen winner HD tile (CM operand)
			SnesHdPpuTileInfo* subTileInfo = nullptr;
			bool cmActive = false;  // hoisted so the rendering section (below) can see it too
			uint8_t winLayer = 0xFF;  // hoisted so the rendering section (below) can see it too
			bool subSprHdFired = false;  // S9: the S7 sub-sprite HD-operand path rendered this pixel
			bool spriteIsSubOperand = false;  // P4.1f: sprite is the final sub winner AND the CM operand → force native

			if(pixelInfo.BgLayerMask != 0 && !spriteWon && !blockBgHd) {
				st.BgPixels++;

				winLayer = pixelInfo.BgWinnerLayer;
				if(winLayer < 4) st.Win[winLayer]++;

				// Count per-layer content bits (diagnostic only)
				for(int li = 0; li < 4; li++) {
					if(pixelInfo.BgLayerMask & (1 << li)) st.LayerBits[li]++;
				}

				// Check if Color Math is active for this pixel
				cmActive = (pixelInfo.MainScreenFlags & 0x80) != 0;

				// ---------------------------------------------------------
				// Unified HD tile lookup (P4.0):
				//
				// 1. Try winner layer for HD tile (+ BG1↔BG2 retry)
				// 2. If found → find bottom layer tile for soft-alpha edges
				// 3. Sub-screen CM operand tile — independent of 1./2.:
				//    the PPU blends main with the SUB-SCREEN WINNER pixel, so
				//    an HD tile for that winner improves both the full-HD case
				//    and the "native overlay over HD content" case.
				//
				// No level-specific detection, no overlay heuristics.
				// ---------------------------------------------------------
				SnesHdScanlineInfo& sl = hdScreen->ScanlineInfo[y];

				// --- Step 1: Try winner layer ---
				// R6.2 (Issue T): the winner must be enabled on the MAIN screen at
				// THIS scanline. DKC2 underwater rows (HDMA: Main=$00/$04, BG1/BG2
				// only on the sub screen) record a sub-side BgWinnerLayer; rendering
				// its HD tile as the main pixel replaced the PPU's dark water/backdrop
				// base with bright art — (HDtile+operand)/2 instead of the PPU's
				// (dark+operand)/2 → underwater terrain rendered ~2x too bright.
				// Without main-screen membership the pixel keeps the native base and
				// still gets HD detail via the sub-operand path below.
				if(winLayer < 4 && (pixelInfo.BgLayerMask & (1 << winLayer))
					&& (sl.MainScreenLayers & (1 << winLayer))) {
					hdTile = CachedGetMatchingTile(hdData, hdScreen->Vram, tileLookupCache, pixelInfo.BgTiles[winLayer].Key);

					// BG1↔BG2 layer retry
					if(!hdTile && (winLayer == 0 || winLayer == 1)) {
						SnesHdTileKey altKey = pixelInfo.BgTiles[winLayer].Key;
						altKey.LayerIndex = (winLayer == 0) ? 1 : 0;
						hdTile = CachedGetMatchingTile(hdData, hdScreen->Vram, tileLookupCache, altKey);
						if(hdTile) st.LayerRetry++;
					}
				}

				if(hdTile) {
					// --- Step 2: Winner found → render with CM + bottom layer ---
					hdTileInfo = &pixelInfo.BgTiles[winLayer];
					st.HdMatch++;
					st.HdLayers[winLayer]++;
					if(cmActive) {
						st.HdCm++;
					}

					// DIAGNOSTIC: Log first 5 unique matches per context
					// (R6: shared sampling state -> s_diagMutex, recheck after lock)
					if(diagMatchCount < 5) {
						auto& key = pixelInfo.BgTiles[winLayer].Key;
						if(key.ContentHash != 0) {
							std::lock_guard<std::mutex> diagLock(s_diagMutex);
							if(diagMatchCount < 5
								&& diagLoggedHashes.find(key.ContentHash) == diagLoggedHashes.end()) {
								diagLoggedHashes.insert(key.ContentHash);
								char buf[320];
								snprintf(buf, sizeof(buf),
									"[SNES HD diag] MATCH hash=%016llX pal=%d layer=%d win=%d cm=%d set=%d",
									(unsigned long long)key.ContentHash, key.PaletteIndex,
									key.LayerIndex, winLayer, cmActive ? 1 : 0,
									(int)hdTile->GfxsetIndex);
								DiagLog(buf);
								diagMatchCount++;
							}
						}
					}

					// DIAGNOSTIC: Sample CM pixels — log SubScreenColor when CM+AddSubscreen active
					// Generic: works for any level where winner has CM (Lockjaw, etc.)
					if(diagCmSampleCount < 20
						&& cmActive && sl.ColorMathAddSubscreen && hdTile) {
						std::lock_guard<std::mutex> diagLock(s_diagMutex);
						if(diagCmSampleCount < 20) {
							uint16_t ssc = pixelInfo.SubScreenColor;
							char buf[400];
							snprintf(buf, sizeof(buf),
								"[SNES HD diag] CM-SAMPLE win=%d SubScreenColor=0x%04X "
								"(R=%d G=%d B=%d) MainFlags=0x%02X SubLayers=0x%02X "
								"MainLayers=0x%02X CMEnabled=0x%02X x=%d y=%d sig=%016llX",
								winLayer, ssc, ssc & 0x1F, (ssc >> 5) & 0x1F, (ssc >> 10) & 0x1F,
								pixelInfo.MainScreenFlags, sl.SubScreenLayers,
								sl.MainScreenLayers, sl.ColorMathEnabled, x, y,
								(unsigned long long)vramSig);
							DiagLog(buf);
							diagCmSampleCount++;
						}
					}
					// Find bottom layer (for transparency compositing).
					// R6.1: only worth doing when the winner tile has transparent
					// pixels at all — behind a fully opaque tile the bottom layer
					// is never visible, so lookup AND per-sub-pixel blending are
					// skipped entirely. (multi= now counts only these searches.)
					if(hdTile->HasTransparentPixels) {
						uint8_t prioOrder[6];
						int prioCount = 0;

						uint8_t bg1P = pixelInfo.BgTiles[0].Priority;
						uint8_t bg2P = pixelInfo.BgTiles[1].Priority;
						uint8_t bg3P = pixelInfo.BgTiles[2].Priority;

						if(sl.Mode1Bg3Priority && bg3P) prioOrder[prioCount++] = 2;
						if(bg1P) prioOrder[prioCount++] = 0;
						if(bg2P) prioOrder[prioCount++] = 1;
						if(!bg1P) prioOrder[prioCount++] = 0;
						if(!bg2P) prioOrder[prioCount++] = 1;
						if(!sl.Mode1Bg3Priority && bg3P) prioOrder[prioCount++] = 2;
						if(!bg3P) prioOrder[prioCount++] = 2;

						bool pastWinner = false;
						for(int pi = 0; pi < prioCount && !hdTileBot; pi++) {
							uint8_t layer = prioOrder[pi];
							if(layer == winLayer) {
								pastWinner = true;
								continue;
							}
							if(!pastWinner) continue;
							if(!(pixelInfo.BgLayerMask & (1 << layer))) continue;
							if(!((sl.MainScreenLayers | sl.SubScreenLayers) & (1 << layer))) continue;

							SnesHdPackTileInfo* tile2 = CachedGetMatchingTile(hdData, hdScreen->Vram, tileLookupCache, pixelInfo.BgTiles[layer].Key);
							if(!tile2 && (layer == 0 || layer == 1)) {
								SnesHdTileKey altKey2 = pixelInfo.BgTiles[layer].Key;
								altKey2.LayerIndex = (layer == 0) ? 1 : 0;
								tile2 = CachedGetMatchingTile(hdData, hdScreen->Vram, tileLookupCache, altKey2);
							}
							if(tile2) {
								hdTileBot = tile2;
								hdTileInfoBot = &pixelInfo.BgTiles[layer];
								st.MultiLayer++;
							}
						}
					}
				}
				else {
					// Winner has no HD tile (or winner is sprite/backdrop 0xFF).
					// The native main color becomes the base — the pixel can
					// still render at HD resolution if the sub-screen operand
					// below finds an HD tile (overlay case: fog/honey/water).
					st.HdMiss++;

					// DIAGNOSTIC: Log first 60 unique misses per context
					// (R6: shared sampling state -> s_diagMutex, recheck after lock)
					if(diagMissCount < 60 && winLayer < 4) {
						SnesHdPpuTileInfo* missInfo = &pixelInfo.BgTiles[winLayer];
						if(missInfo->Key.ContentHash != 0) {
							std::lock_guard<std::mutex> diagLock(s_diagMutex);
							if(diagMissCount < 60
								&& diagLoggedHashes.find(missInfo->Key.ContentHash) == diagLoggedHashes.end()) {
								diagLoggedHashes.insert(missInfo->Key.ContentHash);
								bool inDmaRange = (missInfo->VramWordAddr >= 0x2000
									&& missInfo->VramWordAddr <= 0x21D0);
								char buf[512];
								snprintf(buf, sizeof(buf),
									"[SNES HD diag] MISS hash=%016llX pal=%d layer=%d vram=0x%04X%s "
									"mask=0x%02X win=%d",
									(unsigned long long)missInfo->Key.ContentHash,
									missInfo->Key.PaletteIndex, missInfo->Key.LayerIndex,
									missInfo->VramWordAddr, inDmaRange ? " [DMA_RANGE]" : "",
									pixelInfo.BgLayerMask, winLayer);
								DiagLog(buf);
								diagMissCount++;
							}
						}
					}
				}

				// --- Step 3 (P4.0): Sub-screen CM operand HD lookup ---
				// Runs regardless of the main winner result. The PPU adds the
				// sub-screen WINNER pixel to the main pixel — sampling that
				// tile in HD is what shows HD detail through overlay effects
				// (Mainbrace fog, Rambi honey, Lockjaw water) without any
				// overlay detection code.
				if(cmActive && sl.ColorMathAddSubscreen) {
					if(pixelInfo.SubScreenEmpty) {
						// PPU special case: FixedColor + halve disabled (applied in rendering)
						st.SubOpFixed++;
					} else if(!pixelInfo.SubScreenHasSprite) {
						uint8_t swp = pixelInfo.SubScreenWinnerPlus1;
						if(swp >= 1 && swp <= 4 && (pixelInfo.BgLayerMask & (1 << (swp - 1)))) {
							uint8_t sLayer = swp - 1;
							subTile = CachedGetMatchingTile(hdData, hdScreen->Vram, tileLookupCache, pixelInfo.BgTiles[sLayer].Key);
							if(!subTile && sLayer <= 1) {
								SnesHdTileKey altKey = pixelInfo.BgTiles[sLayer].Key;
								altKey.LayerIndex = sLayer ^ 1;
								subTile = CachedGetMatchingTile(hdData, hdScreen->Vram, tileLookupCache, altKey);
								if(subTile) st.LayerRetry++;
							}
							if(subTile) {
								subTileInfo = &pixelInfo.BgTiles[sLayer];
								st.SubOpHd++;
								if(!hdTile) st.MainNatHd++;
							}
						}
					} else {
						// A sprite is the final sub-screen winner — the CHARACTER
						// ITSELF is the color-math operand (Mainbrace/Rambi fog,
						// Lockjaw water). These are overlay levels: the sprite NEVER
						// wins the main screen, so the S4 main-sprite path can't upgrade
						// it and the character stays SD.
						st.SprSub++;
						if(hdTile) st.SprSubMainHd++;

						// S7: if the sub sprite has HD art, render THAT as the color-math
						// operand (native main base + HD character). Drop any main/bottom
						// HD tile so ONLY the sprite is upgraded — the native main base
						// (fog/water) is preserved. This keeps the P4.1f protection intact:
						// the wash-out came from painting the MAIN winner's HD overlay tile
						// over the pixel, never from the sprite itself; here that main tile
						// is suppressed, so the character can't be washed out.
						SnesHdPackTileInfo* subSprTile = nullptr;
						if(pixelInfo.SpriteCount & 0x02) {
							subSprTile = CachedGetMatchingTile(hdData, hdScreen->Vram, tileLookupCache, pixelInfo.Sprites[1].Key);
						}
						if(subSprTile && !subSprTile->HdTileData.empty()) {
							subTile = subSprTile;
							subTileInfo = &pixelInfo.Sprites[1];
							hdTile = nullptr; hdTileInfo = nullptr;
							hdTileBot = nullptr; hdTileInfoBot = nullptr;
							st.SprSubHd++;
							subSprHdFired = true;
						} else {
						// P4.1f (Issue R fix): no HD sprite art — force the native path.
						// Rendering the main winner's HD tile here would paint semi-
						// transparent HD art (Lockjaw water overlay) over the pixel before
						// the sprite is added; native water texels are dark so the PPU's
						// ADD leaves the character dominant, while brighter HD art washes
						// it out ("half-transparent characters", confirmed via SPR-SAMPLE).
						// Exact PPU output (water + sprite ADD); surrounding water stays HD.
						spriteIsSubOperand = true;
						if(diagSprSampleCount < 12 && x >= 48 && x <= 208 && y >= 40 && y <= 200) {
							std::lock_guard<std::mutex> diagLock(s_diagMutex);
							if(diagSprSampleCount < 12) {
								char buf[400];
								snprintf(buf, sizeof(buf),
									"[SNES HD diag] SPR-SAMPLE x=%d y=%d win=%d mainHd=%d bot=%d "
									"mask=0x%02X swp=%d MainCol=0x%04X SubCol=0x%04X "
									"MainFlags=0x%02X set=%d",
									x, y, winLayer, hdTile ? 1 : 0, hdTileBot ? 1 : 0,
									pixelInfo.BgLayerMask, pixelInfo.SubScreenWinnerPlus1,
									pixelInfo.MainScreenColor, pixelInfo.SubScreenColor,
									pixelInfo.MainScreenFlags,
									hdTile ? (int)hdTile->GfxsetIndex : -1);
								DiagLog(buf);
								diagSprSampleCount++;
							}
						}
						}
					}

					// DIAGNOSTIC: Log first 10 sub-operand decisions per context
					if(diagSubOpSampleCount < 10) {
						std::lock_guard<std::mutex> diagLock(s_diagMutex);
						if(diagSubOpSampleCount < 10) {
							char buf[300];
							snprintf(buf, sizeof(buf),
								"[SNES HD diag] SUBOP-SAMPLE win=%d swp=%d empty=%d spr=%d "
								"subHd=%d mainHd=%d subSet=%d x=%d y=%d Main=$%02X Sub=$%02X",
								winLayer, pixelInfo.SubScreenWinnerPlus1,
								pixelInfo.SubScreenEmpty ? 1 : 0,
								pixelInfo.SubScreenHasSprite ? 1 : 0,
								subTile ? 1 : 0, hdTile ? 1 : 0,
								subTile ? (int)subTile->GfxsetIndex : -1, x, y,
								sl.MainScreenLayers, sl.SubScreenLayers);
							DiagLog(buf);
							diagSubOpSampleCount++;
						}
					}
				}
			}

			// =============================================================
			// S10 (fix): sub-screen sprite over a BG HOLE (BgLayerMask == 0).
			// The S7 sub-sprite HD-operand path lives inside the BG block above,
			// which is gated on BgLayerMask != 0. Where the background has no
			// tile — e.g. the porthole window openings in Lockjaw's Locker, where
			// only the backdrop shows on the main screen and the character is the
			// color-math operand on the sub screen — that gate skipped S7 and the
			// character fell to native SD, exactly in front of the windows (S9-SD
			// reason=mask0, 135/135). Handle it here: native backdrop main + HD
			// sprite operand, identical to S7. Confined to this case, so no path
			// that already works is touched.
			// =============================================================
			if(pixelInfo.BgLayerMask == 0 && !spriteWon && !isWorldmap
				&& pixelInfo.SubScreenHasSprite && !pixelInfo.SubScreenEmpty
				&& (pixelInfo.MainScreenFlags & 0x80) && (pixelInfo.SpriteCount & 0x02)) {
				SnesHdScanlineInfo& slHole = hdScreen->ScanlineInfo[y];
				if(slHole.ColorMathAddSubscreen) {
					SnesHdPackTileInfo* holeSprTile = CachedGetMatchingTile(hdData, hdScreen->Vram, tileLookupCache, pixelInfo.Sprites[1].Key);
					if(holeSprTile && !holeSprTile->HdTileData.empty()) {
						subTile = holeSprTile;
						subTileInfo = &pixelInfo.Sprites[1];
						cmActive = true;  // the rendering section gates color math on this
						subSprHdFired = true;
						st.SprSubHd++;
					}
				}
			}

			// =============================================================
			// S4: HD sprites — a sprite won the MAIN screen. Look up its
			// hash-keyed HD tile (LayerIndex 4; exempt from gfxset scoping
			// and the worldmap gate — hash matches are exact, characters
			// exist everywhere). The shared rendering path below treats it
			// as the main winner: HD sprite texels compose over the native
			// pre-math color (which IS the sprite's own color, the correct
			// base for semi-transparent HD edges), then color math and
			// brightness run unchanged (underwater ADD etc. stay exact).
			// =============================================================
			if(spriteWon && (pixelInfo.SpriteCount & 0x01)) {
				cmActive = (pixelInfo.MainScreenFlags & 0x80) != 0;
				hdTile = CachedGetMatchingTile(hdData, hdScreen->Vram, tileLookupCache, pixelInfo.Sprites[0].Key);
				if(hdTile) {
					hdTileInfo = &pixelInfo.Sprites[0];
					st.SprHd++;
					if(cmActive) st.HdCm++;
				}
			}

			// =============================================================
			// S9: window-SD hunt. A SUB sprite is present AND its tile IS in
			// the pack, yet it did NOT render as an HD sub-operand (S7) and is
			// not covered by an HD main-sprite either → it will show SD. Log
			// x/y + WHY the HD path was not taken. This isolates the Lockjaw
			// "character loses HD in front of the porthole windows" case: the
			// same (packed) swim tile is HD elsewhere, so the cause must be a
			// per-pixel routing gate, not missing art.
			// =============================================================
			if(diagS9SampleCount < 40 && (pixelInfo.SpriteCount & 0x02)
				&& !subSprHdFired && pixelInfo.Sprites[1].Key.ContentHash != 0) {
				bool mainHdSprite = spriteWon && (pixelInfo.SpriteCount & 0x01) && hdTile;
				if(!mainHdSprite && CachedGetMatchingTile(hdData, hdScreen->Vram, tileLookupCache, pixelInfo.Sprites[1].Key)) {
					SnesHdScanlineInfo& slS9 = hdScreen->ScanlineInfo[y];
					const char* reason =
						spriteWon ? (pixelInfo.SpriteCount & 0x01 ? "MAIN-sprMiss" : "MAIN-noCap") :
						pixelInfo.BgLayerMask == 0 ? "mask0" :
						!(pixelInfo.MainScreenFlags & 0x80) ? "noCM" :
						!slS9.ColorMathAddSubscreen ? "noAddSub" :
						pixelInfo.SubScreenEmpty ? "subEmpty" :
						!pixelInfo.SubScreenHasSprite ? "subBGwins" : "other";
					std::lock_guard<std::mutex> diagLock(s_diagMutex);
					if(diagS9SampleCount < 40) {
						char buf[320];
						snprintf(buf, sizeof(buf),
							"[SNES HD diag] S9-SD x=%d y=%d reason=%s sprWon=%d mask=0x%02X "
							"swp=%d subSpr=%d subEmpty=%d CM=%d addSub=%d Main=$%02X Sub=$%02X "
							"hash=%016llX P%d",
							x, y, reason, spriteWon ? 1 : 0, pixelInfo.BgLayerMask,
							pixelInfo.SubScreenWinnerPlus1, pixelInfo.SubScreenHasSprite ? 1 : 0,
							pixelInfo.SubScreenEmpty ? 1 : 0, (pixelInfo.MainScreenFlags & 0x80) ? 1 : 0,
							slS9.ColorMathAddSubscreen ? 1 : 0,
							slS9.MainScreenLayers, slS9.SubScreenLayers,
							(unsigned long long)pixelInfo.Sprites[1].Key.ContentHash,
							pixelInfo.Sprites[1].Key.PaletteIndex);
						DiagLog(buf);
						diagS9SampleCount++;
					}
				}
			}

			// =============================================================
			// Rendering (P4.0): reproduce the PPU composite at HD resolution
			//
			//   main = HD(top) over HD(bottom) over native pre-math color
			//   out  = ColorMath(main, operand) — exact ApplyColorMathToPixel port
			//   out  = Brightness(out)
			//
			// Renders in HD when the main winner OR the sub-screen operand
			// has an HD tile. Overlay effects are just "main native + sub HD".
			// =============================================================
			uint32_t outX = (x - overscan.Left) * hdScale;
			uint32_t outY = (y - overscan.Top) * hdScale;

			bool hasMainHd = hdTile && hdTileInfo && !hdTile->HdTileData.empty();
			bool hasSubHd = subTile && subTileInfo && !subTile->HdTileData.empty();

			// P4.1f: when the character is the color-math operand, render the
			// exact PPU output instead of compositing HD art over it (Issue R).
			if((hasMainHd || hasSubHd) && !spriteIsSubOperand) {
				SnesHdScanlineInfo& sl = hdScreen->ScanlineInfo[y];
				uint8_t brightness = sl.ScreenBrightness;

				// Native pre-math main screen color — base under the HD layers
				uint8_t nmR = ColorUtilities::Convert5BitTo8Bit(pixelInfo.MainScreenColor & 0x1F);
				uint8_t nmG = ColorUtilities::Convert5BitTo8Bit((pixelInfo.MainScreenColor >> 5) & 0x1F);
				uint8_t nmB = ColorUtilities::Convert5BitTo8Bit((pixelInfo.MainScreenColor >> 10) & 0x1F);

				// Native sub-screen color and fixed color — CM operand bases
				uint8_t nsR = ColorUtilities::Convert5BitTo8Bit(pixelInfo.SubScreenColor & 0x1F);
				uint8_t nsG = ColorUtilities::Convert5BitTo8Bit((pixelInfo.SubScreenColor >> 5) & 0x1F);
				uint8_t nsB = ColorUtilities::Convert5BitTo8Bit((pixelInfo.SubScreenColor >> 10) & 0x1F);
				uint8_t fxR = ColorUtilities::Convert5BitTo8Bit(sl.FixedColor & 0x1F);
				uint8_t fxG = ColorUtilities::Convert5BitTo8Bit((sl.FixedColor >> 5) & 0x1F);
				uint8_t fxB = ColorUtilities::Convert5BitTo8Bit((sl.FixedColor >> 10) & 0x1F);

				// Color window state — windows are register-level x ranges,
				// evaluated once per native pixel (no sub-pixel windows needed)
				bool isInsideWindow = IsInsideColorWindow(sl, (int)x);

				// P4.1c perf: everything that is constant across the hdScale×hdScale
				// sub-pixel block is decided once per native pixel. Semantics are an
				// exact match of the previous per-sub-pixel ApplyColorMathToPixel port.
				HdTileSampler botSampler, mainSampler, subSampler;
				botSampler.Init(hdTileBot, hdTileInfoBot, hdScale);
				if(hasMainHd) mainSampler.Init(hdTile, hdTileInfo, hdScale);
				if(hasSubHd) subSampler.Init(subTile, subTileInfo, hdScale);

				// R3: per-tile palette-row transform (live CGRAM vs reference).
				// Resolved once per native pixel; nullptr = identity.
				// R3.1: pointer to the row's 3×256 LUT — one load per channel per sample.
				typedef const uint8_t (*PalLutRow)[256];
				PalLutRow botLut = nullptr, mainLut = nullptr, subLut = nullptr;
				if(anyPalTransform) {
					if(botSampler.valid && palRowActive[hdTileInfoBot->Key.PaletteIndex & 7]) {
						botLut = palLut[hdTileInfoBot->Key.PaletteIndex & 7];
					}
					// S4: no LUT for sprites — the R3 transform covers BG CGRAM rows
					// 0-7 (entries 0-127) only; OBJ palettes live at CGRAM 128-255.
					if(mainSampler.valid && hdTileInfo->Key.LayerIndex != 4
						&& palRowActive[hdTileInfo->Key.PaletteIndex & 7]) {
						mainLut = palLut[hdTileInfo->Key.PaletteIndex & 7];
					}
					// S7: sub operand may now be a sprite (LayerIndex 4) — same OBJ-palette
					// exemption as the main sprite path above.
					if(subSampler.valid && subTileInfo->Key.LayerIndex != 4
						&& palRowActive[subTileInfo->Key.PaletteIndex & 7]) {
						subLut = palLut[subTileInfo->Key.PaletteIndex & 7];
					}
				}

				// Sprites are exempt from the R3 LUT above (it only covers BG rows)
				// and get the recolor instead: live OBJ colors come from CGRAM at the
				// slot the game allocated, the reference ships with the art.
				// A/B switch. The recolor is the only thing S19 changed about how a
				// sprite LOOKS, so being able to turn it off in a running build is
				// the difference between measuring and guessing:
				//     set SNES_HD_NO_SPRITE_RECOLOR=1
				// before starting Mesen to render sprites exactly as S18 did (baked
				// colors, no correction). Read once, so toggling needs a restart.
				static const bool s_noRecolor = getenv("SNES_HD_NO_SPRITE_RECOLOR") != nullptr;

				HdSpriteRecolor mainRecolor, subRecolor;
				if(!s_noRecolor && mainSampler.valid && hdTileInfo->Key.LayerIndex == 4) {
					const uint16_t* spriteRef = hdData->GetSpriteRefPalette(hdTileInfo->Key.ContentHash);
					mainRecolor.Init(spriteRef, hdScreen->Cgram + 128 + (hdTileInfo->Key.PaletteIndex & 7) * 16);
					// Separated on purpose: "no reference shipped" is a pack gap worth
					// reporting, while "reference == live" is the normal, correct case
					// and must not look like one.
					if(!spriteRef) {
						st.SprRecolorNoRef++;
					} else if(mainRecolor.valid) {
						st.SprRecolor++;
					}
				}
				if(!s_noRecolor && subSampler.valid && subTileInfo->Key.LayerIndex == 4) {
					subRecolor.Init(hdData->GetSpriteRefPalette(subTileInfo->Key.ContentHash),
						hdScreen->Cgram + 128 + (subTileInfo->Key.PaletteIndex & 7) * 16);
				}

				// 1. Clip main color to black (runs even without AllowColorMath;
				//    Always mode does NOT reset halfShift — matches PPU)
				int halfShift = sl.ColorMathHalveResult ? 1 : 0;
				bool clipMain = false;
				switch(sl.ColorMathClipMode) {
					default:
					case ColorWindowMode::Never: break;
					case ColorWindowMode::OutsideWindow:
						if(!isInsideWindow) { clipMain = true; halfShift = 0; }
						break;
					case ColorWindowMode::InsideWindow:
						if(isInsideWindow) { clipMain = true; halfShift = 0; }
						break;
					case ColorWindowMode::Always: clipMain = true; break;
				}

				// 2. AllowColorMath (per-pixel PPU flag) + prevent window
				bool prevented = false;
				switch(sl.ColorMathPreventMode) {
					default:
					case ColorWindowMode::Never: break;
					case ColorWindowMode::OutsideWindow: prevented = !isInsideWindow; break;
					case ColorWindowMode::InsideWindow: prevented = isInsideWindow; break;
					case ColorWindowMode::Always: prevented = true; break;
				}
				bool doMath = cmActive && !prevented;
				// 3. Second operand: fixed color when AddSubscreen is off, or on the
				//    PPU's empty-sub-screen special case (which also disables halve)
				bool operandFixed = !sl.ColorMathAddSubscreen || pixelInfo.SubScreenEmpty;
				if(doMath && sl.ColorMathAddSubscreen && pixelInfo.SubScreenEmpty) {
					halfShift = 0;
				}

				// Per sub-pixel: compose pre-math main pixel → color math → brightness
				for(uint32_t dy = 0; dy < hdScale; dy++) {
					for(uint32_t dx = 0; dx < hdScale; dx++) {
						uint32_t outIndex = (outY + dy) * frameWidth + (outX + dx);
						if(outIndex >= frameWidth * frameHeight) continue;

						// --- Compose pre-math main pixel (HD layers over native) ---
						// R6.1: sample the main winner FIRST — a fully opaque texel
						// hides bottom layer and native base completely, so both
						// blends are skipped (interior texels are usually opaque;
						// only tile edges/effects carry partial alpha). Identical
						// result: blend with ha=255 reduces to r=hr anyway.
						int r = 0, g = 0, b = 0;
						if(!clipMain) {
							uint32_t mc = 0;
							uint32_t mha = 0;
							if(mainSampler.valid) {
								mc = mainSampler.Sample(dx, dy);
								if(mainRecolor.valid) {
									mc = mainRecolor.Apply(mc);
								}
								mha = mc >> 24;
							}
							if(mha == 255) {
								r = (mc >> 16) & 0xFF; g = (mc >> 8) & 0xFF; b = mc & 0xFF;
								if(mainLut) {
									// R3: follow live CGRAM (rgb is premultiplied; alpha unchanged)
									r = mainLut[0][r]; g = mainLut[1][g]; b = mainLut[2][b];
								}
							} else {
								r = nmR; g = nmG; b = nmB;
								if(botSampler.valid) {
									uint32_t c = botSampler.Sample(dx, dy);
									uint32_t ha = c >> 24;
									if(ha == 255) {
										r = (c >> 16) & 0xFF; g = (c >> 8) & 0xFF; b = c & 0xFF;
										if(botLut) {
											r = botLut[0][r]; g = botLut[1][g]; b = botLut[2][b];
										}
									} else if(ha > 0) {
										int hr = (c >> 16) & 0xFF, hg = (c >> 8) & 0xFF, hb = c & 0xFF;
										if(botLut) {
											hr = botLut[0][hr]; hg = botLut[1][hg]; hb = botLut[2][hb];
										}
										// premultiplied alpha blend
										r = hr + (r * (255 - (int)ha)) / 255;
										g = hg + (g * (255 - (int)ha)) / 255;
										b = hb + (b * (255 - (int)ha)) / 255;
									}
								}
								if(mha > 0) {
									int hr = (mc >> 16) & 0xFF, hg = (mc >> 8) & 0xFF, hb = mc & 0xFF;
									if(mainLut) {
										hr = mainLut[0][hr]; hg = mainLut[1][hg]; hb = mainLut[2][hb];
									}
									r = hr + (r * (255 - (int)mha)) / 255;
									g = hg + (g * (255 - (int)mha)) / 255;
									b = hb + (b * (255 - (int)mha)) / 255;
								}
							}
						}

						// --- Color math: exact port of SnesPpu::ApplyColorMathToPixel ---
						if(doMath) {
							// 3. Second operand
							int oR, oG, oB;
							if(operandFixed) {
								oR = fxR; oG = fxG; oB = fxB;
							} else {
								oR = nsR; oG = nsG; oB = nsB;
								if(subSampler.valid) {
									uint32_t c = subSampler.Sample(dx, dy);
									if(subRecolor.valid) {
										c = subRecolor.Apply(c);
									}
									uint32_t ha = c >> 24;
									if(ha == 255) {
										// R6.1: opaque fast path (blend reduces to o=h)
										oR = (c >> 16) & 0xFF; oG = (c >> 8) & 0xFF; oB = c & 0xFF;
										if(subLut) {
											oR = subLut[0][oR]; oG = subLut[1][oG]; oB = subLut[2][oB];
										}
									} else if(ha > 0) {
										int hr = (c >> 16) & 0xFF, hg = (c >> 8) & 0xFF, hb = c & 0xFF;
										if(subLut) {
											hr = subLut[0][hr]; hg = subLut[1][hg]; hb = subLut[2][hb];
										}
										oR = hr + (oR * (255 - (int)ha)) / 255;
										oG = hg + (oG * (255 - (int)ha)) / 255;
										oB = hb + (oB * (255 - (int)ha)) / 255;
									}
								}
							}

							// 4. Arithmetic — 8-bit equivalent of the PPU's 5-bit math
							if(sl.ColorMathSubtractMode) {
								r = std::max(0, r - oR) >> halfShift;
								g = std::max(0, g - oG) >> halfShift;
								b = std::max(0, b - oB) >> halfShift;
							} else {
								r = std::min(255, r + oR) >> halfShift;
								g = std::min(255, g + oG) >> halfShift;
								b = std::min(255, b + oB) >> halfShift;
							}
						}

						// --- Brightness (after color math, like the PPU) ---
						if(brightness < 15) {
							r = r * brightness / 15;
							g = g * brightness / 15;
							b = b * brightness / 15;
						}

						outputBuffer[outIndex] = 0xFF000000 | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
					}
				}
			} else {
				// No HD replacement — use original SNES color, scaled up
				uint32_t color = calculatedPalette[ppuOutputBuffer[ppuIndex] & 0x7FFF];
				for(uint32_t dy = 0; dy < hdScale; dy++) {
					for(uint32_t dx = 0; dx < hdScale; dx++) {
						uint32_t outIndex = (outY + dy) * frameWidth + (outX + dx);
						if(outIndex < frameWidth * frameHeight) {
							outputBuffer[outIndex] = color;
						}
					}
				}
			}
		} // end for(x)
	} // end for(y)
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

	// The PPU output buffer width depends on hi-res mode
	uint32_t ppuWidth = _baseFrameInfo.Width;
	bool isHiRes = (ppuWidth == 512);

	// =====================================================================
	// DIAGNOSTIC: Context-aware logging with combined VRAM+PPU detection
	// Context = VRAM signature + core PPU registers (Main/Sub/CM at scanline 120).
	// This ensures levels with different PPU configs are ALWAYS separate contexts,
	// even if their VRAM hashes collide (e.g., Lockjaw vs Mainbrace).
	// All per-context counters are reset together on context change.
	// Frame budget per context: 10 total + 60 BG frames.
	// =====================================================================
	static uint64_t diagPrevContextKey = 0;
	static int diagFrameCount = 0;
	static int diagBgFrameCount = 0;
	static double diagMsMax = 0;         // R6.1: worst filter time (ms) since context change
	// R6: diagMissCount/diagMatchCount/diagCmSampleCount/diagSubOpSampleCount/
	// diagSprSampleCount/diagLoggedHashes moved to file scope — they are sampled
	// from the parallel render threads (under s_diagMutex) and reset below.
	static int diagPalLogCount = 0;      // R3: CGRAM-diff transform detail lines
	static int diagSprCapLogCount = 0;   // S1: SPRTILE sample-line batches per context
	static bool hdmaDumped = false;
	static int diagContextCount = 0;     // total context changes seen

	// Compute VRAM context signature from two stable reference tiles
	uint64_t sigA = 0, sigB = 0;
	bool isLevel2 = false;
	bool isWorldmap = false;
	if(hdScreen->Vram) {
		sigA = ComputeTileContentHash(hdScreen->Vram, 0x32E0);
		sigB = ComputeTileContentHash(hdScreen->Vram, 0x2080);
		isLevel2 = (sigA == 0x1585855B0633F405ULL);
	}
	uint64_t vramSig = sigA ^ (sigB << 1);
	isWorldmap = (vramSig == 0xDBF342F9932FD251ULL);

	// P4.1: gfxset fingerprint detection — DIAGNOSTIC ONLY for now.
	// The 2026-07-14 review found DetectActiveGfxset() was never wired up
	// (fingerprints loaded but dormant). Before enforcing gfxset scoping in
	// GetMatchingTile we log what it reports, to verify fingerprint coverage
	// and confirm the cross-gfxset-contamination hypothesis (Gusty Glade
	// blue squares: BG3 sub-op tiles matching with wrong-set colors).
	if(hdScreen->Vram && _hdData->HasFingerprints()) {
		_hdData->DetectActiveGfxset(hdScreen->Vram);
	}

	// =====================================================================
	// R3: per-palette-row CGRAM-diff transform.
	//
	// HD tiles have their colors baked in at export time. When the game
	// shifts CGRAM afterwards (Lockjaw underwater darkening, Gangplank
	// sunset HDMA, Mainbrace palette cycling), native pixels follow but HD
	// art doesn't. With reference palettes in the pack (palettes.bin, the
	// CGRAM state at export time) we compute — once per frame — a per-
	// palette-row, per-channel ratio live/reference (8.8 fixed point) and
	// scale every HD sample by its tile's row ratio. Rows that match the
	// reference exactly are skipped, so levels without CGRAM effects (and
	// packs without palettes.bin) are completely unaffected.
	// =====================================================================
	uint16_t palRatio[8][3];
	bool palRowActive[8] = {};
	bool anyPalTransform = false;
	// R3.1 perf: the live palettes differ from the ROM reference in EVERY row
	// of EVERY level (the game post-processes palettes at load — the transform
	// therefore also corrects the HD tiles' global color fidelity, confirmed
	// visually). That defeats the "identical row → skip" fast path, so the
	// per-sample cost matters: precomputed 8×3×256 LUTs (6 KB, L1-resident)
	// replace per-sample multiply/shift/clamp with one table load per channel.
	uint8_t palLut[8][3][256];
	if(_hdData->ActiveGfxset >= 0 && !_hdData->GfxsetPalettes.empty()) {
		auto palIt = _hdData->GfxsetPalettes.find((uint8_t)_hdData->ActiveGfxset);
		if(palIt != _hdData->GfxsetPalettes.end() && palIt->second.size() >= 128) {
			const uint16_t* ref = palIt->second.data();
			for(int row = 0; row < 8; row++) {
				const uint16_t* refRow = ref + row * 16;
				const uint16_t* liveRow = hdScreen->Cgram + row * 16;
				// Index 0 of each row is the transparent color — not part of
				// any visible tile pixel, so it is excluded from comparison.
				uint32_t refSum[3] = {}, liveSum[3] = {};
				bool differs = false;
				for(int i = 1; i < 16; i++) {
					uint16_t rc = refRow[i] & 0x7FFF;
					uint16_t lc = liveRow[i] & 0x7FFF;
					if(rc != lc) differs = true;
					refSum[0] += rc & 0x1F;  refSum[1] += (rc >> 5) & 0x1F;  refSum[2] += (rc >> 10) & 0x1F;
					liveSum[0] += lc & 0x1F; liveSum[1] += (lc >> 5) & 0x1F; liveSum[2] += (lc >> 10) & 0x1F;
				}
				if(!differs) continue;
				for(int ch = 0; ch < 3; ch++) {
					// Ratio capped at 4x brightening; ref channel sum 0 → identity
					uint32_t ratio = refSum[ch]
						? std::min<uint32_t>(1024, (liveSum[ch] * 256) / refSum[ch])
						: 256;
					palRatio[row][ch] = (uint16_t)ratio;
					uint8_t* lut = palLut[row][ch];
					for(uint32_t v = 0; v < 256; v++) {
						lut[v] = (uint8_t)std::min<uint32_t>(255, (v * ratio) >> 8);
					}
				}
				palRowActive[row] = true;
				anyPalTransform = true;
			}
		}
	}

	// PPU config snapshot at scanline 120 (mid-screen, stable reference)
	SnesHdScanlineInfo& slCtx = hdScreen->ScanlineInfo[120];
	uint64_t ppuConfigKey =
		((uint64_t)slCtx.MainScreenLayers) |
		((uint64_t)slCtx.SubScreenLayers << 8) |
		((uint64_t)slCtx.ColorMathEnabled << 16) |
		((uint64_t)(slCtx.ColorMathAddSubscreen ? 1 : 0) << 24) |
		((uint64_t)(slCtx.ColorMathSubtractMode ? 1 : 0) << 25) |
		((uint64_t)(slCtx.ColorMathHalveResult ? 1 : 0) << 26) |
		((uint64_t)(slCtx.Mode1Bg3Priority ? 1 : 0) << 27) |
		((uint64_t)slCtx.BgMode << 28) |
		((uint64_t)(slCtx.FixedColor & 0x7FFF) << 32) |
		((uint64_t)(slCtx.ScreenBrightness == 15 ? 15 : 0) << 48);

	// Combined context key: VRAM sig XOR shifted PPU config
	// Ensures Lockjaw (Main=$01/Sub=$16/CM=$21) is always distinct from
	// Mainbrace (Main=$04/Sub=$13/CM=$24) even if VRAM hashes collide
	uint64_t contextKey = vramSig ^ (ppuConfigKey * 0x9E3779B97F4A7C15ULL);

	// P4.1b: ring of recently seen context keys. Lockjaw's animated CGRAM/CHR
	// cycle rotates through ~8 VRAM sigs round-robin, so a plain prev-key compare
	// fired a "context change" EVERY frame — resetting all counters and re-logging
	// FRAME/MISS/MATCH lines each frame. That flooded the log AND put heavy file
	// I/O (fflush per line) on the filter thread every frame, feeding the decode
	// overruns behind Issue Q. A key only counts as a new context if it wasn't
	// seen in the last 16 distinct contexts.
	static uint64_t diagRecentKeys[16] = {};
	static int diagRecentPos = 0;
	bool diagKeyInRing = false;
	for(int ri = 0; ri < 16; ri++) {
		if(diagRecentKeys[ri] == contextKey) { diagKeyInRing = true; break; }
	}

	// Detect context change → reset ALL diagnostic counters
	bool diagContextChanged = (contextKey != diagPrevContextKey) && !diagKeyInRing;
	if(diagContextChanged) {
		diagRecentKeys[diagRecentPos] = contextKey;
		diagRecentPos = (diagRecentPos + 1) % 16;
	}
	if(diagContextChanged && diagPrevContextKey != 0) {
		const char* ctxLabel = isWorldmap ? "WORLDMAP" : (isLevel2 ? "LEVEL2" : "other");
		char buf[512];
		snprintf(buf, sizeof(buf),
			"[SNES HD diag] === CONTEXT CHANGE #%d (build=" SNES_HD_BUILD_VERSION "): "
			"sig %016llX (%s) Main=$%02X Sub=$%02X CM=$%02X AddSub=%d Br=%d gfx=%d/%zu ===",
			diagContextCount, (unsigned long long)vramSig, ctxLabel,
			slCtx.MainScreenLayers, slCtx.SubScreenLayers, slCtx.ColorMathEnabled,
			slCtx.ColorMathAddSubscreen ? 1 : 0, slCtx.ScreenBrightness,
			(int)_hdData->ActiveGfxset, _hdData->GfxsetFingerprints.size());
		DiagLog(buf);
	}
	if(diagContextChanged) {
		diagFrameCount = 0;
		diagBgFrameCount = 0;
		diagMissCount = 0;
		diagMatchCount = 0;
		diagCmSampleCount = 0;
		diagSubOpSampleCount = 0;
		diagSprSampleCount = 0;
		diagS9SampleCount = 0;
		diagPalLogCount = 0;
		diagSprCapLogCount = 0;
		diagLoggedHashes.clear();
		hdmaDumped = false;
		diagMsMax = 0;
		diagContextCount++;
	}

	// Track pending context log for deferred PPU config logging
	static uint64_t diagPrevPpuConfigKey = 0;
	static bool diagPendingContextLog = false;
	static uint64_t diagPendingVramSig = 0;

	if(contextKey != diagPrevContextKey) {
		diagPendingVramSig = vramSig;
	}

	if(ppuConfigKey != diagPrevPpuConfigKey || diagPrevContextKey == 0) {
		diagPendingContextLog = true;
		diagPendingVramSig = vramSig;
		diagPrevPpuConfigKey = ppuConfigKey;
	}

	diagPrevContextKey = contextKey;

	// =====================================================================
	// R6: Main pixel loop -- rendered in parallel row chunks on the worker
	// pool (see RenderHdRows above). Per-frame counters accumulate per
	// thread and are summed below; log values stay identical.
	// =====================================================================
	HdFilterFrameCtx renderCtx;
	renderCtx.hdScreen = hdScreen;
	renderCtx.hdData = _hdData;
	renderCtx.outputBuffer = outputBuffer;
	renderCtx.ppuOutput = ppuOutputBuffer;
	renderCtx.calculatedPalette = _calculatedPalette;
	renderCtx.overscan = overscan;
	renderCtx.frameWidth = frameInfo.Width;
	renderCtx.frameHeight = frameInfo.Height;
	renderCtx.hdScale = hdScale;
	renderCtx.ppuWidth = ppuWidth;
	renderCtx.isHiRes = isHiRes;
	renderCtx.isWorldmap = isWorldmap;
	renderCtx.blockBgHd = isWorldmap && !_hdData->HasFingerprints();
	renderCtx.vramSig = vramSig;
	renderCtx.anyPalTransform = anyPalTransform;
	renderCtx.palRowActive = palRowActive;
	renderCtx.palLut = palLut;

	HdFilterFrameStats statsSlots[HdFilterWorkPool::MaxWorkers];
	HdFilterFrameStats callerStats;
	std::chrono::steady_clock::time_point filterT0 = std::chrono::steady_clock::now();
	GetHdFilterPool().RunFrame(renderCtx, overscan.Top, 239 - overscan.Bottom, statsSlots, callerStats);
	// R6.1: filter time for this frame — logged in the FRAME line (ms=cur/max).
	// Budget is 16.7 ms; frames above it stall the emu thread (P4.1b wait).
	double filterMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - filterT0).count();
	if(filterMs > diagMsMax) {
		diagMsMax = filterMs;
	}

	// Sum per-thread counters -- local names keep the diagnostics code below unchanged
	HdFilterFrameStats total;
	AddFilterStats(total, callerStats);
	for(int si = 0; si < HdFilterWorkPool::MaxWorkers; si++) {
		AddFilterStats(total, statsSlots[si]);
	}
	uint32_t frameTotalPixels = total.TotalPixels;
	uint32_t frameBgPixels = total.BgPixels;
	uint32_t frameHdMatch = total.HdMatch;
	uint32_t frameHdMiss = total.HdMiss;
	uint32_t frameHdCm = total.HdCm;
	uint32_t frameLayerRetry = total.LayerRetry;
	uint32_t frameSpriteWon = total.SpriteWon;
	uint32_t frameMaskZero = total.MaskZero;
	uint32_t frameMultiLayer = total.MultiLayer;
	uint32_t frameHdmaSplit = total.HdmaSplit;
	uint32_t frameSubOpHd = total.SubOpHd;
	uint32_t frameSubOpFixed = total.SubOpFixed;
	uint32_t frameMainNatHd = total.MainNatHd;
	uint32_t frameSprSub = total.SprSub;
	uint32_t frameSprSubMainHd = total.SprSubMainHd;
	uint32_t frameSprHd = total.SprHd;
	uint32_t frameSprSubHd = total.SprSubHd;
	uint32_t frameSprRecolor = total.SprRecolor;
	uint32_t frameSprRecolorNoRef = total.SprRecolorNoRef;
	uint32_t frameLayerBits[4];
	uint32_t frameWin[4];
	uint32_t frameHdLayers[4];
	for(int li = 0; li < 4; li++) {
		frameLayerBits[li] = total.LayerBits[li];
		frameWin[li] = total.Win[li];
		frameHdLayers[li] = total.HdLayers[li];
	}

	// =====================================================================
	// DIAGNOSTIC: Deferred context log — emitted on first frame with BG content
	// after a PPU config change (avoids capturing transition-frame zeros)
	// =====================================================================
	if(diagPendingContextLog && frameBgPixels > 0) {
		diagPendingContextLog = false;

		// Re-read scanline 120 (now guaranteed to have real data)
		SnesHdScanlineInfo& slLog = hdScreen->ScanlineInfo[120];
		const char* ctxLabel = isWorldmap ? "WORLDMAP" : (isLevel2 ? "LEVEL2" : "other");

		const char* renderMode = "NORMAL";
		if(slLog.Mode1Bg3Priority && (slLog.ColorMathEnabled & 0x04)) {
			renderMode = "BG3_OVERLAY";
		} else if(slLog.ColorMathAddSubscreen && (slLog.SubScreenLayers & 0x04)) {
			renderMode = "SUBSCREEN_BLEND";
		} else if(!slLog.ColorMathAddSubscreen && slLog.FixedColor != 0x0000) {
			renderMode = "FIXED_COLOR";
		}

		char ctxBuf[1024];
		snprintf(ctxBuf, sizeof(ctxBuf),
			"--- CONTEXT: sig=%016llX (%s) ---\n"
			"  BgMode=%d  Mode1Bg3Pri=%d\n"
			"  MainScreenLayers=$%02X  SubScreenLayers=$%02X\n"
			"  CMEnabled=$%02X (BG1=%d BG2=%d BG3=%d BG4=%d OBJ=%d BDrop=%d)\n"
			"  AddSubscreen=%d  SubtractMode=%d  HalveResult=%d\n"
			"  FixedColor=$%04X  ScreenBrightness=%d\n"
			"  ClipMode=%d  PreventMode=%d\n"
			"  Window1: L=%d R=%d  Window2: L=%d R=%d\n"
			"  ColorWin: active=[%d,%d] inv=[%d,%d] logic=%d\n"
			"  -> Detected mode: %s\n",
			(unsigned long long)diagPendingVramSig, ctxLabel,
			slLog.BgMode, slLog.Mode1Bg3Priority ? 1 : 0,
			slLog.MainScreenLayers, slLog.SubScreenLayers,
			slLog.ColorMathEnabled,
			(slLog.ColorMathEnabled & 0x01) ? 1 : 0,
			(slLog.ColorMathEnabled & 0x02) ? 1 : 0,
			(slLog.ColorMathEnabled & 0x04) ? 1 : 0,
			(slLog.ColorMathEnabled & 0x08) ? 1 : 0,
			(slLog.ColorMathEnabled & 0x10) ? 1 : 0,
			(slLog.ColorMathEnabled & 0x20) ? 1 : 0,
			slLog.ColorMathAddSubscreen ? 1 : 0,
			slLog.ColorMathSubtractMode ? 1 : 0,
			slLog.ColorMathHalveResult ? 1 : 0,
			slLog.FixedColor, slLog.ScreenBrightness,
			(int)slLog.ColorMathClipMode, (int)slLog.ColorMathPreventMode,
			slLog.Window1Left, slLog.Window1Right,
			slLog.Window2Left, slLog.Window2Right,
			slLog.ColorWindowActive[0] ? 1 : 0, slLog.ColorWindowActive[1] ? 1 : 0,
			slLog.ColorWindowInverted[0] ? 1 : 0, slLog.ColorWindowInverted[1] ? 1 : 0,
			(int)slLog.ColorWindowMaskLogic,
			renderMode);
		ContextLog(ctxBuf);
		DiagLog(ctxBuf);
	}

	// =====================================================================
	// DIAGNOSTIC: Per-frame summary
	// =====================================================================
	bool logThisFrame = frameTotalPixels > 0 &&
		(diagFrameCount < 10 || (frameBgPixels > 0 && diagBgFrameCount < 60));
	if(logThisFrame) {
		const char* ctxLabel = isWorldmap ? "WORLDMAP" : (isLevel2 ? "LEVEL2" : "other");
		char buf[1024];
		snprintf(buf, sizeof(buf),
			"[SNES HD diag] FRAME %d/%d [%s] build=" SNES_HD_BUILD_VERSION
			": total=%u bg=%u match=%u miss=%u hdCm=%u mNat=%u sHd=%u sFix=%u lRetry=%u multi=%u"
			" sprWon=%u sprHd=%u sprSub=%u sprSubHd=%u sprHdSub=%u sprRecol=%u sprNoRef=%u mask0=%u hdmaSplit=%u ms=%.2f/%.2f"
			" BG1=%u BG2=%u BG3=%u BG4=%u"
			" hdBG1=%u hdBG2=%u hdBG3=%u hdBG4=%u"
			" wn0=%u wn1=%u wn2=%u wn3=%u"
			" Main=$%02X Sub=$%02X CM=$%02X"
			" (TileByKey=%zu, sig=%016llX)",
			diagFrameCount, diagBgFrameCount, ctxLabel,
			frameTotalPixels, frameBgPixels, frameHdMatch,
			frameHdMiss, frameHdCm, frameMainNatHd, frameSubOpHd, frameSubOpFixed, frameLayerRetry, frameMultiLayer,
			frameSpriteWon, frameSprHd, frameSprSub, frameSprSubMainHd, frameSprSubHd,
			frameSprRecolor, frameSprRecolorNoRef, frameMaskZero, frameHdmaSplit,
			filterMs, diagMsMax,
			frameLayerBits[0], frameLayerBits[1], frameLayerBits[2], frameLayerBits[3],
			frameHdLayers[0], frameHdLayers[1], frameHdLayers[2], frameHdLayers[3],
			frameWin[0], frameWin[1], frameWin[2], frameWin[3],
			slCtx.MainScreenLayers, slCtx.SubScreenLayers, slCtx.ColorMathEnabled,
			_hdData->TileByKey.size(),
			(unsigned long long)vramSig);
		DiagLog(buf);
		diagFrameCount++;
		if(frameBgPixels > 0) {
			diagBgFrameCount++;
		}

		// R3: log the active CGRAM-diff transform (first few frames per context)
		if(anyPalTransform && diagPalLogCount < 5) {
			char palBuf[512];
			int off = snprintf(palBuf, sizeof(palBuf),
				"[SNES HD diag] PALDIFF gfx=%d rows:", (int)_hdData->ActiveGfxset);
			for(int row = 0; row < 8 && off < (int)sizeof(palBuf) - 48; row++) {
				if(!palRowActive[row]) continue;
				off += snprintf(palBuf + off, sizeof(palBuf) - off,
					" P%d=%u/%u/%u", row, palRatio[row][0], palRatio[row][1], palRatio[row][2]);
			}
			DiagLog(palBuf);
			diagPalLogCount++;
		}

		// =================================================================
		// S1: HD-sprite capture summary. Counts pixels whose OBJ tile
		// identity was captured (Sprites[0]=main, Sprites[1]=sub) and the
		// number of DISTINCT sprite tiles on screen — this sizes the future
		// sprite pack and verifies hash stability. SPRTILE sample lines
		// allow offline cross-checking of runtime hashes against ROM/viewer
		// sprite data. Scan runs only on logged frames (≤70 per context).
		// =================================================================
		{
			constexpr uint32_t pixelCount = (uint32_t)SnesHdScreenInfo::ScreenPixelCount;
			std::unordered_set<uint64_t> sprHashes;
			uint32_t sprCapMain = 0, sprCapSub = 0;
			for(uint32_t i = 0; i < pixelCount; i++) {
				const SnesHdPpuPixelInfo& pi = hdScreen->ScreenTiles[i];
				if(pi.SpriteCount & 0x01) { sprCapMain++; sprHashes.insert(pi.Sprites[0].Key.ContentHash); }
				if(pi.SpriteCount & 0x02) { sprCapSub++; sprHashes.insert(pi.Sprites[1].Key.ContentHash); }
			}
			char sprBuf[192];
			snprintf(sprBuf, sizeof(sprBuf),
				"[SNES HD diag] SPRCAP main=%u sub=%u tiles=%u",
				sprCapMain, sprCapSub, (uint32_t)sprHashes.size());
			DiagLog(sprBuf);

			if(!sprHashes.empty() && diagSprCapLogCount < 3) {
				diagSprCapLogCount++;
				std::unordered_set<uint64_t> logged;
				for(uint32_t i = 0; i < pixelCount && logged.size() < 16; i++) {
					const SnesHdPpuPixelInfo& pi = hdScreen->ScreenTiles[i];
					for(int s = 0; s < 2; s++) {
						if(!(pi.SpriteCount & (1 << s))) continue;
						const SnesHdPpuTileInfo& t = pi.Sprites[s];
						if(!logged.insert(t.Key.ContentHash).second) continue;
						char tileBuf[192];
						snprintf(tileBuf, sizeof(tileBuf),
							"[SNES HD diag] SPRTILE %s hash=%016llX vram=$%04X pal=%u prio=%u hm=%d vm=%d",
							s == 0 ? "main" : "sub",
							(unsigned long long)t.Key.ContentHash, t.VramWordAddr,
							t.Key.PaletteIndex, t.Priority,
							(int)t.HorizontalMirror, (int)t.VerticalMirror);
						DiagLog(tileBuf);
						if(logged.size() >= 16) break;
					}
				}
			}
		}
	}

	// =====================================================================
	// S5a: sprite capture recording. Every distinct (contentHash, palette)
	// pair seen on screen is appended to snes_hd_spritecap.txt together
	// with the tile's 32 VRAM bytes and the OBJ palette's 16 CGRAM colors.
	// This is the ground truth the viewer's sprite pack export consumes:
	// which palette SLOT each tile really uses at runtime + reference data
	// to verify ROM-derived art. Runs every frame (~0.1 ms decode-thread
	// scan); dedup per session via in-memory set, consumers dedup across
	// sessions. VRAM is live (not a frame snapshot), so entries are only
	// recorded when the tile bytes still hash to the captured value — a
	// tile replaced by OBJ streaming mid-frame is recorded on a later one.
	// =====================================================================
	if(hdScreen->Vram) {
		static std::unordered_set<uint64_t> s_spriteCapSeen;
		static FILE* s_spriteCapFile = nullptr;
		static bool s_spriteCapAttempted = false;
		constexpr uint32_t sprCapPixels = (uint32_t)SnesHdScreenInfo::ScreenPixelCount;
		for(uint32_t i = 0; i < sprCapPixels; i++) {
			const SnesHdPpuPixelInfo& pi = hdScreen->ScreenTiles[i];
			if(!pi.SpriteCount) continue;
			for(int s = 0; s < 2; s++) {
				if(!(pi.SpriteCount & (1 << s))) continue;
				const SnesHdPpuTileInfo& t = pi.Sprites[s];
				if(t.Key.ContentHash == 0) continue;
				uint64_t setKey = t.Key.ContentHash ^ ((uint64_t)t.Key.PaletteIndex * 0x9E3779B97F4A7C15ULL);
				if(s_spriteCapSeen.find(setKey) != s_spriteCapSeen.end()) continue;
				uint64_t liveHash = ComputeTileContentHash(hdScreen->Vram, t.VramWordAddr, 16);
				if(liveHash != t.Key.ContentHash) continue;

				if(!s_spriteCapAttempted) {
						s_spriteCapAttempted = true;
						SeedRecorderSet("snes_hd_spritecap.txt", s_spriteCapSeen, ParseSpriteCapKey);
						if(s_spriteCapSeen.find(setKey) != s_spriteCapSeen.end()) continue;   // earlier session had it
						s_spriteCapFile = OpenRecorder("snes_hd_spritecap.txt");
					}
				if(!s_spriteCapFile) break;
				s_spriteCapSeen.insert(setKey);

				char line[320];
				int off = snprintf(line, sizeof(line), "SPR %016llX P%d T",
					(unsigned long long)t.Key.ContentHash, t.Key.PaletteIndex);
				const uint8_t* tileBytes = reinterpret_cast<const uint8_t*>(hdScreen->Vram + t.VramWordAddr);
				for(int b = 0; b < 32; b++) {
					off += snprintf(line + off, sizeof(line) - off, "%02X", tileBytes[b]);
				}
				off += snprintf(line + off, sizeof(line) - off, " C");
				for(int c = 0; c < 16; c++) {
					off += snprintf(line + off, sizeof(line) - off, "%04X",
						hdScreen->Cgram[128 + t.Key.PaletteIndex * 16 + c] & 0x7FFF);
				}
				fprintf(s_spriteCapFile, "%s\n", line);
			}
		}
		if(s_spriteCapFile) fflush(s_spriteCapFile);
	}

	// =====================================================================
	// S6a: BG miss recording — the data source for hash-keyed BG animation
	// tiles (and a coverage-gap map as a byproduct). Every DISTINCT
	// (hash, palette, layer) whose lookup would MISS the pack is appended
	// to snes_hd_bgcap.txt with its VRAM bytes and CGRAM colors. CHR-DMA
	// animation frames (Gusty wind foliage, water/lava cycles, flags) are
	// exactly such misses at addresses whose base tile IS covered — the
	// viewer derives per-frame art from the recorded bytes, upscales it and
	// exports it hash-keyed, so the runtime hash matching animates the art
	// with zero timing logic (same principle as sprites).
	// Cost: sampling every 8th pixel in x AND y still visits every 8x8
	// tile at least once (pigeonhole) → ~900 probes/frame, decode thread.
	// Only recorded while a gfxset is detected (skips menus/transitions).
	// =====================================================================
	if(hdScreen->Vram && _hdData->ActiveGfxset >= 0) {
		static std::unordered_set<uint64_t> s_bgCapSeen;
		static FILE* s_bgCapFile = nullptr;
		static bool s_bgCapAttempted = false;
		// A reloaded pack changes the answers: a tile that missed before may match
		// now. The dedup would keep it out of the log anyway, so "no misses left"
		// after an export would be indistinguishable from "already reported once".
		// That ambiguity cost a real verification — Lockjaw's zero could not be
		// called genuine without knowing whether the emulator had been restarted.
		{
			static const void* s_bgCapPack = nullptr;
			static size_t s_bgCapPackTiles = 0;
			if(s_bgCapPack != (const void*)_hdData || s_bgCapPackTiles != _hdData->Tiles.size()) {
				s_bgCapPack = (const void*)_hdData;
				s_bgCapPackTiles = _hdData->Tiles.size();
				s_bgCapSeen.clear();
			}
		}
		for(uint32_t sy = overscan.Top; sy < (uint32_t)(239 - overscan.Bottom); sy += 8) {
			for(uint32_t sx = 0; sx < 256; sx += 8) {
				const SnesHdPpuPixelInfo& pi = hdScreen->ScreenTiles[sy * SnesHdScreenInfo::ScreenWidth + sx];
				if(pi.BgLayerMask == 0) continue;
				for(int layer = 0; layer < 4; layer++) {
					if(!(pi.BgLayerMask & (1 << layer))) continue;
					const SnesHdPpuTileInfo& t = pi.BgTiles[layer];
					if(t.Key.ContentHash == 0) continue;
					uint64_t seenKey = t.Key.ContentHash
						^ ((uint64_t)t.Key.PaletteIndex * 0x9E3779B97F4A7C15ULL)
						^ ((uint64_t)layer * 0xC2B2AE3D27D4EB4FULL);
					if(s_bgCapSeen.find(seenKey) != s_bgCapSeen.end()) continue;
					s_bgCapSeen.insert(seenKey);
					// S14: ask the RENDER PATH whether this tile is covered, not the
					// raw map. SnesHdTileKey carries no gfxset — that lives on the
					// tile and is applied by GetMatchingTile's strict scoping — so a
					// bare TileByKey lookup counts a tile as covered even when the
					// current context blocks it and the screen shows plain SD. Those
					// tiles were invisible here: present in the pack, never matched,
					// never recorded. The sprite recorder below already does this.
					if(_hdData->GetMatchingTile(t.Key, hdScreen->Vram)) continue;  // covered

					// Verify live VRAM still matches the captured hash (2bpp
					// layers hash 8 words, 4bpp 16 words — same as the PPU)
					const uint16_t words = (layer <= 1) ? 16 : 8;
					if(ComputeTileContentHash(hdScreen->Vram, t.VramWordAddr, words) != t.Key.ContentHash) {
						s_bgCapSeen.erase(seenKey);  // retry on a later frame
						continue;
					}

					if(!s_bgCapAttempted) {
						s_bgCapAttempted = true;
						SeedRecorderSet("snes_hd_bgcap.txt", s_bgCapSeen, ParseBgCapKey);
						if(s_bgCapSeen.find(seenKey) != s_bgCapSeen.end()) continue;   // earlier session had it
						s_bgCapFile = OpenRecorder("snes_hd_bgcap.txt");
					}
					if(!s_bgCapFile) break;

					char line[320];
					int off = snprintf(line, sizeof(line), "BGA G%d L%d P%d A%04X H%016llX T",
						(int)_hdData->ActiveGfxset, layer, t.Key.PaletteIndex, t.VramWordAddr,
						(unsigned long long)t.Key.ContentHash);
					const uint8_t* tileBytes = reinterpret_cast<const uint8_t*>(hdScreen->Vram + t.VramWordAddr);
					for(int b = 0; b < words * 2; b++) {
						off += snprintf(line + off, sizeof(line) - off, "%02X", tileBytes[b]);
					}
					off += snprintf(line + off, sizeof(line) - off, " C");
					// CGRAM colors of the palette row: 4bpp = 16 colors at pal*16,
					// 2bpp (BG3 in mode 1) = 4 colors at pal*4
					const int colBase = (layer <= 1) ? t.Key.PaletteIndex * 16 : t.Key.PaletteIndex * 4;
					const int colCount = (layer <= 1) ? 16 : 4;
					for(int c = 0; c < colCount; c++) {
						off += snprintf(line + off, sizeof(line) - off, "%04X",
							hdScreen->Cgram[colBase + c] & 0x7FFF);
					}
					fprintf(s_bgCapFile, "%s\n", line);
				}
			}
		}
		if(s_bgCapFile) fflush(s_bgCapFile);
	}

	// =====================================================================
	// S14: PALETTE-animation recording — the blind spot the bgcap recorder
	// has by construction. Its dedup key is (ContentHash, palette index,
	// layer), so a tile whose CHR bytes never change and whose palette
	// INDEX never changes gets recorded exactly once, however often the
	// game rewrites the COLORS in that palette row. Blinking lights are
	// precisely that case — Krazy Kremland's sign on the hub, Swanky's
	// light strip — which is why they show up in no capture at all.
	//
	// This decides whether there is any work to do: HD tiles already track
	// live palettes through the R3 CGRAM-diff transform, so a palette-
	// animated element animates BY ITSELF once its tile has HD art and
	// needs no animation pipeline. A CHR-animated one needs the full S6b
	// chain. The two are indistinguishable by eye and, until now, in the
	// logs as well.
	//
	// Frames that rewrite MANY colors at once are fades and screen loads,
	// not animation; they are summarised on one CGF line without indices
	// so they cannot drown the few-color writes that matter.
	// =====================================================================
	// Deliberately NOT gated on ActiveGfxset, unlike the bgcap recorder above.
	// A screen with no HD art yet has no fingerprint, so ActiveGfxset is -1 and
	// a gfxset-gated recorder stays silent exactly where the question "does this
	// blink via palette or via CHR?" is still open — Swanky's shop is that case,
	// and Lost World was before it got art. Chicken and egg. The screen is keyed
	// by its VRAM signature instead, which exists regardless of coverage; the
	// gfxset is still logged when known.
	//
	// GATED SINCE S18. It answered its question (Issue U: the world map blinks by
	// palette, and R3 already follows it) and nothing reads the file since — it
	// just wrote 3 MB per capture. Kept because it is the only way to see palette
	// animation at all, but off unless asked for: set the environment variable
	//     SNES_HD_CGRAMCAP=1
	// before starting Mesen. Read once, so toggling needs a restart.
	static const bool s_cgEnabled = getenv("SNES_HD_CGRAMCAP") != nullptr;
	if(s_cgEnabled) {
		static uint16_t s_cgPrev[256] = {};
		static bool s_cgHave = false;
		static uint64_t s_cgSig = 0;
		static uint32_t s_cgLines = 0;
		static FILE* s_cgFile = nullptr;
		static bool s_cgAttempted = false;
		const int gfx = _hdData->ActiveGfxset;
		if(vramSig != s_cgSig) { s_cgSig = vramSig; s_cgHave = false; }   // new screen: re-baseline

		if(!s_cgHave) {
			memcpy(s_cgPrev, hdScreen->Cgram, sizeof(s_cgPrev));
			s_cgHave = true;
		} else if(s_cgLines < 20000) {   // ~4 MB ceiling; a blink cycle needs a handful of lines
			int changed[12];
			int nListed = 0, nTotal = 0;
			for(int i = 0; i < 256; i++) {
				if((hdScreen->Cgram[i] & 0x7FFF) == (s_cgPrev[i] & 0x7FFF)) continue;
				nTotal++;
				if(nListed < 12) changed[nListed++] = i;
			}
			if(nTotal > 0) {
				if(!s_cgAttempted) {
						s_cgAttempted = true;
						s_cgFile = OpenRecorder("snes_hd_cgramcap.txt");
					}
				if(s_cgFile) {
					if(nTotal <= 12) {
						// Index is the raw CGRAM slot: BG palettes 0x00-0x7F
						// (row = index/16 for 4bpp), sprite palettes 0x80-0xFF.
						char line[512];
						int off = snprintf(line, sizeof(line), "CGA G%d S%016llX F%u N%d",
							gfx, (unsigned long long)vramSig, hdScreen->FrameNumber, nTotal);
						for(int k = 0; k < nListed; k++) {
							off += snprintf(line + off, sizeof(line) - off, " I%02X:%04X>%04X",
								changed[k], s_cgPrev[changed[k]] & 0x7FFF,
								hdScreen->Cgram[changed[k]] & 0x7FFF);
						}
						fprintf(s_cgFile, "%s\n", line);
					} else {
						fprintf(s_cgFile, "CGF G%d S%016llX F%u N%d\n",
							gfx, (unsigned long long)vramSig, hdScreen->FrameNumber, nTotal);
					}
					s_cgLines++;
					fflush(s_cgFile);
				}
			}
			memcpy(s_cgPrev, hdScreen->Cgram, sizeof(s_cgPrev));
		}
	}

	// =====================================================================
	// S8: sprite MISS recording — diagnostic + coverage source for sprites.
	// Every DISTINCT sprite (hash, palette, main/sub slot) whose HD lookup
	// MISSES the pack is appended to snes_hd_spritemiss.txt with its 32 VRAM
	// bytes and the OBJ palette's 16 CGRAM colors (same body as spritecap),
	// prefixed with an M/S slot flag, an overlap marker and the active gfxset.
	// Two uses:
	//   (1) Coverage: the exact list of character frames still to upscale
	//       (viewer decodes the bytes, same path as bgcap animation tiles).
	//   (2) KNOWN ISSUE (underwater overlap flicker): the M/S split shows
	//       whether misses cluster on the MAIN (sprHd) or SUB (sprHdSub) path,
	//       and O1 marks pixels where a sprite is on BOTH screens but main and
	//       sub captured DIFFERENT tiles — the two-slot capture limit, prime
	//       suspect for the partial-SD flicker when the two Kongs overlap.
	// Match check uses GetMatchingTile (exact render-path decision; sprites are
	// scope-exempt). Live re-hash verify; dedup per session, consumers across.
	// =====================================================================
	if(hdScreen->Vram) {
		static std::unordered_set<uint64_t> s_sprMissSeen;
		static FILE* s_sprMissFile = nullptr;
		static bool s_sprMissAttempted = false;
		constexpr uint32_t sprMissPixels = (uint32_t)SnesHdScreenInfo::ScreenPixelCount;
		for(uint32_t i = 0; i < sprMissPixels; i++) {
			const SnesHdPpuPixelInfo& pi = hdScreen->ScreenTiles[i];
			if(!pi.SpriteCount) continue;
			// Overlap: a sprite is present on BOTH screens but the captured main
			// and sub tiles differ (two distinct sprites met at this pixel).
			bool overlap = (pi.SpriteCount & 0x03) == 0x03
				&& pi.Sprites[0].Key.ContentHash != pi.Sprites[1].Key.ContentHash;
			for(int s = 0; s < 2; s++) {
				if(!(pi.SpriteCount & (1 << s))) continue;
				const SnesHdPpuTileInfo& t = pi.Sprites[s];
				if(t.Key.ContentHash == 0) continue;
				uint64_t missKey = t.Key.ContentHash
					^ ((uint64_t)t.Key.PaletteIndex * 0x9E3779B97F4A7C15ULL)
					^ ((uint64_t)s * 0xC2B2AE3D27D4EB4FULL);
				if(s_sprMissSeen.find(missKey) != s_sprMissSeen.end()) continue;
				s_sprMissSeen.insert(missKey);
				if(_hdData->GetMatchingTile(t.Key)) continue;  // covered → not a miss
				if(ComputeTileContentHash(hdScreen->Vram, t.VramWordAddr, 16) != t.Key.ContentHash) {
					s_sprMissSeen.erase(missKey);  // stale (OBJ stream) → retry later
					continue;
				}

				if(!s_sprMissAttempted) {
						s_sprMissAttempted = true;
						SeedRecorderSet("snes_hd_spritemiss.txt", s_sprMissSeen, ParseSprMissKey);
						if(s_sprMissSeen.find(missKey) != s_sprMissSeen.end()) continue;   // earlier session had it
						s_sprMissFile = OpenRecorder("snes_hd_spritemiss.txt");
					}
				if(!s_sprMissFile) break;

				char line[320];
				int off = snprintf(line, sizeof(line), "SPRMISS %c P%d O%d G%d H%016llX T",
					s == 0 ? 'M' : 'S', t.Key.PaletteIndex, overlap ? 1 : 0,
					(int)_hdData->ActiveGfxset, (unsigned long long)t.Key.ContentHash);
				const uint8_t* tileBytes = reinterpret_cast<const uint8_t*>(hdScreen->Vram + t.VramWordAddr);
				for(int b = 0; b < 32; b++) {
					off += snprintf(line + off, sizeof(line) - off, "%02X", tileBytes[b]);
				}
				off += snprintf(line + off, sizeof(line) - off, " C");
				for(int c = 0; c < 16; c++) {
					off += snprintf(line + off, sizeof(line) - off, "%04X",
						hdScreen->Cgram[128 + t.Key.PaletteIndex * 16 + c] & 0x7FFF);
				}
				fprintf(s_sprMissFile, "%s\n", line);
			}
		}
		if(s_sprMissFile) fflush(s_sprMissFile);
	}

	// =====================================================================
	// S17: OAM recording — sprite OBJECTS as the game defines them.
	//
	// S15/S16 sampled the screen and wrote one line per tile. That records WHAT
	// was drawn but never WHAT BELONGS TOGETHER, so the viewer had to infer
	// objects from adjacency and from a window of frames. Every failure of this
	// workstream traces back to that inference: a sprite appeared or vanished
	// depending on the grouping window, objects fused with whatever stood next to
	// them, and animation phases had to be guessed from overlap.
	//
	// OAM answers all of it directly. One entry is one object (8x8 up to 64x64)
	// with its own position, size, palette and mirror flags, and the OAM index is
	// a stable identity across frames — so an animation is simply "same index,
	// different tile number", not something to be reconstructed.
	//
	// Written per FRAME, not per tile, so every record is complete on its own and
	// the window hack disappears. Volume is kept down by writing only frames whose
	// visible OAM set actually CHANGED: a still map screen costs one frame, a
	// torch one frame per phase. Tile CONTENT still comes from spritemiss, which
	// is why only the tile number is written here.
	// =====================================================================
	if(hdScreen->Oam && hdScreen->Vram) {
		static FILE* s_oamFile = nullptr;
		static bool s_oamAttempted = false;
		static uint32_t s_oamFrames = 0;
		static uint64_t s_oamPrevSig = 0;
		// Distinct object sets already on file — seeded from earlier sessions, so
		// the 20000 budget below is now a budget for NEW material rather than one
		// that refills on every restart.
		static std::unordered_set<uint64_t> s_oamSeenComp;
		if(s_oamFrames < 20000) {
			// Exactly SnesPpu::FetchSpritePosition's tables — sprites can be
			// RECTANGULAR (16x32, 32x64), so width and height must be read
			// separately. Index = OamMode | (largeFlag << 3).
			static constexpr uint8_t kOamWidth[16]  = { 8,8,8,16,16,32,16,16, 16,32,64,32,64,64,32,32 };
			static constexpr uint8_t kOamHeight[16] = { 8,8,8,16,16,32,32,32, 16,32,64,32,64,64,64,32 };
			const uint8_t* oam = hdScreen->Oam;

			struct Entry { int idx, x, y, w, h; uint16_t tile; uint8_t pal, prio; bool hm, vm; };
			Entry list[128];
			int visible = 0;
			uint64_t sig = 0;
			for(int i = 0; i < 128; i++) {
				const uint8_t* e = oam + (i << 2);
				const uint8_t hiTable = oam[0x200 | (i >> 2)] >> ((i << 1) & 0x06);
				const int x = (int)(int16_t)((hiTable & 0x01) ? (0xFF00 | e[0]) : e[0]);
				const int y = e[1];
				const uint8_t mode = (uint8_t)((hdScreen->OamMode & 0x07) | ((hiTable & 0x02) << 2));
				const int w = kOamWidth[mode], h = kOamHeight[mode];
				// Off-screen entries are how the game HIDES a sprite; counting them
				// as a change would make a still screen write on every frame.
				// Y is 8-bit and WRAPS, so a sprite is only truly invisible when it
				// sits entirely inside [240,256) without reaching past the wrap —
				// testing "below the screen" alone would drop sprites that are
				// legitimately hanging off the bottom edge.
				if(x <= -w || x >= 256) continue;
				if(y >= 240 && y + h <= 256) continue;
				const uint8_t flags = e[3];
				Entry& en = list[visible++];
				en.idx = i; en.x = x; en.y = y; en.w = w; en.h = h;
				en.tile = (uint16_t)e[2] | (((uint16_t)flags & 0x01) << 8);
				en.pal = (flags >> 1) & 0x07;
				en.prio = (flags >> 4) & 0x03;
				en.hm = (flags & 0x40) != 0;
				en.vm = (flags & 0x80) != 0;
				sig = (sig * 0x100000001B3ULL)
					^ ((uint64_t)i << 40) ^ ((uint64_t)en.tile << 24)
					^ ((uint64_t)(x & 0x1FF) << 12) ^ ((uint64_t)y << 3) ^ (uint64_t)en.pal;
			}

			if(visible > 0 && sig != s_oamPrevSig) {
				s_oamPrevSig = sig;

				// Render the frame's entries first, then decide whether it is worth
				// keeping. The position-based gate above only says "something moved";
				// the composition fold below says whether we have this OBJECT SET
				// already, which is what the viewer actually consumes.
				static std::vector<std::string> s_oamLines;
				s_oamLines.clear();
				uint64_t compSig = 0xCBF29CE484222325ULL;
				char lineBuf[4096];
				for(int n = 0; n < visible; n++) {
					const Entry& en = list[n];
					// A W x H sprite occupies (W/8) x (H/8) tiles laid out in the
					// 16x16 name table, wrapping within the row — the same walk
					// FetchSpriteAttributes does. The hashes are written out so the
					// viewer can find the pixels in spritemiss; it has no VRAM.
					int off = snprintf(lineBuf, sizeof(lineBuf),
						"OAM I%03d X%+04d Y%03d W%02d H%02d T%03X P%d R%d %c%c",
						en.idx, en.x, en.y, en.w, en.h, en.tile, en.pal, en.prio,
						en.hm ? 'H' : '-', en.vm ? 'V' : '-');
					const int baseRow = (en.tile & 0xFF) >> 4;
					const int baseCol = en.tile & 0x0F;
					const bool second = (en.tile & 0x100) != 0;
					for(int dy = 0; dy < en.h / 8 && off < (int)sizeof(lineBuf) - 20; dy++) {
						for(int dx = 0; dx < en.w / 8 && off < (int)sizeof(lineBuf) - 20; dx++) {
							const uint8_t idx = (uint8_t)((((baseRow + dy) & 0x0F) << 4)
								| ((baseCol + dx) & 0x0F));
							const uint16_t vaddr = (uint16_t)((hdScreen->OamBaseAddress
								+ ((uint16_t)idx << 4)
								+ (second ? hdScreen->OamAddressOffset : 0)) & 0x7FFF);
							off += snprintf(lineBuf + off, sizeof(lineBuf) - off, " %016llX",
								(unsigned long long)ComputeTileContentHash(hdScreen->Vram, vaddr, 16));
						}
					}
					compSig = (compSig ^ OamCompositionSig(lineBuf)) * 0x100000001B3ULL;
					s_oamLines.push_back(lineBuf);
				}

				if(!s_oamAttempted) {
					s_oamAttempted = true;
					SeedOamFrameSigs(s_oamSeenComp);
					s_oamFile = OpenRecorder("snes_hd_oam.txt");
				}
				if(s_oamFile && s_oamSeenComp.insert(compSig).second) {
					fprintf(s_oamFile, "OAMF G%d S%016llX F%u M%d N%d\n",
						(int)_hdData->ActiveGfxset, (unsigned long long)vramSig,
						hdScreen->FrameNumber, hdScreen->OamMode, visible);
					for(const std::string& l : s_oamLines) {
						fprintf(s_oamFile, "%s\n", l.c_str());
					}
					s_oamFrames++;
					fflush(s_oamFile);
				}
			}
		}
	}

	// =====================================================================
	// DIAGNOSTIC: Per-scanline HDMA dump (once per context, first BG frame)
	// Shows scanline ranges where MainScreenLayers/Sub/CM/FixedColor change.
	// =====================================================================
	if(logThisFrame && frameBgPixels > 0 && frameHdmaSplit > 0 && !hdmaDumped) {
		hdmaDumped = true;
		DiagLog("[SNES HD diag] HDMA SCANLINE DUMP (first HDMA frame):");
		uint8_t prevMain = 0xFF, prevSub = 0xFF, prevCM = 0xFF;
		uint16_t prevFixed = 0xFFFF;
		uint8_t prevBr = 0xFF;
		uint32_t rangeStart = overscan.Top;
		for(uint32_t sy = overscan.Top; sy <= 239 - overscan.Bottom; sy++) {
			SnesHdScanlineInfo& sli = hdScreen->ScanlineInfo[sy];
			bool last = (sy == 239 - overscan.Bottom);
			bool changed = (sli.MainScreenLayers != prevMain ||
				sli.SubScreenLayers != prevSub ||
				sli.ColorMathEnabled != prevCM ||
				sli.FixedColor != prevFixed ||
				sli.ScreenBrightness != prevBr);
			if((changed || last) && prevMain != 0xFF) {
				uint32_t rangeEnd = changed ? sy - 1 : sy;
				char slBuf[256];
				snprintf(slBuf, sizeof(slBuf),
					"  Y %3u-%3u: Main=$%02X Sub=$%02X CM=$%02X AddSub=%d Fixed=$%04X Br=%d",
					rangeStart, rangeEnd, prevMain, prevSub, prevCM,
					hdScreen->ScanlineInfo[rangeStart].ColorMathAddSubscreen ? 1 : 0,
					prevFixed, prevBr);
				DiagLog(slBuf);
				rangeStart = sy;
			}
			prevMain = sli.MainScreenLayers;
			prevSub = sli.SubScreenLayers;
			prevCM = sli.ColorMathEnabled;
			prevFixed = sli.FixedColor;
			prevBr = sli.ScreenBrightness;
		}
	}
	// Reset HDMA dump flag on context change (handled by diagLoggedHashes clear)
	if(logThisFrame && frameHdmaSplit == 0) {
		hdmaDumped = false;
	}

	// =====================================================================
	// DIAGNOSTIC: LEVEL ANALYSIS — once per context, after first BG frame
	// Human-readable breakdown of how PPU layers are configured and how
	// our HD engine treats them. Helps understand level structure at a glance.
	// =====================================================================
	static bool levelAnalysisDone = false;
	if(diagContextChanged) levelAnalysisDone = false;

	if(!levelAnalysisDone && frameBgPixels > 1000 && logThisFrame) {
		levelAnalysisDone = true;
		SnesHdScanlineInfo& sa = hdScreen->ScanlineInfo[120];
		const char* ctxLabel = isWorldmap ? "WORLDMAP" : (isLevel2 ? "LEVEL2" : "other");

		DiagLog(""); // blank line for readability
		char hdr[256];
		snprintf(hdr, sizeof(hdr),
			"[SNES HD diag] ========== LEVEL ANALYSIS (sig=%016llX, %s) ==========",
			(unsigned long long)vramSig, ctxLabel);
		DiagLog(hdr);

		// --- Per-layer breakdown ---
		const char* layerNames[] = {"BG1", "BG2", "BG3", "BG4"};
		for(int L = 0; L < 4; L++) {
			bool onMain = (sa.MainScreenLayers & (1 << L)) != 0;
			bool onSub  = (sa.SubScreenLayers & (1 << L)) != 0;
			bool cmEnabled = (sa.ColorMathEnabled & (1 << L)) != 0;

			// Skip layers with zero presence
			if(!onMain && !onSub && frameLayerBits[L] == 0) continue;

			// Compute HD match rate for this layer
			int matchPct = (frameLayerBits[L] > 0)
				? (int)(100ULL * frameHdLayers[L] / frameLayerBits[L]) : 0;

			// Determine role
			const char* role = "inactive";
			if(onMain && !cmEnabled) role = "MAIN (no CM)";
			else if(onMain && cmEnabled && sa.ColorMathAddSubscreen) role = "MAIN + CM (add subscreen)";
			else if(onMain && cmEnabled && !sa.ColorMathAddSubscreen) role = "MAIN + CM (fixed color)";
			else if(!onMain && onSub) role = "SUB-SCREEN only (CM operand)";

			char layBuf[512];
			snprintf(layBuf, sizeof(layBuf),
				"  %s: %s | pixels=%u win=%u hdMatch=%u (%d%%) | onMain=%d onSub=%d cmEnabled=%d",
				layerNames[L], role,
				frameLayerBits[L], frameWin[L], frameHdLayers[L], matchPct,
				onMain ? 1 : 0, onSub ? 1 : 0, cmEnabled ? 1 : 0);
			DiagLog(layBuf);
		}

		// --- OBJ/Sprite info ---
		bool objOnMain = (sa.MainScreenLayers & 0x10) != 0;
		bool objOnSub  = (sa.SubScreenLayers & 0x10) != 0;
		bool objCm     = (sa.ColorMathEnabled & 0x10) != 0;
		char objBuf[256];
		snprintf(objBuf, sizeof(objBuf),
			"  OBJ: onMain=%d onSub=%d cmEnabled=%d | spriteWonPixels=%u",
			objOnMain ? 1 : 0, objOnSub ? 1 : 0, objCm ? 1 : 0, frameSpriteWon);
		DiagLog(objBuf);

		// --- Backdrop ---
		bool bdropCm = (sa.ColorMathEnabled & 0x20) != 0;
		char bdBuf[128];
		snprintf(bdBuf, sizeof(bdBuf),
			"  Backdrop: cmEnabled=%d | emptyPixels=%u",
			bdropCm ? 1 : 0, frameMaskZero);
		DiagLog(bdBuf);

		// --- Rendering summary ---
		char sumBuf[512];
		snprintf(sumBuf, sizeof(sumBuf),
			"  Mode=%d | Bg3Priority=%d | AddSubscreen=%d | Subtract=%d | Halve=%d\n"
			"  FixedColor=$%04X | Brightness=%d | HDMA=%s\n"
			"  ClipMode=%d PreventMode=%d | Window1=[%d,%d] Window2=[%d,%d]\n"
			"  ColorWindow: active=[%d,%d] inv=[%d,%d] logic=%d\n"
			"  HD engine: match=%u miss=%u mainNatHdSub=%u subOpHd=%u subOpFixed=%u layerRetry=%u multiLayer=%u cmApplied=%u",
			sa.BgMode, sa.Mode1Bg3Priority ? 1 : 0,
			sa.ColorMathAddSubscreen ? 1 : 0, sa.ColorMathSubtractMode ? 1 : 0,
			sa.ColorMathHalveResult ? 1 : 0,
			sa.FixedColor, sa.ScreenBrightness,
			frameHdmaSplit > 0 ? "YES (per-scanline changes)" : "no",
			(int)sa.ColorMathClipMode, (int)sa.ColorMathPreventMode,
			sa.Window1Left, sa.Window1Right, sa.Window2Left, sa.Window2Right,
			sa.ColorWindowActive[0] ? 1 : 0, sa.ColorWindowActive[1] ? 1 : 0,
			sa.ColorWindowInverted[0] ? 1 : 0, sa.ColorWindowInverted[1] ? 1 : 0,
			(int)sa.ColorWindowMaskLogic,
			frameHdMatch, frameHdMiss, frameMainNatHd, frameSubOpHd, frameSubOpFixed, frameLayerRetry, frameMultiLayer, frameHdCm);
		DiagLog(sumBuf);

		// --- Compositing explanation in plain language ---
		// Helps understand what the PPU is actually doing
		DiagLog("  --- Compositing pipeline (PPU logic) ---");

		// Which layer wins main screen?
		uint8_t mainLayers = sa.MainScreenLayers & 0x0F;
		if(mainLayers == 0x01) DiagLog("  Main screen: BG1 only → all BG winners are BG1");
		else if(mainLayers == 0x04) DiagLog("  Main screen: BG3 only → BG3 overlay mode");
		else if(mainLayers == 0x03) DiagLog("  Main screen: BG1+BG2 → standard priority compositing");
		else if(mainLayers == 0x07) DiagLog("  Main screen: BG1+BG2+BG3 → all layers composited");
		else if(mainLayers == 0x17 || mainLayers == 0x13) DiagLog("  Main screen: multiple layers + OBJ");
		else {
			char mlBuf[128];
			snprintf(mlBuf, sizeof(mlBuf), "  Main screen: layers=$%02X", mainLayers);
			DiagLog(mlBuf);
		}

		// What provides the CM operand?
		if(sa.ColorMathAddSubscreen) {
			uint8_t subLayers = sa.SubScreenLayers & 0x1F;
			char subBuf[256];
			snprintf(subBuf, sizeof(subBuf),
				"  CM operand: Sub-screen (layers=$%02X) → color added to main pixel",
				subLayers);
			DiagLog(subBuf);
		} else if(sa.FixedColor != 0) {
			char fcBuf[128];
			snprintf(fcBuf, sizeof(fcBuf),
				"  CM operand: Fixed color=$%04X (R=%d G=%d B=%d)",
				sa.FixedColor, sa.FixedColor & 0x1F,
				(sa.FixedColor >> 5) & 0x1F, (sa.FixedColor >> 10) & 0x1F);
			DiagLog(fcBuf);
		} else {
			DiagLog("  CM operand: none (FixedColor=0, AddSubscreen=0)");
		}

		// What gets color math?
		uint8_t cmLayers = sa.ColorMathEnabled & 0x3F;
		if(cmLayers) {
			char cmBuf[256];
			snprintf(cmBuf, sizeof(cmBuf),
				"  CM applied to: %s%s%s%s%s%s (register=$%02X, %s mode)",
				(cmLayers & 0x01) ? "BG1 " : "",
				(cmLayers & 0x02) ? "BG2 " : "",
				(cmLayers & 0x04) ? "BG3 " : "",
				(cmLayers & 0x08) ? "BG4 " : "",
				(cmLayers & 0x10) ? "OBJ " : "",
				(cmLayers & 0x20) ? "Backdrop " : "",
				sa.ColorMathEnabled,
				sa.ColorMathSubtractMode ? "SUBTRACT" : "ADD");
			DiagLog(cmBuf);
		}

		DiagLog("[SNES HD diag] ========== END LEVEL ANALYSIS ==========");
		DiagLog("");
	}

	// Log build version once at startup
	static bool buildVersionLogged = false;
	if(!buildVersionLogged) {
		char buf[128];
		snprintf(buf, sizeof(buf), "[SNES HD diag] Build version: " SNES_HD_BUILD_VERSION);
		DiagLog(buf);
		buildVersionLogged = true;
	}
}
