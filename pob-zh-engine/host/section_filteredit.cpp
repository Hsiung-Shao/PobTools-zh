#include "editor_shell.h"
#include "editor_util.h"
#include "filter_card_ui.h"
#include "filter_batch.h"
#include "custom_rules_io.h"
#include "filter_parser.h"
#include "filter_schema.h"
#include "ui_theme.h"
#include "ui_widgets.h"
#include "ui_icons.h"

#define IMGUI_DEFINE_MATH_OPERATORS
#include <imgui.h>
#include <imgui_internal.h>   // RenderTextEllipsis
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

// 規則 — the three-pane block editor (design: FilterEditor.dc.html):
//   left   search + the block list grouped by NeverSink $type (clipper-rendered)
//   middle the selected block: title row, a game-style label preview, the
//          condition card and the look & sound card
//   right  the add-column (search, grouped, ticked when already in the block)
//
// Index discipline: model-derived caches (rows/visRows/selection) are rebuilt
// whenever FilterDocumentEditor::structureVersion changes; the selected block
// survives rebuilds through BlockAnchor. Translation and summaries run only in
// RebuildRows — never inside the clipper loop.

namespace Tok = PobUi::Tok;

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

// NeverSink's $type-> first segment ("currency"), "" when the header has none.
std::string TypeTop(const std::string& header)
{
	size_t p = header.find("$type->");
	if (p == std::string::npos) return std::string();
	p += 7;
	size_t e = p;
	while (e < header.size() && header[e] != ' ' && header[e] != '\t') e++;
	std::string path = header.substr(p, e - p);
	size_t arrow = path.find("->");
	return arrow == std::string::npos ? path : path.substr(0, arrow);
}

// "【通貨】" -> "通貨": the group heading from the shared NeverSink table.
std::string TypeTopZh(const std::string& top)
{
	std::string z = NeverSinkHeaderZh("$type->" + top);
	const std::string open = u8"【", close = u8"】";
	if (z.compare(0, open.size(), open) == 0) z.erase(0, open.size());
	if (z.size() >= close.size() && z.compare(z.size() - close.size(), close.size(), close) == 0)
		z.erase(z.size() - close.size());
	return z;
}

// The label without NeverSink's style markers ("%D4", "%H2"): they mean nothing
// to a reader of the list.
std::string StripStyleMarkers(const std::string& label)
{
	std::string out;
	size_t p = 0;
	while (p < label.size()) {
		size_t sp = label.find_first_not_of(' ', p);
		if (sp == std::string::npos) break;
		size_t e = label.find(' ', sp);
		if (e == std::string::npos) e = label.size();
		const std::string tok = label.substr(sp, e - sp);
		bool marker = tok.size() >= 2 && tok[0] == '%';
		for (size_t i = 1; marker && i < tok.size(); i++)
			if (!((tok[i] >= 'A' && tok[i] <= 'Z') || (tok[i] >= '0' && tok[i] <= '9'))) marker = false;
		if (!marker) {
			if (!out.empty()) out += ' ';
			out += tok;
		}
		p = e;
	}
	return out.empty() ? label : out;
}

void RebuildRows(EditorShell& s)
{
	const FilterFile& f = s.model;
	s.rows.clear();
	s.rows.resize(f.blocks.size());
	s.groupNames.clear();
	s.groupNames.push_back(u8"自訂規則");   // 0
	s.groupNames.push_back(u8"其他規則");   // 1
	const CustomZone z = FindCustomZone(f);
	for (int i = 0; i < (int)f.blocks.size(); i++) {
		const FilterBlock& b = f.blocks[i];
		BlockListRow& r = s.rows[i];
		std::string rawLabel = ExtractBlockLabel(f, b);
		r.custom = z.present() && b.headerLineIdx > z.beginLine && b.headerLineIdx < z.endLine;
		std::string zh = NeverSinkHeaderZh(rawLabel);
		const bool ns = rawLabel.find("$type->") != std::string::npos;
		r.label = ns ? StripStyleMarkers(zh) : zh;
		// A custom rule still carrying the placeholder name reads as what it matches.
		if (r.custom && b.headerComment == "PobTools custom rule") r.label = CardBlockSummaryZh(f, b, s.i18n);
		// A NeverSink label says which tier, not which item: name the first base.
		if (ns) {
			for (int li : b.lineIdx) {
				const FilterLine& ln = f.lines[li];
				if (ln.kind != FilterLineKind::Condition || ln.keyword != "BaseType" || ln.values.empty()) continue;
				r.label += ' ';
				r.label += s.i18n.DisplayName(ln.values[0].text);
				if (ln.values.size() > 1) r.label += u8" 等 " + std::to_string(ln.values.size()) + u8" 種";
				break;
			}
		}
		if (r.custom) {
			r.group = 0;
		} else {
			const std::string top = TypeTop(b.headerComment);
			if (top.empty()) {
				r.group = 1;
			} else {
				const std::string gz = TypeTopZh(top);
				int gi = -1;
				for (int k = 2; k < (int)s.groupNames.size(); k++)
					if (s.groupNames[k] == gz) { gi = k; break; }
				if (gi < 0) { gi = (int)s.groupNames.size(); s.groupNames.push_back(gz); }
				r.group = gi;
			}
		}
		// haystack 同時收原文與譯文,搜尋 "$type->currency" 或 "通貨" 都命中。
		r.haystack = rawLabel + " " + r.label + " " + s.groupNames[r.group] + " " + BlockSummary(f, b) + " " +
		             CardBlockSummaryZh(f, b, s.i18n);
	}
	// Re-resolve the selection through its anchor (indices may have shifted).
	if (s.selAnchor.valid()) s.selectedBlock = s.doc.ResolveAnchor(s.selAnchor);
	if (s.selectedBlock < 0 || s.selectedBlock >= (int)f.blocks.size())
		s.selectedBlock = f.blocks.empty() ? -1 : 0;
	if (s.selectedBlock >= 0) s.selAnchor = s.doc.CaptureAnchor(s.selectedBlock);
	s.batchSel.assign(f.blocks.size(), 0);
	s.rowsVersion = s.doc.structureVersion();
	EdRebuildVisRows(s);
}

