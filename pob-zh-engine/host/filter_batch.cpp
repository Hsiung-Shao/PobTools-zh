#include "filter_batch.h"
#include "filter_card_ui.h"
#include "filter_parser.h"
#include "filter_schema.h"
#include "sound_manager.h"

#define IMGUI_DEFINE_MATH_OPERATORS
#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <string>
#include <vector>

namespace {

using Tri = BatchStyleOp::Tri;

// Set (insert-or-update) one action line; returns lines touched (0 or 1).
// makeLine builds the full default line text when the block lacks the action.
int SetAction(EditorShell& s, int blockIdx, const char* keyword, const char* alias,
              const std::string& newLine)
{
	int li = s.doc.FindLine(blockIdx, keyword);
	if (li < 0 && alias) li = s.doc.FindLine(blockIdx, alias);
	FilterLine dl = ParseFilterLine(newLine);
	if (li >= 0) {
		FilterLine& ln = s.model.lines[li];
		ln.op = dl.op;
		ln.values = dl.values;
		// keyword stays as-is (keeps a Positional/Optional variant), except when
		// the sound family switches between built-in and custom (handled by caller
		// disabling the other family first).
		ln.dirty = true;
	} else {
		s.doc.InsertLine(blockIdx, dl.keyword, dl.op, dl.values);
	}
	s.model.dirty = true;
	return 1;
}

// Disable every live line of the keyword (and alias); returns lines touched.
int RemoveAction(EditorShell& s, int blockIdx, const char* keyword, const char* alias)
{
	int n = 0;
	for (;;) {
		int li = s.doc.FindLine(blockIdx, keyword);
		if (li < 0 && alias) li = s.doc.FindLine(blockIdx, alias);
		if (li < 0) break;
		s.doc.CommentOutLine(li);
		n++;
	}
	return n;
}

std::string ColorLine(const char* kw, const int c[4])
{
	std::string t = kw;
	for (int i = 0; i < 4; i++) t += " " + std::to_string(c[i]);
	return t;
}

// Tri-state radio row; returns the (possibly updated) state.
Tri TriRadio(const char* id, Tri v, bool removable = true)
{
	ImGui::PushID(id);
	int iv = (int)v;
	ImGui::RadioButton(u8"維持", &iv, (int)Tri::Keep);
	ImGui::SameLine();
	ImGui::RadioButton(u8"設為", &iv, (int)Tri::Set);
	if (removable) {
		ImGui::SameLine();
		ImGui::RadioButton(u8"停用", &iv, (int)Tri::Remove);
	}
	ImGui::PopID();
	return (Tri)iv;
}

void ColorField(const char* label, Tri& tri, int c[4], bool& any)
{
	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted(label);
	ImGui::SameLine(120);
	tri = TriRadio(label, tri);
	if (tri == Tri::Set) {
		ImGui::SameLine();
		float col[4] = { c[0] / 255.f, c[1] / 255.f, c[2] / 255.f, c[3] / 255.f };
		std::string id = std::string("##col") + label;
		if (ImGui::ColorEdit4(id.c_str(), col,
				ImGuiColorEditFlags_Uint8 | ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar)) {
			for (int i = 0; i < 4; i++) c[i] = (int)(col[i] * 255.f + 0.5f);
		}
	}
	if (tri != Tri::Keep) any = true;
	ImGui::Separator();
}

} // namespace

