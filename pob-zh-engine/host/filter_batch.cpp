#include "filter_batch.h"
#include "filter_card_ui.h"
#include "filter_parser.h"
#include "filter_schema.h"
#include "editor_util.h"
#include "audio_player.h"
#include "ui_theme.h"
#include "ui_widgets.h"
#include "ui_icons.h"

#define IMGUI_DEFINE_MATH_OPERATORS
#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <memory>
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


// ---- the dialog (design: FilterBatch.dc.html) ---------------------------------

namespace Tok = PobUi::Tok;

struct BatchUiState {
	BatchStyleOp op;
	bool open = false;            // BeginDialog's open flag
	int pop = 0;                  // popover wanted: 1-3 colour (text/border/back), 4 minimap, 5 beam
	bool popReq = false;
	ImVec2 anchor{ 0, 0 };
};

namespace {

BatchUiState& BUI(EditorShell& s)
{
	if (!s.batchUi) s.batchUi = std::make_shared<BatchUiState>();
	return *s.batchUi;
}

ImFont* SmallFace() { const PobUi::WidgetFonts& wf = PobUi::Fonts(); return wf.small ? wf.small : ImGui::GetFont(); }

// One dialog row: label | tri-state | value. The value part is drawn by the
// caller between BatchRowBegin and BatchRowEnd.
struct BRow { float y = 0, x = 0, w = 0; };

BRow BatchRowBegin(const char* label, const char* id, Tri* tri, bool removable, bool first)
{
	BRow r;
	const ImVec2 p = ImGui::GetCursorScreenPos();
	r.x = p.x;
	r.w = ImGui::GetContentRegionAvail().x;
	if (!first)
		ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, p.y), ImVec2(p.x + r.w, p.y), Tok::BorderSubtle, 1.0f);
	r.y = p.y + std::floor(PobUi::D(8.0f));
	ImFont* f = SmallFace();
	ImGui::SetCursorScreenPos(ImVec2(r.x, r.y + std::floor((PobUi::ControlH() - f->FontSize) * 0.5f)));
	PobUi::Hint(label);
	ImGui::SetCursorScreenPos(ImVec2(r.x + std::floor(PobUi::D(120.0f)), r.y));
	static const char* kTri[3] = { u8"維持", u8"設為", u8"停用" };
	const bool en[3] = { true, true, removable };
	static const char* kTips[3] = { nullptr, nullptr, u8"顯示 / 隱藏只能二選一，沒辦法停用" };
	int v = (int)*tri;
	if (PobUi::SegmentedEx(id, &v, kTri, 3, en, removable ? nullptr : kTips)) *tri = (Tri)v;
	ImGui::SameLine(0, PobUi::D(12.0f));
	ImGui::BeginGroup();
	return r;
}

void BatchRowEnd(const BRow& r)
{
	ImGui::EndGroup();
	const float bottom = (std::max)(ImGui::GetItemRectMax().y, r.y + PobUi::ControlH());
	ImGui::SetCursorScreenPos(ImVec2(r.x, bottom + std::floor(PobUi::D(8.0f))));
	ImGui::Dummy(ImVec2(r.w, 0.0f));
}

void CenterHint(const char* t)
{
	ImFont* f = SmallFace();
	const ImVec2 p = ImGui::GetCursorScreenPos();
	ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + std::floor((PobUi::ControlH() - f->FontSize) * 0.5f)));
	PobUi::Hint(t);
}

