// `pob-zh.exe --mcp [--pob <dir>]`: a Model Context Protocol server on stdio,
// so AI clients (Claude Desktop, Claude Code, Codex...) can read and change a
// PoE1 build with Path of Building's own numbers.
//
// This process only speaks the protocol (JSON-RPC 2.0, one message per line).
// The work happens in a headless POB child against a sandbox copy of the
// install (<exeDir>PobTools\mcp_sandbox): Data\bridge\mcp_tools.lua answers
// `mcp_call`, and Data\bridge\mcp_tools.json lists the tools, so both ship
// with the data line. Nothing is written to the user's POB folder, except a new
// file in Builds\AI\ by the save_build tool.
#pragma once

#include <string>

int RunMcpServer(const std::wstring& exeDir, const std::wstring& pobDirOverride);

// `pob-zh.exe --report <source> [--pob <dir>] [--out <file>]`: load_build +
// build_report in the same sandboxed engine, the Markdown written as UTF-8 to
// <file> (default <exeDir>PobTools\ai_report.md). 0 written, 1 the source did
// not load, 2 anything else; on failure no output file is left behind.
int RunReport(const std::wstring& exeDir, const std::wstring& source, const std::wstring& pobDirOverride,
              const std::wstring& outOverride);

// `pob-zh.exe --agent-serve [--session <name>] [--pob <dir>]`: the same tools
// served to pobtools.exe (host/agent_cli.cpp) over the named pipe
// AgentPipeName(session), one connection at a time, keeping the engine and its
// build between connections. Exits after 30 idle minutes or on `agent/stop`;
// 2 when the session name is bad or another daemon already has it.
int RunAgentDaemon(const std::wstring& exeDir, const std::wstring& pobDirOverride, const std::wstring& session);

// [A-Za-z0-9_-]{1,32}: the name goes into a pipe name.
inline bool ValidAgentSession(const std::wstring& s)
{
	if (s.empty() || s.size() > 32) return false;
	for (wchar_t c : s)
		if (!((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') || c == L'_' || c == L'-')) return false;
	return true;
}
inline std::wstring AgentPipeName(const std::wstring& session) { return L"\\\\.\\pipe\\pobtools-agent-" + session; }

// Where the user's PoE1 POB keeps its builds: <POB>\Builds, or for an installed
// POB (installed.cfg) Documents\Path of Building\Builds (empty if Documents is
// unknown). The sandbox copies them from there; save_build writes back to AI\.
std::wstring PobBuildsDir(const std::wstring& pobDir);
