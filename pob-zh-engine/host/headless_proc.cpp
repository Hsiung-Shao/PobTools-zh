#include "headless_proc.h"

#include "error_log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

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

std::string narrow(const std::wstring& w)
{
	if (w.empty()) return {};
	int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
	std::string s(n, '\0');
	WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
	return s;
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

} // namespace HeadlessProc
