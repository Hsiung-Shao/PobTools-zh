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
#include <imgui_internal.h>   // GetActiveID
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Tok = PobUi::Tok;

// ---- Chinese display helpers ------------------------------------------------

std::string CardValueZh(const FilterLine& ln, const FilterI18n& i18n)
{
	std::string s;
	if (!ln.op.empty()) s += ln.op;
	for (const FilterToken& v : ln.values) {
		if (!s.empty()) s += ' ';
		std::string zh = FilterSchemaValueZh(ln.keyword, v.text);
		if (zh == v.text) {
			if (ln.keyword == "Class") { std::string z = i18n.ClassNameZh(v.text); if (z != v.text) zh = z; }
			else if (ln.keyword == "BaseType") { std::string z = i18n.DisplayName(v.text); if (z != v.text) zh = z; }
		}
		s += zh;
	}
	return s;
}

std::string CardConditionZh(const FilterLine& ln, const FilterI18n& i18n)
{
	std::string s = FilterSchemaKeywordZh(ln.keyword);
	const std::string v = CardValueZh(ln, i18n);
	if (!v.empty()) { s += ' '; s += v; }
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

namespace {

struct EffectColor { const char* token; const char* zh; };
const EffectColor kEffectColors[] = {
	{ "Red", u8"紅色" }, { "Green", u8"綠色" }, { "Blue", u8"藍色" }, { "White", u8"白色" },
	{ "Yellow", u8"黃色" }, { "Cyan", u8"青色" }, { "Grey", u8"灰色" }, { "Pink", u8"粉色" },
	{ "Orange", u8"橙色" }, { "Purple", u8"紫色" }, { "Brown", u8"棕色" },
};
struct IconShape { const char* token; const char* zh; };
const IconShape kIconShapes[] = {
	{ "Circle", u8"圓形" }, { "Diamond", u8"鑽石" }, { "Hexagon", u8"六邊形" },
	{ "Square", u8"方形" }, { "Star", u8"星形" }, { "Triangle", u8"三角形" },
	{ "Cross", u8"十字" }, { "Moon", u8"月亮" }, { "Raindrop", u8"雨滴" },
	{ "Kite", u8"風箏" }, { "Pentagon", u8"五邊形" }, { "UpsideDownHouse", u8"倒屋" },
};
const char* kSizeZh[3] = { u8"大", u8"中", u8"小" };

const char* ShapeZhImpl(const std::string& tok)
{
	for (const IconShape& is : kIconShapes)
		if (tok == is.token) return is.zh;
	return tok.c_str();
}

} // namespace

const char* CardShapeZh(const std::string& token) { return ShapeZhImpl(token); }

const char* CardEffectColorZh(const std::string& token)
{
	for (const EffectColor& c : kEffectColors)
		if (token == c.token) return c.zh;
	return token.c_str();
}

std::string CardMinimapSummary(const FilterLine& ln)
{
	const int size = std::clamp(FilterValueInt(ln, 0, 1), 0, 2);
	const std::string col = ln.values.size() > 1 ? ln.values[1].text : "White";
	const std::string shape = ln.values.size() > 2 ? ln.values[2].text : "Circle";
	return std::string(kSizeZh[size]) + u8" · " + CardEffectColorZh(col) + u8" · " + ShapeZhImpl(shape);
}

std::string CardSoundSummary(const FilterLine& ln)
{
	if (ln.keyword == "CustomAlertSound" || ln.keyword == "CustomAlertSoundOptional") {
		std::string s = u8"自訂 " + (ln.values.empty() ? std::string("?") : ln.values[0].text);
		if (ln.values.size() > 1) s += u8" · 音量 " + ln.values[1].text;
		return s;
	}
	std::string s = u8"內建 " + std::to_string(FilterValueInt(ln, 0, 1)) + u8" 號";
	if (ln.values.size() > 1) s += u8" · 音量 " + std::to_string(FilterValueInt(ln, 1, 300));
	if (ln.keyword == "PlayAlertSoundPositional") s += u8" · 3D";
	return s;
}

std::string CardEffectSummary(const FilterLine& ln)
{
	const std::string col = ln.values.empty() ? "White" : ln.values[0].text;
	const bool temp = ln.values.size() > 1 && ln.values[1].text == "Temp";
	return std::string(CardEffectColorZh(col)) + (temp ? u8" · 只在掉落瞬間" : u8" · 持續顯示");
}

// ---- per-panel UI state -----------------------------------------------------

enum class CardPop { None, Color, Minimap, Effect, Sound };

struct CardUiState {
	// BaseType / Class text input (one line at a time)
	std::string input;
	const void* inputLine = nullptr;
	std::vector<LibItem> live;
	std::string liveFor;
	bool liveClass = false;
	bool notFound = false;          // Enter on a name the catalog does not know
	// popovers
	CardPop pop = CardPop::None;    // open (or opening) popover
	bool openReq = false;
	int colorChan = 0;              // 0 text / 1 border / 2 background
	ImVec2 anchor{ 0, 0 };
	char rgbaBuf[48] = "";
	int rgbaFor = -1;               // channel the buffer mirrors
};

namespace {

CardUiState& UI(EditorShell& s)
{
	if (!s.cardUi) s.cardUi = std::make_shared<CardUiState>();
	return *s.cardUi;
}

const char* const kColorKw[3] = { "SetTextColor", "SetBorderColor", "SetBackgroundColor" };

// English raw text of a line for hover tooltips.
std::string LineEn(const FilterLine& ln)
{
	std::string t = FilterSerializeLine(ln);
	size_t i = 0;
	while (i < t.size() && (t[i] == ' ' || t[i] == '\t')) i++;
	return t.substr(i);
}

void MarkEdited(EditorShell& s) { s.model.dirty = true; }

int FindLiveLine(EditorShell& s, int blockIdx, const CardSchema& cs)
{
	int li = s.doc.FindLine(blockIdx, cs.keyword);
	if (li < 0 && cs.alias) li = s.doc.FindLine(blockIdx, cs.alias);
	return li;
}

int FindLiveKw(EditorShell& s, int blockIdx, const char* kw, const char* alias = nullptr)
{
	int li = s.doc.FindLine(blockIdx, kw);
	if (li < 0 && alias) li = s.doc.FindLine(blockIdx, alias);
	return li;
}

// A disabled ("#!") line of this keyword (or alias) in the block.
int FindDisabledKw(EditorShell& s, int blockIdx, const char* kw, const char* alias = nullptr)
{
	if (blockIdx < 0 || blockIdx >= (int)s.model.blocks.size()) return -1;
	for (int li : s.model.blocks[blockIdx].lineIdx) {
		FilterLine parsed;
		if (!s.doc.IsDisabledLine(li, &parsed)) continue;
		if (parsed.keyword == kw || (alias && parsed.keyword == alias)) return li;
	}
	return -1;
}

int FindDisabledLine(EditorShell& s, int blockIdx, const CardSchema& cs)
{
	return FindDisabledKw(s, blockIdx, cs.keyword, cs.alias);
}

// Live line of kw, else restore its disabled line, else insert the schema default.
void EnsureLine(EditorShell& s, int bi, const char* kw, const char* alias = nullptr)
{
	if (FindLiveKw(s, bi, kw, alias) >= 0) return;
	const int dis = FindDisabledKw(s, bi, kw, alias);
	if (dis >= 0) { s.doc.RestoreLine(dis); return; }
	const CardSchema* cs = FilterSchemaFind(kw);
	FilterLine dl = ParseFilterLine(cs ? cs->defaultLine : kw);
	s.doc.InsertLine(bi, dl.keyword, dl.op, dl.values);
}

// Disable every live line of kw (and alias). Returns lines disabled.
int DisableAll(EditorShell& s, int bi, const char* kw, const char* alias = nullptr)
{
	int n = 0;
	for (;;) {
		const int li = FindLiveKw(s, bi, kw, alias);
		if (li < 0) break;
		s.doc.CommentOutLine(li);
		n++;
	}
	return n;
}

ImU32 SmallPxFontCol() { return Tok::TextMuted; }
ImFont* SmallF() { const PobUi::WidgetFonts& wf = PobUi::Fonts(); return wf.small ? wf.small : ImGui::GetFont(); }

// ---- card rows ------------------------------------------------------------------
//
// A row: label column (130 design px) | widgets | a right-aligned ghost button
// (停用 / 恢復). The widgets wrap at `g_rowRight`.

float g_rowRight = 0.0f;

struct Row {
	float cardX = 0, cardW = 0, x = 0, w = 0, y = 0, btnW = 0;
	const char* btn = nullptr;
};

Row RowStart(const char* label, const char* tip, const char* btn, bool first, bool dim)
{
	Row r;
	r.x = PobUi::CardInnerX();
	r.w = PobUi::CardInnerWidth();
	const float padX = r.x - (ImGui::GetCursorScreenPos().x);
	(void)padX;
	r.cardX = ImGui::GetCursorScreenPos().x;
	r.cardW = r.w + (r.x - r.cardX) * 2.0f;
	const ImVec2 top = ImGui::GetCursorScreenPos();
	ImDrawList* dl = ImGui::GetWindowDrawList();
	if (!first) dl->AddLine(ImVec2(r.cardX + 1.0f, top.y), ImVec2(r.cardX + r.cardW - 1.0f, top.y), Tok::BorderSubtle, 1.0f);
	r.y = top.y + std::floor(PobUi::D(7.0f));
	r.btn = btn;
	r.btnW = btn ? PobUi::ButtonWidth(btn, PobUi::BtnSize::Sm) : 0.0f;
	const float H = PobUi::ControlH();
	ImFont* lf = SmallF();
	ImGui::SetCursorScreenPos(ImVec2(r.x, r.y + std::floor((H - lf->FontSize) * 0.5f)));
	ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(dim ? Tok::TextFaint : Tok::TextMuted));
	const float labW = std::floor(PobUi::D(122.0f));
	ImGui::PushFont(lf);
	ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + labW);
	ImGui::TextUnformatted(label);
	ImGui::PopTextWrapPos();
	ImGui::PopFont();
	ImGui::PopStyleColor();
	if (tip && *tip && ImGui::IsItemHovered()) PobUi::Tooltip(tip);
	ImGui::SetCursorScreenPos(ImVec2(r.x + std::floor(PobUi::D(130.0f)), r.y));
	g_rowRight = r.x + r.w - r.btnW - PobUi::D(10.0f);
	ImGui::BeginGroup();
	return r;
}

