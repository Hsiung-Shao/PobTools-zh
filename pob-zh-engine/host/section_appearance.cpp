#include "editor_shell.h"
#include "filter_parser.h"   // FilterGetColor / FilterSetColor

#define IMGUI_DEFINE_MATH_OPERATORS
#include <imgui.h>

#include <cstdio>

// 外觀: batch-recolour a whole category, plus per-tier access to the shared style
// editor. Only ever calls FilterSetColor / DrawBlockStyleEditor, so output stays
// English and edits flow through the existing dirty-tracking.

void DrawAppearanceSection(EditorShell& s)
{
	if (!s.loaded) { ImGui::TextDisabled(u8"開啟一個 .filter 後即可在此調整外觀。"); return; }
	if (s.tierIndex.categories.empty()) { ImGui::TextDisabled(u8"此過濾器沒有可分類的規則。"); return; }
	if (s.appCategory < 0 || s.appCategory >= (int)s.tierIndex.categories.size()) s.appCategory = 0;

	ImGui::TextDisabled(u8"外觀 — 批次調整顏色，或逐階編輯樣式");
	ImGui::Spacing();

	ImGui::SetNextItemWidth(220 * s.scale);
	if (ImGui::BeginCombo(u8"分類", s.tierIndex.categories[s.appCategory].display.c_str())) {
		for (int c = 0; c < (int)s.tierIndex.categories.size(); c++)
			if (ImGui::Selectable(s.tierIndex.categories[c].display.c_str(), c == s.appCategory)) s.appCategory = c;
		ImGui::EndCombo();
	}
	const TierCategory& cat = s.tierIndex.categories[s.appCategory];

	// --- batch text colour ---
	ImGui::SeparatorText(u8"批次文字顏色");
	static float col[3] = { 0.95f, 0.95f, 0.95f };
	ImGui::SetNextItemWidth(220 * s.scale);
	ImGui::ColorEdit3("##batchcol", col, ImGuiColorEditFlags_Uint8);
	ImGui::SameLine();
	if (ImGui::Button(u8"套用到此分類")) {
		int n = 0;
		for (int ti : cat.tierEntryIdx) {
			FilterBlock& b = s.model.blocks[s.tierIndex.tiers[ti].blockIndex];
			if (b.idxTextColor < 0) continue;
			FilterLine& ln = s.model.lines[b.idxTextColor];
			int r, g, bl, a; bool ha; FilterGetColor(ln, r, g, bl, a, ha);
			FilterSetColor(ln, (int)(col[0] * 255 + .5f), (int)(col[1] * 255 + .5f), (int)(col[2] * 255 + .5f), a, ha);
			n++;
		}
		if (n) { s.model.dirty = true; s.tlui.needsRebuild = true; }
		char msg[64]; snprintf(msg, sizeof(msg), u8"已套用文字顏色到 %d 條規則", n);
		s.status = msg;
	}
	ImGui::TextDisabled(u8"僅套用到已有 SetTextColor 動作的規則。");

	// --- per-tier style ---
	ImGui::SeparatorText(u8"逐階樣式");
	ImGui::BeginChild("##apptiers");
	for (int ti : cat.tierEntryIdx) {
		TierEntry& te = s.tierIndex.tiers[ti];
		FilterBlock& b = s.model.blocks[te.blockIndex];
		ImGui::PushID(te.blockIndex);

		if (b.idxTextColor >= 0) {
			int r, g, bl, a; bool ha; FilterGetColor(s.model.lines[b.idxTextColor], r, g, bl, a, ha);
			ImGui::ColorButton("##sw", ImVec4(r / 255.f, g / 255.f, bl / 255.f, 1.f),
				ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoPicker | ImGuiColorEditFlags_NoDragDrop,
				ImVec2(14 * s.scale, 14 * s.scale));
		} else {
			ImGui::Dummy(ImVec2(14 * s.scale, 14 * s.scale));
		}
		ImGui::SameLine();

		std::string label = (te.draggable() && !te.items.empty()) ? s.i18n.DisplayName(te.items[0]) : te.tierLabel;
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted((b.hide ? (u8"[隱藏] " + label) : label).c_str());
		ImGui::SameLine(ImGui::GetContentRegionAvail().x - 70 * s.scale);
		if (ImGui::SmallButton(u8"樣式…")) ImGui::OpenPopup("##sp");
		if (ImGui::BeginPopup("##sp")) {
			if (DrawBlockStyleEditor(s.model, b, s.scale)) s.tlui.needsRebuild = true;
			ImGui::EndPopup();
		}
		ImGui::PopID();
	}
	ImGui::EndChild();
}
