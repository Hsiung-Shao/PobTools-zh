#include "editor_shell.h"
#include "editor_util.h"
#include "filter_card_ui.h"
#include "filter_batch.h"
#include "custom_rules_io.h"
#include "filter_parser.h"
#include "ui_theme.h"

#define IMGUI_DEFINE_MATH_OPERATORS
#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <string>
#include <vector>

// 過濾編輯 — the three-pane block editor (reference: 文字過濾編輯器):
//   left   block list (search + 顯示 checkbox + label, clipper-rendered)
//   middle selected block's condition / action cards
//   right  add-column (tick to insert, untick to disable)
//
// Index discipline: model-derived caches (rows/visRows/selection) are rebuilt
// whenever FilterDocumentEditor::structureVersion changes; the selected block
// survives rebuilds through BlockAnchor. Translation and summaries run only in
// RebuildRows — never inside the clipper loop.

namespace {

// Display label for a block: NeverSink marker from the header's trailing
// comment, else the last non-decorative comment right above the header, else an
// English condition summary.
std::string ExtractBlockLabel(const FilterFile& f, const FilterBlock& b)
{
	if (!b.headerComment.empty()) return b.headerComment;

	for (int li = b.headerLineIdx - 1; li >= 0; li--) {
		const FilterLine& ln = f.lines[li];
		if (ln.kind != FilterLineKind::Comment) break;
		std::string t = ln.raw.substr(ln.indent.size());
		size_t p = 0;
		while (p < t.size() && (t[p] == '#' || t[p] == ' ' || t[p] == '\t')) p++;
		t = t.substr(p);
		// Skip decorative rules like "=====" / "-----".
		bool decorative = t.empty();
		if (!t.empty() && (t[0] == '=' || t[0] == '-')) decorative = true;
		if (!decorative) return t;
	}
	return BlockSummary(f, b);
}

void RebuildVisRows(EditorShell& s)
{
	s.visRows.clear();
	s.visRows.reserve(s.rows.size());
	for (int i = 0; i < (int)s.rows.size(); i++) {
		if (!s.searchLower.empty() && !EdContainsCI(s.rows[i].haystack, s.searchLower)) continue;
		s.visRows.push_back(i);
	}
}

void RebuildRows(EditorShell& s)
{
	const FilterFile& f = s.model;
	s.rows.clear();
	s.rows.resize(f.blocks.size());
	for (int i = 0; i < (int)f.blocks.size(); i++) {
		const FilterBlock& b = f.blocks[i];
		BlockListRow& r = s.rows[i];
		std::string rawLabel = ExtractBlockLabel(f, b);
		r.label = NeverSinkHeaderZh(rawLabel);
		// haystack 同時收原文與譯文,搜尋 "$type->currency" 或 "通貨" 都命中。
		r.haystack = rawLabel + " " + r.label + " " + BlockSummary(f, b) + " " + CardBlockSummaryZh(f, b, s.i18n);
	}
	// Re-resolve the selection through its anchor (indices may have shifted).
	if (s.selAnchor.valid()) s.selectedBlock = s.doc.ResolveAnchor(s.selAnchor);
	if (s.selectedBlock < 0 || s.selectedBlock >= (int)f.blocks.size())
		s.selectedBlock = f.blocks.empty() ? -1 : 0;
	if (s.selectedBlock >= 0) s.selAnchor = s.doc.CaptureAnchor(s.selectedBlock);
	s.batchSel.assign(f.blocks.size(), 0);
	s.rowsVersion = s.doc.structureVersion();
	RebuildVisRows(s);
}

// Left pane: search box + clipper-rendered block list (+ batch multi-select).
// Returns true when the batch modal should open (popup must be opened at the
// section level, outside this child's ID scope).
bool DrawBlockList(EditorShell& s)
{
	bool openBatch = false;

	PobUi::PushPrimaryButton();
	bool addRule = ImGui::Button(u8"＋ 新增自訂規則", ImVec2(-1, 0));
	PobUi::PopButtonStyle();
	if (addRule) {
		CustomZone z = EnsureCustomZone(s.doc);
		if (z.present()) {
			int nb = s.doc.CreateBlockAtLine(z.endLine, false, u8"PobTools custom rule");
			if (nb >= 0) {
				// A bare Show with no condition would match EVERYTHING — seed a
				// BaseType the user is meant to replace.
				s.doc.InsertLine(nb, "BaseType", "",
					{ FilterToken{ "Divine Orb", true } });
				s.selectedBlock = nb;
				s.selAnchor = s.doc.CaptureAnchor(nb);
				s.status = u8"已在自訂區新增規則（請修改物品名稱）";
			}
		}
		return false;  // caches are stale; skip the list until next frame's rebuild
	}
	if (ImGui::IsItemHovered()) ImGui::SetTooltip(u8"新規則加在檔案最上方的自訂區，優先權最高");

	ImGui::SetNextItemWidth(-1);
	if (ImGui::InputTextWithHint("##search", u8"輸入文字自動搜尋過濾條目…", &s.search)) {
		s.searchLower = EdToLowerAscii(s.search);
		RebuildVisRows(s);
	}

	if (s.batchMode) {
		int nSel = 0;
		for (char c : s.batchSel) if (c) nSel++;
		if (ImGui::SmallButton(u8"全選可見")) {
			for (int bi : s.visRows) s.batchSel[bi] = 1;
		}
		ImGui::SameLine();
		if (ImGui::SmallButton(u8"清除")) s.batchSel.assign(s.batchSel.size(), 0);
		ImGui::SameLine();
		ImGui::BeginDisabled(nSel == 0);
		if (ImGui::SmallButton((u8"套用樣式（已選 " + std::to_string(nSel) + u8" 條）…").c_str()))
			openBatch = true;
		ImGui::EndDisabled();
	} else {
		ImGui::TextDisabled(u8"過濾項  %d / %d（單擊勾選切換顯示隱藏）",
			(int)s.visRows.size(), (int)s.rows.size());
	}
	ImGui::Separator();

	ImGui::BeginChild("##blocklist", ImVec2(0, 0), false);
	ImGuiListClipper clip;
	clip.Begin((int)s.visRows.size());
	while (clip.Step()) {
		for (int vi = clip.DisplayStart; vi < clip.DisplayEnd; vi++) {
			int bi = s.visRows[vi];
			FilterBlock& b = s.model.blocks[bi];
			const BlockListRow& row = s.rows[bi];
			ImGui::PushID(bi);

			if (s.batchMode) {
				bool bsel = s.batchSel[bi] != 0;
				if (ImGui::Checkbox("##bsel", &bsel)) s.batchSel[bi] = bsel ? 1 : 0;
				ImGui::SameLine();
			}

			bool show = !b.hide;
			if (ImGui::Checkbox("##show", &show)) SetBlockHide(s, b, !show);
			if (ImGui::IsItemHovered()) ImGui::SetTooltip(show ? u8"顯示中（點擊改為隱藏）" : u8"隱藏中（點擊改為顯示）");
			ImGui::SameLine();

			bool blockDirty = false;
			for (int li : b.lineIdx)
				if (s.model.lines[li].dirty) { blockDirty = true; break; }
			std::string label = row.label;
			if (blockDirty) label += u8"  *";

			bool sel = (s.selectedBlock == bi);
			if (b.hide) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.62f, 0.46f, 0.46f, 1.0f));
			if (ImGui::Selectable(label.c_str(), sel)) {
				if (s.batchMode) {
					s.batchSel[bi] = s.batchSel[bi] ? 0 : 1;
				} else {
					s.selectedBlock = bi;
					s.selAnchor = s.doc.CaptureAnchor(bi);
				}
			}
			if (b.hide) ImGui::PopStyleColor();
			ImGui::PopID();
		}
	}
	clip.End();
	ImGui::EndChild();
	return openBatch;
}

} // namespace

