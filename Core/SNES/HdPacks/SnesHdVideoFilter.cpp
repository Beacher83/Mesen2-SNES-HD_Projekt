#include "pch.h"
#include "SNES/HdPacks/SnesHdVideoFilter.h"
#include "SNES/HdPacks/SnesHdPerf.h"
#include "SNES/SnesConsole.h"
#include "Shared/Emulator.h"
#include "Shared/EmuSettings.h"
#include "Shared/Video/BaseVideoFilter.h"
#include "Shared/ColorUtilities.h"
#include "Shared/MessageManager.h"
#include "Utilities/PNGHelper.h"
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
#include <unordered_map>
#include <vector>
#ifdef _MSC_VER
#include <intrin.h>
#endif

// Build version — logged in diagnostics so test PC can verify correct code is running.
#define SNES_HD_BUILD_VERSION "S54"

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

// S25: one line that says WHICH build and WHICH A/B switches produced the block of
// log below it. Needed the moment more than one variant is tested in a sitting: the
// diagnostics file used to be truncated on every start, so four comparison runs left
// exactly one log — and even appended, four blocks are indistinguishable unless the
// switches are written down. The switch names are spelled out here rather than read
// from the statics further down, because those are declared after this point.
// S27b: ALWAYS append, and bound the size by rotating instead of truncating.
//
// The first attempt made "fresh on every start" the default and put appending behind
// a switch that the TEST_*.bat files set. It destroyed a test series within the hour:
// a run started any other way — and a plain reference run is exactly that — wiped the
// two comparison runs before it. A default that silently deletes the previous run's
// evidence is the wrong default, whatever the switch says.
//
// So: never truncate. Once the file passes the cap it is moved aside to <name>_old
// and a new one begins, which bounds the whole thing at twice the cap while the
// current series is never the thing that gets thrown away. These two logs are small
// (diag ~350 KB per run, context ~2 KB); they were never the disk problem — the OAM
// recorder was, and that one is now off by default.
static constexpr long kLogRotateBytes = 16L * 1024 * 1024;

static void RotateLogIfLarge(const char* path)
{
	FILE* f = fopen(path, "rb");
	if(!f) return;
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fclose(f);
	if(size < kLogRotateBytes) return;

	char oldPath[600];
	snprintf(oldPath, sizeof(oldPath), "%s.old", path);
	remove(oldPath);            // rename() will not overwrite on Windows
	rename(path, oldPath);
}

