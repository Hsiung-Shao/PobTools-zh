#include "hang_watch.h"

#include "app_version.h"
#include "error_log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>

#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "dbghelp.lib")

namespace {

// ---- shared state ------------------------------------------------------------

const size_t kStageMax   = 96;        // breadcrumb, fixed size so it can live in a seqlock
const size_t kStackBytes = 256 * 1024;// how much of a thread's stack we copy
const int    kMaxFrames  = 64;
const int    kMaxThreads = 8;         // the stalled thread plus whoever it is waiting on
const int    kMaxReports = 3;         // per process run; a hang that repeats says the same thing

std::atomic<unsigned long long> g_beat{0};
std::atomic<bool>               g_running{false};
std::atomic<int>                g_reports{0};

// The breadcrumb, written by the watched thread and read by the watchdog while
// that thread may be suspended. A mutex here would deadlock exactly when it
// matters, so this is a seqlock: odd sequence means a write is in progress and
// the reader retries. Torn reads are impossible; a reader that keeps losing the
// race simply reports the breadcrumb as unknown.
std::atomic<unsigned>           g_stageSeq{0};
char                            g_stage[kStageMax] = {};
std::atomic<unsigned long long> g_stageSince{0};

DWORD           g_mainTid = 0;
std::thread     g_watch;
HANDLE          g_stopEvt = nullptr;
std::string     g_role    = "app";
std::string     g_game;
unsigned        g_stallMs = 20000;
bool            g_requireWindow = true;
LPTOP_LEVEL_EXCEPTION_FILTER g_prevFilter = nullptr;

std::mutex             g_peerMx;
std::vector<HangWatch::Peer> g_peers;
HangWatch::PeerStatus  g_peerStatus;

std::mutex g_reportMx;   // one report at a time; also guards the walk snapshot

void SetStage(const char* tag)
{
	const unsigned seq = g_stageSeq.load(std::memory_order_relaxed);
	g_stageSeq.store(seq + 1, std::memory_order_release);   // odd: writing
	std::atomic_thread_fence(std::memory_order_release);
	size_t n = 0;
	if (tag) {
		while (n + 1 < kStageMax && tag[n]) { g_stage[n] = tag[n]; n++; }
	}
	g_stage[n] = '\0';
	std::atomic_thread_fence(std::memory_order_release);
	g_stageSeq.store(seq + 2, std::memory_order_release);   // even: readable
	g_stageSince.store(GetTickCount64(), std::memory_order_relaxed);
}

std::string ReadStage()
{
	for (int attempt = 0; attempt < 8; attempt++) {
		const unsigned a = g_stageSeq.load(std::memory_order_acquire);
		if (a & 1u) { Sleep(1); continue; }
		char copy[kStageMax];
		memcpy(copy, g_stage, kStageMax);
		std::atomic_thread_fence(std::memory_order_acquire);
		if (g_stageSeq.load(std::memory_order_acquire) == a) {
			copy[kStageMax - 1] = '\0';
			return std::string(copy);
		}
	}
	return std::string();
}

// ---- small helpers -----------------------------------------------------------

std::wstring InstallDir()
{
	wchar_t buf[MAX_PATH] = {};
	const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
	std::wstring p(buf, n);
	const size_t slash = p.find_last_of(L'\\');
	return slash == std::wstring::npos ? std::wstring() : p.substr(0, slash + 1);
}

std::string Narrow(const std::wstring& w)
{
	if (w.empty()) return std::string();
	const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
	std::string s((size_t)(n > 0 ? n : 0), '\0');
	if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
	return s;
}

// Only what a file name may contain. A role reaches this from an env var and a
// command line, and a report that cannot be written is a report that is not read.
std::string SafeRole(const std::string& role)
{
	std::string out;
	for (char c : role) {
		if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-') out += c;
		else if (c >= 'A' && c <= 'Z') out += (char)(c - 'A' + 'a');
		else if (!out.empty() && out.back() != '-') out += '-';
	}
	while (!out.empty() && out.back() == '-') out.pop_back();
	return out.empty() ? std::string("app") : out;
}

std::string Hex(unsigned long long v, int width)
{
	char buf[32];
	sprintf_s(buf, "%0*llx", width, v);
	return std::string(buf);
}

// ---- module table ------------------------------------------------------------
// Built ourselves rather than asked of dbghelp, because this is what a report
// without symbols lives on: name + offset here, plus the size and PE timestamp
// so the exact build can be matched to its PDB months later.
struct ModuleRec {
	std::string name;
	uintptr_t   base = 0;
	uintptr_t   size = 0;
	unsigned    stamp = 0;
};

std::vector<ModuleRec> Modules()
{
	std::vector<ModuleRec> out;
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
	if (snap == INVALID_HANDLE_VALUE) return out;
	MODULEENTRY32W me{};
	me.dwSize = sizeof(me);
	if (Module32FirstW(snap, &me)) {
		do {
			ModuleRec m;
			m.name = Narrow(me.szModule);
			m.base = (uintptr_t)me.modBaseAddr;
			m.size = (uintptr_t)me.modBaseSize;
			// PE TimeDateStamp, straight out of the mapped image.
			const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)me.modBaseAddr;
			if (dos && dos->e_magic == IMAGE_DOS_SIGNATURE) {
				const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)((const char*)dos + dos->e_lfanew);
				if (nt->Signature == IMAGE_NT_SIGNATURE) m.stamp = nt->FileHeader.TimeDateStamp;
			}
			out.push_back(m);
		} while (Module32NextW(snap, &me));
	}
	CloseHandle(snap);
	return out;
}

