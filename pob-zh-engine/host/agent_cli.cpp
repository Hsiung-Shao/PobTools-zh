// pobtools.exe: the MCP tools as shell commands, for AI agents that run
// commands rather than speak MCP. Each command is a JSON-RPC request over
// AgentPipeName(session) to `pob-zh.exe --agent-serve` (started here when none
// answers), which keeps the build between commands. See mcp_server.h.
//
//   pobtools [--session <name>] [--pob <dir>] <command> [args...]
//     tools | <tool> [key=value | @file.json | -]... | report <source> | status | stop | serve
//
// stdout is one line of JSON (report: the Markdown), UTF-8. Exit 0 ok, 1 the
// tool answered an error, 2 the daemon could not be reached.

#include "mcp_server.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "win_util.h"

#include <json.hpp>

#include <algorithm>
#include <string>
#include <vector>

using json = nlohmann::json;

namespace {

// Longer than the daemon's slowest wait: a boot (180 s) and then build_report (280 s).
const DWORD kCallMs = 600000;
const DWORD kStartMs = 180000;

std::wstring g_exeDir;

void WriteAll(DWORD which, const std::string& s)
{
	HANDLE h = GetStdHandle(which);
	size_t off = 0;
	DWORD w = 0;
	while (off < s.size() && WriteFile(h, s.data() + off, (DWORD)std::min<size_t>(s.size() - off, 1 << 20), &w, nullptr) && w > 0) off += w;
}
void Out(const json& j) { WriteAll(STD_OUTPUT_HANDLE, j.dump(-1, ' ', false, json::error_handler_t::replace) + "\n"); }
int Fail(const std::string& msg)
{
	WriteAll(STD_ERROR_HANDLE, "pobtools: " + msg + "\n");
	return 2;
}
int ToolFail(const std::string& msg)
{
	Out(json{{"error", msg}});
	return 1;
}

std::wstring Quote(const std::wstring& a)
{
	std::wstring o = L"\"";
	size_t bs = 0;
	for (wchar_t ch : a) {
		if (ch == L'\\') { bs++; continue; }
		o.append(ch == L'"' ? bs * 2 + 1 : bs, L'\\');
		bs = 0;
		o += ch;
	}
	o.append(bs * 2, L'\\');
	return o + L"\"";
}

// One overlapped read or write, given up after ms.
bool Io(HANDLE h, bool write, void* buf, DWORD n, DWORD& done, DWORD ms)
{
	OVERLAPPED ov{};
	ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	done = 0;
	BOOL ok = write ? WriteFile(h, buf, n, nullptr, &ov) : ReadFile(h, buf, n, nullptr, &ov);
	if (!ok && GetLastError() == ERROR_IO_PENDING) {
		if (WaitForSingleObject(ov.hEvent, ms) != WAIT_OBJECT_0) CancelIoEx(h, &ov);
		ok = TRUE;
	}
	ok = ok && GetOverlappedResult(h, &ov, &done, TRUE);
	CloseHandle(ov.hEvent);
	return ok && done > 0;
}

// The pipe's server must be this install's pob-zh.exe: a name in the pipe
// namespace can be taken by any process.
bool OurDaemon(HANDLE pipe, DWORD& pid)
{
	if (!GetNamedPipeServerProcessId(pipe, &pid)) return false;
	HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
	if (!p) return false;
	wchar_t path[MAX_PATH * 4] = {};
	DWORD n = MAX_PATH * 4;
	const bool ok = QueryFullProcessImageNameW(p, 0, path, &n) && _wcsicmp(path, (g_exeDir + L"pob-zh.exe").c_str()) == 0;
	CloseHandle(p);
	return ok;
}

// A connection to the session's daemon. `none`: no daemon has that pipe.
HANDLE Connect(const std::wstring& session, DWORD ms, bool& none, DWORD& pid, std::string& err)
{
	const std::wstring name = AgentPipeName(session);
	const ULONGLONG end = GetTickCount64() + ms;
	none = false;
	for (;;) {
		// SECURITY_IDENTIFICATION: the server may learn who we are, not act as us.
		HANDLE h = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
		                       FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
		if (h != INVALID_HANDLE_VALUE) {
			if (OurDaemon(h, pid)) return h;
			CloseHandle(h);
			err = "session " + narrow(session) + " is served by another program (not " + narrow(g_exeDir) + "pob-zh.exe)";
			return INVALID_HANDLE_VALUE;
		}
		const DWORD e = GetLastError();
		const ULONGLONG now = GetTickCount64();
		if (e == ERROR_FILE_NOT_FOUND) { none = true; err = "no daemon for session " + narrow(session); return INVALID_HANDLE_VALUE; }
		if (e != ERROR_PIPE_BUSY || now >= end) { err = "cannot open the session pipe (error " + std::to_string(e) + ")"; return INVALID_HANDLE_VALUE; }
		WaitNamedPipeW(name.c_str(), (DWORD)std::min<ULONGLONG>(end - now, 1000));
	}
}

std::wstring DaemonCommand(const std::wstring& session, const std::wstring& pob)
{
	std::wstring cmd = Quote(g_exeDir + L"pob-zh.exe") + L" --agent-serve --session " + session;
	if (!pob.empty()) cmd += L" --pob " + Quote(pob);
	return cmd;
}

// Connects, starting the daemon first if there is none.
HANDLE Open(const std::wstring& session, const std::wstring& pob, DWORD& pid, std::string& err)
{
	bool none = false;
	HANDLE h = Connect(session, kCallMs, none, pid, err);
	if (h != INVALID_HANDLE_VALUE || !none) return h;
	// Detached and out of the caller's job, so it outlives this command and the
	// agent's shell; no handles inherited, so it never holds our stdout open.
	const std::wstring exe = g_exeDir + L"pob-zh.exe";
	std::wstring cmd = DaemonCommand(session, pob);
	STARTUPINFOW si{sizeof(si)};
	PROCESS_INFORMATION pi{};
	BOOL started = FALSE;
	for (DWORD flags : {(DWORD)(DETACHED_PROCESS | CREATE_BREAKAWAY_FROM_JOB), (DWORD)DETACHED_PROCESS}) {
		std::vector<wchar_t> buf(cmd.begin(), cmd.end());
		buf.push_back(0);
		started = CreateProcessW(exe.c_str(), buf.data(), nullptr, nullptr, FALSE, flags, nullptr, g_exeDir.c_str(), &si, &pi);
		if (started || GetLastError() != ERROR_ACCESS_DENIED) break; // a job that forbids breakaway
	}
	if (!started) {
		err = "cannot start " + narrow(exe) + " (error " + std::to_string(GetLastError()) + ")";
		return INVALID_HANDLE_VALUE;
	}
	CloseHandle(pi.hThread);
	const ULONGLONG end = GetTickCount64() + kStartMs;
	for (;;) {
		h = Connect(session, kCallMs, none, pid, err);
		// A daemon started by a concurrent command may have won the name: ours then exits.
		if (h != INVALID_HANDLE_VALUE || !none) break;
		if (WaitForSingleObject(pi.hProcess, 100) == WAIT_OBJECT_0) {
			Sleep(200);
			h = Connect(session, kCallMs, none, pid, err);
			if (h == INVALID_HANDLE_VALUE) {
				DWORD code = 0;
				GetExitCodeProcess(pi.hProcess, &code);
				err = "the daemon exited at start (code " + std::to_string(code) + ")";
			}
			break;
		}
		if (GetTickCount64() >= end) { err = "the daemon did not open its pipe in time"; break; }
	}
	CloseHandle(pi.hProcess);
	return h;
}

// One request, one answer line. false: the daemon went away or did not answer in time.
bool Rpc(HANDLE pipe, const std::string& method, const json& params, json& resp, std::string& err)
{
	std::string line = json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", method}, {"params", params}}.dump(-1, ' ', false, json::error_handler_t::replace) + "\n";
	for (size_t off = 0; off < line.size();) {
		DWORD w = 0;
		if (!Io(pipe, true, line.data() + off, (DWORD)(line.size() - off), w, kCallMs)) { err = "the daemon closed the connection"; return false; }
		off += w;
	}
	std::string got;
	char buf[65536];
	size_t nl;
	while ((nl = got.find('\n')) == std::string::npos) {
		DWORD n = 0;
		if (!Io(pipe, false, buf, sizeof(buf), n, kCallMs)) { err = "no answer from the daemon (it stopped or timed out)"; return false; }
		got.append(buf, n);
	}
	resp = json::parse(got.substr(0, nl), nullptr, false);
	if (!resp.is_object()) { err = "the daemon answered something that is not JSON"; return false; }
	return true;
}

// tools/call: prints structuredContent; 0, or 1 when the tool answered an error.
int CallAndPrint(HANDLE pipe, const std::string& tool, const json& args, json* sc = nullptr, bool print = true)
{
	json resp;
	std::string err;
	if (!Rpc(pipe, "tools/call", json{{"name", tool}, {"arguments", args}}, resp, err)) return Fail(err);
	if (resp.contains("error")) return ToolFail(resp["error"].value("message", "error"));
	const json r = resp.value("result", json::object());
	const json out = r.value("structuredContent", json::object());
	if (sc) *sc = out;
	const bool isError = r.value("isError", false);
	if (print || isError) Out(out);
	return isError ? 1 : 0;
}

bool ReadStdin(std::string& out)
{
	HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
	char buf[65536];
	DWORD n = 0;
	while (ReadFile(h, buf, sizeof(buf), &n, nullptr) && n > 0) out.append(buf, n);
	return true;
}

// key=value (value parsed as JSON when it is JSON, else a string), @file.json, - (stdin).
bool ParseArgs(const std::string& tool, const std::vector<std::wstring>& in, json& args, std::string& err)
{
	args = json::object();
	// A string parameter keeps its text: name=3.30 is a file name, not 3.3.
	json props;
	std::string list;
	if (read_file_utf8(g_exeDir + L"Data\\bridge\\mcp_tools.json", list)) {
		const json d = json::parse(list, nullptr, false);
		if (d.is_object())
			for (const auto& t : d.value("tools", json::array()))
				if (t.value("name", "") == tool) props = t.value("inputSchema", json::object()).value("properties", json::object());
	}
	for (const auto& a : in) {
		std::string text;
		if (a == L"-" || (!a.empty() && a[0] == L'@')) {
			if (a == L"-") ReadStdin(text);
			else if (!read_file_utf8(a.substr(1), text)) { err = "cannot read " + narrow(a.substr(1)); return false; }
			const json j = json::parse(text, nullptr, false);
			if (!j.is_object()) { err = (a == L"-" ? std::string("stdin") : narrow(a)) + " is not a JSON object"; return false; }
			for (auto it = j.begin(); it != j.end(); ++it) args[it.key()] = it.value();
			continue;
		}
		const size_t eq = a.find(L'=');
		if (eq == std::wstring::npos || eq == 0) { err = "argument " + narrow(a) + " is not key=value, @file.json or -"; return false; }
		const std::string v = narrow(a.substr(eq + 1));
		const std::string k = narrow(a.substr(0, eq));
		const json j = json::parse(v, nullptr, false);
		const bool asText = props.is_object() && props.contains(k) && props[k].value("type", "") == "string";
		args[k] = j.is_discarded() || asText ? json(v) : j;
	}
	return true;
}

std::wstring FullPath(std::wstring p)
{
	wchar_t buf[MAX_PATH * 4] = {};
	if (GetFullPathNameW(p.c_str(), MAX_PATH * 4, buf, nullptr)) p = buf;
	while (!p.empty() && p.back() == L'\\') p.pop_back();
	return p;
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
	wchar_t self[MAX_PATH * 4] = {};
	GetModuleFileNameW(nullptr, self, MAX_PATH * 4);
	g_exeDir = self;
	g_exeDir.erase(g_exeDir.find_last_of(L'\\') + 1);

	std::wstring session = L"default", pob;
	int i = 1;
	for (; i + 1 < argc; i += 2) {
		if (wcscmp(argv[i], L"--session") == 0) session = argv[i + 1];
		else if (wcscmp(argv[i], L"--pob") == 0) pob = argv[i + 1];
		else break;
	}
	if (i >= argc)
		return Fail("usage: pobtools [--session <name>] [--pob <dir>] tools | <tool> [key=value | @file.json | -]... | report <source> | status | stop | serve");
	if (!ValidAgentSession(session)) return Fail("--session must be 1-32 of A-Z a-z 0-9 _ -");
	const std::wstring cmd = argv[i];
	const std::vector<std::wstring> rest(argv + i + 1, argv + argc);

	if (cmd == L"serve") {
		std::wstring line = DaemonCommand(session, pob);
		std::vector<wchar_t> buf(line.begin(), line.end());
		buf.push_back(0);
		STARTUPINFOW si{sizeof(si)};
		PROCESS_INFORMATION pi{};
		if (!CreateProcessW((g_exeDir + L"pob-zh.exe").c_str(), buf.data(), nullptr, nullptr, FALSE, 0, nullptr, g_exeDir.c_str(), &si, &pi))
			return Fail("cannot start pob-zh.exe (error " + std::to_string(GetLastError()) + ")");
		CloseHandle(pi.hThread);
		WaitForSingleObject(pi.hProcess, INFINITE);
		DWORD code = 2;
		GetExitCodeProcess(pi.hProcess, &code);
		CloseHandle(pi.hProcess);
		return (int)code;
	}

	std::string err;
	DWORD pid = 0;
	if (cmd == L"status" || cmd == L"stop") {
		bool none = false;
		HANDLE h = Connect(session, kCallMs, none, pid, err);
		if (h == INVALID_HANDLE_VALUE) {
			if (!none) return Fail(err);
			Out(cmd == L"status" ? json{{"running", false}, {"session", narrow(session)}, {"pid", 0}} : json{{"stopped", false}});
			return 0;
		}
		if (cmd == L"status") {
			CloseHandle(h);
			Out(json{{"running", true}, {"session", narrow(session)}, {"pid", pid}});
			return 0;
		}
		// Done when the process is gone: it removes its sandbox before exiting.
		HANDLE proc = OpenProcess(SYNCHRONIZE, FALSE, pid);
		json resp;
		const bool asked = Rpc(h, "agent/stop", json::object(), resp, err);
		CloseHandle(h);
		const bool gone = proc && WaitForSingleObject(proc, kCallMs) == WAIT_OBJECT_0;
		if (proc) CloseHandle(proc);
		if (!asked && !gone) return Fail(err);
		Out(json{{"stopped", gone}});
		return gone ? 0 : 2;
	}

	json args = json::object();
	if (cmd != L"tools" && cmd != L"report" && !ParseArgs(narrow(cmd), rest, args, err)) return ToolFail(err);
	if (cmd == L"report" && rest.size() != 1) return ToolFail("usage: pobtools report <share code | build URL | Builds file name>");

	HANDLE pipe = Open(session, pob, pid, err);
	if (pipe == INVALID_HANDLE_VALUE) return Fail(err);
	// --pob only takes effect when the daemon starts: say so rather than work on the other folder.
	if (!pob.empty()) {
		json info;
		const std::string has = Rpc(pipe, "agent/info", json::object(), info, err) ? info.value("result", json::object()).value("pob", "") : "";
		if (!has.empty() && _wcsicmp(FullPath(widen(has)).c_str(), FullPath(pob).c_str()) != 0) {
			CloseHandle(pipe);
			return Fail("session " + narrow(session) + " already runs on " + has + "; run `pobtools stop` first or use another --session");
		}
	}
	int rc = 0;
	if (cmd == L"tools") {
		json resp;
		if (!Rpc(pipe, "tools/list", json::object(), resp, err)) rc = Fail(err);
		else Out(json{{"tools", resp.value("result", json::object()).value("tools", json::array())}});
	} else if (cmd == L"report") {
		json sc;
		// A share code can be longer than a command line allows: @file or - (stdin).
		std::string src = narrow(rest[0]);
		if (rest[0] == L"-") { src.clear(); ReadStdin(src); }
		else if (rest[0][0] == L'@' && !read_file_utf8(rest[0].substr(1), src)) rc = ToolFail("cannot read " + narrow(rest[0].substr(1)));
		if (rc == 0) rc = CallAndPrint(pipe, "load_build", json{{"source", src}}, nullptr, false);
		if (rc == 0) rc = CallAndPrint(pipe, "build_report", json::object(), &sc, false);
		if (rc == 0) {
			const json t = sc.value("text", json());
			if (t.is_string()) WriteAll(STD_OUTPUT_HANDLE, t.get<std::string>());
			else rc = ToolFail("build_report returned no text");
		}
	} else {
		rc = CallAndPrint(pipe, narrow(cmd), args);
	}
	CloseHandle(pipe);
	return rc;
}