// Ends the row; true when its button was pressed.
bool RowFinish(const Row& r)
{
	ImGui::EndGroup();
	float bottom = (std::max)(ImGui::GetItemRectMax().y, r.y + PobUi::ControlH());
	bool click = false;
	if (r.btn) {
		const float smH = std::floor(PobUi::D(28.0f));
		ImGui::SetCursorScreenPos(ImVec2(r.x + r.w - r.btnW, r.y + std::floor((PobUi::ControlH() - smH) * 0.5f)));
		click = PobUi::Button(r.btn, PobUi::BtnKind::Ghost, PobUi::BtnSize::Sm);
		if (ImGui::IsItemHovered()) {
			if (std::string(r.btn) == u8"停用")
				PobUi::Tooltip(u8"以 #! 註解停用這一行：檔案裡保留，之後可以恢復");
			else if (std::string(r.btn) == u8"恢復")
				PobUi::Tooltip(u8"把停用的這一行恢復成有效");
		}
		bottom = (std::max)(bottom, ImGui::GetItemRectMax().y);
	}
	ImGui::SetCursorScreenPos(ImVec2(r.cardX, bottom + std::floor(PobUi::D(7.0f))));
	ImGui::Dummy(ImVec2(r.cardW, 0.0f));
	return click;
}

// Fits the next widget into the row: wraps to a new line when it would cross
// g_rowRight.
void Flow(float w, bool first)
{
	if (first) return;
	ImGui::SameLine(0, PobUi::D(8.0f));
	if (ImGui::GetCursorScreenPos().x + w > g_rowRight) ImGui::NewLine();
}

// A toggle chip (EnumMulti values). True on click.
bool ChipToggle(const char* label, bool on, bool enabled = true)
{
	ImFont* f = SmallF();
	const float px = f->FontSize;
	const ImVec2 ts = f->CalcTextSizeA(px, FLT_MAX, 0.0f, label);
	const float h = std::floor(PobUi::D(24.0f));
	const ImVec2 sz(ts.x + PobUi::D(16.0f), h);
	const float boxH = PobUi::ControlH();
	ImVec2 p = ImGui::GetCursorScreenPos();
	ImGui::BeginDisabled(!enabled);
	const bool click = ImGui::InvisibleButton(label, ImVec2(sz.x, boxH));
	const bool hov = ImGui::IsItemHovered();
	ImGui::EndDisabled();
	p.y += std::floor((boxH - h) * 0.5f);
	ImDrawList* dl = ImGui::GetWindowDrawList();
	dl->AddRectFilled(p, p + sz, on ? Tok::AccentSoft : (hov ? Tok::Surface3 : Tok::Surface2), h * 0.5f);
	dl->AddRect(p, p + sz, on ? Tok::Accent : Tok::Border, h * 0.5f, 0, 1.0f);
	dl->AddText(f, px, ImVec2(p.x + PobUi::D(8.0f), p.y + std::floor((h - ts.y) * 0.5f)), on ? Tok::Text : Tok::TextMuted, label);
	return click && enabled;
}

// A value chip with a remove "×". Returns true when × was pressed.
bool ValueChip(const char* id, const std::string& label, const char* tip, bool removable)
{
	ImFont* f = ImGui::GetFont();
	const float px = ImGui::GetFontSize() * 0.86f;
	const ImVec2 ts = f->CalcTextSizeA(px, FLT_MAX, 0.0f, label.c_str());
	const ImVec2 xs = f->CalcTextSizeA(px, FLT_MAX, 0.0f, u8"×");
	const float h = std::floor(PobUi::D(24.0f));
	const float w = ts.x + PobUi::D(10.0f) + (removable ? xs.x + PobUi::D(14.0f) : PobUi::D(10.0f));
	const float boxH = PobUi::ControlH();
	const ImVec2 top = ImGui::GetCursorScreenPos();
	ImGui::PushID(id);
	ImDrawList* dl = ImGui::GetWindowDrawList();
	const ImVec2 p(top.x, top.y + std::floor((boxH - h) * 0.5f));
	bool removed = false, xh = false;
	ImVec2 xp(0, 0);
	if (removable) {
		xp = ImVec2(p.x + w - xs.x - PobUi::D(8.0f), p.y + std::floor((h - xs.y) * 0.5f));
		ImGui::SetCursorScreenPos(ImVec2(xp.x - PobUi::D(4.0f), top.y));
		removed = ImGui::InvisibleButton("##x", ImVec2(xs.x + PobUi::D(10.0f), boxH));
		xh = ImGui::IsItemHovered();
		ImGui::SetCursorScreenPos(top);
	}
	// the whole chip as the row's last item, so SameLine continues after it
	ImGui::Dummy(ImVec2(w, boxH));
	const bool hov = ImGui::IsItemHovered();
	dl->AddRectFilled(p, p + ImVec2(w, h), Tok::Surface3, h * 0.5f);
	dl->AddText(f, px, ImVec2(p.x + PobUi::D(10.0f), p.y + std::floor((h - ts.y) * 0.5f)), Tok::Text, label.c_str());
	if (removable) {
		dl->AddText(f, px, xp, xh ? Tok::Danger : Tok::TextFaint, u8"×");
		if (xh) { PobUi::Tooltip(u8"移除"); ImGui::SetMouseCursor(ImGuiMouseCursor_Hand); }
	}
	if (hov && !xh && tip) PobUi::Tooltip(tip);
	ImGui::PopID();
	return removed;
}

// "已修改" pill after a row's widgets (phase 2 adds the baseline value).
void ModPill(const std::string& text)
{
	ImFont* f = SmallF();
	const float px = f->FontSize;
	const ImVec2 ts = f->CalcTextSizeA(px, FLT_MAX, 0.0f, text.c_str());
	const float h = std::floor(px + PobUi::D(3.0f));
	const float boxH = PobUi::ControlH();
	const ImVec2 sz(ts.x + PobUi::D(12.0f), h);
	Flow(sz.x, false);
	ImVec2 p = ImGui::GetCursorScreenPos();
	ImGui::Dummy(ImVec2(sz.x, boxH));
	p.y += std::floor((boxH - h) * 0.5f);
	ImDrawList* dl = ImGui::GetWindowDrawList();
	dl->AddRectFilled(p, p + sz, Tok::AccentSoft, h * 0.5f);
	dl->AddText(f, px, ImVec2(p.x + PobUi::D(6.0f), p.y + std::floor((h - ts.y) * 0.5f)), Tok::AccentText, text.c_str());
}

// Operator Select. StringList cards speak 包含/絕對等於 (partial vs exact match).
const char* OpLabel(const char* op, bool stringList)
{
	if (stringList) {
		if (!op[0]) return u8"包含";
		if (op[0] == '=') return u8"絕對等於";   // "=" / "=="
		if (op[0] == '!') return u8"不等於";
		return op;
	}
	if (!op[0]) return "=";
	return op;
}

bool OpSelect(FilterLine& ln, const CardSchema& cs)
{
	if (cs.ops.empty()) return false;
	const bool stringList = (cs.kind == CardKind::StringList);
	std::vector<const char*> labels;
	int sel = 0;
	for (size_t i = 0; i < cs.ops.size(); i++) {
		labels.push_back(OpLabel(cs.ops[i], stringList));
		if (ln.op == cs.ops[i]) sel = (int)i;
	}
	const float w = (std::max)(std::floor(PobUi::D(stringList ? 96.0f : 70.0f)),
	                           PobUi::SelectFitWidth(labels.data(), (int)labels.size()));
	Flow(w, true);
	if (PobUi::Select("##op", &sel, labels.data(), nullptr, (int)labels.size(), w) && ln.op != cs.ops[sel]) {
		ln.op = cs.ops[sel];
		ln.dirty = true;
		return true;
	}
	return false;
}