// ---- small drawing helpers ------------------------------------------------------

ImU32 Col(const unsigned char c[4]) { return IM_COL32(c[0], c[1], c[2], c[3]); }

// A 15 px checkbox (the batch list's tick).
bool TickBox(const char* id, bool on, float px)
{
	const ImVec2 p = ImGui::GetCursorScreenPos();
	const bool click = ImGui::InvisibleButton(id, ImVec2(px, px));
	ImDrawList* dl = ImGui::GetWindowDrawList();
	const float r = PobUi::D(4.0f);
	if (on) {
		dl->AddRectFilled(p, p + ImVec2(px, px), Tok::Accent, r);
		if (PobUi::Fonts().icons)
			PobUi::IconAt(dl, p + ImVec2(std::floor(px * 0.12f), std::floor(px * 0.08f)), PobIcon::Check, Tok::OnAccent,
			              std::floor(px * 0.8f));
	} else {
		dl->AddRect(p, p + ImVec2(px, px), Tok::BorderStrong, r, 0, 1.5f);
	}
	return click;
}

// A pill after a list label ("已修改" / "新增").
float PillW(const char* t)
{
	const PobUi::WidgetFonts& wf = PobUi::Fonts();
	ImFont* f = wf.small ? wf.small : ImGui::GetFont();
	return f->CalcTextSizeA(f->FontSize, FLT_MAX, 0.0f, t).x + PobUi::D(12.0f);
}

void PillAt(ImDrawList* dl, ImVec2 p, float rowH, const char* t, ImU32 bg, ImU32 fg)
{
	const PobUi::WidgetFonts& wf = PobUi::Fonts();
	ImFont* f = wf.small ? wf.small : ImGui::GetFont();
	const float px = f->FontSize;
	const float h = std::floor(px + PobUi::D(2.0f));
	const float w = PillW(t);
	const ImVec2 a(p.x, p.y + std::floor((rowH - h) * 0.5f));
	dl->AddRectFilled(a, a + ImVec2(w, h), bg, h * 0.5f);
	dl->AddText(f, px, ImVec2(a.x + PobUi::D(6.0f), a.y + std::floor((h - px) * 0.5f)), fg, t);
}

} // namespace

// ---- shared helpers (editor_shell.h) ----------------------------------------------

std::uint32_t EdEffectColor(const std::string& tok)
{
	// The game's eleven beam / minimap colour tokens, as the editor shows them
	// (Tok::Fx*, the design's palette).
	struct { const char* t; ImU32 c; } k[] = {
		{ "Red", Tok::FxRed }, { "Green", Tok::FxGreen }, { "Blue", Tok::FxBlue }, { "White", Tok::FxWhite },
		{ "Yellow", Tok::FxYellow }, { "Cyan", Tok::FxCyan }, { "Grey", Tok::FxGrey }, { "Pink", Tok::FxPink },
		{ "Orange", Tok::FxOrange }, { "Purple", Tok::FxPurple }, { "Brown", Tok::FxBrown },
	};
	for (auto& e : k)
		if (tok.rfind(e.t, 0) == 0) return e.c;
	return Tok::TextMuted;
}

bool EdSwatch(const char* id, const unsigned char fill[4], const unsigned char edge[4], float designPx,
              bool clickable, bool hasFill, bool hasEdge)
{
	const float px = std::floor(PobUi::D(designPx));
	const ImVec2 p = ImGui::GetCursorScreenPos();
	bool click = false;
	if (clickable) click = ImGui::InvisibleButton(id, ImVec2(px, px));
	else ImGui::Dummy(ImVec2(px, px));
	const bool hov = clickable && ImGui::IsItemHovered();
	ImDrawList* dl = ImGui::GetWindowDrawList();
	const float r = PobUi::D(3.0f);
	const float bw = designPx >= 20.0f ? 1.0f : std::max(1.0f, std::floor(PobUi::D(2.0f)));
	if (hasFill) {
		// alpha shows over a checker-ish dark base
		dl->AddRectFilled(p, p + ImVec2(px, px), Tok::Canvas, r);
		dl->AddRectFilled(p, p + ImVec2(px, px), Col(fill), r);
	}
	if (hasEdge) dl->AddRect(p, p + ImVec2(px, px), Col(edge), r, 0, bw);
	else dl->AddRect(p, p + ImVec2(px, px), Tok::BorderStrong, r, 0, bw);
	if (hov) {
		dl->AddRect(p - ImVec2(2, 2), p + ImVec2(px + 2, px + 2), Tok::Accent, r + 2.0f, 0, 1.5f);
		ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
	}
	return click;
}

