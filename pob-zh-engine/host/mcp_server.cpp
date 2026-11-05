#include "mcp_server.h"

#include "app_version.h"
#include "error_log.h"
#include "headless_proc.h"
#include "launcher_config.h"
#include "pob_sandbox.h"

#include <json.hpp>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>

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
	HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
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
			CopyTree(pobDir_, sandbox_, st, true);
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
			if (st.errors > 0 || st.files < 100) {
				err = "Could not prepare the sandbox copy of Path of Building (" + std::to_string(st.errors) + " errors).";
				return false;
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
	if (tool == "suggest_nodes" || tool == "what_if" || tool == "load_build") return 180000;
	return 60000;
}

json CallTool(Engine& eng, const std::string& name, const json& args, bool debug)
{
	if (debug && name == "debug_kill_engine") {
		eng.Kill();
		return ToolResult(json{{"ok", true}}, false);
	}
	std::string err;
	bool restarted = false;
	if (!eng.Ensure(err, restarted)) return ToolError(err, restarted);
	json out;
	bool ok = eng.Child()->Call("mcp_call", json{{"name", name}, {"arguments", args.is_object() ? args : json::object()}}, out, TimeoutFor(name));
	if (!ok) {
		if (!eng.Child()->Alive()) {
			return ToolError("The Path of Building engine stopped while running " + name + ".", true);
		}
		std::string msg = out.is_object() ? out.value("message", out.dump()) : out.dump();
		if (msg == "null" || msg.empty()) msg = name + " timed out";
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
	g_in = GetStdHandle(STD_INPUT_HANDLE);
	g_out = GetStdHandle(STD_OUTPUT_HANDLE);
	if (g_in == INVALID_HANDLE_VALUE || g_in == nullptr || g_out == INVALID_HANDLE_VALUE || g_out == nullptr) return 2;
	// The engine child is spawned with handle inheritance on; it must not hold
	// the client's pipes open (the client would wait on it for EOF).
	SetHandleInformation(g_in, HANDLE_FLAG_INHERIT, 0);
	SetHandleInformation(g_out, HANDLE_FLAG_INHERIT, 0);
	HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
	if (err && err != INVALID_HANDLE_VALUE) SetHandleInformation(err, HANDLE_FLAG_INHERIT, 0);

	std::wstring pobDir = pobDirOverride;
	if (pobDir.empty()) pobDir = DetectInstalls(exeDir).poe1Dir;
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
