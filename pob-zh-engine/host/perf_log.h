// Opt-in performance diagnostics for the POB window: PobTools\logs\perf-<date>.log
//
// Why this exists: a user's POB sat at 87% of a 3060 Ti with the CPU at 0.9%,
// and nothing we could do on our own machines reproduced it (2026-09-17). The
// only way forward was data from THEIR machine, and a screenshot of Task
// Manager is not data. So the engine can write down, on request, what it was
// doing -- and, just as importantly, WHERE the user was when it did it.
//
// What a report has to answer, and the part of the file that answers it:
//
//   * WHICH GPU, WHAT SCREEN, WHAT SETTINGS  -> the HEAD lines (GL_RENDERER,
//     framebuffer, refresh, fps caps, appearance values)
//   * WHAT PAGE / STATE WAS EXPENSIVE        -> every SEC line carries the POB
//     mode, build tab, popups, tree zoom/drag, focus, minimised/occluded; the
//     SUMMARY groups all seconds by page+state and sorts by GPU cost
//   * WHAT THE USER DID JUST BEFORE          -> EVT lines, written the moment a
//     state changes (tab switch, focus lost, drag started, recalculation...)
//   * WHICH PART OF A FRAME                  -> CPU per section (Lua per page,
//     layers, glass, hash, blit, swap, translation) and GPU per section from
//     timer queries; SPIKE lines break down single slow frames
//   * WHY IT KEEPS REDRAWING                 -> why a frame was not elided and
//     which draw layers changed
//
// Rules:
//   * OFF BY DEFAULT, ZERO COST OFF. Every entry point returns on one bool.
//   * NEVER CHANGES WHAT IT MEASURES. No glFinish, no waiting on GPU queries.
//   * BOUNDED. At most kMaxBytes or kMaxSeconds per process, then one LIMIT
//     line; the SUMMARY is still written at exit, it is the most useful part.
//   * NOTHING PRIVATE. Numbers, page names and the GPU name only -- no build
//     contents, no account names, no file paths beyond "a background is set".
//
// The Recorder is pure (clock and sink injected) so --perf-selftest drives it
// with a fake clock; the engine uses the global one through the free functions.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace PerfLog {

constexpr size_t kMaxBytes   = 10u * 1024u * 1024u;
constexpr double kMaxSeconds = 2.0 * 60.0 * 60.0;
constexpr double kSpikeMs    = 50.0;   // a frame this slow on the CPU is always a SPIKE
constexpr int    kMaxSpikesPerSecond = 3;

enum Cpu {
	CpuLua,        // POB's OnFrame, all of it
	CpuSubscripts, // POB subscripts (downloads, imports)
	CpuHash,       // frame elision digest
	CpuLayers,     // replaying the draw layers, glass included
	CpuGlass,      // liquid-glass passes (subset of layers)
	CpuBlit,       // present blit + ImGui
	CpuSwap,       // SwapBuffers
	CpuCapSleep,   // frame-cap sleep
	CpuTranslate,  // dictionary lookups for drawn strings
	kCpuCount
};

enum Gpu {
	GpuLayers,     // layers excluding glass
	GpuGlass,
	GpuPresent,    // present blit + ImGui
	kGpuCount
};

// Why a frame was drawn instead of elided.
enum DrawReason {
	DrawElided = 0,
	DrawHashChanged,
	DrawInhibited,    // textures still loading
	DrawAppearance,   // an appearance value changed
	DrawFirst,
	DrawNoElision,    // elision disabled
	kDrawReasonCount
};

enum Counter {
	CountDrawCmds,
	CountStrings,
	CountTranslations,
	CountGlassRects,
	kCounterCount
};

struct State {
	std::string mode;        // LIST / BUILD / ... ("" = unknown)
	std::string view;        // TREE / ITEMS / ... (BUILD only)
	int  popups = 0;
	bool dropdown = false;   // a dropdown/context menu is open
	int  treeZoom = -1;      // -1 = not on the tree
	bool treeDragging = false;
	bool treeSearch = false;
	bool active = false;     // focus
	bool cursorOver = false;
	bool minimized = false;
	bool occluded = false;
	bool coroutine = false;  // coroutine or subscript running
	int  texAsync = 0;       // textures waiting to load
	int  luaKB = 0;
	int  fbW = 0, fbH = 0;
	int  fpsCap = 0;         // limit in force this second