EdLabelStyle EdBlockStyle(const EditorShell& s, int bi)
{
	EdLabelStyle st;
	if (bi < 0 || bi >= (int)s.model.blocks.size()) return st;
	const FilterBlock& b = s.model.blocks[bi];
	auto color = [&](int li, unsigned char out[4]) {
		if (li < 0) return false;
		int r, g, bb, a;
		bool ha;
		FilterGetColor(s.model.lines[li], r, g, bb, a, ha);
		out[0] = (unsigned char)std::clamp(r, 0, 255);
		out[1] = (unsigned char)std::clamp(g, 0, 255);
		out[2] = (unsigned char)std::clamp(bb, 0, 255);
		out[3] = (unsigned char)std::clamp(a, 0, 255);
		return true;
	};
	color(b.idxTextColor, st.text);
	st.hasBorder = color(b.idxBorderColor, st.border);
	color(b.idxBgColor, st.back);
	if (b.idxFontSize >= 0) st.fontSize = std::clamp(FilterValueInt(s.model.lines[b.idxFontSize], 0, 32), 1, 45);
	if (b.idxPlayEffect >= 0) {
		const FilterLine& ln = s.model.lines[b.idxPlayEffect];
		if (!ln.values.empty()) st.beam = ln.values[0].text;
	}
	if (b.idxMinimapIcon >= 0) {
		const FilterLine& ln = s.model.lines[b.idxMinimapIcon];
		st.minimapNote = CardMinimapSummary(ln);
	}
	const int snd = b.idxCustomSound >= 0 ? b.idxCustomSound : b.idxAlertSound;
	if (snd >= 0) st.soundNote = CardSoundSummary(s.model.lines[snd]);
	return st;
}

std::string EdBlockItemName(const EditorShell& s, int bi)
{
	if (bi < 0 || bi >= (int)s.model.blocks.size()) return u8"物品";
	const FilterBlock& b = s.model.blocks[bi];
	std::string cls;
	for (int li : b.lineIdx) {
		const FilterLine& ln = s.model.lines[li];
		if (ln.kind != FilterLineKind::Condition || ln.values.empty()) continue;
		if (ln.keyword == "BaseType") return s.i18n.DisplayName(ln.values[0].text);
		if (ln.keyword == "Class" && cls.empty()) cls = s.i18n.ClassNameZh(ln.values[0].text);
	}
	return cls.empty() ? std::string(u8"物品") : cls;
}

void EdDrawLabelBox(const char* id, const EdLabelStyle& st, const std::string& name, float designHeight)
{
	ImGui::PushID(id);
	const float w = ImGui::GetContentRegionAvail().x;
	const float h = std::floor(PobUi::D(designHeight));
	const ImVec2 p = ImGui::GetCursorScreenPos();
	ImGui::Dummy(ImVec2(w, h));
	ImDrawList* dl = ImGui::GetWindowDrawList();
	dl->PushClipRect(p, p + ImVec2(w, h), true);
	dl->AddRectFilled(p, p + ImVec2(w, h), Tok::Canvas, PobUi::D(8.0f));
	dl->AddRect(p, p + ImVec2(w, h), Tok::Border, PobUi::D(8.0f), 0, 1.0f);

	ImFont* font = ImGui::GetFont();
	// the game draws 32 as its "normal" size; the box scales to it
	const float px = std::min(ImGui::GetFontSize() * (float)st.fontSize / 32.0f, h * 0.42f);
	const ImVec2 ts = font->CalcTextSizeA(px, FLT_MAX, 0.0f, name.c_str());
	const float padX = 0.45f * px, padY = 0.2f * px;
	const ImVec2 lsz(ts.x + padX * 2.0f, ts.y + padY * 2.0f);
	const ImVec2 lp(std::floor(p.x + (w - lsz.x) * 0.5f), std::floor(p.y + (h - lsz.y) * 0.5f) - PobUi::D(4.0f));
	if (!st.beam.empty()) {
		const ImU32 bc = EdEffectColor(st.beam);
		const float bx = lp.x + lsz.x * 0.5f;
		const float bwid = PobUi::D(3.0f);
		dl->AddRectFilledMultiColor(ImVec2(bx - bwid, p.y), ImVec2(bx + bwid, lp.y),
		                            bc & 0x00FFFFFF, bc & 0x00FFFFFF, (bc & 0x00FFFFFF) | 0x90000000u,
		                            (bc & 0x00FFFFFF) | 0x90000000u);
	}
	dl->AddRectFilled(lp, lp + lsz, Col(st.back));
	if (st.hasBorder) dl->AddRect(lp, lp + lsz, Col(st.border), 0.0f, 0, std::max(1.5f, std::floor(PobUi::D(2.0f))));
	dl->AddText(font, px, ImVec2(lp.x + padX, lp.y + padY), Col(st.text), name.c_str());

	const PobUi::WidgetFonts& wf = PobUi::Fonts();
	ImFont* sf = wf.small ? wf.small : font;
	const float spx = sf->FontSize;
	const float by = p.y + h - spx - PobUi::D(6.0f);
	if (!st.minimapNote.empty())
		dl->AddText(sf, spx, ImVec2(p.x + PobUi::D(10.0f), by), Tok::TextMuted, st.minimapNote.c_str());
	if (!st.soundNote.empty()) {
		float x = p.x + w - PobUi::D(10.0f);
		const ImVec2 ns = sf->CalcTextSizeA(spx, FLT_MAX, 0.0f, st.soundNote.c_str());
		x -= ns.x;
		dl->AddText(sf, spx, ImVec2(x, by), Tok::TextMuted, st.soundNote.c_str());
		if (wf.icons) {
			const float iw = PobUi::IconWidth(PobIcon::Volume, spx);
			PobUi::IconAt(dl, ImVec2(x - iw - PobUi::D(4.0f), by), PobIcon::Volume, Tok::TextMuted, spx);
		}
	}
	dl->PopClipRect();
	ImGui::PopID();
}

