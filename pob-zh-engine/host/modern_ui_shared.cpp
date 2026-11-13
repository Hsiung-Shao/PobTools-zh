#include "modern_ui_shared.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include "win_util.h"

using json = nlohmann::json;

json ModernUiPrefs(const std::wstring& exeDir, const std::wstring& game, const LauncherConfig& cfg)
{
	json fonts = json::array();
	for (const std::wstring& f : ListAvailableFonts(exeDir)) fonts.push_back(narrow(f));
	// The launcher's own choice, re-read: "follow the launcher" must show what
	// the launcher uses NOW, not what it used when this window opened.
	const LauncherConfig fromIni = LoadLauncherConfig(exeDir + L"pob-zh.ini");
	const std::wstring launcherFont = fromIni.fontFile;
	// Background: this window's game only. "launcherLook" is the launcher's
	// Appearance set, re-read for the same reason as the font.
	const int g = GameIndex(game);
	// The launcher's set never carries a video (it cannot play one) nor a scope.
	auto lookJson = [](const AppearanceConfig& a, bool follow, int scope, bool video) {
		return json{ {"follow", follow}, {"background", narrow(NormalizeBackgroundFile(a.background, video))},
		             {"bgBright", ClampPercent(a.bgBright, kBgBrightDefault)}, {"panelOpacity", ClampWindowOpacity(a.windowOpacity)},
		             {"glassBlur", ClampPercent(a.glassBlur, 0)}, {"treeBg", ClampPercent(a.treeBg, 100)},
		             {"bgScope", scope == 1 ? 1 : 0} };
	};
	json backgrounds = json::array();
	for (const std::wstring& f : ListAvailableBackgrounds(exeDir, true)) backgrounds.push_back(narrow(f));
	return json{ {"zoom", ClampModernZoom(cfg.modernZoom)}, {"fontSize", ClampModernFontSize(cfg.modernFontSize)},
	             {"theme", narrow(NormalizeModernTheme(cfg.modernTheme))}, {"accent", narrow(NormalizeModernAccent(cfg.modernAccent))},
	             {"font", narrow(NormalizeModernFont(cfg.modernFont))}, {"launcherFont", narrow(launcherFont)}, {"fonts", fonts},
	             {"look", lookJson(cfg.modernLook[g].look, cfg.modernLook[g].follow, cfg.modernLook[g].bgScope, true)},
	             {"launcherLook", lookJson(fromIni.look[g], true, 0, false)}, {"backgrounds", backgrounds} };
}

void ModernUiSetPrefs(const std::wstring& exeDir, const std::wstring& game, const json& params, LauncherConfig& cfg)
{
	// Like SaveWindowSize: re-read (the launcher may have written other keys), change ours, write.
	LauncherConfig fresh = LoadLauncherConfig(exeDir + L"pob-zh.ini");
	if (params.contains("zoom")) fresh.modernZoom = ClampModernZoom(params.value("zoom", kModernZoomDefault));
	if (params.contains("fontSize")) fresh.modernFontSize = ClampModernFontSize(params.value("fontSize", kModernFontSizeDefault));
	cfg.modernZoom = fresh.modernZoom;
	cfg.modernFontSize = fresh.modernFontSize;
	if (params.contains("theme") && params["theme"].is_string()) fresh.modernTheme = NormalizeModernTheme(widen(params["theme"].get<std::string>()));
	if (params.contains("accent") && params["accent"].is_string()) fresh.modernAccent = NormalizeModernAccent(widen(params["accent"].get<std::string>()));
	if (params.contains("font") && params["font"].is_string()) {
		// Only a file that is actually under Fonts\ -- anything else means "follow the launcher".
		std::wstring f = NormalizeModernFont(widen(params["font"].get<std::string>()));
		bool listed = false;
		for (const std::wstring& a : ListAvailableFonts(exeDir)) if (_wcsicmp(a.c_str(), f.c_str()) == 0) listed = true;
		fresh.modernFont = listed ? f : L"";
	}
	cfg.modernTheme = fresh.modernTheme;
	cfg.modernAccent = fresh.modernAccent;
	cfg.modernFont = fresh.modernFont;
	if (params.contains("look") && params["look"].is_object()) {
		// {follow:true} = back to the launcher's set; anything else is the
		// page's own set (the page sends all five values with it).
		const json& l = params["look"];
		ModernLookConfig& m = fresh.modernLook[GameIndex(game)];
		m.follow = l.value("follow", false);
		if (!m.follow) {
			if (l.contains("background") && l["background"].is_string()) {
				std::wstring f = NormalizeBackgroundFile(widen(l["background"].get<std::string>()), true);
				bool listed = f.empty();
				for (const std::wstring& a : ListAvailableBackgrounds(exeDir, true)) if (_wcsicmp(a.c_str(), f.c_str()) == 0) listed = true;
				m.look.background = listed ? f : L"";
			}
			auto num = [&](const char* k, int def) { return l.contains(k) && l[k].is_number() ? l[k].get<int>() : def; };
			m.look.bgBright = ClampPercent(num("bgBright", m.look.bgBright), kBgBrightDefault);
			m.look.windowOpacity = ClampWindowOpacity(num("panelOpacity", m.look.windowOpacity));
			m.look.glassBlur = ClampPercent(num("glassBlur", m.look.glassBlur), 0);
			m.look.treeBg = ClampPercent(num("treeBg", m.look.treeBg), 100);
			m.bgScope = num("bgScope", m.bgScope) == 1 ? 1 : 0;
		}
		cfg.modernLook[GameIndex(game)] = m;
	}
	SaveLauncherConfig(exeDir + L"pob-zh.ini", fresh);
}

