#include "editor_shell.h"

#define IMGUI_DEFINE_MATH_OPERATORS
#include <imgui.h>

#include <cstdio>

// 快速篩選: coarse per-category Show/Hide. Only flips rule verbs (SetBlockHide),
// so the .filter content is otherwise untouched and output stays English.

static void SetCategoryHide(EditorShell& s, const TierCategory& cat, bool hide)
{
	for (int ti : cat.tierEntryIdx)
		SetBlockHide(s, s.model.blocks[s.tierIndex.tiers[ti].blockIndex], hide);
	s.tlui.needsRebuild = true;
}

void DrawQuickFilterSection(EditorShell& s)
{
	if (!s.loaded) { ImGui::TextDisabled(u8"開啟一個 .filter 後即可在此快速篩選。"); return; }
	if (s.tierIndex.categories.empty()) { ImGui::TextDisabled(u8"此過濾器沒有可分類的規則。"); return; }

	ImGui::TextDisabled(u8"快速篩選 — 依分類批次顯示／隱藏");
	ImGui::Spacing();

	if (ImGui::Button(u8"全部顯示")) {
		for (FilterBlock& b : s.model.blocks) SetBlockHide(s, b, false);
		s.tlui.needsRebuild = true; s.status = u8"已全部顯示";
	}
	ImGui::SameLine();
	if (ImGui::Button(u8"全部隱藏")) {
		for (FilterBlock& b : s.model.blocks) SetBlockHide(s, b, true);
		s.tlui.needsRebuild = true; s.status = u8"已全部隱藏";
	}
	ImGui::Separator();

	ImGui::BeginChild("##qfcats");
	for (const TierCategory& cat : s.tierIndex.categories) {
		int nShow = 0, nHide = 0;
		for (int ti : cat.tierEntryIdx)
			(s.model.blocks[s.tierIndex.tiers[ti].blockIndex].hide ? nHide : nShow)++;

		ImGui::PushID(&cat);
		ImGui::AlignTextToFramePadding();
		char label[96];
		snprintf(label, sizeof(label), u8"%s  ·  顯示 %d / 隱藏 %d", cat.display.c_str(), nShow, nHide);
		ImGui::TextUnformatted(label);
		ImGui::SameLine(ImGui::GetContentRegionAvail().x - 130 * s.scale);
		if (ImGui::SmallButton(u8"顯示")) SetCategoryHide(s, cat, false);
		ImGui::SameLine();
		if (ImGui::SmallButton(u8"隱藏")) SetCategoryHide(s, cat, true);
		ImGui::PopID();
	}
	ImGui::EndChild();
}