// A compact integer field (no +/- buttons).
bool IntField(const char* id, int* v, float designW)
{
	PobUi::PushControlFrame();
	ImGui::SetNextItemWidth(std::floor(PobUi::D(designW)));
	const bool ch = ImGui::InputInt(id, v, 0, 0);
	PobUi::PopControlFrame();
	return ch;
}

// ---- generic widgets per CardKind ------------------------------------------

void DrawBoolWidget(EditorShell& s, FilterLine& ln)
{
	const bool isTrue = ln.values.empty() || ln.values[0].text != "False";
	int v = isTrue ? 0 : 1;
	const char* items[2] = { u8"是", u8"否" };
	if (PobUi::Segmented("##bool", &v, items, 2)) {
		FilterSetValueStr(ln, 0, v == 0 ? "True" : "False", false);
		MarkEdited(s);
	}
}

void DrawIntOpWidget(EditorShell& s, FilterLine& ln, const CardSchema& cs)
{
	if (OpSelect(ln, cs)) MarkEdited(s);
	int v = FilterValueInt(ln, 0, cs.minInt);
	ImGui::SameLine(0, PobUi::D(8.0f));
	if (IntField("##val", &v, 72.0f)) {
		if (v < cs.minInt) v = cs.minInt;
		if (cs.maxInt > cs.minInt && v > cs.maxInt) v = cs.maxInt;
		FilterSetValueInt(ln, 0, v);
		MarkEdited(s);
	}
}

void DrawIntRangeWidget(EditorShell& s, FilterLine& ln, const CardSchema& cs)
{
	int v = FilterValueInt(ln, 0, cs.maxInt);
	if (IntField("##val", &v, 72.0f)) {
		v = std::clamp(v, cs.minInt, cs.maxInt);
		FilterSetValueInt(ln, 0, v);
		MarkEdited(s);
	}
}

void DrawEnumOpWidget(EditorShell& s, FilterLine& ln, const CardSchema& cs)
{
	if (OpSelect(ln, cs)) MarkEdited(s);
	const std::string cur = ln.values.empty() ? std::string() : ln.values[0].text;
	std::vector<const char*> labels;
	int sel = -1;
	for (size_t i = 0; i < cs.enums.size(); i++) {
		labels.push_back(cs.enums[i].zh);
		if (cur == cs.enums[i].token) sel = (int)i;
	}
	const float w = (std::max)(std::floor(PobUi::D(110.0f)), PobUi::SelectFitWidth(labels.data(), (int)labels.size()));
	ImGui::SameLine(0, PobUi::D(8.0f));
	if (PobUi::Select("##enum", &sel, labels.data(), nullptr, (int)labels.size(), w) && sel >= 0) {
		FilterSetValueStr(ln, 0, cs.enums[sel].token, false);
		MarkEdited(s);
	}
}

void DrawEnumMultiWidget(EditorShell& s, FilterLine& ln, const CardSchema& cs)
{
	int nSet = 0;
	for (const SchemaEnumValue& e : cs.enums)
		if (FilterHasValue(ln, e.token)) nSet++;
	bool first = true;
	for (const SchemaEnumValue& e : cs.enums) {
		const bool has = FilterHasValue(ln, e.token);
		ImFont* f = SmallF();
		Flow(f->CalcTextSizeA(f->FontSize, FLT_MAX, 0.0f, e.zh).x + PobUi::D(16.0f), first);
		first = false;
		// Keep at least one value: an empty value list is invalid filter syntax.
		if (ChipToggle(e.zh, has, !(has && nSet <= 1))) {
			if (!has) FilterAddValue(ln, e.token, false);
			else FilterRemoveValue(ln, e.token);
			MarkEdited(s);
		}
	}
}

// StringList chips + a text input that resolves Chinese through the catalog.
// chipStart skips a leading count value (ModList).
bool DrawChipsAndInput(EditorShell& s, FilterLine& ln, size_t chipStart, bool translateInput, bool first)
{
	CardUiState& u = UI(s);
	bool changed = false;

	int removeIdx = -1;
	for (size_t i = chipStart; i < ln.values.size(); i++) {
		std::string label = ln.values[i].text;
		if (ln.keyword == "Class") { std::string z = s.i18n.ClassNameZh(label); if (z != label) label = z; }
		if (ln.keyword == "BaseType") { std::string z = s.i18n.DisplayName(label); if (z != label) label = z; }
		ImFont* f = ImGui::GetFont();
		const float w = f->CalcTextSizeA(ImGui::GetFontSize() * 0.86f, FLT_MAX, 0.0f, label.c_str()).x + PobUi::D(34.0f);
		Flow(w, first && i == chipStart);
		const std::string cid = "##chip" + std::to_string(i);
		// keep >= 1: an empty value list is invalid filter syntax
		if (ValueChip(cid.c_str(), label, ln.values[i].text.c_str(), ln.values.size() - chipStart > 1)) removeIdx = (int)i;
	}
	if (removeIdx >= 0) {
		ln.values.erase(ln.values.begin() + removeIdx);
		ln.dirty = true;
		MarkEdited(s);
		changed = true;
	}

	// the input (one line at a time keeps its text)
	if (u.inputLine != (const void*)&ln) {
		u.input.clear();
		u.live.clear();
		u.liveFor.clear();
		u.notFound = false;
		u.inputLine = &ln;
	}
	const float inW = std::floor(PobUi::D(200.0f));
	Flow(inW, false);
	PobUi::PushControlFrame();
	ImGui::SetNextItemWidth(inW);
	const bool enter = ImGui::InputTextWithHint("##manual", u8"輸入物品名稱，可中文", &u.input,
	                                            ImGuiInputTextFlags_EnterReturnsTrue);
	PobUi::PopControlFrame();
	if (ImGui::IsItemEdited()) u.notFound = false;

	const bool classCard = (ln.keyword == "Class");
	auto addToken = [&](const std::string& en) {
		if (!FilterHasValue(ln, en)) {
			FilterAddValue(ln, en, true);
			MarkEdited(s);
			changed = true;
		}
		u.input.clear();
		u.liveFor.clear();
		u.live.clear();
		u.notFound = false;
	};

	// live candidates (Chinese or English substring)
	if (translateInput && !u.input.empty() && (u.liveFor != u.input || u.liveClass != classCard)) {
		u.liveFor = u.input;
		u.liveClass = classCard;
		u.live.clear();
		const std::string lower = EdToLowerAscii(u.input);
		if (classCard) {
			std::vector<std::string> seen;
			for (const LibItem& it : s.library.items()) {
				if (it.enClass.empty()) continue;
				if (std::find(seen.begin(), seen.end(), it.enClass) != seen.end()) continue;
				seen.push_back(it.enClass);
				const std::string zh = s.i18n.ClassNameZh(it.enClass);
				if (EdContainsCI(it.enClass, lower) || zh.find(u.input) != std::string::npos) {
					LibItem cand;
					cand.en = it.enClass;
					cand.zh = zh;
					u.live.push_back(std::move(cand));
				}
			}
		} else {
			for (const LibItem& it : s.library.items()) {
				if (EdContainsCI(it.en, lower) || it.zh.find(u.input) != std::string::npos) {
					u.live.push_back(it);
					if (u.live.size() >= 40) break;
				}
			}
		}
	}

	if (enter && !u.input.empty()) {
		std::string token;
		if (translateInput) {
			for (const LibItem& it : u.live)
				if (it.en == u.input || it.zh == u.input) { token = it.en; break; }
			if (token.empty() && u.live.size() == 1) token = u.live[0].en;
			if (token.empty() && !u.live.empty()) token = u.live[0].en;   // the highlighted first row
		} else {
			token = u.input;
		}
		if (!token.empty()) addToken(token);
		else u.notFound = true;
	}

	if (translateInput && !u.input.empty() && !u.live.empty()) {
		// the candidate menu, under the input
		ImGui::NewLine();
		ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, ImGui::GetCursorScreenPos().y - PobUi::D(4.0f)));
		const float rowH = std::floor(ImGui::GetTextLineHeight() + PobUi::D(8.0f));
		const float listW = (std::min)(std::floor(PobUi::D(420.0f)), g_rowRight - ImGui::GetCursorScreenPos().x);
		const float listH = (std::min)((float)u.live.size(), 6.5f) * rowH + PobUi::D(8.0f);
		ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(Tok::SurfaceRaised));
		ImGui::PushStyleColor(ImGuiCol_Border, ImGui::ColorConvertU32ToFloat4(Tok::Border));
		ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, PobUi::D(8.0f));
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(PobUi::D(4.0f), PobUi::D(4.0f)));
		ImGui::BeginChild("##livecands", ImVec2(listW, listH), true);
		ImDrawList* dl = ImGui::GetWindowDrawList();
		ImFont* sf = SmallF();
		std::string picked;
		for (size_t i = 0; i < u.live.size(); i++) {
			const LibItem& it = u.live[i];
			ImGui::PushID((int)i);
			const ImVec2 rp = ImGui::GetCursorScreenPos();
			const float rw = ImGui::GetContentRegionAvail().x;
			const bool click = ImGui::InvisibleButton("##cand", ImVec2(rw, rowH));
			const bool hov = ImGui::IsItemHovered();
			if (hov || i == 0) dl->AddRectFilled(rp, rp + ImVec2(rw, rowH), hov ? Tok::Surface3 : Tok::AccentSoft, PobUi::D(5.0f));
			const float ty = rp.y + std::floor((rowH - ImGui::GetTextLineHeight()) * 0.5f);
			float x = rp.x + PobUi::D(8.0f);
			const std::string zh = it.zh.empty() ? it.en : it.zh;
			dl->AddText(ImVec2(x, ty), Tok::Text, zh.c_str());
			x += ImGui::CalcTextSize(zh.c_str()).x + PobUi::D(8.0f);
			if (it.zh != it.en)
				dl->AddText(sf, sf->FontSize, ImVec2(x, ty + (ImGui::GetTextLineHeight() - sf->FontSize) * 0.5f), Tok::TextMuted, it.en.c_str());
			// the class, so two items of one name tell apart
			std::string cls = classCard ? std::string() : s.i18n.ClassNameZh(it.enClass);
			if (i == 0 && cls.empty()) cls = u8"Enter 加入";
			if (!cls.empty()) {
				const ImVec2 cs = sf->CalcTextSizeA(sf->FontSize, FLT_MAX, 0.0f, cls.c_str());
				const ImVec2 bp(rp.x + rw - cs.x - PobUi::D(16.0f), rp.y + std::floor((rowH - cs.y - PobUi::D(4.0f)) * 0.5f));
				if (!(i == 0 && cls == u8"Enter 加入"))
					dl->AddRectFilled(bp, bp + ImVec2(cs.x + PobUi::D(8.0f), cs.y + PobUi::D(4.0f)), Tok::Surface2, PobUi::D(4.0f));
				dl->AddText(sf, sf->FontSize, bp + ImVec2(PobUi::D(4.0f), PobUi::D(2.0f)), Tok::TextMuted, cls.c_str());
			}
			if (click) picked = it.en;
			ImGui::PopID();
		}
		ImGui::EndChild();
		ImGui::PopStyleVar(2);
		ImGui::PopStyleColor(2);
		if (!picked.empty()) addToken(picked);
	} else if (translateInput && !u.input.empty() && u.live.empty()) {
		ImGui::NewLine();
		PobUi::Hint(u8"遊戲內沒有這個名稱。過濾器只認英文名稱，用原文加入會比對不到物品。", g_rowRight - ImGui::GetCursorScreenPos().x);
		if (PobUi::Button(u8"仍以原文加入", PobUi::BtnKind::Secondary, PobUi::BtnSize::Sm)) addToken(u.input);
	}
	return changed;
}

