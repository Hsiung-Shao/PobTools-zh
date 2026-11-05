// The snippets the launcher's "AI (MCP)" buttons copy: how to register
// `pob-zh.exe --mcp` with each AI client, with this exe's path filled in.
#pragma once

#include <string>

namespace McpConfig {

// exePathUtf8: the full path of pob-zh.exe (UTF-8).
std::string ClaudeDesktopJson(const std::string& exePathUtf8);  // claude_desktop_config.json
std::string ClaudeCodeCommand(const std::string& exePathUtf8);  // `claude mcp add ...` (Claude Code / CLI)
std::string CodexToml(const std::string& exePathUtf8);          // ~/.codex/config.toml

} // namespace McpConfig
