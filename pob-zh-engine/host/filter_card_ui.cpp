#include "filter_card_ui.h"
#include "filter_parser.h"
#include "filter_schema.h"
#include "editor_util.h"
#include "sound_manager.h"
#include "audio_player.h"

#define IMGUI_DEFINE_MATH_OPERATORS
#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <functional>
#include <string>
#include <vector>

// ---- Chinese display helpers ------------------------------------------------

std::string CardConditionZh(const FilterLine& ln, const FilterI18n& i18n)
{
	std::string s = FilterSchemaKeywordZh(ln.keyword);
	if (!ln.op.empty()) { s += ' '; s += ln.op; }
	for (const FilterToken& v : ln.values) {
		s += ' ';
		std::string zh = FilterSchemaValueZh(ln.keyword, v.text);
		if (zh == v.text) {
			if (ln.keyword == "Class") { std::string z = i18n.ClassNameZh(v.text); if (z != v.text) zh = z; }
			else if (ln.keyword == "BaseType") { std::string z = i18n.DisplayName(v.text); if (z != v.text) zh = z; }
		}
		s += zh;
	}
	return s;
}

std::string CardBlockSummaryZh(const FilterFile& f, const FilterBlock& b, const FilterI18n& i18n)
{
	std::string s;
	int n = 0;
	for (int li : b.lineIdx) {
		const FilterLine& ln = f.lines[li];
		if (ln.kind != FilterLineKind::Condition) continue;
		if (!s.empty()) s += u8"  ·  ";
		s += CardConditionZh(ln, i18n);
		if (++n >= 5) { s += u8" …"; break; }
	}
	if (s.empty()) s = u8"（無條件）";
	return s;
}

// ---- card row plumbing ------------------------------------------------------

