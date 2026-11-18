#include "headless_proc.h"

#include "error_log.h"
#include "pob_sandbox.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include "win_util.h"

#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

using json = nlohmann::json;

namespace HeadlessProc {

namespace {

std::wstring exe_path()
{
	wchar_t buf[MAX_PATH];
	GetModuleFileNameW(nullptr, buf, MAX_PATH);
	return buf;
}

// Both environments, for the same reason pob_launch.cpp does it: this exe is
// /MT, the engine DLL is /MD, and a child inherits the Win32 block.
void set_env_both(const wchar_t* var, const wchar_t* val)
{
	SetEnvironmentVariableW(var, val);
	_wputenv_s(var, val);
}

} // namespace

struct Child::Impl {
	HANDLE process = nullptr;
	HANDLE thread = nullptr;
	HANDLE childStdin = INVALID_HANDLE_VALUE;  // our write end
	HANDLE childStdout = INVALID_HANDLE_VALUE; // our read end
	std::thread* reader = nullptr;

	std::mutex mu;
	std::condition_variable cv;
	std::map<long long, json> responses;
	std::deque<json> events;
	std::string stray;
	bool readerDone = false;
	long long nextId = 1;
	std::function<void(const std::string&, bool)> lineSink;
	std::function<void()> exitSink;

	void ReaderProc()
	{
		std::string partial;
		char buf[16384];
		for (;;) {
			DWORD got = 0;
			if (!ReadFile(childStdout, buf, sizeof(buf), &got, nullptr) || got == 0) break;
			partial.append(buf, got);
			size_t start = 0;
			for (;;) {
				size_t nl = partial.find('\n', start);
				if (nl == std::string::npos) break;
				std::string line = partial.substr(start, nl - start);
				if (!line.empty() && line.back() == '\r') line.pop_back();
				start = nl + 1;
				if (line.empty()) continue;
				Deliver(line);
			}
			partial.erase(0, start);
		}
		if (!partial.empty()) Deliver(partial);
		std::function<void()> onExit;
		{
			std::lock_guard<std::mutex> lock(mu);
			readerDone = true;
			onExit = exitSink;
			cv.notify_all();
		}
		if (onExit) onExit();
	}

	void Deliver(const std::string& line)
	{
		json j;
		bool ok = false;
		if (!line.empty() && line[0] == '{') {
			try { j = json::parse(line); ok = j.is_object(); } catch (...) { ok = false; }
		}
		std::function<void(const std::string&, bool)> sink;
		{
			std::lock_guard<std::mutex> lock(mu);
			sink = lineSink;
		}
		if (sink) {
			// Pass-through: the window forwards the line to the page as-is; parsing
			// it here was only to tell JSON from a stray print.
			sink(line, ok);
			return;
		}
		std::lock_guard<std::mutex> lock(mu);
		if (!ok) {
			stray += line;
			stray += "\n";
			return;
		}
		if (j.contains("id") && j["id"].is_number_integer()) {
			responses[j["id"].get<long long>()] = j;
		} else if (j.contains("event")) {
			events.push_back(j);
		} else {
			stray += line;
			stray += "\n";
		}
		cv.notify_all();
	}
};

Child::Child() : impl_(new Impl) {}

Child::~Child()
{
	Stop(1000);
	if (impl_->reader) {
		if (impl_->reader->joinable()) impl_->reader->join();
		delete impl_->reader;
	}
	if (impl_->childStdout != INVALID_HANDLE_VALUE) CloseHandle(impl_->childStdout);
	if (impl_->childStdin != INVALID_HANDLE_VALUE) CloseHandle(impl_->childStdin);
	if (impl_->thread) CloseHandle(impl_->thread);
	if (impl_->process) CloseHandle(impl_->process);
	delete impl_;
}

bool Child::Start(const Options& opt, std::string& error)
{
	SECURITY_ATTRIBUTES sa{};
	sa.nLength = sizeof(sa);
	sa.bInheritHandle = TRUE;
	HANDLE inRead = nullptr, inWrite = nullptr, outRead = nullptr, outWrite = nullptr;
	if (!CreatePipe(&inRead, &inWrite, &sa, 0) || !CreatePipe(&outRead, &outWrite, &sa, 0)) {
		error = "CreatePipe failed, GetLastError=" + std::to_string(GetLastError());
		return false;
	}
	// Our ends must not leak into the child, or its stdin never sees EOF when we
	// close ours (the child would hold a copy of the write end).
	SetHandleInformation(inWrite, HANDLE_FLAG_INHERIT, 0);
	SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);