int ApplyBatchStyle(EditorShell& s, const std::vector<int>& blocks, const BatchStyleOp& op)
{
	int touched = 0;
	for (int bi : blocks) {
		if (bi < 0 || bi >= (int)s.model.blocks.size()) continue;

		if (op.showHide == Tri::Set) {
			FilterBlock& b = s.model.blocks[bi];
			if (b.hide != op.hide) { SetBlockHide(s, b, op.hide); touched++; }
		}

		auto colorField = [&](Tri tri, const char* kw, const int c[4]) {
			if (tri == Tri::Set) touched += SetAction(s, bi, kw, nullptr, ColorLine(kw, c));
			else if (tri == Tri::Remove) touched += RemoveAction(s, bi, kw, nullptr);
		};
		colorField(op.textColor, "SetTextColor", op.text);
		colorField(op.borderColor, "SetBorderColor", op.border);
		colorField(op.bgColor, "SetBackgroundColor", op.bg);

		if (op.fontSize == Tri::Set)
			touched += SetAction(s, bi, "SetFontSize", nullptr, "SetFontSize " + std::to_string(op.size));
		else if (op.fontSize == Tri::Remove)
			touched += RemoveAction(s, bi, "SetFontSize", nullptr);

		if (op.sound == Tri::Set) {
			if (op.custom) {
				touched += RemoveAction(s, bi, "PlayAlertSound", "PlayAlertSoundPositional");
				touched += SetAction(s, bi, "CustomAlertSound", "CustomAlertSoundOptional",
					"CustomAlertSound \"" + op.customPath + "\" " + std::to_string(op.volume));
			} else {
				touched += RemoveAction(s, bi, "CustomAlertSound", "CustomAlertSoundOptional");
				touched += SetAction(s, bi, "PlayAlertSound", "PlayAlertSoundPositional",
					"PlayAlertSound " + std::to_string(op.soundId) + " " + std::to_string(op.volume));
			}
		} else if (op.sound == Tri::Remove) {
			touched += RemoveAction(s, bi, "PlayAlertSound", "PlayAlertSoundPositional");
			touched += RemoveAction(s, bi, "CustomAlertSound", "CustomAlertSoundOptional");
		}

		if (op.minimapIcon == Tri::Set)
			touched += SetAction(s, bi, "MinimapIcon", nullptr,
				"MinimapIcon " + std::to_string(op.mmSize) + " " + op.mmColor + " " + op.mmShape);
		else if (op.minimapIcon == Tri::Remove)
			touched += RemoveAction(s, bi, "MinimapIcon", nullptr);

		if (op.playEffect == Tri::Set)
			touched += SetAction(s, bi, "PlayEffect", nullptr,
				"PlayEffect " + op.fxColor + (op.fxTemp ? " Temp" : ""));
		else if (op.playEffect == Tri::Remove)
			touched += RemoveAction(s, bi, "PlayEffect", nullptr);
	}
	return touched;
}