namespace {

// English raw text of a line for hover tooltips.
std::string LineEn(const FilterLine& ln)
{
	std::string t = FilterSerializeLine(ln);
	size_t i = 0;
	while (i < t.size() && (t[i] == ' ' || t[i] == '\t')) i++;
	return t.substr(i);
}

// Operator label: StringList cards speak 包含/絕對等於 (partial vs exact match),
// numeric/enum cards show the operator itself.
const char* OpLabel(const char* op, bool stringList)
{
	if (stringList) {
		if (!op[0]) return u8"包含";
		if (op[0] == '=' ) return u8"絕對等於";   // "=" / "=="
		if (op[0] == '!') return u8"不等於";
		return op;
	}
	if (!op[0]) return "=";
	return op;
}

// Draw the operator combo for a line. Returns true if the op changed.
bool OpCombo(FilterLine& ln, const CardSchema& cs, float scale)
{
	if (cs.ops.empty()) return false;
	bool stringList = (cs.kind == CardKind::StringList);
	ImGui::SetNextItemWidth(110 * scale);
	bool changed = false;
	if (ImGui::BeginCombo("##op", OpLabel(ln.op.c_str(), stringList))) {
		for (const char* op : cs.ops) {
			bool sel = (ln.op == op);
			if (ImGui::Selectable(OpLabel(op, stringList), sel) && !sel) {
				ln.op = op;
				ln.dirty = true;
				changed = true;
			}
		}
		ImGui::EndCombo();
	}
	return changed;
}

// One card row frame: title (with tooltip) on the left, widgets in the middle
// via drawWidgets(), a right-aligned 停用 button. Returns true if 停用 clicked.
bool CardRow(EditorShell& s, int lineIdx, const char* title, const char* tooltip,
             const std::function<void()>& drawWidgets)
{
	bool disable = false;
	ImGui::PushID(lineIdx);
	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted(title);
	if (tooltip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tooltip);
	ImGui::SameLine(130 * s.scale);
	drawWidgets();
	ImGui::SameLine(ImGui::GetContentRegionMax().x - 64 * s.scale);
	if (ImGui::SmallButton(u8"停用")) disable = true;
	if (ImGui::IsItemHovered()) ImGui::SetTooltip(u8"以 #! 註解停用此行（檔內保留，可於右欄重新勾選恢復）");
	ImGui::PopID();
	ImGui::Separator();
	return disable;
}

void MarkEdited(EditorShell& s) { s.model.dirty = true; }

// ---- generic widgets per CardKind ------------------------------------------

void DrawBoolWidget(EditorShell& s, FilterLine& ln)
{
	bool isTrue = ln.values.empty() || ln.values[0].text != "False";
	int v = isTrue ? 0 : 1;
	const char* items[2] = { u8"是", u8"否" };
	ImGui::SetNextItemWidth(90 * s.scale);
	if (ImGui::Combo("##bool", &v, items, 2)) {
		FilterSetValueStr(ln, 0, v == 0 ? "True" : "False", false);
		MarkEdited(s);
	}
}

void DrawIntOpWidget(EditorShell& s, FilterLine& ln, const CardSchema& cs)
{
	if (OpCombo(ln, cs, s.scale)) MarkEdited(s);
	ImGui::SameLine();
	int v = FilterValueInt(ln, 0, cs.minInt);
	ImGui::SetNextItemWidth(200 * s.scale);
	bool ch;
	if (cs.maxInt > cs.minInt && cs.maxInt - cs.minInt <= 300)
		ch = ImGui::SliderInt("##val", &v, cs.minInt, cs.maxInt);
	else {
		ch = ImGui::InputInt("##val", &v);
		if (v < cs.minInt) v = cs.minInt;
		if (cs.maxInt > cs.minInt && v > cs.maxInt) v = cs.maxInt;
	}
	if (ch) { FilterSetValueInt(ln, 0, v); MarkEdited(s); }
}

void DrawIntRangeWidget(EditorShell& s, FilterLine& ln, const CardSchema& cs)
{
	int v = FilterValueInt(ln, 0, cs.maxInt);
	ImGui::SetNextItemWidth(220 * s.scale);
	if (ImGui::SliderInt("##val", &v, cs.minInt, cs.maxInt)) {
		FilterSetValueInt(ln, 0, v);
		MarkEdited(s);
	}
}

void DrawEnumOpWidget(EditorShell& s, FilterLine& ln, const CardSchema& cs)
{
	if (OpCombo(ln, cs, s.scale)) MarkEdited(s);
	ImGui::SameLine();
	std::string cur = ln.values.empty() ? std::string() : ln.values[0].text;
	ImGui::SetNextItemWidth(130 * s.scale);
	if (ImGui::BeginCombo("##enum", FilterSchemaValueZh(ln.keyword, cur).c_str())) {
		for (const SchemaEnumValue& e : cs.enums) {
			bool sel = (cur == e.token);
			if (ImGui::Selectable(e.zh, sel) && !sel) {
				FilterSetValueStr(ln, 0, e.token, false);
				MarkEdited(s);
			}
		}
		ImGui::EndCombo();
	}
}

void DrawEnumMultiWidget(EditorShell& s, FilterLine& ln, const CardSchema& cs)
{
	int nSet = 0;
	for (const SchemaEnumValue& e : cs.enums)
		if (FilterHasValue(ln, e.token)) nSet++;
	bool first = true;
	for (const SchemaEnumValue& e : cs.enums) {
		if (!first) ImGui::SameLine();
		first = false;
		bool has = FilterHasValue(ln, e.token);
		bool v = has;
		// Keep at least one value: an empty value list is invalid filter syntax.
		ImGui::BeginDisabled(has && nSet <= 1);
		if (ImGui::Checkbox(e.zh, &v)) {
			if (v) FilterAddValue(ln, e.token, false);
			else FilterRemoveValue(ln, e.token);
			MarkEdited(s);
		}
		ImGui::EndDisabled();
	}
}

void DrawColorWidget(EditorShell& s, FilterLine& ln)
{
	int r, g, b, a;
	bool ha;
	FilterGetColor(ln, r, g, b, a, ha);
	float col[4] = { r / 255.f, g / 255.f, b / 255.f, a / 255.f };
	ImGui::SetNextItemWidth(320 * s.scale);
	if (ImGui::ColorEdit4("##col", col,
			ImGuiColorEditFlags_Uint8 | ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreviewHalf)) {
		int nr = (int)(col[0] * 255.f + 0.5f), ng = (int)(col[1] * 255.f + 0.5f);
		int nb = (int)(col[2] * 255.f + 0.5f), na = (int)(col[3] * 255.f + 0.5f);
		FilterSetColor(ln, nr, ng, nb, na, ha || na != 255);
		MarkEdited(s);
	}
}

void DrawSoundBuiltinWidget(EditorShell& s, FilterLine& ln)
{
	int id = FilterValueInt(ln, 0, 1);
	int vol = FilterValueInt(ln, 1, 300);
	if (id < 1) id = 1;
	if (id > 16) id = 16;
	ImGui::SetNextItemWidth(130 * s.scale);
	if (ImGui::SliderInt(u8"編號", &id, 1, 16)) { FilterSetValueInt(ln, 0, id); MarkEdited(s); }
	ImGui::SameLine();
	ImGui::SetNextItemWidth(150 * s.scale);
	if (ImGui::SliderInt(u8"音量", &vol, 0, 300)) { FilterSetValueInt(ln, 1, vol); MarkEdited(s); }
	ImGui::SameLine();
	bool positional = (ln.keyword == "PlayAlertSoundPositional");
	if (ImGui::Checkbox(u8"3D方位", &positional)) {
		ln.keyword = positional ? "PlayAlertSoundPositional" : "PlayAlertSound";
		ln.dirty = true;
		MarkEdited(s);
	}
	if (ImGui::IsItemHovered()) ImGui::SetTooltip(u8"PlayAlertSoundPositional：音效帶 3D 方位感");
}

void DrawSoundCustomWidget(EditorShell& s, FilterLine& ln)
{
	std::string path = ln.values.empty() ? std::string() : ln.values[0].text;
	ImGui::SetNextItemWidth(220 * s.scale);
	if (ImGui::InputText("##path", &path)) { FilterSetValueStr(ln, 0, path, true); MarkEdited(s); }
	ImGui::SameLine();
	if (ImGui::Button(u8"播放")) PlayAudioFile(EdWiden(path));
	ImGui::SameLine();
	if (ImGui::Button(u8"音效庫…")) ImGui::OpenPopup("##soundlib");
	if (ImGui::BeginPopup("##soundlib")) {
		std::string picked;
		if (DrawSoundLibrary(picked, s.scale)) {
			FilterSetValueStr(ln, 0, picked, true);
			MarkEdited(s);
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}
	ImGui::SameLine();
	bool optional = (ln.keyword == "CustomAlertSoundOptional");
	if (ImGui::Checkbox(u8"可缺", &optional)) {
		ln.keyword = optional ? "CustomAlertSoundOptional" : "CustomAlertSound";
		ln.dirty = true;
		MarkEdited(s);
	}
	if (ImGui::IsItemHovered()) ImGui::SetTooltip(u8"CustomAlertSoundOptional：檔案不存在時忽略，不報錯");
}

// Lines without a schema card render read-only.
void DrawRawFallbackWidget(const FilterLine& ln)
{
	ImGui::TextDisabled("%s", LineEn(ln).c_str());
}

// ---- rich widgets (chips / sockets / mods / icon / effect) ------------------

// Shared colour tokens for MinimapIcon / PlayEffect, with swatch approximations.
struct EffectColor { const char* token; const char* zh; ImVec4 col; };
const EffectColor kEffectColors[] = {
	{ "Red",    u8"紅色", { 0.90f, 0.22f, 0.21f, 1 } },
	{ "Green",  u8"綠色", { 0.30f, 0.78f, 0.31f, 1 } },
	{ "Blue",   u8"藍色", { 0.26f, 0.45f, 0.95f, 1 } },
	{ "Brown",  u8"棕色", { 0.55f, 0.38f, 0.24f, 1 } },
	{ "White",  u8"白色", { 0.95f, 0.95f, 0.95f, 1 } },
	{ "Yellow", u8"黃色", { 0.95f, 0.85f, 0.25f, 1 } },
	{ "Cyan",   u8"青色", { 0.25f, 0.85f, 0.90f, 1 } },
	{ "Grey",   u8"灰色", { 0.55f, 0.55f, 0.55f, 1 } },
	{ "Orange", u8"橙色", { 0.95f, 0.55f, 0.15f, 1 } },
	{ "Pink",   u8"粉色", { 0.95f, 0.55f, 0.75f, 1 } },
	{ "Purple", u8"紫色", { 0.65f, 0.35f, 0.90f, 1 } },
};
struct IconShape { const char* token; const char* zh; };
const IconShape kIconShapes[] = {
	{ "Circle", u8"圓形" }, { "Diamond", u8"鑽石" }, { "Hexagon", u8"六邊形" },
	{ "Square", u8"方形" }, { "Star", u8"星形" }, { "Triangle", u8"三角" },
	{ "Cross", u8"十字" }, { "Moon", u8"月亮" }, { "Raindrop", u8"雨滴" },
	{ "Kite", u8"風箏" }, { "Pentagon", u8"五邊形" }, { "UpsideDownHouse", u8"倒屋" },
};

const EffectColor* FindEffectColor(const std::string& token)
{
	for (const EffectColor& c : kEffectColors)
		if (token == c.token) return &c;
	return nullptr;
}

// Colour combo over kEffectColors with a swatch per entry. Returns new token or "".
std::string EffectColorCombo(const char* id, const std::string& cur, float scale)
{
	std::string out;
	const EffectColor* curC = FindEffectColor(cur);
	if (curC) {
		ImGui::ColorButton("##sw", curC->col,
			ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoPicker | ImGuiColorEditFlags_NoDragDrop,
			ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight()));
		ImGui::SameLine();
	}
	ImGui::SetNextItemWidth(110 * scale);
	if (ImGui::BeginCombo(id, curC ? curC->zh : cur.c_str())) {
		for (const EffectColor& c : kEffectColors) {
			ImGui::PushID(c.token);
			ImGui::ColorButton("##c", c.col,
				ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoPicker | ImGuiColorEditFlags_NoDragDrop,
				ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight()));
			ImGui::SameLine();
			if (ImGui::Selectable(c.zh, cur == c.token)) out = c.token;
			ImGui::PopID();
		}
		ImGui::EndCombo();
	}
	return out;
}

