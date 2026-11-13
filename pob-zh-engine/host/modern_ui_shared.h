// What the two new-interface hosts (modern_ui_window: WebView2, modern_ui_browser: system browser) do identically.
#pragma once
#include "bridge_gate.h"
#include "headless_proc.h"
#include "launcher_config.h"

#include <json.hpp>

#include <memory>
#include <string>

// host.get_prefs: the page's own settings, plus the launcher's font and Appearance set re-read from the ini.
nlohmann::json ModernUiPrefs(const std::wstring& exeDir, const std::wstring& game, const LauncherConfig& cfg);
// host.set_prefs: re-read the ini, take the page's changes, write it back; `cfg` follows what was written.
void ModernUiSetPrefs(const std::wstring& exeDir, const std::wstring& game, const nlohmann::json& params, LauncherConfig& cfg);
// host.open_folder: the background-image folder, or a folder under the POB install. False = refused (outside it).
bool ModernUiOpenFolder(const std::wstring& exeDir, const std::wstring& pobDir, const nlohmann::json& params);
// A child line that is the bridge's `gate_result`: parsed into `v` and written to bridge_gate.json. False = not that line.
bool ModernUiRecordGate(const std::wstring& exeDir, const std::wstring& pobDir, const std::string& line, BridgeGate::Verdict& v);
void StopHeadlessChild(std::unique_ptr<HeadlessProc::Child>& child);