bool EdBlockIsCustom(const EditorShell& s, int bi)
{
	return bi >= 0 && bi < (int)s.rows.size() && s.rows[bi].custom;
}

void EdRebuildVisRows(EditorShell& s)
{
	s.visRows.clear();
	s.visRows.reserve(s.rows.size());
	for (int i = 0; i < (int)s.rows.size(); i++) {
		if (!s.searchLower.empty() && !EdContainsCI(s.rows[i].haystack, s.searchLower)) continue;
		s.visRows.push_back(i);
	}
	// Group headings wherever the group changes (file order is kept: the game
	// reads first-match top to bottom, so the list never re-sorts).
	s.visList.clear();
	s.visList.reserve(s.visRows.size() + 32);
	int last = -1;
	for (int bi : s.visRows) {
		const int g = s.rows[bi].group;
		if (g != last) {
			s.visList.push_back(BlockListEntry{ -1, g });
			last = g;
		}
		s.visList.push_back(BlockListEntry{ bi, g });
	}
}

// Shared cache rebuild — 掉落預覽 needs rows (labels) without visiting this
// page first.
void EdRebuildRows(EditorShell& s) { RebuildRows(s); }

namespace {

// Left pane. Returns false when a structural mutation made every cached index
// stale (the caller then skips the other panes this frame).
bool DrawBlockList(EditorShell& s, float height)
{
	const float padX = PobUi::D(10.0f);
	const float w = ImGui::GetContentRegionAvail().x;
	ImGui::SetCursorPos(ImGui::GetCursorPos() + ImVec2(padX, PobUi::D(10.0f)));
	const float innerW = w - padX * 2.0f;
	ImGui::BeginGroup();
	if (PobUi::SearchField("##fesearch", s.searchBuf, (int)sizeof(s.searchBuf), u8"搜尋規則", innerW)) {
		s.search = s.searchBuf;
		s.searchLower = EdToLowerAscii(s.search);
		EdRebuildVisRows(s);
	}
	ImGui::Dummy(ImVec2(0, PobUi::D(2.0f)));

	int nBlocks = 0, nSel = 0;
	for (int bi : s.visRows) { (void)bi; nBlocks++; }
	for (char c : s.batchSel) if (c) nSel++;
	const float smH = std::floor(PobUi::D(26.0f));
	const float rowY = ImGui::GetCursorScreenPos().y;
	if (s.batchMode) {
		const std::string t = u8"已選 " + std::to_string(nSel) + u8" 條 · 符合 " + std::to_string(nBlocks) + u8" 條";
		const float b1 = PobUi::ButtonWidth(u8"全選可見", PobUi::BtnSize::Sm), b2 = PobUi::ButtonWidth(u8"清除", PobUi::BtnSize::Sm);
		ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, rowY + std::floor((smH - ImGui::GetTextLineHeight()) * 0.5f)));
		PobUi::Hint(t.c_str());
		const float x0 = ImGui::GetItemRectMin().x;
		ImGui::SetCursorScreenPos(ImVec2(x0 + innerW - b1 - b2 - PobUi::D(4.0f), rowY));
		if (PobUi::Button(u8"全選可見", PobUi::BtnKind::Ghost, PobUi::BtnSize::Sm))
			for (int bi : s.visRows) s.batchSel[bi] = 1;
		ImGui::SameLine(0, PobUi::D(4.0f));
		if (PobUi::Button(u8"清除", PobUi::BtnKind::Ghost, PobUi::BtnSize::Sm))
			s.batchSel.assign(s.batchSel.size(), 0);
	} else {
		const std::string t = u8"符合 " + std::to_string(nBlocks) + u8" 條";
		const char* addLbl = u8"新增自訂規則";
		const float bw = PobUi::ButtonWidth(addLbl, PobUi::BtnSize::Sm, PobIcon::Plus);
		const float x0 = ImGui::GetCursorScreenPos().x;
		ImGui::SetCursorScreenPos(ImVec2(x0, rowY + std::floor((smH - ImGui::GetTextLineHeight()) * 0.5f)));
		PobUi::Hint(t.c_str());
		ImGui::SetCursorScreenPos(ImVec2(x0 + innerW - bw, rowY));
		if (PobUi::Button(addLbl, PobUi::BtnKind::Ghost, PobUi::BtnSize::Sm, PobIcon::Plus)) {
			CustomZone z = EnsureCustomZone(s.doc);
			if (z.present()) {
				int nb = s.doc.CreateBlockAtLine(z.endLine, false, u8"PobTools custom rule");
				if (nb >= 0) {
					// A bare Show with no condition would match EVERYTHING — seed a
					// BaseType the user is meant to replace.
					s.doc.InsertLine(nb, "BaseType", "", { FilterToken{ "Divine Orb", true } });
					s.selectedBlock = nb;
					s.selAnchor = s.doc.CaptureAnchor(nb);
					s.Notify(u8"已在自訂區新增規則：改成你要的物品名稱");
				}
			}
			ImGui::EndGroup();
			return false;  // caches are stale; skip the list until next frame's rebuild
		}
		if (ImGui::IsItemHovered()) PobUi::Tooltip(u8"新規則加在檔案最上方的自訂區，比 NeverSink 的規則優先");
	}
	ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, rowY + smH + PobUi::D(4.0f)));
	ImGui::EndGroup();

	// the batch button sits under the list
	const float footH = s.batchMode ? PobUi::ControlH() + PobUi::D(20.0f) : PobUi::D(6.0f);
	const float listH = (std::max)(PobUi::D(60.0f), height - (ImGui::GetCursorPosY()) - footH);

	ImGui::SetCursorPosX(ImGui::GetCursorPosX() + PobUi::D(4.0f));
	ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
	ImGui::BeginChild("##blocklist", ImVec2(w - PobUi::D(8.0f), listH), false);
	const float rowH = std::floor(PobUi::D(28.0f));
	const float lw = ImGui::GetContentRegionAvail().x;
	ImDrawList* dl = ImGui::GetWindowDrawList();
	const PobUi::WidgetFonts& wf = PobUi::Fonts();
	ImFont* sf = wf.small ? wf.small : ImGui::GetFont();
	if (s.scrollToSel) {
		s.scrollToSel = false;
		for (int vi = 0; vi < (int)s.visList.size(); vi++)
			if (s.visList[vi].block == s.selectedBlock) {
				ImGui::SetScrollY((std::max)(0.0f, rowH * (float)vi - listH * 0.35f));
				break;
			}
	}
	ImGuiListClipper clip;
	clip.Begin((int)s.visList.size(), rowH);
	int toggleHide = -1;
	while (clip.Step()) {
		for (int vi = clip.DisplayStart; vi < clip.DisplayEnd; vi++) {
			const BlockListEntry& e = s.visList[vi];
			const ImVec2 p = ImGui::GetCursorScreenPos();
			if (e.block < 0) {
				// group heading: overline style, sitting on the row's baseline
				const char* gname = s.groupNames[e.group].c_str();
				dl->AddText(sf, sf->FontSize, ImVec2(p.x + PobUi::D(8.0f), p.y + rowH - sf->FontSize - PobUi::D(3.0f)),
				            Tok::TextMuted, gname);
				ImGui::Dummy(ImVec2(lw, rowH));
				continue;
			}
			const int bi = e.block;
			const FilterBlock& b = s.model.blocks[bi];
			const BlockListRow& row = s.rows[bi];
			ImGui::PushID(bi);
			const bool sel = !s.batchMode && s.selectedBlock == bi;
			const float lead = std::floor(PobUi::D(22.0f));
			// the row's background first, so the eye and the label draw over it
			const bool rowHov = ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(p, p + ImVec2(lw, rowH));
			if (sel || rowHov)
				dl->AddRectFilled(p, p + ImVec2(lw, rowH), sel ? Tok::AccentSoft : Tok::Surface2, PobUi::D(5.0f));

			// leading control: eye (show / hide) or the batch tick
			ImGui::SetCursorScreenPos(ImVec2(p.x + PobUi::D(6.0f), p.y + std::floor((rowH - PobUi::D(16.0f)) * 0.5f)));
			if (s.batchMode) {
				if (TickBox("##tick", s.batchSel[bi] != 0, std::floor(PobUi::D(15.0f))))
					s.batchSel[bi] = s.batchSel[bi] ? 0 : 1;
			} else {
				const float ip = std::floor(PobUi::D(16.0f));
				const ImVec2 ep = ImGui::GetCursorScreenPos();
				if (ImGui::InvisibleButton("##eye", ImVec2(ip, ip))) toggleHide = bi;
				const bool eh = ImGui::IsItemHovered();
				if (wf.icons)
					PobUi::IconAt(dl, ep, b.hide ? PobIcon::EyeOff : PobIcon::Eye, eh ? Tok::Text : Tok::TextMuted, ip);
				if (eh) PobUi::Tooltip(b.hide ? u8"隱藏中：點一下改成顯示" : u8"顯示中：點一下改成隱藏");
			}

			// the rest of the row selects
			ImGui::SetCursorScreenPos(ImVec2(p.x + lead + PobUi::D(6.0f), p.y));
			const float restW = lw - lead - PobUi::D(6.0f);
			const bool click = ImGui::InvisibleButton("##row", ImVec2(restW, rowH));
			const bool hov = ImGui::IsItemHovered();
			if (click) {
				if (s.batchMode) s.batchSel[bi] = s.batchSel[bi] ? 0 : 1;
				else { s.selectedBlock = bi; s.selAnchor = s.doc.CaptureAnchor(bi); }
			}
			if (hov && !sel) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

			// swatch (background + border colour) at the right
			const EdLabelStyle st = EdBlockStyle(s, bi);
			const float swPx = std::floor(PobUi::D(14.0f));
			const ImVec2 swp(p.x + lw - swPx - PobUi::D(8.0f), p.y + std::floor((rowH - swPx) * 0.5f));
			{
				const ImU32 fill = b.hide ? 0 : Col(st.back);
				if (fill) dl->AddRectFilled(swp, swp + ImVec2(swPx, swPx), fill, PobUi::D(3.0f));
				dl->AddRect(swp, swp + ImVec2(swPx, swPx), st.hasBorder && !b.hide ? Col(st.border) : Tok::BorderStrong,
				            PobUi::D(3.0f), 0, std::max(1.0f, std::floor(PobUi::D(2.0f))));
			}

			// label + markers
			bool blockDirty = false;
			for (int li : b.lineIdx)
				if (s.model.lines[li].dirty) { blockDirty = true; break; }
			const char* mark = blockDirty ? u8"已修改" : nullptr;
			const char* hid = b.hide ? u8"隱藏" : nullptr;
			const float tx = p.x + lead + PobUi::D(10.0f);
			const float tRight = swp.x - PobUi::D(8.0f);
			float markW = 0.0f;
			if (mark) markW += PillW(mark) + PobUi::D(6.0f);
			if (hid) markW += sf->CalcTextSizeA(sf->FontSize, FLT_MAX, 0.0f, hid).x + PobUi::D(6.0f);
			const ImVec2 ls = ImGui::CalcTextSize(row.label.c_str());
			const float labelMax = (std::max)(PobUi::D(40.0f), tRight - tx - markW);
			const float ty = p.y + std::floor((rowH - ImGui::GetTextLineHeight()) * 0.5f);
			ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(b.hide ? Tok::TextFaint : Tok::Text));
			ImGui::RenderTextEllipsis(dl, ImVec2(tx, ty), ImVec2(tx + labelMax, ty + ls.y), tx + labelMax, tx + labelMax,
			                          row.label.c_str(), nullptr, &ls);
			ImGui::PopStyleColor();
			float mx = tx + (std::min)(ls.x, labelMax) + PobUi::D(6.0f);
			if (mark) {
				PillAt(dl, ImVec2(mx, p.y), rowH, mark, Tok::AccentSoft, Tok::AccentText);
				mx += PillW(mark) + PobUi::D(6.0f);
			}
			if (hid)
				dl->AddText(sf, sf->FontSize, ImVec2(mx, p.y + std::floor((rowH - sf->FontSize) * 0.5f)), Tok::TextFaint, hid);
			if (hov && ls.x > labelMax) PobUi::Tooltip(row.label.c_str());

			ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + rowH));
			ImGui::Dummy(ImVec2(0, 0));
			ImGui::PopID();
		}
	}
	clip.End();
	if (s.visList.empty()) PobUi::Hint(u8"沒有符合的規則");
	ImGui::EndChild();
	ImGui::PopStyleColor();

	if (toggleHide >= 0 && toggleHide < (int)s.model.blocks.size())
		SetBlockHide(s, s.model.blocks[toggleHide], !s.model.blocks[toggleHide].hide);

	if (s.batchMode) {
		ImGui::SetCursorPos(ImVec2(padX, height - PobUi::ControlH() - PobUi::D(10.0f)));
		const std::string lbl = u8"修改已選的 " + std::to_string(nSel) + u8" 條…";
		if (PobUi::Button(lbl.c_str(), PobUi::BtnKind::Primary, PobUi::BtnSize::Md, nullptr, innerW, nSel > 0))
			s.wantBatchDialog = true;
	}
	return true;
}