	bool operator==(const State& o) const;
	// Grouping key for the summary: page + focus + popups + minimised.
	std::string GroupKey() const;
};

struct ProcStats {
	double workingSetMB = -1;
	double privateMB = -1;
	double cpuPct = -1;      // of all cores
};

using Clock = std::function<double()>;                 // seconds, monotonic
using Sink  = std::function<void(const std::string&)>; // one line, no newline

class Recorder {
public:
	Recorder(Clock clock, Sink sink);

	void Header(const std::vector<std::pair<std::string, std::string>>& kv);

	void FrameBegin();
	void AddCpu(Cpu sec, double ms);
	void Mark(const std::string& page, double ms);     // Lua-side per-page draw time
	void Count(Counter c, long n = 1);
	void LayerChanged(int layer, int subLayer);
	void FrameEnd(DrawReason why, bool presented);
	void IdleGated(double sleptMs);

	void AddGpu(Gpu sec, double ms);                   // whenever a query result arrives
	void GpuDisjoint();                                // drop this second's GPU samples
	void SetGpuSupported(bool yes) { gpuSupported_ = yes; }

	void SetState(const State& s);                     // writes EVT lines for what changed
	void Event(const std::string& text);               // free-form EVT

	bool SecondDue() const;                            // a SEC line is due (sample ProcStats only then)
	void Tick(const ProcStats& ps);                    // closes the second if due
	void Finish();                                     // SUMMARY (once)

	bool Limited() const { return limited_; }
	size_t BytesWritten() const { return bytes_; }

private:
	struct Stat { double sum = 0, max = 0; long n = 0; void Add(double v) { sum += v; if (v > max) max = v; n++; } };
	struct Second {
		double start = 0;
		long frames = 0, presented = 0, idleGated = 0;
		long reason[kDrawReasonCount] = {};
		double idleMs = 0;
		Stat cpu[kCpuCount];
		double gpu[kGpuCount] = {};
		long gpuSamples = 0;
		bool gpuDisjoint = false;
		long counters[kCounterCount] = {};
		std::map<std::pair<int, int>, long> layers;
		std::map<std::string, Stat> pages;
		int spikes = 0;
	};
	struct Group {
		double seconds = 0;
		long frames = 0, drawn = 0, presented = 0;
		double cpu[kCpuCount] = {};
		double gpu[kGpuCount] = {};
		double gpuSeconds = 0;       // seconds that had GPU samples
		std::map<std::string, double> pages;
	};

	void Emit(const std::string& line, bool force = false);
	void CloseSecond(const ProcStats& ps);
	std::string Stamp() const;
	static std::string Fmt(double v, int prec = 1);

	Clock clock_;
	Sink sink_;
	double t0_ = 0;
	size_t bytes_ = 0;
	bool limited_ = false;
	bool finished_ = false;
	bool gpuSupported_ = false;

	Second sec_;
	double frameCpu_[kCpuCount] = {};
	bool inFrame_ = false;
	State state_;
	bool haveState_ = false;
	std::map<std::string, Group> groups_;
};

// ---- the engine's global recorder ------------------------------------------------

// POB_ZH_PERFLOG=1. Read once.
bool Enabled();
// Creates the file-backed recorder if enabled (idempotent).
Recorder* Get();
// Samples working set / private bytes / process CPU.
ProcStats SampleProcess();
// Writes the summary and flushes; safe to call when disabled.
void Shutdown();

// CPU timing of a scope; no-op when disabled.
class Scope {
public:
	explicit Scope(Cpu sec);
	~Scope();
	Scope(const Scope&) = delete;
	Scope& operator=(const Scope&) = delete;
private:
	Cpu sec_;
	long long start_ = 0;
	bool on_ = false;
};

// High-resolution now in milliseconds (QueryPerformanceCounter).
double NowMs();

// Window facts for the state tags: minimised, and "occluded" meaning another
// top-level window (typically the game, fullscreen) covers it entirely.
void WindowFacts(void* hwnd, bool& minimized, bool& occluded);
// "Windows 10 Pro 22H2 (build 19045)" from the registry; "" if unreadable.
std::string OsDescription();

// Redirect the file for a self-test ("" = back to PobTools\logs).
void SetPathForTest(const std::wstring& path);
std::wstring LogPathForToday();

} // namespace PerfLog