// Shared cache rebuild — 掉落預覽 needs rows (labels) without visiting this
// section first.
void EdRebuildRows(EditorShell& s) { RebuildRows(s); }

void DrawFilterEditSection(EditorShell& s)
{
	if (!s.loaded) {
		const char* prompt = u8"尚未開啟過濾器";
		ImVec2 avail = ImGui::GetContentRegionAvail();
		ImGui::Dummy(ImVec2(0, std::max(60.0f * s.scale, avail.y * 0.30f)));
		float textW = ImGui::CalcTextSize(prompt).x;
		ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (avail.x - textW) * 0.5f));
		ImGui::TextDisabled("%s", prompt);
		ImGui::Spacing();
		const float buttonW = 190.0f * s.scale;
		ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (avail.x - buttonW) * 0.5f));
		PobUi::PushPrimaryButton();
		bool open = ImGui::Button(u8"開啟 .filter 檔…", ImVec2(buttonW, 0));
		PobUi::PopButtonStyle();
		if (open) {
			std::wstring p = EdFilterDialog(s.initialDir, false);
			if (!p.empty()) s.OpenByPath(p, false);
		}
		return;
	}
	if (s.doc.file() != &s.model) s.doc.Attach(&s.model);
	if (s.rowsVersion != s.doc.structureVersion()) RebuildRows(s);

	const float rightW = 260 * s.scale;
	const float leftW = std::clamp(ImGui::GetContentRegionAvail().x * 0.30f,
		300 * s.scale, 420 * s.scale);

	ImGui::BeginChild("##left", ImVec2(leftW, 0), true);
	bool openBatch = DrawBlockList(s);
	ImGui::EndChild();
	if (openBatch) ImGui::OpenPopup(u8"批量修改###batchmodal");
	DrawBatchModal(s);

	// A structural change in the left pane (new custom rule, import) leaves
	// every cached index stale — skip the other panes until the next frame's
	// rebuild.
	if (s.rowsVersion != s.doc.structureVersion()) return;

	ImGui::SameLine();

	bool mutated = false;
	bool wantDeleteRule = false;
	ImGui::BeginChild("##mid", ImVec2(-rightW - ImGui::GetStyle().ItemSpacing.x, 0), true);
	if (s.selectedBlock < 0) {
		ImGui::TextDisabled(u8"在左側選擇一個過濾項以編輯。");
	} else {
		const BlockListRow& row = s.rows[s.selectedBlock];
		ImGui::TextColored(PobUi::Accent(), "%s", row.label.c_str());
		// 自訂區的規則是使用者自己加的:提供真刪除(其餘區塊維持 #! 停用哲學)。
		CustomZone z = FindCustomZone(s.model);
		const FilterBlock& blk = s.model.blocks[s.selectedBlock];
		if (z.present() && blk.headerLineIdx > z.beginLine && blk.headerLineIdx < z.endLine) {
			ImGui::SameLine(ImGui::GetContentRegionMax().x - 92 * s.scale);
			if (ImGui::SmallButton(u8"刪除規則")) wantDeleteRule = true;
			if (ImGui::IsItemHovered()) ImGui::SetTooltip(u8"從檔案中刪除這條自訂規則（無法復原）");
		}
		ImGui::Separator();
		ImGui::PushID(s.selectedBlock);
		mutated = DrawBlockCards(s, s.selectedBlock);
		ImGui::PopID();
	}
	ImGui::EndChild();

	ImGui::SameLine();

	ImGui::BeginChild("##right", ImVec2(rightW, 0), true);
	// After a structural mutation every cached index is stale: skip the
	// add-column this frame and let the next frame rebuild.
	if (!mutated) DrawAddColumn(s, s.selectedBlock);
	ImGui::EndChild();

	// Delete-custom-rule confirm modal — opened at section level (popup 不能在
	// PushID 迴圈/子視窗內開)。實際刪除發生在本 frame 尾端,下一 frame 重建快取。
	if (wantDeleteRule) ImGui::OpenPopup(u8"刪除自訂規則###delrule");
	if (ImGui::BeginPopupModal(u8"刪除自訂規則###delrule", nullptr,
	                           ImGuiWindowFlags_AlwaysAutoResize)) {
		const char* nm = (s.selectedBlock >= 0 && s.selectedBlock < (int)s.rows.size())
			? s.rows[s.selectedBlock].label.c_str() : "";
		ImGui::Text(u8"確定刪除自訂規則「%s」？", nm);
		ImGui::TextDisabled(u8"此動作直接從檔案移除該規則，無法復原。");
		ImGui::Spacing();
		PobUi::PushPrimaryButton();
		if (ImGui::Button(u8"刪除", ImVec2(110 * s.scale, 0))) {
			if (s.selectedBlock >= 0 && s.selectedBlock < (int)s.model.blocks.size()) {
				s.doc.RemoveBlock(s.selectedBlock);
				s.selectedBlock = -1;
				s.selAnchor = {};
				s.status = u8"已刪除自訂規則（記得儲存）";
			}
			ImGui::CloseCurrentPopup();
		}
		PobUi::PopButtonStyle();
		ImGui::SameLine();
		if (ImGui::Button(u8"取消", ImVec2(110 * s.scale, 0))) ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}
}