// Middle pane: title row + label preview + cards. Returns true after a
// structural mutation.
bool DrawMiddle(EditorShell& s, bool* wantDelete)
{
	const int bi = s.selectedBlock;
	if (bi < 0 || bi >= (int)s.model.blocks.size()) {
		PobUi::EmptyState("##nosel", PobIcon::Funnel, u8"在左側選一條規則", u8"選了之後在這裡改條件、顏色與音效");
		return false;
	}
	const BlockListRow& row = s.rows[bi];
	FilterBlock& b = s.model.blocks[bi];
	const bool custom = row.custom;

	// title row: name + where it is | 刪除規則 (custom) | 顯示 / 隱藏
	{
		const float avail = ImGui::GetContentRegionAvail().x;
		const ImVec2 p = ImGui::GetCursorScreenPos();
		const char* shLabels[2] = { u8"顯示", u8"隱藏" };
		const float segW = PobUi::SegmentedWidth(shLabels, 2);
		const float delW = custom ? PobUi::ButtonWidth(u8"刪除規則", PobUi::BtnSize::Sm, PobIcon::Trash) + PobUi::D(8.0f) : 0.0f;
		const float titleW = (std::max)(PobUi::D(80.0f), avail - segW - delW - PobUi::D(12.0f));
		const PobUi::WidgetFonts& wf = PobUi::Fonts();
		ImFont* hf = wf.heading ? wf.heading : ImGui::GetFont();
		const float hpx = wf.headingPx > 0 ? wf.headingPx : hf->FontSize;
		ImFont* sf = wf.small ? wf.small : ImGui::GetFont();
		const float rowH = (std::max)(PobUi::ControlH(), hpx + sf->FontSize + PobUi::D(2.0f));
		ImDrawList* dl = ImGui::GetWindowDrawList();
		const ImVec4 clip(p.x, p.y, p.x + titleW, p.y + rowH);
		const float ty = p.y + std::floor((rowH - hpx - sf->FontSize - PobUi::D(2.0f)) * 0.5f);
		dl->AddText(hf, hpx, ImVec2(p.x, ty), Tok::Text, row.label.c_str(), nullptr, 0.0f, &clip);
		std::string where = custom ? std::string(u8"自訂規則") : std::string(u8"NeverSink 預設規則");
		if (row.label.find(u8"【") == std::string::npos && !custom) where = u8"規則";
		bool dirty = false;
		for (int li : b.lineIdx) if (s.model.lines[li].dirty) { dirty = true; break; }
		if (dirty) where += u8" · 有修改還沒儲存";
		where += u8" · 第 " + std::to_string(bi + 1) + u8" 條";
		dl->AddText(sf, sf->FontSize, ImVec2(p.x, ty + hpx + PobUi::D(2.0f)), Tok::TextMuted, where.c_str(), nullptr, 0.0f, &clip);
		if (ImGui::IsMouseHoveringRect(ImVec2(clip.x, clip.y), ImVec2(clip.z, clip.w)))
			PobUi::Tooltip(row.label.c_str());

		float x = p.x + avail - segW - delW;
		if (custom) {
			ImGui::SetCursorScreenPos(ImVec2(x, p.y + std::floor((rowH - PobUi::D(28.0f)) * 0.5f)));
			if (PobUi::Button(u8"刪除規則", PobUi::BtnKind::Ghost, PobUi::BtnSize::Sm, PobIcon::Trash)) *wantDelete = true;
			if (ImGui::IsItemHovered()) PobUi::Tooltip(u8"從檔案移除這條自訂規則，沒辦法復原");
			x += delW;
		}
		ImGui::SetCursorScreenPos(ImVec2(x, p.y + std::floor((rowH - PobUi::ControlH()) * 0.5f)));
		int sh = b.hide ? 1 : 0;
		if (PobUi::Segmented("##showhide", &sh, shLabels, 2)) SetBlockHide(s, b, sh == 1);
		ImGui::SetCursorScreenPos(p);
		ImGui::Dummy(ImVec2(avail, rowH));
	}
	ImGui::Dummy(ImVec2(0, PobUi::D(4.0f)));

	// the game-style label, live
	EdDrawLabelBox("##dropbox", EdBlockStyle(s, bi), EdBlockItemName(s, bi), 76.0f);
	ImGui::Dummy(ImVec2(0, PobUi::D(4.0f)));

	ImGui::PushID(bi);
	const bool mutated = DrawBlockCards(s, bi);
	ImGui::PopID();
	return mutated;
}

} // namespace

