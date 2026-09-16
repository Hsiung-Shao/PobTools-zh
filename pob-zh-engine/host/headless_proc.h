// A headless POB child (pob-zh.exe --engine-headless <Launch.lua>) and the
// JSON Lines conversation with it over its stdin/stdout.
//
// The child runs POB's own Lua in our engine with no window; Data\bridge\
// bridge.lua inside it answers method calls (engine/headless_ipc.h has the
// wire shape). This is the host half: spawn with pipes, a reader thread that
// splits lines into responses (by id) and events (by name), and a blocking
// Call() for the caller that wants an answer.
#pragma once

#include <json.hpp>

#include <functional>
#include <string>
#include <vector>

namespace HeadlessProc {

struct Options {
	std::wstring exeDir;      // where pob-zh.exe lives (trailing backslash)
	std::wstring launchLua;   // the POB install's Launch.lua
	std::wstring bridgeLua;   // empty = engine default (<exeDir>Data\bridge\bridge.lua)
	std::wstring game;        // "poe1" / "poe2" -> POB_GAME
	std::wstring locale;      // -> POB_LOCALE ("" = no translation)
	bool hangWatch = false;   // the watchdog's window gate never opens headless; off by default
};

class Child {
public:
	Child();
	~Child();
	Child(const Child&) = delete;
	Child& operator=(const Child&) = delete;

	// Spawns the child. false (with `error` filled) when CreateProcess failed.
	bool Start(const Options& opt, std::string& error);

	// Sends {"id","method","params"} and waits up to timeoutMs for the matching
	// response. Returns false on timeout, child exit, or an "error" response (the
	// error object is then in `out`).
	bool Call(const std::string& method, const nlohmann::json& params, nlohmann::json& out,
	          unsigned timeoutMs = 30000);

	// Waits up to timeoutMs for an event with this name (consuming it). Events
	// that arrived earlier are kept, so the order of Start()/WaitEvent() calls
	// does not matter.
	bool WaitEvent(const std::string& name, nlohmann::json& data, unsigned timeoutMs = 30000);

	// Every event received so far, in order, with the ones WaitEvent consumed
	// removed. For the self-test's "did it say X" checks.
	std::vector<nlohmann::json> Events();

	bool Alive();
	// Blocks up to timeoutMs for the child to exit; false if still running.
	bool WaitExit(unsigned timeoutMs);
	unsigned long ExitCode(); // valid once !Alive()

	// Closes our end of stdin (the child treats that as "host gone" and exits),
	// waits briefly, then terminates if it did not.
	void Stop(unsigned graceMs = 3000);

	// Everything the child wrote that was not JSON (a crash dump, a stray
	// print). Kept so a failing self-test can show it.
	std::string StrayOutput();

	// --- pass-through mode (the new UI window) -------------------------------
	// The WebView2 window does not want typed calls: the page speaks the wire
	// format itself, so the host only forwards lines both ways.
	//
	// Writes one line (no newline) to the child's stdin. false = stdin closed.
	bool SendRaw(const std::string& jsonLine);
	// Once set, every line the child writes goes to `sink` (reader thread!) and
	// is NOT queued for Call/WaitEvent. `isJson` is false for a stray line. Pass
	// nullptr to go back to the queued mode.
	void SetLineSink(std::function<void(const std::string& line, bool isJson)> sink);
	// Called once (reader thread) when the child's stdout reaches EOF.
	void SetExitSink(std::function<void()> sink);

private:
	struct Impl;
	Impl* impl_;
};

} // namespace HeadlessProc

// --headless-selftest [pobDir]: builds a sandbox copy of the PoE1 install under
// <exeDir>PobTools\sandbox\ (heavy folders junctioned), boots the headless
// engine against it, loads a real build and checks the bridge's numbers against
// POB's own <PlayerStat> save output, runs POB's update check synchronously,
// and proves a broken script ends the child with a non-zero exit code. Report
// at <exeDir>headless_selftest.txt; returns the number of failed checks.
int RunHeadlessSelfTest(const std::wstring& exeDir, const std::wstring& pobDirOverride);