void DrawStringListWidget(EditorShell& s, FilterLine& ln, const CardSchema& cs)
{
	bool first = true;
	if (!cs.ops.empty()) { if (OpSelect(ln, cs)) MarkEdited(s); first = false; }
	// Only BaseType / Class get catalog translation; mod-name lists stay raw.
	const bool translate = (ln.keyword == "BaseType" || ln.keyword == "Class");
	DrawChipsAndInput(s, ln, 0, translate, first);
}

// Sockets / SocketGroup token: optional count digit + colour letters ("5GGG").
void DrawSocketSpecWidget(EditorShell& s, FilterLine& ln, const CardSchema& cs)
{
	if (OpSelect(ln, cs)) MarkEdited(s);

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
	Flow(PobUi::D(110.0f), false);
	PobUi::Hint(u8"孔數");
	ImGui::SameLine(0, PobUi::D(4.0f));
	if (IntField("##num", &numUi, 48.0f)) { num = std::clamp(numUi, 0, 6); ch = true; }
	static const char* kColorZh[6] = { u8"紅", u8"綠", u8"藍", u8"白", u8"深淵", u8"掘獄" };
	for (int c = 0; c < 6; c++) {
		Flow(PobUi::D(90.0f), false);
		ImGui::PushID(c);
		PobUi::Hint(kColorZh[c]);
		ImGui::SameLine(0, PobUi::D(4.0f));
		int v = cnt[c];
		if (IntField("##c", &v, 44.0f)) { cnt[c] = std::clamp(v, 0, 6); ch = true; }
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
	if (OpSelect(ln, cs)) MarkEdited(s);
	bool hasCount = false;
	int count = 0;
	if (!ln.values.empty() && !ln.values[0].quoted && !ln.values[0].text.empty()) {
		hasCount = true;
		for (char c : ln.values[0].text)
			if (c < '0' || c > '9') { hasCount = false; break; }
		if (hasCount) count = FilterValueInt(ln, 0, 0);
	}
	int ui = hasCount ? count : 0;
	Flow(PobUi::D(110.0f), false);
	PobUi::Hint(u8"至少");
	ImGui::SameLine(0, PobUi::D(4.0f));
	if (IntField("##cnt", &ui, 48.0f)) {
		ui = std::clamp(ui, 0, 6);
		if (ui > 0 && hasCount) FilterSetValueInt(ln, 0, ui);
		else if (ui > 0 && !hasCount) { ln.values.insert(ln.values.begin(), FilterToken{ std::to_string(ui), false }); ln.dirty = true; }
		else if (ui == 0 && hasCount) { ln.values.erase(ln.values.begin()); ln.dirty = true; }
		MarkEdited(s);
		hasCount = ui > 0;
	}
	if (ImGui::IsItemHovered()) PobUi::Tooltip(u8"0 = 不限；搭配 >= 2 表示至少 2 條相符詞綴");
	DrawChipsAndInput(s, ln, hasCount ? 1 : 0, false, false);
}

// Lines without a schema card render read-only.
void DrawRawFallbackWidget(const FilterLine& ln)
{
	PobUi::Numeric(LineEn(ln).c_str());
}

// Opens a popover for the selected block (submitted by DrawCardPopovers).
void RequestPop(EditorShell& s, CardPop pop, int chan = 0)
{
	CardUiState& u = UI(s);
	u.pop = pop;
	u.openReq = true;
	u.colorChan = chan;
	u.rgbaFor = -1;
	u.anchor = ImVec2(ImGui::GetItemRectMin().x, ImGui::GetItemRectMax().y + PobUi::D(4.0f));
}

// A clickable value summary (opens a popover).
bool SummaryLink(const char* text)
{
	const float boxH = PobUi::ControlH();
	const ImVec2 p = ImGui::GetCursorScreenPos();
	const ImVec2 ts = ImGui::CalcTextSize(text);
	const bool click = ImGui::InvisibleButton(text, ImVec2(ts.x, boxH));
	const bool hov = ImGui::IsItemHovered();
	ImDrawList* dl = ImGui::GetWindowDrawList();
	const float ty = p.y + std::floor((boxH - ts.y) * 0.5f);
	dl->AddText(ImVec2(p.x, ty), hov ? Tok::AccentText : Tok::Text, text);
	if (hov) {
		dl->AddLine(ImVec2(p.x, ty + ts.y), ImVec2(p.x + ts.x, ty + ts.y), Tok::AccentText, 1.0f);
		ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
		PobUi::Tooltip(u8"點一下編輯");
	}
	return click;
}

void PlayCustom(EditorShell& s, const FilterLine& ln)
{
	if (ln.values.empty()) return;
	std::wstring file = EdWiden(ln.values[0].text);
	if (!s.soundsInit) {
		s.sounds.Init(s.exeDir);
		s.soundsInit = true;
	}
	const std::wstring path = (file.find(L':') != std::wstring::npos) ? file : (s.sounds.folder() + L"\\" + file);
	const int vol = ln.values.size() > 1 ? std::clamp(FilterValueInt(ln, 1, 100), 0, 300) : 100;
	PlayAudioFileVol(path, std::clamp(vol / 3, 0, 100));
}

// Dispatch one live condition-ish line's widgets.
void DrawCardWidgets(EditorShell& s, FilterLine& ln, const CardSchema& cs)
{
	switch (cs.kind) {
		case CardKind::Toggle:     PobUi::Hint(u8"已啟用"); break;
		case CardKind::Bool:       DrawBoolWidget(s, ln); break;
		case CardKind::IntOp:      DrawIntOpWidget(s, ln, cs); break;
		case CardKind::IntRange:   DrawIntRangeWidget(s, ln, cs); break;
		case CardKind::EnumOp:     DrawEnumOpWidget(s, ln, cs); break;
		case CardKind::EnumMulti:  DrawEnumMultiWidget(s, ln, cs); break;
		case CardKind::StringList: DrawStringListWidget(s, ln, cs); break;
		case CardKind::ModList:    DrawModListWidget(s, ln, cs); break;
		case CardKind::SocketSpec: DrawSocketSpecWidget(s, ln, cs); break;
		case CardKind::Color: {
			int r, g, b, a;
			bool ha;
			FilterGetColor(ln, r, g, b, a, ha);
			const unsigned char c[4] = { (unsigned char)r, (unsigned char)g, (unsigned char)b, (unsigned char)a };
			int chan = 0;
			for (int k = 0; k < 3; k++) if (ln.keyword == kColorKw[k]) chan = k;
			ImGui::SetCursorScreenPos(ImGui::GetCursorScreenPos() + ImVec2(0, std::floor((PobUi::ControlH() - PobUi::D(22.0f)) * 0.5f)));
			if (EdSwatch("##sw", c, c, 22.0f, true)) RequestPop(s, CardPop::Color, chan);
			ImGui::SameLine(0, PobUi::D(8.0f));
			char buf[48];
			std::snprintf(buf, sizeof(buf), "%d %d %d %d", r, g, b, a);
			PobUi::Numeric(buf, Tok::Text);
			break;
		}
		case CardKind::SoundBuiltin:
		case CardKind::SoundCustom: {
			const std::string sum = CardSoundSummary(ln);
			if (SummaryLink(sum.c_str())) RequestPop(s, CardPop::Sound);
			if (cs.kind == CardKind::SoundCustom) {
				ImGui::SameLine(0, PobUi::D(10.0f));
				if (PobUi::Button(u8"試聽", PobUi::BtnKind::Secondary, PobUi::BtnSize::Sm, PobIcon::Play)) PlayCustom(s, ln);
			}
			break;
		}
		case CardKind::MinimapIcon: {
			const std::string sum = CardMinimapSummary(ln);
			if (SummaryLink(sum.c_str())) RequestPop(s, CardPop::Minimap);
			break;
		}
		case CardKind::PlayEffect: {
			const std::string sum = CardEffectSummary(ln);
			if (SummaryLink(sum.c_str())) RequestPop(s, CardPop::Effect);
			break;
		}
		default: DrawRawFallbackWidget(ln); break;
	}
}

bool IsLookKw(const std::string& kw)
{
	return kw == "SetTextColor" || kw == "SetBorderColor" || kw == "SetBackgroundColor" || kw == "SetFontSize";
}

} // namespace