std::string FormatAddr(const std::vector<ModuleRec>& mods, uintptr_t addr)
{
	for (const ModuleRec& m : mods) {
		if (addr >= m.base && addr < m.base + m.size)
			return m.name + "+0x" + Hex(addr - m.base, 6);
	}
	return "0x" + Hex(addr, 16);
}

// ---- stack capture -----------------------------------------------------------
// The suspended window contains nothing but ReadProcessMemory. No allocation, no
// loader lock, no dbghelp: every one of those is a lock the stalled thread may
// already hold, and taking it here would wedge the watchdog against the very
// thread it is trying to describe.
struct StackSnap {
	CONTEXT ctx{};
	uintptr_t stackBase = 0;
	std::vector<unsigned char> stack;
	bool ok = false;
};

StackSnap CaptureThread(DWORD tid)
{
	StackSnap s;
	if (tid == GetCurrentThreadId()) return s;   // never suspend ourselves
	HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
	                       FALSE, tid);
	if (!th) return s;
	s.stack.resize(kStackBytes);
	if (SuspendThread(th) != (DWORD)-1) {
		s.ctx.ContextFlags = CONTEXT_FULL;
		if (GetThreadContext(th, &s.ctx)) {
#ifdef _M_X64
			const uintptr_t sp = (uintptr_t)s.ctx.Rsp;
#else
			const uintptr_t sp = (uintptr_t)s.ctx.Esp;
#endif
			MEMORY_BASIC_INFORMATION mbi{};
			size_t want = kStackBytes;
			if (VirtualQuery((void*)sp, &mbi, sizeof(mbi)) == sizeof(mbi)) {
				const uintptr_t end = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
				if (end > sp && end - sp < want) want = (size_t)(end - sp);
			}
			SIZE_T got = 0;
			// ReadProcessMemory rather than memcpy: a stack overflow is one of the
			// hangs worth reporting, and its guard page must not fault us too.
			if (ReadProcessMemory(GetCurrentProcess(), (void*)sp, s.stack.data(), want, &got) && got > 0) {
				s.stack.resize(got);
				s.stackBase = sp;
				s.ok = true;
			}
		}
		ResumeThread(th);
	}
	CloseHandle(th);
	if (!s.ok) s.stack.clear();
	return s;
}