void ColorValue(BatchUiState& u, int chan, const int c[4])
{
	const unsigned char sw[4] = { (unsigned char)c[0], (unsigned char)c[1], (unsigned char)c[2], (unsigned char)c[3] };
	const ImVec2 p = ImGui::GetCursorScreenPos();
	ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + std::floor((PobUi::ControlH() - PobUi::D(22.0f)) * 0.5f)));
	ImGui::PushID(chan);
	if (EdSwatch("##sw", sw, sw, 22.0f, true)) {
		u.pop = chan;
		u.popReq = true;
		u.anchor = ImVec2(ImGui::GetItemRectMin().x, ImGui::GetItemRectMax().y + PobUi::D(4.0f));
	}
	if (ImGui::IsItemHovered()) PobUi::Tooltip(u8"點一下改顏色");
	ImGui::PopID();
	ImGui::SameLine(0, PobUi::D(8.0f));
	char buf[48];
	std::snprintf(buf, sizeof(buf), "%d %d %d %d", c[0], c[1], c[2], c[3]);
	ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, p.y + std::floor((PobUi::ControlH() - SmallFace()->FontSize) * 0.5f)));
	PobUi::Numeric(buf, Tok::Text);
}

} // namespace

void OpenBatchDialog(EditorShell& s)
{
	BatchUiState& u = BUI(s);
	u.open = true;
}

