// Headless engine <-> host transport: JSON Lines over the child's stdin/stdout.
//
// In headless mode (POB_ZH_HEADLESS=1) the engine runs POB's Lua with no window
// and no GL. Nothing on screen means nothing to look at, so this is the only way
// anything gets out: the host reads one JSON object per line from our stdout and
// writes requests one per line to our stdin.
//
//   request  {"id":1,"method":"get_stats","params":{...}}
//   response {"id":1,"result":...}  |  {"id":1,"error":{"code":"...","message":"..."}}
//   event    {"event":"hello","data":{...}}         (no id; engine -> host only)
//
// Why stdio rather than a named pipe: the child is spawned by the host, so the
// handles' lifetime is the process's lifetime with nothing to name, secure or
// clean up, and a PowerShell / Python harness can drive the child directly.
//
// A GUI-subsystem exe (which pob-zh.exe is) still gets usable std handles when
// the parent passes them with STARTF_USESTDHANDLES; when it was started from a
// shell with no redirection there is no stdin at all and Start() reports that.
#pragma once

#include <string>

namespace HeadlessIpc {

// Begins the stdin reader thread. false when this process has no usable stdin
// (not started by a host that redirected it) -- headless mode then has nobody to
// talk to and the caller should treat that as a configuration error.
bool Start();
void Stop();
bool Active();

// Next complete line from stdin, without the newline. false when none is queued.
// Never blocks: the frame loop asks once per iteration.
bool Pop(std::string& line);

// True once stdin reached EOF or broke: the host is gone. The frame loop exits on
// this, so a dead host never leaves an orphan engine spinning.
bool HostGone();

// One JSON document, written with a trailing newline and flushed. Thread-safe.
void Send(const std::string& json);

// Sugar over Send for the three shapes above. `dataJson` / `resultJson` are
// already-serialised JSON text.
void SendEvent(const char* event, const std::string& dataJson);
void SendResult(long long id, const std::string& resultJson);
void SendError(long long id, const char* code, const std::string& message);

// Serialise a string as a JSON string literal (quotes and escapes included).
std::string Quote(const std::string& s);

} // namespace HeadlessIpc