// The walk runs after the thread is back on its feet, out of the copy above, so
// dbghelp's lock is taken with nobody suspended.
const StackSnap* g_walkSnap = nullptr;   // guarded by g_reportMx

BOOL CALLBACK ReadMemCb(HANDLE, DWORD64 addr, PVOID buf, DWORD size, LPDWORD read)
{
	if (g_walkSnap && g_walkSnap->ok) {
		const uintptr_t a = (uintptr_t)addr;
		const uintptr_t lo = g_walkSnap->stackBase;
		const uintptr_t hi = lo + g_walkSnap->stack.size();
		if (a >= lo && a < hi) {
			const size_t avail = (size_t)(hi - a);
			const size_t n = size < avail ? size : avail;
			memcpy(buf, g_walkSnap->stack.data() + (a - lo), n);
			if (read) *read = (DWORD)n;
			return n == size;
		}
	}
	// Anything else is module memory (unwind tables), still mapped and stable.
	SIZE_T got = 0;
	const BOOL ok = ReadProcessMemory(GetCurrentProcess(), (void*)(uintptr_t)addr, buf, size, &got);
	if (read) *read = (DWORD)got;
	return ok && got == size;
}

void EnsureSyms()
{
	static bool done = false;
	if (done) return;
	done = true;
	// DEFERRED_LOADS means no PDB is touched unless SymFromAddr asks for one, and
	// the search path is pinned to the install directory: never a symbol server,
	// which would turn a frozen program into a frozen program making network
	// requests. A developer can point POB_ZH_SYMPATH at a build directory to get
	// function names; a shipped install has no PDB and prints module+offset.
	SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_FAIL_CRITICAL_ERRORS |
	              SYMOPT_NO_PROMPTS | SYMOPT_UNDNAME);
	std::wstring path;
	wchar_t env[1024] = {};
	if (GetEnvironmentVariableW(L"POB_ZH_SYMPATH", env, 1024) > 0) path = env;
	else path = InstallDir();
	SymInitializeW(GetCurrentProcess(), path.empty() ? nullptr : path.c_str(), TRUE);
}

std::vector<std::string> WalkFrames(const StackSnap& snap, const std::vector<ModuleRec>& mods)
{
	std::vector<std::string> out;
	if (!snap.ok) return out;
#if defined(_M_X64) || defined(_M_IX86)
	EnsureSyms();
	CONTEXT ctx = snap.ctx;
	STACKFRAME64 sf{};
	sf.AddrPC.Mode = sf.AddrFrame.Mode = sf.AddrStack.Mode = AddrModeFlat;
#ifdef _M_X64
	const DWORD machine = IMAGE_FILE_MACHINE_AMD64;
	sf.AddrPC.Offset    = ctx.Rip;
	sf.AddrFrame.Offset = ctx.Rbp;
	sf.AddrStack.Offset = ctx.Rsp;
#else
	const DWORD machine = IMAGE_FILE_MACHINE_I386;
	sf.AddrPC.Offset    = ctx.Eip;
	sf.AddrFrame.Offset = ctx.Ebp;
	sf.AddrStack.Offset = ctx.Esp;
#endif
	g_walkSnap = &snap;
	for (int i = 0; i < kMaxFrames; i++) {
		if (!StackWalk64(machine, GetCurrentProcess(), GetCurrentThread(), &sf, &ctx,
		                 ReadMemCb, SymFunctionTableAccess64, SymGetModuleBase64, nullptr))
			break;
		if (sf.AddrPC.Offset == 0) break;
		std::string line = "#" + Hex((unsigned)i, 2) + " " +
		                   FormatAddr(mods, (uintptr_t)sf.AddrPC.Offset);
		// Only ever an extra: on a user's machine there is no PDB and this fails.
		unsigned char sym[sizeof(SYMBOL_INFO) + 512] = {};
		SYMBOL_INFO* si = (SYMBOL_INFO*)sym;
		si->SizeOfStruct = sizeof(SYMBOL_INFO);
		si->MaxNameLen = 511;
		DWORD64 disp = 0;
		if (SymFromAddr(GetCurrentProcess(), sf.AddrPC.Offset, &disp, si) && si->NameLen > 0)
			line += std::string("  ") + si->Name;
		out.push_back(line);
	}
	g_walkSnap = nullptr;
#else
	(void)mods;
#endif
	return out;
}