static void WriteSessionBanner(FILE* f, const char* what)
{
	static const char* const switchNames[] = {
		"SNES_HD_NO_SPRITE_EDGES",
		"SNES_HD_NO_SPRITE_UNDER",
		"SNES_HD_NO_SUB_UNDER",
		"SNES_HD_NO_SUB_SPRITE_UNDER",
		"SNES_HD_NO_SPRITE_RECOLOR",
		"SNES_HD_CGRAMCAP",
		"SNES_HD_OAMCAP",
		"SNES_HD_DIAG_FRAMES",
		"SNES_HD_NO_BACKDROP_GROUND",
		"SNES_HD_NO_SUB_BG_UNDER",
		"SNES_HD_SPRWATCH",
		"SNES_HD_NO_OAM_TIEBREAK",   // S49
		"SNES_HD_NO_PAL_TRANSFORM",  // S50
		"SNES_HD_NO_SUB_HD_OPERAND", // S50
		"SNES_HD_NO_BG_RECOLOR",     // S52
		"SNES_HD_OLD_BG_PAL_ROWS",   // S53b
		"SNES_HD_NO_RECOLOR_GRID",   // S54
		"SNES_HD_PAINT_LAYERS",      // S47
		"SNES_HD_DUMP_FRAMES",       // S48
	};
	char active[512];
	active[0] = 0;
	for(const char* name : switchNames) {
		if(getenv(name)) {
			if(active[0]) strncat(active, " ", sizeof(active) - strlen(active) - 1);
			strncat(active, name, sizeof(active) - strlen(active) - 1);
		}
	}
	time_t now = time(nullptr);
	struct tm lt {};
#ifdef _WIN32
	localtime_s(&lt, &now);
#else
	localtime_r(&now, &lt);
#endif
	fprintf(f, "\n=== SESSION %04d-%02d-%02d %02d:%02d:%02d build=" SNES_HD_BUILD_VERSION
		" | %s | A/B: %s ===\n",
		lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, lt.tm_hour, lt.tm_min, lt.tm_sec,
		what, active[0] ? active : "(keine - Referenzlauf)");
	fflush(f);
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
			// S25/S27b: append, rotating at the cap. Before S25 this was truncated on
			// every start, which made a series of A/B runs impossible.
			RotateLogIfLarge(path);
			diagFile = fopen(path, "a");
			if(diagFile) {
				WriteSessionBanner(diagFile, "Diagnostics");
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
			// S25/S27b: same handling as the diagnostics file above.
			RotateLogIfLarge(path);
			ctxFile = fopen(path, "a");
			if(ctxFile) {
				WriteSessionBanner(ctxFile, "Context (register snapshot from scanline 120)");
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
// S54: Index des niedrigsten gesetzten Bits (mask != 0).
static inline int BitScanLowest(uint32_t mask)
{
#ifdef _MSC_VER
	unsigned long i;
	_BitScanForward(&i, mask);
	return (int)i;
#else
	return __builtin_ctz(mask);
#endif
}

struct HdSpriteRecolor
{
	bool valid = false;
	// S53: wie viele Farben die Palette hat -- 16 bei 4bpp und Sprites, 4 bei
	// 2bpp. Apply() sucht nur ueber Eintraege, die die Kachel wirklich haben kann.
	uint8_t count = 16;
	uint8_t refR[16] = {}, refG[16] = {}, refB[16] = {};
	int16_t dR[16] = {}, dG[16] = {}, dB[16] = {};
	// S54: optionales Kandidatengitter, siehe BuildRecolorGrid(). nullptr = volle Suche.
	const uint16_t* grid = nullptr;

	void Init(const uint16_t* refPal, const uint16_t* liveRow, int n = 16)
	{
		valid = false;
		count = (uint8_t)n;
		if(!refPal || !liveRow) {
			return;
		}
		bool anyDelta = false;
		for(int i = 0; i < n; i++) {
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
		if(grid) {
			// S54: nur die Eintraege, die in diesem 4x4x4-Wuerfel ueberhaupt die
			// naechsten sein koennen, in aufsteigender Reihenfolge -- dasselbe
			// Ergebnis wie die volle Suche, auch bei Gleichstand.
			uint32_t mask = grid[((r >> 2) << 12) | ((g >> 2) << 6) | (b >> 2)];
			if((mask & (mask - 1)) == 0) {
				best = BitScanLowest(mask);
			} else {
				while(mask) {
					const int i = BitScanLowest(mask);
					mask &= mask - 1;
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
			}
		} else {
			for(int i = 1; i < count; i++) {
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

// S54: Kandidatengitter fuer HdSpriteRecolor::Apply.
//
// Apply() sucht je Texel die naechste von 15 Referenzfarben. In Glimmer's Galleon,
// wo die Live-Palette die Umkehrung der Referenz ist und jede Zeile umgefaerbt
// wird, kostete das den Filter 16,7 ms Median je Frame -- das ganze Budget (Log
// 25.09., Kontext E10E4686, Main=$04). Mit SNES_HD_NO_BG_RECOLOR lief es fluessig.
//
// Das Gitter teilt den RGB-Raum in 64x64x64 Wuerfel zu 4x4x4 Werten und merkt sich
// je Wuerfel als Bitmaske, welche Eintraege dort UEBERHAUPT die naechsten sein
// koennen: Eintrag i fliegt nur raus, wenn sein kleinster Abstand zum Wuerfel
// groesser ist als der groesste Abstand irgendeines anderen Eintrags. Damit ist
// das Ergebnis EXAKT das der vollen Suche, auch bei Gleichstand, weil die Maske
// in aufsteigender Reihenfolge durchlaufen wird. (Die 2026-09-23 verworfene
// 5-Bit-Tabelle hat dagegen gerundet -- 6,74 % falsche Texel.)
//
// Gemessen an der Pack-Kunst (BG1/BG2), Kandidaten je Texel im Mittel:
// gfxset 3 -> 1,89 (38,9 % sofort eindeutig), 7 -> 1,21, 37 -> 1,22, 38 -> 1,35.
// Statt 14-15 Vergleichen also ein bis zwei.
//
// Das Gitter haengt nur an der REFERENZ (palettes.bin), nicht an der Live-Palette,
// und wird deshalb einmal je gfxset und 4bpp-Zeile gebaut (512 KB, ~10 ms) und
// gemerkt. 2bpp-Bloecke (3 Farben) brauchen keins. Beim Bau rechnet eine
// Stichprobe gegen die volle Suche und schreibt das Ergebnis ins Diagnose-Log.
//
// SNES_HD_NO_RECOLOR_GRID=1 schaltet zurueck auf die volle Suche (A/B).
static const bool s_noRecolorGrid = getenv("SNES_HD_NO_RECOLOR_GRID") != nullptr;
static constexpr int kRecolorGridSide = 64;

static int RecolorNearestFull(const HdSpriteRecolor& rc, int r, int g, int b)
{
	int best = 1, bestD = 0x7FFFFFFF;
	for(int i = 1; i < rc.count; i++) {
		int er = r - (int)rc.refR[i], eg = g - (int)rc.refG[i], eb = b - (int)rc.refB[i];
		int d = er * er + eg * eg + eb * eb;
		if(d < bestD) {
			bestD = d;
			best = i;
			if(d == 0) {
				break;
			}
		}
	}
	return best;
}

static void BuildRecolorGrid(const HdSpriteRecolor& rc, std::vector<uint16_t>& grid)
{
	const int n = kRecolorGridSide;
	const int cnt = rc.count;
	// Je Kanal, Eintrag und Wuerfelzeile: kleinster und groesster quadrierter Abstand.
	static int mn[3][16][kRecolorGridSide], mx[3][16][kRecolorGridSide];
	const uint8_t* ref[3] = { rc.refR, rc.refG, rc.refB };
	for(int ch = 0; ch < 3; ch++) {
		for(int i = 1; i < cnt; i++) {
			const int v = ref[ch][i];
			for(int c = 0; c < n; c++) {
				const int lo = c * 4, hi = lo + 3;
				const int dmin = v < lo ? lo - v : (v > hi ? v - hi : 0);
				const int dmax = std::max(std::abs(v - lo), std::abs(v - hi));
				mn[ch][i][c] = dmin * dmin;
				mx[ch][i][c] = dmax * dmax;
			}
		}
	}
	grid.assign((size_t)n * n * n, 0);
	for(int cr = 0; cr < n; cr++) {
		for(int cg = 0; cg < n; cg++) {
			uint16_t* out = grid.data() + (((size_t)cr << 12) | ((size_t)cg << 6));
			for(int cb = 0; cb < n; cb++) {
				int bound = 0x7FFFFFFF;
				for(int i = 1; i < cnt; i++) {
					bound = std::min(bound, mx[0][i][cr] + mx[1][i][cg] + mx[2][i][cb]);
				}
				uint16_t mask = 0;
				for(int i = 1; i < cnt; i++) {
					if(mn[0][i][cr] + mn[1][i][cg] + mn[2][i][cb] <= bound) {
						mask |= (uint16_t)(1u << i);
					}
				}
				out[cb] = mask;
			}
		}
	}
}

struct RecolorGridEntry
{
	uint8_t refR[16], refG[16], refB[16];
	std::vector<uint16_t> grid;
};
static std::unordered_map<uint32_t, RecolorGridEntry> s_recolorGrids;

// Nur aus ApplyFilter() vor dem Start der Render-Threads aufrufen; danach wird
// das Gitter nur noch gelesen.
static const uint16_t* GetRecolorGrid(int gfxset, int block, const HdSpriteRecolor& rc)
{
	const uint32_t key = ((uint32_t)(gfxset & 0xFF) << 8) | (uint32_t)block;
	auto it = s_recolorGrids.find(key);
	if(it != s_recolorGrids.end()) {
		const RecolorGridEntry& e = it->second;
		// Ein neu geladenes Pack kann andere Referenzfarben bringen.
		if(!memcmp(e.refR, rc.refR, 16) && !memcmp(e.refG, rc.refG, 16) && !memcmp(e.refB, rc.refB, 16)) {
			return e.grid.data();
		}
	}
	if(s_recolorGrids.size() >= 64) {
		s_recolorGrids.clear();   // 32 MB Obergrenze; ein Level braucht selten mehr als 8
	}
	auto t0 = std::chrono::high_resolution_clock::now();
	RecolorGridEntry& e = s_recolorGrids[key];
	memcpy(e.refR, rc.refR, 16);
	memcpy(e.refG, rc.refG, 16);
	memcpy(e.refB, rc.refB, 16);
	BuildRecolorGrid(rc, e.grid);
	double ms = std::chrono::duration<double, std::milli>(
		std::chrono::high_resolution_clock::now() - t0).count();

	// Selbstpruefung: Stichprobe gegen die volle Suche.
	HdSpriteRecolor probe = rc;
	probe.grid = nullptr;
	uint32_t seed = 0x9E3779B9u ^ key;
	int mismatches = 0;
	uint64_t candSum = 0;
	const int samples = 8192;
	for(int k = 0; k < samples; k++) {
		seed = seed * 1664525u + 1013904223u;
		const int r = (seed >> 8) & 0xFF, g = (seed >> 16) & 0xFF, b = (seed >> 24) & 0xFF;
		const uint16_t mask = e.grid[((r >> 2) << 12) | ((g >> 2) << 6) | (b >> 2)];
		int best = 1, bestD = 0x7FFFFFFF;
		for(uint32_t m = mask; m; m &= m - 1) {
			const int i = BitScanLowest(m);
			int er = r - (int)rc.refR[i], eg = g - (int)rc.refG[i], eb = b - (int)rc.refB[i];
			int d = er * er + eg * eg + eb * eb;
			if(d < bestD) {
				bestD = d;
				best = i;
				if(d == 0) break;
			}
			candSum++;
		}
		if(best != RecolorNearestFull(probe, r, g, b)) {
			mismatches++;
		}
	}
	char buf[256];
	snprintf(buf, sizeof(buf),
		"[SNES HD diag] RECOLOR-GRID gfx=%d P%d gebaut in %.1f ms, Stichprobe %d: %d Abweichungen, "
		"Kandidaten im Mittel %.2f (volle Suche %d)",
		gfxset, block, ms, samples, mismatches, (double)candSum / samples, rc.count - 1);
	DiagLog(buf);
	return e.grid.data();
}

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
	// S44: sprite edge smoothing, read once per frame from SnesConfig in
	// ApplyFilter. It rides in the frame context instead of a file-scope
	// static because the render threads take this struct by const ref --
	// a setting read inside the pixel loop would be both a data race and
	// 57,344 redundant lookups. Changing the checkbox takes effect on the
	// NEXT frame: nothing about the edge paths is baked at load time, the
	// pack tiles keep their alpha either way, and the PPU fills slot 2
	// regardless of the setting.
	bool smoothEdges = true;
	// M5.7's worldmap lockout, narrowed. Blocking BG HD on the worldmap was only
	// ever a stand-in for scoping: back then any level tile could match a map tile
	// by hash alone. P4.2's strict gfxset scoping does that job properly, and the
	// maps now ship their own gfxsets and fingerprints, so the lockout would just
	// keep their own art from rendering. It still applies when a pack has no
	// fingerprints at all, where strict scoping cannot help.
	bool blockBgHd = false;
	uint64_t vramSig = 0;
	bool anyPalTransform = false;
	// S53: indiziert ueber BgPalBlock(), nicht mehr ueber die Palettennummer.
	const bool* palRowActive = nullptr;        // [BgPalBlockCount]
	const uint8_t (*palLut)[3][256] = nullptr; // [BgPalBlockCount][3][256]
	// S52: je BG-Palettenblock einmal vorberechnet; nullptr = nicht aktiv.
	const void* bgRecolor = nullptr;            // HdSpriteRecolor[BgPalBlockCount]
	const bool* bgRecolorActive = nullptr;      // [BgPalBlockCount]
};

// Per-thread frame counters, summed after all threads joined.
// S21 A/B switch: set SNES_HD_NO_SPRITE_EDGES=1 before starting Mesen to render
// sprite edges exactly as S20 did — native silhouette, partial alpha blended
// against the sprite's own SD colour. Read once, so toggling needs a restart.
// Both halves of the edge work hang off this one flag, so a single comparison
// run answers whether it helps.
static const bool s_noSpriteEdges = getenv("SNES_HD_NO_SPRITE_EDGES") != nullptr;
// S22 A/B: ignore slot 3, so a soft sprite edge never blends against the sprite
// behind it (wall 2 falls back to the BG search, i.e. S21 behaviour). Separate
// from the switch above because S22 and S23 landed together and their effects
// have to be told apart in a running build rather than argued about:
//     set SNES_HD_NO_SPRITE_UNDER=1
static const bool s_noSpriteUnder = getenv("SNES_HD_NO_SPRITE_UNDER") != nullptr;
// S26 A/B: do not give a sub-screen sprite a ground to blend against, i.e. keep
// S24's behaviour where a semi-transparent texel mixes with the sprite's own SD
// colour. Separate switch because S25 and S26 ship together and a single run has
// to be able to tell them apart:
//     set SNES_HD_NO_SUB_UNDER=1
static const bool s_noSubUnder = getenv("SNES_HD_NO_SUB_UNDER") != nullptr;
// S27 A/B: in the sub-operand path, do not use a second SPRITE as the ground — fall
// straight to the BG search, i.e. S26 behaviour. The user saw the remaining case on
// 08 Sep: under water, with the Kongs one behind the other, the outline goes hard
// again. That is the `!SubScreenHasSprite` limit S25 was deliberately kept inside:
//     set SNES_HD_NO_SUB_SPRITE_UNDER=1
static const bool s_noSubSprUnder = getenv("SNES_HD_NO_SUB_SPRITE_UNDER") != nullptr;
// S32: when the sub screen carries no BG behind a sub-screen sprite, blend the sprite's
// soft edge against the BACKDROP -- which is what the sub screen actually outputs there.
// On by default; this turns it off for an A/B run:
//     set SNES_HD_NO_BACKDROP_GROUND=1
static const bool s_noBackdropGround = getenv("SNES_HD_NO_BACKDROP_GROUND") != nullptr;

// S47: Ebenen-Anstrich. Jeder BG-Gewinnerpixel wird flaechig eingefaerbt --
// BG1 rot, BG2 gruen, BG3 blau, BG4 gelb -- Sprites bleiben unberuehrt.
//
// Warum: am 23.09. sagte die Messung, die HD-Kunst von BG3 werde an 26.886 Pixeln
// je Frame in den Ausgabepuffer geschrieben (47 % des Schirms), und der User sah
// beim Entfernen derselben Kunst KEINE Aenderung -- nachweislich, mit Quittung
// (TileByKey 56515 -> 56294, miss 0 -> 24.584, hdBG3 -> 0). Beide Messungen sind
// sauber, beide koennen nicht stimmen. Dazwischen steht eine Frage, die kein
// Zaehler beantwortet: WO auf dem Schirm liegen diese Pixel? Ein Bild davon
// schlaegt jede weitere Zahl.
//
// Mit SNES_HD_PAINT_LAYERS=1 einschalten. Reiner Diagnosepfad.
static const bool s_paintLayers = getenv("SNES_HD_PAINT_LAYERS") != nullptr;

// S49: force-off for the OAM tie-break below, so the old behaviour can be put
// back in the same session without a rebuild -- the pattern S42/S44 used.
static const bool s_noOamTiebreak = getenv("SNES_HD_NO_OAM_TIEBREAK") != nullptr;

// S50 A/B, zwei Schalter fuer EINEN Befund: Glimmer's Galleon wird mit HD-Pack
// als Negativ dargestellt, ohne Pack ist es korrekt (vom User belegt, 23.09.).
//
// Die Lage dort: Main=$04 (nur BG3), Sub=$13 (BG1+BG2+OBJ), CM=$24, AddSub=1,
// SubtractMode=1. Das Level verdunkelt per SUBTRAKTION -- die PPU rechnet
// main - sub, und der Main-Screen ist hell (gemessen (24,22,24)). Wird der
// Subtrahend zu klein, kommt das Bild zu hell heraus.
//
// Genau das ist der Verdacht: an 53.839 Pixeln je Frame stammt der Operand aus
// HD-Kunst (sHd), und die laeuft durch den R3-Palettentransform. Dessen
// Verstaerkung ist bei 4x GEDECKELT (:2382), und in diesem Level stehen fuenf
// von acht Palettenzeilen im Blaukanal am Anschlag (PALDIFF: P1=1024/1024/1024).
// Die Live-Palette liegt bei ~390 von maximal 465, die im Pack gebackene
// Referenz bei 141-214 -- noetig waeren Faktoren ueber 4. Die HD-Kunst bleibt
// also zu dunkel, es wird zu wenig subtrahiert.
//
//   SNES_HD_NO_PAL_TRANSFORM=1     R3 ganz aus -- Kunst behaelt ihre gebackenen
//                                  Farben. Klaert, ob der LUT die Ursache ist.
//   SNES_HD_NO_SUB_HD_OPERAND=1    Farbmath-Operand immer nativ, also exakt das,
//                                  was die PPU subtrahiert. Klaert, ob der Pfad
//                                  als Ganzes die Ursache ist.
//
// Beide sind reine Diagnose. Wird das Bild mit einem davon richtig, ist die
// Ursache eingegrenzt, ohne dass irgendetwas geraten werden musste.
static const bool s_noPalTransform = getenv("SNES_HD_NO_PAL_TRANSFORM") != nullptr;
static const bool s_noSubHdOperand = getenv("SNES_HD_NO_SUB_HD_OPERAND") != nullptr;

// S52: BG-Kacheln bekommen dasselbe Umfaerben wie Sprites.
//
// Warum R3 nicht reicht, gemessen in Glimmer's Galleon (S51): die Live-Palette
// ist dort die UMKEHRUNG der im Pack gebackenen -- Korrelation r = -0,99 in
// allen drei Kanaelen, affin mit LIVE ~ (31,0|28,4|30,0) - (0,50|0,75|0,50)*REF.
// R3 modelliert `live = ref * gain`: rein multiplikativ, Faktor positiv, bei 4x
// gedeckelt. Ein Versatz mit negativer Steigung laesst sich damit durch keine
// Parametrisierung ausdruecken -- deshalb blieb das Bild auch mit
// SNES_HD_NO_PAL_TRANSFORM=1 ein Negativ.
//
// HdSpriteRecolor bildet stattdessen jede Referenzfarbe EINZELN auf ihre
// Live-Entsprechung ab und traegt den Abstand des Texels mit. Das ist exakt
// statt gefittet und traegt Inversion so gut wie Farbrotation oder Austausch.
// Fuer Sprites laeuft es seit S19; die BG-Seite hat es nie bekommen.
//
// Kosten: Init() ist teuer (16 Farben), deshalb wird es EINMAL JE FRAME fuer
// die BG-Palettenbloecke vorberechnet, genau wie palLut -- im Pixel steht dann nur
// ein Zeiger. Apply() kostet je Texel bis zu 15 Vergleiche statt drei
// Tabellenzugriffen, mit Frueh-Ausstieg bei exaktem Treffer, der bei
// Palettenfarben fast immer greift. Frame-Zeit steht als ms=cur/max im Log.
//
// SNES_HD_NO_BG_RECOLOR=1 faellt auf den alten R3-LUT zurueck.
static const bool s_noBgRecolor = getenv("SNES_HD_NO_BG_RECOLOR") != nullptr;

// S53: welche CGRAM-Farben eine BG-Kachel ueberhaupt hat.
//
// Bis S52 haben R3-LUT und Umfaerber jede BG-Kachel ueber `PaletteIndex & 7` an
// die 4bpp-Zeile CGRAM[pal*16 .. pal*16+15] gehaengt. Die Hardware adressiert
// aber `basis + pal * 2^bpp + farbe` (SnesPpu::GetRgbColor): eine 2bpp-Kachel
// mit Palette 3 hat die Farben CGRAM[12..15], nicht [48..63]. Nur Palette 0
// traf ueberhaupt die richtigen drei Farben, und selbst dort suchte Apply() ueber
// zwoelf fremde mit. Belegt in bgcap: BG3 in gfxset 37 laeuft zu 95 % auf P1,
// gfxset 38 auf P0-P6 -- also bekam BG3 dort die Farbkorrektur von BG1/BG2.
//
// Die Bloecke 0-7 sind die 4bpp-Zeilen (16 Farben ab pal*16), die Bloecke
// 8-39 die 2bpp-Viertelzeilen (4 Farben ab (b-8)*4). Mode 0 legt die vier
// Ebenen je 32 Farben auseinander, alle anderen Modi beginnen bei 0 -- genau die
// basePaletteOffset-Werte der RenderTilemap-Aufrufe in SnesPpu.cpp.
// 8bpp (Mode 3/4 BG1, Mode 7) ignoriert die Palettenbits und haette 255 Farben;
// dafuer gibt es keinen Block, diese Kacheln bleiben unveraendert.
static constexpr int BgPalBlockCount = 8 + 32;

// S53b A/B: SNES_HD_OLD_BG_PAL_ROWS=1 stellt das Verhalten bis S52 wieder her --
// jede BG-Kachel haengt an der 4bpp-Zeile pal*16, auch 2bpp und 8bpp. Klaert,
// ob S53 hinter den Farbfehlern vom 25.09. steckt (Red Hot Ride, Rickety Race).
static const bool s_oldBgPalRows = getenv("SNES_HD_OLD_BG_PAL_ROWS") != nullptr;

static inline int BgPalBlock(uint8_t bgMode, uint8_t layer, uint8_t pal)
{
	if(layer > 3) {
		return -1;
	}
	pal &= 7;
	if(s_oldBgPalRows) {
		return pal;
	}
	switch(bgMode & 7) {
		case 0: return 8 + layer * 8 + pal;
		case 1: return layer <= 1 ? pal : (layer == 2 ? 8 + pal : -1);
		case 2: return layer <= 1 ? pal : -1;
		case 3: return layer == 1 ? pal : -1;
		case 4: return layer == 1 ? 8 + pal : -1;
		case 5: return layer == 0 ? pal : (layer == 1 ? 8 + pal : -1);
		case 6: return layer == 0 ? pal : -1;
		default: return -1;
	}
}

static inline int BgPalBlockStart(int block) { return block < 8 ? block * 16 : (block - 8) * 4; }
static inline int BgPalBlockSize(int block) { return block < 8 ? 16 : 4; }

// S48: Framebuffer-Dump. Schreibt den fertigen Ausgabepuffer des Filters als
// PNG -- exakt das, was auf dem Schirm landet, in voller HD-Aufloesung und mit
// ganzzahligem Massstab.
//
// Warum: am 23.09. hat ein halber Tag Messarbeit nichts ergeben, weil das
// einzige verfuegbare Bild ein FENSTERFOTO war -- 1071 px fuer 256 native
// Pixel, Massstab 4,1836. Diese krumme Skalierung verwischt genau das native
// Pixelraster, an dem sich nativ und HD unterscheiden lassen. Mesens eigene
// Screenshot-Taste (F12) tut das Richtige, wird auf dem Testrechner aber von
// etwas anderem abgefangen. Dieser Weg braucht keine Taste.
//
//   SNES_HD_DUMP_FRAMES=2    Anzahl Bilder (0 oder ungesetzt = aus)
//   SNES_HD_DUMP_GFXSET=38   nur dieses Gfxset (ungesetzt = jedes)
//   SNES_HD_DUMP_EVERY=30    Abstand in Frames (Standard 30, sonst sind
//                            aufeinanderfolgende Bilder identisch)
//
// Der Dump laeuft NACH dem Rendern und NUR auf einem echten Levelbild
// (ActiveGfxset gesetzt, genug BG-Pixel) -- sonst faengt er Uebergaenge und
// schwarze Frames ein, von denen es beim Levelstart reichlich gibt.
static int GetEnvInt(const char* name, int fallback)
{
	const char* v = getenv(name);
	if(!v || !*v) return fallback;
	return atoi(v);
}
static const int s_dumpFrames = GetEnvInt("SNES_HD_DUMP_FRAMES", 0);
static const int s_dumpGfxset = GetEnvInt("SNES_HD_DUMP_GFXSET", -1);
static const int s_dumpEvery  = GetEnvInt("SNES_HD_DUMP_EVERY", 30);
// S37 A/B: do not give a sub-screen BG operand a ground, i.e. keep the behaviour up to
// S36 where a semi-transparent texel of a BG tile mixed with that tile's own SD colour:
//     set SNES_HD_NO_SUB_BG_UNDER=1
static const bool s_noSubBgUnder = getenv("SNES_HD_NO_SUB_BG_UNDER") != nullptr;
// S43: the reference-palette gate is GONE from all three edge paths.
//
// S21 (2756fa1c, 12 Aug) made every edge path conditional on the tile shipping a
// reference palette, and said plainly that this is an ORIGIN test, not a colour
// one: runtime-captured tiles are grabbed off the live screen with background
// baked into their border texels, the export drops their reference for that
// reason, and extending them outwards smeared that background in -- washed-out
// world map objects on the first test run.
//
// That premise no longer holds, and both halves of it were measured on 21 Sep
// with SNES_HD_EDGE_IGNORE_REF (S42), an A/B pair on the same day:
//
//   * The world map is the case the gate was built for. In August the fringe
//     covered 898,087 sub-pixels per frame -- 97.9% of a 4x screen -- and washed
//     the objects out. With the gate bypassed it now draws 5,643 per frame,
//     0.6%, a factor of 159 less, and the user reports the objects look clean.
//     The test was not vacuous: SPRAREA puts only 33.1% / 51.9% of the sprite
//     area in those contexts behind a reference, so the switch really did
//     unlock half to two thirds of the art there.
//
//   * The Kleever sword splinters are the first object that reached the pack
//     entirely through the runtime category: 158 tiles, not one with a
//     reference, so the gate switched their smoothing off no matter how the art
//     was made. SPRWATCH over 705 frames: 43,638 fringe pixels -- 2.7x their own
//     silhouette -- and the pack answers 100% of them, on all three slots.
//
// What remains is the ordinary escape hatch, unchanged:
//     set SNES_HD_NO_SPRITE_EDGES=1
// Runtime art whose border texels really do carry baked-in background is a
// pipeline problem and belongs to the export, not to a per-pixel test that
// cannot tell the two apart.
// How many GAMEPLAY frames per context are logged.
// S29 raised this from a hard-wired 60 to an environment variable, because 60 --
// one second -- was never a measurement of a level, it was a measurement of
// walking through the door: the fade is still running, the level-name banner is
// on screen and the player has not moved. That is how a bonus screen full of
// KONG letters came to be analysed as Rambi Rumble.
// S30 makes 600 (ten seconds) the DEFAULT. Leaving the useful window behind an
// environment variable meant every real measurement needed a wrapper .bat to
// start the emulator, which is friction on the one action taken most often --
// and a default that has to be overridden to be correct is the wrong default.
// The variable stays, for turning the log DOWN on a long play session as much as
// up for a very long look:
//     set SNES_HD_DIAG_FRAMES=60
// The area recorder below uses the same number to skip the first quarter and
// report from the middle of the window, i.e. from actual play.
static const int s_diagFrames = getenv("SNES_HD_DIAG_FRAMES")
	? std::max(20, atoi(getenv("SNES_HD_DIAG_FRAMES"))) : 600;
// S25w WAS HERE AND IS GONE — measured out, 08 Sep, five runs / 3634 frames.
// The idea: even on a scanline that carries OBJ on the main screen, a sprite can LOSE
// that screen to a background and reach the picture only as the colour-math operand,
// so its fringe would be drawn by neither branch. It was shipped as a counter
// (`sprEdgeWide`) plus an opt-in switch rather than as a change, and the counter came
// back **0 in all five runs, including the one that enabled the switch**.
// Why the estimate was wrong: it was extrapolated from `sprHdSub` (39.160 px in
// Gangplank, $17/$10), but those pixels lie INSIDE the silhouette. Just outside it
// there is no native sprite, and with $212D=$10 the sub screen carries OBJ and nothing
// else — so a fringe pixel finds an EMPTY sub screen and `SubScreenEmpty` rejects it
// before any of this matters. A counter answered in one run what the reasoning got
// wrong; same as `sprFrOver` in S23.

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
	// S45: die Luecke zwischen GEFUNDEN und GEZEICHNET.
	//
	// HdLayers[] steht unmittelbar hinter GetMatchingTile und zaehlt den
	// NACHSCHLAG. Am 21.09. meldete Barrel Bayou damit 26.886 HD-Treffer je
	// Frame auf BG3 -- und der Hintergrund war im Spiel trotzdem nativ. Der
	// User hat es entschieden, indem er den Pack-Ordner umbenannte: das Bild
	// blieb unveraendert. Die Kunst wird also gefunden und danach verworfen,
	// und kein Zaehler im Log konnte in diese Luecke sehen.
	//
	// Diese vier zaehlen je Ebene und je NATIVEM Pixel (nicht je Subpixel),
	// damit sie direkt gegen HdLayers[] vergleichbar sind. Ihre Summe muss
	// HdLayers[] ergeben -- eine Abweichung waere selbst ein Befund.
	uint32_t DrawnLayer[4] = {};   // HD-Kunst des Gewinners kam in den Ausgabepuffer
	uint32_t SkipClip[4] = {};     // von clipMain geblockt (Fenstermaske)
	uint32_t SkipNoSmp[4] = {};    // Kachel gefunden, aber Sampler nicht gueltig
	uint32_t SkipAlpha0[4] = {};   // Sampler gueltig, aber jeder Texel alpha=0
	uint32_t SkipSubOp[4] = {};    // fuenftes Tor: spriteIsSubOperand erzwingt nativ
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
	uint32_t SprEdge = 0;       // S21: pixels outside the native silhouette that carry sprite fringe art
	uint32_t SprEdgeBlend = 0;  // S21: of those, sub-pixels where the fringe was actually drawn
	uint32_t SprEdgeUnder = 0;  // S22: pixels blended against the sprite behind, not the BG
	uint32_t SprEdgeTie = 0;     // S23: fringe present over a sprite, priority rejects it
	uint32_t SprEdgeTieWon = 0;  // S49: that tie resolved by OAM order -- fringe drawn after all
	uint32_t SubSprBot = 0;     // S26: sub-screen sprite given a BG ground to blend against
	uint32_t SubSprUnder = 0;   // S27: sub-screen sprite blended against the SPRITE behind it
	uint32_t SubSprNoBot = 0;   // S29: gate passed, but no ground found (no sub-screen BG with HD art)
	uint32_t SprHoleHd = 0;     // S31: HD sub sprite via the BgLayerMask==0 hole path -- NOT part of SprSub
	uint32_t SubGateNoRef = 0;  // S31: sub-sprite ground REFUSED -- no reference palette for this tile
	uint32_t SubGateOpaque = 0; // S31: sub-sprite ground refused -- tile has no transparent pixels
	uint32_t SubNoBotEmpty = 0; // S33: of SubSprNoBot -- no BG layer on the sub screen at all
	uint32_t SubNoBotNoHd = 0;  // S33: of SubSprNoBot -- a BG IS there, it just has no HD art
	uint32_t SubBotRetry = 0;   // S34: ground found only via the BG1<->BG2 layer-agnostic retry
	uint32_t SubBgBot = 0;      // S37: sub-screen BG operand given a ground to blend against
	uint32_t SubBgBotRetry = 0; // S37: of those, found only via the BG1<->BG2 retry
	uint32_t SubBgNoBot = 0;    // S37: transparent BG operand, no ground found on the sub screen
	uint32_t SubBgOpaque = 0;   // S37: BG operand tile is fully opaque -- it has no edge to soften
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
	dst.SprEdge += src.SprEdge;
	dst.SprEdgeBlend += src.SprEdgeBlend;
	dst.SprEdgeUnder += src.SprEdgeUnder;
	dst.SprEdgeTie += src.SprEdgeTie;
	dst.SprEdgeTieWon += src.SprEdgeTieWon;
	dst.SubSprBot += src.SubSprBot;
	dst.SubSprUnder += src.SubSprUnder;
	dst.SubSprNoBot += src.SubSprNoBot;
	dst.SprHoleHd += src.SprHoleHd;
	dst.SubGateNoRef += src.SubGateNoRef;
	dst.SubGateOpaque += src.SubGateOpaque;
	dst.SubNoBotEmpty += src.SubNoBotEmpty;
	dst.SubNoBotNoHd += src.SubNoBotNoHd;
	dst.SubBotRetry += src.SubBotRetry;
	dst.SubBgBot += src.SubBgBot;
	dst.SubBgBotRetry += src.SubBgBotRetry;
	dst.SubBgNoBot += src.SubBgNoBot;
	dst.SubBgOpaque += src.SubBgOpaque;
	for(int i = 0; i < 4; i++) {
		dst.LayerBits[i] += src.LayerBits[i];
		dst.Win[i] += src.Win[i];
		dst.HdLayers[i] += src.HdLayers[i];
		dst.DrawnLayer[i] += src.DrawnLayer[i];
		dst.SkipClip[i] += src.SkipClip[i];
		dst.SkipNoSmp[i] += src.SkipNoSmp[i];
		dst.SkipAlpha0[i] += src.SkipAlpha0[i];
		dst.SkipSubOp[i] += src.SkipSubOp[i];
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
	// S44: the environment switch stays as a force-off for headless A/B runs;
	// the setting is the master. Either one off means off.
	const bool noSpriteEdges = s_noSpriteEdges || !ctx.smoothEdges;
	const uint64_t vramSig = ctx.vramSig;
	const bool anyPalTransform = ctx.anyPalTransform;
	const bool* palRowActive = ctx.palRowActive;
	const uint8_t (*palLut)[3][256] = ctx.palLut;
	const HdSpriteRecolor* bgRecolor = (const HdSpriteRecolor*)ctx.bgRecolor;   // S52
	const bool* bgRecolorActive = ctx.bgRecolorActive;
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
			SnesHdPackTileInfo* subTileBot = nullptr;  // S26: ground under a sub-screen SPRITE operand
			SnesHdPpuTileInfo* subTileInfoBot = nullptr;
			bool cmActive = false;  // hoisted so the rendering section (below) can see it too
			uint8_t winLayer = 0xFF;  // hoisted so the rendering section (below) can see it too
			bool subSprHdFired = false;  // S9: the S7 sub-sprite HD-operand path rendered this pixel
			// S32: gate passed, ground search empty -- the sub screen shows the BACKDROP here.
			bool subGroundIsBackdrop = false;
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

								// S37: give the BG operand the ground the sub-screen SPRITE
								// got in S26. Until now `subTileBot` was set only in the
								// sprite branch below, so a semi-transparent texel of a BG
								// tile was blended against nsR/nsG/nsB -- the native
								// sub-screen colour, which at a BG pixel IS that same tile's
								// own SD colour. Art mixed with itself cannot show a soft
								// edge, so the level geometry kept its native 8x8 silhouette
								// and went stair-stepped at 4x. Mainbrace carries its terrain
								// (BG1) on the sub screen (DATA_FD7AB6: Main=$04, Sub=$13),
								// which is where the user saw it.
								if(!subTile->HasTransparentPixels) {
									st.SubBgOpaque++;
								} else if(!s_noSubBgUnder) {
									bool anyBgBehind = false;
									for(int layer = 0; layer < 4 && !subTileBot; layer++) {
										if(layer == sLayer) continue;
										if(!(pixelInfo.BgLayerMask & (1 << layer))) continue;
										if(!(sl.SubScreenLayers & (1 << layer))) continue;
										// `sLayer` IS the sub-screen winner, so any other layer
										// still present on that screen lost to it and is behind
										// it -- no priority order has to be rebuilt here.
										anyBgBehind = true;
										SnesHdPackTileInfo* below = CachedGetMatchingTile(hdData,
											hdScreen->Vram, tileLookupCache, pixelInfo.BgTiles[layer].Key);
										// S34's BG1<->BG2 retry, needed here for its reason:
										// with swapped chr bases the art sits in the pack under
										// the other layer index, and a strict lookup finds
										// nothing while the pack is complete.
										if(!below && layer <= 1) {
											SnesHdTileKey altKey = pixelInfo.BgTiles[layer].Key;
											altKey.LayerIndex = layer ^ 1;
											below = CachedGetMatchingTile(hdData, hdScreen->Vram,
												tileLookupCache, altKey);
											if(below) st.SubBgBotRetry++;
										}
										if(below) {
											subTileBot = below;
											subTileInfoBot = &pixelInfo.BgTiles[layer];
											st.SubBgBot++;
										}
									}
									if(!subTileBot) {
										st.SubBgNoBot++;
										// Nothing behind it on the sub screen: the sub screen
										// outputs the BACKDROP at this pixel, so that is what
										// the soft edge has to fade into. Same reasoning as S32,
										// which reached this conclusion for sprites.
										if(!anyBgBehind) subGroundIsBackdrop = !s_noBackdropGround;
									}
								}
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

							// S26: the same wall S21 tore down in the main path, still
							// standing here. A semi-transparent texel of this sprite is
							// blended below against nsR/nsG/nsB — the native SUB-screen
							// colour — and at a sprite pixel that IS the sprite's own SD
							// colour. Mixing art with itself cannot show a soft edge, so
							// in overlay levels the character kept a hard silhouette even
							// where the art carries a gradient. The ground is whatever BG
							// lies behind it ON THE SUB SCREEN: same reasoning as S21b's
							// MainScreenLayers gate, mirrored to $212D, because here it is
							// the sub screen that composites the character.
							// S43: the reference-palette gate that used to stand here is
							// gone; the counters below stay, because they are the only
							// per-pixel reading of how much art is baked per palette slot.
							// S31: WHY the gate refused, counted separately. Until then the
							// only reference counters were SprRecolor/SprRecolorNoRef, and
							// both hang off the MAIN-screen sprite path -- where sprWon is 0
							// in every overlay level. They read 0/0 in Mainbrace too, where
							// everything works, so "Rambi has no reference problem, sprNoRef
							// is 0" was never a measurement. Third counter in a row that is
							// blind in exactly the levels it was needed for; see the notes.
							bool subHasRef = hdData->GetSpriteRefPalette(pixelInfo.Sprites[1].Key.ContentHash) != nullptr;
							if(!subSprTile->HasTransparentPixels) {
								st.SubGateOpaque++;
							} else if(!subHasRef) {
								st.SubGateNoRef++;
							}
							if(!noSpriteEdges && !s_noSubUnder && subSprTile->HasTransparentPixels) {
								// S27: when a second sprite lies under this one, THAT is the
								// ground, not the background — the Kongs one behind the other
								// under water, which the user reported on 08 Sep as the one
								// place the outline still goes hard. `hdCaptureSpriteFrom`
								// fills slot 3 for the SUB slot as well, so the displaced
								// sprite is already on hand here; nothing new has to be
								// recorded. Exactly what S22 does in the main path.
								// No BG fallback when slot 3 exists, for S22's reason: the
								// background is the known-wrong backdrop behind a character,
								// and a hard edge beats showing terrain through a Kong.
								if((pixelInfo.SpriteCount & 0x08) && !s_noSpriteUnder && !s_noSubSprUnder) {
									SnesHdPackTileInfo* under = CachedGetMatchingTile(hdData, hdScreen->Vram,
										tileLookupCache, pixelInfo.Sprites[3].Key);
									if(under) {
										subTileBot = under;
										subTileInfoBot = &pixelInfo.Sprites[3];
										st.SubSprUnder++;
									}
								} else {
									// S33: does a BG lie on the sub screen behind the character
									// at all? `subNoBot` alone could not say -- it fired both
									// when no layer qualified AND when one did but carried no HD
									// art. Those need opposite grounds, and S32 gave both of
									// them the backdrop, which is why Rambi came back looking
									// slightly WORSE: where terrain really is behind the Kong,
									// its colour got replaced by the backdrop.
									bool anyBgOnSub = false;
									for(int layer = 0; layer < 4 && !subTileBot; layer++) {
										if(!(pixelInfo.BgLayerMask & (1 << layer))) continue;
										if(!(sl.SubScreenLayers & (1 << layer))) continue;
										anyBgOnSub = true;
										SnesHdPackTileInfo* below = CachedGetMatchingTile(hdData, hdScreen->Vram,
											tileLookupCache, pixelInfo.BgTiles[layer].Key);
										// S34: the SAME BG1<->BG2 layer-agnostic retry the two
										// rendering paths already do (lines ~1059 and ~1211).
										// This search did not have it, and that is the whole
										// difference in Rambi Rumble: measured `lRetry` there is
										// 36.427 px per frame -- 64% of the screen -- against 0
										// in every other level in the log. Rambi's chr bases are
										// swapped (ppuConfig DATA_FD7ADF: $210B=$0725, so BG1 =
										// $5000 and BG2 = $2000), so its art sits in the pack
										// under the other layer index. The render paths bridge
										// that with the retry and the level looks perfect; this
										// search asked once, strictly, and gave up -- in exactly
										// the level where the strict question never works.
										// Hence: art present, level clean, edges hard.
										if(!below && layer <= 1) {
											SnesHdTileKey altKey = pixelInfo.BgTiles[layer].Key;
											altKey.LayerIndex = layer ^ 1;
											below = CachedGetMatchingTile(hdData, hdScreen->Vram,
												tileLookupCache, altKey);
											if(below) st.SubBotRetry++;
										}
										if(below) {
											subTileBot = below;
											subTileInfoBot = &pixelInfo.BgTiles[layer];
											st.SubSprBot++;
										}
									}
									// S29: the gate let this pixel through and the search still
									// came back empty -- 338.741 px per 652 frames of Rambi
									// Rumble (48%), against ZERO in Mainbrace. The cause is
									// $212D, not the art:
									//     Rambi     Main=$01 BG1     Sub=$16 BG2+BG3+OBJ
									//     Mainbrace Main=$04 BG3     Sub=$13 BG1+BG2+OBJ
									// Rambi keeps its scenery on the MAIN screen, so wherever
									// the character hangs in open air the sub screen carries no
									// BG at all and this loop searches an empty screen.
									//
									// S30 TRIED TO FILL THAT FROM THE MAIN SCREEN AND WAS WRONG.
									// Measured: the fallback fired on 326.902 px and drove
									// subNoBot to 0, and the picture did not change. It cannot
									// help, because with Main=$01 the only layer it can find IS
									// the main-screen winner -- and the operand is then added
									// back onto that same winner below, so BG1 lands on itself.
									// The operand already carries the right ground without it:
									// `oR = nsR` is the native sub-screen colour, which is the
									// backdrop exactly where this loop finds nothing, and the
									// backdrop is what the real PPU composites there too.
									// A ground for these pixels has to come from the SUB screen
									// or not at all.
									if(!subTileBot) {
										st.SubSprNoBot++;
										if(anyBgOnSub) st.SubNoBotNoHd++; else st.SubNoBotEmpty++;
										// S32: nothing on the sub screen behind the character, so
										// the sub screen outputs the BACKDROP at this pixel. That
										// is the ground the soft edge has to fade into. Without
										// it the fringe blends against `nsR`, which at a sprite
										// pixel IS the sprite's own SD colour -- art mixed with
										// itself, which is why the silhouette stayed hard.
										// NOT the main screen: S30 tried that and it does
										// nothing, because colour math adds the operand back
										// onto the main screen and BG1 would land on itself.
										subGroundIsBackdrop = !anyBgOnSub && !s_noBackdropGround;
									}
								}
							}
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
						// S31: NOT SprSubHd. This block sits outside the
						// `if(cmActive && ColorMathAddSubscreen)` chain that increments
						// SprSub, and it only runs where cmActive was false -- so every
						// hit here is a pixel the SprSub denominator excludes by
						// construction. Counting it as SprSubHd made sprHdSub/sprSub a
						// ratio of two different populations, inflating it by a
						// level-dependent amount. That is where "39% of the sprite pixels
						// have no HD art" came from, twice, and it was wrong both times.
						st.SprHoleHd++;
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

					// S21 (second half): give the sprite a bottom layer, the same way a
					// BG winner gets one. Without it a partly transparent sprite texel
					// blends against nm* — and at a sprite pixel that is the sprite's own
					// SD colour, so the softest edge in the art comes out as opaque as
					// the hard one. Blending against the BG art beneath is what makes an
					// anti-aliased silhouette visible at all. Where the background has no
					// HD art the old behaviour stands: the native colour is all there is.
					SnesHdScanlineInfo& slSpr = hdScreen->ScanlineInfo[y];
					// S22: when a second sprite lies under this one, THAT is the ground —
					// not the BG tile. S21b could only spot the case and stand down, which
					// left the edge hard exactly where two sprites meet (Kruncha's arm in
					// front of the sun, the Kongs behind one another). Now the displaced
					// sprite arrives as slot 3 and the existing blend does the rest.
					// No BG fallback in that case on purpose: the BG is the known-wrong
					// backdrop, and a hard edge beats showing it through a character.
					if(!noSpriteEdges && hdTile->HasTransparentPixels && !hdTileBot) {
						if((pixelInfo.SpriteCount & 0x08) && !s_noSpriteUnder) {
							SnesHdPackTileInfo* under = CachedGetMatchingTile(hdData, hdScreen->Vram,
								tileLookupCache, pixelInfo.Sprites[3].Key);
							if(under) {
								hdTileBot = under;
								hdTileInfoBot = &pixelInfo.Sprites[3];
								st.SprEdgeUnder++;
							}
						} else {
							for(int layer = 0; layer < 4 && !hdTileBot; layer++) {
								if(!(pixelInfo.BgLayerMask & (1 << layer))) continue;
								if(!(slSpr.MainScreenLayers & (1 << layer))) continue;
								// MAIN screen only, and that gate is the whole underwater story:
								// below the water line DKC2 switches Main to $00/$04 by HDMA and
								// leaves BG1/BG2 on the SUB screen alone. Such a layer is not what
								// lies behind the sprite on the main screen — blending Dixie's soft
								// hair against its bright art is what put a light rim around her,
								// and only under water, because nowhere else do the registers look
								// like this. Issue T needed the same gate in July, one block up.
								SnesHdPackTileInfo* below = CachedGetMatchingTile(hdData, hdScreen->Vram,
									tileLookupCache, pixelInfo.BgTiles[layer].Key);
								if(below) {
									hdTileBot = below;
									hdTileInfoBot = &pixelInfo.BgTiles[layer];
									st.MultiLayer++;
								}
							}
						}
					}
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
			// S50: Schalter oben -- Operand dann nativ, wie bei der PPU.
			bool hasSubHd = !s_noSubHdOperand && subTile && subTileInfo && !subTile->HdTileData.empty();

			// =============================================================
			// S21: sprite edge fringe.
			//
			// The SNES decides sprite coverage on the 1x grid — colour index 0 is
			// nothing at all — so the native silhouette is a staircase. The 4x art
			// carries its own soft fringe just outside that staircase, and until now
			// it could not be drawn: no HD identity was recorded where the native
			// pixel was transparent. The PPU now records it in slot 2 (see
			// SnesPpu::FetchSpriteTile), and this is where it gets drawn.
			//
			// The priority comparison happens HERE and not in the PPU on purpose: at
			// the time sprites render, the tilemaps have not composited yet, so the
			// question "would this sprite be in front?" cannot be answered there.
			// MainScreenFlags now holds the actual winner, so its priority is exactly
			// what the sprite would have had to beat.
			SnesHdPackTileInfo* edgeTile = nullptr;
			const SnesHdPpuTileInfo* edgeTileInfo = nullptr;
			// Only art whose baked palette we know takes part. That is not a colour
			// argument but an origin one: runtime-captured tiles (world map objects —
			// wasps, flag, torches) are grabbed off the live screen, so background
			// colour is baked into their border texels, and the export drops their
			// reference for exactly that reason. Extending such a tile outwards smears
			// that background into the picture — user-visible as a washed-out fringe on
			// the hub objects, while gallery art like Kruncha looked right. Having a
			// reference is precisely what separates the two.
			// S23 tried dropping the !spriteWon guard here so a fringe could be drawn
			// over a sprite that won the pixel. Measured, and put back: the A/B run of
			// 07 Sep settled it. With the guard removed, Dixie's yellow hair showed
			// THROUGH the crate she carries over her head, and the counters explain why
			// that was the whole of its effect — sprFrOver stayed 0 while sprFrTie ran
			// into the thousands, i.e. fringe and winner nearly always share a priority
			// and the honest gate below rejects them anyway. The smoothing the user sees
			// on Kruncha's arm comes from slot 3 (S22) instead: turning THAT off brings
			// the hard edge back, turning this off does not.
			// Telling one from the other needed a switch, not an argument.
			//
			// S25 (removed 08 Sep, full implementation in commit 4482908a) added a third
			// branch here for the overlay levels, where OBJ is off the main screen and the
			// character exists only as the colour-math operand: it routed the fringe into
			// that operand instead of into the pre-math main colour, gated on
			// `!objOnMain && objOnSub` from the scanline registers, on `!SubScreenEmpty`,
			// `!SubScreenHasSprite`, and on a priority test against the SUB-screen winner
			// (`SubScreenWinnerPlus1` -> `BgTiles[].Priority`). The PPU counterpart
			// recorded slot 2 whenever sprites were drawn on EITHER screen.
			// It was correct and it drew: `sprEdgeSub` reached 747 subpixels per frame in
			// Mainbrace. It is gone because five A/B runs over 3634 frames could not find
			// a visible effect -- with it off the user saw no change, while switching off
			// the sub GROUND below made them report "not smoothed". Keeping code whose
			// only evidence is that it executes is how the S23 branch survived as long as
			// it did.
			if(!noSpriteEdges && (pixelInfo.SpriteCount & 0x04) && !spriteWon
				&& pixelInfo.Sprites[2].Key.ContentHash != 0
				&& pixelInfo.Sprites[2].Priority > (pixelInfo.MainScreenFlags & 0x0F)) {
				edgeTile = CachedGetMatchingTile(hdData, hdScreen->Vram, tileLookupCache,
					pixelInfo.Sprites[2].Key);
				if(edgeTile && !edgeTile->HdTileData.empty()) {
					edgeTileInfo = &pixelInfo.Sprites[2];
					st.SprEdge++;
				} else {
					edgeTile = nullptr;
				}
			} else if(!noSpriteEdges && (pixelInfo.SpriteCount & 0x04) && spriteWon
				&& pixelInfo.Sprites[2].Key.ContentHash != 0
				&& pixelInfo.Sprites[2].Priority <= (pixelInfo.MainScreenFlags & 0x0F)) {
				// S23 diagnostic: a fringe was recorded over a winning sprite but the
				// priority test rejected it. A large number here means equal priorities
				// are common and the tie-break has to come from OAM order after all.
				//
				// S49 supplies it. On EQUAL priority the hardware separates the two by
				// OAM order, and slot 2 now carries the scanline fetch sequence, where
				// a HIGHER value means fetched later and therefore in front (see
				// HdSpritePixel::OamSeq). Only the equal case is opened up: a fringe of
				// LOWER priority stays rejected, exactly as before.
				//
				// This is deliberately the narrow version of what S23 tried. S23 dropped
				// the !spriteWon guard outright and Dixie's hair showed through the crate
				// she carries -- fringes were drawn over sprites really in front of them.
				// With the sequence in hand that case is now the one being excluded.
				const bool tieBreakable = !s_noOamTiebreak
					&& (pixelInfo.SpriteCount & 0x01)
					&& pixelInfo.Sprites[2].Priority == (pixelInfo.MainScreenFlags & 0x0F)
					&& pixelInfo.Sprites[2].OamSeq > pixelInfo.Sprites[0].OamSeq;
				if(tieBreakable) {
					edgeTile = CachedGetMatchingTile(hdData, hdScreen->Vram, tileLookupCache,
						pixelInfo.Sprites[2].Key);
					if(edgeTile && !edgeTile->HdTileData.empty()) {
						edgeTileInfo = &pixelInfo.Sprites[2];
						st.SprEdge++;
						st.SprEdgeTieWon++;
					} else {
						edgeTile = nullptr;
						st.SprEdgeTie++;
					}
				} else {
					st.SprEdgeTie++;
				}
			}
			bool hasEdgeHd = edgeTile != nullptr;

			// P4.1f: when the character is the color-math operand, render the
			// exact PPU output instead of compositing HD art over it (Issue R).
			if((hasMainHd || hasSubHd || hasEdgeHd) && !spriteIsSubOperand) {
				// S45: wurde die HD-Kunst des Gewinners wirklich sichtbar?
				bool hdWritten = false;
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
				HdTileSampler botSampler, mainSampler, subSampler, edgeSampler, subBotSampler;
				botSampler.Init(hdTileBot, hdTileInfoBot, hdScale);
				if(hasMainHd) mainSampler.Init(hdTile, hdTileInfo, hdScale);
				if(hasSubHd) subSampler.Init(subTile, subTileInfo, hdScale);
				if(hasEdgeHd) edgeSampler.Init(edgeTile, edgeTileInfo, hdScale);
				// S26: only ever set for a sub-screen SPRITE operand, so this stays
				// invalid — and costs nothing — for every ordinary BG operand pixel.
				subBotSampler.Init(subTileBot, subTileInfoBot, hdScale);

				// R3: per-tile palette-row transform (live CGRAM vs reference).
				// Resolved once per native pixel; nullptr = identity.
				// R3.1: pointer to the row's 3×256 LUT — one load per channel per sample.
				typedef const uint8_t (*PalLutRow)[256];
				PalLutRow botLut = nullptr, mainLut = nullptr, subLut = nullptr, subBotLut = nullptr;
				// S53: der CGRAM-Block je Kachel, nach Modus, Ebene und Palette --
				// siehe BgPalBlock(). -1 = Sprite, 8bpp oder kein Sampler.
				const int botBlk = botSampler.valid && hdTileInfoBot->Key.LayerIndex != 4
					? BgPalBlock(sl.BgMode, hdTileInfoBot->Key.LayerIndex, hdTileInfoBot->Key.PaletteIndex) : -1;
				const int mainBlk = mainSampler.valid && hdTileInfo->Key.LayerIndex != 4
					? BgPalBlock(sl.BgMode, hdTileInfo->Key.LayerIndex, hdTileInfo->Key.PaletteIndex) : -1;
				const int subBlk = subSampler.valid && subTileInfo->Key.LayerIndex != 4
					? BgPalBlock(sl.BgMode, subTileInfo->Key.LayerIndex, subTileInfo->Key.PaletteIndex) : -1;
				const int subBotBlk = subBotSampler.valid && subTileInfoBot->Key.LayerIndex != 4
					? BgPalBlock(sl.BgMode, subTileInfoBot->Key.LayerIndex, subTileInfoBot->Key.PaletteIndex) : -1;
				if(anyPalTransform) {
					// S22: the bottom tile can now be a SPRITE (slot 3). Same OBJ-palette
					// exemption as main/sub below — the R3 LUT covers BG CGRAM rows 0-7
					// only, and running a sprite through it would tint it with a BG row.
					if(botBlk >= 0 && palRowActive[botBlk]) {
						botLut = palLut[botBlk];
					}
					// S4: no LUT for sprites — the R3 transform covers BG CGRAM rows
					// 0-7 (entries 0-127) only; OBJ palettes live at CGRAM 128-255.
					if(mainBlk >= 0 && palRowActive[mainBlk]) {
						mainLut = palLut[mainBlk];
					}
					// S7: sub operand may now be a sprite (LayerIndex 4) — same OBJ-palette
					// exemption as the main sprite path above.
					if(subBlk >= 0 && palRowActive[subBlk]) {
						subLut = palLut[subBlk];
					}
					// S26: the ground under a sub-screen sprite is a BG tile, so unlike
					// the sprite above it this one DOES belong under the R3 row LUT.
					if(subBotBlk >= 0 && palRowActive[subBotBlk]) {
						subBotLut = palLut[subBotBlk];
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

				// S52: Zeiger statt Wert -- die BG-Fassung ist je Frame vorberechnet
				// und wird nur referenziert, die Sprite-Fassung liegt lokal.
				HdSpriteRecolor mainRecolorObj, subRecolorObj;
				const HdSpriteRecolor* mainRecolor = nullptr;
				const HdSpriteRecolor* subRecolor = nullptr;
				if(bgRecolor && mainBlk >= 0 && bgRecolorActive[mainBlk]) {
					mainRecolor = &bgRecolor[mainBlk];
					mainLut = nullptr;   // beides waere doppelt gemoppelt
				}
				if(bgRecolor && subBlk >= 0 && bgRecolorActive[subBlk]) {
					subRecolor = &bgRecolor[subBlk];
					subLut = nullptr;
				}
				if(!s_noRecolor && mainSampler.valid && hdTileInfo->Key.LayerIndex == 4) {
					const uint16_t* spriteRef = hdData->GetSpriteRefPalette(hdTileInfo->Key.ContentHash);
					mainRecolorObj.Init(spriteRef, hdScreen->Cgram + 128 + (hdTileInfo->Key.PaletteIndex & 7) * 16);
					if(mainRecolorObj.valid) mainRecolor = &mainRecolorObj;
					// Separated on purpose: "no reference shipped" is a pack gap worth
					// reporting, while "reference == live" is the normal, correct case
					// and must not look like one.
					if(!spriteRef) {
						st.SprRecolorNoRef++;
					} else if(mainRecolorObj.valid) {
						st.SprRecolor++;
					}
				}
				if(!s_noRecolor && subSampler.valid && subTileInfo->Key.LayerIndex == 4) {
					subRecolorObj.Init(hdData->GetSpriteRefPalette(subTileInfo->Key.ContentHash),
						hdScreen->Cgram + 128 + (subTileInfo->Key.PaletteIndex & 7) * 16);
					if(subRecolorObj.valid) subRecolor = &subRecolorObj;
				}
				// S21: the fringe is the same sprite's art and follows the same live
				// OBJ palette — without this it would be the one part of a character
				// that ignores the level's tint.
				// S22: a sprite used as the bottom tile needs the recolor too — it is
				// the same art under the same live OBJ palette as any other sprite, and
				// without this the character showing through a soft edge would keep its
				// baked colors while the one in front follows the level's tint.
				HdSpriteRecolor botRecolorObj;
				const HdSpriteRecolor* botRecolor = nullptr;
				if(bgRecolor && botBlk >= 0 && bgRecolorActive[botBlk]) {
					botRecolor = &bgRecolor[botBlk];
					botLut = nullptr;
				}
				if(!s_noRecolor && botSampler.valid && hdTileInfoBot->Key.LayerIndex == 4) {
					botRecolorObj.Init(hdData->GetSpriteRefPalette(hdTileInfoBot->Key.ContentHash),
						hdScreen->Cgram + 128 + (hdTileInfoBot->Key.PaletteIndex & 7) * 16);
					if(botRecolorObj.valid) botRecolor = &botRecolorObj;
				}
				// S27: the operand's ground can now be a sprite (slot 3) — same pair of
				// traps S22 walked into in the main path: the R3 row LUT above is already
				// guarded on LayerIndex != 4, and a sprite ground needs the recolor
				// instead, or the character behind keeps its baked colours while the one
				// in front follows the level's live OBJ palette.
				HdSpriteRecolor subBotRecolorObj;
				const HdSpriteRecolor* subBotRecolor = nullptr;
				if(bgRecolor && subBotBlk >= 0 && bgRecolorActive[subBotBlk]) {
					subBotRecolor = &bgRecolor[subBotBlk];
					subBotLut = nullptr;
				}
				if(!s_noRecolor && subBotSampler.valid && subTileInfoBot->Key.LayerIndex == 4) {
					subBotRecolorObj.Init(hdData->GetSpriteRefPalette(subTileInfoBot->Key.ContentHash),
						hdScreen->Cgram + 128 + (subTileInfoBot->Key.PaletteIndex & 7) * 16);
					if(subBotRecolorObj.valid) subBotRecolor = &subBotRecolorObj;
				}
				// Die Franse ist immer Sprite-Kunst -- hier aendert sich nichts.
				HdSpriteRecolor edgeRecolorObj;
				const HdSpriteRecolor* edgeRecolor = nullptr;
				if(!s_noRecolor && edgeSampler.valid) {
					edgeRecolorObj.Init(hdData->GetSpriteRefPalette(edgeTileInfo->Key.ContentHash),
						hdScreen->Cgram + 128 + (edgeTileInfo->Key.PaletteIndex & 7) * 16);
					if(edgeRecolorObj.valid) edgeRecolor = &edgeRecolorObj;
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
								if(mainRecolor) {
									mc = mainRecolor->Apply(mc);
								}
								mha = mc >> 24;
							}
							if(mha == 255) {
								hdWritten = true;   // S45
								r = (mc >> 16) & 0xFF; g = (mc >> 8) & 0xFF; b = mc & 0xFF;
								if(mainLut) {
									// R3: follow live CGRAM (rgb is premultiplied; alpha unchanged)
									r = mainLut[0][r]; g = mainLut[1][g]; b = mainLut[2][b];
								}
							} else {
								r = nmR; g = nmG; b = nmB;
								if(botSampler.valid) {
									uint32_t c = botSampler.Sample(dx, dy);
									if(botRecolor) {
										c = botRecolor->Apply(c);
									}
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
									hdWritten = true;   // S45
									int hr = (mc >> 16) & 0xFF, hg = (mc >> 8) & 0xFF, hb = mc & 0xFF;
									if(mainLut) {
										hr = mainLut[0][hr]; hg = mainLut[1][hg]; hb = mainLut[2][hb];
									}
									r = hr + (r * (255 - (int)mha)) / 255;
									g = hg + (g * (255 - (int)mha)) / 255;
									b = hb + (b * (255 - (int)mha)) / 255;
								}
							}

							// --- S21: sprite fringe, mixed into the PRE-MATH main colour ---
							// It sits at a pixel the BG won, so whatever the PPU does to
							// that pixel has to happen to the fringe as well. Mixing it in
							// afterwards let it escape the underwater colour math in
							// Lockjaw's Locker: the scene darkened around Dixie while her
							// fringe kept full brightness, which read as a bright rim along
							// her hair — and only under water, where that math runs.
							if(edgeSampler.valid) {
								uint32_t ec = edgeSampler.Sample(dx, dy);
								if(edgeRecolor) {
									ec = edgeRecolor->Apply(ec);
								}
								uint32_t ea = ec >> 24;
								if(ea) {
									// premultiplied, like every other blend here
									int er = (ec >> 16) & 0xFF, eg = (ec >> 8) & 0xFF, eb = ec & 0xFF;
									r = er + (r * (255 - (int)ea)) / 255;
									g = eg + (g * (255 - (int)ea)) / 255;
									b = eb + (b * (255 - (int)ea)) / 255;
									st.SprEdgeBlend++;
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
								// S32: at a sub-screen sprite pixel whose ground search came
								// back empty, `nsR` is the sprite's OWN SD colour, so blending
								// the HD art's soft edge against it mixes art with itself and
								// no gradient can appear -- the silhouette stays hard at native
								// resolution. What the sub screen really outputs behind the
								// character there is the BACKDROP (no sub-screen BG at this
								// pixel), so that is what the edge must fade into. Measured on
								// Rambi Rumble: 38,8% of its HD sub-sprite pixels, against 0%
								// in Mainbrace -- which is exactly why the smoothing works in
								// one and not the other. Levels like Mainbrace never reach
								// this line.
								if(subGroundIsBackdrop) {
									uint16_t bd = hdScreen->Cgram[0];
									oR = ColorUtilities::Convert5BitTo8Bit(bd & 0x1F);
									oG = ColorUtilities::Convert5BitTo8Bit((bd >> 5) & 0x1F);
									oB = ColorUtilities::Convert5BitTo8Bit((bd >> 10) & 0x1F);
								}
								uint32_t c = 0;
								uint32_t ha = 0;
								if(subSampler.valid) {
									c = subSampler.Sample(dx, dy);
									if(subRecolor) {
										c = subRecolor->Apply(c);
									}
									ha = c >> 24;
								}
								if(ha == 255) {
									// R6.1: opaque fast path (blend reduces to o=h)
									oR = (c >> 16) & 0xFF; oG = (c >> 8) & 0xFF; oB = c & 0xFF;
									if(subLut) {
										oR = subLut[0][oR]; oG = subLut[1][oG]; oB = subLut[2][oB];
									}
								} else {
									// S26: the operand gets a bottom layer, exactly as the main
									// pixel has had one since S21 — this is the ground a soft
									// sprite edge is supposed to be seen against. Invalid for
									// every case but a sub-screen sprite, so ordinary operand
									// pixels take the same path as before.
									if(subBotSampler.valid) {
										uint32_t bc = subBotSampler.Sample(dx, dy);
										if(subBotRecolor) {
											bc = subBotRecolor->Apply(bc);
										}
										uint32_t ba = bc >> 24;
										if(ba) {
											int br = (bc >> 16) & 0xFF, bgc = (bc >> 8) & 0xFF, bb = bc & 0xFF;
											if(subBotLut) {
												br = subBotLut[0][br]; bgc = subBotLut[1][bgc]; bb = subBotLut[2][bb];
											}
											oR = br + (oR * (255 - (int)ba)) / 255;
											oG = bgc + (oG * (255 - (int)ba)) / 255;
											oB = bb + (oB * (255 - (int)ba)) / 255;
										}
									}
									if(ha > 0) {
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

				// S45: je BG-Gewinnerpixel genau einer der vier Zaehler. Sprites
				// sind ausgenommen -- winLayer gilt dort nicht, und die Frage
				// stammt von der BG-Seite. Die Reihenfolge ist die der Gates im
				// Code darueber, damit der erste greifende Grund gezaehlt wird.
				if(!spriteWon && winLayer < 4 && hasMainHd) {
					if(clipMain) st.SkipClip[winLayer]++;
					else if(!mainSampler.valid) st.SkipNoSmp[winLayer]++;
					else if(!hdWritten) st.SkipAlpha0[winLayer]++;
					else st.DrawnLayer[winLayer]++;
				}
			} else {
				// S45: das fuenfte Tor. Liegt HD-Kunst fuer den Gewinner vor und wir
				// landen trotzdem hier, kann nur spriteIsSubOperand (:1513) es sein --
				// der Pixel wird bewusst nativ gezeichnet. Ohne diesen Zaehler waere
				// die Summe kleiner als hdBGn und saehe nach einem unbekannten Tor aus.
				if(!spriteWon && winLayer < 4 && hasMainHd) st.SkipSubOp[winLayer]++;
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

			// S47: der Anstrich liegt ganz am Ende und ueberschreibt, was auch immer
			// oben entstanden ist -- HD wie nativ. Nur so zeigt er die Ebenenaufteilung
			// und nicht die Kunst.
			if(s_paintLayers && !spriteWon && winLayer < 4) {
				static const uint32_t layerColor[4] = {
					0xFFFF0000,  // BG1 rot
					0xFF00FF00,  // BG2 gruen
					0xFF0080FF,  // BG3 blau
					0xFFFFFF00   // BG4 gelb
				};
				const uint32_t c = layerColor[winLayer];
				for(uint32_t dy = 0; dy < hdScale; dy++) {
					for(uint32_t dx = 0; dx < hdScale; dx++) {
						uint32_t outIndex = (outY + dy) * frameWidth + (outX + dx);
						if(outIndex < frameWidth * frameHeight) {
							outputBuffer[outIndex] = c;
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
	// S35: start of the whole filter frame (see SnesHdPerf.h).
	const SnesHdPerf::Clock::time_point perfT0 = SnesHdPerf::Clock::now();

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
	static int diagPalRowLogged = -1;    // S51: gfxset, dessen PALROW-Zeilen schon geschrieben sind
	static int diagSprCapLogCount = 0;   // S1: SPRTILE sample-line batches per context
	// S28: sprite tiles ranked by the AREA they cover, not by scan order.
	// SPRTILE, the existing sample, stops after 16 tiles and walks the screen from
	// pixel 0 — i.e. from the top edge, where the HUD sits. It reported 74 % of
	// Gangplank's sprite tiles as having no reference palette while the complete
	// per-pixel counter said 0.2 %. A sample that disagrees with the full count by
	// that much cannot answer "which art covers the character", so this one counts
	// pixels per tile and reports the largest.
	static std::unordered_map<uint64_t, uint32_t> diagSprAreaPx;
	static std::unordered_map<uint64_t, uint8_t> diagSprAreaPal;
	static bool diagSprAreaLogged = false;
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
	// S53: je CGRAM-Block statt je Palettennummer, siehe BgPalBlock().
	uint16_t palRatio[BgPalBlockCount][3];
	bool palRowActive[BgPalBlockCount] = {};
	bool anyPalTransform = false;
	// S50: R3 ganz abschaltbar, siehe Schalter oben.
	const bool palTransformEnabled = !s_noPalTransform;
	// R3.1 perf: the live palettes differ from the ROM reference in EVERY row
	// of EVERY level (the game post-processes palettes at load — the transform
	// therefore also corrects the HD tiles' global color fidelity, confirmed
	// visually). That defeats the "identical row → skip" fast path, so the
	// per-sample cost matters: precomputed 8×3×256 LUTs (6 KB, L1-resident)
	// replace per-sample multiply/shift/clamp with one table load per channel.
	uint8_t palLut[BgPalBlockCount][3][256];
	// S52: je BG-Palettenzeile ein Umfaerber, EINMAL je Frame gebaut. Init() ist
	// zu teuer fuer den Pixel (16 Farben), Apply() dagegen billig genug.
	HdSpriteRecolor bgRecolor[BgPalBlockCount];
	bool bgRecolorActive[BgPalBlockCount] = {};
	bool anyBgRecolor = false;
	if(!s_noBgRecolor && _hdData->ActiveGfxset >= 0 && !_hdData->GfxsetPalettes.empty()) {
		auto it = _hdData->GfxsetPalettes.find((uint8_t)_hdData->ActiveGfxset);
		if(it != _hdData->GfxsetPalettes.end() && it->second.size() >= 128) {
			for(int row = 0; row < BgPalBlockCount; row++) {
				const int start = BgPalBlockStart(row);
				bgRecolor[row].Init(it->second.data() + start, hdScreen->Cgram + start, BgPalBlockSize(row));
				// Init setzt valid nur, wenn sich ueberhaupt etwas unterscheidet --
				// bei ref == live ist Apply() die exakte Identitaet und der Umweg
				// waere reine Rechenzeit.
				bgRecolorActive[row] = bgRecolor[row].valid;
				anyBgRecolor |= bgRecolorActive[row];
				// S54: nur 4bpp-Zeilen -- bei 3 Farben lohnt kein Gitter.
				if(bgRecolorActive[row] && !s_noRecolorGrid && BgPalBlockSize(row) == 16) {
					bgRecolor[row].grid = GetRecolorGrid(_hdData->ActiveGfxset, row, bgRecolor[row]);
				}
			}
		}
	}
	if(palTransformEnabled && _hdData->ActiveGfxset >= 0 && !_hdData->GfxsetPalettes.empty()) {
		auto palIt = _hdData->GfxsetPalettes.find((uint8_t)_hdData->ActiveGfxset);
		if(palIt != _hdData->GfxsetPalettes.end() && palIt->second.size() >= 128) {
			const uint16_t* ref = palIt->second.data();
			for(int row = 0; row < BgPalBlockCount; row++) {
				const uint16_t* refRow = ref + BgPalBlockStart(row);
				const uint16_t* liveRow = hdScreen->Cgram + BgPalBlockStart(row);
				const int rowSize = BgPalBlockSize(row);
				// Index 0 of each row is the transparent color — not part of
				// any visible tile pixel, so it is excluded from comparison.
				uint32_t refSum[3] = {}, liveSum[3] = {};
				bool differs = false;
				for(int i = 1; i < rowSize; i++) {
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
		diagSprAreaPx.clear();
		diagSprAreaPal.clear();
		diagSprAreaLogged = false;
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
	renderCtx.smoothEdges = _emu->GetSettings()->GetSnesConfig().HdSmoothSpriteEdges;
	renderCtx.blockBgHd = isWorldmap && !_hdData->HasFingerprints();
	renderCtx.vramSig = vramSig;
	renderCtx.anyPalTransform = anyPalTransform;
	renderCtx.palRowActive = palRowActive;
	renderCtx.palLut = palLut;
	// S52: nur anbinden, wenn wirklich eine Zeile abweicht -- sonst bleibt der
	// Zeiger null und der Pixelpfad nimmt unveraendert den alten Weg.
	renderCtx.bgRecolor = anyBgRecolor ? (const void*)bgRecolor : nullptr;
	renderCtx.bgRecolorActive = bgRecolorActive;

	HdFilterFrameStats statsSlots[HdFilterWorkPool::MaxWorkers];
	HdFilterFrameStats callerStats;
	std::chrono::steady_clock::time_point filterT0 = std::chrono::steady_clock::now();
	GetHdFilterPool().RunFrame(renderCtx, overscan.Top, 239 - overscan.Bottom, statsSlots, callerStats);
	// R6.1: filter time for this frame — logged in the FRAME line (ms=cur/max).
	// Budget is 16.7 ms; frames above it stall the emu thread (P4.1b wait).
	const std::chrono::steady_clock::time_point perfRenderT1 = std::chrono::steady_clock::now();
	double filterMs = std::chrono::duration<double, std::milli>(perfRenderT1 - filterT0).count();
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

	// =====================================================================
	// S48: Framebuffer-Dump (siehe Kommentar an den Schaltern oben).
	// Steht hier, weil RunFrame die Render-Threads bereits eingesammelt hat --
	// der Ausgabepuffer ist ab dieser Zeile fertig und wird nicht mehr
	// angefasst. Kostet ausserhalb eines Dumps genau einen Integer-Vergleich.
	// =====================================================================
	if(s_dumpFrames > 0) {
		static int s_dumpDone = 0;
		static int s_dumpWait = 0;
		const int activeSet = _hdData ? _hdData->ActiveGfxset : -1;
		// BgPixels als Anzeichen fuer ein echtes Levelbild: beim Levelwechsel
		// liefert der Filter reihenweise leere Frames (bg=0), und ein schwarzes
		// PNG beantwortet keine Frage.
		const bool realFrame = activeSet >= 0
			&& (s_dumpGfxset < 0 || activeSet == s_dumpGfxset)
			&& total.BgPixels > 20000;
		if(s_dumpDone < s_dumpFrames && realFrame) {
			if(s_dumpWait > 0) {
				s_dumpWait--;
			} else {
				const char* home = getenv("USERPROFILE");
				if(!home) home = getenv("HOME");
				if(home) {
					char path[600];
					snprintf(path, sizeof(path),
#ifdef _WIN32
						"%s\\Downloads\\snes_hd_frame_gfx%02d_%02d.png",
#else
						"%s/Downloads/snes_hd_frame_gfx%02d_%02d.png",
#endif
						home, activeSet, s_dumpDone);
					bool ok = PNGHelper::WritePNG(path, outputBuffer, frameInfo.Width, frameInfo.Height);
					char buf[720];
					snprintf(buf, sizeof(buf),
						"[SNES HD diag] FRAMEDUMP %s %s (%ux%u, gfxset=%d, bg=%u, hdBG3=%u)",
						ok ? "geschrieben:" : "FEHLGESCHLAGEN:", path,
						frameInfo.Width, frameInfo.Height, activeSet,
						total.BgPixels, total.HdLayers[2]);
					DiagLog(buf);
					s_dumpDone++;
					s_dumpWait = s_dumpEvery;
				}
			}
		}
	}
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
	uint32_t frameSprEdge = total.SprEdge;
	uint32_t frameSprEdgeBlend = total.SprEdgeBlend;
	uint32_t frameSprEdgeUnder = total.SprEdgeUnder;
	uint32_t frameSprEdgeTie = total.SprEdgeTie;
	uint32_t frameSprEdgeTieWon = total.SprEdgeTieWon;
	uint32_t frameSubBgBot = total.SubBgBot;
	uint32_t frameSubBgBotRetry = total.SubBgBotRetry;
	uint32_t frameSubBgNoBot = total.SubBgNoBot;
	uint32_t frameSubBgOpaque = total.SubBgOpaque;
	uint32_t frameSubSprBot = total.SubSprBot;
	uint32_t frameSubSprUnder = total.SubSprUnder;
	uint32_t frameSubSprNoBot = total.SubSprNoBot;
	uint32_t frameSprHoleHd = total.SprHoleHd;
	uint32_t frameSubGateNoRef = total.SubGateNoRef;
	uint32_t frameSubGateOpaque = total.SubGateOpaque;
	uint32_t frameSubNoBotEmpty = total.SubNoBotEmpty;
	uint32_t frameSubNoBotNoHd = total.SubNoBotNoHd;
	uint32_t frameSubBotRetry = total.SubBotRetry;
	uint32_t frameLayerBits[4];
	uint32_t frameWin[4];
	uint32_t frameHdLayers[4];
	uint32_t frameDrawn[4], frameSkipClip[4], frameSkipNoSmp[4], frameSkipA0[4], frameSkipSubOp[4];
	for(int li = 0; li < 4; li++) {
		frameLayerBits[li] = total.LayerBits[li];
		frameWin[li] = total.Win[li];
		frameHdLayers[li] = total.HdLayers[li];
		frameDrawn[li] = total.DrawnLayer[li];
		frameSkipClip[li] = total.SkipClip[li];
		frameSkipNoSmp[li] = total.SkipNoSmp[li];
		frameSkipA0[li] = total.SkipAlpha0[li];
		frameSkipSubOp[li] = total.SkipSubOp[li];
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
		(diagFrameCount < 10 || (frameBgPixels > 0 && diagBgFrameCount < s_diagFrames));
	if(logThisFrame) {
		const char* ctxLabel = isWorldmap ? "WORLDMAP" : (isLevel2 ? "LEVEL2" : "other");
		char buf[1024];
		snprintf(buf, sizeof(buf),
			"[SNES HD diag] FRAME %d/%d [%s] build=" SNES_HD_BUILD_VERSION
			": total=%u bg=%u match=%u miss=%u hdCm=%u mNat=%u sHd=%u sFix=%u lRetry=%u multi=%u"
			" sprWon=%u sprHd=%u sprSub=%u sprSubHd=%u sprHdSub=%u sprRecol=%u sprNoRef=%u"
			" sprEdge=%u/%u sprUnder=%u sprFrTie=%u sprFrTieWon=%u subBot=%u subSprUnder=%u subNoBot=%u sprHoleHd=%u subNoRef=%u subOpaque=%u nbEmpty=%u nbNoHd=%u subBotRetry=%u"
			" bgBot=%u bgBotRetry=%u bgNoBot=%u bgOpaque=%u"
			" mask0=%u hdmaSplit=%u ms=%.2f/%.2f"
			" BG1=%u BG2=%u BG3=%u BG4=%u"
			" hdBG1=%u hdBG2=%u hdBG3=%u hdBG4=%u"
			" drawBG1=%u drawBG2=%u drawBG3=%u drawBG4=%u"
			" wn0=%u wn1=%u wn2=%u wn3=%u"
			" Main=$%02X Sub=$%02X CM=$%02X"
			" (TileByKey=%zu, sig=%016llX)",
			diagFrameCount, diagBgFrameCount, ctxLabel,
			frameTotalPixels, frameBgPixels, frameHdMatch,
			frameHdMiss, frameHdCm, frameMainNatHd, frameSubOpHd, frameSubOpFixed, frameLayerRetry, frameMultiLayer,
			frameSpriteWon, frameSprHd, frameSprSub, frameSprSubMainHd, frameSprSubHd,
			frameSprRecolor, frameSprRecolorNoRef,
			frameSprEdge, frameSprEdgeBlend, frameSprEdgeUnder,
			frameSprEdgeTie, frameSprEdgeTieWon, frameSubSprBot,
			frameSubSprUnder, frameSubSprNoBot, frameSprHoleHd, frameSubGateNoRef, frameSubGateOpaque, frameSubNoBotEmpty, frameSubNoBotNoHd, frameSubBotRetry,
			frameSubBgBot, frameSubBgBotRetry, frameSubBgNoBot, frameSubBgOpaque,
			frameMaskZero, frameHdmaSplit,
			filterMs, diagMsMax,
			frameLayerBits[0], frameLayerBits[1], frameLayerBits[2], frameLayerBits[3],
			frameHdLayers[0], frameHdLayers[1], frameHdLayers[2], frameHdLayers[3],
			frameDrawn[0], frameDrawn[1], frameDrawn[2], frameDrawn[3],
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
			char palBuf[1024];
			int off = snprintf(palBuf, sizeof(palBuf),
				"[SNES HD diag] PALDIFF gfx=%d rows:", (int)_hdData->ActiveGfxset);
			// S53: P = 4bpp-Zeile, Q = 2bpp-Block (Nummer = CGRAM-Start / 4).
			for(int row = 0; row < BgPalBlockCount && off < (int)sizeof(palBuf) - 48; row++) {
				if(!palRowActive[row]) continue;
				off += snprintf(palBuf + off, sizeof(palBuf) - off,
					row < 8 ? " P%d=%u/%u/%u" : " Q%d=%u/%u/%u", row < 8 ? row : row - 8,
					palRatio[row][0], palRatio[row][1], palRatio[row][2]);
			}
			DiagLog(palBuf);

			diagPalLogCount++;
		}

		// =================================================================
		// S51: unabhaengig von anyPalTransform -- der Vergleichslauf mit
		// SNES_HD_NO_PAL_TRANSFORM=1 braucht diese Zeilen genauso, und dort ist
		// der Transform per Definition aus.
		if(_hdData->ActiveGfxset >= 0 && diagPalRowLogged != (int)_hdData->ActiveGfxset) {
			auto palIt2 = _hdData->GfxsetPalettes.find((uint8_t)_hdData->ActiveGfxset);
			if(palIt2 != _hdData->GfxsetPalettes.end() && palIt2->second.size() >= 128) {
				diagPalRowLogged = (int)_hdData->ActiveGfxset;
				const uint16_t* ref2 = palIt2->second.data();
				for(int row = 0; row < 8; row++) {
					char line[400];
					int o = snprintf(line, sizeof(line), "[SNES HD diag] PALROW%d REF", row);
					for(int i = 0; i < 16; i++) {
						o += snprintf(line + o, sizeof(line) - o, " %04X", ref2[row * 16 + i] & 0x7FFF);
					}
					o += snprintf(line + o, sizeof(line) - o, " | LIVE");
					for(int i = 0; i < 16; i++) {
						o += snprintf(line + o, sizeof(line) - o, " %04X", hdScreen->Cgram[row * 16 + i] & 0x7FFF);
					}
					DiagLog(line);
				}
			}
		}

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
	// S28: which sprite art actually covers the screen, and does it carry a
	// reference palette? Pixels per tile, largest first, with the reference and pack
	// status of each. Accumulates over a context, logs once.
	//
	// S43 changed what this measures. It was built when every edge mechanism was
	// gated on `GetSpriteRefPalette(...) != nullptr`, to name the tiles a level was
	// losing its smoothing on -- and it did that job: it is what showed that only
	// 33.1% / 51.9% of the sprite area in the world map contexts sat behind a
	// reference, which is why bypassing the gate there was a real test and not a
	// vacuous one. The gate is gone now, so `ref=` no longer decides whether a tile
	// is smoothed. It still says whether the tile follows the live CGRAM row or is
	// baked for one palette slot, which is what the recolor path turns on.
	// =====================================================================
	if(diagBgFrameCount >= (s_diagFrames * 3 / 4) && !diagSprAreaLogged && !diagSprAreaPx.empty()) {
		diagSprAreaLogged = true;
		std::vector<std::pair<uint64_t, uint32_t>> ranked(diagSprAreaPx.begin(), diagSprAreaPx.end());
		std::sort(ranked.begin(), ranked.end(),
			[](const std::pair<uint64_t, uint32_t>& a, const std::pair<uint64_t, uint32_t>& b) {
				return a.second > b.second;
			});
		uint64_t totalPx = 0, refPx = 0;
		for(auto& e : ranked) {
			totalPx += e.second;
			if(_hdData->GetSpriteRefPalette(e.first)) refPx += e.second;
		}
		char hdr[256];
		snprintf(hdr, sizeof(hdr),
			"[SNES HD diag] SPRAREA gfx=%d tiles=%zu px=%llu withRef=%llu (%.1f%% of area)",
			(int)_hdData->ActiveGfxset, ranked.size(),
			(unsigned long long)totalPx, (unsigned long long)refPx,
			totalPx ? 100.0 * (double)refPx / (double)totalPx : 0.0);
		DiagLog(hdr);
		// 40, not 16: in Rambi Rumble the first sixteen were all KONG letters -- one
		// letter is several 32x32 tiles and they sit still, so they dominate an area
		// ranking, while a character is many small tiles that each cover less. The
		// list has to reach past them to show the art the user is actually asking about.
		for(size_t i = 0; i < ranked.size() && i < 40; i++) {
			uint64_t h = ranked[i].first;
			uint8_t pal = diagSprAreaPal.count(h) ? diagSprAreaPal[h] : 0xFF;
			SnesHdTileKey k; k.ContentHash = h; k.PaletteIndex = pal; k.LayerIndex = 4;
			bool hasArt = _hdData->GetMatchingTile(k) != nullptr;
			char line[256];
			snprintf(line, sizeof(line),
				"[SNES HD diag] SPRAREA   #%02zu hash=%016llX px=%u pal=%u art=%d ref=%d",
				i + 1, (unsigned long long)h, ranked[i].second, pal,
				hasArt ? 1 : 0, _hdData->GetSpriteRefPalette(h) ? 1 : 0);
			DiagLog(line);
		}
	}

	// S35: the recorders (spritecap, bgcap, cgramcap, spritemiss, OAM) run from
	// here to the HDMA dump below — timed as one block, "rec" in snes_hd_perf.txt.
	const SnesHdPerf::Clock::time_point perfRecT0 = SnesHdPerf::Clock::now();

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
	if(!diagSprAreaLogged && diagBgFrameCount >= (s_diagFrames / 4)) {
		// S29: count the SUB-screen sprite too. The first version only looked at
		// sprites that won the MAIN screen -- and in an overlay level no sprite ever
		// does (`sprWon=0` in every Rambi and Mainbrace frame), so the recorder
		// collected nothing and never logged, in precisely the levels it was built
		// for. Starts a quarter into the window so the entry banner is not what gets
		// measured.
		constexpr uint32_t sprAreaPixels = (uint32_t)SnesHdScreenInfo::ScreenPixelCount;
		for(uint32_t i = 0; i < sprAreaPixels; i++) {
			const SnesHdPpuPixelInfo& pi = hdScreen->ScreenTiles[i];
			int slot = -1;
			if((pi.MainScreenFlags & 0x40) && (pi.SpriteCount & 0x01)) slot = 0;
			else if(pi.SubScreenHasSprite && (pi.SpriteCount & 0x02)) slot = 1;
			if(slot < 0) continue;
			uint64_t h = pi.Sprites[slot].Key.ContentHash;
			if(!h) continue;
			diagSprAreaPx[h]++;
			diagSprAreaPal[h] = pi.Sprites[slot].Key.PaletteIndex;
		}
	}

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

				// S40: open the file BEFORE asking whether an earlier session already
				// logged this key. S39 read this recorder as the healthy one because it
				// inserts after the open -- true, but it still had the second failure
				// that commit named and only fixed in spritemiss: the open depended on
				// the seeding's answer. With a 88 MB file the first candidate of a run
				// is almost always one an earlier session had, so `continue` fired with
				// the file still closed, s_spriteCapAttempted stayed true, and every
				// later tile died on the `!file` break. The last session banner in
				// snes_hd_spritecap.txt is 2026-08-10 21:24 build=S19 -- the day
				// SeedRecorderSet was added. Dead for five weeks, like the other two.
				if(!s_spriteCapAttempted) {
						s_spriteCapAttempted = true;
						SeedRecorderSet("snes_hd_spritecap.txt", s_spriteCapSeen, ParseSpriteCapKey);
						s_spriteCapFile = OpenRecorder("snes_hd_spritecap.txt");
					}
				if(!s_spriteCapFile) break;
				if(s_spriteCapSeen.find(setKey) != s_spriteCapSeen.end()) continue;   // earlier session had it
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
					// S14: ask the RENDER PATH whether this tile is covered, not the
					// raw map. SnesHdTileKey carries no gfxset — that lives on the
					// tile and is applied by GetMatchingTile's strict scoping — so a
					// bare TileByKey lookup counts a tile as covered even when the
					// current context blocks it and the screen shows plain SD. Those
					// tiles were invisible here: present in the pack, never matched,
					// never recorded. The sprite recorder below already does this.
					// S39: memo a COVERED tile here. See the sprite recorder below for
					// why the key must not enter the set before the seeding step.
					if(_hdData->GetMatchingTile(t.Key, hdScreen->Vram)) { s_bgCapSeen.insert(seenKey); continue; }  // covered

					// Verify live VRAM still matches the captured hash (2bpp
					// layers hash 8 words, 4bpp 16 words — same as the PPU).
					// Not memoed: a later frame must look at it again.
					const uint16_t words = (layer <= 1) ? 16 : 8;
					if(ComputeTileContentHash(hdScreen->Vram, t.VramWordAddr, words) != t.Key.ContentHash) continue;

					if(!s_bgCapAttempted) {
						s_bgCapAttempted = true;
						SeedRecorderSet("snes_hd_bgcap.txt", s_bgCapSeen, ParseBgCapKey);
						s_bgCapFile = OpenRecorder("snes_hd_bgcap.txt");
					}
					if(!s_bgCapFile) break;
					if(s_bgCapSeen.find(seenKey) != s_bgCapSeen.end()) continue;   // earlier session had it
					s_bgCapSeen.insert(seenKey);

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
				// S39: memo COVERED tiles here, not before the check. The old order
				// inserted the key first and then asked, after seeding, whether the
				// set already contained it — it always did, because that was our own
				// entry. So the "earlier session had it" branch fired on the very
				// first miss, OpenRecorder was never reached, and every later tile hit
				// the `!s_sprMissFile` break. This recorder and bgcap have been dead
				// since SeedRecorderSet was added on 10 Aug 2026. The files stopped
				// growing that day, which the comment further down took for
				// saturation — it was this. S40: spritecap did NOT survive either,
				// as this comment first claimed; it inserts after the open, but its
				// open hung on the seeding's answer, so it died the same day.
				if(_hdData->GetMatchingTile(t.Key)) { s_sprMissSeen.insert(missKey); continue; }  // covered → not a miss
				// Stale (OBJ stream replaced the bytes mid-frame): do NOT memo, so a
				// later frame looks at it again. Replaces the old insert/erase pair.
				if(ComputeTileContentHash(hdScreen->Vram, t.VramWordAddr, 16) != t.Key.ContentHash) continue;

				// Seed and open on the first real miss, while missKey is still absent
				// from the set. The open no longer depends on the seed's answer: a
				// first miss that an earlier session had already logged used to leave
				// the file closed for the whole run.
				if(!s_sprMissAttempted) {
					s_sprMissAttempted = true;
					SeedRecorderSet("snes_hd_spritemiss.txt", s_sprMissSeen, ParseSprMissKey);
					s_sprMissFile = OpenRecorder("snes_hd_spritemiss.txt");
				}
				if(!s_sprMissFile) break;
				if(s_sprMissSeen.find(missKey) != s_sprMissSeen.end()) continue;   // earlier session had it
				s_sprMissSeen.insert(missKey);

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
	// S41: positive sprite probe — the counterpart to spritemiss.
	//
	// spritemiss only records a tile that was CAPTURED and then found no art.
	// A tile that is never captured leaves no trace at all, so "not in
	// spritemiss" has been read as "covered" when it can equally mean "never
	// seen". Two objects hit that blind spot: the bonus barrel's B emblem
	// (gfxRef $3170) and the big white digits ($2D40-$2D64) are complete in the
	// pack, carry a reference palette identical to the live CGRAM row (so the
	// recolor is the exact identity and does not even run), and were missed in
	// neither of two runs — and are still reported as blocky on screen.
	//
	// Only two sprite tiles per pixel are captured (main + sub), plus the
	// displaced one as slot 3 and the fringe as slot 2; a small sprite stacked
	// on another one can lose every slot. This probe says which case it is:
	//
	//   SNES_HD_SPRWATCH=E563D7702FE967CC,38E78F2E2A1400EC
	//
	// Per frame and watched hash: pixels per capture slot, whether the pack has
	// art for the captured key, and whether the sprite actually won the pixel.
	// No line at all for a whole level means the tile never reaches the HD path,
	// and then the pack cannot be the reason it looks native.
	// =====================================================================
	{
		constexpr int MaxWatch = 8;
		static uint64_t s_watch[MaxWatch] = {};
		static int s_watchCount = -1;
		if(s_watchCount < 0) {
			s_watchCount = 0;
			if(const char* env = getenv("SNES_HD_SPRWATCH")) {
				const char* q = env;
				while(*q && s_watchCount < MaxWatch) {
					uint64_t h = 0; int n = 0;
					while(*q && *q != ',') {
						char c = *q++;
						int v = (c >= '0' && c <= '9') ? c - '0'
						      : (c >= 'A' && c <= 'F') ? c - 'A' + 10
						      : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1;
						if(v >= 0) { h = (h << 4) | (uint64_t)v; n++; }
					}
					if(n > 0) s_watch[s_watchCount++] = h;
					if(*q == ',') q++;
				}
				char msg[64];
				snprintf(msg, sizeof(msg), "SPRWATCH: %d Hash(es) beobachtet", s_watchCount);
				DiagLog(msg);
			}
		}

		if(s_watchCount > 0) {
			constexpr uint32_t px = (uint32_t)SnesHdScreenInfo::ScreenPixelCount;
			for(int wi = 0; wi < s_watchCount; wi++) {
				const uint64_t want = s_watch[wi];
				uint32_t slotPx[4] = {}, slotHd[4] = {}, wonMain = 0, wonSub = 0;
				uint8_t pal[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
				for(uint32_t i = 0; i < px; i++) {
					const SnesHdPpuPixelInfo& pi = hdScreen->ScreenTiles[i];
					if(!pi.SpriteCount) continue;
					for(int s = 0; s < 4; s++) {
						if(!(pi.SpriteCount & (1 << s))) continue;
						const SnesHdPpuTileInfo& t = pi.Sprites[s];
						if(t.Key.ContentHash != want) continue;
						slotPx[s]++;
						pal[s] = t.Key.PaletteIndex;
						if(_hdData->GetMatchingTile(t.Key)) slotHd[s]++;
						if(s == 0 && (pi.MainScreenFlags & 0x40)) wonMain++;
						if(s == 1 && pi.SubScreenHasSprite) wonSub++;
					}
				}
				if(slotPx[0] || slotPx[1] || slotPx[2] || slotPx[3]) {
					char line[256];
					snprintf(line, sizeof(line),
						"SPRWATCH F%u H%016llX main=%u/%u sub=%u/%u fringe=%u/%u under=%u/%u "
						"P%d/%d/%d/%d wonMain=%u wonSub=%u",
						hdScreen->FrameNumber, (unsigned long long)want,
						slotHd[0], slotPx[0], slotHd[1], slotPx[1],
						slotHd[2], slotPx[2], slotHd[3], slotPx[3],
						(int)(int8_t)pal[0], (int)(int8_t)pal[1], (int)(int8_t)pal[2], (int)(int8_t)pal[3],
						wonMain, wonSub);
					DiagLog(line);
				}
			}
		}
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
	// S39 NOTE: the sentence below about bgcap and spritemiss having "saturated back
	// on 10 Aug" was wrong. They stopped because SeedRecorderSet, added that day, was
	// consulted AFTER the tile's own key had been inserted — see the fix above. Their
	// keys really are per tile and finite, so they will saturate eventually; they had
	// simply never been given the chance.
	// S27: OFF by default (08 Sep). This recorder de-duplicates on an object's
	// COMPOSITION, and a composition is "which objects stand where" — so almost every
	// frame with a slightly different sprite arrangement counts as new material. Over
	// 42 sessions the file reached 853 MB and was still growing, while spritecap,
	// bgcap and spritemiss saturated back on 10 Aug and have not been written since:
	// their keys are per TILE, of which there is a finite number.
	// Kept, not deleted — the viewer's "Laufzeit" button reads it (`parseOam`) and it
	// is what defines which HD tiles form one runtime object (S16/S17). Turn it on for
	// the session in which objects are actually being captured, exactly as S18 did
	// with cgramcap:
	//     set SNES_HD_OAMCAP=1
	static const bool s_oamEnabled = getenv("SNES_HD_OAMCAP") != nullptr;
	if(s_oamEnabled && hdScreen->Oam && hdScreen->Vram) {
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

	const SnesHdPerf::Clock::time_point perfRecT1 = SnesHdPerf::Clock::now();

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

			// S45: gefunden ist nicht gezeichnet. Diese Zeile ist der eigentliche
			// Befund -- steht hdMatch hoch und drawn bei null, nennt der Grund
			// rechts daneben die Stelle im Code, die es verwirft.
			if(L < 4 && frameHdLayers[L] > 0) {
				char drawBuf[512];
				snprintf(drawBuf, sizeof(drawBuf),
					"       -> davon GEZEICHNET: %u (%d%%)  | verworfen: clip=%u noSampler=%u alpha0=%u subOp=%u"
					"  [Summe=%u, muss hdMatch=%u sein]",
					frameDrawn[L],
					(int)(100ULL * frameDrawn[L] / frameHdLayers[L]),
					frameSkipClip[L], frameSkipNoSmp[L], frameSkipA0[L], frameSkipSubOp[L],
					frameDrawn[L] + frameSkipClip[L] + frameSkipNoSmp[L] + frameSkipA0[L]
						+ frameSkipSubOp[L],
					frameHdLayers[L]);
				DiagLog(drawBuf);
			}
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

	// S35: hand this frame's timing to the collector. pre = everything before the
	// pixel loop (context detection, fingerprints, palette LUT), render = the loop,
	// rec = the recorders, post = the rest (counter sums, diagnostics, log writes).
	if(SnesHdPerf::Enabled()) {
		const SnesHdPerf::Clock::time_point perfT1 = SnesHdPerf::Clock::now();
		SnesHdPerf::FilterFrame pf;
		pf.Total = SnesHdPerf::Ms(perfT0, perfT1);
		pf.Pre = SnesHdPerf::Ms(perfT0, filterT0);
		pf.Render = filterMs;
		pf.Rec = SnesHdPerf::Ms(perfRecT0, perfRecT1);
		pf.Post = pf.Total - pf.Pre - pf.Render - pf.Rec;
		pf.SprWon = frameSpriteWon;
		pf.SprHd = frameSprHd;
		pf.SprSub = frameSprSub;
		pf.SprHdSub = frameSprSubHd;
		pf.Sig = vramSig;
		SnesHdPerf::AddFilterFrame(pf, SNES_HD_BUILD_VERSION);
	}
}
