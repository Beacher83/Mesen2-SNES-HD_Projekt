#pragma once
// ---------------------------------------------------------------------------
// S35: where does a slow frame spend its time?
//
// The FRAME line's ms= times only the parallel pixel loop of the HD filter, and
// only during the first 600 frames of each context -- the level entry. The busy
// scenes later in a level, where the game is felt to stutter, were never measured
// at all, and neither was anything around that loop.
//
// A frame leaves the emulation thread through SnesPpu::SendFrame ->
// VideoDecoder::UpdateFrame, which SPINS while the decode thread is still busy
// with the previous frame. So a frame is late when either side runs over:
//   emulation thread:  CPU + PPU (with the HD per-pixel capture) -> SendFrame
//                      (wait for the decoder, then clear 16 MB of pixel info)
//                      -> frame limiter sleep
//   decode thread:     the whole HD filter (context detection, pixel loop,
//                      recorders, diagnostics)
// Both sides feed this collector. The emulation thread writes one line per
// second to %USERPROFILE%\Downloads\snes_hd_perf.txt, plus one SLOW line for
// every frame whose work does not fit the frame, with that frame's breakdown and
// the most recent filter frame's. Always on while an HD pack is active (a few
// hundred clock reads per frame); SNES_HD_PERF=1 forces it on without a pack,
// for an A/B run with HD disabled.
// ---------------------------------------------------------------------------
#include <chrono>
#include <mutex>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <ctime>

namespace SnesHdPerf
{
	using Clock = std::chrono::steady_clock;

	inline double Ms(Clock::time_point a, Clock::time_point b)
	{
		return std::chrono::duration<double, std::milli>(b - a).count();
	}

	// One NTSC frame. Work above this cannot keep 60 fps.
	constexpr double FrameBudgetMs = 16.64;
	// A frame this far over gets its own SLOW line.
	constexpr double SlowLineMs = 20.0;
	// S36: work is not the only way a frame arrives late. The limiter may oversleep,
	// the OS may deschedule the thread, the present may block -- none of that is work,
	// and none of it showed up until the period was counted as well.
	//   late    -- the frame took longer than a budget plus a fifth
	//   dropped -- over one and a half budgets: the display repeats the previous frame
	constexpr double LateFrameMs = 20.0;
	constexpr double DroppedFrameMs = 25.0;
	constexpr int MaxSlowLinesPerSecond = 6;

	struct Acc
	{
		double Sum = 0, Max = 0;
		uint32_t N = 0;
		void Add(double v) { Sum += v; N++; if(v > Max) Max = v; }
		double Avg() const { return N ? Sum / N : 0.0; }
	};

	struct FilterFrame
	{
		double Total = 0, Pre = 0, Render = 0, Rec = 0, Post = 0;
		uint32_t SprWon = 0, SprHd = 0, SprSub = 0, SprHdSub = 0;
		uint64_t Sig = 0;
	};

	struct EmuFrame
	{
		double Period = 0;   // end of last frame -> end of this one (what the player sees)
		double Emu = 0;      // frame start -> SendFrame: CPU + PPU emulation
		double Scan = 0;     //   of that: PPU scanline rendering incl. HD capture
		double Wait = 0;     // SendFrame spinning for the decode thread
		double Clear = 0;    // SendFrame clearing the next frame's pixel info
		double Send = 0;     // all of SendFrame (wait + clear + the rest)
		double Sleep = 0;    // frame limiter + end-of-frame work after SendFrame
		double Work() const { return Emu + Send; }
	};

	struct State
	{
		std::mutex Lock;
		Acc Period, Work, Emu, Scan, Wait, Clear, Sleep;
		Acc FTotal, FPre, FRender, FRec, FPost;
		Acc SprWon, SprHd, SprSub;
		uint32_t Frames = 0, Over = 0, Late = 0, Dropped = 0;
		FilterFrame LastFilter;
		// Handed over by the filter with every frame, so the build tag keeps one
		// definition (SNES_HD_BUILD_VERSION in SnesHdVideoFilter.cpp).
		const char* Build = "? (HD filter not running)";
		FILE* File = nullptr;
		bool Opened = false;
		bool Started = false;
		Clock::time_point SessionStart, SecondStart;
		int SlowLines = 0;
	};

	inline State& Get()
	{
		static State s;
		return s;
	}

	inline bool Forced()
	{
		static const bool forced = getenv("SNES_HD_PERF") != nullptr;
		return forced;
	}

	// Caller holds the lock.
	inline FILE* File(State& s)
	{
		const char* build = s.Build;
		if(!s.Opened) {
			s.Opened = true;
			const char* home = getenv("USERPROFILE");
			if(!home) home = getenv("HOME");
			if(home) {
				char path[512];
#ifdef _WIN32
				snprintf(path, sizeof(path), "%s\\Downloads\\snes_hd_perf.txt", home);
#else
				snprintf(path, sizeof(path), "%s/Downloads/snes_hd_perf.txt", home);
#endif
				s.File = fopen(path, "a");
				if(s.File) {
					time_t now = time(nullptr);
					struct tm lt {};
#ifdef _WIN32
					localtime_s(&lt, &now);
#else
					localtime_r(&now, &lt);
#endif
					fprintf(s.File, "=== SESSION %04d-%02d-%02d %02d:%02d:%02d build=%s | frame budget %.2f ms"
						" | PERF = one line per second (avg/max) | SLOW = one frame whose work exceeded %.0f ms ===\n",
						lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, lt.tm_hour, lt.tm_min, lt.tm_sec,
						build, FrameBudgetMs, SlowLineMs);
					fflush(s.File);
				}
			}
		}
		return s.File;
	}