std::vector<DWORD> OtherThreads()
{
	std::vector<DWORD> out;
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
	if (snap == INVALID_HANDLE_VALUE) return out;
	THREADENTRY32 te{};
	te.dwSize = sizeof(te);
	const DWORD me = GetCurrentThreadId();
	const DWORD pid = GetCurrentProcessId();
	if (Thread32First(snap, &te)) {
		do {
			if (te.dwSize < FIELD_OFFSET(THREADENTRY32, th32OwnerProcessID) + sizeof(DWORD)) continue;
			if (te.th32OwnerProcessID != pid) continue;
			if (te.th32ThreadID == me || te.th32ThreadID == g_mainTid) continue;
			out.push_back(te.th32ThreadID);
		} while (Thread32Next(snap, &te));
	}
	CloseHandle(snap);
	return out;
}

// ---- the report --------------------------------------------------------------

std::string Stamp(const SYSTEMTIME& st)
{
	char buf[64];
	sprintf_s(buf, "%04d-%02d-%02d %02d:%02d:%02d",
	          st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
	return buf;
}

bool WriteFileUtf8(const std::wstring& path, const std::string& body)
{
	HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
	                       CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE) return false;
	DWORD wrote = 0;
	// With a BOM, unlike the error log. This one is a whole document a user opens
	// and reads before deciding to send it; without the BOM an editor that guesses
	// the local code page turns the Chinese half of it into rubbish, and a report
	// that looks corrupt is a report nobody attaches.
	const char bom[3] = { (char)0xEF, (char)0xBB, (char)0xBF };
	WriteFile(h, bom, 3, &wrote, nullptr);
	const BOOL ok = WriteFile(h, body.data(), (DWORD)body.size(), &wrote, nullptr);
	CloseHandle(h);
	return ok != FALSE;
}

std::string RoleLine()
{
	std::string s = g_role;
	if (!g_game.empty()) s += " (" + g_game + ")";
	return s;
}

