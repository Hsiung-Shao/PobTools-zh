#include "mcp_config.h"

#include <json.hpp>

namespace McpConfig {

std::string ClaudeDesktopJson(const std::string& exe)
{
	nlohmann::json j = {
		{"mcpServers", {{"pobtools", {{"command", exe}, {"args", nlohmann::json::array({"--mcp"})}}}}},
	};
	return j.dump(2);
}

std::string ClaudeCodeCommand(const std::string& exe)
{
	// The path is quoted for the shell; a Windows path cannot contain '"'.
	return "claude mcp add pobtools -- \"" + exe + "\" --mcp";
}

std::string CodexToml(const std::string& exe)
{
	// TOML basic string: backslash and quote escaped.
	std::string q;
	for (char c : exe) {
		if (c == '\\' || c == '"') q += '\\';
		q += c;
	}
	return "[mcp_servers.pobtools]\n"
	       "command = \"" + q + "\"\n"
	       "args = [\"--mcp\"]\n"
	       "startup_timeout_sec = 60\n"
	       "tool_timeout_sec = 300\n";
}

} // namespace McpConfig
