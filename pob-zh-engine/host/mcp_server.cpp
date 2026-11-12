#include "mcp_server.h"

#include "app_update.h"
#include "app_version.h"
#include "error_log.h"
#include "headless_proc.h"
#include "launcher_config.h"
#include "pob_sandbox.h"

#include <json.hpp>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using json = nlohmann::json;

namespace {

const char* const kDefaultProtocol = "2025-06-18";

std::string Narrow(const std::wstring& w)
{
	if (w.empty()) return {};
	int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
	std::string s(n, '\0');
	WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
	return s;
}

std::string ReadAll(const std::wstring& path)
{
	// FILE_SHARE_WRITE: the failure log may be open for appending by another
	// PobTools process at the same moment.
	HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
	if (h == INVALID_HANDLE_VALUE) return {};
	std::string out;
	char buf[65536];
	DWORD got = 0;
	while (ReadFile(h, buf, sizeof(buf), &got, nullptr) && got > 0) out.append(buf, got);
	CloseHandle(h);
	return out;
}

// ---- stdio ------------------------------------------------------------------

HANDLE g_in = INVALID_HANDLE_VALUE;
HANDLE g_out = INVALID_HANDLE_VALUE;
std::wstring g_exeDir;

void WriteMessage(const json& msg)
{
	const std::string line = msg.dump(-1, ' ', false, json::error_handler_t::replace) + "\n";
	DWORD w = 0;
	WriteFile(g_out, line.data(), (DWORD)line.size(), &w, nullptr);
}

// One line from stdin (without the newline). false at EOF.
bool ReadLine(std::string& carry, std::string& line)
{
	for (;;) {
		size_t nl = carry.find('\n');
		if (nl != std::string::npos) {
			line = carry.substr(0, nl);
			carry.erase(0, nl + 1);
			if (!line.empty() && line.back() == '\r') line.pop_back();
			return true;
		}
		char buf[65536];
		DWORD got = 0;
		if (!ReadFile(g_in, buf, sizeof(buf), &got, nullptr) || got == 0) {
			if (carry.empty()) return false;
			line.swap(carry);
			carry.clear();
			return true;
		}
		carry.append(buf, got);
	}
}

// ---- the headless engine ----------------------------------------------------

class Engine {
public:
	Engine(std::wstring exeDir, std::wstring pobDir) : exeDir_(std::move(exeDir)), pobDir_(std::move(pobDir))
	{
		// One sandbox per server process: Claude Desktop, Claude Code and Codex
		// may each run one at the same time, and POB rewrites Settings.xml and
		// Builds\ in its own folder.
		root_ = exeDir_ + L"PobTools\\mcp_sandbox";
		ownDir_ = root_ + L"\\" + std::to_wstring(GetCurrentProcessId());
		sandbox_ = ownDir_ + L"\\PathOfBuildingCommunity";
		wchar_t env[2048] = {};
		if (GetEnvironmentVariableW(L"POB_ZH_BRIDGE", env, 2048)) bridgeOverride_ = env;
	}
	~Engine()
	{
		if (boot_.joinable()) boot_.join();
		if (child_) child_->Stop(3000);
		child_.reset();
		PobSandbox::Rmtree(ownDir_);
	}

	// First boot runs in the background: clients expect `initialize` and
	// `tools/list` answered at once, and POB takes up to a minute to start.
	void StartAsync()
	{
		RemoveStaleSandboxes();
		boot_ = std::thread([this] {
			std::string err;
			bool ok = Boot(true, err);
			std::lock_guard<std::mutex> lk(mu_);
			ready_ = true;
			bootOk_ = ok;
			bootError_ = err;
			cv_.notify_all();
		});
	}

	// Waits for the first boot; retries it if it failed (the user may have
	// set the POB folder since), and restarts the child if it died.
	// `restarted` is set when this call had to restart a child that was up.
	bool Ensure(std::string& err, bool& restarted)
	{
		restarted = false;
		bool ok;
		{
			std::unique_lock<std::mutex> lk(mu_);
			cv_.wait(lk, [this] { return ready_; });
			ok = bootOk_;
		}
		if (!ok) {
			if (!Boot(true, err)) return false;
			std::lock_guard<std::mutex> lk(mu_);
			bootOk_ = true;
			return true;
		}
		if (child_ && child_->Alive()) return true;
		restarted = true;
		return Boot(false, err);
	}