// Display label for one chip value (BaseType/Class translated).
std::string ChipZh(EditorShell& s, const std::string& keyword, const std::string& v)
{
	if (keyword == "Class") { std::string z = s.i18n.ClassNameZh(v); if (z != v) return z + " (" + v + ")"; }
	if (keyword == "BaseType") { std::string z = s.i18n.DisplayName(v); if (z != v) return z + " (" + v + ")"; }
	return v;
}

// StringList card: value chips (click ✕ to remove) + a manual input that
// resolves Chinese via the item catalog. chipStart skips a leading count value
// (ModList). Returns true if any value changed.
bool DrawChipsAndInput(EditorShell& s, FilterLine& ln, size_t chipStart, bool translateInput)
{
	bool changed = false;

	// --- chips, wrapped to the pane width ---
	const float wrapRight = ImGui::GetWindowPos().x + ImGui::GetContentRegionMax().x - 70 * s.scale;
	const float spacing = ImGui::GetStyle().ItemSpacing.x;
	int removeIdx = -1;
	for (size_t i = chipStart; i < ln.values.size(); i++) {
		// U+00D7 ×（在 Default 字型範圍內;U+2715 ✕ 不在 atlas 會畫成 '?'）
		std::string label = ChipZh(s, ln.keyword, ln.values[i].text) + u8"  ×";
		float w = ImGui::CalcTextSize(label.c_str()).x + ImGui::GetStyle().FramePadding.x * 2;
		if (i > chipStart && ImGui::GetItemRectMax().x + spacing + w < wrapRight) ImGui::SameLine();
		ImGui::PushID((int)i);
		ImGui::BeginDisabled(ln.values.size() - chipStart <= 1);  // keep >=1: empty list is invalid
		if (ImGui::SmallButton(label.c_str())) removeIdx = (int)i;
		ImGui::EndDisabled();
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", ln.values[i].text.c_str());
		ImGui::PopID();
	}
	if (removeIdx >= 0) {
		ln.values.erase(ln.values.begin() + removeIdx);
		ln.dirty = true;
		MarkEdited(s);
		changed = true;
	}

	// --- manual input (persistent per line) ---
	static std::string s_input;
	static int s_inputLine = -1;
	static std::vector<LibItem> s_cands;
	uintptr_t lineKey = (uintptr_t)&ln;
	if (s_inputLine != (int)(lineKey & 0x7fffffff)) { s_input.clear(); s_cands.clear(); s_inputLine = (int)(lineKey & 0x7fffffff); }

	ImGui::SetNextItemWidth(200 * s.scale);
	bool enter = ImGui::InputTextWithHint("##manual", u8"可輸入分類/物品等，可中文",
		&s_input, ImGuiInputTextFlags_EnterReturnsTrue);
	ImGui::SameLine();
	bool addClicked = ImGui::Button(u8"點擊增加") || enter;

	// 即時搜尋清單:輸入即過濾物品庫（中/英文子字串），點擊直接加入。
	// Enter / 點擊增加 仍走原本的精確解析+多選 popup 流程。
	static std::vector<LibItem> s_live;
	static std::string s_liveFor;
	static bool s_liveClass = false;
	if (translateInput && !s_input.empty()) {
		bool classCard = (ln.keyword == "Class");
		if (s_liveFor != s_input || s_liveClass != classCard) {
			s_liveFor = s_input;
			s_liveClass = classCard;
			s_live.clear();
			std::string lower = EdToLowerAscii(s_input);
			if (classCard) {
				std::vector<std::string> seen;
				for (const LibItem& it : s.library.items()) {
					if (it.enClass.empty()) continue;
					bool dup = false;
					for (const std::string& c : seen) if (c == it.enClass) { dup = true; break; }
					if (dup) continue;
					seen.push_back(it.enClass);
					std::string zh = s.i18n.ClassNameZh(it.enClass);
					if (EdContainsCI(it.enClass, lower) || zh.find(s_input) != std::string::npos) {
						LibItem cand; cand.en = it.enClass; cand.zh = zh;
						s_live.push_back(std::move(cand));
					}
				}
			} else {
				for (const LibItem& it : s.library.items()) {
					if (EdContainsCI(it.en, lower) || it.zh.find(s_input) != std::string::npos) {
						s_live.push_back(it);
						if (s_live.size() >= 40) break;
					}
				}
			}
		}
		if (!s_live.empty()) {
			float hgt = std::min((float)s_live.size(), 6.5f) * ImGui::GetTextLineHeightWithSpacing()
			            + ImGui::GetStyle().WindowPadding.y * 2;
			ImGui::BeginChild("##livecands", ImVec2(340 * s.scale, hgt), true);
			for (const LibItem& it : s_live) {
				std::string lbl = it.zh == it.en ? it.en : (it.zh + "  (" + it.en + ")");
				if (ImGui::Selectable(lbl.c_str())) {
					if (!FilterHasValue(ln, it.en)) {
						FilterAddValue(ln, it.en, true);
						MarkEdited(s);
						changed = true;
					}
					s_input.clear();
					s_liveFor.clear();
					s_live.clear();
					break;   // s_live cleared — the loop cannot continue
				}
			}
			ImGui::EndChild();
		} else {
			ImGui::TextDisabled(u8"（沒有相符物品 — 可按「點擊增加」以原文加入）");
		}
	}

	if (addClicked && !s_input.empty()) {
		std::string token = s_input;
		s_cands.clear();
		bool exact = false;
		bool classCard = (ln.keyword == "Class");
		if (translateInput) {
			std::string lower = EdToLowerAscii(s_input);
			if (classCard) {
				// Class values are CLASS names, not item names: match the unique
				// class set (English + its Chinese label) from the catalog.
				std::vector<std::string> seen;
				for (const LibItem& it : s.library.items()) {
					if (it.enClass.empty()) continue;
					bool dup = false;
					for (const std::string& c : seen) if (c == it.enClass) { dup = true; break; }
					if (dup) continue;
					seen.push_back(it.enClass);
					std::string zh = s.i18n.ClassNameZh(it.enClass);
					if (it.enClass == token || zh == token) {
						token = it.enClass;
						exact = true;
						s_cands.clear();   // earlier partial matches are moot
						break;
					}
					if (EdContainsCI(it.enClass, lower) || zh.find(s_input) != std::string::npos) {
						LibItem cand;
						cand.en = it.enClass;
						cand.zh = zh;
						s_cands.push_back(std::move(cand));
					}
				}
			} else {
				// Exact English or exact Chinese item hits add directly; otherwise
				// collect substring candidates for the picker popup.
				for (const LibItem& it : s.library.items()) {
					if (it.en == token) { exact = true; break; }
					if (it.zh == token) { token = it.en; exact = true; break; }
				}
				if (!exact) {
					for (const LibItem& it : s.library.items()) {
						if (EdContainsCI(it.en, lower) || it.zh.find(s_input) != std::string::npos) {
							s_cands.push_back(it);
							if (s_cands.size() >= 30) break;
						}
					}
				}
			}
			if (!exact && s_cands.size() == 1) { token = s_cands[0].en; s_cands.clear(); exact = true; }
		}

		// Never silently write non-ASCII (untranslated Chinese) into the model —
		// the .filter output must stay English. Route it through the picker so
		// adding the raw text is an explicit, informed choice.
		bool nonAscii = false;
		for (char c : token)
			if ((unsigned char)c >= 0x80) nonAscii = true;

		if (!s_cands.empty() || (translateInput && !exact && nonAscii)) {
			ImGui::OpenPopup("##candidates");
		} else {
			if (!FilterHasValue(ln, token)) {
				FilterAddValue(ln, token, true);
				MarkEdited(s);
				changed = true;
			}
			s_input.clear();
		}
	}

	if (ImGui::BeginPopup("##candidates")) {
		if (s_cands.empty()) {
			ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.25f, 1),
				u8"找不到相符項。過濾器只認英文名稱，原文加入將無法比對物品。");
		} else {
			ImGui::TextDisabled(u8"多個相符項，請選擇：");
		}
		for (const LibItem& it : s_cands) {
			std::string lbl = it.zh == it.en ? it.en : (it.zh + "  (" + it.en + ")");
			if (ImGui::Selectable(lbl.c_str())) {
				if (!FilterHasValue(ln, it.en)) {
					FilterAddValue(ln, it.en, true);
					MarkEdited(s);
					changed = true;
				}
				s_input.clear();
				ImGui::CloseCurrentPopup();
			}
		}
		ImGui::Separator();
		std::string rawLbl = u8"仍以原文加入：\"" + s_input + "\"";
		if (ImGui::Selectable(rawLbl.c_str())) {
			if (!s_input.empty() && !FilterHasValue(ln, s_input)) {
				FilterAddValue(ln, s_input, true);
				MarkEdited(s);
				changed = true;
			}
			s_input.clear();
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}
	return changed;
}

