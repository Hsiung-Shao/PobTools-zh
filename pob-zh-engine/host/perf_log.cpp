#include "perf_log.h"

#include "app_version.h"
#include "error_log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#pragma comment(lib, "psapi.lib")

#include <algorithm>
#include <cstdio>
#include <memory>

namespace PerfLog {

namespace {

const char* kCpuNames[kCpuCount] = { "lua", "sub", "hash", "layers", "glass", "blit", "swap", "sleep", "tr" };
const char* kGpuNames[kGpuCount] = { "layers", "glass", "present" };
const char* kReasonNames[kDrawReasonCount] = { "elided", "hash", "inhibit", "appearance", "first", "noelide" };
const char* kCounterNames[kCounterCount] = { "cmds", "strings", "tr", "glass" };

const char* FocusName(const State& s)
{
	return s.active ? "fg" : (s.cursorOver ? "hover" : "bg");
}

std::string PageName(const State& s)
{
	std::string p = s.mode.empty() ? "?" : s.mode;
	if (!s.view.empty()) p += "/" + s.view;
	return p;
}

} // namespace

bool State::operator==(const State& o) const
{
	return mode == o.mode && view == o.view && popups == o.popups && dropdown == o.dropdown &&
	       treeZoom == o.treeZoom && treeDragging == o.treeDragging && treeSearch == o.treeSearch &&
	       active == o.active && cursorOver == o.cursorOver && minimized == o.minimized &&
	       occluded == o.occluded && coroutine == o.coroutine && (texAsync > 0) == (o.texAsync > 0) &&
	       fbW == o.fbW && fbH == o.fbH && fpsCap == o.fpsCap;
}

std::string State::GroupKey() const
{
	std::string k = PageName(*this) + " " + FocusName(*this);
	if (popups > 0 || dropdown) k += " popup";
	if (minimized) k += " minimized";
	else if (occluded) k += " occluded";
	return k;
}

Recorder::Recorder(Clock clock, Sink sink) : clock_(std::move(clock)), sink_(std::move(sink))
{
	t0_ = clock_();
	sec_.start = t0_;
}

std::string Recorder::Fmt(double v, int prec)
{
	char buf[48];
	snprintf(buf, sizeof(buf), "%.*f", prec, v);
	return buf;
}

std::string Recorder::Stamp() const
{
	return "t=" + Fmt(clock_() - t0_, 3);
}

void Recorder::Emit(const std::string& line, bool force)
{
	if (finished_ && !force) return;
	if (limited_ && !force) return;
	if (!force && (bytes_ + line.size() > kMaxBytes || clock_() - t0_ > kMaxSeconds)) {
		limited_ = true;
		const std::string lim = "LIMIT " + Stamp() + " size or duration cap reached; per-second lines stop here, SUMMARY is still written at exit";
		sink_(lim);
		bytes_ += lim.size() + 2;
		return;
	}
	sink_(line);
	bytes_ += line.size() + 2;
}

void Recorder::Header(const std::vector<std::pair<std::string, std::string>>& kv)
{
	for (const auto& p : kv) Emit("HEAD " + p.first + "=" + p.second);
}

void Recorder::FrameBegin()
{
	for (double& v : frameCpu_) v = 0;
	inFrame_ = true;
}

void Recorder::AddCpu(Cpu sec, double ms)
{
	if (sec < 0 || sec >= kCpuCount || ms < 0) return;
	if (inFrame_) frameCpu_[sec] += ms;
	else sec_.cpu[sec].sum += ms; // outside a frame (e.g. sleep after it): no per-frame max
}

void Recorder::Mark(const std::string& page, double ms)
{
	if (ms < 0) return;
	sec_.pages[page].Add(ms);
}

void Recorder::Count(Counter c, long n)
{
	if (c < 0 || c >= kCounterCount) return;
	sec_.counters[c] += n;
}

void Recorder::LayerChanged(int layer, int subLayer)
{
	sec_.layers[{ layer, subLayer }]++;
}

void Recorder::FrameEnd(DrawReason why, bool presented)
{
	if (!inFrame_) return;
	inFrame_ = false;
	double total = 0;
	for (int i = 0; i < kCpuCount; i++)
		if (i != CpuGlass && i != CpuCapSleep) total += frameCpu_[i]; // glass is inside layers
	double prevAvg = 0;
	if (sec_.frames > 0) {
		double s = 0;
		for (int i = 0; i < kCpuCount; i++)
			if (i != CpuGlass && i != CpuCapSleep) s += sec_.cpu[i].sum;
		prevAvg = s / sec_.frames;
	}
	const bool spike = total >= kSpikeMs || (sec_.frames >= 5 && total >= 8.0 && total > 4.0 * prevAvg);

	if (why < 0 || why >= kDrawReasonCount) why = DrawHashChanged;
	sec_.frames++;
	sec_.reason[why]++;
	if (presented) sec_.presented++;
	for (int i = 0; i < kCpuCount; i++) sec_.cpu[i].Add(frameCpu_[i]);

	if (spike && sec_.spikes < kMaxSpikesPerSecond) {
		sec_.spikes++;
		std::string l = "SPIKE " + Stamp() + " page=" + PageName(state_) + " focus=" + FocusName(state_) +
		                " popups=" + std::to_string(state_.popups) + " why=" + kReasonNames[why] +
		                " cpu_ms=" + Fmt(total, 2) + " [";
		for (int i = 0; i < kCpuCount; i++) {
			if (frameCpu_[i] <= 0.005) continue;
			l += std::string(kCpuNames[i]) + "=" + Fmt(frameCpu_[i], 2) + " ";
		}
		if (l.back() == ' ') l.pop_back();
		l += "]";
		Emit(l);
	}
}

void Recorder::IdleGated(double sleptMs)
{
	sec_.idleGated++;
	sec_.idleMs += sleptMs;
}

void Recorder::AddGpu(Gpu sec, double ms)
{
	if (sec < 0 || sec >= kGpuCount || ms < 0) return;
	sec_.gpu[sec] += ms;
	sec_.gpuSamples++;
}

void Recorder::GpuDisjoint()
{
	sec_.gpuDisjoint = true;
}

void Recorder::Event(const std::string& text)
{
	Emit("EVT " + Stamp() + " " + text);
}

void Recorder::SetState(const State& s)
{
	if (!haveState_) {
		haveState_ = true;
		state_ = s;
		Event("state page=" + PageName(s) + " focus=" + FocusName(s) + " popups=" + std::to_string(s.popups) +
		      " fb=" + std::to_string(s.fbW) + "x" + std::to_string(s.fbH) + " cap=" + std::to_string(s.fpsCap) +
		      (s.minimized ? " minimized" : "") + (s.occluded ? " occluded" : ""));
		return;
	}
	if (s == state_ && s.treeZoom == state_.treeZoom) { state_.luaKB = s.luaKB; state_.texAsync = s.texAsync; return; }
	const State& o = state_;
	if (s.mode != o.mode || s.view != o.view) Event("page " + PageName(o) + " -> " + PageName(s));
	if (s.active != o.active) Event(s.active ? "focus gained" : "focus lost");
	if (s.cursorOver != o.cursorOver) Event(s.cursorOver ? "cursor entered window" : "cursor left window");
	if (s.minimized != o.minimized) Event(s.minimized ? "minimized" : "restored");
	if (s.occluded != o.occluded) Event(s.occluded ? "window hidden behind others" : "window visible again");
	if (s.popups != o.popups) Event("popups " + std::to_string(o.popups) + " -> " + std::to_string(s.popups));
	if (s.dropdown != o.dropdown) Event(s.dropdown ? "dropdown/menu opened" : "dropdown/menu closed");
	if (s.treeDragging != o.treeDragging) Event(s.treeDragging ? "tree drag started" : "tree drag ended");
	if (s.treeZoom != o.treeZoom && s.treeZoom >= 0 && o.treeZoom >= 0)
		Event("tree zoom " + std::to_string(o.treeZoom) + " -> " + std::to_string(s.treeZoom));
	if (s.treeSearch != o.treeSearch) Event(s.treeSearch ? "tree search active" : "tree search cleared");
	if (s.coroutine != o.coroutine) Event(s.coroutine ? "background task started" : "background task finished");
	if ((s.texAsync > 0) != (o.texAsync > 0))
		Event(s.texAsync > 0 ? "textures loading (" + std::to_string(s.texAsync) + ")" : "textures loaded");
	if (s.fbW != o.fbW || s.fbH != o.fbH)
		Event("framebuffer " + std::to_string(o.fbW) + "x" + std::to_string(o.fbH) + " -> " +
		      std::to_string(s.fbW) + "x" + std::to_string(s.fbH));
	if (s.fpsCap != o.fpsCap) Event("fps cap " + std::to_string(o.fpsCap) + " -> " + std::to_string(s.fpsCap));
	state_ = s;
}

bool Recorder::SecondDue() const
{
	return clock_() - sec_.start >= 1.0;
}

void Recorder::Tick(const ProcStats& ps)
{
	if (!SecondDue()) return;
	CloseSecond(ps);
}

void Recorder::CloseSecond(const ProcStats& ps)
{
	const double now = clock_();
	double el = now - sec_.start;
	if (el <= 0) el = 1e-3;
	const long frames = sec_.frames;
	const long drawn = frames - sec_.reason[DrawElided];
	const bool gpuOk = gpuSupported_ && sec_.gpuSamples > 0 && !sec_.gpuDisjoint;

	// ---- the SEC line ----
	std::string l = "SEC " + Stamp() + " page=" + PageName(state_) + " focus=" + FocusName(state_);
	if (state_.popups > 0 || state_.dropdown) l += " popups=" + std::to_string(state_.popups) + (state_.dropdown ? "+menu" : "");
	if (state_.minimized) l += " minimized";
	if (state_.occluded) l += " occluded";
	if (state_.treeZoom >= 0) l += " zoom=" + std::to_string(state_.treeZoom) + (state_.treeDragging ? " dragging" : "");
	l += " fps=" + Fmt(frames / el) + " drawn=" + Fmt(drawn / el) + " presented=" + Fmt(sec_.presented / el);
	l += " cap=" + std::to_string(state_.fpsCap);
	if (sec_.idleGated) l += " idle=" + std::to_string(sec_.idleGated) + "/" + Fmt(sec_.idleMs, 0) + "ms";

	l += " cpu_ms_per_frame[";
	for (int i = 0; i < kCpuCount; i++) {
		const double avg = frames ? sec_.cpu[i].sum / frames : 0;
		if (avg < 0.005 && sec_.cpu[i].max < 0.005) continue;
		l += std::string(kCpuNames[i]) + "=" + Fmt(avg, 2) + "/" + Fmt(sec_.cpu[i].max, 1) + " ";
	}
	if (l.back() == ' ') l.pop_back();
	l += "]";

	if (!sec_.pages.empty()) {
		l += " lua_pages_ms[";
		for (const auto& p : sec_.pages)
			l += p.first + "=" + Fmt(p.second.n ? p.second.sum / p.second.n : 0, 2) + "/" + Fmt(p.second.max, 1) + " ";
		l.back() = ']';
	}

	if (!gpuSupported_) l += " gpu=unsupported";
	else if (sec_.gpuDisjoint) l += " gpu=disjoint";
	else if (!gpuOk) l += " gpu=idle";
	else {
		double tot = 0;
		l += " gpu_ms_per_s[";
		for (int i = 0; i < kGpuCount; i++) {
			tot += sec_.gpu[i];
			l += std::string(kGpuNames[i]) + "=" + Fmt(sec_.gpu[i] / el, 1) + " ";
		}
		l.back() = ']';
		l += " gpu_est=" + Fmt(tot / el / 10.0) + "%";
	}

	if (drawn > 0) {
		l += " why[";
		for (int i = 1; i < kDrawReasonCount; i++)
			if (sec_.reason[i]) l += std::string(kReasonNames[i]) + "=" + std::to_string(sec_.reason[i]) + " ";
		l.back() = ']';
	}
	if (!sec_.layers.empty()) {
		std::vector<std::pair<long, std::pair<int, int>>> top;
		for (const auto& kv : sec_.layers) top.push_back({ kv.second, kv.first });
		std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
		l += " changed_layers[";
		for (size_t i = 0; i < top.size() && i < 5; i++)
			l += std::to_string(top[i].second.first) + "/" + std::to_string(top[i].second.second) + ":" +
			     std::to_string(top[i].first) + " ";
		l.back() = ']';
	}
	if (frames > 0) {
		std::string counts;
		for (int i = 0; i < kCounterCount; i++) {
			if (!sec_.counters[i]) continue;
			counts += std::string(kCounterNames[i]) + "=" + Fmt((double)sec_.counters[i] / frames) + " ";
		}
		if (!counts.empty()) {
			counts.pop_back();
			l += " counts[" + counts + "]";
		}
	}
	if (ps.workingSetMB >= 0) l += " ws=" + Fmt(ps.workingSetMB, 0) + "MB";
	if (ps.privateMB >= 0) l += " priv=" + Fmt(ps.privateMB, 0) + "MB";
	if (state_.luaKB > 0) l += " lua=" + Fmt(state_.luaKB / 1024.0, 0) + "MB";
	if (ps.cpuPct >= 0) l += " proc_cpu=" + Fmt(ps.cpuPct) + "%";
	if (state_.texAsync > 0) l += " tex_loading=" + std::to_string(state_.texAsync);
	Emit(l);

	// ---- fold into the page+state group ----
	Group& g = groups_[state_.GroupKey()];
	g.seconds += el;
	g.frames += frames;
	g.drawn += drawn;
	g.presented += sec_.presented;
	for (int i = 0; i < kCpuCount; i++) g.cpu[i] += sec_.cpu[i].sum;
	if (gpuOk) {
		for (int i = 0; i < kGpuCount; i++) g.gpu[i] += sec_.gpu[i];
		g.gpuSeconds += el;
	}
	for (const auto& p : sec_.pages) g.pages[p.first] += p.second.sum;

	sec_ = Second();
	sec_.start = now;
}

void Recorder::Finish()
{
	if (finished_) return;
	if (sec_.frames > 0 || sec_.idleGated > 0) CloseSecond(ProcStats());
	finished_ = true;

	struct Row { std::string key; const Group* g; double gpuPct; double cpuMsPerS; };
	std::vector<Row> rows;
	for (const auto& kv : groups_) {
		const Group& g = kv.second;
		if (g.seconds <= 0) continue;
		double gtot = 0;
		for (double v : g.gpu) gtot += v;
		const double gpuPct = g.gpuSeconds > 0 ? gtot / g.gpuSeconds / 10.0 : -1;
		double ctot = 0;
		for (int i = 0; i < kCpuCount; i++)
			if (i != CpuGlass && i != CpuCapSleep) ctot += g.cpu[i];
		rows.push_back({ kv.first, &g, gpuPct, ctot / g.seconds });
	}
	std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
		if (a.gpuPct != b.gpuPct) return a.gpuPct > b.gpuPct;
		return a.cpuMsPerS > b.cpuMsPerS;
	});

	Emit("SUMMARY " + Stamp() + " groups=" + std::to_string(rows.size()) +
	     " (sorted by estimated GPU use, then CPU; paste this block when reporting)", true);
	int rank = 0;
	for (const Row& r : rows) {
		const Group& g = *r.g;
		std::string l = "SUMMARY #" + std::to_string(++rank) + " [" + r.key + "] secs=" + Fmt(g.seconds, 0) +
		                " fps=" + Fmt(g.frames / g.seconds) + " drawn=" + Fmt(g.drawn / g.seconds) +
		                " presented=" + Fmt(g.presented / g.seconds) +
		                " gpu_est=" + (r.gpuPct >= 0 ? Fmt(r.gpuPct) + "%" : std::string("n/a")) +
		                " cpu_ms_per_s=" + Fmt(r.cpuMsPerS);
		// top three CPU sections and all GPU sections, per second
		std::vector<std::pair<double, int>> cs;
		for (int i = 0; i < kCpuCount; i++)
			if (i != CpuCapSleep && g.cpu[i] > 0) cs.push_back({ g.cpu[i] / g.seconds, i });
		std::sort(cs.begin(), cs.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
		if (!cs.empty()) {
			l += " top_cpu[";
			for (size_t i = 0; i < cs.size() && i < 3; i++) l += std::string(kCpuNames[cs[i].second]) + "=" + Fmt(cs[i].first) + " ";
			l.back() = ']';
		}
		if (g.gpuSeconds > 0) {
			l += " gpu_ms_per_s[";
			for (int i = 0; i < kGpuCount; i++) l += std::string(kGpuNames[i]) + "=" + Fmt(g.gpu[i] / g.gpuSeconds) + " ";
			l.back() = ']';
		}
		if (!g.pages.empty()) {
			std::vector<std::pair<double, std::string>> ps;
			for (const auto& p : g.pages) ps.push_back({ p.second / g.seconds, p.first });
			std::sort(ps.begin(), ps.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
			l += " lua_pages_ms_per_s[";
			for (size_t i = 0; i < ps.size() && i < 3; i++) l += ps[i].second + "=" + Fmt(ps[i].first) + " ";
			l.back() = ']';
		}
		Emit(l, true);
	}
}

// ---- global, file-backed ---------------------------------------------------------

namespace {

int g_enabled = -1;
std::unique_ptr<Recorder> g_rec;
std::wstring g_testPath;
std::wstring g_path;

long long Qpc()
{
	LARGE_INTEGER t;
	QueryPerformanceCounter(&t);
	return t.QuadPart;
}

double QpcToMs(long long ticks)
{
	static const double freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return (double)f.QuadPart; }();
	return ticks * 1000.0 / freq;
}

void AppendLine(const std::wstring& path, const std::string& line)
{
	HANDLE h = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
	                       OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE) return;
	std::string out = line + "\r\n";
	DWORD wrote = 0;
	WriteFile(h, out.data(), (DWORD)out.size(), &wrote, nullptr);
	CloseHandle(h);
}

} // namespace

