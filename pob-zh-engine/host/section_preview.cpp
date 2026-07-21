#include "editor_shell.h"
#include "filter_parser.h"   // FilterGetColor

#define IMGUI_DEFINE_MATH_OPERATORS
#include <imgui.h>

#include <string>
#include <vector>
#include <set>
#include <cstdlib>   // rand

// 預覽: a "simulate loot" view, like FilterBlade's SIMULATE tab — press a button to
// drop a sample of items, rendered as in-game-style coloured labels on a neutral
// dark canvas (no GGG art). Click a label to edit that rule's style/sound in place.

namespace {

struct Drop { int block = -1; std::string item; float jitter = 0; };
std::vector<Drop> g_drops;   // single editor window, so file-local state is fine
int g_selBlock = -1;

ImU32 ColorOr(FilterFile& m, int lineIdx, ImU32 fallback, bool useAlpha)
{
	if (lineIdx < 0) return fallback;
	int r, g, b, a; bool ha; FilterGetColor(m.lines[lineIdx], r, g, b, a, ha);
	return IM_COL32(r, g, b, useAlpha ? a : 255);
}

bool ValuableItem(EditorShell& s, const std::string& cat, const std::string& item)
{
	static const std::set<std::string> kCats = { u8"通貨", u8"傳奇", u8"命運卡", u8"碎片", u8"地圖" };
	if (s.economy.available()) {
		PriceInfo p = s.economy.PriceOf(item);
		if (p.known && p.chaos >= 5.0) return true;
	}
	return kCats.count(cat) != 0;
}

void Generate(EditorShell& s, bool valuable)
{
	g_drops.clear();
	g_selBlock = -1;

	std::vector<std::pair<int, const std::string*>> cand;
	for (const TierCategory& cat : s.tierIndex.categories) {
		for (int ti : cat.tierEntryIdx) {
			const TierEntry& te = s.tierIndex.tiers[ti];
			const FilterBlock& b = s.model.blocks[te.blockIndex];
			if (b.hide || !te.draggable() || te.items.empty()) continue;
			for (const std::string& it : te.items) {
				if (valuable && !ValuableItem(s, cat.display, it)) continue;
				cand.push_back({ te.blockIndex, &it });
			}
		}
	}
	if (cand.empty()) return;

	const int N = 14;
	for (int i = 0; i < N; i++) {
		int k = rand() % (int)cand.size();
		Drop d;
		d.block = cand[k].first;
		d.item = *cand[k].second;
		d.jitter = (float)((rand() % 160) - 80);  // -80..+80 px horizontal scatter
		g_drops.push_back(std::move(d));
	}
}

} // namespace

void DrawPreviewSection(EditorShell& s)
{
	if (!s.loaded) { ImGui::TextDisabled(u8"開啟一個 .filter 後即可預覽。"); return; }
	if (s.tierIndex.categories.empty()) { ImGui::TextDisabled(u8"此過濾器沒有可分類的規則。"); return; }

	ImGui::TextDisabled(u8"模擬掉落 — 產生一批掉落，點標籤可就地編輯該規則（僅模擬 Show 規則）");
	if (ImGui::Button(u8"產生掉落")) Generate(s, false);
	ImGui::SameLine();
	if (ImGui::Button(u8"產生高價值掉落")) Generate(s, true);
	ImGui::SameLine();
	if (ImGui::Button(u8"清除")) { g_drops.clear(); g_selBlock = -1; }
	ImGui::Separator();

	const bool hasSel = (g_selBlock >= 0 && g_selBlock < (int)s.model.blocks.size());
	float avail = ImGui::GetContentRegionAvail().y;
	float canvasH = hasSel ? avail * 0.55f : avail;

	// ---- loot canvas ----
	ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.04f, 0.05f, 0.06f, 1.0f));
	ImGui::BeginChild("##canvas", ImVec2(0, canvasH), true);
	ImDrawList* dl = ImGui::GetWindowDrawList();
	if (g_drops.empty()) {
		ImGui::TextDisabled(u8"按「產生掉落」開始。");
	} else {
		const float cx = ImGui::GetContentRegionAvail().x * 0.5f;
		for (int i = 0; i < (int)g_drops.size(); i++) {
			Drop& d = g_drops[i];
			if (d.block < 0 || d.block >= (int)s.model.blocks.size()) continue;
			FilterBlock& b = s.model.blocks[d.block];
			std::string zh = s.i18n.DisplayName(d.item);

			ImU32 bg = ColorOr(s.model, b.idxBgColor,   IM_COL32(0, 0, 0, 200), true);
			ImU32 tc = ColorOr(s.model, b.idxTextColor, IM_COL32(200, 200, 200, 255), false);
			ImU32 bd = (b.idxBorderColor >= 0) ? ColorOr(s.model, b.idxBorderColor, 0, false) : 0;

			ImVec2 tsz = ImGui::CalcTextSize(zh.c_str());
			const float padX = 14 * s.scale, padY = 6 * s.scale;
			const float w = tsz.x + 2 * padX, h = tsz.y + 2 * padY;

			float lx = cx + d.jitter * s.scale - w * 0.5f;
			if (lx < 0) lx = 0;
			ImGui::SetCursorPosX(lx);

			ImGui::PushID(i);
			bool clicked = ImGui::InvisibleButton("##d", ImVec2(w, h));
			bool hov = ImGui::IsItemHovered();
			ImVec2 pmin = ImGui::GetItemRectMin(), pmax = ImGui::GetItemRectMax();
			dl->AddRectFilled(pmin, pmax, bg, 4 * s.scale);
			if (bd) dl->AddRect(pmin, pmax, bd, 4 * s.scale, 0, 2 * s.scale);
			if (hov) dl->AddRect(pmin, pmax, IM_COL32(255, 255, 255, 120), 4 * s.scale, 0, 1.5f * s.scale);
			dl->AddText(ImVec2(pmin.x + padX, pmin.y + padY), tc, zh.c_str());
			if (clicked) g_selBlock = d.block;
			if (hov) ImGui::SetTooltip("%s", d.item.c_str());
			ImGui::PopID();
			ImGui::Dummy(ImVec2(0, 4 * s.scale));
		}
	}
	ImGui::EndChild();
	ImGui::PopStyleColor();

	// ---- inline editor for the clicked label ----
	if (hasSel) {
		ImGui::Separator();
		FilterBlock& b = s.model.blocks[g_selBlock];
		ImGui::TextDisabled(u8"編輯規則樣式 / 音效（即時反映於上方）");
		ImGui::BeginChild("##pvedit", ImVec2(0, 0), true);
		ImGui::PushID(g_selBlock);
		DrawBlockStyleEditor(s.model, b, s.scale);
		ImGui::PopID();
		ImGui::EndChild();
	} else if (!g_drops.empty()) {
		ImGui::TextDisabled(u8"點上方任一物品來編輯它的規則。");
	}
}