void DrawStringListWidget(EditorShell& s, FilterLine& ln, const CardSchema& cs)
{
	if (!cs.ops.empty() && OpCombo(ln, cs, s.scale)) MarkEdited(s);
	// Only BaseType / Class get catalog translation; mod-name lists stay raw.
	bool translate = (ln.keyword == "BaseType" || ln.keyword == "Class");
	DrawChipsAndInput(s, ln, 0, translate);
}

// Sockets / SocketGroup token: optional count digit + colour letters ("5GGG").
void DrawSocketSpecWidget(EditorShell& s, FilterLine& ln, const CardSchema& cs)
{
	if (OpCombo(ln, cs, s.scale)) MarkEdited(s);
	ImGui::SameLine();

	std::string tok = ln.values.empty() ? std::string() : ln.values[0].text;
	int num = -1;
	int cnt[6] = { 0, 0, 0, 0, 0, 0 };  // R G B W A D
	static const char kLetters[6] = { 'R', 'G', 'B', 'W', 'A', 'D' };
	size_t p = 0;
	while (p < tok.size() && tok[p] >= '0' && tok[p] <= '9') {
		if (num < 0) num = 0;
		num = num * 10 + (tok[p] - '0');
		p++;
	}
	for (; p < tok.size(); p++)
		for (int c = 0; c < 6; c++)
			if (tok[p] == kLetters[c] || tok[p] == kLetters[c] + 32) cnt[c]++;

	bool ch = false;
	int numUi = num < 0 ? 0 : num;
	ImGui::SetNextItemWidth(110 * s.scale);
	if (ImGui::SliderInt(u8"孔數", &numUi, 0, 6)) { num = numUi; ch = true; }
	static const char* kColorZh[6] = { u8"紅", u8"綠", u8"藍", u8"白", u8"深淵", u8"掘獄" };
	for (int c = 0; c < 6; c++) {
		ImGui::SameLine();
		ImGui::PushID(c);
		ImGui::SetNextItemWidth(64 * s.scale);
		int v = cnt[c];
		if (ImGui::InputInt(kColorZh[c], &v, 1, 1)) {
			if (v < 0) v = 0;
			if (v > 6) v = 6;
			cnt[c] = v;
			ch = true;
		}
		ImGui::PopID();
	}
	if (ch) {
		std::string ntok;
		if (num > 0) ntok += std::to_string(num);
		for (int c = 0; c < 6; c++)
			for (int k = 0; k < cnt[c]; k++) ntok += kLetters[c];
		if (ntok.empty()) ntok = "0";
		FilterSetValueStr(ln, 0, ntok, false);
		MarkEdited(s);
	}
}

