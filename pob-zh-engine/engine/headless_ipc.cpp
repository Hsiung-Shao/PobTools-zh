#include "headless_ipc.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <deque>
#include <mutex>
#include <thread>

namespace HeadlessIpc {

namespace {

std::mutex g_inMutex;
std::deque<std::string> g_inQueue;
std::atomic<bool> g_hostGone{false};
std::atomic<bool> g_active{false};
std::thread* g_reader = nullptr; // heap pointer: see hang_watch.cpp for why not a static object
HANDLE g_stdin = INVALID_HANDLE_VALUE;
HANDLE g_stdout = INVALID_HANDLE_VALUE;
std::mutex g_outMutex;

void ReaderProc()
{
	std::string partial;
	char buf[8192];
	for (;;) {
		DWORD got = 0;
		if (!ReadFile(g_stdin, buf, sizeof(buf), &got, nullptr) || got == 0) {
			break; // EOF or broken pipe: the host closed our stdin
		}
		partial.append(buf, got);
		size_t start = 0;
		for (;;) {
			size_t nl = partial.find('\n', start);
			if (nl == std::string::npos) break;
			std::string line = partial.substr(start, nl - start);
			if (!line.empty() && line.back() == '\r') line.pop_back();
			start = nl + 1;
			if (line.empty()) continue;
			std::lock_guard<std::mutex> lock(g_inMutex);
			g_inQueue.push_back(std::move(line));
		}
		partial.erase(0, start);
	}
	g_hostGone = true;
}

} // namespace

bool Start()
{
	if (g_active) return true;
	g_stdin = GetStdHandle(STD_INPUT_HANDLE);
	g_stdout = GetStdHandle(STD_OUTPUT_HANDLE);
	if (g_stdin == INVALID_HANDLE_VALUE || g_stdin == nullptr ||
	    g_stdout == INVALID_HANDLE_VALUE || g_stdout == nullptr) {
		return false;
	}
	// A pipe or a file, never a console: a console stdin would make ReadFile wait
	// for a keyboard, and a console stdout would translate our UTF-8.
	if (GetFileType(g_stdin) == FILE_TYPE_CHAR || GetFileType(g_stdout) == FILE_TYPE_CHAR) {
		return false;
	}
	g_hostGone = false;
	g_active = true;
	g_reader = new std::thread(ReaderProc);
	return true;
}

void Stop()
{
	if (!g_active) return;
	g_active = false;
	// The reader is blocked in a synchronous ReadFile on the stdin pipe.
	// Closing the handle does NOT unblock it (the first version of this hung
	// every child whose host was still alive, which the broken-script self-test
	// caught). CancelSynchronousIo does, but only hits a read that is in flight
	// at that instant, so repeat until the thread is gone; give up and detach
	// rather than never exit.
	if (g_reader) {
		HANDLE th = (HANDLE)g_reader->native_handle();
		for (int i = 0; i < 40 && WaitForSingleObject(th, 0) == WAIT_TIMEOUT; i++) {
			CancelSynchronousIo(th);
			WaitForSingleObject(th, 50);
		}
		if (WaitForSingleObject(th, 0) == WAIT_OBJECT_0) {
			g_reader->join();
		} else {
			g_reader->detach();
		}
		delete g_reader;
		g_reader = nullptr;
	}
	if (g_stdin != INVALID_HANDLE_VALUE) {
		CloseHandle(g_stdin);
		g_stdin = INVALID_HANDLE_VALUE;
	}
}

bool Active() { return g_active; }

bool Pop(std::string& line)
{
	std::lock_guard<std::mutex> lock(g_inMutex);
	if (g_inQueue.empty()) return false;
	line = std::move(g_inQueue.front());
	g_inQueue.pop_front();
	return true;
}

bool HostGone() { return g_hostGone; }

void Send(const std::string& json)
{
	if (!g_active) return;
	std::lock_guard<std::mutex> lock(g_outMutex);
	std::string out = json;
	out.push_back('\n');
	const char* p = out.data();
	size_t left = out.size();
	while (left > 0) {
		DWORD wrote = 0;
		if (!WriteFile(g_stdout, p, (DWORD)left, &wrote, nullptr) || wrote == 0) {
			g_hostGone = true;
			return;
		}
		p += wrote;
		left -= wrote;
	}
	FlushFileBuffers(g_stdout);
}

std::string Quote(const std::string& s)
{
	std::string out;
	out.reserve(s.size() + 2);
	out.push_back('"');
	for (unsigned char c : s) {
		switch (c) {
		case '"': out += "\\\""; break;
		case '\\': out += "\\\\"; break;
		case '\n': out += "\\n"; break;
		case '\r': out += "\\r"; break;
		case '\t': out += "\\t"; break;
		default:
			if (c < 0x20) {
				char hex[8];
				snprintf(hex, sizeof(hex), "\\u%04x", c);
				out += hex;
			} else {
				out.push_back((char)c);
			}
		}
	}
	out.push_back('"');
	return out;
}

void SendEvent(const char* event, const std::string& dataJson)
{
	Send("{\"event\":" + Quote(event) + ",\"data\":" + (dataJson.empty() ? "null" : dataJson) + "}");
}

void SendResult(long long id, const std::string& resultJson)
{
	Send("{\"id\":" + std::to_string(id) + ",\"result\":" + (resultJson.empty() ? "null" : resultJson) + "}");
}

void SendError(long long id, const char* code, const std::string& message)
{
	Send("{\"id\":" + std::to_string(id) + ",\"error\":{\"code\":" + Quote(code) +
	     ",\"message\":" + Quote(message) + "}}");
}

} // namespace HeadlessIpc