	set_env_both(L"POB_ZH_HEADLESS", L"1");
	set_env_both(L"POB_ZH_BRIDGE", opt.bridgeLua.c_str());
	set_env_both(L"POB_GAME", opt.game.c_str());
	set_env_both(L"POB_LOCALE", opt.locale.c_str());
	set_env_both(L"POB_ZH_HANGWATCH", opt.hangWatch ? L"1" : L"0");

	std::wstring exe = exe_path();
	std::wstring cmd = L"\"" + exe + L"\" --engine-headless \"" + opt.launchLua + L"\"";
	std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
	cmdBuf.push_back(L'\0');
	STARTUPINFOW si{};
	si.cb = sizeof(si);
	si.dwFlags = STARTF_USESTDHANDLES;
	si.hStdInput = inRead;
	si.hStdOutput = outWrite;
	si.hStdError = outWrite;
	PROCESS_INFORMATION pi{};
	BOOL ok = CreateProcessW(exe.c_str(), cmdBuf.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
	                         nullptr, nullptr, &si, &pi);
	// The child's copies are its own now; keeping ours open would keep the
	// pipe alive after the child died and hang the reader.
	CloseHandle(inRead);
	CloseHandle(outWrite);
	// Restore the environment for whatever this process spawns next: a classic
	// POB started from the same launcher must not come up headless.
	set_env_both(L"POB_ZH_HEADLESS", L"");
	set_env_both(L"POB_ZH_BRIDGE", L"");
	if (!ok) {
		error = "CreateProcess failed, GetLastError=" + std::to_string(GetLastError());
		PobLog::Error("headless", error);
		CloseHandle(inWrite);
		CloseHandle(outRead);
		return false;
	}
	impl_->process = pi.hProcess;
	impl_->thread = pi.hThread;
	impl_->childStdin = inWrite;
	impl_->childStdout = outRead;
	impl_->reader = new std::thread([this] { impl_->ReaderProc(); });
	return true;
}

bool Child::Call(const std::string& method, const json& params, json& out, unsigned timeoutMs)
{
	long long id;
	{
		std::lock_guard<std::mutex> lock(impl_->mu);
		id = impl_->nextId++;
	}
	json req = { {"id", id}, {"method", method}, {"params", params} };
	std::string line = req.dump();
	line.push_back('\n');
	DWORD wrote = 0;
	if (impl_->childStdin == INVALID_HANDLE_VALUE ||
	    !WriteFile(impl_->childStdin, line.data(), (DWORD)line.size(), &wrote, nullptr)) {
		out = { {"code", "write_failed"}, {"message", "child stdin closed"} };
		return false;
	}
	std::unique_lock<std::mutex> lock(impl_->mu);
	bool got = impl_->cv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] {
		return impl_->responses.count(id) > 0 || impl_->readerDone;
	});
	auto it = impl_->responses.find(id);
	if (it == impl_->responses.end()) {
		out = { {"code", impl_->readerDone ? "child_exited" : "timeout"},
		        {"message", got ? "child exited before answering" : "no response within " + std::to_string(timeoutMs) + " ms"} };
		return false;
	}
	json resp = it->second;
	impl_->responses.erase(it);
	if (resp.contains("error")) {
		out = resp["error"];
		return false;
	}
	out = resp.value("result", json());
	return true;
}

bool Child::WaitEvent(const std::string& name, json& data, unsigned timeoutMs)
{
	std::unique_lock<std::mutex> lock(impl_->mu);
	auto find = [&]() {
		for (auto it = impl_->events.begin(); it != impl_->events.end(); ++it) {
			if ((*it).value("event", "") == name) return it;
		}
		return impl_->events.end();
	};
	impl_->cv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] {
		return find() != impl_->events.end() || impl_->readerDone;
	});
	auto it = find();
	if (it == impl_->events.end()) return false;
	data = (*it).value("data", json());
	impl_->events.erase(it);
	return true;
}

std::vector<json> Child::Events()
{
	std::lock_guard<std::mutex> lock(impl_->mu);
	return std::vector<json>(impl_->events.begin(), impl_->events.end());
}

bool Child::Alive()
{
	if (!impl_->process) return false;
	return WaitForSingleObject(impl_->process, 0) == WAIT_TIMEOUT;
}

bool Child::WaitExit(unsigned timeoutMs)
{
	if (!impl_->process) return true;
	return WaitForSingleObject(impl_->process, timeoutMs) == WAIT_OBJECT_0;
}

unsigned long Child::ExitCode()
{
	DWORD code = (DWORD)-1;
	if (impl_->process) GetExitCodeProcess(impl_->process, &code);
	return code;
}

