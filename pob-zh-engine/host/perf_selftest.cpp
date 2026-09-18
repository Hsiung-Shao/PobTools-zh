// --perf-selftest: the POB window's frame cap and present rules as a table, and
// the performance log's recorder driven by a fake clock. Both run inside the
// engine's frame loop, where a mistake shows up as a stuck picture, a
// spinning core or a log that points at the wrong page -- none of which a
// user reports precisely, so they are pinned down here.

#include "perf_log.h"
#include "pob_frame_cap.h"

#include "error_log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cmath>
#include <string>
#include <vector>

namespace {

std::string g_report;
int g_fail = 0;

void line(const std::string& s) { g_report += s; g_report += "\r\n"; }

void check(const std::string& what, bool ok, const std::string& detail = "")
{
	if (!ok) g_fail++;
	line(std::string(ok ? "PASS " : "FAIL ") + what + (detail.empty() ? "" : "  (" + detail + ")"));
}

bool approx(double a, double b, double eps = 1e-6) { return std::fabs(a - b) <= eps; }

bool contains(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

struct Harness {
	double now = 100.0;
	std::vector<std::string> lines;
	PerfLog::Recorder rec{ [this] { return now; }, [this](const std::string& l) { lines.push_back(l); } };

	std::string All() const
	{
		std::string s;
		for (const auto& l : lines) s += l + "\n";
		return s;
	}
	int CountPrefix(const std::string& p) const
	{
		int n = 0;
		for (const auto& l : lines) if (l.compare(0, p.size(), p) == 0) n++;
		return n;
	}
	std::string Last(const std::string& p) const
	{
		for (auto it = lines.rbegin(); it != lines.rend(); ++it)
			if (it->compare(0, p.size(), p) == 0) return *it;
		return std::string();
	}
	double secondStart = 100.0;
	// one frame of `ms` CPU in Lua, drawn or elided; time advances inside the
	// current second only (EndSecond closes it exactly on the boundary)
	void Frame(double luaMs, PerfLog::DrawReason why, bool presented, double advance = 0.05)
	{
		rec.FrameBegin();
		rec.AddCpu(PerfLog::CpuLua, luaMs);
		rec.FrameEnd(why, presented);
		now += advance;
	}
	void EndSecond()
	{
		now = secondStart + 1.0;
		rec.Tick(PerfLog::ProcStats());
		secondStart = now;
	}
};

PerfLog::State Page(const char* mode, const char* view, bool active)
{
	PerfLog::State s;
	s.mode = mode;
	s.view = view;
	s.active = active;
	s.fbW = 1920;
	s.fbH = 1080;
	s.fpsCap = active ? 60 : 15;
	return s;
}

} // namespace

int RunPerfSelfTest(const std::wstring& exeDir)
{
	using namespace PobFrameCap;
	g_report.clear();
	g_fail = 0;
	line("POB frame cap + performance log self-test");
	line("");

	// ---- C: frame cap ----------------------------------------------------------
	{
		CapInputs c;
		c.active = true;
		c.foregroundFps = 60;
		c.backgroundFps = 15;
		c.frameStart = 10.0;
		c.now = 10.004;
		check("C1 foreground 60 fps: sleeps the rest of 16.7 ms", approx(SleepSeconds(c), 1.0 / 60 - 0.004, 1e-9),
		      std::to_string(SleepSeconds(c)));
		c.active = false;
		check("C2 background uses the background cap", approx(SleepSeconds(c), 1.0 / 15 - 0.004, 1e-9));
		c.active = true;
		c.now = 10.030;
		check("C3 a frame over budget does not sleep", SleepSeconds(c) == 0.0);
		c.foregroundFps = 0;
		c.now = 10.001;
		c.presented = true;
		check("C4 no cap and presented: vsync did the pacing, no sleep", SleepSeconds(c) == 0.0);
		c.presented = false;
		c.refreshHz = 144;
		check("C5 no cap and not presented: paced at the monitor refresh", approx(SleepSeconds(c), 1.0 / 144 - 0.001, 1e-9));
		c.refreshHz = 0;
		check("C6 unknown refresh falls back to 60", EffectiveFps(c) == 60);
		c.foregroundFps = 60;
		c.now = 9.0; // clock went backwards
		check("C7 clock going backwards sleeps at most one budget", SleepSeconds(c) <= 1.0 / 60 + 1e-12);
		check("C8 fps clamps: negative/absurd = no cap, sane kept",
		      ClampFps(-5) == 0 && ClampFps(100000) == 0 && ClampFps(144) == 144 && ClampFps(0) == 0);
	}

	// ---- R: present rule -------------------------------------------------------
	{
		PresentInputs p;
		p.now = 50.0;
		p.lastPresent = 49.9;
		p.elided = false;
		check("R1 a changed frame is presented", ShouldPresent(p));
		p.elided = true;
		check("R2 an unchanged frame is not presented", !ShouldPresent(p));
		p.sizeChanged = true;
		check("R3 ...unless the size changed", ShouldPresent(p));
		p.sizeChanged = false;
		p.screenshot = true;
		check("R4 ...or a screenshot needs the back buffer", ShouldPresent(p));
		p.screenshot = false;
		p.imguiContent = true;
		check("R5 ...or ImGui has something on screen", ShouldPresent(p));
		p.imguiContent = false;
		p.firstFrame = true;
		check("R6 ...or it is the first frame", ShouldPresent(p));
		p.firstFrame = false;
		p.lastPresent = 48.9;
		check("R7 ...or a second has passed (keep-alive)", ShouldPresent(p));
		p.lastPresent = 51.0;
		check("R8 ...or the clock went backwards", ShouldPresent(p));
	}

	// ---- L: recorder -------------------------------------------------------------
	{
		Harness h;
		h.rec.SetGpuSupported(true);
		h.rec.Header({ { "gl_renderer", "Test GPU" } });
		check("L1 header lines", h.CountPrefix("HEAD gl_renderer=Test GPU") == 1);

		h.rec.SetState(Page("LIST", "", true));
		check("L2 first state is an EVT", contains(h.Last("EVT"), "state page=LIST focus=fg"), h.Last("EVT"));

		// one second on the list page: 10 frames, all elided, 1 presented
		for (int i = 0; i < 10; i++) h.Frame(1.0, PerfLog::DrawElided, i == 0);
		h.EndSecond();
		const std::string sec1 = h.Last("SEC");
		check("L3 a SEC line after one second", !sec1.empty(), h.All());
		check("L4 SEC carries page, focus and rates",
		      contains(sec1, "page=LIST") && contains(sec1, "focus=fg") && contains(sec1, "fps=10.0") &&
		      contains(sec1, "drawn=0.0") && contains(sec1, "presented=1.0"), sec1);
		check("L5 CPU per frame avg/max", contains(sec1, "lua=1.00/1.0"), sec1);

		// switch to the tree: EVT, then drawn frames with GPU cost and layer changes
		h.rec.SetState(Page("BUILD", "TREE", true));
		check("L6 page change EVT", contains(h.Last("EVT"), "page LIST -> BUILD/TREE"), h.Last("EVT"));
		for (int i = 0; i < 10; i++) {
			h.rec.AddGpu(PerfLog::GpuLayers, 5.0);
			h.rec.LayerChanged(5, 0);
			h.Frame(4.0, PerfLog::DrawHashChanged, true);
		}
		h.EndSecond();
		const std::string sec2 = h.Last("SEC");
		check("L7 GPU ms per second and estimate", contains(sec2, "gpu_ms_per_s[layers=50.0") && contains(sec2, "gpu_est=5.0%"), sec2);
		check("L8 why drawn and changed layers", contains(sec2, "why[hash=10]") && contains(sec2, "changed_layers[5/0:10]"), sec2);

		// a spike: 60 ms frame
		size_t before = h.lines.size();
		h.Frame(60.0, PerfLog::DrawHashChanged, true, 0.001);
		bool spike = false;
		for (size_t i = before; i < h.lines.size(); i++) if (contains(h.lines[i], "SPIKE") && contains(h.lines[i], "page=BUILD/TREE")) spike = true;
		check("L9 a 60 ms frame writes a SPIKE with the page", spike, h.All());
		for (int i = 0; i < 10; i++) h.Frame(60.0, PerfLog::DrawHashChanged, true, 0.001);
		h.EndSecond();
		int spikesThisSecond = 0;
		for (size_t i = before; i < h.lines.size(); i++) if (h.lines[i].compare(0, 5, "SPIKE") == 0) spikesThisSecond++;
		check("L10 at most three SPIKE lines per second", spikesThisSecond == PerfLog::kMaxSpikesPerSecond,
		      std::to_string(spikesThisSecond));

		// focus lost + popup: EVT lines, then disjoint GPU second
		PerfLog::State bg = Page("BUILD", "TREE", false);
		bg.popups = 1;
		h.rec.SetState(bg);
		const std::string all = h.All();
		check("L11 focus and popup EVTs", contains(all, "EVT") && contains(all, "focus lost") && contains(all, "popups 0 -> 1"));
		h.rec.AddGpu(PerfLog::GpuLayers, 1.0);
		h.rec.GpuDisjoint();
		for (int i = 0; i < 10; i++) h.Frame(1.0, PerfLog::DrawHashChanged, true);
		h.EndSecond();
		check("L12 a disjoint second reports no GPU numbers", contains(h.Last("SEC"), "gpu=disjoint"), h.Last("SEC"));

		// idle-gated seconds still produce SEC lines with the idle count
		for (int i = 0; i < 10; i++) { h.rec.IdleGated(100.0); h.now += 0.05; }
		h.EndSecond();
		check("L13 idle-gated second", contains(h.Last("SEC"), "idle=10/1000ms"), h.Last("SEC"));

		// summary: groups sorted by GPU; the tree group (5% GPU) must rank first
		h.rec.Mark("tree", 3.0);
		h.rec.Finish();
		std::string first;
		for (const auto& l : h.lines) if (l.compare(0, 10, "SUMMARY #1") == 0) first = l;
		check("L14 SUMMARY ranks the GPU-heaviest page+state first", contains(first, "[BUILD/TREE fg]"), h.All());
		const size_t count = h.lines.size();
		h.rec.Event("after finish");
		h.rec.Finish();
		check("L15 nothing is written after the summary, and it is written once", h.lines.size() == count);
	}

	// ---- size cap ----------------------------------------------------------------
	{
		Harness h;
		const std::string big(4096, 'x');
		for (int i = 0; i < 3000 && !h.rec.Limited(); i++) h.rec.Event(big);
		check("L16 the size cap stops the log with one LIMIT line", h.rec.Limited() && h.CountPrefix("LIMIT") == 1 &&
		      h.rec.BytesWritten() <= PerfLog::kMaxBytes + 512, std::to_string(h.rec.BytesWritten()));
		h.rec.Finish();
		check("L17 the summary is still written after the cap", h.CountPrefix("SUMMARY") >= 1);
	}

	// ---- state equality / group keys -------------------------------------------------
	{
		PerfLog::State a = Page("BUILD", "ITEMS", true), b = a;
		b.luaKB = 12345;
		b.texAsync = 0;
		check("L18 memory numbers alone are not a state change", a == b);
		b.minimized = true;
		check("L19 group key marks minimised", b.GroupKey() == "BUILD/ITEMS fg minimized", b.GroupKey());
	}

	// ---- file naming and pruning ----------------------------------------------------
	{
		wchar_t tmp[MAX_PATH] = {};
		GetTempPathW(MAX_PATH, tmp);
		std::wstring box = std::wstring(tmp) + L"pobtools_perf_selftest\\";
		CreateDirectoryW(box.c_str(), nullptr);
		PobLog::SetDirForTest(box);
		const std::wstring today = PerfLog::LogPathForToday();
		const size_t slash = today.find_last_of(L'\\');
		const std::wstring name = slash == std::wstring::npos ? today : today.substr(slash + 1);
		check("L20 file is perf-YYYY-MM-DD.log in the log folder",
		      name.size() == 19 && name.compare(0, 5, L"perf-") == 0 && name.compare(15, 4, L".log") == 0 &&
		      today.compare(0, box.size(), box) == 0);
		const std::wstring old = box + L"perf-2001-01-01.log";
		HANDLE f = CreateFileW(old.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
		const std::wstring notOurs = box + L"perf-notes.log";
		f = CreateFileW(notOurs.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
		PobLog::PruneOlderThan(30);
		check("L21 old perf logs are pruned, look-alikes are not",
		      GetFileAttributesW(old.c_str()) == INVALID_FILE_ATTRIBUTES &&
		      GetFileAttributesW(notOurs.c_str()) != INVALID_FILE_ATTRIBUTES);
		DeleteFileW(notOurs.c_str());
		RemoveDirectoryW(box.c_str());
		PobLog::SetDirForTest(L"");
	}

	// ---- switch ------------------------------------------------------------------------
	check("L22 disabled unless POB_ZH_PERFLOG=1 (this process has it unset)",
	      GetEnvironmentVariableW(L"POB_ZH_PERFLOG", nullptr, 0) != 0 || !PerfLog::Enabled());

	line("");
	line(g_fail ? "RESULT FAIL" : "RESULT PASS");

	CreateDirectoryW((exeDir + L"PobTools").c_str(), nullptr);
	HANDLE h = CreateFileW((exeDir + L"PobTools\\perf_selftest.txt").c_str(),
	                       GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h != INVALID_HANDLE_VALUE) {
		DWORD w = 0;
		WriteFile(h, g_report.data(), (DWORD)g_report.size(), &w, nullptr);
		CloseHandle(h);
	}
	return g_fail ? 2 : 0;
}