std::wstring LogPathForToday()
{
	if (!g_testPath.empty()) return g_testPath;
	SYSTEMTIME st{};
	GetLocalTime(&st);
	wchar_t name[64];
	swprintf_s(name, L"perf-%04d-%02d-%02d.log", st.wYear, st.wMonth, st.wDay);
	return PobLog::LogDir() + name;
}

void SetPathForTest(const std::wstring& path)
{
	g_testPath = path;
}

void WindowFacts(void* hwndPtr, bool& minimized, bool& occluded)
{
	minimized = occluded = false;
	HWND hwnd = (HWND)hwndPtr;
	if (!hwnd) return;
	minimized = IsIconic(hwnd) != 0;
	if (minimized) return;
	HWND fg = GetForegroundWindow();
	if (!fg || fg == hwnd) return;
	RECT me{}, other{};
	if (!GetWindowRect(hwnd, &me) || !GetWindowRect(fg, &other)) return;
	occluded = other.left <= me.left && other.top <= me.top && other.right >= me.right && other.bottom >= me.bottom;
}

std::string OsDescription()
{
	auto readStr = [](const wchar_t* name) -> std::string {
		wchar_t buf[256] = {};
		DWORD size = sizeof(buf);
		if (RegGetValueW(HKEY_LOCAL_MACHINE, LR"(SOFTWARE\Microsoft\Windows NT\CurrentVersion)", name,
		                 RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS)
			return std::string();
		char out[512] = {};
		WideCharToMultiByte(CP_UTF8, 0, buf, -1, out, sizeof(out), nullptr, nullptr);
		return out;
	};
	const std::string product = readStr(L"ProductName");
	const std::string display = readStr(L"DisplayVersion");
	const std::string build = readStr(L"CurrentBuild");
	std::string s = product;
	if (!display.empty()) s += " " + display;
	if (!build.empty()) s += " (build " + build + ")";
	return s;
}

double NowMs()
{
	return QpcToMs(Qpc());
}

bool Enabled()
{
	if (g_enabled < 0) {
		wchar_t buf[8] = {};
		const DWORD n = GetEnvironmentVariableW(L"POB_ZH_PERFLOG", buf, 8);
		g_enabled = (n > 0 && buf[0] == L'1') ? 1 : 0;
	}
	return g_enabled > 0;
}

Recorder* Get()
{
	if (!Enabled()) return nullptr;
	if (!g_rec) {
		g_path = LogPathForToday();
		SYSTEMTIME st{};
		GetLocalTime(&st);
		char head[160];
		snprintf(head, sizeof(head), "==== PobTools v%s POB performance log, pid %lu, started %04d-%02d-%02d %02d:%02d:%02d ====",
		         POBTOOLS_VERSION_STRING, (unsigned long)GetCurrentProcessId(), st.wYear, st.wMonth, st.wDay,
		         st.wHour, st.wMinute, st.wSecond);
		AppendLine(g_path, "");
		AppendLine(g_path, head);
		const std::wstring path = g_path;
		g_rec.reset(new Recorder([] { return NowMs() / 1000.0; },
		                         [path](const std::string& line) { AppendLine(path, line); }));
	}
	return g_rec.get();
}

ProcStats SampleProcess()
{
	ProcStats ps;
	PROCESS_MEMORY_COUNTERS_EX pmc{};
	pmc.cb = sizeof(pmc);
	if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc))) {
		ps.workingSetMB = pmc.WorkingSetSize / (1024.0 * 1024.0);
		ps.privateMB = pmc.PrivateUsage / (1024.0 * 1024.0);
	}
	static ULONGLONG lastCpu = 0;
	static long long lastWall = 0;
	FILETIME c, e, k, u;
	if (GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) {
		const ULONGLONG cpu = (((ULONGLONG)k.dwHighDateTime << 32) | k.dwLowDateTime) +
		                      (((ULONGLONG)u.dwHighDateTime << 32) | u.dwLowDateTime);
		const long long wall = Qpc();
		if (lastWall != 0) {
			const double wallMs = QpcToMs(wall - lastWall);
			static const int cores = [] { SYSTEM_INFO si; GetSystemInfo(&si); return (int)si.dwNumberOfProcessors; }();
			if (wallMs > 0 && cores > 0) ps.cpuPct = (cpu - lastCpu) / 10000.0 / wallMs / cores * 100.0;
		}
		lastCpu = cpu;
		lastWall = wall;
	}
	return ps;
}

void Shutdown()
{
	if (g_rec) g_rec->Finish();
}

Scope::Scope(Cpu sec) : sec_(sec)
{
	if (!Enabled()) return;
	on_ = true;
	start_ = Qpc();
}

Scope::~Scope()
{
	if (!on_) return;
	if (Recorder* r = Get()) r->AddCpu(sec_, QpcToMs(Qpc() - start_));
}

} // namespace PerfLog