	HeadlessProc::Child* Child() { return child_.get(); }
	const std::wstring& PobDir() const { return pobDir_; }

	void Kill()
	{
		if (child_) child_->Stop(0);
	}

private:
	// Sandboxes of server processes that are gone (crashed, killed by the client).
	void RemoveStaleSandboxes()
	{
		WIN32_FIND_DATAW fd{};
		HANDLE h = FindFirstFileW((root_ + L"\\*").c_str(), &fd);
		if (h == INVALID_HANDLE_VALUE) return;
		std::vector<std::wstring> stale;
		do {
			const std::wstring n = fd.cFileName;
			if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || n == L"." || n == L"..") continue;
			if (n.find_first_not_of(L"0123456789") != std::wstring::npos) {
				stale.push_back(n); // the layout before per-process folders
				continue;
			}
			const DWORD pid = (DWORD)_wtoi(n.c_str());
			if (pid == GetCurrentProcessId()) continue;
			HANDLE p = OpenProcess(SYNCHRONIZE, FALSE, pid);
			const bool alive = p && WaitForSingleObject(p, 0) == WAIT_TIMEOUT;
			if (p) CloseHandle(p);
			if (!alive) stale.push_back(n);
		} while (FindNextFileW(h, &fd));
		FindClose(h);
		for (const auto& n : stale) PobSandbox::Rmtree(root_ + L"\\" + n);
	}

	bool Boot(bool first, std::string& err)
	{
		using namespace PobSandbox;
		if (pobDir_.empty() || !IsDir(pobDir_)) {
			err = "No PoE1 Path of Building install found. Start PobTools once and set the PoE1 folder, or pass --pob <folder>.";
			return false;
		}
		if (first || !IsDir(sandbox_)) {
			if (child_) { child_->Stop(1000); child_.reset(); }
			Rmtree(sandbox_);
			MkdirP(sandbox_);
			CopyStats st;
			LinkTree(pobDir_, sandbox_, st);
			// An installed POB (installed.cfg, left out of the sandbox) keeps its
			// builds under Documents\Path of Building\Builds: bring those along.
			if (GetFileAttributesW((pobDir_ + L"\\installed.cfg").c_str()) != INVALID_FILE_ATTRIBUTES) {
				PWSTR docs = nullptr;
				if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs))) {
					const std::wstring userBuilds = std::wstring(docs) + L"\\Path of Building\\Builds";
					if (IsDir(userBuilds)) CopyTree(userBuilds, sandbox_ + L"\\Builds", st, true);
				}
				CoTaskMemFree(docs);
			}
			if (st.errors > 0 || (st.junctions == 0 && st.files < 100)) {
				err = "Could not prepare the sandbox copy of Path of Building (" + std::to_string(st.errors) + " errors).";
				return false;
			}
			// POB's background update check (Launch.lua runs UpdateCheck.lua from
			// its folder at start and every 12 h) would download a whole POB
			// update into the sandbox's Update\ on every session. The sandbox's
			// copy answers "no update" instead; the user's own file is untouched.
			HANDLE uc = CreateFileW((sandbox_ + L"\\UpdateCheck.lua").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (uc != INVALID_HANDLE_VALUE) {
				static const char stub[] = "return \"none\"\n";
				DWORD w = 0;
				WriteFile(uc, stub, sizeof(stub) - 1, &w, nullptr);
				CloseHandle(uc);
			}
		}
		if (child_) child_->Stop(1000);
		child_ = std::make_unique<HeadlessProc::Child>();
		HeadlessProc::Options opt;
		opt.exeDir = exeDir_;
		opt.launchLua = sandbox_ + L"\\Launch.lua";
		opt.game = L"poe1";
		opt.locale = L"zh-rTW";
		opt.bridgeLua = bridgeOverride_;
		if (!child_->Start(opt, err)) return false;
		json hello, gate;
		if (!child_->WaitEvent("hello", hello, 180000)) {
			err = "Path of Building did not start in the headless engine: " + child_->StrayOutput().substr(0, 400);
			child_->Stop(1000);
			return false;
		}
		if (child_->WaitEvent("gate_result", gate, 10000) && !gate.value("ok", false)) {
			err = "This Path of Building version is not supported by the installed PobTools data pack yet; update PobTools. (" +
			      gate.dump().substr(0, 300) + ")";
			child_->Stop(1000);
			return false;
		}
		json r;
		child_->Call("set_translate", json{{"enabled", true}}, r, 10000);
		// Stop POB caching timeless-jewel tables through the Data junction into
		// the user's folder (mcp_tools.lua M.mcp_setup). It reaches into POB's
		// private locals, so a POB refactor can break it: refuse to run then,
		// like a failed bridge gate, rather than silently writing into the
		// user's install. mcp_tools.lua ships in the data pack, so the fix can.
		json setup;
		if (!child_->Call("mcp_setup", json::object(), setup, 10000) || !setup.value("jewelCacheOff", false)) {
			err = "This Path of Building version is not supported by the installed PobTools data pack yet "
			      "(timeless jewel loader changed); update PobTools.";
			PobLog::Error("mcp", err);
			child_->Stop(1000);
			return false;
		}
		return true;
	}

	std::wstring exeDir_, pobDir_, root_, ownDir_, sandbox_, bridgeOverride_;
	std::unique_ptr<HeadlessProc::Child> child_;
	std::thread boot_;
	std::mutex mu_;
	std::condition_variable cv_;
	bool ready_ = false;
	bool bootOk_ = false;
	std::string bootError_;
};