// Builds and writes one report. `kind` is "hang" or "crash"; `head` is the two
// or three lines specific to that kind. Returns the file name, or empty.
std::string WriteReport(const char* kind, const std::string& head,
                        const std::vector<DWORD>& tids, const CONTEXT* crashCtx,
                        DWORD crashTid)
{
	const std::wstring dir = PobLog::LogDir();
	if (dir.empty()) return std::string();

	SYSTEMTIME st{};
	GetLocalTime(&st);
	const std::vector<ModuleRec> mods = Modules();

	std::string body;
	body += u8"PobTools 問題報告（";
	body += (strcmp(kind, "crash") == 0) ? u8"崩潰" : u8"沒有回應";
	body += u8"）\r\n";
	body += u8"時間: " + Stamp(st) + "\r\n";
	body += u8"版本: v" POBTOOLS_VERSION_STRING "\r\n";
	body += u8"角色: " + RoleLine() + u8"    行程: " + std::to_string(GetCurrentProcessId()) + "\r\n";
	body += head;
	const std::string stage = ReadStage();
	const unsigned long long since = g_stageSince.load(std::memory_order_relaxed);
	body += u8"正在進行: " + (stage.empty() ? std::string(u8"（沒有記錄）") : stage);
	if (!stage.empty() && since)
		body += u8"（已 " + std::to_string((GetTickCount64() - since) / 1000) + u8" 秒）";
	body += "\r\n";

	int walked = 0;
	for (DWORD tid : tids) {
		if (walked >= kMaxThreads) break;
		walked++;
		body += "\r\n--- ";
		body += u8"執行緒 " + std::to_string(tid);
		if (tid == g_mainTid) body += u8"（主）";
		if (crashCtx && tid == crashTid) body += u8"（發生例外）";
		body += " ---\r\n";
		std::vector<std::string> frames;
		if (crashCtx && tid == crashTid) {
			// The exception's own context: nothing to suspend, the thread is us.
			StackSnap s;
			s.ctx = *crashCtx;
#ifdef _M_X64
			const uintptr_t sp = (uintptr_t)crashCtx->Rsp;
#else
			const uintptr_t sp = (uintptr_t)crashCtx->Esp;
#endif
			s.stack.resize(kStackBytes);
			SIZE_T got = 0;
			if (ReadProcessMemory(GetCurrentProcess(), (void*)sp, s.stack.data(), kStackBytes, &got) && got) {
				s.stack.resize(got);
				s.stackBase = sp;
				s.ok = true;
			}
			frames = WalkFrames(s, mods);
		} else {
			frames = WalkFrames(CaptureThread(tid), mods);
		}
		if (frames.empty()) body += u8"  （抓不到堆疊）\r\n";
		for (const std::string& f : frames) body += "  " + f + "\r\n";
	}

	body += u8"\r\n--- 模組（配對符號檔用）---\r\n";
	for (const ModuleRec& m : mods) {
		body += "  " + m.name + "  base=0x" + Hex(m.base, 12) +
		        " size=0x" + Hex(m.size, 6) + " stamp=0x" + Hex(m.stamp, 8) + "\r\n";
	}

	char name[128];
	sprintf_s(name, "%s-%04d-%02d-%02d-%02d%02d%02d-%s.txt", kind,
	          st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
	          SafeRole(g_role).c_str());
	if (!WriteFileUtf8(dir + std::wstring(name, name + strlen(name)), body)) return std::string();
	return name;
}

// ---- the watchdog thread -----------------------------------------------------

bool WindowAnswers()
{
	// Our own top-level windows. One that answers WM_NULL means the message pump
	// is alive -- a window drag, a menu or a modal file dialog all look like this
	// while the frame loop is stopped, and none of them is a hang.
	struct Ctx { DWORD pid; bool answered; bool found; } ctx{GetCurrentProcessId(), false, false};
	EnumWindows([](HWND h, LPARAM lp) -> BOOL {
		Ctx* c = (Ctx*)lp;
		DWORD pid = 0;
		GetWindowThreadProcessId(h, &pid);
		if (pid != c->pid || !IsWindowVisible(h) || GetWindow(h, GW_OWNER)) return TRUE;
		c->found = true;
		DWORD_PTR res = 0;
		if (SendMessageTimeoutW(h, WM_NULL, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 1000, &res)) {
			c->answered = true;
			return FALSE;
		}
		return TRUE;
	}, (LPARAM)&ctx);
	if (!ctx.found) return true;   // no window: gate two cannot be satisfied, so never report
	return ctx.answered;
}

void ReportHang(unsigned long long stalledMs)
{
	std::lock_guard<std::mutex> lk(g_reportMx);
	if (g_reports.load() >= kMaxReports) return;
	g_reports.fetch_add(1);

	const std::string head = u8"沒有回應: " + std::to_string(stalledMs / 1000) + u8" 秒\r\n";

	std::vector<DWORD> tids;
	if (g_mainTid) tids.push_back(g_mainTid);
	for (DWORD t : OtherThreads()) tids.push_back(t);

	const std::string file = WriteReport("hang", head, tids, nullptr, 0);
	const std::string stage = ReadStage();
	PobLog::Error("hang", RoleLine() + u8" 沒有回應 " + std::to_string(stalledMs / 1000) + u8" 秒" +
	                      (stage.empty() ? std::string() : u8"；正在進行 " + stage) +
	                      (file.empty() ? std::string(u8"；報告寫不出來") : u8"；詳見 " + file));
}