void Child::Stop(unsigned graceMs)
{
	if (impl_->childStdin != INVALID_HANDLE_VALUE) {
		CloseHandle(impl_->childStdin);
		impl_->childStdin = INVALID_HANDLE_VALUE;
	}
	if (impl_->process && !WaitExit(graceMs)) {
		TerminateProcess(impl_->process, 9);
		WaitExit(2000);
	}
}

std::string Child::StrayOutput()
{
	std::lock_guard<std::mutex> lock(impl_->mu);
	return impl_->stray;
}

bool Child::SendRaw(const std::string& jsonLine)
{
	if (impl_->childStdin == INVALID_HANDLE_VALUE) return false;
	std::string line = jsonLine;
	line.push_back('\n');
	DWORD wrote = 0;
	return WriteFile(impl_->childStdin, line.data(), (DWORD)line.size(), &wrote, nullptr) && wrote == line.size();
}

void Child::SetLineSink(std::function<void(const std::string&, bool)> sink)
{
	std::lock_guard<std::mutex> lock(impl_->mu);
	impl_->lineSink = std::move(sink);
}

void Child::SetExitSink(std::function<void()> sink)
{
	std::lock_guard<std::mutex> lock(impl_->mu);
	impl_->exitSink = std::move(sink);
}

bool BootSandboxed(const std::wstring& exeDir, const std::wstring& pobDirIn, const std::wstring& sandboxDir, bool rebuild,
                   const std::wstring& bridgeOverride, std::unique_ptr<Child>& child, std::string& err)
{
	using namespace PobSandbox;
	// FindPoe1Dir ends in a backslash; junctions built from "dir\\name" point nowhere
	std::wstring pobDir = pobDirIn;
	while (pobDir.size() > 3 && (pobDir.back() == L'\\' || pobDir.back() == L'/')) pobDir.pop_back();
	if (rebuild || !IsDir(sandboxDir)) {
		if (child) { child->Stop(1000); child.reset(); }
		Rmtree(sandboxDir);
		MkdirP(sandboxDir);
		CopyStats st;
		LinkTree(pobDir, sandboxDir, st);
		// An installed POB (installed.cfg, left out of the sandbox) keeps its
		// builds under Documents\Path of Building\Builds: bring those along.
		if (GetFileAttributesW((pobDir + L"\\installed.cfg").c_str()) != INVALID_FILE_ATTRIBUTES) {
			PWSTR docs = nullptr;
			if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs))) {
				const std::wstring userBuilds = std::wstring(docs) + L"\\Path of Building\\Builds";
				if (IsDir(userBuilds)) CopyTree(userBuilds, sandboxDir + L"\\Builds", st, true);
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
		HANDLE uc = CreateFileW((sandboxDir + L"\\UpdateCheck.lua").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (uc != INVALID_HANDLE_VALUE) {
			static const char stub[] = "return \"none\"\n";
			DWORD w = 0;
			WriteFile(uc, stub, sizeof(stub) - 1, &w, nullptr);
			CloseHandle(uc);
		}
	}
	if (child) child->Stop(1000);
	child = std::make_unique<HeadlessProc::Child>();
	HeadlessProc::Options opt;
	opt.exeDir = exeDir;
	opt.launchLua = sandboxDir + L"\\Launch.lua";
	opt.game = L"poe1";
	opt.locale = L"zh-rTW";
	opt.bridgeLua = bridgeOverride;
	if (!child->Start(opt, err)) return false;
	json hello, gate;
	if (!child->WaitEvent("hello", hello, 180000)) {
		err = "Path of Building did not start in the headless engine: " + child->StrayOutput().substr(0, 400);
		child->Stop(1000);
		return false;
	}
	if (child->WaitEvent("gate_result", gate, 10000) && !gate.value("ok", false)) {
		err = "This Path of Building version is not supported by the installed PobTools data pack yet; update PobTools. (" +
		      gate.dump().substr(0, 300) + ")";
		child->Stop(1000);
		return false;
	}
	json r;
	child->Call("set_translate", json{{"enabled", true}}, r, 10000);
	// Stop POB caching timeless-jewel tables through the Data junction into
	// the user's folder (mcp_tools.lua M.mcp_setup). It reaches into POB's
	// private locals, so a POB refactor can break it: refuse to run then,
	// like a failed bridge gate, rather than silently writing into the
	// user's install. mcp_tools.lua ships in the data pack, so the fix can.
	json setup;
	if (!child->Call("mcp_setup", json::object(), setup, 10000) || !setup.value("jewelCacheOff", false)) {
		err = "This Path of Building version is not supported by the installed PobTools data pack yet "
		      "(timeless jewel loader changed); update PobTools.";
		PobLog::Error("mcp", err);
		child->Stop(1000);
		return false;
	}
	return true;
}

} // namespace HeadlessProc