// ---- tools ------------------------------------------------------------------

json ToolResult(const json& structured, bool isError)
{
	json sc = structured.is_object() ? structured : json{{"result", structured}};
	return json{
		{"content", json::array({json{{"type", "text"}, {"text", sc.dump(-1, ' ', false, json::error_handler_t::replace)}}})},
		{"structuredContent", sc},
		{"isError", isError},
	};
}

// Appended to every error that is PobTools' or the install's fault (not the
// model's arguments). Same words as mcp_tools.lua REPORT_HINT.
const char* kReportHint =
	" (details were written to PobTools\\logs\\error-<date>.log; call report_problem to prepare a bug report for the user)";
const char* kIssueUrl = "https://github.com/Hsiung-Shao/PobTools-zh/issues/new";

// Ensure retries a failed boot on every call; the same reason is written once
// until a boot succeeds again.
std::string g_lastBootError;
void LogOnce(const std::string& msg)
{
	if (msg == g_lastBootError) return;
	g_lastBootError = msg;
	PobLog::Error("mcp", msg);
}

// Setup problems the user can fix themselves: say how, and keep the issue
// tracker for what they cannot.
bool IsSetupProblem(const std::string& err)
{
	return err.find("No PoE1 Path of Building install found") != std::string::npos ||
	       err.find("update PobTools") != std::string::npos;
}
const char* kSetupHint =
	" (this is a setup step the user can do; it was also written to PobTools\\logs\\error-<date>.log -- only if it "
	"keeps failing after that, call report_problem to prepare a bug report)";

json ToolError(const std::string& message, bool restarted = false)
{
	json sc{{"error", message}};
	if (restarted) {
		sc["engineRestarted"] = true;
		sc["notice"] = "The Path of Building engine was restarted; the build was reset. Load it again.";
	}
	return ToolResult(sc, true);
}

unsigned TimeoutFor(const std::string& tool)
{
	if (tool == "suggest_nodes" || tool == "what_if" || tool == "load_build" || tool == "rank_anoints" || tool == "rank_supports") return 180000;
	return 60000;
}

json g_clientInfo = json::object();