void PollPeers()
{
	std::vector<HangWatch::Peer> peers;
	{
		std::lock_guard<std::mutex> lk(g_peerMx);
		peers = g_peers;
	}
	// pid -> when it stopped answering, and whether it has been logged already.
	struct PeerState { unsigned long long since = 0; bool logged = false; };
	static std::map<unsigned long, PeerState> state;
	std::map<unsigned long, PeerState> next;

	const unsigned long long now = GetTickCount64();
	HangWatch::PeerStatus status;
	for (const HangWatch::Peer& p : peers) {
		if (!p.hwnd || !IsWindow((HWND)p.hwnd)) continue;
		DWORD_PTR res = 0;
		const bool alive = SendMessageTimeoutW((HWND)p.hwnd, WM_NULL, 0, 0,
		                                       SMTO_ABORTIFHUNG | SMTO_BLOCK, 1000, &res) != 0;
		PeerState ps = state.count(p.pid) ? state[p.pid] : PeerState{};
		if (alive) {
			ps = PeerState{};
		} else {
			if (!ps.since) ps.since = now;
			const unsigned long long stalled = now - ps.since;
			if (stalled >= g_stallMs) {
				if (!status.hung) {
					status.hung = true;
					status.label = p.label;
					status.seconds = (unsigned)(stalled / 1000);
				}
				if (!ps.logged) {
					ps.logged = true;
					// The child writes its own stack; this line is what remains if it
					// was wedged badly enough that even its watchdog never ran.
					PobLog::Error("hang", p.label + u8" (pid " + std::to_string(p.pid) + u8") 沒有回應 " +
					                      std::to_string(stalled / 1000) + u8" 秒");
				}
			}
		}
		next[p.pid] = ps;
	}
	state.swap(next);
	std::lock_guard<std::mutex> lk(g_peerMx);
	g_peerStatus = status;
}

void WatchLoop()
{
	// Checks are frequent relative to the threshold so a stall is noticed near
	// the moment it crosses, not up to a full period later.
	DWORD period = g_stallMs / 4;
	if (period < 100) period = 100;
	if (period > 2000) period = 2000;

	bool reported = false;
	while (g_running.load()) {
		if (WaitForSingleObject(g_stopEvt, period) == WAIT_OBJECT_0) break;
		if (!g_running.load()) break;

		PollPeers();

		const unsigned long long beat = g_beat.load(std::memory_order_relaxed);
		const unsigned long long now = GetTickCount64();
		const unsigned long long stalled = (beat && now > beat) ? now - beat : 0;
		if (stalled < g_stallMs) { reported = false; continue; }
		if (reported) continue;                     // one report per episode
		if (g_requireWindow && WindowAnswers()) continue;   // gate two: not a hang
		reported = true;
		ReportHang(stalled);
	}
}

} // namespace

bool HangWatch::Enabled()
{
	wchar_t buf[8] = {};
	if (GetEnvironmentVariableW(L"POB_ZH_HANGWATCH", buf, 8) > 0 && buf[0] == L'0') return false;
	return true;
}

void HangWatch::Start(const Options& opt)
{
	if (g_running.load()) return;
	if (!Enabled()) return;
	g_role = opt.role ? opt.role : "app";
	g_game = opt.game ? opt.game : "";
	g_stallMs = opt.stallMs < 500 ? 500 : opt.stallMs;
	g_requireWindow = opt.requireWindow;
	g_mainTid = GetCurrentThreadId();
	g_beat.store(GetTickCount64(), std::memory_order_relaxed);
	SetStage("start");
	if (!g_prevFilter) g_prevFilter = SetUnhandledExceptionFilter(&HangWatch::CrashFilter);
	g_stopEvt = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	g_running.store(true);
	g_watch = std::thread(WatchLoop);
}

