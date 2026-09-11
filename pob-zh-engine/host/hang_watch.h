// Why a frozen program says nothing, and what this fixes.
//
// PobLog::Error only ever writes when the program NOTICES a failure. A hang is
// the case where nothing notices anything ever again: the frame loop stops, the
// window greys out, the user force-quits, and error-<date>.log is empty. A crash
// was nearly as bad -- the engine's SEH catch printed one bare address to a
// console window that dies with the process.
//
// So a second thread watches the first one and, when it stops moving, writes
// down what it was doing and where it stopped. That report is the answer to the
// only question worth asking about a freeze: WHY.
//
// Three rules this module lives by:
//
//   * A FALSE ALARM IS WORSE THAN NO ALARM. If PobTools\logs\ fills up with
//     "hang" reports every time somebody drags a window, nobody reads any of
//     them. So a stall is reported only when BOTH the heartbeat stopped for
//     `stallMs` AND the window stopped answering WM_NULL. Dragging, resizing,
//     menus and file dialogs all block the frame loop while the window keeps
//     answering sent messages, which is exactly the case gate two removes.
//
//   * THE WATCHDOG MUST NEVER BE THE THING THAT HANGS. While the watched thread
//     is suspended we do nothing but copy memory -- no loader lock, no dbghelp,
//     no allocation. The stack is walked afterwards, out of a snapshot. A
//     watchdog that deadlocks against the thread it suspended turns a hang the
//     user could force-quit into one they cannot.
//
//   * THE BREADCRUMB IS WORTH MORE THAN THE STACK. "lua:OnFrame", "dict-wait",
//     "load-module:Build" tells us where to look in one word, and unlike a stack
//     it survives having no PDB. Stack frames ship as module+offset; the build
//     that produced the release resolves them offline.
//
// Linked into BOTH pob-zh.exe and SimpleGraphic.dll, like error_log.cpp: POB
// runs in its own process, so the engine has to watch itself. The launcher
// additionally watches the POB processes it started (SetPeers), because a child
// wedged badly enough to lose even its watchdog thread still has to leave a
// trace somewhere.
#pragma once

#include <string>
#include <vector>

struct _EXCEPTION_POINTERS;

namespace HangWatch {

// Off only via POB_ZH_HANGWATCH=0 (the launcher passes the ini setting down).
// There is no UI switch: the cost is one thread and one atomic store per frame.
bool Enabled();

struct Options {
	const char* role = "app";      // "launcher" / "engine" / "tool:atlas"; goes in the file name
	const char* game = nullptr;    // "poe1" / "poe2", when the role has one
	unsigned stallMs = 20000;      // heartbeat silence before gate two is even consulted
	// Gate two. False means "no window to ask", which is only true for the
	// self-test and --hang-probe; a windowless process cannot tell a hang from
	// honest work and must never report on the heartbeat alone.
	bool requireWindow = true;
};

// Call once, from the thread that owns the frame loop. Also installs the
// unhandled-exception filter for this process. No-op when disabled.
void Start(const Options& opt);
void Stop();

// Once per iteration of the frame loop, from the same thread as Start().
void Beat();

// What that thread is doing right now. Calls from any other thread are ignored:
// a breadcrumb that mixes threads points at the wrong one.
void Stage(const char* tag);

// Scoped breadcrumb; restores the previous one. Use for anything that can block
// for seconds (module loads, dictionary waits, downloads).
class Scope {
public:
	explicit Scope(const char* tag);
	~Scope();
	Scope(const Scope&) = delete;
	Scope& operator=(const Scope&) = delete;
private:
	char prev_[96];
	bool active_;
};

// --- watching other processes (launcher only) --------------------------------
// The launcher owns the POB child processes but must not poll them from the UI
// thread: a SendMessageTimeout against a wedged window is exactly the kind of
// wait that would freeze the launcher too. So the UI hands over a snapshot each
// frame (RunningInstances is not thread-safe) and the watchdog thread does the
// asking.
struct Peer {
	unsigned long pid = 0;
	void* hwnd = nullptr;      // HWND
	std::string label;         // "POB (poe1)" -- what the user calls it
};
void SetPeers(const std::vector<Peer>& peers);

struct PeerStatus {
	bool hung = false;
	std::string label;
	unsigned seconds = 0;
};
PeerStatus GetPeerStatus();

// Self-test only. Stops the watchdog, forgets the per-run report cap and clears
// the breadcrumb, so each case starts from the same place. Nothing in the
// product calls these: the cap is per run on purpose.
void ResetForTest();
std::string CurrentStageForTest();
// True between Start() and Stop(): the watchdog thread object exists.
bool RunningForTest();

// Writes crash-<date>-<time>-<role>.txt and one line to the error log, and
// nothing else. The engine's existing SEH catch (sys_main.cpp) calls this: it
// has already caught the exception, so the report is all that is missing.
void ReportCrash(_EXCEPTION_POINTERS* ep);

// The unhandled-exception filter, installed by Start(). Reports, then hands the
// exception on exactly as it would have gone -- never swallows it, because a
// process that survives a crash it should not have survived corrupts data
// quietly instead of loudly.
long __stdcall CrashFilter(_EXCEPTION_POINTERS* ep);

} // namespace HangWatch

// --hang-selftest: exercises the reporter, both gates, the cap and the crash
// path against a scratch log directory. 0 = pass.
int RunHangWatchSelfTest(const std::wstring& exeDir);

// --hang-probe <seconds>: really stalls the main thread so a real report lands
// in the real log directory. For verifying on a live machine that the frames
// resolve against the build's PDB.
int RunHangProbe(const std::wstring& exeDir, int seconds);

// --hang-exit-probe: starts the watchdog and returns WITHOUT stopping it, the
// way a caller that forgot Stop() would. The self-test spawns this and asserts
// the process still exits 0 and promptly -- the v1.4.0 regression was a
// std::terminate at exit (0xC0000409) plus the Windows Error Reporting delay
// that came with it.
int RunHangExitProbe();