bool ModernUiPathUnderPob(const std::wstring& pobDir, const std::wstring& path)
{
	// Resolve "..", "/" and trailing slashes first: a raw prefix compare lets "<pob>\..\.." and "<pob> Evil" through.
	auto full = [](const std::wstring& s) {
		wchar_t buf[1024];
		DWORD n = s.empty() ? 0 : GetFullPathNameW(s.c_str(), 1024, buf, nullptr);
		std::wstring r(buf, n < 1024 ? n : 0);
		while (!r.empty() && r.back() == L'\\') r.pop_back();
		return r;
	};
	const std::wstring root = full(pobDir), p = full(path);
	if (root.empty() || p.size() < root.size() || _wcsnicmp(p.c_str(), root.c_str(), root.size()) != 0) return false;
	return p.size() == root.size() || p[root.size()] == L'\\';
}

bool ModernUiOpenFolder(const std::wstring& exeDir, const std::wstring& pobDir, const json& params)
{
	if (params.value("which", "") == "backgrounds") {
		// The one folder outside the POB install the page may open: where the
		// launcher and the page both look for background images.
		ShellExecuteW(nullptr, L"explore", BackgroundsDir(exeDir).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
		return true;
	}
	// Only folders under the POB install, so "open a folder" never becomes "open anything".
	std::wstring p = widen(params.value("path", ""));
	for (auto& c : p) if (c == L'/') c = L'\\';
	if (!ModernUiPathUnderPob(pobDir, p)) return false;
	ShellExecuteW(nullptr, L"explore", p.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
	return true;
}

bool ModernUiRecordGate(const std::wstring& exeDir, const std::wstring& pobDir, const std::string& line, BridgeGate::Verdict& v)
{
	if (line.find("\"gate_result\"") == std::string::npos) return false;
	json msg;
	try { msg = json::parse(line); } catch (...) { return false; }
	if (!msg.is_object() || msg.value("event", "") != "gate_result") return false;
	const json& d = msg.contains("data") ? msg["data"] : json::object();
	v.ok = d.value("ok", false);
	if (d.contains("failed") && d["failed"].is_array())
		for (auto& f : d["failed"]) if (f.is_string()) v.failed.push_back(f.get<std::string>());
	v.pobVersion = d.value("pobVersion", "");
	v.pobBranch = d.value("pobBranch", "");
	v.pobDir = pobDir;
	v.bridgeHash = BridgeGate::BridgeFingerprint(exeDir);
	BridgeGate::Write(exeDir, v);
	return true;
}

void StopHeadlessChild(std::unique_ptr<HeadlessProc::Child>& child)
{
	if (child) {
		child->SetLineSink(nullptr);
		child->SetExitSink(nullptr);
		child->Stop(3000);
		child.reset();
	}
}