// ---- middle pane ------------------------------------------------------------

bool DrawBlockCards(EditorShell& s, int blockIdx)
{
	if (blockIdx < 0 || blockIdx >= (int)s.model.blocks.size()) return false;
	FilterBlock& b = s.model.blocks[blockIdx];

	// Collect the block's rows, conditions first, keeping file order inside
	// each section. Disabled lines render with a 恢復 button.
	struct CRow { int li; const CardSchema* cs; bool disabled; FilterLine parsed; };
	std::vector<CRow> conds, acts;
	for (int li : b.lineIdx) {
		const FilterLine& ln = s.model.lines[li];
		if (ln.kind == FilterLineKind::Condition || ln.kind == FilterLineKind::Action ||
		    ln.kind == FilterLineKind::Unknown) {
			CRow r{ li, FilterSchemaFind(ln.keyword), false, {} };
			const bool isAct = (ln.kind == FilterLineKind::Action) || (r.cs && r.cs->isAction);
			(isAct ? acts : conds).push_back(std::move(r));
		} else if (ln.kind == FilterLineKind::Comment) {
			CRow r{ li, nullptr, true, {} };
			if (!s.doc.IsDisabledLine(li, &r.parsed)) continue;
			if (r.parsed.kind == FilterLineKind::BlockHeader) continue;
			r.cs = FilterSchemaFind(r.parsed.keyword);
			const bool isAct = (r.parsed.kind == FilterLineKind::Action) || (r.cs && r.cs->isAction);
			(isAct ? acts : conds).push_back(std::move(r));
		}
	}

	bool mutated = false;
	auto drawRow = [&](CRow& r, bool first) {
		if (r.disabled) {
			const std::string title = FilterSchemaKeywordZh(r.parsed.keyword);
			ImGui::PushID(r.li);
			Row row = RowStart(title.c_str(), nullptr, u8"恢復", first, true);
			const std::string v = u8"已停用：" + CardValueZh(r.parsed, s.i18n);
			const float ty = std::floor((PobUi::ControlH() - ImGui::GetTextLineHeight()) * 0.5f);
			ImGui::SetCursorScreenPos(ImGui::GetCursorScreenPos() + ImVec2(0, ty));
			PobUi::Hint(v.c_str());
			if (ImGui::IsItemHovered()) PobUi::Tooltip(LineEn(r.parsed).c_str());
			const bool restore = RowFinish(row);
			ImGui::PopID();
			if (restore) { s.doc.RestoreLine(r.li); mutated = true; }
			return;
		}
		FilterLine& ln = s.model.lines[r.li];
		const std::string title = FilterSchemaKeywordZh(ln.keyword);
		const std::string en = LineEn(ln);
		const char* tip = (r.cs && r.cs->tooltip) ? r.cs->tooltip : en.c_str();
		ImGui::PushID(r.li);
		Row row = RowStart(title.c_str(), tip, u8"停用", first, false);
		if (r.cs) DrawCardWidgets(s, ln, *r.cs);
		else DrawRawFallbackWidget(ln);
		if (ln.dirty) ModPill(u8"已修改");
		const bool disable = RowFinish(row);
		ImGui::PopID();
		if (disable) { s.doc.CommentOutLine(r.li); mutated = true; }
	};

	// ---- 條件 ----
	PobUi::CardBegin("##condcard", nullptr, u8"條件", u8"全部符合才套用", false);
	if (conds.empty()) {
		ImGui::SetCursorScreenPos(ImVec2(PobUi::CardInnerX(), ImGui::GetCursorScreenPos().y + PobUi::D(10.0f)));
		PobUi::Hint(u8"沒有條件：這條規則會套用到所有物品。從右側加入條件。");
		ImGui::Dummy(ImVec2(0, PobUi::D(6.0f)));
	}
	for (size_t i = 0; i < conds.size() && !mutated; i++) drawRow(conds[i], i == 0);
	PobUi::CardEnd();
	if (mutated) return true;
	ImGui::Dummy(ImVec2(0, PobUi::D(6.0f)));

	// ---- 外觀與音效 ----
	PobUi::CardBegin("##actcard", nullptr, u8"外觀與音效", nullptr, false);
	bool first = true;
	// the look row: text / border / background swatches + font size
	{
		int live[4], dis[4];
		bool anyLive = false, anyDis = false;
		const char* lookKw[4] = { "SetTextColor", "SetBorderColor", "SetBackgroundColor", "SetFontSize" };
		for (int k = 0; k < 4; k++) {
			live[k] = FindLiveKw(s, blockIdx, lookKw[k]);
			dis[k] = live[k] < 0 ? FindDisabledKw(s, blockIdx, lookKw[k]) : -1;
			anyLive |= live[k] >= 0;
			anyDis |= dis[k] >= 0;
		}
		if (anyLive || anyDis) {
			Row row = RowStart(u8"文字 / 邊框 / 背景", u8"點色塊改顏色；字級在色塊旁", anyLive ? u8"停用" : u8"恢復", first, !anyLive);
			first = false;
			if (!anyLive) {
				ImGui::SetCursorScreenPos(ImGui::GetCursorScreenPos() + ImVec2(0, std::floor((PobUi::ControlH() - ImGui::GetTextLineHeight()) * 0.5f)));
				PobUi::Hint(u8"已停用：顏色與字級");
			} else {
				const float sw = std::floor(PobUi::D(22.0f));
				const ImVec2 base = ImGui::GetCursorScreenPos();
				for (int k = 0; k < 3; k++) {
					ImGui::SetCursorScreenPos(ImVec2(base.x + (sw + PobUi::D(6.0f)) * k, base.y + std::floor((PobUi::ControlH() - sw) * 0.5f)));
					ImGui::PushID(k);
					unsigned char c[4] = { 0, 0, 0, 0 };
					if (live[k] >= 0) {
						int r, g, bb, a;
						bool ha;
						FilterGetColor(s.model.lines[live[k]], r, g, bb, a, ha);
						c[0] = (unsigned char)r; c[1] = (unsigned char)g; c[2] = (unsigned char)bb; c[3] = (unsigned char)a;
					}
					static const char* kTips[3] = { u8"文字顏色", u8"邊框顏色", u8"背景顏色" };
					if (EdSwatch("##sw", c, c, 22.0f, true, live[k] >= 0, live[k] >= 0)) RequestPop(s, CardPop::Color, k);
					if (ImGui::IsItemHovered())
						PobUi::Tooltip(live[k] >= 0 ? kTips[k] : (std::string(kTips[k]) + u8"：沒有設定（點一下加入）").c_str());
					ImGui::PopID();
				}
				ImGui::SetCursorScreenPos(ImVec2(base.x + (sw + PobUi::D(6.0f)) * 3 + PobUi::D(4.0f),
				                                 base.y + std::floor((PobUi::ControlH() - SmallF()->FontSize) * 0.5f)));
				const std::string fs = live[3] >= 0
					? u8"字級 " + std::to_string(FilterValueInt(s.model.lines[live[3]], 0, 32)) : std::string(u8"字級 預設");
				PobUi::Hint(fs.c_str());
				bool lookDirty = false;
				for (int k = 0; k < 4; k++) if (live[k] >= 0 && s.model.lines[live[k]].dirty) lookDirty = true;
				ImGui::SetCursorScreenPos(base);
				ImGui::Dummy(ImVec2((sw + PobUi::D(6.0f)) * 3 + PobUi::D(80.0f), PobUi::ControlH()));
				if (lookDirty) ModPill(u8"已修改");
			}
			const bool btn = RowFinish(row);
			if (btn) {
				if (anyLive) { for (int k = 0; k < 4; k++) DisableAll(s, blockIdx, lookKw[k]); }
				else { for (int k = 0; k < 4; k++) { const int d = FindDisabledKw(s, blockIdx, lookKw[k]); if (d >= 0) s.doc.RestoreLine(d); } }
				PobUi::CardEnd();
				return true;
			}
		}
	}
	for (size_t i = 0; i < acts.size() && !mutated; i++) {
		const std::string& kw = acts[i].disabled ? acts[i].parsed.keyword : s.model.lines[acts[i].li].keyword;
		if (IsLookKw(kw)) continue;   // in the look row
		drawRow(acts[i], first);
		first = false;
	}
	if (first) {
		ImGui::SetCursorScreenPos(ImVec2(PobUi::CardInnerX(), ImGui::GetCursorScreenPos().y + PobUi::D(10.0f)));
		PobUi::Hint(u8"沒有外觀設定：遊戲用預設樣式顯示。從右側加入顏色、音效或光柱。");
		ImGui::Dummy(ImVec2(0, PobUi::D(6.0f)));
	}
	PobUi::CardEnd();
	return mutated;
}