void DrawFilterEditSection(EditorShell& s)
{
	if (!s.loaded) {
		const ImVec2 avail = ImGui::GetContentRegionAvail();
		const float w = (std::min)(avail.x - PobUi::D(32.0f), PobUi::D(560.0f));
		const char* hint = u8"從遊戲的過濾器資料夾選一個 .filter 開始編輯。";
		const float h = PobUi::EmptyStateHeight(u8"還沒開啟過濾器", hint, true, w) + PobUi::D(28.0f);
		ImGui::SetCursorPos(ImGui::GetCursorPos() +
		                    ImVec2(std::floor((avail.x - w) * 0.5f), (std::max)(PobUi::D(16.0f), std::floor((avail.y - h) * 0.4f))));
		const int r = PobUi::EmptyStateEx("##nofile", PobIcon::Funnel, u8"還沒開啟過濾器", hint, u8"選擇過濾器…",
		                                  u8"開啟其他檔案…", u8"Documents\\My Games\\Path of Exile\\", w);
		if (r == 1) {
			if (s.fileList.empty()) s.pendingDialog = EdDialog::OpenFilter;
			else PobUi::TestOpenSelect("##fefile");
		} else if (r == 2) {
			s.pendingDialog = EdDialog::OpenFilter;
		}
		return;
	}
	if (s.doc.file() != &s.model) s.doc.Attach(&s.model);
	if (s.rowsVersion != s.doc.structureVersion()) RebuildRows(s);

	const float H = ImGui::GetContentRegionAvail().y;
	const float totalW = ImGui::GetContentRegionAvail().x;
	const float leftW = std::floor(PobUi::D(330.0f));
	const float rightW = s.batchMode ? 0.0f : std::floor(PobUi::D(250.0f));
	ImDrawList* wdl = ImGui::GetWindowDrawList();
	const ImVec2 origin = ImGui::GetCursorScreenPos();

	// left pane (surface-1 panel)
	wdl->AddRectFilled(origin, origin + ImVec2(leftW, H), Tok::Surface1);
	wdl->AddLine(origin + ImVec2(leftW - 1.0f, 0), origin + ImVec2(leftW - 1.0f, H), Tok::Border, 1.0f);
	ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
	ImGui::BeginChild("##left", ImVec2(leftW - 1.0f, H), false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
	const bool listOk = DrawBlockList(s, H);
	ImGui::EndChild();
	ImGui::PopStyleColor();

	// A structural change in the left pane (new custom rule) leaves every
	// cached index stale — skip the other panes until the next frame's rebuild.
	if (!listOk || s.rowsVersion != s.doc.structureVersion()) return;

	bool mutated = false;
	bool wantDelete = s.wantDeleteDialog;
	s.wantDeleteDialog = false;
	const float midW = totalW - leftW - rightW;
	ImGui::SetCursorScreenPos(origin + ImVec2(leftW, 0));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(PobUi::D(16.0f), PobUi::D(12.0f)));
	ImGui::BeginChild("##mid", ImVec2(midW, H), false, ImGuiWindowFlags_AlwaysUseWindowPadding);
	ImGui::PopStyleVar();
	if (s.batchMode) {
		const int nSel = (int)std::count(s.batchSel.begin(), s.batchSel.end(), (char)1);
		const std::string hint = u8"勾選左側規則，再按「修改已選的 " + std::to_string(nSel) + u8" 條…」";
		PobUi::EmptyState("##batchhint", PobIcon::List, u8"批量修改", hint.c_str(), nullptr, 0.0f,
		                  ImGui::GetContentRegionAvail().y);
	} else {
		mutated = DrawMiddle(s, &wantDelete);
	}
	ImGui::EndChild();

	if (!s.batchMode) {
		ImGui::SetCursorScreenPos(origin + ImVec2(leftW + midW, 0));
		wdl->AddRectFilled(origin + ImVec2(leftW + midW, 0), origin + ImVec2(leftW + midW + rightW, H), Tok::Surface1);
		wdl->AddLine(origin + ImVec2(leftW + midW, 0), origin + ImVec2(leftW + midW, H), Tok::Border, 1.0f);
		ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(PobUi::D(10.0f), PobUi::D(10.0f)));
		ImGui::BeginChild("##right", ImVec2(rightW, H), false, ImGuiWindowFlags_AlwaysUseWindowPadding);
		ImGui::PopStyleVar();
		// After a structural mutation every cached index is stale: skip the
		// add-column this frame and let the next frame rebuild.
		if (!mutated) mutated = DrawAddColumn(s, s.selectedBlock);
		ImGui::EndChild();
		ImGui::PopStyleColor();
	}
	ImGui::SetCursorScreenPos(origin + ImVec2(0, H));

	// ---- everything below is submitted at the page's top level: popups never
	// open from inside a child or a PushID loop (agent-data error_imgui_popup_selftest).
	if (!mutated && s.rowsVersion == s.doc.structureVersion()) DrawCardPopovers(s);

	if (s.wantBatchDialog) {
		s.wantBatchDialog = false;
		OpenBatchDialog(s);
	}
	DrawBatchDialog(s);

	// delete a custom rule (Danger confirm). The deletion happens at the end of
	// this frame; the next frame rebuilds the caches.
	{
		static const char* kDelId = u8"##fedelrule";
		bool open = wantDelete;
		const std::string nm = (s.selectedBlock >= 0 && s.selectedBlock < (int)s.rows.size())
			? s.rows[s.selectedBlock].label : std::string();
		const std::string title = u8"刪除自訂規則「" + nm + u8"」？";
		const PobUi::DialogResult r = PobUi::ConfirmDialog(kDelId, &open, title.c_str(),
			u8"這條規則會從檔案移除，沒辦法復原。NeverSink 的預設規則只能停用，不能刪除。", nullptr,
			u8"取消", u8"刪除規則", nullptr);
		if (r == PobUi::DialogResult::Danger && s.selectedBlock >= 0 &&
		    s.selectedBlock < (int)s.model.blocks.size() && EdBlockIsCustom(s, s.selectedBlock)) {
			s.doc.RemoveBlock(s.selectedBlock);
			s.selectedBlock = -1;
			s.selAnchor = {};
			s.Notify(u8"已刪除自訂規則（記得儲存）");
		}
	}
}