// Replaces every spelling of the user's profile path, so a report pasted into
// a public issue does not carry the Windows account name.
// Each user-folder spelling that can reach the log: UTF-8 (C++ messages), the
// ANSI code page (Lua chunk names and package.path are ANSI, so a Chinese
// account name arrives in CP950 bytes), forward slashes, JSON-escaped doubled
// backslashes, and the 8.3 short form. Plus the Documents folder, which can be
// redirected outside the profile.
std::vector<std::pair<std::string, std::string>> RedactNeedles()
{
	std::vector<std::pair<std::string, std::string>> out;
	auto addAll = [&](const std::wstring& w, const char* label) {
		if (w.empty()) return;
		std::vector<std::string> forms;
		forms.push_back(Narrow(w));
		int n = WideCharToMultiByte(CP_ACP, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
		if (n > 0) {
			std::string acp(n, '\0');
			WideCharToMultiByte(CP_ACP, 0, w.c_str(), (int)w.size(), acp.data(), n, nullptr, nullptr);
			forms.push_back(acp);
		}
		const size_t base = forms.size();
		for (size_t i = 0; i < base; i++) {
			std::string fwd = forms[i], dbl;
			for (char& c : fwd) if (c == '\\') c = '/';
			for (char c : forms[i]) { dbl += c; if (c == '\\') dbl += '\\'; }
			forms.push_back(fwd);
			forms.push_back(dbl);
		}
		for (auto& f : forms) out.emplace_back(f, label);
	};
	wchar_t prof[MAX_PATH] = {};
	if (GetEnvironmentVariableW(L"USERPROFILE", prof, MAX_PATH) && prof[0]) {
		addAll(prof, "%USERPROFILE%");
		wchar_t shortp[MAX_PATH] = {};
		if (GetShortPathNameW(prof, shortp, MAX_PATH) && _wcsicmp(shortp, prof) != 0) addAll(shortp, "%USERPROFILE%");
	}
	PWSTR docs = nullptr;
	if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs)) && docs) addAll(docs, "%DOCUMENTS%");
	CoTaskMemFree(docs);
	// Longest first, so "C:\Users\name\Documents" is not half-replaced as the profile.
	std::sort(out.begin(), out.end(), [](const auto& x, const auto& y) { return x.first.size() > y.first.size(); });
	return out;
}

// Replaces every spelling of the user's folders, so a report pasted into a
// public issue does not carry the Windows account name. Only whole path
// components match: a profile of C:\Users\a leaves C:\Users\abc alone.
std::string Redact(std::string s)
{
	static const auto needles = RedactNeedles();
	for (const auto& nd : needles) {
		const std::string& needle = nd.first;
		if (needle.empty()) continue;
		std::string lowS = s, lowN = needle;
		for (char& c : lowS) c = (char)tolower((unsigned char)c);
		for (char& c : lowN) c = (char)tolower((unsigned char)c);
		size_t p = lowS.find(lowN);
		while (p != std::string::npos) {
			const size_t end = p + needle.size();
			// The name goes on only if a name character follows (letters, digits,
			// _ - or a multi-byte continuation); anything else ends the path.
			const unsigned char nx = end < s.size() ? (unsigned char)s[end] : 0;
			const bool whole = !(isalnum(nx) || nx == '_' || nx == '-' || nx >= 0x80);
			if (whole) {
				s.replace(p, needle.size(), nd.second);
				lowS.replace(p, needle.size(), nd.second);
				p = lowS.find(lowN, p + nd.second.size());
			} else {
				p = lowS.find(lowN, end);
			}
		}
	}
	return s;
}

std::string PobVersionOf(const std::wstring& pobDir)
{
	const std::string xml = ReadAll(pobDir + L"\\manifest.xml");
	const size_t v = xml.find("<Version");
	if (v == std::string::npos) return "unknown";
	auto attr = [&](const char* key) -> std::string {
		const std::string k = std::string(key) + "=\"";
		const size_t p = xml.find(k, v);
		if (p == std::string::npos || p > xml.find('>', v)) return {};
		const size_t e = xml.find('"', p + k.size());
		return e == std::string::npos ? std::string() : xml.substr(p + k.size(), e - p - k.size());
	};
	std::string ver = attr("number");
	const std::string branch = attr("branch");
	if (!branch.empty()) ver += " (" + branch + ")";
	return ver.empty() ? "unknown" : ver;
}