// HasExplicitMod: op + optional count + mod-name chips.
void DrawModListWidget(EditorShell& s, FilterLine& ln, const CardSchema& cs)
{
	if (OpCombo(ln, cs, s.scale)) MarkEdited(s);

	// A leading all-digit value is the "at least N mods" count (only meaningful
	// with an operator).
	bool hasCount = false;
	int count = 0;
	if (!ln.values.empty() && !ln.values[0].quoted && !ln.values[0].text.empty()) {
		hasCount = true;
		for (char c : ln.values[0].text)
			if (c < '0' || c > '9') { hasCount = false; break; }
		if (hasCount) count = FilterValueInt(ln, 0, 0);
	}
	ImGui::SameLine();
	int ui = hasCount ? count : 0;
	ImGui::SetNextItemWidth(110 * s.scale);
	if (ImGui::InputInt(u8"至少 N 條", &ui, 1, 1)) {
		if (ui < 0) ui = 0;
		if (ui > 6) ui = 6;
		if (ui > 0 && hasCount) {
			FilterSetValueInt(ln, 0, ui);
		} else if (ui > 0 && !hasCount) {
			ln.values.insert(ln.values.begin(), FilterToken{ std::to_string(ui), false });
			ln.dirty = true;
		} else if (ui == 0 && hasCount) {
			ln.values.erase(ln.values.begin());
			ln.dirty = true;
		}
		MarkEdited(s);
		hasCount = ui > 0;
	}
	if (ImGui::IsItemHovered()) ImGui::SetTooltip(u8"0 = 不限；搭配運算子如 >=2 表示至少 2 條相符詞綴");

	DrawChipsAndInput(s, ln, hasCount ? 1 : 0, false);
}