// ---- right add-column -------------------------------------------------------

bool DrawAddColumn(EditorShell& s, int blockIdx)
{
	const float w = ImGui::GetContentRegionAvail().x;
	PobUi::SearchField("##addsearch", s.addSearchBuf, (int)sizeof(s.addSearchBuf), u8"搜尋可加入的項目", w);
	if (blockIdx < 0 || blockIdx >= (int)s.model.blocks.size()) {
		ImGui::Dummy(ImVec2(0, PobUi::D(8.0f)));
		PobUi::Hint(u8"先在左側選一條規則", w);
		return false;
	}
	const std::string q = s.addSearchBuf;
	const std::string ql = EdToLowerAscii(q);
	ImDrawList* dl = ImGui::GetWindowDrawList();
	ImFont* sf = SmallF();
	const float rowH = std::floor(PobUi::D(26.0f));
	const PobUi::WidgetFonts& wf = PobUi::Fonts();
	for (const char* grp : FilterSchemaGroups()) {
		bool headed = false;
		for (const CardSchema& cs : FilterSchemaAll()) {
			if (cs.group != grp) continue;
			if (!q.empty() && std::string(cs.zh).find(q) == std::string::npos && !EdContainsCI(cs.keyword, ql)) continue;
			if (!headed) {
				const ImVec2 hp = ImGui::GetCursorScreenPos();
				ImGui::Dummy(ImVec2(w, sf->FontSize + PobUi::D(10.0f)));
				dl->AddText(sf, sf->FontSize, ImVec2(hp.x + PobUi::D(8.0f), hp.y + PobUi::D(8.0f)), Tok::TextMuted, grp);
				headed = true;
			}
			ImGui::PushID(cs.keyword);
			const int live = FindLiveLine(s, blockIdx, cs);
			const bool checked = live >= 0;
			const bool hasDisabled = !checked && FindDisabledLine(s, blockIdx, cs) >= 0;
			const ImVec2 p = ImGui::GetCursorScreenPos();
			const bool click = ImGui::InvisibleButton("##add", ImVec2(w, rowH));
			const bool hov = ImGui::IsItemHovered();
			ImGui::PopID();
			if (hov) {
				dl->AddRectFilled(p, p + ImVec2(w, rowH), Tok::Surface2, PobUi::D(5.0f));
				ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
				std::string tip = checked ? u8"已在這條規則裡：點一下停用（之後可以恢復）"
				                          : (hasDisabled ? u8"已停用：點一下恢復" : u8"點一下加入這條規則");
				if (cs.tooltip) tip = std::string(cs.tooltip) + "\n" + tip;
				PobUi::Tooltip(tip.c_str());
			}
			const float ty = p.y + std::floor((rowH - ImGui::GetTextLineHeight()) * 0.5f);
			dl->AddText(ImVec2(p.x + PobUi::D(8.0f), ty), checked ? Tok::TextMuted : Tok::Text, cs.zh);
			const float ipx = std::floor(PobUi::D(14.0f));
			float ix = p.x + w - ipx - PobUi::D(8.0f);
			if (wf.icons)
				PobUi::IconAt(dl, ImVec2(ix, p.y + std::floor((rowH - ipx) * 0.5f)), checked ? PobIcon::Check : PobIcon::Plus,
				              checked ? Tok::Success : Tok::AccentText, ipx);
			if (hasDisabled) {
				const char* t = u8"已停用";
				const float tw = sf->CalcTextSizeA(sf->FontSize, FLT_MAX, 0.0f, t).x;
				dl->AddText(sf, sf->FontSize, ImVec2(ix - tw - PobUi::D(6.0f), p.y + std::floor((rowH - sf->FontSize) * 0.5f)),
				            Tok::TextFaint, t);
			}
			if (!click) continue;

			if (!checked) {
				// restore a disabled line of this keyword if one exists, otherwise
				// insert the schema's default line
				const int dis = FindDisabledLine(s, blockIdx, cs);
				if (dis >= 0) {
					s.doc.RestoreLine(dis);
				} else {
					FilterLine dline = ParseFilterLine(cs.defaultLine);
					s.doc.InsertLine(blockIdx, dline.keyword, dline.op, dline.values);
				}
				// mutually-exclusive group: disable the other members' live lines
				if (cs.exclusiveGroup > 0) {
					for (const CardSchema& other : FilterSchemaAll()) {
						if (&other == &cs || other.exclusiveGroup != cs.exclusiveGroup) continue;
						for (;;) {
							const int oli = FindLiveLine(s, blockIdx, other);
							if (oli < 0) break;
							s.doc.CommentOutLine(oli);
						}
					}
				}
			} else {
				for (;;) {
					const int oli = FindLiveLine(s, blockIdx, cs);
					if (oli < 0) break;
					s.doc.CommentOutLine(oli);
				}
			}
			return true;
		}
	}
	return false;
}

// ---- shared editors -----------------------------------------------------------

bool CardEffectPalette(const char* id, std::string* color)
{
	ImGui::PushID(id);
	bool changed = false;
	const float d = std::floor(PobUi::D(22.0f));
	ImDrawList* dl = ImGui::GetWindowDrawList();
	for (int i = 0; i < 11; i++) {
		if (i) ImGui::SameLine(0, PobUi::D(5.0f));
		ImGui::PushID(i);
		const ImVec2 p = ImGui::GetCursorScreenPos();
		if (ImGui::InvisibleButton("##c", ImVec2(d, d)) && *color != kEffectColors[i].token) {
			*color = kEffectColors[i].token;
			changed = true;
		}
		const bool hov = ImGui::IsItemHovered();
		if (hov) PobUi::Tooltip(kEffectColors[i].zh);
		const ImVec2 c = p + ImVec2(d * 0.5f, d * 0.5f);
		dl->AddCircleFilled(c, d * 0.5f - PobUi::D(2.0f), EdEffectColor(kEffectColors[i].token), 20);
		if (*color == kEffectColors[i].token) dl->AddCircle(c, d * 0.5f, Tok::Text, 20, 2.0f);
		else if (hov) dl->AddCircle(c, d * 0.5f, Tok::BorderStrong, 20, 1.5f);
		ImGui::PopID();
	}
	ImGui::PopID();
	return changed;
}

bool CardMinimapEditor(const char* id, int* size, std::string* color, std::string* shape)
{
	ImGui::PushID(id);
	bool changed = false;
	const float labW = std::floor(PobUi::D(44.0f));
	auto label = [&](const char* t) {
		const ImVec2 p = ImGui::GetCursorScreenPos();
		ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + std::floor((PobUi::ControlH() - SmallF()->FontSize) * 0.5f)));
		PobUi::Hint(t);
		ImGui::SetCursorScreenPos(ImVec2(p.x + labW, p.y));
	};
	label(u8"大小");
	int sz = std::clamp(*size, 0, 2);
	if (PobUi::Segmented("##size", &sz, kSizeZh, 3)) { *size = sz; changed = true; }
	label(u8"顏色");
	ImGui::SetCursorScreenPos(ImGui::GetCursorScreenPos() + ImVec2(0, std::floor((PobUi::ControlH() - PobUi::D(22.0f)) * 0.5f)));
	if (CardEffectPalette("##pal", color)) changed = true;
	ImGui::Dummy(ImVec2(0, PobUi::D(4.0f)));
	label(u8"形狀");
	const float x0 = ImGui::GetCursorScreenPos().x;
	const float cellW = std::floor(PobUi::D(56.0f)), cellH = std::floor(PobUi::D(30.0f)), g = PobUi::D(6.0f);
	ImDrawList* dl = ImGui::GetWindowDrawList();
	ImFont* sf = SmallF();
	const float y0 = ImGui::GetCursorScreenPos().y;
	for (int i = 0; i < 12; i++) {
		const ImVec2 p(x0 + (cellW + g) * (i % 6), y0 + (cellH + g) * (i / 6));
		ImGui::SetCursorScreenPos(p);
		ImGui::PushID(i);
		const bool click = ImGui::InvisibleButton("##sh", ImVec2(cellW, cellH));
		const bool hov = ImGui::IsItemHovered();
		ImGui::PopID();
		const bool on = *shape == kIconShapes[i].token;
		dl->AddRectFilled(p, p + ImVec2(cellW, cellH), on ? Tok::AccentSoft : (hov ? Tok::Surface3 : Tok::Surface2), PobUi::D(5.0f));
		dl->AddRect(p, p + ImVec2(cellW, cellH), on ? Tok::Accent : Tok::Border, PobUi::D(5.0f), 0, 1.0f);
		const ImVec2 ts = sf->CalcTextSizeA(sf->FontSize, FLT_MAX, 0.0f, kIconShapes[i].zh);
		dl->AddText(sf, sf->FontSize, p + ImVec2(std::floor((cellW - ts.x) * 0.5f), std::floor((cellH - ts.y) * 0.5f)),
		            on ? Tok::Text : Tok::TextMuted, kIconShapes[i].zh);
		if (click && !on) { *shape = kIconShapes[i].token; changed = true; }
	}
	ImGui::SetCursorScreenPos(ImVec2(x0 - labW, y0 + (cellH + g) * 2));
	ImGui::Dummy(ImVec2(labW + (cellW + g) * 6, 0));
	ImGui::PopID();
	return changed;
}