std::string WindowsVersion()
{
	using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
	OSVERSIONINFOW vi{sizeof(vi)};
	auto fn = (RtlGetVersionFn)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
	if (!fn || fn(&vi) != 0) return "Windows (unknown)";
	return "Windows " + std::to_string(vi.dwMajorVersion) + "." + std::to_string(vi.dwMinorVersion) + "." +
	       std::to_string(vi.dwBuildNumber);
}

// Today's and yesterday's [mcp] lines from the failure log, newest last.
std::vector<std::string> RecentMcpErrors(size_t max)
{
	std::vector<std::string> lines;
	SYSTEMTIME now{};
	GetLocalTime(&now);
	FILETIME ft{};
	SystemTimeToFileTime(&now, &ft);
	ULARGE_INTEGER t{};
	t.LowPart = ft.dwLowDateTime;
	t.HighPart = ft.dwHighDateTime;
	for (int back = 1; back >= 0; back--) {
		ULARGE_INTEGER d = t;
		d.QuadPart -= (ULONGLONG)back * 24ull * 3600ull * 10000000ull;
		FILETIME f{d.LowPart, d.HighPart};
		SYSTEMTIME st{};
		FileTimeToSystemTime(&f, &st);
		wchar_t name[32];
		swprintf(name, 32, L"error-%04d-%02d-%02d.log", st.wYear, st.wMonth, st.wDay);
		const std::string text = ReadAll(PobLog::LogDir() + name);
		size_t pos = 0;
		while (pos < text.size()) {
			size_t e = text.find('\n', pos);
			if (e == std::string::npos) e = text.size();
			std::string ln = text.substr(pos, e - pos);
			if (!ln.empty() && ln.back() == '\r') ln.pop_back();
			if (ln.find("[mcp]") != std::string::npos) lines.push_back(Redact(ln));
			pos = e + 1;
		}
	}
	if (lines.size() > max) lines.erase(lines.begin(), lines.end() - (std::ptrdiff_t)max);
	return lines;
}

json ReportProblem(const Engine& eng)
{
	const std::string pobDir = eng.PobDir().empty() ? "(not found)" : Redact(Narrow(eng.PobDir()));
	const std::string pobVer = eng.PobDir().empty() ? "unknown" : PobVersionOf(eng.PobDir());
	auto str = [](const json& o, const char* k) {
		return o.is_object() && o.contains(k) && o[k].is_string() ? o[k].get<std::string>() : std::string();
	};
	std::string client = str(g_clientInfo, "name") + " " + str(g_clientInfo, "version");
	if (client == " ") client = "unknown";
	const std::string dataVer = ReadLocalDataVersion(g_exeDir);
	const std::vector<std::string> errs = RecentMcpErrors(20);
	std::string text = "### PobTools MCP problem report\n\n";
	text += "- PobTools: " + std::string(POBTOOLS_VERSION_STRING) + "\n";
	text += "- Data pack: " + (dataVer.empty() ? std::string("(none)") : dataVer) + "\n";
	text += "- Path of Building: " + pobVer + " at " + pobDir + "\n";
	text += "- " + WindowsVersion() + "\n";
	text += "- AI client: " + client + "\n\n";
	text += "What I asked the AI to do / what went wrong:\n\n(describe it here)\n\n";
	text += "Recent MCP errors (PobTools\\logs; lines written by the engine carry the engine DLL's own build version):\n\n```\n";
	for (const auto& l : errs) text += l + "\n";
	if (errs.empty()) text += "(none logged)\n";
	text += "```\n\nOpen a new issue at " + std::string(kIssueUrl) + " and paste this.\n";
	return ToolResult(json{
		{"text", text},
		{"issueUrl", kIssueUrl},
		{"pobtoolsVersion", POBTOOLS_VERSION_STRING},
		{"pobVersion", pobVer},
		{"dataVersion", dataVer},
		{"recentErrors", errs},
		{"instructions", "Show the user `text`, ask them to fill in what went wrong, review it, and paste it into a new issue at issueUrl. Do not post it yourself."},
	}, false);
}