void DrawMinimapIconWidget(EditorShell& s, FilterLine& ln)
{
	// values: size(0..2) colour shape
	int size = FilterValueInt(ln, 0, 1);
	if (size < 0) size = 0;
	if (size > 2) size = 2;
	static const char* kSizeZh[3] = { u8"大", u8"中", u8"小" };
	ImGui::SetNextItemWidth(80 * s.scale);
	if (ImGui::Combo(u8"大小", &size, kSizeZh, 3)) { FilterSetValueInt(ln, 0, size); MarkEdited(s); }

	ImGui::SameLine();
	std::string col = ln.values.size() > 1 ? ln.values[1].text : "White";
	std::string ncol = EffectColorCombo("##mmcol", col, s.scale);
	if (!ncol.empty()) { FilterSetValueStr(ln, 1, ncol, false); MarkEdited(s); }

	ImGui::SameLine();
	std::string shape = ln.values.size() > 2 ? ln.values[2].text : "Circle";
	const char* shapeZh = shape.c_str();
	for (const IconShape& is : kIconShapes)
		if (shape == is.token) { shapeZh = is.zh; break; }
	ImGui::SetNextItemWidth(100 * s.scale);
	if (ImGui::BeginCombo("##mmshape", shapeZh)) {
		for (const IconShape& is : kIconShapes)
			if (ImGui::Selectable(is.zh, shape == is.token)) {
				FilterSetValueStr(ln, 2, is.token, false);
				MarkEdited(s);
			}
		ImGui::EndCombo();
	}
}