void DrawBatchModal(EditorShell& s)
{
	static BatchStyleOp op;

	ImGui::SetNextWindowSize(ImVec2(560 * s.scale, 0));
	if (!ImGui::BeginPopupModal(u8"批量修改###batchmodal", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		return;

	int nSel = 0;
	for (char c : s.batchSel) if (c) nSel++;
	ImGui::TextDisabled(u8"將套用到已勾選的 %d 條過濾項；每欄可選「維持 / 設為 / 停用」。", nSel);
	ImGui::Separator();

	bool any = false;

	// Show/Hide
	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted(u8"顯示狀態");
	ImGui::SameLine(120);
	op.showHide = TriRadio("showhide", op.showHide, false);
	if (op.showHide == Tri::Set) {
		ImGui::SameLine();
		int v = op.hide ? 1 : 0;
		const char* items[2] = { u8"顯示", u8"隱藏" };
		ImGui::SetNextItemWidth(100 * s.scale);
		if (ImGui::Combo("##sh", &v, items, 2)) op.hide = (v == 1);
		any = true;
	}
	ImGui::Separator();

	ColorField(u8"文字顏色", op.textColor, op.text, any);
	ColorField(u8"邊框顏色", op.borderColor, op.border, any);
	ColorField(u8"背景顏色", op.bgColor, op.bg, any);

	// Font size
	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted(u8"字體大小");
	ImGui::SameLine(120);
	op.fontSize = TriRadio("fontsize", op.fontSize);
	if (op.fontSize == Tri::Set) {
		ImGui::SameLine();
		ImGui::SetNextItemWidth(160 * s.scale);
		ImGui::SliderInt("##fs", &op.size, 1, 45);
	}
	if (op.fontSize != Tri::Keep) any = true;
	ImGui::Separator();

	// Sound
	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted(u8"音效");
	ImGui::SameLine(120);
	op.sound = TriRadio("sound", op.sound);
	if (op.sound == Tri::Set) {
		ImGui::SameLine();
		int v = op.custom ? 1 : 0;
		const char* items[2] = { u8"內建", u8"自訂" };
		ImGui::SetNextItemWidth(90 * s.scale);
		if (ImGui::Combo("##sndkind", &v, items, 2)) op.custom = (v == 1);
		ImGui::Indent(120);
		if (op.custom) {
			ImGui::SetNextItemWidth(240 * s.scale);
			ImGui::InputText(u8"檔名", &op.customPath);
			ImGui::SameLine();
			if (ImGui::Button(u8"音效庫…")) ImGui::OpenPopup("##batchsoundlib");
			if (ImGui::BeginPopup("##batchsoundlib")) {
				std::string picked;
				if (DrawSoundLibrary(picked, s.scale)) {
					op.customPath = picked;
					ImGui::CloseCurrentPopup();
				}
				ImGui::EndPopup();
			}
		} else {
			ImGui::SetNextItemWidth(140 * s.scale);
			ImGui::SliderInt(u8"編號", &op.soundId, 1, 16);
			ImGui::SameLine();
		}
		ImGui::SetNextItemWidth(140 * s.scale);
		ImGui::SliderInt(u8"音量", &op.volume, 0, 300);
		ImGui::Unindent(120);
	}
	if (op.sound != Tri::Keep) any = true;
	ImGui::Separator();

	// Minimap icon
	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted(u8"物品圖標");
	ImGui::SameLine(120);
	op.minimapIcon = TriRadio("mmicon", op.minimapIcon);
	if (op.minimapIcon == Tri::Set) {
		ImGui::SameLine();
		static const char* kSizeZh[3] = { u8"大", u8"中", u8"小" };
		ImGui::SetNextItemWidth(70 * s.scale);
		ImGui::Combo("##mmsz", &op.mmSize, kSizeZh, 3);
		ImGui::SameLine();
		std::string nc = CardEffectColorCombo("##mmc", op.mmColor, s.scale);
		if (!nc.empty()) op.mmColor = nc;
		ImGui::SameLine();
		std::string ns = CardIconShapeCombo("##mms", op.mmShape, s.scale);
		if (!ns.empty()) op.mmShape = ns;
	}
	if (op.minimapIcon != Tri::Keep) any = true;
	ImGui::Separator();

	// Play effect
	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted(u8"物品光柱");
	ImGui::SameLine(120);
	op.playEffect = TriRadio("fx", op.playEffect);
	if (op.playEffect == Tri::Set) {
		ImGui::SameLine();
		std::string nc = CardEffectColorCombo("##fxc", op.fxColor, s.scale);
		if (!nc.empty()) op.fxColor = nc;
		ImGui::SameLine();
		ImGui::Checkbox(u8"僅掉落瞬間", &op.fxTemp);
	}
	if (op.playEffect != Tri::Keep) any = true;
	ImGui::Separator();

	ImGui::BeginDisabled(!any || nSel == 0);
	if (ImGui::Button(u8"套用", ImVec2(120 * s.scale, 0))) {
		std::vector<int> blocks;
		for (int i = 0; i < (int)s.batchSel.size(); i++)
			if (s.batchSel[i]) blocks.push_back(i);
		int touched = ApplyBatchStyle(s, blocks, op);
		s.status = u8"批量修改完成：" + std::to_string((int)blocks.size()) + u8" 條規則、" +
		           std::to_string(touched) + u8" 行變更（未儲存）";
		s.batchMode = false;
		op = BatchStyleOp{};
		ImGui::CloseCurrentPopup();
	}
	ImGui::EndDisabled();
	ImGui::SameLine();
	if (ImGui::Button(u8"取消", ImVec2(120 * s.scale, 0))) {
		op = BatchStyleOp{};
		ImGui::CloseCurrentPopup();
	}
	ImGui::EndPopup();
}