bool CardEffectEditor(const char* id, std::string* color, bool* temp)
{
	ImGui::PushID(id);
	bool changed = false;
	ImGui::SetCursorScreenPos(ImGui::GetCursorScreenPos() + ImVec2(0, std::floor((PobUi::ControlH() - PobUi::D(22.0f)) * 0.5f)));
	if (CardEffectPalette("##pal", color)) changed = true;
	ImGui::Dummy(ImVec2(0, PobUi::D(4.0f)));
	const ImVec2 p = ImGui::GetCursorScreenPos();
	ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + std::floor((PobUi::ControlH() - SmallF()->FontSize) * 0.5f)));
	PobUi::Hint(u8"只在掉落瞬間");
	ImGui::SameLine(0, PobUi::D(10.0f));
	ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, p.y));
	if (PobUi::Switch("##temp", temp)) changed = true;
	if (ImGui::IsItemHovered()) PobUi::Tooltip(u8"開啟：光柱只在掉落的那一下出現，之後熄滅");
	ImGui::PopID();
	return changed;
}

bool CardColorEditor(EditorShell& s, const char* id, int rgba[4])
{
	(void)s;
	ImGui::PushID(id);
	bool changed = false;
	float col[4] = { rgba[0] / 255.f, rgba[1] / 255.f, rgba[2] / 255.f, rgba[3] / 255.f };
	ImGui::SetNextItemWidth(std::floor(PobUi::D(220.0f)));
	if (ImGui::ColorPicker4("##pick", col,
	                        ImGuiColorEditFlags_NoSidePreview | ImGuiColorEditFlags_NoSmallPreview |
	                        ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel |
	                        ImGuiColorEditFlags_PickerHueBar)) {
		for (int i = 0; i < 4; i++) rgba[i] = std::clamp((int)(col[i] * 255.f + 0.5f), 0, 255);
		changed = true;
	}
	// RGBA as typed text ("255 0 0 255")
	{
		CardUiState& u = UI(s);
		char cur[48];
		std::snprintf(cur, sizeof(cur), "%d %d %d %d", rgba[0], rgba[1], rgba[2], rgba[3]);
		const ImGuiID fid = ImGui::GetID("##rgba");
		if (ImGui::GetActiveID() != fid) std::snprintf(u.rgbaBuf, sizeof(u.rgbaBuf), "%s", cur);
		const unsigned char sw[4] = { (unsigned char)rgba[0], (unsigned char)rgba[1], (unsigned char)rgba[2], (unsigned char)rgba[3] };
		ImGui::SetCursorScreenPos(ImGui::GetCursorScreenPos() + ImVec2(0, PobUi::D(4.0f)));
		const ImVec2 p = ImGui::GetCursorScreenPos();
		ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + std::floor((PobUi::ControlH() - PobUi::D(22.0f)) * 0.5f)));
		EdSwatch("##cur", sw, sw, 22.0f, false);
		ImGui::SetCursorScreenPos(ImVec2(p.x + PobUi::D(30.0f), p.y));
		PobUi::PushControlFrame();
		ImGui::SetNextItemWidth(std::floor(PobUi::D(150.0f)));
		ImGui::InputText("##rgba", u.rgbaBuf, sizeof(u.rgbaBuf));
		PobUi::PopControlFrame();
		if (ImGui::IsItemHovered()) PobUi::Tooltip(u8"紅 綠 藍 透明度（0~255），空白分隔");
		if (ImGui::IsItemDeactivatedAfterEdit()) {
			int v[4] = { rgba[0], rgba[1], rgba[2], 255 };
			const int n = std::sscanf(u.rgbaBuf, "%d %d %d %d", &v[0], &v[1], &v[2], &v[3]);
			if (n >= 3) {
				for (int i = 0; i < 4; i++) rgba[i] = std::clamp(v[i], 0, 255);
				changed = true;
			}
		}
	}
	ImGui::PopID();
	return changed;
}

// ---- popovers -------------------------------------------------------------------

bool CardBeginPopover(const char* id)
{
	ImGui::PushStyleColor(ImGuiCol_PopupBg, ImGui::ColorConvertU32ToFloat4(Tok::SurfaceRaised));
	ImGui::PushStyleColor(ImGuiCol_Border, ImGui::ColorConvertU32ToFloat4(Tok::Border));
	ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, PobUi::D(8.0f));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(PobUi::D(12.0f), PobUi::D(12.0f)));
	ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(PobUi::D(8.0f), PobUi::D(8.0f)));
	if (ImGui::BeginPopup(id)) return true;
	ImGui::PopStyleVar(4);
	ImGui::PopStyleColor(2);
	return false;
}
void CardEndPopover()
{
	ImGui::EndPopup();
	ImGui::PopStyleVar(4);
	ImGui::PopStyleColor(2);
}