void DrawPlayEffectWidget(EditorShell& s, FilterLine& ln)
{
	std::string col = ln.values.empty() ? "White" : ln.values[0].text;
	std::string ncol = EffectColorCombo("##fxcol", col, s.scale);
	if (!ncol.empty()) { FilterSetValueStr(ln, 0, ncol, false); MarkEdited(s); }

	ImGui::SameLine();
	bool temp = ln.values.size() > 1 && ln.values[1].text == "Temp";
	if (ImGui::Checkbox(u8"僅掉落瞬間", &temp)) {
		if (temp) FilterSetValueStr(ln, 1, "Temp", false);
		else if (ln.values.size() > 1) { ln.values.resize(1); ln.dirty = true; }
		MarkEdited(s);
	}
	if (ImGui::IsItemHovered()) ImGui::SetTooltip(u8"Temp：光柱只在掉落瞬間顯示，之後熄滅");
}

// Dispatch one live schema card's widgets.
void DrawCardWidgets(EditorShell& s, FilterLine& ln, const CardSchema& cs)
{
	switch (cs.kind) {
		case CardKind::Toggle:       ImGui::TextDisabled(u8"已啟用"); break;
		case CardKind::Bool:         DrawBoolWidget(s, ln); break;
		case CardKind::IntOp:        DrawIntOpWidget(s, ln, cs); break;
		case CardKind::IntRange:     DrawIntRangeWidget(s, ln, cs); break;
		case CardKind::EnumOp:       DrawEnumOpWidget(s, ln, cs); break;
		case CardKind::EnumMulti:    DrawEnumMultiWidget(s, ln, cs); break;
		case CardKind::StringList:   DrawStringListWidget(s, ln, cs); break;
		case CardKind::ModList:      DrawModListWidget(s, ln, cs); break;
		case CardKind::SocketSpec:   DrawSocketSpecWidget(s, ln, cs); break;
		case CardKind::Color:        DrawColorWidget(s, ln); break;
		case CardKind::SoundBuiltin: DrawSoundBuiltinWidget(s, ln); break;
		case CardKind::SoundCustom:  DrawSoundCustomWidget(s, ln); break;
		case CardKind::MinimapIcon:  DrawMinimapIconWidget(s, ln); break;
		case CardKind::PlayEffect:   DrawPlayEffectWidget(s, ln); break;
		default:                     DrawRawFallbackWidget(ln); break;
	}
}

// Find a disabled ("#!") line of this card's keyword (or alias) in the block.
int FindDisabledLine(EditorShell& s, int blockIdx, const CardSchema& cs)
{
	if (blockIdx < 0 || blockIdx >= (int)s.model.blocks.size()) return -1;
	for (int li : s.model.blocks[blockIdx].lineIdx) {
		FilterLine parsed;
		if (!s.doc.IsDisabledLine(li, &parsed)) continue;
		if (parsed.keyword == cs.keyword || (cs.alias && parsed.keyword == cs.alias)) return li;
	}
	return -1;
}

int FindLiveLine(EditorShell& s, int blockIdx, const CardSchema& cs)
{
	int li = s.doc.FindLine(blockIdx, cs.keyword);
	if (li < 0 && cs.alias) li = s.doc.FindLine(blockIdx, cs.alias);
	return li;
}

} // namespace

// ---- shared combos (also used by the batch-modify modal) --------------------

std::string CardEffectColorCombo(const char* id, const std::string& cur, float scale)
{
	return EffectColorCombo(id, cur, scale);
}

std::string CardIconShapeCombo(const char* id, const std::string& cur, float scale)
{
	std::string out;
	const char* curZh = cur.c_str();
	for (const IconShape& is : kIconShapes)
		if (cur == is.token) { curZh = is.zh; break; }
	ImGui::SetNextItemWidth(100 * scale);
	if (ImGui::BeginCombo(id, curZh)) {
		for (const IconShape& is : kIconShapes)
			if (ImGui::Selectable(is.zh, cur == is.token)) out = is.token;
		ImGui::EndCombo();
	}
	return out;
}

// ---- middle pane ------------------------------------------------------------