void DrawBatchDialog(EditorShell& s)
{
	BatchUiState& u = BUI(s);
	BatchStyleOp& op = u.op;
	const int nSel = (int)std::count(s.batchSel.begin(), s.batchSel.end(), (char)1);
	const std::string nStr = std::to_string(nSel);
	const std::string title = u8"修改已選的 " + nStr + u8" 條規則";
	// no spaces inside the sentence: ImGui wraps only at spaces, and a CJK run
	// after one would jump to the next line whole
	const std::string body = u8"每一項選「維持」就不動；「設為」會把這" + nStr +
		u8"條都改成同一個值；「停用」會停用這一項，之後可以恢復。";
	if (!PobUi::BeginDialog(u8"##febatch", &u.open, title.c_str(), body.c_str(), nullptr, 600.0f)) return;
	ImGui::Dummy(ImVec2(0, PobUi::D(4.0f)));
	auto removedHint = [&](const char* what) {
		const std::string t = nStr + u8" 條的" + what + u8"會被停用";
		CenterHint(t.c_str());
	};

	{	// show / hide
		BRow r = BatchRowBegin(u8"顯示 / 隱藏", "##tsh", &op.showHide, false, true);
		if (op.showHide == Tri::Set) {
			static const char* kSh[2] = { u8"顯示", u8"隱藏" };
			int v = op.hide ? 1 : 0;
			if (PobUi::Segmented("##sh", &v, kSh, 2)) op.hide = v == 1;
		}
		BatchRowEnd(r);
	}
	struct ColorRow { const char* label; Tri* tri; int* c; };
	ColorRow colors[3] = {
		{ u8"文字顏色", &op.textColor, op.text },
		{ u8"邊框顏色", &op.borderColor, op.border },
		{ u8"背景顏色", &op.bgColor, op.bg },
	};
	for (int k = 0; k < 3; k++) {
		ImGui::PushID(k);
		BRow r = BatchRowBegin(colors[k].label, "##tcol", colors[k].tri, true, false);
		if (*colors[k].tri == Tri::Set) ColorValue(u, k + 1, colors[k].c);
		else if (*colors[k].tri == Tri::Remove) removedHint(colors[k].label);
		BatchRowEnd(r);
		ImGui::PopID();
	}
	{	// font size
		BRow r = BatchRowBegin(u8"字級", "##tfs", &op.fontSize, true, false);
		if (op.fontSize == Tri::Set) {
			PobUi::PushControlFrame();
			ImGui::SetNextItemWidth(std::floor(PobUi::D(72.0f)));
			if (ImGui::InputInt("##fs", &op.size, 0, 0)) op.size = std::clamp(op.size, 1, 45);
			PobUi::PopControlFrame();
			ImGui::SameLine(0, PobUi::D(8.0f));
			CenterHint(u8"1~45");
		} else if (op.fontSize == Tri::Remove) {
			removedHint(u8"字級");
		}
		BatchRowEnd(r);
	}
	{	// sound
		BRow r = BatchRowBegin(u8"音效", "##tsnd", &op.sound, true, false);
		if (op.sound == Tri::Set) {
			static const char* kSrc[2] = { u8"內建", u8"自訂" };
			int v = op.custom ? 1 : 0;
			if (PobUi::Segmented("##src", &v, kSrc, 2)) op.custom = v == 1;
			if (!op.custom) {
				ImGui::SameLine(0, PobUi::D(8.0f));
				std::vector<std::string> ids;
				std::vector<const char*> idp;
				for (int i = 1; i <= 16; i++) ids.push_back(std::to_string(i) + u8" 號");
				for (const std::string& t : ids) idp.push_back(t.c_str());
				int sel = std::clamp(op.soundId, 1, 16) - 1;
				if (PobUi::Select("##sid", &sel, idp.data(), nullptr, 16, std::floor(PobUi::D(90.0f)))) op.soundId = sel + 1;
			} else {
				if (!s.soundsInit) { s.sounds.Init(s.exeDir); s.soundsInit = true; }
				std::vector<std::string> names;
				int sel = -1;
				for (const SoundFileInfo& fi : s.sounds.files()) {
					names.push_back(EdNarrow(fi.name));
					if (names.back() == op.customPath) sel = (int)names.size() - 1;
				}
				if (names.empty()) {
					CenterHint(u8"音效資料夾裡沒有檔案");
				} else {
					std::vector<const char*> np;
					for (const std::string& n : names) np.push_back(n.c_str());
					if (sel < 0) { sel = 0; op.customPath = names[0]; }
					// the file on its own line, sized to what is left of the row
					const float testW = PobUi::ButtonWidth(u8"試聽", PobUi::BtnSize::Sm, PobIcon::Play);
					const float room = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x - ImGui::GetCursorScreenPos().x -
					                   testW - PobUi::D(8.0f);
					const float fw = (std::max)(PobUi::D(120.0f), (std::min)(room, (std::max)(std::floor(PobUi::D(180.0f)),
					                                                                     PobUi::SelectFitWidth(np.data(), (int)np.size()))));
					if (PobUi::Select("##sfile", &sel, np.data(), nullptr, (int)np.size(), fw)) op.customPath = names[sel];
					ImGui::SameLine(0, PobUi::D(8.0f));
					if (PobUi::Button(u8"試聽", PobUi::BtnKind::Secondary, PobUi::BtnSize::Sm, PobIcon::Play))
						PlayAudioFileVol(s.sounds.folder() + L"\\" + EdWiden(op.customPath), std::clamp(op.volume / 3, 0, 100));
				}
			}
			CenterHint(u8"音量");
			ImGui::SameLine(0, PobUi::D(8.0f));
			ImGui::SetNextItemWidth(std::floor(PobUi::D(180.0f)));
			ImGui::SliderInt("##vol", &op.volume, 0, 300);
		} else if (op.sound == Tri::Remove) {
			removedHint(u8"音效");
		}
		BatchRowEnd(r);
	}
	{	// minimap icon
		BRow r = BatchRowBegin(u8"小地圖圖示", "##tmm", &op.minimapIcon, true, false);
		if (op.minimapIcon == Tri::Set) {
			static const char* kSz[3] = { u8"大", u8"中", u8"小" };
			const std::string sum = std::string(kSz[std::clamp(op.mmSize, 0, 2)]) + u8" · " + CardEffectColorZh(op.mmColor) +
			                        u8" · " + CardShapeZh(op.mmShape);
			if (PobUi::Button((sum + "##mmsum").c_str(), PobUi::BtnKind::Secondary, PobUi::BtnSize::Sm)) {
				u.pop = 4;
				u.popReq = true;
				u.anchor = ImVec2(ImGui::GetItemRectMin().x, ImGui::GetItemRectMax().y + PobUi::D(4.0f));
			}
		} else if (op.minimapIcon == Tri::Remove) {
			removedHint(u8"小地圖圖示");
		}
		BatchRowEnd(r);
	}
	{	// beam
		BRow r = BatchRowBegin(u8"光柱", "##tfx", &op.playEffect, true, false);
		if (op.playEffect == Tri::Set) {
			const std::string sum = std::string(CardEffectColorZh(op.fxColor)) + (op.fxTemp ? u8" · 只在掉落瞬間" : u8" · 持續顯示");
			if (PobUi::Button((sum + "##fxsum").c_str(), PobUi::BtnKind::Secondary, PobUi::BtnSize::Sm)) {
				u.pop = 5;
				u.popReq = true;
				u.anchor = ImVec2(ImGui::GetItemRectMin().x, ImGui::GetItemRectMax().y + PobUi::D(4.0f));
			}
		} else if (op.playEffect == Tri::Remove) {
			removedHint(u8"光柱");
		}
		BatchRowEnd(r);
	}

	// popovers inside the dialog, at the dialog's top level (never in a PushID loop)
	static const char* kPop[6] = { "", "##bpcol", "##bpcol", "##bpcol", "##bpmm", "##bpfx" };
	if (u.popReq) {
		u.popReq = false;
		if (u.pop > 0) ImGui::OpenPopup(kPop[u.pop]);
	}
	if (u.pop > 0) {
		ImGui::SetNextWindowPos(u.anchor, ImGuiCond_Appearing);
		if (CardBeginPopover(kPop[u.pop])) {
			if (u.pop <= 3) {
				int* c = u.pop == 1 ? op.text : (u.pop == 2 ? op.border : op.bg);
				CardColorEditor(s, "##bce", c);
			} else if (u.pop == 4) {
				CardMinimapEditor("##bmm", &op.mmSize, &op.mmColor, &op.mmShape);
			} else {
				CardEffectEditor("##bfx", &op.fxColor, &op.fxTemp);
			}
			CardEndPopover();
		} else if (!ImGui::IsPopupOpen(kPop[u.pop])) {
			u.pop = 0;
		}
	}

	const bool any = op.showHide != Tri::Keep || op.textColor != Tri::Keep || op.borderColor != Tri::Keep ||
	                 op.bgColor != Tri::Keep || op.fontSize != Tri::Keep || op.sound != Tri::Keep ||
	                 op.minimapIcon != Tri::Keep || op.playEffect != Tri::Keep;
	const std::string applyLbl = u8"套用到 " + nStr + u8" 條";
	const PobUi::DialogResult res = PobUi::DialogButtons(u8"取消", nullptr, nullptr, applyLbl.c_str(), any && nSel > 0);
	if (res == PobUi::DialogResult::Primary) {
		std::vector<int> blocks;
		for (int i = 0; i < (int)s.batchSel.size(); i++)
			if (s.batchSel[i]) blocks.push_back(i);
		const int touched = ApplyBatchStyle(s, blocks, op);
		s.Notify(u8"已修改 " + std::to_string((int)blocks.size()) + u8" 條規則、" + std::to_string(touched) +
		         u8" 行（還沒儲存）");
		s.batchMode = false;
		op = BatchStyleOp{};
	} else if (res == PobUi::DialogResult::Cancel) {
		op = BatchStyleOp{};
	}
	PobUi::EndDialog();
}

// Test aid (POBTOOLS_FILTER_STATE=batch): the dialog as the design shows it.
void BatchTestPreset(EditorShell& s)
{
	BatchUiState& u = BUI(s);
	u.op = BatchStyleOp{};
	u.op.textColor = Tri::Set;
	const int red[4] = { 255, 0, 0, 255 }, white[4] = { 255, 255, 255, 255 };
	for (int i = 0; i < 4; i++) { u.op.text[i] = red[i]; u.op.bg[i] = white[i]; }
	u.op.bgColor = Tri::Set;
	u.op.sound = Tri::Set;
	u.op.custom = true;
	u.op.customPath = "1maybevaluable.mp3";
	u.op.playEffect = Tri::Remove;
	u.open = true;
}