bool DrawCardPopovers(EditorShell& s)
{
	CardUiState& u = UI(s);
	const int bi = s.selectedBlock;
	static const char* kPopId[5] = { "", "##fecolorpop", "##feminipop", "##fefxpop", "##fesndpop" };
	if (u.openReq) {
		u.openReq = false;
		if (u.pop != CardPop::None) ImGui::OpenPopup(kPopId[(int)u.pop]);
	}
	if (u.pop == CardPop::None) return false;
	if (bi < 0 || bi >= (int)s.model.blocks.size()) { u.pop = CardPop::None; return false; }

	ImGui::SetNextWindowPos(u.anchor, ImGuiCond_Appearing);
	if (!CardBeginPopover(kPopId[(int)u.pop])) {
		if (!ImGui::IsPopupOpen(kPopId[(int)u.pop])) u.pop = CardPop::None;
		return false;
	}
	ImGui::Dummy(ImVec2(std::floor(PobUi::D(380.0f)), 0));
	bool mutated = false;
	const float smH = std::floor(PobUi::D(28.0f));
	(void)smH;

	switch (u.pop) {
		case CardPop::Color: {
			static const char* kChan[3] = { u8"文字", u8"邊框", u8"背景" };
			PobUi::Segmented("##chan", &u.colorChan, kChan, 3);
			// font size, on the right of the channel switch
			{
				ImGui::SameLine(0, PobUi::D(16.0f));
				const int fl = FindLiveKw(s, bi, "SetFontSize");
				const ImVec2 p = ImGui::GetCursorScreenPos();
				ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + std::floor((PobUi::ControlH() - SmallF()->FontSize) * 0.5f)));
				PobUi::Hint(u8"字級");
				ImGui::SameLine(0, PobUi::D(6.0f));
				ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, p.y));
				if (fl >= 0) {
					int v = FilterValueInt(s.model.lines[fl], 0, 32);
					if (IntField("##fs", &v, 60.0f)) {
						FilterSetValueInt(s.model.lines[fl], 0, std::clamp(v, 1, 45));
						MarkEdited(s);
					}
					if (ImGui::IsItemHovered()) PobUi::Tooltip(u8"1~45；垃圾用小字、重要物品用大字");
				} else if (PobUi::Button(u8"加入", PobUi::BtnKind::Secondary, PobUi::BtnSize::Sm)) {
					EnsureLine(s, bi, "SetFontSize");
					mutated = true;
				}
			}
			ImGui::Dummy(ImVec2(0, PobUi::D(4.0f)));
			const char* kw = kColorKw[std::clamp(u.colorChan, 0, 2)];
			const int li = mutated ? -1 : FindLiveKw(s, bi, kw);
			if (!mutated && li < 0) {
				const bool dis = FindDisabledKw(s, bi, kw) >= 0;
				PobUi::Hint(dis ? u8"這一項已停用。" : u8"這條規則沒有設定這個顏色，遊戲用預設色。");
				if (PobUi::Button(dis ? u8"恢復" : u8"加入", PobUi::BtnKind::Secondary, PobUi::BtnSize::Sm)) {
					EnsureLine(s, bi, kw);
					mutated = true;
				}
			} else if (!mutated) {
				FilterLine& ln = s.model.lines[li];
				int r, g, b, a;
				bool ha;
				FilterGetColor(ln, r, g, b, a, ha);
				int rgba[4] = { r, g, b, a };
				if (CardColorEditor(s, "##ced", rgba)) {
					FilterSetColor(ln, rgba[0], rgba[1], rgba[2], rgba[3], ha || rgba[3] != 255);
					MarkEdited(s);
				}
				ImGui::Dummy(ImVec2(0, PobUi::D(4.0f)));
				EdDrawLabelBox("##cpv", EdBlockStyle(s, bi), EdBlockItemName(s, bi), 56.0f);
				ImGui::Dummy(ImVec2(0, PobUi::D(2.0f)));
				if (PobUi::Button(u8"停用這一項", PobUi::BtnKind::Ghost, PobUi::BtnSize::Sm)) {
					s.doc.CommentOutLine(li);
					mutated = true;
				}
			}
			break;
		}
		case CardPop::Minimap: {
			const int li = FindLiveKw(s, bi, "MinimapIcon");
			if (li < 0) { ImGui::CloseCurrentPopup(); break; }
			FilterLine& ln = s.model.lines[li];
			int size = std::clamp(FilterValueInt(ln, 0, 1), 0, 2);
			std::string col = ln.values.size() > 1 ? ln.values[1].text : "White";
			std::string shape = ln.values.size() > 2 ? ln.values[2].text : "Circle";
			if (CardMinimapEditor("##mm", &size, &col, &shape)) {
				FilterSetValueInt(ln, 0, size);
				FilterSetValueStr(ln, 1, col, false);
				FilterSetValueStr(ln, 2, shape, false);
				MarkEdited(s);
			}
			break;
		}
		case CardPop::Effect: {
			const int li = FindLiveKw(s, bi, "PlayEffect");
			if (li < 0) { ImGui::CloseCurrentPopup(); break; }
			FilterLine& ln = s.model.lines[li];
			std::string col = ln.values.empty() ? "White" : ln.values[0].text;
			bool temp = ln.values.size() > 1 && ln.values[1].text == "Temp";
			if (CardEffectEditor("##fx", &col, &temp)) {
				FilterSetValueStr(ln, 0, col, false);
				if (temp) FilterSetValueStr(ln, 1, "Temp", false);
				else if (ln.values.size() > 1) { ln.values.resize(1); ln.dirty = true; }
				MarkEdited(s);
			}
			break;
		}
		case CardPop::Sound: {
			const int bl = FindLiveKw(s, bi, "PlayAlertSound", "PlayAlertSoundPositional");
			const int cl = FindLiveKw(s, bi, "CustomAlertSound", "CustomAlertSoundOptional");
			int src = cl >= 0 ? 1 : (bl >= 0 ? 0 : 2);
			static const char* kSrc[3] = { u8"內建音效", u8"自訂音效檔", u8"不播放" };
			if (PobUi::Segmented("##src", &src, kSrc, 3)) {
				if (src == 0) {
					DisableAll(s, bi, "CustomAlertSound", "CustomAlertSoundOptional");
					EnsureLine(s, bi, "PlayAlertSound", "PlayAlertSoundPositional");
				} else if (src == 1) {
					DisableAll(s, bi, "PlayAlertSound", "PlayAlertSoundPositional");
					EnsureLine(s, bi, "CustomAlertSound", "CustomAlertSoundOptional");
				} else {
					DisableAll(s, bi, "PlayAlertSound", "PlayAlertSoundPositional");
					DisableAll(s, bi, "CustomAlertSound", "CustomAlertSoundOptional");
				}
				mutated = true;
				break;
			}
			ImGui::Dummy(ImVec2(0, PobUi::D(4.0f)));
			const float labW = std::floor(PobUi::D(44.0f));
			auto label = [&](const char* t) {
				const ImVec2 p = ImGui::GetCursorScreenPos();
				ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + std::floor((PobUi::ControlH() - SmallF()->FontSize) * 0.5f)));
				PobUi::Hint(t);
				ImGui::SetCursorScreenPos(ImVec2(p.x + labW, p.y));
			};
			auto volume = [&](FilterLine& ln, int def) {
				label(u8"音量");
				int vol = std::clamp(FilterValueInt(ln, 1, def), 0, 300);
				ImGui::SetNextItemWidth(std::floor(PobUi::D(200.0f)));
				if (ImGui::SliderInt("##vol", &vol, 0, 300)) { FilterSetValueInt(ln, 1, vol); MarkEdited(s); }
			};
			if (src == 0 && bl >= 0) {
				FilterLine& ln = s.model.lines[bl];
				label(u8"編號");
				std::vector<std::string> ids;
				std::vector<const char*> idp;
				for (int i = 1; i <= 16; i++) ids.push_back(std::to_string(i) + u8" 號");
				for (const std::string& t : ids) idp.push_back(t.c_str());
				int sel = std::clamp(FilterValueInt(ln, 0, 1), 1, 16) - 1;
				if (PobUi::Select("##sid", &sel, idp.data(), nullptr, 16, std::floor(PobUi::D(110.0f)))) {
					FilterSetValueInt(ln, 0, sel + 1);
					MarkEdited(s);
				}
				ImGui::SameLine(0, PobUi::D(16.0f));
				{
					const ImVec2 p = ImGui::GetCursorScreenPos();
					ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + std::floor((PobUi::ControlH() - SmallF()->FontSize) * 0.5f)));
					PobUi::Hint(u8"3D 方位");
					ImGui::SameLine(0, PobUi::D(8.0f));
					ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, p.y));
					bool pos = ln.keyword == "PlayAlertSoundPositional";
					if (PobUi::Switch("##3d", &pos)) {
						ln.keyword = pos ? "PlayAlertSoundPositional" : "PlayAlertSound";
						ln.dirty = true;
						MarkEdited(s);
					}
					if (ImGui::IsItemHovered()) PobUi::Tooltip(u8"開啟：音效帶 3D 方位感（PlayAlertSoundPositional）");
				}
				volume(ln, 300);
				ImGui::Dummy(ImVec2(0, PobUi::D(2.0f)));
				PobUi::Hint(u8"內建音效是遊戲裡的音檔，這裡沒辦法試聽");
			} else if (src == 1 && cl >= 0) {
				FilterLine& ln = s.model.lines[cl];
				if (!s.soundsInit) { s.sounds.Init(s.exeDir); s.soundsInit = true; }
				label(u8"檔案");
				const std::string cur = ln.values.empty() ? std::string() : ln.values[0].text;
				std::vector<std::string> names;
				int sel = -1;
				for (const SoundFileInfo& fi : s.sounds.files()) {
					names.push_back(EdNarrow(fi.name));
					if (names.back() == cur) sel = (int)names.size() - 1;
				}
				if (sel < 0) { names.insert(names.begin(), cur); sel = 0; }
				std::vector<const char*> np;
				for (const std::string& n : names) np.push_back(n.c_str());
				const float fw = (std::max)(std::floor(PobUi::D(200.0f)), PobUi::SelectFitWidth(np.data(), (int)np.size()));
				if (PobUi::Select("##sfile", &sel, np.data(), nullptr, (int)np.size(), fw) && sel >= 0) {
					FilterSetValueStr(ln, 0, names[sel], true);
					MarkEdited(s);
				}
				volume(ln, 300);
				label("");
				bool opt = ln.keyword == "CustomAlertSoundOptional";
				{
					const ImVec2 p = ImGui::GetCursorScreenPos();
					ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + std::floor((PobUi::ControlH() - SmallF()->FontSize) * 0.5f)));
					PobUi::Hint(u8"檔案不在時不報錯");
					ImGui::SameLine(0, PobUi::D(8.0f));
					ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, p.y));
					if (PobUi::Switch("##opt", &opt)) {
						ln.keyword = opt ? "CustomAlertSoundOptional" : "CustomAlertSound";
						ln.dirty = true;
						MarkEdited(s);
					}
				}
				ImGui::Dummy(ImVec2(0, PobUi::D(2.0f)));
				if (PobUi::Button(u8"試聽", PobUi::BtnKind::Secondary, PobUi::BtnSize::Sm, PobIcon::Play)) PlayCustom(s, ln);
				ImGui::SameLine(0, PobUi::D(8.0f));
				if (PobUi::Button(u8"開啟音效頁…", PobUi::BtnKind::Ghost, PobUi::BtnSize::Sm)) {
					s.section = Section::Sounds;
					ImGui::CloseCurrentPopup();
				}
			} else {
				PobUi::Hint(u8"掉落時不播提示音（遊戲的落地音照常）");
			}
			break;
		}
		default: break;
	}
	CardEndPopover();
	return mutated;
}

// Test aid (POBTOOLS_FILTER_STATE): open a popover for the selected block as if
// its value had been clicked. which: 1 colour, 2 minimap, 3 beam, 4 sound.
void CardTestOpenPop(EditorShell& s, int which)
{
	CardUiState& u = UI(s);
	u.pop = (CardPop)std::clamp(which, 1, 4);
	u.openReq = true;
	u.colorChan = 0;
	u.rgbaFor = -1;
	u.anchor = ImVec2(PobUi::D(520.0f), PobUi::D(300.0f));
}
