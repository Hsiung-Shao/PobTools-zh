// `pob-zh.exe --mcp [--pob <dir>]`: a Model Context Protocol server on stdio,
// so AI clients (Claude Desktop, Claude Code, Codex...) can read and change a
// PoE1 build with Path of Building's own numbers.
//
// This process only speaks the protocol (JSON-RPC 2.0, one message per line).
// The work happens in a headless POB child against a sandbox copy of the
// install (<exeDir>PobTools\mcp_sandbox): Data\bridge\mcp_tools.lua answers
// `mcp_call`, and Data\bridge\mcp_tools.json lists the tools, so both ship
// with the data line. Nothing is written to the user's POB folder.
#pragma once

#include <string>

int RunMcpServer(const std::wstring& exeDir, const std::wstring& pobDirOverride);

// `pob-zh.exe --report <source> [--pob <dir>] [--out <file>]`: load_build +
// build_report in the same sandboxed engine, the Markdown written as UTF-8 to
// <file> (default <exeDir>PobTools\ai_report.md). 0 written, 1 the source did
// not load, 2 anything else; on failure no output file is left behind.
int RunReport(const std::wstring& exeDir, const std::wstring& source, const std::wstring& pobDirOverride,
              const std::wstring& outOverride);