json CallTool(Engine& eng, const std::string& name, const json& args, bool debug)
{
	if (name == "report_problem") return ReportProblem(eng);
	// Static reading guide: answered without waiting for the engine, so a model
	// can read it while POB is still starting.
	if (name == "build_guide") {
		const std::string text = ReadAll(g_exeDir + L"Data\\bridge\\mcp_guide.md");
		if (text.empty()) return ToolError("Data\\bridge\\mcp_guide.md is missing from this PobTools install.");
		return ToolResult(json{{"text", text}}, false);
	}
	if (debug && name == "debug_kill_engine") {
		eng.Kill();
		return ToolResult(json{{"ok", true}}, false);
	}
	std::string err;
	bool restarted = false;
	const bool ready = eng.Ensure(err, restarted);
	if (restarted) PobLog::Error("mcp", "the engine had stopped; restarted it before " + name + " (the build was reset)");
	if (!ready) {
		LogOnce(err);
		return ToolError(err + (IsSetupProblem(err) ? kSetupHint : kReportHint), restarted);
	}
	g_lastBootError.clear();
	json out;
	bool ok = eng.Child()->Call("mcp_call", json{{"name", name}, {"arguments", args.is_object() ? args : json::object()}}, out, TimeoutFor(name));
	if (!ok) {
		if (!eng.Child()->Alive()) {
			PobLog::Error("mcp", "the engine stopped while running " + name);
			return ToolError("The Path of Building engine stopped while running " + name + "." + kReportHint, true);
		}
		std::string msg = out.is_object() ? out.value("message", out.dump()) : out.dump();
		if (msg == "null" || msg.empty() || (out.is_object() && out.value("code", "") == "timeout")) {
			PobLog::Error("mcp", name + " timed out");
			msg = name + " timed out" + kReportHint;
		}
		// The bridge appends a Lua traceback; the model only needs the reason.
		const size_t tb = msg.find("\nstack traceback:");
		if (tb != std::string::npos) msg.erase(tb);
		return ToolError(msg, restarted);
	}
	json sc = out.is_object() ? out : json{{"result", out}};
	if (restarted) {
		sc["engineRestarted"] = true;
		sc["notice"] = "The Path of Building engine was restarted; the build was reset.";
	}
	return ToolResult(sc, false);
}

json RpcError(const json& id, int code, const std::string& message)
{
	return json{{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}};
}

} // namespace