void HangWatch::Stop()
{
	if (!g_running.exchange(false)) return;
	if (g_stopEvt) SetEvent(g_stopEvt);
	if (g_watch.joinable()) g_watch.join();
	if (g_stopEvt) { CloseHandle(g_stopEvt); g_stopEvt = nullptr; }
}

void HangWatch::Beat()
{
	g_beat.store(GetTickCount64(), std::memory_order_relaxed);
}

void HangWatch::Stage(const char* tag)
{
	if (g_mainTid && GetCurrentThreadId() != g_mainTid) return;
	SetStage(tag);
}

HangWatch::Scope::Scope(const char* tag) : active_(false)
{
	prev_[0] = '\0';
	if (g_mainTid && GetCurrentThreadId() != g_mainTid) return;
	const std::string cur = ReadStage();
	const size_t n = cur.size() < sizeof(prev_) - 1 ? cur.size() : sizeof(prev_) - 1;
	memcpy(prev_, cur.c_str(), n);
	prev_[n] = '\0';
	active_ = true;
	SetStage(tag);
}

HangWatch::Scope::~Scope()
{
	if (active_) SetStage(prev_);
}

void HangWatch::ResetForTest()
{
	Stop();
	g_reports.store(0);
	g_mainTid = 0;
	SetStage("");
	std::lock_guard<std::mutex> lk(g_peerMx);
	g_peers.clear();
	g_peerStatus = PeerStatus{};
}

std::string HangWatch::CurrentStageForTest()
{
	return ReadStage();
}

void HangWatch::SetPeers(const std::vector<Peer>& peers)
{
	std::lock_guard<std::mutex> lk(g_peerMx);
	g_peers = peers;
}

HangWatch::PeerStatus HangWatch::GetPeerStatus()
{
	std::lock_guard<std::mutex> lk(g_peerMx);
	return g_peerStatus;
}

void HangWatch::ReportCrash(_EXCEPTION_POINTERS* ep)
{
	if (!ep || !ep->ExceptionRecord || !ep->ContextRecord) return;
	{
		std::lock_guard<std::mutex> lk(g_reportMx);
		const EXCEPTION_RECORD* er = ep->ExceptionRecord;
		char detail[256];
		if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2) {
			sprintf_s(detail, "%s 0x%llx",
			          er->ExceptionInformation[0] ? "write to" : "read from",
			          (unsigned long long)er->ExceptionInformation[1]);
		} else {
			detail[0] = '\0';
		}
		std::string head = u8"例外: 0x" + Hex(er->ExceptionCode, 8) +
		                   u8"  位址: 0x" + Hex((uintptr_t)er->ExceptionAddress, 12);
		if (detail[0]) head += std::string("  (") + detail + ")";
		head += "\r\n";

		std::vector<DWORD> tids;
		tids.push_back(GetCurrentThreadId());
		if (g_mainTid && g_mainTid != GetCurrentThreadId()) tids.push_back(g_mainTid);
		for (DWORD t : OtherThreads()) tids.push_back(t);

		const std::string file = WriteReport("crash", head, tids, ep->ContextRecord, GetCurrentThreadId());
		const std::string stage = ReadStage();
		PobLog::Error("crash", RoleLine() + u8" 崩潰,例外 0x" + Hex(er->ExceptionCode, 8) +
		                       u8" 於 0x" + Hex((uintptr_t)er->ExceptionAddress, 12) +
		                       (stage.empty() ? std::string() : u8"；正在進行 " + stage) +
		                       (file.empty() ? std::string(u8"；報告寫不出來") : u8"；詳見 " + file));
	}
}

long __stdcall HangWatch::CrashFilter(_EXCEPTION_POINTERS* ep)
{
	ReportCrash(ep);
	// Never swallow it: the engine's own handler still has to show its message and
	// the process still has to die the way it would have.
	return g_prevFilter ? g_prevFilter(ep) : EXCEPTION_CONTINUE_SEARCH;
}