	// Wall-clock time of day, so a line can be matched to "it stuttered at 21:42".
	inline void TimeOfDay(char* out, size_t n)
	{
		time_t now = time(nullptr);
		struct tm lt {};
#ifdef _WIN32
		localtime_s(&lt, &now);
#else
		localtime_r(&now, &lt);
#endif
		snprintf(out, n, "%02d:%02d:%02d", lt.tm_hour, lt.tm_min, lt.tm_sec);
	}

	// Decode thread, once per filtered frame.
	inline void AddFilterFrame(const FilterFrame& f, const char* build)
	{
		State& s = Get();
		std::lock_guard<std::mutex> g(s.Lock);
		s.Build = build;
		s.FTotal.Add(f.Total);
		s.FPre.Add(f.Pre);
		s.FRender.Add(f.Render);
		s.FRec.Add(f.Rec);
		s.FPost.Add(f.Post);
		s.SprWon.Add(f.SprWon);
		s.SprHd.Add(f.SprHd);
		s.SprSub.Add(f.SprSub);
		s.LastFilter = f;
	}

	// Emulation thread, once per frame. Also the writer.
	inline void AddEmuFrame(const EmuFrame& e)
	{
		State& s = Get();
		std::lock_guard<std::mutex> g(s.Lock);
		Clock::time_point now = Clock::now();
		if(!s.Started) {
			s.Started = true;
			s.SessionStart = now;
			s.SecondStart = now;
		}
		double t = Ms(s.SessionStart, now) / 1000.0;
		double work = e.Work();
		s.Frames++;
		if(work > FrameBudgetMs) s.Over++;
		if(e.Period > LateFrameMs) s.Late++;
		if(e.Period > DroppedFrameMs) s.Dropped++;
		s.Period.Add(e.Period);
		s.Work.Add(work);
		s.Emu.Add(e.Emu);
		s.Scan.Add(e.Scan);
		s.Wait.Add(e.Wait);
		s.Clear.Add(e.Clear);
		s.Sleep.Add(e.Sleep);

		const bool slowWork = work > SlowLineMs;
		const bool slowPeriod = e.Period > DroppedFrameMs;
		if((slowWork || slowPeriod) && s.SlowLines < MaxSlowLinesPerSecond) {
			if(FILE* f = File(s)) {
				s.SlowLines++;
				const FilterFrame& lf = s.LastFilter;
				const char* why = slowWork ? (slowPeriod ? "work+period" : "work") : "period";
				char tod[16];
				TimeOfDay(tod, sizeof(tod));
				fprintf(f, "SLOW %s t=%.3f why=%s work=%.1f period=%.1f | emu=%.1f (scan=%.1f) wait=%.1f clear=%.1f sleep=%.1f"
					" | last filter: total=%.1f pre=%.1f render=%.1f rec=%.1f post=%.1f"
					" sprWon=%u sprHd=%u sprSub=%u sig=%016llX\n",
					tod, t, why, work, e.Period, e.Emu, e.Scan, e.Wait, e.Clear, e.Sleep,
					lf.Total, lf.Pre, lf.Render, lf.Rec, lf.Post,
					lf.SprWon, lf.SprHd, lf.SprSub, (unsigned long long)lf.Sig);
			}
		}

		if(Ms(s.SecondStart, now) >= 1000.0) {
			if(FILE* f = File(s)) {
				char tod[16];
				TimeOfDay(tod, sizeof(tod));
				fprintf(f, "PERF %s t=%.0f fps=%u over=%u late=%u drop=%u | period=%.1f/%.1f work=%.1f/%.1f emu=%.1f/%.1f scan=%.1f/%.1f"
					" wait=%.1f/%.1f clear=%.1f/%.1f sleep=%.1f/%.1f"
					" | filter=%.1f/%.1f pre=%.1f/%.1f render=%.1f/%.1f rec=%.1f/%.1f post=%.1f/%.1f"
					" | sprWon=%.0f/%.0f sprHd=%.0f/%.0f sprSub=%.0f/%.0f sig=%016llX\n",
					tod, t, s.Frames, s.Over, s.Late, s.Dropped,
					s.Period.Avg(), s.Period.Max, s.Work.Avg(), s.Work.Max, s.Emu.Avg(), s.Emu.Max, s.Scan.Avg(), s.Scan.Max,
					s.Wait.Avg(), s.Wait.Max, s.Clear.Avg(), s.Clear.Max, s.Sleep.Avg(), s.Sleep.Max,
					s.FTotal.Avg(), s.FTotal.Max, s.FPre.Avg(), s.FPre.Max, s.FRender.Avg(), s.FRender.Max,
					s.FRec.Avg(), s.FRec.Max, s.FPost.Avg(), s.FPost.Max,
					s.SprWon.Avg(), s.SprWon.Max, s.SprHd.Avg(), s.SprHd.Max, s.SprSub.Avg(), s.SprSub.Max,
					(unsigned long long)s.LastFilter.Sig);
				fflush(f);
			}
			s.Period = s.Work = s.Emu = s.Scan = s.Wait = s.Clear = s.Sleep = Acc();
			s.FTotal = s.FPre = s.FRender = s.FRec = s.FPost = Acc();
			s.SprWon = s.SprHd = s.SprSub = Acc();
			s.Frames = s.Over = s.Late = s.Dropped = 0;
			s.SlowLines = 0;
			s.SecondStart = now;
		}
	}
}
