#include "editor_shell.h"

#define IMGUI_DEFINE_MATH_OPERATORS
#include <imgui.h>

#include <string>
#include <set>

// 預設: load the bundled starter template, or one-click filter-wide visibility
// templates. Visibility presets only flip Show/Hide verbs (SetBlockHide) — output
// stays English and any preset is reversible with another.

static void ShowAll(EditorShell& s, bool hide)
{
	for (FilterBlock& b : s.model.blocks) SetBlockHide(s, b, hide);
	s.tlui.needsRebuild = true;
}

// Hide everything, then re-show the categories whose Chinese display is in `keep`.
static void StrictKeep(EditorShell& s, const char* const* keep, int keepN)
{
	for (FilterBlock& b : s.model.blocks) SetBlockHide(s, b, true);
	for (const TierCategory& cat : s.tierIndex.categories) {
		bool wanted = false;
		for (int i = 0; i < keepN; i++) if (cat.display == keep[i]) { wanted = true; break; }
		if (!wanted) continue;
		for (int ti : cat.tierEntryIdx)
			SetBlockHide(s, s.model.blocks[s.tierIndex.tiers[ti].blockIndex], false);
	}
	s.tlui.needsRebuild = true;
}

// Balanced: show the high-value categories; for gear, keep only the top base tiers.
static void Balanced(EditorShell& s)
{
	static const std::set<std::string> showCats = {
		u8"通貨", u8"碎片", u8"命運卡", u8"地圖", u8"傳奇", u8"寶石", u8"珠寶"
	};
	for (const TierCategory& cat : s.tierIndex.categories) {
		if (showCats.count(cat.display)) {
			for (int ti : cat.tierEntryIdx)
				SetBlockHide(s, s.model.blocks[s.tierIndex.tiers[ti].blockIndex], false);
		} else if (cat.display == std::string(u8"裝備武器")) {
			int seen = 0;
			for (int ti : cat.tierEntryIdx) {
				TierEntry& te = s.tierIndex.tiers[ti];
				if (!te.draggable()) continue;
				SetBlockHide(s, s.model.blocks[te.blockIndex], seen >= 2); // keep top 2 tiers shown
				seen++;
			}
		}
	}
	s.tlui.needsRebuild = true;
}

void DrawPresetsSection(EditorShell& s)
{
	ImGui::TextDisabled(u8"預設 — 載入內建起手範本，或一鍵套用可見性範本");
	ImGui::Spacing();

	// Load the bundled starter template (works even with no file open).
	if (ImGui::Button(u8"載入內建預設範本", ImVec2(260 * s.scale, 0))) {
		s.OpenByPath(s.exeDir + L"Filters\\default.filter", false);
		if (s.loaded) {
			s.model.path.clear();
			s.model.name = u8"預設範本（請用「另存為」存到遊戲資料夾）";
			s.status = u8"已載入內建範本，請於上方「另存為」存檔";
		}
	}
	ImGui::TextDisabled(u8"無自有 .filter 時的起手範本（NeverSink POE1）。編好後用工具列「另存為」存到 Documents\\My Games\\Path of Exile\\。");
	ImGui::Separator();

	if (!s.loaded) { ImGui::TextDisabled(u8"載入或開啟一個 .filter 後即可套用下列可見性預設。"); return; }
	if (s.tierIndex.categories.empty()) { ImGui::TextDisabled(u8"此過濾器沒有可分類的規則。"); return; }

	if (ImGui::Button(u8"推薦（平衡）", ImVec2(260 * s.scale, 0))) { Balanced(s); s.status = u8"已套用推薦（平衡）"; }
	ImGui::TextDisabled(u8"顯示通貨/碎片/命運卡/地圖/傳奇/寶石/珠寶；裝備武器只留高階基底。");
	ImGui::Spacing();

	if (ImGui::Button(u8"全部顯示（重置可見性）", ImVec2(260 * s.scale, 0))) { ShowAll(s, false); s.status = u8"已重置為全部顯示"; }
	ImGui::TextDisabled(u8"把所有規則設為 Show。");
	ImGui::Spacing();

	if (ImGui::Button(u8"全部隱藏", ImVec2(260 * s.scale, 0))) { ShowAll(s, true); s.status = u8"已全部隱藏"; }
	ImGui::TextDisabled(u8"把所有規則設為 Hide（之後再逐項開啟）。");
	ImGui::Spacing();

	if (ImGui::Button(u8"嚴格：只顯示高價值分類", ImVec2(260 * s.scale, 0))) {
		static const char* keep[] = { u8"通貨", u8"傳奇", u8"命運卡", u8"碎片" };
		StrictKeep(s, keep, 4);
		s.status = u8"已套用嚴格範本（通貨／傳奇／命運卡／碎片）";
	}
	ImGui::TextDisabled(u8"隱藏其餘分類，只保留通貨、傳奇、命運卡、碎片。");
}