int RunMcpServer(const std::wstring& exeDir, const std::wstring& pobDirOverride)
{
	g_exeDir = exeDir;
	g_in = GetStdHandle(STD_INPUT_HANDLE);
	g_out = GetStdHandle(STD_OUTPUT_HANDLE);
	if (g_in == INVALID_HANDLE_VALUE || g_in == nullptr || g_out == INVALID_HANDLE_VALUE || g_out == nullptr) return 2;
	// The engine child is spawned with handle inheritance on; it must not hold
	// the client's pipes open (the client would wait on it for EOF).
	SetHandleInformation(g_in, HANDLE_FLAG_INHERIT, 0);
	SetHandleInformation(g_out, HANDLE_FLAG_INHERIT, 0);
	HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
	if (err && err != INVALID_HANDLE_VALUE) SetHandleInformation(err, HANDLE_FLAG_INHERIT, 0);
	// A client that kills this process must take the engine child with it:
	// an orphaned POB exits later and writes Settings.xml into a sandbox that
	// has already been cleaned up. The job handle closes when this process dies.
	if (HANDLE job = CreateJobObjectW(nullptr, nullptr)) {
		JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
		li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
		SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof(li));
		AssignProcessToJobObject(job, GetCurrentProcess());
	}

	std::wstring pobDir = pobDirOverride;
	if (pobDir.empty()) pobDir = DetectInstalls(exeDir).poe1Dir;
	// A hand-written client config often says --pob D:/... : the sandbox's
	// junctions take this path verbatim, and an NT junction target with '/' in
	// it resolves to nothing (POB then fails on require "xml"). Canonicalise.
	for (wchar_t& ch : pobDir) if (ch == L'/') ch = L'\\';
	if (!pobDir.empty()) {
		wchar_t full[MAX_PATH * 2] = {};
		const DWORD n = GetFullPathNameW(pobDir.c_str(), MAX_PATH * 2, full, nullptr);
		if (n > 0 && n < MAX_PATH * 2) pobDir = full;
	}
	while (!pobDir.empty() && pobDir.back() == L'\\') pobDir.pop_back();

	wchar_t dbg[8] = {};
	const bool debug = GetEnvironmentVariableW(L"POB_ZH_MCP_DEBUG", dbg, 8) > 0 && dbg[0] == L'1';

	json toolsDoc = json::parse(ReadAll(exeDir + L"Data\\bridge\\mcp_tools.json"), nullptr, false);
	if (!toolsDoc.is_object()) {
		PobLog::Error("mcp", "Data\\bridge\\mcp_tools.json missing or invalid");
		toolsDoc = json{{"tools", json::array()}};
	}
	json tools = toolsDoc.value("tools", json::array());
	if (debug) {
		tools.push_back(json{{"name", "debug_kill_engine"}, {"description", "Test only: kill the headless engine."},
		                     {"inputSchema", {{"type", "object"}, {"properties", json::object()}}}});
		tools.push_back(json{{"name", "debug_lua_error"}, {"description", "Test only: raise a Lua runtime error."},
		                     {"inputSchema", {{"type", "object"}, {"properties", json::object()}}}});
	}

	Engine eng(exeDir, pobDir);
	eng.StartAsync();

	std::string carry, line;
	while (ReadLine(carry, line)) {
		if (line.find_first_not_of(" \t") == std::string::npos) continue;
		json msg = json::parse(line, nullptr, false);
		if (msg.is_discarded() || !msg.is_object()) {
			WriteMessage(RpcError(nullptr, -32700, "parse error"));
			continue;
		}
		const bool isRequest = msg.contains("id") && !msg["id"].is_null();
		if (!isRequest) continue; // notifications (initialized, cancelled) need no answer
		const json id = msg["id"];
		// A field of the wrong type (params as an array, a numeric name...) makes
		// nlohmann throw; answer "invalid params" instead of terminating.
		try {
			const std::string method = msg.value("method", "");
			const json params = msg.value("params", json::object());

			if (method == "initialize") {
				std::string want = params.value("protocolVersion", "");
				if (want != "2024-11-05" && want != "2025-03-26" && want != "2025-06-18") want = kDefaultProtocol;
				if (params.contains("clientInfo") && params["clientInfo"].is_object()) g_clientInfo = params["clientInfo"];
				WriteMessage(json{{"jsonrpc", "2.0"}, {"id", id}, {"result", {
					{"protocolVersion", want},
					{"capabilities", {{"tools", json::object()}}},
					{"serverInfo", {{"name", "pobtools"}, {"version", POBTOOLS_VERSION_STRING}}},
					{"instructions", toolsDoc.value("instructions", "")},
				}}});
			} else if (method == "ping") {
				WriteMessage(json{{"jsonrpc", "2.0"}, {"id", id}, {"result", json::object()}});
			} else if (method == "tools/list") {
				WriteMessage(json{{"jsonrpc", "2.0"}, {"id", id}, {"result", {{"tools", tools}}}});
			} else if (method == "tools/call") {
				const std::string name = params.value("name", "");
				bool known = false;
				for (const auto& t : tools) if (t.value("name", "") == name) known = true;
				json result = known ? CallTool(eng, name, params.value("arguments", json::object()), debug)
				                    : ToolError("unknown tool " + name);
				WriteMessage(json{{"jsonrpc", "2.0"}, {"id", id}, {"result", result}});
			} else {
				WriteMessage(RpcError(id, -32601, "method not found: " + method));
			}
		} catch (const std::exception& ex) {
			WriteMessage(RpcError(id, -32602, std::string("invalid params: ") + ex.what()));
		}
	}
	return 0;
}