bool DrawBlockCards(EditorShell& s, int blockIdx)
{
	if (blockIdx < 0 || blockIdx >= (int)s.model.blocks.size()) return false;
	FilterBlock& b = s.model.blocks[blockIdx];

	// Show/Hide toggle (header verb). Not structural: indices stay valid.
	{
		bool show = !b.hide;
		if (ImGui::Checkbox(u8"顯示物品", &show)) SetBlockHide(s, b, !show);
		ImGui::SameLine();
		ImGui::TextDisabled(b.hide ? u8"Hide — 此類物品將被隱藏" : u8"Show — 此類物品會顯示標籤");
	}
	ImGui::Separator();

	// Collect the block's rows, conditions first, keeping file order inside
	// each section. Disabled lines render with a 恢復 button.
	struct Row { int li; const CardSchema* cs; bool disabled; FilterLine parsed; };
	std::vector<Row> conds, acts;
	for (int li : b.lineIdx) {
		const FilterLine& ln = s.model.lines[li];
		if (ln.kind == FilterLineKind::Condition || ln.kind == FilterLineKind::Action ||
		    ln.kind == FilterLineKind::Unknown) {
			Row r{ li, FilterSchemaFind(ln.keyword), false, {} };
			bool isAct = (ln.kind == FilterLineKind::Action) || (r.cs && r.cs->isAction);
			(isAct ? acts : conds).push_back(std::move(r));
		} else if (ln.kind == FilterLineKind::Comment) {
			Row r{ li, nullptr, true, {} };
			if (!s.doc.IsDisabledLine(li, &r.parsed)) continue;
			if (r.parsed.kind == FilterLineKind::BlockHeader) continue;
			r.cs = FilterSchemaFind(r.parsed.keyword);
			bool isAct = (r.parsed.kind == FilterLineKind::Action) || (r.cs && r.cs->isAction);
			(isAct ? acts : conds).push_back(std::move(r));
		}
	}

	auto drawRows = [&](std::vector<Row>& rows, const char* sectionTitle) -> bool {
		if (rows.empty()) return false;
		ImGui::TextDisabled("%s", sectionTitle);
		for (Row& r : rows) {
			if (r.disabled) {
				ImGui::PushID(r.li);
				ImGui::AlignTextToFramePadding();
				std::string t = u8"（已停用）" + FilterSchemaKeywordZh(r.parsed.keyword);
				ImGui::TextDisabled("%s", t.c_str());
				if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", LineEn(r.parsed).c_str());
				ImGui::SameLine(ImGui::GetContentRegionMax().x - 64 * s.scale);
				bool restore = ImGui::SmallButton(u8"恢復");
				ImGui::PopID();
				ImGui::Separator();
				if (restore) { s.doc.RestoreLine(r.li); return true; }
				continue;
			}
			FilterLine& ln = s.model.lines[r.li];
			std::string title = FilterSchemaKeywordZh(ln.keyword);
			const char* tooltip = (r.cs && r.cs->tooltip) ? r.cs->tooltip : nullptr;
			std::string en = LineEn(ln);
			bool disable = CardRow(s, r.li, title.c_str(),
				tooltip ? tooltip : en.c_str(),
				[&] {
					if (r.cs) DrawCardWidgets(s, ln, *r.cs);
					else DrawRawFallbackWidget(ln);
				});
			if (disable) { s.doc.CommentOutLine(r.li); return true; }
		}
		return false;
	};

	if (drawRows(conds, u8"條件（符合以下全部才套用）")) return true;
	ImGui::Spacing();
	if (drawRows(acts, u8"動作（外觀 / 音效 / 提示）")) return true;
	return false;
}

// ---- right add-column -------------------------------------------------------

bool DrawAddColumn(EditorShell& s, int blockIdx)
{
	ImGui::TextUnformatted(u8"過濾項增加欄");
	ImGui::TextDisabled(u8"勾選加入該項，取消停用該項");
	ImGui::Separator();
	if (blockIdx < 0 || blockIdx >= (int)s.model.blocks.size()) {
		ImGui::TextDisabled(u8"（先在左側選擇一個過濾項）");
		return false;
	}

	for (const char* grp : FilterSchemaGroups()) {
		ImGui::SeparatorText(grp);
		for (const CardSchema& cs : FilterSchemaAll()) {
			if (cs.group != grp) continue;
			ImGui::PushID(cs.keyword);
			int live = FindLiveLine(s, blockIdx, cs);
			bool checked = live >= 0;
			bool v = checked;
			bool clicked = ImGui::Checkbox(cs.zh, &v);
			if (cs.tooltip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", cs.tooltip);
			if (!checked && FindDisabledLine(s, blockIdx, cs) >= 0) {
				ImGui::SameLine();
				ImGui::TextDisabled(u8"（已停用）");
			}
			ImGui::PopID();
			if (!clicked) continue;

			if (v) {
				// Tick: restore a disabled line of this keyword if one exists,
				// otherwise insert the schema's default line.
				int dis = FindDisabledLine(s, blockIdx, cs);
				if (dis >= 0) {
					s.doc.RestoreLine(dis);
				} else {
					FilterLine dl = ParseFilterLine(cs.defaultLine);
					s.doc.InsertLine(blockIdx, dl.keyword, dl.op, dl.values);
				}
				// Mutually-exclusive group: disable the other members' live lines.
				if (cs.exclusiveGroup > 0) {
					for (const CardSchema& other : FilterSchemaAll()) {
						if (&other == &cs || other.exclusiveGroup != cs.exclusiveGroup) continue;
						for (;;) {
							int oli = FindLiveLine(s, blockIdx, other);
							if (oli < 0) break;
							s.doc.CommentOutLine(oli);
						}
					}
				}
			} else {
				// Untick: disable every live line of this keyword (and alias).
				for (;;) {
					int oli = FindLiveLine(s, blockIdx, cs);
					if (oli < 0) break;
					s.doc.CommentOutLine(oli);
				}
			}
			return true;
		}
	}
	return false;
}
