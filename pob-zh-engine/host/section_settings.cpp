#include "editor_shell.h"
#include "editor_util.h"
#include "sound_manager.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>   // ShellExecuteW

#define IMGUI_DEFINE_MATH_OPERATORS
#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#pragma comment(lib, "shell32.lib")

// pob-zh.ini [PobTools] keys, shared with the launcher (Locale) plus editor-only
// keys (League / EconomyEnabled). Win32 profile API so we don't pull in a parser.

void LoadEditorSettings(EditorShell& s)
{
	std::wstring ini = s.exeDir + L"pob-zh.ini";
	wchar_t buf[128] = L"";
	GetPrivateProfileStringW(L"PobTools", L"League", L"Mirage", buf, 128, ini.c_str());
	s.league = EdNarrow(buf);
	if (s.league.empty()) s.league = "Mirage";
	s.economyEnabled = GetPrivateProfileIntW(L"PobTools", L"EconomyEnabled", 0, ini.c_str()) != 0;
}

void SaveEditorSettings(EditorShell& s)
{
	std::wstring ini = s.exeDir + L"pob-zh.ini";
	WritePrivateProfileStringW(L"PobTools", L"League", EdWiden(s.league).c_str(), ini.c_str());
	WritePrivateProfileStringW(L"PobTools", L"EconomyEnabled", s.economyEnabled ? L"1" : L"0", ini.c_str());
}

void DrawSettingsSection(EditorShell& s)
{
	ImGui::TextDisabled(u8"設定");
	ImGui::Spacing();

	// --- language / locale ---
	ImGui::SeparatorText(u8"語言");
	struct Loc { const char* code; const char* label; };
	static const Loc locs[] = {
		{ "zh-rTW", u8"繁體中文" }, { "en", u8"English" },
	};
	std::string cur = EdNarrow(s.locale);
	std::string curLabel = cur;
	for (const Loc& l : locs) if (cur == l.code) curLabel = l.label;
	ImGui::SetNextItemWidth(200 * s.scale);
	if (ImGui::BeginCombo(u8"介面語言", curLabel.c_str())) {
		for (const Loc& l : locs) {
			if (ImGui::Selectable(l.label, cur == l.code) && cur != l.code) {
				s.locale = EdWiden(l.code);
				s.i18n.Load(s.exeDir, l.code);          // reload Chinese names
				s.library.Load(s.exeDir, s.i18n);       // and the catalog
				std::wstring ini = s.exeDir + L"pob-zh.ini";
				WritePrivateProfileStringW(L"PobTools", L"Locale", s.locale.c_str(), ini.c_str());
				s.status = u8"已切換語言（同步給啟動器）";
			}
		}
		ImGui::EndCombo();
	}
	ImGui::TextDisabled(u8"此設定與啟動器共用；物品名稱顯示用，輸出的 .filter 仍為英文。");

	// --- economy ---
	ImGui::Spacing();
	ImGui::SeparatorText(u8"經濟估值");
	if (ImGui::Checkbox(u8"啟用經濟估值（顯示估值與建議分階）", &s.economyEnabled)) {
		s.economy.SetEnabled(s.economyEnabled);
		SaveEditorSettings(s);
	}
	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted(u8"聯盟："); ImGui::SameLine();
	ImGui::SetNextItemWidth(200 * s.scale);
	if (ImGui::InputText("##league", &s.league)) {} // commit on the buttons below
	ImGui::SameLine();
	if (ImGui::Button(u8"儲存聯盟")) { s.economy.SetLeague(s.league); SaveEditorSettings(s); }
	ImGui::SameLine();
	if (ImGui::SmallButton("Mirage")) { s.league = "Mirage"; s.economy.SetLeague(s.league); SaveEditorSettings(s); }
	ImGui::SameLine();
	if (ImGui::SmallButton("Standard")) { s.league = "Standard"; s.economy.SetLeague(s.league); SaveEditorSettings(s); }
	ImGui::TextDisabled(u8"線上價格在啟用後抓取；無網路或來源失效時自動略過，不影響其他功能。");

	// --- sound library ---
	ImGui::Spacing();
	ImGui::SeparatorText(u8"音效");
	if (ImGui::Button(u8"開啟音效庫（試聽／設定資料夾）")) ImGui::OpenPopup("##setsndlib");
	if (ImGui::BeginPopup("##setsndlib")) {
		std::string picked;
		DrawSoundLibrary(picked, s.scale);
		ImGui::EndPopup();
	}
	ImGui::TextDisabled(u8"設定自訂音效資料夾並試聽；在「客製規則／樣式」的自訂音效處可指派與播放。");

	// --- filter folder ---
	ImGui::Spacing();
	ImGui::SeparatorText(u8"過濾器資料夾");
	if (s.initialDir.empty()) {
		ImGui::TextDisabled(u8"未找到 Documents\\My Games\\Path of Exile\\。");
	} else {
		ImGui::TextWrapped("%s", EdNarrow(s.initialDir).c_str());
		if (ImGui::Button(u8"開啟資料夾"))
			ShellExecuteW(nullptr, L"open", s.initialDir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
	}
}
