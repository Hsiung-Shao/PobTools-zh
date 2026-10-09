#include "regex_tool.h"

#include "clipboard_util.h"
#include "regex_algo_pages.h"
#include "regex_bookmarks_share.h"
#include "regex_data.h"
#include "regex_embed.h"
#include "regex_folders.h"
#include "regex_gen.h"
#include "regex_itemmods.h"
#include "regex_send.h"
#include "regex_share.h"
#include "regex_state.h"
#include "error_log.h"
#include "tool_panel.h"
#include "tool_window.h"
#include "ui_theme.h"
#include "ui_widgets.h"
#include "ui_icons.h"

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>   // ShellExecuteW: "開啟資料夾" on the data error

#define IMGUI_DEFINE_MATH_OPERATORS
#include <imgui.h>
#include <imgui_internal.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <thread>
#include <cstdlib>
#include <optional>
#include <set>
#include <string>
#include <vector>

// 搜尋字串產生器 — tick the modifiers, get the shortest string that finds exactly
// those and nothing else, paste it into the game's search box.
//
// The panel owns no knowledge of what a map modifier is. It shows the pages the
// data files happen to contain, and everything about picking tokens lives in
// regex_gen.
//
// Choosing is two-level: the game first, then that game's list. PoE1 and PoE2
// have nothing to say to each other -- a waystone modifier is not a candidate
// for a map search and vice versa -- so a flat list of every page with the game
// spelled out in each label was one reading step where there should be none. The
// bookmark list follows the same selector for the same reason.
//
// A row shows the line in the language the query is being built from, and -- when
// the bilingual switch is on -- the other language underneath it. Which language
// the QUERY uses is the player's choice, because the two are not interchangeable:
// a token cut from the Chinese only avoids false positives among the Chinese
// lines, and pasting it into an English client would match nothing at all.
//
// Algorithmic pages (regex_algo_pages, ported from exile-appraiser): the map /
// waystone modifier pages carry a collapsible numeric SECTION on top (tier,
// quantity, rarity ...) whose terms join the modifier tokens in one string, and
// each game has a vendor page. Their rows are an input + a fragment, not a line
// to cut tokens from.
//
// Multi-page merge (R4, exile-appraiser combine.ts / RegexCombined.vue): every
// page of the current game with ticks, plus free-typed custom terms and excludes,
// go into ONE string (RegexAlgo::Combine). The output switches between that
// merged string and the current page's own ("合併 / 單頁", B's outScope, default
// merged); a "已選（合併）" view lists which pages take part, what each costs,
// the custom / exclude chips and the merge conflicts. Ticks live per page, so
// they survive switching pages either way.
//
// Persistence (R5, regex_ui.json schema 5 = exile-appraiser's state.ts): every
// page's ticks (a section's under its host's `num`), algorithmic values, custom
// text, excludes, output scope, the merged / page view and folded sections all
// go to state_ the moment they change. A bookmark is the whole page (embed.ts):
// a host page carries its section, the vendor page its values.
//
// Bookmarks (R6, exile-appraiser folders.ts / RegexBookmarks.vue): PoE1 / PoE2
// tabs, one level of folders (add / rename / delete / fold, an "uncategorised"
// group), drag to reorder or into a folder, with up / down buttons and a
// "move to" menu as the non-drag way. No hotkeys and no paste-into-game: those
// are exile-appraiser overlay features this panel does not have.
//
// Share codes and templates (R8, exile-appraiser share.ts / RegexPanel.vue): the
// whole game's ticks, values, custom text and excludes as one gzip+base64url
// string ("複製分享碼"), pasted back through a dialog ("貼上分享碼"), and seven
// hand-written templates (Data/regex_templates.json). Both OVERWRITE every list
// of that game, so both ask first (B's confirm dialogs). Codes travel both ways
// with exile-appraiser, every page included (2026-10-09: the item-mod values
// page reads the same stats.ndjson and keys by trade stat id).
//
// Look (2026-10-09 design, Regex*.dc.html): one header row, the list column +
// a 450 px output / bookmark column, .it / .nr rows, cards, dialogs and menus
// from ui_widgets; POBTOOLS_REGEX_STATE renders each draft's state for a
// POBTOOLS_TOOL_SHOT screenshot.

namespace {

const ImVec4 kWarn(0.95f, 0.66f, 0.25f, 1.0f);
const ImVec4 kBad(0.94f, 0.27f, 0.27f, 1.0f);
const ImVec4 kGood(0.45f, 0.85f, 0.55f, 1.0f);

// The game id is "poe1" / "poe2" and nothing else, so narrowing it for a message
// is a cast, not a conversion. Spelled out because the implicit form warns, and a
// silenced warning here would also silence the day someone passes real text.
std::string NarrowAscii(const std::wstring& w)
{
	std::string out;
	out.reserve(w.size());
	for (wchar_t c : w) out += (c > 0 && c < 128) ? (char)c : '?';
	return out;
}

// Which language a query is built from, and therefore which line a row leads
// with. Not a display preference: the two produce completely different tokens.
enum class Lang { Zh = 0, En = 1 };

const std::string& ZhLine(const RegexEntryDef& e)
{
	static const std::string empty;
	if (!e.zh.empty()) return e.zh[0];
	if (!e.en.empty()) return e.en[0];
	return empty;
}

// An entry can carry more than one English name where GGG gave two things the
// same Chinese one, so they are all named rather than one of them picked.
std::string EnLine(const RegexEntryDef& e)
{
	std::string out;
	for (size_t i = 0; i < e.en.size(); i++) {
		if (i) out += " / ";
		out += e.en[i];
	}
	return out.empty() ? ZhLine(e) : out;
}

// The line in `lang`, and the one in the other language. Every label, warning
// and bookmark caption goes through these, so nothing can disagree about which
// language the panel is currently in.
std::string LineIn(const RegexEntryDef& e, Lang lang)
{
	return lang == Lang::Zh ? ZhLine(e) : EnLine(e);
}

std::string OtherLine(const RegexEntryDef& e, Lang lang)
{
	return lang == Lang::Zh ? EnLine(e) : ZhLine(e);
}

// The language-neutral identity of an entry, and therefore what a saved pick is
// stored as. See regex_state.h for why it is not the row number.
const std::string& KeyOf(const RegexEntryDef& e)
{
	static const std::string empty;
	return e.en.empty() ? empty : e.en[0];
}

std::string ToLowerAscii(std::string s)
{
	for (char& c : s) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
	return s;
}

// Per-page UI state. Kept separate from the data so switching pages and coming
// back does not lose the ticks -- a player comparing two pages should not be
// punished for looking.
struct PageState {
	std::vector<char> picked;      // parallel to the page's entries
	RegexGen::Corpus corpus;
	bool corpusReady = false;
	RegexGen::Result result;
	bool dirty = true;
	std::string search;
	int groupFilter = -1;          // -1 = every group
	bool t17Only = false;
	bool hideT17 = false;
	std::vector<int> visible;      // entry indices passing the filter
	bool filterDirty = true;
	// Algorithmic pages (vendor page, numeric section): ticks + values.
	RegexAlgo::AlgoSelection algo;
	// This page's output: its own picks plus, for a host page, its numeric
	// section (exile-appraiser store.ts pageCombined).
	RegexAlgo::CombineResult combined;
	// Item-mod values page (R7, RegexItemModList.vue): "only ticked" and the
	// filtered rows (ticked first, then at most kListCap more), rebuilt when the
	// search / group / ticks change.
	bool pickedOnly = false;
	char searchBuf[128] = {};      // the SearchField's buffer, mirrored into `search`
	std::vector<int> imvRows;
	int imvTotal = 0;
};

// R7: the item-mod values page of one game is built the first time it is opened
// (store.ts ensureItemMods) -- a few thousand modifiers, ~0.1-0.3 s to read and
// index -- on a worker thread; the panel polls `done` once per frame.
struct ItemModLoad {
	enum class Phase { Idle, Loading, Ready, Error };
	Phase phase = Phase::Idle;
	std::string err;
	long long ms = 0;
	int count = 0;
	std::vector<int> groupCounts;
	std::thread worker;
	std::atomic<bool> done{false};
	// Written by the worker before `done`, read by the panel after it.
	std::unique_ptr<RegexItemMods::Data> result;
	std::string resultErr;
	long long resultMs = 0;
};

// The panel is two-level: pick the game, then the list. Both games' catalogues
// are loaded, and every page belongs to exactly one of them, so this is the only
// vocabulary the selector needs.
constexpr const char* kGames[2] = {"poe1", "poe2"};

const char* GameLabel(const std::string& g)
{
	return g == "poe2" ? "PoE2" : "PoE1";
}

// Which modal wants to open. Raised by a button deep inside a child window and
// acted on at the top level, because OpenPopup and BeginPopupModal have to be
// called from the same ID scope or the popup simply never appears.
enum class Modal { None, Save, Rename, Delete, FolderAdd, FolderRename, FolderDelete, Paste, Template, SendPick, ImportPack };

// The left column: the merged overview or the page's own list (store.ts panelView).
enum class View { Page, Combined };

// ---- design-system pieces this panel needs and ui_widgets does not have ------
//
// The 2026-10-09 design (Regex*.dc.html, regex.css): rows are .it (tick + two
// lines), numeric / condition rows are .nr (name | controls | fragment, 1.1 /
// 1.6 / 1.3), groups are .gbox, the compact segmented control inside a row is
// .rx .pt-seg (13 px labels, 26 px tall). Everything is in design px through
// PobUi::D, so it follows the font-size setting and the tool density.

namespace Tok = PobUi::Tok;

float Dp(float v) { return std::floor(PobUi::D(v)); }

ImFont* SmallF()
{
	const PobUi::WidgetFonts& f = PobUi::Fonts();
	return f.small ? f.small : ImGui::GetFont();
}
ImFont* BodyF()
{
	const PobUi::WidgetFonts& f = PobUi::Fonts();
	return f.body ? f.body : ImGui::GetFont();
}
ImVec2 TextSz(ImFont* f, const char* s, float wrap = 0.0f)
{
	return f->CalcTextSizeA(f->FontSize, FLT_MAX, wrap, s);
}
void DrawTextAt(ImDrawList* dl, ImFont* f, ImVec2 p, ImU32 col, const char* s, float wrap = 0.0f)
{
	dl->AddText(f, f->FontSize, p, col, s, nullptr, wrap);
}
// Small text as an item, optionally wrapped at `wrap` px from here.
void SmallText(const char* s, ImU32 col = Tok::TextMuted, float wrap = 0.0f)
{
	ImGui::PushFont(SmallF());
	ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(col));
	if (wrap > 0.0f) ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap);
	ImGui::TextUnformatted(s);
	if (wrap > 0.0f) ImGui::PopTextWrapPos();
	ImGui::PopStyleColor();
	ImGui::PopFont();
}

// UTF-8 text broken into lines no wider than `width`, anywhere (CSS
// word-break: break-all). ImGui only breaks at spaces, and a search string is
// one long word: the output box and the fragments need this.
std::vector<std::string> WrapAnywhere(ImFont* f, const std::string& s, float width)
{
	std::vector<std::string> out;
	std::string line;
	float w = 0.0f;
	for (size_t i = 0; i < s.size();) {
		const unsigned char c = (unsigned char)s[i];
		const size_t n = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
		const std::string ch = s.substr(i, n);
		i += n;
		if (ch == "\n") {
			out.push_back(line);
			line.clear();
			w = 0.0f;
			continue;
		}
		const float cw = f->CalcTextSizeA(f->FontSize, FLT_MAX, 0.0f, ch.c_str()).x;
		if (!line.empty() && w + cw > width) {
			out.push_back(line);
			line.clear();
			w = 0.0f;
		}
		line += ch;
		w += cw;
	}
	if (!line.empty() || out.empty()) out.push_back(line);
	return out;
}
// Draws the wrapped lines as one item; returns its height.
float WrappedBlock(ImFont* f, const std::string& s, float width, ImU32 col)
{
	const std::vector<std::string> lines = WrapAnywhere(f, s, width);
	const ImVec2 p = ImGui::GetCursorScreenPos();
	const float lh = f->FontSize * 1.25f;
	ImDrawList* dl = ImGui::GetWindowDrawList();
	for (size_t i = 0; i < lines.size(); i++)
		DrawTextAt(dl, f, ImVec2(p.x, p.y + std::floor(lh * (float)i + (lh - f->FontSize) * 0.5f)), col, lines[i].c_str());
	const float h = std::ceil(lh * (float)lines.size());
	ImGui::Dummy(ImVec2(width, h));
	return h;
}

// .cb: a 15 px square, accent-filled with a tick when on; `mixed` = a dash
// (a folder some of whose bookmarks are picked).
void DrawCheck(ImDrawList* dl, ImVec2 p, bool on, bool mixed = false)
{
	const float s = Dp(15.0f);
	const ImVec2 b(p.x + s, p.y + s);
	if (on || mixed) {
		dl->AddRectFilled(p, b, Tok::Accent, Dp(4.0f));
		if (mixed) {
			dl->AddLine(ImVec2(p.x + s * 0.25f, p.y + s * 0.5f), ImVec2(p.x + s * 0.75f, p.y + s * 0.5f), Tok::OnAccent, std::max(1.5f, Dp(2.0f)));
		} else {
			const ImVec2 pts[3] = {ImVec2(p.x + s * 0.24f, p.y + s * 0.52f), ImVec2(p.x + s * 0.43f, p.y + s * 0.70f),
			                       ImVec2(p.x + s * 0.77f, p.y + s * 0.32f)};
			dl->AddPolyline(pts, 3, Tok::OnAccent, 0, std::max(1.5f, Dp(2.0f)));
		}
	} else {
		dl->AddRect(p, b, Tok::BorderStrong, Dp(4.0f), 0, std::max(1.0f, PobUi::D(1.5f)));
	}
}

// A clickable tick + label as one item (the "只看已勾選" style toggle).
bool CheckLabel(const char* id, const char* label, bool on)
{
	ImGui::PushID(id);
	const float s = Dp(15.0f), gap = Dp(6.0f);
	const ImVec2 ts = TextSz(SmallF(), label);
	const float h = std::max(s, ts.y);
	const ImVec2 p = ImGui::GetCursorScreenPos();
	const bool click = ImGui::InvisibleButton("##ck", ImVec2(s + gap + ts.x, h));
	ImDrawList* dl = ImGui::GetWindowDrawList();
	DrawCheck(dl, ImVec2(p.x, p.y + std::floor((h - s) * 0.5f)), on);
	DrawTextAt(dl, SmallF(), ImVec2(p.x + s + gap, p.y + std::floor((h - ts.y) * 0.5f)),
	           ImGui::IsItemHovered() ? Tok::Text : Tok::TextMuted, label);
	if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
	ImGui::PopID();
	return click;
}

// Pill (.t17 / .test / pt-pill): small text on a soft fill.
void Pill(const char* text, ImU32 fg, ImU32 bg)
{
	const ImVec2 ts = TextSz(SmallF(), text);
	const float padX = Dp(6.0f), h = ts.y + Dp(2.0f);
	const ImVec2 p = ImGui::GetCursorScreenPos();
	ImDrawList* dl = ImGui::GetWindowDrawList();
	dl->AddRectFilled(p, ImVec2(p.x + ts.x + padX * 2, p.y + h), bg, h * 0.5f);
	DrawTextAt(dl, SmallF(), ImVec2(p.x + padX, p.y + Dp(1.0f)), fg, text);
	ImGui::Dummy(ImVec2(ts.x + padX * 2, h));
}
float PillW(const char* text) { return TextSz(SmallF(), text).x + Dp(12.0f); }

// The row segmented control (.rx .pt-seg): every option is its own button and
// `lit` says which look selected -- exactly one for a choice, any number for the
// rarity multi-select. Returns the clicked option or -1.
float SegItemW(const char* label) { return TextSz(SmallF(), label).x + Dp(20.0f); }
float SegTogglesW(const char* const* labels, int n)
{
	float w = Dp(6.0f) + Dp(2.0f) * (float)(n > 0 ? n - 1 : 0);
	for (int i = 0; i < n; i++) w += SegItemW(labels[i]);
	return w;
}
int SegToggles(const char* id, const char* const* labels, const bool* lit, int n, const char* const* tips = nullptr)
{
	ImGui::PushID(id);
	const float h = Dp(26.0f), w = SegTogglesW(labels, n);
	const ImVec2 p = ImGui::GetCursorScreenPos();
	ImDrawList* dl = ImGui::GetWindowDrawList();
	dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), Tok::Surface2, Dp(7.0f));
	dl->AddRect(p, ImVec2(p.x + w, p.y + h), Tok::Border, Dp(7.0f), 0, 1.0f);
	int clicked = -1;
	float x = p.x + Dp(3.0f);
	const float ih = h - Dp(6.0f);
	for (int i = 0; i < n; i++) {
		const float iw = SegItemW(labels[i]);
		ImGui::SetCursorScreenPos(ImVec2(x, p.y + Dp(3.0f)));
		ImGui::PushID(i);
		if (ImGui::InvisibleButton("##sg", ImVec2(iw, ih))) clicked = i;
		const bool hov = ImGui::IsItemHovered();
		ImGui::PopID();
		if (hov && tips && tips[i] && *tips[i]) PobUi::Tooltip(tips[i]);
		const ImVec2 a(x, p.y + Dp(3.0f)), b(x + iw, p.y + Dp(3.0f) + ih);
		if (lit[i]) {
			dl->AddRectFilled(a, b, Tok::AccentSoft, Dp(5.0f));
			dl->AddRect(a, b, Tok::Accent, Dp(5.0f), 0, 1.0f);
		} else if (hov) {
			dl->AddRectFilled(a, b, Tok::Surface3, Dp(5.0f));
		}
		const ImVec2 ts = TextSz(SmallF(), labels[i]);
		DrawTextAt(dl, SmallF(), ImVec2(x + std::floor((iw - ts.x) * 0.5f), a.y + std::floor((ih - ts.y) * 0.5f)),
		           lit[i] || hov ? Tok::Text : Tok::TextMuted, labels[i]);
		if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
		x += iw + Dp(2.0f);
	}
	ImGui::SetCursorScreenPos(p);
	ImGui::Dummy(ImVec2(w, h));
	ImGui::PopID();
	return clicked;
}

// .vsep: a 1 px rule the height of a row control.
void VSep()
{
	const ImVec2 p = ImGui::GetCursorScreenPos();
	const float w = Dp(9.0f), h = Dp(22.0f);
	ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x + w * 0.5f, p.y + Dp(2.0f)), ImVec2(p.x + w * 0.5f, p.y + Dp(2.0f) + h),
	                                    Tok::Border, 1.0f);
	ImGui::Dummy(ImVec2(w, Dp(26.0f)));
}

// .gbox: a hairline-bordered group with an overline title; content between Begin / End.
struct GBox {
	ImVec2 p0;
	float w = 0.0f;
};
GBox GBoxBegin(const char* title, float width)
{
	GBox g;
	g.p0 = ImGui::GetCursorScreenPos();
	g.w = width;
	ImGui::SetCursorScreenPos(ImVec2(g.p0.x + Dp(6.0f), g.p0.y + Dp(6.0f)));
	ImGui::BeginGroup();
	if (title && *title) {
		ImGui::SetCursorScreenPos(ImVec2(g.p0.x + Dp(16.0f), ImGui::GetCursorScreenPos().y + Dp(4.0f)));
		SmallText(title, Tok::TextMuted);
		ImGui::Dummy(ImVec2(0, Dp(2.0f)));
	}
	return g;
}
void GBoxEnd(const GBox& g)
{
	ImGui::EndGroup();
	const float bottom = ImGui::GetItemRectMax().y + Dp(6.0f);
	ImGui::GetWindowDrawList()->AddRect(g.p0, ImVec2(g.p0.x + g.w, bottom), Tok::BorderSubtle, Dp(10.0f), 0, 1.0f);
	ImGui::SetCursorScreenPos(ImVec2(g.p0.x, bottom));
	ImGui::Dummy(ImVec2(g.w, Dp(8.0f)));
}

// A meter (.meter): track + fill in the length's colour.
void Meter(float fraction, float width, ImU32 col)
{
	const float h = std::max(3.0f, Dp(6.0f));
	const ImVec2 p = ImGui::GetCursorScreenPos();
	const float boxH = Dp(18.0f);
	ImGui::Dummy(ImVec2(width, boxH));
	const float y = p.y + std::floor((boxH - h) * 0.5f);
	ImDrawList* dl = ImGui::GetWindowDrawList();
	dl->AddRectFilled(ImVec2(p.x, y), ImVec2(p.x + width, y + h), Tok::Surface3, h * 0.5f);
	const float f = fraction < 0.0f ? 0.0f : (fraction > 1.0f ? 1.0f : fraction);
	if (f > 0.0f) dl->AddRectFilled(ImVec2(p.x, y), ImVec2(p.x + std::max(h, width * f), y + h), col, h * 0.5f);
}

// A dialog's button row that does NOT close the popup by itself (a paste that
// fails keeps the dialog open with its error). Right-aligned like DialogButtons.
PobUi::DialogResult DlgButtons(float inner, const char* cancel, const char* secondary, const char* primary,
                               bool primaryEnabled = true, bool danger = false)
{
	using PobUi::DialogResult;
	DialogResult res = DialogResult::None;
	ImGui::Dummy(ImVec2(0, Dp(12.0f)));
	const float gap = Dp(8.0f), minW = Dp(88.0f);
	float total = 0.0f;
	int n = 0;
	for (const char* b : {cancel, secondary, primary})
		if (b) {
			total += PobUi::ButtonWidth(b, PobUi::BtnSize::Md, nullptr, minW);
			n++;
		}
	total += gap * (float)(n > 0 ? n - 1 : 0);
	ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, inner - total));
	bool first = true;
	auto place = [&]() { if (!first) ImGui::SameLine(0, gap); first = false; };
	if (cancel) { place(); if (PobUi::Button(cancel, PobUi::BtnKind::Secondary, PobUi::BtnSize::Md, nullptr, minW)) res = DialogResult::Cancel; }
	if (secondary) { place(); if (PobUi::Button(secondary, PobUi::BtnKind::Secondary, PobUi::BtnSize::Md, nullptr, minW)) res = DialogResult::Secondary; }
	if (primary) {
		place();
		if (PobUi::Button(primary, danger ? PobUi::BtnKind::Danger : PobUi::BtnKind::Primary, PobUi::BtnSize::Md, nullptr, minW,
		                  primaryEnabled))
			res = DialogResult::Primary;
	}
	if (ImGui::IsKeyPressed(ImGuiKey_Escape)) res = DialogResult::Cancel;
	return res;
}

class RegexToolPanel : public IToolPanel {
public:
	bool Init(const ToolPanelHost& h) override
	{
		host_ = &h;
		exeDir_ = h.exeDir;
		// Test aid (POBTOOLS_REGEX_STATE, with POBTOOLS_TOOL_SHOT): a known state for a
		// screenshot of each design draft; regex_ui.json is neither read nor written.
		{
			wchar_t st[32] = {};
			const DWORD n = GetEnvironmentVariableW(L"POBTOOLS_REGEX_STATE", st, 32);
			if (n > 0 && n < 32)
				for (const wchar_t* c = st; *c; c++) testState_ += (char)(*c < 128 ? *c : '?');
			testMode_ = !testState_.empty() || GetEnvironmentVariableW(L"POBTOOLS_TOOL_SHOT", nullptr, 0) > 0;
		}
		game_ = h.game.empty() ? std::wstring(L"poe1") : h.game;
		dataOk_ = data_.Load(exeDir_, game_, &dataErr_);
		// Corpus pages first, in the data's order, so a corpus page's index here
		// is its index in data_.Pages(); the algorithmic pages of both games after
		// them (store.ts:116 appends algoPages to each catalogue). `algo_` is
		// filled once and never grows again: refs_ point into it.
		for (const char* g : kGames) {
			for (RegexAlgo::AlgoPage& p : RegexAlgo::AlgoPages(g, data_.Labels(g)))
				algo_.push_back(std::move(p));
			// R7 (store.ts:116): the item-mod values page, without entries until
			// it is first opened. Only for a game that has a catalogue at all.
			if (data_.HasGame(g)) algo_.push_back(RegexItemMods::MakePage(g, nullptr));
		}
		for (const RegexPageDef& p : data_.Pages()) refs_.push_back({&p, nullptr});
		for (const RegexAlgo::AlgoPage& p : algo_) refs_.push_back({nullptr, &p});
		pages_.resize(refs_.size());
		for (size_t i = 0; i < refs_.size(); i++) {
			pages_[i].picked.assign(refs_[i].Size(), 0);
			if (refs_[i].algo) pages_[i].algo.Reset(refs_[i].Size());
		}
		// The launcher's game is the opening answer, but only if it has a
		// catalogue: offering an empty PoE2 tab to someone whose Data folder
		// predates it would be a dead end, not information.
		selGame_ = NarrowAscii(game_);
		if (!data_.HasGame(selGame_)) {
			selGame_.clear();
			for (const char* g : kGames)
				if (selGame_.empty() && data_.HasGame(g)) selGame_ = g;
		}

		// R8 templates: a missing / broken file only disables the drop-down.
		{
			std::vector<std::string> errs;
			if (!RegexShare::LoadTemplates(exeDir_, templates_, errs, &templatesErr_))
				PobLog::Error("data", "regex_templates.json: " + templatesErr_);
			for (const std::string& e : errs) PobLog::Error("data", "regex_templates.json: " + e);
		}
		if (!testMode_) state_.Load(exeDir_);   // a fresh install has no file; the defaults are fine
		restoreState();
		lang_ = state_.lang == "en" ? Lang::En : Lang::Zh;
		bilingual_ = state_.bilingual;
		scopeCombined_ = state_.outScope != "page";
		view_ = state_.panelView == "combined" ? View::Combined : View::Page;
		bmTab_ = bmTabFollow_ = selGame_;
		// "送到 ExileAppraiser": sweep temp files left by earlier sessions, and
		// find the exe once so the button can say whether it will work.
		if (!testMode_) {
			RegexSend::SweepTempDir(RegexSend::DefaultTempDir(), RegexSend::kTempMaxAgeSeconds);
			relocateExileAppraiser();
		}
		if (state_.SaveBlocked())
			notice_ = u8"regex_ui.json 是較新版本的 PobTools 寫的（schema " + std::to_string(state_.loadedSchema) +
			          u8"），這個版本不會覆寫它：可以照常使用，但這次的變更（勾選、書籤）不會保存。";
		return true;   // a missing data file is a message, not a dead tab
	}

	const char* InitError() const override { return ""; }

	~RegexToolPanel() override
	{
		joinItemMods();
		// Only files old enough that ExileAppraiser has surely read them; the rest
		// go in a later session's sweep (regex_send.h).
		RegexSend::CleanupSession(sendFiles_, RegexSend::kSessionMinAgeSeconds);
	}

	void Frame() override
	{
		pollItemMods();
		if (!testApplied_) {
			testApplied_ = true;
			applyTestState();
		}
		if (!dataOk_ || testState_ == "dataerr") {
			drawDataError();
			return;
		}
		drawHeader();
		// Guarded because every panel below reaches for the current page. Load()
		// having succeeded does not promise a page survived the entry filter.
		if (!hasPage()) {
			PobUi::Banner("rx_nopage", PobUi::BannerTone::Warn, PobIcon::TriangleAlert, u8"這個版本沒有任何可用的清單",
			              nullptr, false, nullptr, false);
			return;
		}

		// Design: the list column takes the rest, the output / bookmark column is
		// 450 px (narrower windows give it at most 45%).
		const float avail = ImGui::GetContentRegionAvail().x;
		const float gap = Dp(14.0f);
		const float asideW = std::floor(std::min(PobUi::D(450.0f), avail * 0.45f));
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
		ImGui::BeginChild("##rx_left", ImVec2(avail - asideW - gap, 0), false);
		if (view_ == View::Combined) drawCombinedView();
		else drawList();
		ImGui::EndChild();
		ImGui::SameLine(0, gap);
		ImGui::BeginChild("##rx_right", ImVec2(0, 0), false);
		drawOutput();
		drawNotice();
		drawBookmarks();
		ImGui::EndChild();
		ImGui::PopStyleVar();

		drawModals();
	}

	// RegexStates "清單資料讀不到": what is missing and where it should be.
	void drawDataError()
	{
		std::string missing;
		for (const char* g : kGames) {
			const DWORD a = GetFileAttributesW((exeDir_ + L"Data\\regex_" + std::wstring(g, g + 4) + L".json").c_str());
			if (a == INVALID_FILE_ATTRIBUTES) missing += (missing.empty() ? "" : u8"、") + std::string("regex_") + g + ".json";
		}
		if (testState_ == "dataerr") missing = "regex_poe2.json";   // the screenshot of this state
		std::string desc = missing.empty() ? (u8"清單檔讀取失敗：" + dataErr_)
		                                   : (u8"安裝目錄的 Data 底下缺少 " + missing + u8"。");
		desc += u8"重新解壓縮完整的 PobTools 或更新翻譯資料。";
		if (PobUi::Banner("rx_dataerr", PobUi::BannerTone::Bad, PobIcon::CircleX, u8"讀不到搜尋字串資料", desc.c_str(), false,
		                  u8"開啟資料夾", false) == PobUi::BannerResult::Action)
			openDataDir_ = true;
	}

	void RunDeferred() override
	{
		if (!copyRequest_.empty()) {
			if (!testMode_) WriteClipboardUtf8(host_ ? host_->hostHwnd : nullptr, copyRequest_);
			copied_ = true;
			copyRequest_.clear();
		}
		if (!shareCopyRequest_.empty()) {
			shareCopied_ = (testMode_ || WriteClipboardUtf8(host_ ? host_->hostHwnd : nullptr, shareCopyRequest_)) ? 1 : 2;
			shareCopiedAt_ = std::chrono::steady_clock::now();
			shareCopyRequest_.clear();
			if (shareCopied_ == 1) PobUi::ShowToast((u8"已複製" + shareCopiedWhat_).c_str(), PobUi::Tone::Ok);
			else PobUi::ShowToast(u8"複製失敗", PobUi::Tone::Bad);
		}
		if (openDataDir_) {
			openDataDir_ = false;
			if (!testMode_) ShellExecuteW(nullptr, L"open", (exeDir_ + L"Data").c_str(), nullptr, nullptr, SW_SHOWNORMAL);
		}
		if (!testMode_) runSendRequests();
		// Written here rather than in Frame(): this is the one place a panel is
		// allowed to touch the disk, and it runs at most once per frame.
		flushState();
	}

	ToolCloseState RequestClose() override { return close_ = ToolCloseState::Closed; }
	ToolCloseState CloseState() const override { return close_; }
	void AbortClose() override
	{
		if (close_ == ToolCloseState::Closed) close_ = ToolCloseState::Open;
	}
	void Shutdown() override
	{
		// The backstop. RunDeferred normally gets there first, but a close can
		// land between a tick and the next deferred pass, and bookmarks are the
		// one thing here the player cannot recreate from anywhere else.
		flushState();
		joinItemMods();
	}
	PobUi::Density Density() const override { return PobUi::Density::Compact; }
	const char* PanelId() const override { return "regex"; }

private:
	bool hasPage() const { return page_ >= 0 && page_ < (int)refs_.size(); }
	PageState& st() { return pages_[page_]; }
	const PageState& st() const { return pages_[page_]; }

	// The current page is an algorithmic one (the vendor page); everything that
	// reads entries() / groups() is for corpus pages only.
	bool isAlgo() const { return refs_[page_].algo != nullptr; }
	const std::vector<RegexEntryDef>& entries() const
	{
		return refs_[page_].corpus->entries;
	}
	const std::vector<std::string>& groups() const
	{
		return refs_[page_].corpus->groups;
	}
	int limit() const { return refs_[page_].Limit(); }
	const std::string& pageNote() const
	{
		return refs_[page_].corpus ? refs_[page_].corpus->note : refs_[page_].algo->note;
	}

	// The numeric / condition section of page `host` (same game), as an index into
	// refs_; -1 if none. Step 40: a host may itself be algorithmic (the item-mod
	// values page carries the rarity | corruption section).
	int sectionIndexOf(int host) const
	{
		if (host < 0 || host >= (int)refs_.size() || refs_[host].IsSection()) return -1;
		for (int i = 0; i < (int)refs_.size(); i++)
			if (refs_[i].IsSection() && refs_[i].algo->sectionOf == refs_[host].Id() &&
			    refs_[i].algo->game == refs_[host].Game())
				return i;
		return -1;
	}

	// ---- games ---------------------------------------------------------------

	// The first page of a game, or -1. Also the answer to "does this game have a
	// catalogue at all", which is why the caller never asks that separately.
	int firstPageOf(const std::string& g) const
	{
		for (size_t i = 0; i < refs_.size(); i++)
			if (refs_[i].Game() == g && !refs_[i].IsSection()) return (int)i;
		return -1;
	}
	// Which game a page id belongs to; empty when no loaded catalogue has it.
	// That case is a bookmark saved against a list this build no longer ships,
	// and it is reported rather than quietly filed under one of the games.
	std::string gameOfPage(const std::string& id) const
	{
		for (const RegexAlgo::PageRef& p : refs_)
			if (p.Id() == id) return p.Game();
		return std::string();
	}

	// Every page's corpus is language-specific, so switching language throws them
	// all away rather than only the one on screen: coming back to a page whose
	// index was built from the other language would silently produce tokens that
	// match nothing.
	void invalidateCorpora()
	{
		for (PageState& ps : pages_) {
			ps.corpusReady = false;
			ps.dirty = true;
		}
		combinedDirty_ = true;
	}
	std::string pageId() const
	{
		return hasPage() ? refs_[page_].Id() : std::string();
	}
	std::string pageTitleById(const std::string& id) const
	{
		for (const RegexAlgo::PageRef& p : refs_)
			if (p.Id() == id) return p.Title();
		return id;
	}

	// ---- remembered state ----------------------------------------------------

	void flushState()
	{
		if (!stateDirty_ || saveFailed_) return;
		if (testMode_) {   // a screenshot never writes the player's file
			stateDirty_ = false;
			return;
		}
		// A newer build's file: not ours to overwrite (said once, at Init).
		if (state_.SaveBlocked()) {
			stateDirty_ = false;
			return;
		}
		// The return value used to be dropped. Bookmarks are the only thing this
		// tool holds that the player cannot rebuild from anywhere else, so a save
		// that quietly did nothing would surface days later as "my bookmarks are
		// gone" with nothing to point at.
		if (!state_.Save(exeDir_)) {
			PobLog::Error("save", u8"regex_ui.json 存檔失敗（書籤與勾選沒有保存）");
			// RunDeferred runs EVERY FRAME. Retrying here without a brake meant
			// sixty failed opens a second -- and, before the log learned to
			// collapse repeats, sixty identical lines a second with it.
			// The moment worth retrying is the next time the user changes
			// something, not the next frame; `stateDirty_` stays set so that
			// retry still writes everything.
			saveFailed_ = true;
			return;
		}
		stateDirty_ = false;
	}

	void restoreState()
	{
		// The remembered game wins over the launcher's, but only if it still has
		// a catalogue; otherwise Init's answer stands.
		if (!state_.game.empty() && data_.HasGame(state_.game)) selGame_ = state_.game;
		page_ = firstPageOf(selGame_);
		for (size_t i = 0; i < refs_.size(); i++)
			if (refs_[i].Id() == state_.page && refs_[i].Game() == selGame_ && !refs_[i].IsSection())
				page_ = (int)i;
		mode_ = ModeFromId(state_.mode);

		// Bookmarks written before the split carry no game. Filling it in from
		// the page id -- and writing it back -- is what keeps them visible: the
		// list is filtered by game, and an unfilled one would have no column to
		// appear in. One that names a page this build no longer ships stays
		// empty on purpose and is counted in the panel instead of vanishing.
		bool filled = false;
		for (RegexBookmark& b : state_.bookmarks) {
			if (!b.game.empty()) continue;
			const std::string g = gameOfPage(b.page);
			if (g.empty()) continue;
			b.game = g;
			filled = true;
			stateDirty_ = true;
		}
		// A filled-in bookmark may name a folder: list it and keep the order
		// invariant (store.ts onCatalogueReady).
		if (filled) RegexFolders::Normalize(state_);

		// Every page's saved ticks (a section's live in its host's `num`) and
		// every algorithmic page's values (a section's under its host id).
		int missedTotal = 0;
		std::string firstPage;
		for (size_t i = 0; i < refs_.size(); i++) {
			const RegexAlgo::PageRef& ref = refs_[i];
			if (ref.algo) {
				if (const RegexValueList* m = state_.NumericOf(RegexAlgo::NumericKeyOf(ref.Id())))
					for (const auto& kv : *m)
						if (ownsValue((int)i, kv.first)) pages_[i].algo.values[kv.first] = kv.second;
			}
			// The item-mod values page has no entries until it is loaded: its
			// ticks are restored then (finishItemMods), else every saved key
			// would read as "not found" (store.ts onCatalogueReady).
			if (isItemPage((int)i)) continue;
			const std::optional<RegexEmbed::Applied> r = RegexEmbed::SavedPicksOf(ref, state_);
			if (!r) continue;
			setTicks((int)i, r->picked);
			if (r->missed > 0) {
				missedTotal += r->missed;
				if (firstPage.empty()) firstPage = refs_[hostIndexOf((int)i)].Title();
			}
		}
		if (missedTotal > 0)
			notice_ = u8"上次的勾選有 " + std::to_string(missedTotal) + u8" 項（" + firstPage +
			          u8" 等）在目前的資料裡找不到，可能是賽季更新後詞條有變動。";
		// Saved ticks on an item-mod values page, or it is the remembered page:
		// load it in the background now (it restores itself when ready).
		for (const char* g : kGames) {
			const int ip = itemPageIndex(g);
			if (ip < 0) continue;
			bool want = (selGame_ == g && page_ == ip);
			for (const RegexPagePicks& c : state_.current)
				if (c.page == refs_[ip].Id() && !c.keys.empty()) want = true;
			if (want) startItemMods(g);
		}
	}

	static RegexGen::Mode ModeFromId(const std::string& id)
	{
		return id == "all" ? RegexGen::Mode::All
		     : id == "none" ? RegexGen::Mode::None : RegexGen::Mode::Any;
	}
	const char* modeId() const
	{
		return mode_ == RegexGen::Mode::All ? "all"
		     : mode_ == RegexGen::Mode::None ? "none" : "any";
	}

	// Overwrite one page's ticks (corpus or algorithmic) from a list of indices.
	void setTicks(int idx, const std::vector<int>& picked)
	{
		std::vector<char>& v = refs_[idx].algo ? pages_[idx].algo.picked : pages_[idx].picked;
		std::fill(v.begin(), v.end(), (char)0);
		for (int i : picked)
			if (i >= 0 && i < (int)v.size()) v[i] = 1;
		pages_[idx].dirty = true;
		pages_[idx].filterDirty = true;
	}

	// Does algorithmic page `idx` own value `id`? A host that is itself algorithmic
	// (the item-mod values page) and its rarity | corruption section keep their
	// values under the same store key (the host id; entry ids never overlap), so
	// each page only takes -- and only writes back -- its own entries' values.
	// Otherwise a stale copy on one page would overwrite the other's newer value.
	bool ownsValue(int idx, const std::string& id) const
	{
		if (!refs_[idx].algo) return false;
		if (refs_[idx].IsSection()) {
			for (const RegexAlgo::AlgoEntry& e : refs_[idx].algo->entries)
				if (e.def.id == id) return true;
			return false;
		}
		const int sec = sectionIndexOf(idx);
		if (sec >= 0)
			for (const RegexAlgo::AlgoEntry& e : refs_[sec].algo->entries)
				if (e.def.id == id) return false;
		return true;
	}

	// store.ts syncCurrent: page idx's ticks -> its saved record. A section's
	// ticks are its host's `num`; the vendor page's are entry ids.
	void syncCurrent(int idx)
	{
		const RegexAlgo::PageRef& ref = refs_[idx];
		const RegexEmbed::Keys k = RegexEmbed::PageKeysOf(ref, picksOf(idx));
		if (ref.IsSection()) {
			state_.PicksFor(ref.algo->sectionOf).num = k.keys;
		} else {
			RegexPagePicks& p = state_.PicksFor(ref.Id());
			p.keys = k.keys;
			p.alt = k.alt;
		}
		markStateDirty();
	}

	// store.ts setValue: an algorithmic page's values -> numeric[store key].
	void syncValues(int idx)
	{
		if (!refs_[idx].algo) return;
		RegexValueList& dst = state_.NumericFor(RegexAlgo::NumericKeyOf(refs_[idx].Id()));
		for (const auto& kv : pages_[idx].algo.values) {
			if (!ownsValue(idx, kv.first)) continue;
			const RegexFrag::AlgoValue* had = RegexValueFind(dst, kv.first);
			if (!had || had->min != kv.second.min || had->max != kv.second.max ||
			    had->choice != kv.second.choice || RegexFrag::HasChoice(*had) != RegexFrag::HasChoice(kv.second))
				RegexValueSet(dst, kv.first, kv.second);
		}
		markStateDirty();
	}

	void markStateDirty()
	{
		stateDirty_ = true;
		saveFailed_ = false;   // a fresh change deserves a fresh attempt
	}

	// Everything that changes what is ticked funnels through here, so there is
	// exactly one place that could forget to persist.
	void picksChanged()
	{
		st().dirty = true;
		combinedDirty_ = true;
		st().filterDirty = true;   // ticked rows move to the top; see refreshFilter
		copied_ = false;
		syncCurrent(page_);
		state_.game = selGame_;
		state_.page = pageId();
		state_.mode = modeId();
	}

	// A tick or a value on an algorithmic page changed.
	void algoChanged(int idx)
	{
		pages_[idx].dirty = true;
		pages_[idx].filterDirty = true;   // the item-mod page keeps ticked rows on top
		combinedDirty_ = true;
		if (refs_[idx].IsSection()) {
			// The section's output is part of its host page's string.
			const int host = hostIndexOf(idx);
			if (host != idx) pages_[host].dirty = true;
		}
		copied_ = false;
		syncCurrent(idx);
		syncValues(idx);
	}

	// ---- header ----------------------------------------------------------------

	// One header row (Regex.dc.html .pt-thead): icon + name | game | view |
	// list (page view) | mode | ... | copy / paste share code | more | bilingual.
	// Items flow left to right and wrap to a second row when the window is narrow.
	void drawHeader()
	{
		const float H = PobUi::ControlH();
		const float gap = Dp(10.0f);
		const ImVec2 origin = ImGui::GetCursorScreenPos();
		const float right = origin.x + ImGui::GetContentRegionAvail().x;
		float x = origin.x, y = origin.y;
		auto slot = [&](float w) {
			if (x > origin.x && x + w > right) {
				x = origin.x;
				y += H + Dp(8.0f);
			}
			ImGui::SetCursorScreenPos(ImVec2(x, y));
			x += w + gap;
		};
		auto vcenter = [&](float h) { ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, y + std::floor((H - h) * 0.5f))); };

		// icon + name
		{
			const PobUi::WidgetFonts& wf = PobUi::Fonts();
			const float iconPx = Dp(20.0f);
			const char* title = "Poe Regex";
			const float hw = wf.heading ? wf.heading->CalcTextSizeA(wf.headingPx > 0 ? wf.headingPx : wf.heading->FontSize, FLT_MAX, 0.0f, title).x
			                            : ImGui::CalcTextSize(title).x;
			const float iw = PobUi::IconWidth(PobIcon::CodeXml, iconPx);
			slot(iw + (iw > 0 ? Dp(8.0f) : 0.0f) + hw);
			PobUi::IconAt(ImGui::GetWindowDrawList(), ImVec2(ImGui::GetCursorScreenPos().x, y + std::floor((H - iconPx) * 0.5f)),
			              PobIcon::CodeXml, Tok::AccentText, iconPx);
			ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x + iw + (iw > 0 ? Dp(8.0f) : 0.0f), y));
			vcenter(ImGui::GetTextLineHeight());
			PobUi::Heading(title);
		}

		// game: only games with a catalogue are offered (an option that leads to
		// an empty panel is worse than not seeing it; --regex-selftest fails when
		// either file is missing from the install, so this cannot hide a packaging
		// mistake).
		{
			std::vector<const char*> labels;
			std::vector<std::string> ids;
			int sel = 0;
			for (const char* g : kGames) {
				if (firstPageOf(g) < 0) continue;
				if (selGame_ == g) sel = (int)ids.size();
				ids.push_back(g);
				labels.push_back(GameLabel(g));
			}
			slot(PobUi::SegmentedWidth(labels.data(), (int)labels.size()));
			if (PobUi::Segmented("##rxgame", &sel, labels.data(), (int)labels.size())) switchGame(ids[sel]);
		}

		// view: the merged overview or the page's own list (RegexPanel.vue view seg)
		{
			const std::string merged = u8"已選（合併）· " + std::to_string(mergeIdx().size()) + u8" 頁";
			const char* labels[2] = {merged.c_str(), u8"單頁清單"};
			int sel = view_ == View::Combined ? 0 : 1;
			slot(PobUi::SegmentedWidth(labels, 2));
			if (PobUi::Segmented("##rxview", &sel, labels, 2)) setView(sel == 0 ? View::Combined : View::Page);
			if (ImGui::IsItemHovered()) PobUi::Tooltip(u8"已選（合併）：目前遊戲所有有勾選的清單、自訂文字與排除詞，合成一串");
		}

		// the list (page view): "地圖詞綴 (5)", each option noted with its size
		if (view_ == View::Page) {
			std::vector<int> idx;
			std::vector<std::string> lab, note;
			int sel = 0;
			for (size_t i = 0; i < refs_.size(); i++) {
				if (refs_[i].Game() != selGame_ || refs_[i].IsSection()) continue;
				const int n = pagePickCount((int)i);
				if ((int)i == page_) sel = (int)idx.size();
				idx.push_back((int)i);
				lab.push_back(refs_[i].Title() + (n ? " (" + std::to_string(n) + ")" : std::string()));
				const bool itemPending = isItemPage((int)i) && imv_[GameIdx(selGame_)].phase != ItemModLoad::Phase::Ready;
				note.push_back(itemPending ? std::string(u8"第一次開啟時載入") : std::to_string(refs_[i].Size()) + u8" 條");
			}
			std::vector<const char*> lp, np;
			for (size_t i = 0; i < lab.size(); i++) {
				lp.push_back(lab[i].c_str());
				np.push_back(note[i].c_str());
			}
			const float w = std::max(Dp(190.0f), PobUi::SelectFitWidth(lp.data(), (int)lp.size()));
			slot(w);
			if (PobUi::Select("##rxpage", &sel, lp.data(), np.data(), (int)lp.size(), w) && idx[sel] != page_) switchPage(idx[sel]);
		}

		// the three shapes the client's search has; changes the string, not the picks
		{
			const char* labels[3] = {u8"含任一個", u8"全部都有", u8"一個都沒有"};
			const char* tips[3] = {u8"任一項中就選（一個 term 裡用 | 串起來）", u8"每一項都要中（每項各自一個 term）",
			                       u8"一項都不能中：產生排除字串（開頭的 !），把有這些詞綴的東西藏起來"};
			int m = (int)mode_;
			slot(PobUi::SegmentedWidth(labels, 3));
			if (PobUi::SegmentedEx("##rxmode", &m, labels, 3, nullptr, tips) && m != (int)mode_) {
				mode_ = (RegexGen::Mode)m;
				for (PageState& ps : pages_) ps.dirty = true;
				combinedDirty_ = true;
				copied_ = false;
				state_.mode = modeId();
				markStateDirty();
			}
		}

		// right end: share code buttons, the more menu, the bilingual switch
		const bool copiedNow = shareCopied_ == 1 && std::chrono::steady_clock::now() - shareCopiedAt_ < std::chrono::milliseconds(2500);
		const char* copyLbl = copiedNow ? u8"已複製" : u8"複製分享碼";
		const char* copyIcon = copiedNow ? PobIcon::Check : PobIcon::Copy;
		const char* pasteLbl = u8"貼上分享碼";
		const char* biLbl = u8"雙語顯示";
		const float smH = Dp(28.0f);
		const float wCopy = PobUi::ButtonWidth(copyLbl, PobUi::BtnSize::Sm, copyIcon);
		const float wPaste = PobUi::ButtonWidth(pasteLbl, PobUi::BtnSize::Sm);
		const float wMore = PobUi::ButtonWidth("##rxmore", PobUi::BtnSize::Sm, PobIcon::MoreHorizontal, smH);
		const float wBi = TextSz(SmallF(), biLbl).x + Dp(8.0f) + PobUi::SwitchWidth();
		const float blockW = wCopy + gap + wPaste + Dp(6.0f) + wMore + gap + wBi;
		if (x + blockW > right && x > origin.x) {
			x = origin.x;
			y += H + Dp(8.0f);
		}
		x = std::max(x, right - blockW);
		ImGui::SetCursorScreenPos(ImVec2(x, y + std::floor((H - smH) * 0.5f)));
		const bool any = hasShareable();
		if (copiedNow) {
			ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(Tok::Success));
			PobUi::Button(copyLbl, PobUi::BtnKind::Secondary, PobUi::BtnSize::Sm, copyIcon);
			ImGui::PopStyleColor();
		} else if (PobUi::Button(copyLbl, PobUi::BtnKind::Secondary, PobUi::BtnSize::Sm, copyIcon, 0.0f, any)) {
			makeShareCode();
		}
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			PobUi::Tooltip(any ? u8"把目前遊戲所有清單的勾選、數值、自訂文字與排除詞壓成一串分享碼。分享碼可以直接貼進流亡鑑價（ExileAppraiser），兩邊格式相同。"
			                   : u8"沒有任何勾選、自訂文字或排除詞時沒有東西可以分享。");
		ImGui::SameLine(0, gap);
		if (PobUi::Button(pasteLbl, PobUi::BtnKind::Secondary, PobUi::BtnSize::Sm)) openPaste();
		if (ImGui::IsItemHovered()) PobUi::Tooltip(u8"貼上別人給的分享碼並套用（套用前會先確認）");
		ImGui::SameLine(0, Dp(6.0f));
		if (PobUi::Button("##rxmore", PobUi::BtnKind::Ghost, PobUi::BtnSize::Sm, PobIcon::MoreHorizontal, smH) ||
		    testOpenMore_) {
			testOpenMore_ = false;
			ImGui::OpenPopup("##rxmoremenu");
		}
		if (ImGui::IsItemHovered()) PobUi::Tooltip(u8"範本、送到 ExileAppraiser、匯入書籤包");
		drawMoreMenu();
		ImGui::SameLine(0, gap);
		ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, y + std::floor((H - TextSz(SmallF(), biLbl).y) * 0.5f)));
		SmallText(biLbl);
		ImGui::SameLine(0, Dp(8.0f));
		ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, y + std::floor((H - ImGui::GetFrameHeight()) * 0.5f)));
		if (PobUi::Switch("##rxbi", &bilingual_)) {
			state_.bilingual = bilingual_;
			markStateDirty();
		}
		if (ImGui::IsItemHovered()) PobUi::Tooltip(u8"在每一列下面加上另一種語言的原文");

		ImGui::SetCursorScreenPos(ImVec2(origin.x, y + H + Dp(10.0f)));
		ImGui::Dummy(ImVec2(0, 0));
	}

	// The header's more menu: templates of this game, 送到 ExileAppraiser, 匯入書籤包,
	// where ExileAppraiser.exe is. (Not in the design: PobTools / exile-appraiser
	// features the drafts do not cover, put where the design keeps secondary actions.)
	void drawMoreMenu()
	{
		// under the button that opened it, right-aligned (not wherever the mouse is)
		ImGui::SetNextWindowPos(ImVec2(ImGui::GetItemRectMax().x, ImGui::GetItemRectMax().y + Dp(4.0f)), ImGuiCond_Appearing, ImVec2(1.0f, 0.0f));
		if (!PobUi::BeginMenuPopup("##rxmoremenu")) return;
		int mine = 0;
		for (const RegexShare::Template& t : templates_) mine += t.game == selGame_ ? 1 : 0;
		PobUi::Overline((std::string(u8"套用範本（") + GameLabel(selGame_) + u8"）").c_str());
		if (!templatesErr_.empty()) SmallText((u8"範本檔載入失敗：" + templatesErr_).c_str(), Tok::Danger);
		else if (mine == 0) SmallText(u8"這個遊戲沒有內建範本", Tok::TextFaint);
		for (int i = 0; i < (int)templates_.size(); i++) {
			const RegexShare::Template& t = templates_[i];
			if (t.game != selGame_) continue;
			ImGui::PushID(i);
			if (PobUi::MenuRow(PobIcon::FileText, t.nameZh.c_str())) {
				tplPending_ = i;
				modal_ = Modal::Template;
			}
			if (ImGui::IsItemHovered() && !t.descZh.empty()) PobUi::Tooltip(t.descZh.c_str());
			ImGui::PopID();
		}
		PobUi::MenuSeparator();
		const int have = sendableBookmarks();
		if (PobUi::MenuRow(PobIcon::Share, u8"送到 ExileAppraiser…", nullptr, have > 0)) openSendPick();
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
			if (std::chrono::steady_clock::now() - sendLocAt_ > std::chrono::seconds(3)) relocateWanted_ = true;
			std::string tip = have == 0 ? std::string(u8"還沒有書籤可以送：先「存成書籤」。")
			                            : std::string(u8"選幾筆書籤（或整個資料夾，PoE1 / PoE2 可以混選）交給 ExileAppraiser，加進它的書籤。"
			                                          u8"它會跳出確認框，按「加入」才寫入。需要 v0.2.1 以上。");
			tip += sendLoc_.exe.empty() ? std::string(u8"\n找不到 ExileAppraiser.exe：送出時會請你手動指定。")
			                            : u8"\n位置：" + RegexSend::Narrow(sendLoc_.exe) + u8"（" + RegexSend::SourceLabel(sendLoc_.source) + u8"）";
			if (sendLoc_.manualMissing) tip += u8"\n手動指定的檔案已不存在：" + state_.exileAppraiserExe;
			PobUi::Tooltip(tip.c_str());
		}
		if (PobUi::MenuRow(PobIcon::Download, u8"匯入書籤包…")) openImport();
		if (ImGui::IsItemHovered())
			PobUi::Tooltip(u8"貼上別人給的書籤包（或 ExileAppraiser 產的），加進書籤：同名會改名加「 (2)」，不動目前的勾選。加入前會先預覽。");
		PobUi::MenuSeparator();
		if (PobUi::MenuRow(PobIcon::FolderOpen, u8"手動指定 ExileAppraiser.exe…")) pickRequest_ = true;
		if (PobUi::MenuRow(PobIcon::X, u8"清除手動指定", nullptr, !state_.exileAppraiserExe.empty())) {
			state_.exileAppraiserExe.clear();
			markStateDirty();
			relocateWanted_ = true;
		}
		PobUi::EndMenuPopup();
	}

	int pickCount() const
	{
		if (isAlgo()) return st().algo.Count();
		int n = 0;
		for (char c : st().picked) n += c ? 1 : 0;
		return n;
	}

	// ---- the list ------------------------------------------------------------

	// The page's own explanation, above everything else on the page.
	void drawPageNote()
	{
		const std::string& note = pageNote();
		if (note.empty()) return;
		SmallText(note.c_str(), Tok::TextMuted, ImGui::GetContentRegionAvail().x);
		ImGui::Dummy(ImVec2(0, Dp(6.0f)));
	}

	void drawList()
	{
		drawPageNote();
		if (isAlgo()) {
			if (isItemPage(page_)) {
				// RegexItemMods.dc.html: the rarity | corruption card above the list, loaded or not
				const int sec = sectionIndexOf(page_);
				if (sec >= 0) drawSection(page_, sec);
				drawItemModPage(page_);
			} else {
				drawAlgoPage(page_);
			}
			return;
		}
		const int sec = sectionIndexOf(page_);
		if (sec >= 0) drawSection(page_, sec);
		PageState& s = st();

		// toolbar: search | group | T17 | ticked count | select all / clear
		const float gap = Dp(8.0f);
		const float avail = ImGui::GetContentRegionAvail().x;
		std::vector<const char*> gl;
		std::string gAll = u8"全部分類";
		gl.push_back(gAll.c_str());
		for (const std::string& g : groups()) gl.push_back(g.c_str());
		const float groupW = groups().empty() ? 0.0f : std::max(Dp(110.0f), PobUi::SelectFitWidth(gl.data(), (int)gl.size()));
		const char* t17Labels[3] = {u8"全部", u8"只看 T17", u8"排除 T17"};
		const bool t17 = pageHasT17();
		const float t17W = t17 ? PobUi::SegmentedWidth(t17Labels, 3) : 0.0f;
		const std::string count = u8"已勾選 " + std::to_string(pickCount()) + " / " + std::to_string((int)refs_[page_].Size());
		const float countW = TextSz(SmallF(), count.c_str()).x;
		const float btnW = PobUi::ButtonWidth(u8"全選", PobUi::BtnSize::Sm) + PobUi::ButtonWidth(u8"清除", PobUi::BtnSize::Sm) + Dp(4.0f);
		const float fixed = (groupW > 0 ? groupW + gap : 0) + (t17W > 0 ? t17W + gap : 0) + countW + gap + btnW;
		const bool oneRow = avail - fixed >= Dp(160.0f);
		const float searchW = oneRow ? avail - fixed - gap : avail;
		syncSearchBuf(s);
		if (PobUi::SearchField("##rx_search", s.searchBuf, (int)sizeof s.searchBuf, u8"搜尋中英文…", searchW)) {
			s.search = s.searchBuf;
			s.filterDirty = true;
		}
		const float rowY = ImGui::GetItemRectMin().y, H = ImGui::GetItemRectSize().y;
		auto next = [&](float w, bool first) {
			if (first && !oneRow) ImGui::Dummy(ImVec2(0, Dp(2.0f)));
			else ImGui::SameLine(0, gap);
			(void)w;
		};
		bool first = true;
		if (groupW > 0) {
			next(groupW, first);
			first = false;
			int sel = s.groupFilter + 1;
			if (PobUi::Select("##rx_group", &sel, gl.data(), nullptr, (int)gl.size(), groupW) && sel - 1 != s.groupFilter) {
				s.groupFilter = sel - 1;
				s.filterDirty = true;
			}
		}
		if (t17) {
			next(t17W, first);
			first = false;
			int cur = s.t17Only ? 1 : (s.hideT17 ? 2 : 0);
			if (PobUi::Segmented("##rx_t17", &cur, t17Labels, 3)) {
				s.t17Only = cur == 1;
				s.hideT17 = cur == 2;
				s.filterDirty = true;
			}
		}
		next(countW, first);
		const float lineY = oneRow ? rowY : ImGui::GetCursorScreenPos().y;
		ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, lineY + std::floor((H - TextSz(SmallF(), "A").y) * 0.5f)));
		SmallText(count.c_str());
		ImGui::SameLine(0, gap);
		ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, lineY + std::floor((H - Dp(28.0f)) * 0.5f)));
		if (s.filterDirty) refreshFilter();
		if (PobUi::Button(u8"全選", PobUi::BtnKind::Ghost, PobUi::BtnSize::Sm)) {
			for (int i : s.visible) s.picked[i] = 1;
			picksChanged();
		}
		if (ImGui::IsItemHovered()) PobUi::Tooltip((u8"把目前篩選出來的 " + std::to_string(s.visible.size()) + u8" 項全部勾選").c_str());
		ImGui::SameLine(0, Dp(4.0f));
		if (PobUi::Button(u8"清除", PobUi::BtnKind::Ghost, PobUi::BtnSize::Sm, nullptr, 0.0f, pickCount() > 0)) {
			std::fill(s.picked.begin(), s.picked.end(), (char)0);
			picksChanged();
		}
		ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, std::max(ImGui::GetCursorScreenPos().y, lineY + H)));
		ImGui::Dummy(ImVec2(0, Dp(4.0f)));

		if (s.filterDirty) refreshFilter();
		if (s.visible.empty()) {
			// RegexStates "搜尋沒有結果"
			const bool filtered = !s.search.empty() || s.groupFilter >= 0 || s.t17Only || s.hideT17;
			const std::string title = !s.search.empty() ? u8"找不到「" + s.search + u8"」" : std::string(u8"沒有符合的項目");
			if (PobUi::EmptyState("rx_none", PobIcon::Search, title.c_str(), u8"試試英文或較短的關鍵字。分類與T17篩選也會影響結果。",
			                      filtered ? u8"清除篩選" : nullptr)) {
				s.search.clear();
				s.searchBuf[0] = 0;
				s.groupFilter = -1;
				s.t17Only = s.hideT17 = false;
				s.filterDirty = true;
			}
			return;
		}

		// .it rows: a row is one line, or two with the bilingual switch, so the
		// clipper is given the height and every row is PINNED to it.
		ImGui::BeginChild("##rx_rows", ImVec2(0, 0), false);
		const float padY = Dp(6.0f);
		const float textH = BodyF()->FontSize + (bilingual_ ? SmallF()->FontSize + Dp(2.0f) : 0.0f);
		const float rowH = std::floor(textH + padY * 2.0f);
		const float top = ImGui::GetCursorPosY();
		ImGuiListClipper clip;
		clip.Begin((int)s.visible.size(), rowH);
		while (clip.Step()) {
			for (int row = clip.DisplayStart; row < clip.DisplayEnd; row++) {
				ImGui::SetCursorPosY(top + row * rowH);
				drawRow(s, s.visible[row], rowH);
			}
		}
		ImGui::EndChild();
	}

	void syncSearchBuf(PageState& s)
	{
		if (s.search != s.searchBuf) {
			const size_t n = std::min(s.search.size(), sizeof s.searchBuf - 1);
			memcpy(s.searchBuf, s.search.data(), n);
			s.searchBuf[n] = 0;
		}
	}

	void drawRow(PageState& s, int idx, float rowH)
	{
		const RegexEntryDef& e = entries()[idx];
		ImGui::PushID(idx);
		const float w = ImGui::GetContentRegionAvail().x;
		const ImVec2 p = ImGui::GetCursorScreenPos();
		const bool click = ImGui::InvisibleButton("##row", ImVec2(w, rowH - Dp(2.0f)));
		const bool hov = ImGui::IsItemHovered();
		const bool on = s.picked[idx] != 0;
		ImDrawList* dl = ImGui::GetWindowDrawList();
		if (on || hov)
			dl->AddRectFilled(p, ImVec2(p.x + w, p.y + rowH - Dp(2.0f)), on ? Tok::AccentSoft : Tok::Surface2, Dp(6.0f));
		const float padX = Dp(10.0f), padY = Dp(6.0f);
		DrawCheck(dl, ImVec2(p.x + padX, p.y + padY + Dp(3.0f)), on);
		float tx = p.x + padX + Dp(15.0f) + Dp(10.0f);
		const float maxW = p.x + w - padX - tx;
		std::string label = LineIn(e, lang_);
		ImGui::PushClipRect(ImVec2(tx, p.y), ImVec2(p.x + w - padX, p.y + rowH), true);
		DrawTextAt(dl, BodyF(), ImVec2(tx, p.y + padY), Tok::Text, label.c_str());
		float lx = tx + TextSz(BodyF(), label.c_str()).x;
		const size_t extra = (lang_ == Lang::Zh ? e.zh.size() : e.en.size());
		if (e.t17) {
			const ImVec2 ts = TextSz(SmallF(), "T17");
			const float px = lx + Dp(6.0f), py = p.y + padY + std::floor((BodyF()->FontSize - ts.y) * 0.5f);
			dl->AddRectFilled(ImVec2(px, py - Dp(1.0f)), ImVec2(px + ts.x + Dp(12.0f), py + ts.y + Dp(1.0f)), Tok::WarningSoft, ts.y);
			DrawTextAt(dl, SmallF(), ImVec2(px + Dp(6.0f), py), Tok::Warning, "T17");
			lx = px + ts.x + Dp(12.0f);
		}
		if (extra > 1) {
			const std::string more = u8"另有 " + std::to_string(extra - 1) + u8" 行";
			DrawTextAt(dl, SmallF(), ImVec2(lx + Dp(6.0f), p.y + padY + std::floor((BodyF()->FontSize - SmallF()->FontSize) * 0.5f)),
			           Tok::TextFaint, more.c_str());
		}
		if (bilingual_) {
			const std::string other = OtherLine(e, lang_);
			DrawTextAt(dl, SmallF(), ImVec2(tx, p.y + padY + BodyF()->FontSize + Dp(2.0f)),
			           other.empty() ? Tok::TextFaint : Tok::TextMuted,
			           other.empty() ? (lang_ == Lang::Zh ? u8"沒有英文對照" : u8"沒有中文對照") : other.c_str());
		}
		(void)maxW;
		ImGui::PopClipRect();
		if (click) {
			s.picked[idx] = on ? 0 : 1;
			picksChanged();
		}
		if (hov) {
			ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
			drawEntryTooltip(e);
		}
		ImGui::PopID();
	}

	void drawEntryTooltip(const RegexEntryDef& e)
	{
		ImGui::BeginTooltip();
		ImGui::PushTextWrapPos(Dp(380.0f));
		for (const std::string& l : e.zh) ImGui::TextUnformatted(l.c_str());
		if (!e.en.empty()) {
			ImGui::Separator();
			for (const std::string& l : e.en) SmallText(l.c_str());
		}
		if (!e.affixZh.empty()) {
			ImGui::Separator();
			SmallText((u8"來源詞綴：" + e.affixZh).c_str());
		}
		// What the search reads besides the line: it explains why a token is
		// longer than the line alone would need.
		// The other wordings of the same modifier (alts: 「一個」 at 1, 「#個」 at 2)
		// are the line itself at another roll, not extra text: listed on their own.
		// The data also carries them in the hidden text, so they are left out there.
		const std::vector<std::string>& alts = (lang_ == Lang::Zh) ? e.altZh : e.altEn;
		if (!alts.empty()) {
			ImGui::Separator();
			SmallText(u8"同一詞綴的其他寫法：");
			for (const std::string& a : alts) SmallText((u8"· " + a).c_str());
		}
		std::vector<std::string> hidden;
		for (const std::string& h : (lang_ == Lang::Zh) ? e.hiddenZh : e.hiddenEn)
			if (std::find(alts.begin(), alts.end(), h) == alts.end()) hidden.push_back(h);
		if (!hidden.empty()) {
			ImGui::Separator();
			SmallText(u8"遊戲搜尋也會比對：");
			const size_t shown = hidden.size() < 4 ? hidden.size() : 4;
			for (size_t i = 0; i < shown; i++) SmallText((u8"· " + hidden[i]).c_str());
			if (hidden.size() > shown) SmallText((u8"另有 " + std::to_string(hidden.size() - shown) + u8" 行").c_str(), Tok::TextFaint);
		}
		ImGui::PopTextWrapPos();
		ImGui::EndTooltip();
	}

	// ---- output --------------------------------------------------------------

	// Regex.dc.html "貼進遊戲搜尋列" card: scope, the string, the length meter
	// (three colours, RegexLimit.dc.html), per-page costs, output language +
	// copy, conflicts; single-page details below.
	void drawOutput()
	{
		PageState& s = st();
		if (!isAlgo() && !s.corpusReady) buildCorpus();
		if (s.dirty) recompute();
		const RegexAlgo::CombineResult& out = scopeCombined_ ? combinedAll() : s.combined;

		PobUi::CardBegin("rx_out", nullptr, nullptr, nullptr, true);
		const float inner = PobUi::CardInnerWidth();
		const float x0 = ImGui::GetCursorScreenPos().x;
		{
			const char* scope[2] = {u8"合併", u8"單頁"};
			const char* tips[2] = {u8"所有有勾選的清單合成一串", u8"只有目前這份清單（含它的數值條件）"};
			const float sw = PobUi::SegmentedWidth(scope, 2);
			const float y = ImGui::GetCursorScreenPos().y;
			const float H = PobUi::ControlH();
			ImGui::SetCursorScreenPos(ImVec2(x0, y + std::floor((H - BodyF()->FontSize) * 0.5f)));
			ImGui::TextUnformatted(u8"貼進遊戲搜尋列");
			ImGui::SetCursorScreenPos(ImVec2(x0 + inner - sw, y));
			int sc = scopeCombined_ ? 0 : 1;
			if (PobUi::SegmentedEx("##rx_scope", &sc, scope, 2, nullptr, tips)) setScope(sc == 0);
			ImGui::SetCursorScreenPos(ImVec2(x0, y + H + Dp(8.0f)));
		}
		// .out: the string, wrapped anywhere (it is one long word), min 3 lines
		{
			const ImVec2 p = ImGui::GetCursorScreenPos();
			const float padX = Dp(12.0f), padY = Dp(10.0f);
			const std::vector<std::string> lines = WrapAnywhere(BodyF(), out.query, inner - padX * 2);
			const float lh = std::floor(BodyF()->FontSize * 1.4f);
			const float h = std::max(Dp(66.0f), lh * (float)lines.size() + padY * 2);
			ImDrawList* dl = ImGui::GetWindowDrawList();
			dl->AddRectFilled(p, ImVec2(p.x + inner, p.y + h), Tok::Surface2, Dp(6.0f));
			dl->AddRect(p, ImVec2(p.x + inner, p.y + h), Tok::Border, Dp(6.0f), 0, 1.0f);
			bool condClash = false;
			for (const RegexAlgo::Conflict& c : out.conflicts) condClash |= c.kind == RegexAlgo::ConflictKind::ConditionClash;
			if (out.query.empty() && condClash) {
				// R10 (en: "The rarity / corruption conditions contradict each other")
				DrawTextAt(dl, SmallF(), ImVec2(p.x + padX, p.y + padY), Tok::Danger, u8"稀有度 / 汙染條件互相矛盾，無法合成字串");
			} else if (out.query.empty()) {
				DrawTextAt(dl, SmallF(), ImVec2(p.x + padX, p.y + padY), Tok::TextFaint,
				           scopeCombined_ ? u8"勾選清單裡的項目，這裡就會出現要貼的字串" : u8"這一頁還沒有勾選");
			} else {
				for (size_t i = 0; i < lines.size(); i++)
					DrawTextAt(dl, BodyF(), ImVec2(p.x + padX, p.y + padY + lh * (float)i + std::floor((lh - BodyF()->FontSize) * 0.5f)), Tok::Text,
					           lines[i].c_str());
			}
			ImGui::Dummy(ImVec2(inner, h));
			if (ImGui::IsItemHovered() && !out.query.empty()) PobUi::Tooltip(u8"按「複製」放進剪貼簿，再到遊戲的搜尋列貼上（Ctrl+V）");
		}
		// R10: the merge only takes the current page's kind of item
		// (en: "N more page(s) with picks are a different item and were left out")
		if (scopeCombined_ && mergeSkipped_ > 0)
			SmallText((u8"另有 " + std::to_string(mergeSkipped_) + u8" 頁的勾選屬於其他物品，未併入").c_str(), Tok::TextMuted, inner);
		// meter + length: ok / over 4/5 / over the limit
		const int len = out.length, lim = out.limit;
		const ImU32 lenCol = len > lim ? Tok::Danger : (len * 5 > lim * 4 ? Tok::Warning : (len > 0 ? Tok::Success : Tok::TextMuted));
		{
			const std::string t = std::to_string(len) + " / " + std::to_string(lim) + u8" 字";
			const float tw = TextSz(SmallF(), t.c_str()).x;
			const float y = ImGui::GetCursorScreenPos().y + Dp(4.0f);
			ImGui::SetCursorScreenPos(ImVec2(x0, y));
			Meter(lim > 0 ? (float)len / (float)lim : 0.0f, inner - tw - Dp(10.0f), lenCol == Tok::TextMuted ? Tok::Accent : lenCol);
			ImGui::SetCursorScreenPos(ImVec2(x0 + inner - tw, y + std::floor((Dp(18.0f) - SmallF()->FontSize) * 0.5f)));
			SmallText(t.c_str(), lenCol == Tok::TextMuted ? Tok::Text : lenCol);
			ImGui::SetCursorScreenPos(ImVec2(x0, y + Dp(18.0f) + Dp(4.0f)));
		}
		if (len > lim) SmallText(u8"超過上限，請減少勾選（超過時仍可複製，由你自行刪減）", Tok::Danger, inner);
		else if (len * 5 > lim * 4) SmallText(u8"超過4/5，再勾幾項就會到上限", Tok::TextMuted, inner);
		// RegexPanel.vue partsText: what each part costs, when more than one part
		if (scopeCombined_ && out.perPage.size() + (out.custom.empty() ? 0 : 1) + (out.excludes.empty() ? 0 : 1) > 1) {
			std::string parts;
			for (const RegexAlgo::PageContribution& c : out.perPage)
				parts += (parts.empty() ? "" : u8" · ") + contributionName(c.id) + " " + std::to_string(c.length);
			if (!out.custom.empty()) parts += (parts.empty() ? "" : u8" · ") + std::string(u8"自訂 ") + std::to_string(out.customLength);
			if (!out.excludes.empty()) parts += (parts.empty() ? "" : u8" · ") + std::string(u8"排除 ") + std::to_string(out.excludesLength);
			SmallText(parts.c_str(), Tok::TextMuted, inner);
		}
		ImGui::Dummy(ImVec2(0, Dp(4.0f)));
		// output language + copy
		{
			const char* langs[2] = {u8"輸出：繁體中文", u8"輸出：English"};
			int li = lang_ == Lang::Zh ? 0 : 1;
			const float lw = std::max(Dp(140.0f), PobUi::SelectFitWidth(langs, 2));
			const float y = ImGui::GetCursorScreenPos().y;
			if (PobUi::Select("##rx_lang", &li, langs, nullptr, 2, lw)) setLang(li == 0 ? Lang::Zh : Lang::En);
			if (ImGui::IsItemHovered()) PobUi::Tooltip(u8"要貼進哪一種語言的遊戲客戶端。兩邊產生的片段完全不同，不能互換使用。");
			const char* cl = copied_ ? u8"已複製" : u8"複製";
			const float cw = PobUi::ButtonWidth(cl, PobUi::BtnSize::Md, copied_ ? PobIcon::Check : PobIcon::Copy, Dp(88.0f));
			ImGui::SetCursorScreenPos(ImVec2(x0 + inner - cw, y));
			if (PobUi::Button(cl, PobUi::BtnKind::Primary, PobUi::BtnSize::Md, copied_ ? PobIcon::Check : PobIcon::Copy, Dp(88.0f),
			                  !out.query.empty())) {
				copyRequest_ = out.query;
				copied_ = false;
			}
		}

		// combine.ts classTerm (RegexPanel.vue ppz.regex.class_term; en: "{term} was
		// added automatically: only Tablets light up, so other items (e.g. a jewel's
		// \"Area of Effect\") are not matched.")
		if (!out.classTerm.empty()) {
			ImGui::Dummy(ImVec2(0, Dp(4.0f)));
			SmallText((u8"已自動加上 " + out.classTerm + u8":只會亮碑牌,避免誤中其他物品(例:珠寶的「範圍效果」)。").c_str(), Tok::TextMuted, inner);
		}
		if (scopeCombined_) {
			// the merge details live in the merged view; here how many, and a way there
			std::string body;
			if (!out.conflicts.empty()) body = std::to_string(out.conflicts.size()) + u8" 個合併衝突";
			if (!out.custom.empty()) body += (body.empty() ? "" : u8"。") + std::string(u8"自訂文字不經驗證，可能誤中其他物品。");
			if (!body.empty()) {
				ImGui::Dummy(ImVec2(0, Dp(4.0f)));
				const bool canJump = !out.conflicts.empty() && view_ != View::Combined;
				if (PobUi::Banner("rx_outwarn", PobUi::BannerTone::Warn, PobIcon::TriangleAlert, body.c_str(), nullptr, false,
				                  canJump ? u8"查看" : nullptr, false, true, inner) == PobUi::BannerResult::Action)
					setView(View::Combined);
			}
		} else {
			drawSingleDetails(s, inner);
		}
		PobUi::CardEnd();
		ImGui::Dummy(ImVec2(0, Dp(6.0f)));
	}

	// A page id in the per-page cost line: a numeric section says so.
	std::string contributionName(const std::string& id) const
	{
		for (const RegexAlgo::PageRef& p : refs_)
			if (p.Id() == id && p.Game() == selGame_) {
				if (p.IsSection())
					return pageTitleInGame(p.algo->sectionOf) + (RegexAlgo::IsConditionSectionId(id) ? u8"條件" : u8"數值條件");
				return p.Title();
			}
		return id;
	}

	// Single page: what the string could not express, conditions that did not
	// make it, and the fragments it is made of (collapsible, RegexLimit.dc.html).
	void drawSingleDetails(PageState& s, float inner)
	{
		int invalid = 0;
		std::vector<std::string> clash, condClash;
		for (const RegexAlgo::Conflict& c : s.combined.conflicts) {
			if (c.kind == RegexAlgo::ConflictKind::Invalid) invalid++;
			else if (c.kind == RegexAlgo::ConflictKind::Fragment) clash.push_back(c.text);
			else if (c.kind == RegexAlgo::ConflictKind::ConditionClash) condClash.push_back(c.text);
		}
		if (invalid > 0) SmallText((u8"有 " + std::to_string(invalid) + u8" 個數值條件輸入不成立，沒有放進字串。").c_str(), Tok::Warning, inner);
		// R10 (en: "The rarity / corruption conditions contradict each other; no string was made:")
		if (!condClash.empty()) {
			SmallText(u8"稀有度 / 汙染條件互相矛盾，無法合成字串：", Tok::Danger, inner);
			for (const std::string& t : condClash) SmallText((u8"· " + t).c_str(), Tok::TextMuted, inner);
		}
		if (!clash.empty()) {
			SmallText((u8"有 " + std::to_string(clash.size()) + u8" 個數值條件也會中這一頁的詞綴：").c_str(), Tok::Warning, inner);
			for (const std::string& t : clash) SmallText((u8"· " + t).c_str(), Tok::TextMuted, inner);
		}
		if (!s.result.unresolved.empty()) {
			const std::string head = u8"有 " + std::to_string(s.result.unresolved.size()) + u8" 項無法單獨指定";
			if (PobUi::CollapsingSection(head.c_str(), "###rx_unres", nullptr, nullptr, false)) {
				for (int i : s.result.unresolved) SmallText((u8"· " + LineIn(entries()[i], lang_)).c_str(), Tok::Text, inner);
				SmallText(u8"清單裡有其他項目印出一模一樣的文字，或這一行能用的每一段字也出現在每張物品都有的文字裡"
				          u8"（詞綴名稱、階層、提示說明、已汙染這類標籤），遊戲的搜尋沒有辦法只中它。",
				          Tok::TextMuted, inner);
			}
		}
		std::vector<std::string> parts = s.result.tokens;
		for (const RegexAlgo::PageContribution& c : s.combined.perPage)
			if (c.kind == RegexPageKind::Numeric || c.kind == RegexPageKind::Sockets)
				parts.insert(parts.end(), c.fragments.begin(), c.fragments.end());
		if (!parts.empty()) {
			const std::string head = u8"用到的片段（" + std::to_string(parts.size()) + u8" 段）";
			if (PobUi::CollapsingSection(head.c_str(), "###rx_tok", nullptr, nullptr, false)) {
				SmallText(u8"括號只是為了看清楚頭尾的空白，不要打進去", Tok::TextFaint, inner);
				// bracketed: a space at either end of a token is significant and otherwise invisible
				for (const std::string& t : parts) WrappedBlock(SmallF(), u8"「" + t + u8"」", inner, Tok::TextMuted);
			}
		}
	}

	// Whatever the panel last did, said under the output (RegexShare.dc.html
	// "套用之後": a warning banner with a title and an explanation).
	void drawNotice()
	{
		if (notice_.empty()) return;
		const PobUi::BannerResult r =
			PobUi::Banner("rx_notice", noticeWarn_ ? PobUi::BannerTone::Warn : PobUi::BannerTone::Info,
			              noticeWarn_ ? PobIcon::TriangleAlert : PobIcon::Info, notice_.c_str(),
			              noticeDesc_.empty() ? nullptr : noticeDesc_.c_str(), false, u8"知道了", false);
		if (r == PobUi::BannerResult::Action) {
			notice_.clear();
			noticeDesc_.clear();
			noticeWarn_ = false;
		}
		ImGui::Dummy(ImVec2(0, Dp(6.0f)));
	}
	void say(const std::string& msg, bool warn = false, const std::string& desc = std::string())
	{
		notice_ = msg;
		noticeWarn_ = warn;
		noticeDesc_ = desc;
	}

	// ---- bookmarks -----------------------------------------------------------
	//
	// R6 (exile-appraiser RegexBookmarks.vue): PoE1 / PoE2 tabs, one level of
	// folders, drag to reorder / into a folder. The folder logic itself is
	// regex_folders (pure, under --regex-selftest); this only draws it. Edits
	// made while drawing are queued in bmAction_ and applied after the list, so
	// no index the loop is still using moves under it.

	// One game's pages (corpus + algorithmic, sections included): page ids repeat
	// across games (gem_names, vendor_bases), so RegexEmbed always looks within one.
	std::vector<RegexAlgo::PageRef> gamePages(const std::string& g) const
	{
		std::vector<RegexAlgo::PageRef> out;
		for (const RegexAlgo::PageRef& p : refs_)
			if (p.Game() == g) out.push_back(p);
		return out;
	}
	int indexInGame(const std::string& g, const std::string& id) const
	{
		for (int i = 0; i < (int)refs_.size(); i++)
			if (refs_[i].Game() == g && refs_[i].Id() == id) return i;
		return -1;
	}
	std::string pageTitleIn(const std::string& g, const std::string& id) const
	{
		const int i = indexInGame(g, id);
		return i >= 0 ? refs_[i].Title() : pageTitleById(id);
	}

	// store.ts currentBookmarkBody: the page on screen (+ its section) as a bookmark.
	std::optional<RegexBookmark> currentBookmarkBody() const
	{
		if (!hasPage()) return std::nullopt;
		RegexEmbed::PicksMap picks;
		RegexEmbed::ValuesMap values;
		auto add = [&](int i) {
			picks[refs_[i].Id()] = picksOf(i);
			// merged: the item-mod values page and its section share the store key
			if (refs_[i].algo) {
				RegexAlgo::ValueMap& dst = values[RegexAlgo::NumericKeyOf(refs_[i].Id())];
				for (const auto& kv : pages_[i].algo.values)
					if (ownsValue(i, kv.first)) dst[kv.first] = kv.second;
			}
		};
		add(page_);
		const int sec = sectionIndexOf(page_);
		if (sec >= 0) add(sec);
		return RegexEmbed::BookmarkBodyOf(gamePages(selGame_), refs_[page_], picks, values, selGame_, modeId(),
		                                  lang_ == Lang::En ? "en" : "zh");
	}

	struct BmAction {
		enum Kind { None, ToFolder, Before, Step, FolderTo, FolderStep, Fold } kind = None;
		int index = -1;      // bookmark
		int before = -1;     // Before: the bookmark to land in front of
		int delta = 0;       // Step / FolderStep
		int to = 0;          // FolderTo
		bool on = false;     // Fold
		std::string folder;  // ToFolder / FolderTo / FolderStep / Fold ("" = uncategorised)
	};

	void applyBmAction()
	{
		const BmAction a = bmAction_;
		bmAction_ = BmAction{};
		bool changed = false;
		switch (a.kind) {
		case BmAction::None: return;
		case BmAction::ToFolder: changed = RegexFolders::MoveBookmark(state_, a.index, a.folder) >= 0; break;
		case BmAction::Before: changed = RegexFolders::MoveBookmark(state_, a.index, std::string(), a.before) >= 0; break;
		case BmAction::Step: changed = RegexFolders::MoveBookmarkBy(state_, a.index, a.delta) != a.index; break;
		case BmAction::FolderTo: changed = RegexFolders::MoveTo(state_, bmTab_, a.folder, a.to); break;
		case BmAction::FolderStep: changed = RegexFolders::MoveBy(state_, bmTab_, a.folder, a.delta); break;
		case BmAction::Fold: changed = RegexFolders::SetCollapsed(state_, bmTab_, a.folder, a.on); break;
		}
		if (changed) markStateDirty();
	}

	// The bookmark card (Regex.dc.html, RegexBookmarks.dc.html): "書籤 · PoE1",
	// the PoE1 / PoE2 tabs (a game's bookmarks stay reachable from the other),
	// 存成書籤 and a more menu (folders, import, send); rows with 載入 and a ⋯
	// menu; folders as foldable groups; drag to reorder or into a folder.
	void drawBookmarks()
	{
		// The tab follows the list's game when that changes (RegexBookmarks.vue
		// watches selGame); a tab picked by hand stays until then.
		if (bmTabFollow_ != selGame_) bmTab_ = bmTabFollow_ = selGame_;
		int count[2] = {0, 0}, orphans = 0;
		for (const RegexBookmark& b : state_.bookmarks) {
			if (b.game.empty()) orphans++;
			else count[b.game == "poe2" ? 1 : 0]++;
		}
		const int tabIdx = bmTab_ == "poe2" ? 1 : 0;
		const float availH = ImGui::GetContentRegionAvail().y;
		PobUi::CardBegin("rx_bmcard", nullptr, nullptr, nullptr, false, 0.0f, std::max(Dp(160.0f), availH - Dp(2.0f)));
		const ImVec2 c0 = ImGui::GetCursorScreenPos();
		const float cw = ImGui::GetContentRegionAvail().x;
		const float padX = Dp(14.0f), headH = Dp(44.0f);
		ImDrawList* dl = ImGui::GetWindowDrawList();
		// head: title, game tabs, save, more
		{
			const std::string title = std::string(u8"書籤 · ") + GameLabel(bmTab_);
			const float ty = c0.y + std::floor((headH - BodyF()->FontSize) * 0.5f);
			DrawTextAt(dl, BodyF(), ImVec2(c0.x + padX, ty), Tok::Text, title.c_str());
			float x = c0.x + padX + TextSz(BodyF(), title.c_str()).x + Dp(10.0f);
			std::string l0 = "PoE1 " + std::to_string(count[0]), l1 = "PoE2 " + std::to_string(count[1]);
			const char* tl[2] = {l0.c_str(), l1.c_str()};
			const bool lit[2] = {tabIdx == 0, tabIdx == 1};
			const bool both = firstPageOf("poe2") >= 0 || count[1] > 0;
			if (both) {
				ImGui::SetCursorScreenPos(ImVec2(x, c0.y + std::floor((headH - Dp(26.0f)) * 0.5f)));
				const int k = SegToggles("##rx_bmtab", tl, lit, 2);
				if (k >= 0) bmTab_ = kGames[k];
			}
			const int picks = pickCount() + sectionPickCount();
			const float moreW = PobUi::ButtonWidth("##rxbmmore", PobUi::BtnSize::Sm, PobIcon::MoreHorizontal, Dp(28.0f));
			const float saveW = PobUi::ButtonWidth(u8"存成書籤", PobUi::BtnSize::Sm);
			const float by = c0.y + std::floor((headH - Dp(28.0f)) * 0.5f);
			ImGui::SetCursorScreenPos(ImVec2(c0.x + cw - padX - moreW - Dp(4.0f) - saveW, by));
			if (PobUi::Button(u8"存成書籤", PobUi::BtnKind::Secondary, PobUi::BtnSize::Sm, nullptr, 0.0f, picks > 0)) openSave(picks);
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
				PobUi::Tooltip(picks == 0 ? u8"先勾選幾項才有東西可以存"
				                          : (std::string(u8"把目前這一頁存成 ") + GameLabel(selGame_) + u8" 的書籤：勾選、數值條件、模式與輸出語言").c_str());
			ImGui::SameLine(0, Dp(4.0f));
			if (PobUi::Button("##rxbmmore", PobUi::BtnKind::Ghost, PobUi::BtnSize::Sm, PobIcon::MoreHorizontal, Dp(28.0f)))
				ImGui::OpenPopup("##rxbmmoremenu");
			// under the button that opened it, right-aligned (not wherever the mouse is)
			ImGui::SetNextWindowPos(ImVec2(ImGui::GetItemRectMax().x, ImGui::GetItemRectMax().y + Dp(4.0f)), ImGuiCond_Appearing, ImVec2(1.0f, 0.0f));
			if (PobUi::BeginMenuPopup("##rxbmmoremenu")) {
				if (PobUi::MenuRow(PobIcon::Folder, u8"新增資料夾…")) {
					nameBuf_.clear();
					folderErr_.clear();
					modal_ = Modal::FolderAdd;
				}
				if (PobUi::MenuRow(PobIcon::Download, u8"匯入書籤包…")) openImport();
				if (PobUi::MenuRow(PobIcon::Share, u8"送到 ExileAppraiser…", nullptr, sendableBookmarks() > 0)) openSendPick();
				PobUi::EndMenuPopup();
			}
			dl->AddLine(ImVec2(c0.x + 1, c0.y + headH), ImVec2(c0.x + cw - 1, c0.y + headH), Tok::BorderSubtle, 1.0f);
			ImGui::SetCursorScreenPos(ImVec2(c0.x, c0.y + headH + 1));
		}

		const RegexFolders::Grouped grouped = RegexFolders::GroupBookmarks(state_, bmTab_, false);
		const float listH = std::max(Dp(100.0f), availH - Dp(2.0f) - headH - 2);
		if (count[tabIdx] == 0 && !grouped.headers) {
			// RegexBookmarks.dc.html "還沒有書籤"
			ImGui::SetCursorScreenPos(ImVec2(c0.x + Dp(10.0f), ImGui::GetCursorScreenPos().y + Dp(10.0f)));
			const std::string t = std::string(GameLabel(bmTab_)) + u8" 還沒有書籤";
			const int picks = pickCount() + sectionPickCount();
			if (PobUi::EmptyState("rx_bmempty", PobIcon::FileText, t.c_str(), u8"勾好一組常用的詞綴後按「存成書籤」，下次直接叫回來。",
			                      bmTab_ == selGame_ && picks > 0 ? u8"存成書籤" : nullptr, cw - Dp(20.0f)))
				openSave(picks);
			ImGui::SetCursorScreenPos(ImVec2(c0.x + padX, ImGui::GetCursorScreenPos().y + Dp(6.0f)));
			drawOrphanNote(orphans, cw - padX * 2);
			PobUi::CardEnd();
			return;
		}

		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 0));
		ImGui::BeginChild("##rx_bm", ImVec2(cw, listH), false);
		bool anyMissing = false;
		for (size_t gi = 0; gi < grouped.groups.size(); gi++) {
			const RegexFolders::Group& grp = grouped.groups[gi];
			ImGui::PushID((int)gi);
			if (grouped.headers) drawFolderHeader(grp);
			if (!grouped.headers || !grp.collapsed) {
				if (grp.items.empty() && grouped.headers) {
					ImGui::SetCursorPosX(ImGui::GetCursorPosX() + Dp(36.0f));
					SmallText(u8"空的：把書籤拖到這裡", Tok::TextFaint);
					if (ImGui::BeginDragDropTarget()) {
						if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("RX_BM"))
							bmAction_ = BmAction{BmAction::ToFolder, *(const int*)pl->Data, -1, 0, 0, false, grp.folder};
						ImGui::EndDragDropTarget();
					}
					ImGui::Dummy(ImVec2(0, Dp(6.0f)));
				}
				for (size_t k = 0; k < grp.items.size(); k++) {
					anyMissing |= indexInGame(bmTab_, state_.bookmarks[grp.items[k]].page) < 0;
					drawBookmarkRow(grp.items[k], grp, k, grouped.headers);
				}
			}
			ImGui::PopID();
		}
		ImGui::Dummy(ImVec2(0, Dp(6.0f)));
		ImGui::SetCursorPosX(ImGui::GetCursorPosX() + padX);
		if (anyMissing)
			SmallText(u8"標「清單已下架」的書籤：這個版本沒有那份清單了，書籤先保留不刪；之後清單回來就能再載入。", Tok::TextMuted, cw - padX * 2);
		if (bmTab_ != selGame_) {
			ImGui::SetCursorPosX(ImGui::GetCursorPosX() + padX);
			SmallText((std::string(u8"這是 ") + GameLabel(bmTab_) + u8" 的書籤：「載入」會切換到 " + GameLabel(bmTab_) + u8"。").c_str(),
			          Tok::TextMuted, cw - padX * 2);
		}
		ImGui::SetCursorPosX(ImGui::GetCursorPosX() + padX);
		drawOrphanNote(orphans, cw - padX * 2);
		ImGui::EndChild();
		ImGui::PopStyleVar(2);
		PobUi::CardEnd();
		applyBmAction();
	}

	// A folder's header (or "未分類"): fold chevron, name + count, a ⋯ menu.
	// The whole row is the drag handle for folders and a drop target for both.
	void drawFolderHeader(const RegexFolders::Group& grp)
	{
		const bool uncat = grp.folder.empty();
		const std::vector<RegexBookmarkFolder>& list = state_.Folders(bmTab_);
		int fi = -1;
		for (int i = 0; i < (int)list.size(); i++)
			if (list[i].name == grp.folder) fi = i;
		const float w = ImGui::GetContentRegionAvail().x, h = Dp(32.0f), padX = Dp(10.0f);
		const ImVec2 p = ImGui::GetCursorScreenPos();
		const float moreW = uncat ? 0.0f : PobUi::ButtonWidth("##fmore", PobUi::BtnSize::Sm, PobIcon::MoreHorizontal, Dp(26.0f));
		const bool click = ImGui::InvisibleButton("##fhead", ImVec2(w - moreW - padX, h));
		const bool hov = ImGui::IsItemHovered();
		if (click) bmAction_ = BmAction{BmAction::Fold, -1, -1, 0, 0, !grp.collapsed, grp.folder};
		if (!uncat && ImGui::BeginDragDropSource()) {
			ImGui::SetDragDropPayload("RX_FOLDER", &fi, sizeof fi);
			ImGui::Text(u8"移動資料夾：%s", grp.folder.c_str());
			ImGui::EndDragDropSource();
		}
		if (ImGui::BeginDragDropTarget()) {
			const ImVec2 r0 = ImGui::GetItemRectMin(), r1 = ImGui::GetItemRectMax();
			const bool lower = ImGui::GetMousePos().y > (r0.y + r1.y) * 0.5f;
			const ImGuiDragDropFlags f = ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect;
			if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("RX_BM", f)) {
				ImGui::GetWindowDrawList()->AddRect(r0, r1, Tok::Accent, Dp(6.0f), 0, 2.0f);
				if (pl->IsDelivery()) bmAction_ = BmAction{BmAction::ToFolder, *(const int*)pl->Data, -1, 0, 0, false, grp.folder};
			}
			if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("RX_FOLDER", f)) {
				const float y = (uncat || lower) ? r1.y : r0.y;
				ImGui::GetWindowDrawList()->AddLine(ImVec2(r0.x, y), ImVec2(r1.x, y), Tok::Accent, 2.0f);
				const int src = *(const int*)pl->Data;
				if (pl->IsDelivery() && src >= 0 && src < (int)list.size()) {
					int to = uncat ? (int)list.size() - 1 : (lower ? fi + 1 : fi);
					if (!uncat && src < to) to--;
					bmAction_ = BmAction{BmAction::FolderTo, -1, -1, 0, to, false, list[src].name};
				}
			}
			ImGui::EndDragDropTarget();
		}
		ImDrawList* dl = ImGui::GetWindowDrawList();
		if (hov) dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), Tok::Surface2, Dp(5.0f));
		float x = p.x + padX;
		const float iconPx = SmallF()->FontSize;
		const float ty = p.y + std::floor((h - BodyF()->FontSize) * 0.5f);
		if (PobUi::Fonts().icons) {
			PobUi::IconAt(dl, ImVec2(x, p.y + std::floor((h - iconPx) * 0.5f)), grp.collapsed ? PobIcon::ChevronRight : PobIcon::ChevronDown,
			              Tok::TextMuted, iconPx);
			x += PobUi::IconWidth(PobIcon::ChevronDown, iconPx) + Dp(6.0f);
			PobUi::IconAt(dl, ImVec2(x, p.y + std::floor((h - iconPx) * 0.5f)), grp.collapsed ? PobIcon::Folder : PobIcon::FolderOpen,
			              Tok::TextMuted, iconPx);
			x += PobUi::IconWidth(PobIcon::Folder, iconPx) + Dp(6.0f);
		}
		const std::string name = uncat ? std::string(u8"未分類") : grp.folder;
		DrawTextAt(dl, BodyF(), ImVec2(x, ty), Tok::Text, name.c_str());
		x += TextSz(BodyF(), name.c_str()).x + Dp(8.0f);
		DrawTextAt(dl, SmallF(), ImVec2(x, p.y + std::floor((h - SmallF()->FontSize) * 0.5f)), Tok::TextMuted,
		           (std::to_string(grp.items.size()) + u8" 筆").c_str());
		if (!uncat) {
			ImGui::SetCursorScreenPos(ImVec2(p.x + w - padX - moreW, p.y + std::floor((h - Dp(28.0f)) * 0.5f)));
			if (PobUi::Button("##fmore", PobUi::BtnKind::Ghost, PobUi::BtnSize::Sm, PobIcon::MoreHorizontal, Dp(26.0f)))
				ImGui::OpenPopup("##fmenu");
			// under the button that opened it, right-aligned (not wherever the mouse is)
			ImGui::SetNextWindowPos(ImVec2(ImGui::GetItemRectMax().x, ImGui::GetItemRectMax().y + Dp(4.0f)), ImGuiCond_Appearing, ImVec2(1.0f, 0.0f));
			if (PobUi::BeginMenuPopup("##fmenu")) {
				if (PobUi::MenuRow(PobIcon::Pencil, u8"改名…")) {
					folderEdit_ = grp.folder;
					nameBuf_ = grp.folder;
					folderErr_.clear();
					modal_ = Modal::FolderRename;
				}
				if (PobUi::MenuRow(nullptr, u8"上移", nullptr, fi > 0))
					bmAction_ = BmAction{BmAction::FolderStep, -1, -1, -1, 0, false, grp.folder};
				if (PobUi::MenuRow(nullptr, u8"下移", nullptr, fi >= 0 && fi + 1 < (int)list.size()))
					bmAction_ = BmAction{BmAction::FolderStep, -1, -1, 1, 0, false, grp.folder};
				PobUi::MenuSeparator();
				if (PobUi::MenuRow(PobIcon::Trash, u8"刪除資料夾…", nullptr, true, true)) {
					folderEdit_ = grp.folder;
					modal_ = Modal::FolderDelete;
				}
				PobUi::EndMenuPopup();
			}
		}
		ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h));
		ImGui::Dummy(ImVec2(0, 0));
	}

	std::string bookmarkMeta(const RegexBookmark& b) const
	{
		const char* modeZh = b.mode == "all" ? u8"全部都有" : b.mode == "none" ? u8"一個都沒有" : u8"含任一個";
		std::string meta = pageTitleIn(b.game, b.page) + u8" · " + modeZh + u8" · " + (b.lang == "en" ? "English" : u8"繁中") +
		                   u8" · " + std::to_string(b.keys.size()) + u8" 項";
		if (!b.num.empty())
			meta += RegexAlgo::IsConditionSectionId(RegexAlgo::SectionIdOf(b.page)) ? std::string(u8" ＋ 稀有度 / 汙染")
			                                                                       : u8" ＋ 數值條件 " + std::to_string(b.num.size()) + u8" 項";
		return meta;
	}

	// .bm: name + what it holds | 載入 | ⋯ (update, rename, move, delete). A row
	// whose list this build does not ship is muted with "清單已下架" and no 載入.
	void drawBookmarkRow(int i, const RegexFolders::Group& grp, size_t k, bool inFolder)
	{
		const RegexBookmark& b = state_.bookmarks[i];
		ImGui::PushID(i);
		const bool missing = indexInGame(b.game.empty() ? bmTab_ : b.game, b.page) < 0;
		const float w = ImGui::GetContentRegionAvail().x;
		const float padX = Dp(14.0f), padY = Dp(8.0f);
		const float indent = inFolder ? Dp(22.0f) : 0.0f;
		const ImVec2 p = ImGui::GetCursorScreenPos();
		ImDrawList* dl = ImGui::GetWindowDrawList();
		if (k > 0 || inFolder) dl->AddLine(ImVec2(p.x + indent + padX, p.y), ImVec2(p.x + w - padX, p.y), Tok::BorderSubtle, 1.0f);
		const float moreW = PobUi::ButtonWidth("##bmore", PobUi::BtnSize::Sm, PobIcon::MoreHorizontal, Dp(26.0f));
		const char* loadL = u8"載入";
		const char* pillL = u8"清單已下架";
		const float actW = missing ? PillW(pillL) : PobUi::ButtonWidth(loadL, PobUi::BtnSize::Sm);
		const std::string meta = bookmarkMeta(b);
		const float textW = w - indent - padX * 2 - actW - moreW - Dp(16.0f);
		const float h = padY * 2 + BodyF()->FontSize + Dp(2.0f) + SmallF()->FontSize;
		// the text area: drag handle (and a double-click loads)
		ImGui::SetCursorScreenPos(ImVec2(p.x + indent, p.y));
		ImGui::InvisibleButton("##bmtext", ImVec2(std::max(1.0f, textW + padX), h));
		const bool hov = ImGui::IsItemHovered();
		if (hov && !missing && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) loadBookmark(i);
		if (hov) PobUi::Tooltip((b.name + "\n" + meta + u8"\n拖曳來排序或拖進資料夾；按兩下載入").c_str());
		if (ImGui::BeginDragDropSource()) {
			ImGui::SetDragDropPayload("RX_BM", &i, sizeof i);
			ImGui::Text(u8"移動書籤：%s", b.name.c_str());
			ImGui::EndDragDropSource();
		}
		if (ImGui::BeginDragDropTarget()) {
			const ImVec2 r0 = ImGui::GetItemRectMin(), r1 = ImGui::GetItemRectMax();
			const bool lower = ImGui::GetMousePos().y > (r0.y + r1.y) * 0.5f;
			const ImGuiDragDropFlags f = ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect;
			if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("RX_BM", f)) {
				const float y = lower ? r1.y + 1 : r0.y - 1;
				ImGui::GetWindowDrawList()->AddLine(ImVec2(r0.x, y), ImVec2(p.x + w, y), Tok::Accent, 2.0f);
				if (pl->IsDelivery()) {
					const int src = *(const int*)pl->Data;
					if (!lower) bmAction_ = BmAction{BmAction::Before, src, i, 0, 0, false, std::string()};
					else if (k + 1 < grp.items.size())
						bmAction_ = BmAction{BmAction::Before, src, grp.items[k + 1], 0, 0, false, std::string()};
					else bmAction_ = BmAction{BmAction::ToFolder, src, -1, 0, 0, false, b.folder};
				}
			}
			ImGui::EndDragDropTarget();
		}
		{
			const float tx = p.x + indent + padX;
			ImGui::PushClipRect(ImVec2(tx, p.y), ImVec2(tx + std::max(1.0f, textW), p.y + h), true);
			DrawTextAt(dl, BodyF(), ImVec2(tx, p.y + padY), missing ? Tok::TextMuted : Tok::Text, b.name.c_str());
			DrawTextAt(dl, SmallF(), ImVec2(tx, p.y + padY + BodyF()->FontSize + Dp(2.0f)), Tok::TextMuted, meta.c_str());
			ImGui::PopClipRect();
		}
		const float by = p.y + std::floor((h - Dp(28.0f)) * 0.5f);
		float bx = p.x + w - padX - moreW - Dp(4.0f) - actW;
		if (missing) {
			ImGui::SetCursorScreenPos(ImVec2(bx, p.y + std::floor((h - SmallF()->FontSize - Dp(2.0f)) * 0.5f)));
			Pill(pillL, Tok::Warning, Tok::WarningSoft);
		} else {
			ImGui::SetCursorScreenPos(ImVec2(bx, by));
			if (PobUi::Button(loadL, PobUi::BtnKind::Secondary, PobUi::BtnSize::Sm)) loadBookmark(i);
			if (ImGui::IsItemHovered())
				PobUi::Tooltip(b.game == selGame_ ? u8"載入這組勾選（覆蓋這一頁目前的勾選；其他頁不動）"
				                                  : (std::string(u8"載入並切換到 ") + GameLabel(b.game)).c_str());
		}
		ImGui::SetCursorScreenPos(ImVec2(p.x + w - padX - moreW, by));
		if (PobUi::Button("##bmore", PobUi::BtnKind::Ghost, PobUi::BtnSize::Sm, PobIcon::MoreHorizontal, Dp(26.0f)) ||
		    testOpenBmMenu_ == i) {
			testOpenBmMenu_ = -1;
			ImGui::OpenPopup("##bmmenu");
		}
		// under the button that opened it, right-aligned (not wherever the mouse is)
		ImGui::SetNextWindowPos(ImVec2(ImGui::GetItemRectMax().x, ImGui::GetItemRectMax().y + Dp(4.0f)), ImGuiCond_Appearing, ImVec2(1.0f, 0.0f));
		if (PobUi::BeginMenuPopup("##bmmenu")) {
			const bool sameGame = b.game == selGame_;
			if (PobUi::MenuRow(PobIcon::Refresh, u8"用目前的勾選更新", nullptr, sameGame && !missing)) updateBookmark(i);
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
				PobUi::Tooltip(sameGame ? u8"用目前這一頁的勾選、數值條件、模式與輸出語言覆寫這個書籤（名稱與資料夾不變）"
				                        : u8"這個書籤屬於另一個遊戲：先在上方切換遊戲才能更新");
			if (PobUi::MenuRow(PobIcon::Pencil, u8"改名…")) {
				nameBuf_ = b.name;
				editIdx_ = i;
				nameErr_.clear();
				modal_ = Modal::Rename;
			}
			const std::vector<RegexBookmarkFolder>& list = state_.Folders(b.game);
			if (!list.empty()) {
				PobUi::MenuSeparator();
				PobUi::Overline(u8"移到資料夾");
				if (PobUi::MenuRow(b.folder.empty() ? PobIcon::Check : nullptr, u8"未分類"))
					bmAction_ = BmAction{BmAction::ToFolder, i, -1, 0, 0, false, std::string()};
				for (int f = 0; f < (int)list.size(); f++) {
					ImGui::PushID(f);
					if (PobUi::MenuRow(b.folder == list[f].name ? PobIcon::Check : nullptr, list[f].name.c_str()))
						bmAction_ = BmAction{BmAction::ToFolder, i, -1, 0, 0, false, list[f].name};
					ImGui::PopID();
				}
			}
			PobUi::MenuSeparator();
			if (PobUi::MenuRow(nullptr, u8"上移", nullptr, k > 0)) bmAction_ = BmAction{BmAction::Step, i, -1, -1, 0, false, std::string()};
			if (PobUi::MenuRow(nullptr, u8"下移", nullptr, k + 1 < grp.items.size()))
				bmAction_ = BmAction{BmAction::Step, i, -1, 1, 0, false, std::string()};
			PobUi::MenuSeparator();
			if (PobUi::MenuRow(PobIcon::Trash, u8"刪除書籤…", nullptr, true, true)) {
				editIdx_ = i;
				modal_ = Modal::Delete;
			}
			PobUi::EndMenuPopup();
		}
		ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h));
		ImGui::Dummy(ImVec2(0, 0));
		ImGui::PopID();
	}

	// A bookmark whose page id belongs to no loaded catalogue and that carries no
	// game: still in regex_ui.json and written back on every save, counted here
	// rather than shown as a row no button could act on.
	void drawOrphanNote(int orphans, float width)
	{
		if (orphans <= 0) return;
		SmallText((u8"另有 " + std::to_string(orphans) + u8" 筆書籤存在這個版本沒有的清單上，沒有顯示（資料仍保留在 PobTools\\regex_ui.json）。").c_str(),
		          Tok::TextMuted, width);
	}

	// store.ts loadBookmark / embed.ts bookmarkApplyOf: the bookmark's page (and
	// its section) is overwritten -- a bookmark is the whole page -- and every
	// other page keeps its ticks.
	void loadBookmark(int i)
	{
		if (i < 0 || i >= (int)state_.bookmarks.size()) return;
		// By value: everything below writes to state_.
		const RegexBookmark b = state_.bookmarks[i];
		const std::string g = b.game.empty() ? gameOfPage(b.page) : b.game;
		// store.ts loadBookmark: an item-mod values bookmark loads that page first
		// (here synchronously: the click is the moment, and it takes well under a second).
		if (RegexItemMods::IsPageId(b.page) && !g.empty() && !ensureItemModsNow(g)) {
			notice_ = u8"書籤「" + b.name + u8"」的物品詞綴資料載入失敗：" + imv_[GameIdx(g)].err;
			return;
		}
		const std::optional<RegexEmbed::BookmarkApply> a =
			g.empty() ? std::nullopt : RegexEmbed::BookmarkApplyOf(gamePages(g), b);
		const int target = a ? indexInGame(g, a->page) : -1;
		if (target < 0) {
			notice_ = u8"書籤「" + b.name + u8"」的清單「" + pageTitleById(b.page) +
			          u8"」在這個版本不存在，沒有載入。";
			return;
		}
		// The bookmark carries its own game: loading one never leaves the
		// selector pointing somewhere else.
		if (selGame_ != g) {
			selGame_ = g;
			combinedDirty_ = true;
		}
		state_.game = selGame_;
		switchPage(target);
		setView(View::Page);
		mode_ = ModeFromId(b.mode);
		state_.mode = modeId();
		setLang(b.lang == "en" ? Lang::En : Lang::Zh);
		for (const auto& p : a->picks) {
			const int idx = indexInGame(g, p.first);
			if (idx < 0) continue;
			setTicks(idx, p.second);
			syncCurrent(idx);
		}
		for (const auto& v : a->values)
			for (int idx = 0; idx < (int)refs_.size(); idx++) {
				if (!refs_[idx].algo || refs_[idx].Game() != g || RegexAlgo::NumericKeyOf(refs_[idx].Id()) != v.first)
					continue;
				for (const auto& kv : v.second)
					if (ownsValue(idx, kv.first)) pages_[idx].algo.values[kv.first] = kv.second;
				syncValues(idx);
			}
		for (PageState& ps : pages_) ps.dirty = true;   // mode / values changed under them
		combinedDirty_ = true;
		copied_ = false;
		notice_ = a->missed > 0
			? u8"已載入書籤「" + b.name + u8"」，但其中 " + std::to_string(a->missed) +
			  u8" 項在目前的資料裡找不到（賽季更新後詞條可能有變動）。"
			: u8"已載入書籤「" + b.name + u8"」。";
		state_.page = pageId();
		markStateDirty();
	}

	// store.ts updateBookmark: the page on screen overwrites the bookmark; its
	// name, folder (and exile-appraiser hotkey) stay.
	void updateBookmark(int i)
	{
		if (i < 0 || i >= (int)state_.bookmarks.size()) return;
		RegexBookmark& b = state_.bookmarks[i];
		if (b.game != selGame_) {
			notice_ = u8"書籤「" + b.name + u8"」屬於 " + GameLabel(b.game) + u8"，先切到那個遊戲再更新。";
			return;
		}
		std::optional<RegexBookmark> body = currentBookmarkBody();
		if (!body) {
			notice_ = u8"目前一項都沒有勾選，沒有更新書籤（要清空請改用刪除）。";
			return;
		}
		b.page = body->page;
		b.game = body->game;
		b.mode = body->mode;
		b.lang = body->lang;
		b.keys = std::move(body->keys);
		b.alt = std::move(body->alt);
		b.numeric = std::move(body->numeric);
		b.num = std::move(body->num);
		notice_ = u8"書籤「" + b.name + u8"」已更新為目前的勾選。";
		markStateDirty();
	}

	void openSave(int picks)
	{
		nameBuf_ = pageTitleIn(selGame_, pageId()) + " " + std::to_string(picks) + u8" 項";
		editIdx_ = -1;
		nameErr_.clear();
		modal_ = Modal::Save;
	}

	// Is `name` already a bookmark of game g (other than `except`)? (RegexBookmarks.dc.html
	// "已經有同名的書籤"; a PobTools rule -- exile-appraiser allows duplicates.)
	bool nameTaken(const std::string& g, const std::string& name, int except) const
	{
		const std::string n = RegexAlgo::JsTrim(name);
		for (int i = 0; i < (int)state_.bookmarks.size(); i++)
			if (i != except && state_.bookmarks[i].game == g && state_.bookmarks[i].name == n) return true;
		return false;
	}

	// Every dialog, at the top level: OpenPopup and BeginPopupModal must share
	// an ID scope, so requests travel up here from the child that raised them.
	void drawModals()
	{
		const Modal opening = modal_;
		modal_ = Modal::None;
		bool oName = false, oDel = false, oFolder = false, oFolderDel = false, oPaste = false, oTpl = false, oSend = false, oImport = false;
		switch (opening) {
		case Modal::Save: case Modal::Rename: renameMode_ = opening == Modal::Rename; oName = true; break;
		case Modal::Delete: oDel = true; break;
		case Modal::FolderAdd: case Modal::FolderRename: folderRenameMode_ = opening == Modal::FolderRename; oFolder = true; break;
		case Modal::FolderDelete: oFolderDel = true; break;
		case Modal::Paste: oPaste = true; break;
		case Modal::Template: oTpl = true; break;
		case Modal::SendPick: oSend = true; break;
		case Modal::ImportPack: oImport = true; break;
		case Modal::None: break;
		}
		using PobUi::DialogResult;

		// 存成書籤 / 重新命名書籤
		{
			const std::string g = renameMode_ && editIdx_ >= 0 && editIdx_ < (int)state_.bookmarks.size() ? state_.bookmarks[editIdx_].game
			                                                                                              : selGame_;
			const bool empty = RegexAlgo::JsTrim(nameBuf_).empty();
			const bool dup = !empty && nameTaken(g, nameBuf_, renameMode_ ? editIdx_ : -1);
			if (PobUi::BeginDialog("###rx_name", &oName, renameMode_ ? u8"重新命名書籤" : u8"存成書籤", nullptr, nullptr, 340.0f)) {
				const float inner = ImGui::GetContentRegionAvail().x;
				if (!renameMode_) SmallText(u8"名稱");
				PobUi::PushControlFrame();
				ImGui::SetNextItemWidth(inner);
				if (oName || opening != Modal::None) ImGui::SetKeyboardFocusHere();
				ImGui::InputText("##bmname", &nameBuf_);
				PobUi::PopControlFrame();
				if (dup) SmallText(u8"已經有同名的書籤", Tok::Danger);
				if (!renameMode_) {
					if (std::optional<RegexBookmark> body = currentBookmarkBody()) {
						const char* modeZh = body->mode == "all" ? u8"全部都有" : body->mode == "none" ? u8"一個都沒有" : u8"含任一個";
						std::string what = u8"會記住：" + pageTitleIn(body->game, body->page) + u8" · " + modeZh + u8" · " +
						                   (body->lang == "en" ? "English" : u8"繁體中文") + u8" · " + std::to_string(body->keys.size()) + u8" 項";
						if (!body->num.empty()) what += u8"＋條件 " + std::to_string(body->num.size()) + u8" 項";
						SmallText(what.c_str(), Tok::TextMuted, inner);
					}
				}
				const DialogResult r = DlgButtons(inner, u8"取消", nullptr, renameMode_ ? u8"改名" : u8"儲存", !empty && !dup);
				const bool enter = !empty && !dup && (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter));
				if (r == DialogResult::Primary || enter) {
					commitName();
					ImGui::CloseCurrentPopup();
				} else if (r == DialogResult::Cancel) {
					ImGui::CloseCurrentPopup();
				}
				PobUi::EndDialog();
			}
		}

		// 刪除書籤
		{
			const bool valid = editIdx_ >= 0 && editIdx_ < (int)state_.bookmarks.size();
			const std::string t = valid ? u8"刪除「" + state_.bookmarks[editIdx_].name + u8"」？" : std::string(u8"這個書籤已經不在了");
			const DialogResult r = PobUi::ConfirmDialog("###rx_del", &oDel, t.c_str(), u8"刪掉之後沒辦法復原。", nullptr, u8"取消",
			                                            valid ? u8"刪除" : nullptr, nullptr);
			if (r == DialogResult::Danger && valid) {
				say(u8"已刪除書籤「" + state_.bookmarks[editIdx_].name + u8"」。");
				state_.bookmarks.erase(state_.bookmarks.begin() + editIdx_);
				editIdx_ = -1;
				markStateDirty();
			}
		}

		// 新增 / 重新命名資料夾
		if (PobUi::BeginDialog("###rx_folder", &oFolder, folderRenameMode_ ? u8"重新命名資料夾" : u8"新增資料夾",
		                       (std::string(GameLabel(bmTab_)) + u8" 的書籤（資料夾只有一層）").c_str(), nullptr, 340.0f)) {
			const float inner = ImGui::GetContentRegionAvail().x;
			PobUi::PushControlFrame();
			ImGui::SetNextItemWidth(inner);
			if (opening != Modal::None) ImGui::SetKeyboardFocusHere();
			ImGui::InputText("##fname", &nameBuf_);
			PobUi::PopControlFrame();
			if (!folderErr_.empty()) SmallText(folderErr_.c_str(), Tok::Danger);
			const bool empty = RegexAlgo::JsTrim(nameBuf_).empty();
			DialogResult r = DlgButtons(inner, u8"取消", nullptr, folderRenameMode_ ? u8"改名" : u8"新增", !empty);
			if (!empty && (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter))) r = DialogResult::Primary;
			if (r == DialogResult::Primary) {
				const RegexFolders::Result fr = folderRenameMode_ ? RegexFolders::Rename(state_, bmTab_, folderEdit_, nameBuf_)
				                                                  : RegexFolders::Add(state_, bmTab_, nameBuf_);
				if (fr == RegexFolders::Result::Ok) {
					const std::string n = RegexFolders::NormalizeName(nameBuf_);
					say(folderRenameMode_ ? u8"資料夾已改名為「" + n + u8"」。" : u8"已新增資料夾「" + n + u8"」。");
					markStateDirty();
					ImGui::CloseCurrentPopup();
				} else {
					folderErr_ = fr == RegexFolders::Result::Empty ? u8"名稱不能是空白"
					           : fr == RegexFolders::Result::Duplicate ? u8"已經有同名的資料夾" : u8"這個資料夾已經不在了";
				}
			} else if (r == DialogResult::Cancel) {
				ImGui::CloseCurrentPopup();
			}
			PobUi::EndDialog();
		}

		// 刪除資料夾
		{
			const int n = RegexFolders::Counts(state_, bmTab_)[folderEdit_];
			const std::string t = u8"刪除資料夾「" + folderEdit_ + u8"」？";
			const std::string body = u8"裡面的 " + std::to_string(n) + u8" 筆書籤會移回未分類，書籤本身不會刪除。";
			if (PobUi::ConfirmDialog("###rx_fdel", &oFolderDel, t.c_str(), body.c_str(), nullptr, u8"取消", u8"刪除", nullptr) ==
			    DialogResult::Danger) {
				const int moved = RegexFolders::Delete(state_, bmTab_, folderEdit_);
				if (moved >= 0) {
					say(u8"已刪除資料夾「" + folderEdit_ + u8"」，" + std::to_string(moved) + u8" 筆書籤移回未分類。");
					markStateDirty();
				}
			}
		}

		drawPasteDialog(oPaste, opening);
		drawTemplateDialog(oTpl);
		drawSendPickModal(oSend);
		drawImportModal(oImport, opening);
	}

	void commitName()
	{
		const std::string name = RegexAlgo::JsTrim(nameBuf_);
		if (editIdx_ >= 0) {
			if (editIdx_ < (int)state_.bookmarks.size()) {
				state_.bookmarks[editIdx_].name = name;
				say(u8"書籤已改名為「" + name + u8"」。");
				markStateDirty();
			}
		} else if (std::optional<RegexBookmark> body = currentBookmarkBody()) {
			// store.ts saveBookmark: appended = this game's uncategorised, which
			// sorts last, so the order invariant holds without a sort.
			body->name = name;
			state_.bookmarks.push_back(std::move(*body));
			bmTab_ = selGame_;
			say(u8"已存成書籤「" + name + u8"」。");
			markStateDirty();
		}
		editIdx_ = -1;
	}

	// RegexShare.dc.html "貼上分享碼" / "分享碼無法套用".
	void openPaste()
	{
		pasteBuf_.clear();
		pasteErr_.clear();
		pasteAuto_ = false;
		// RegexPanel.vue openPaste: pre-filled when the clipboard looks like a code
		if (!testMode_) {
			const std::string clip = RegexAlgo::JsTrim(ReadClipboardUtf8(host_ ? host_->hostHwnd : nullptr));
			if (LooksLikeCode(clip)) {
				pasteBuf_ = clip;
				pasteAuto_ = true;
			}
		}
		modal_ = Modal::Paste;
	}

	void drawPasteDialog(bool& open, Modal opening)
	{
		if (!PobUi::BeginDialog("###rx_paste", &open, u8"貼上分享碼", u8"套用後會覆蓋分享碼那個遊戲目前所有清單的勾選。", nullptr, 480.0f))
			return;
		const float inner = ImGui::GetContentRegionAvail().x;
		ImGui::Dummy(ImVec2(0, Dp(4.0f)));
		ImGui::PushStyleColor(ImGuiCol_Border, ImGui::ColorConvertU32ToFloat4(pasteErr_.empty() ? Tok::Border : Tok::Danger));
		ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
		ImGui::PushFont(SmallF());
		if (opening == Modal::Paste) ImGui::SetKeyboardFocusHere();
		if (ImGui::InputTextMultiline("##rx_paste_code", &pasteBuf_, ImVec2(inner, Dp(72.0f)))) {
			pasteErr_.clear();
			pasteAuto_ = false;
		}
		ImGui::PopFont();
		ImGui::PopStyleVar();
		ImGui::PopStyleColor();
		if (!pasteErr_.empty()) SmallText(pasteErr_.c_str(), Tok::Danger, inner);
		else if (pasteAuto_) SmallText(u8"剪貼簿裡的分享碼已自動帶入", Tok::TextMuted, inner);
		else SmallText(u8"不影響書籤；要保留目前的勾選，先存成書籤。", Tok::TextMuted, inner);
		const bool empty = RegexAlgo::JsTrim(pasteBuf_).empty();
		const PobUi::DialogResult r = DlgButtons(inner, u8"取消", u8"從剪貼簿貼上", u8"套用", !empty);
		if (r == PobUi::DialogResult::Primary) {
			RegexShare::Normalized d;
			std::string err;
			RegexBookmarksShare::Normalized bp;
			if (!RegexShare::Decode(pasteBuf_, d, &err)) {
				pasteErr_ = u8"分享碼無法套用：" + err;
				if (RegexBookmarksShare::Decode(pasteBuf_, bp, nullptr))
					pasteErr_ = u8"這是書籤包，不是分享碼：請用「匯入書籤包」。";
				else if (err.find(u8"版本不符") != std::string::npos)
					pasteErr_ += u8"（可能是較新版本的 PobTools / ExileAppraiser 產的）";
			} else if (!applyCombo(d.state, std::string(u8"分享碼（") + GameLabel(d.state.game) + u8"）", d.warnings, &err)) {
				pasteErr_ = u8"分享碼無法套用：" + err;
			} else {
				ImGui::CloseCurrentPopup();
			}
		} else if (r == PobUi::DialogResult::Secondary) {
			if (!testMode_) pasteBuf_ = RegexAlgo::JsTrim(ReadClipboardUtf8(host_ ? host_->hostHwnd : nullptr));
			pasteErr_.clear();
			pasteAuto_ = false;
		} else if (r == PobUi::DialogResult::Cancel) {
			ImGui::CloseCurrentPopup();
		}
		PobUi::EndDialog();
	}

	void drawTemplateDialog(bool& open)
	{
		const bool valid = tplPending_ >= 0 && tplPending_ < (int)templates_.size();
		const std::string title = valid ? u8"套用範本「" + templates_[tplPending_].nameZh + u8"」？" : std::string(u8"這個範本已經不在了");
		std::string body;
		if (valid) {
			const RegexShare::Template& t = templates_[tplPending_];
			body = t.descZh + (t.descZh.empty() ? "" : "\n") + u8"會覆蓋 " + GameLabel(t.game) +
			       u8" 目前所有清單的勾選、數值、自訂文字、排除詞與模式（不影響書籤）。";
		}
		const PobUi::DialogResult r = PobUi::ConfirmDialog("###rx_tpl", &open, title.c_str(), body.c_str(), nullptr, u8"取消", nullptr,
		                                                   valid ? u8"套用" : nullptr);
		if (r == PobUi::DialogResult::Primary && valid) {
			const RegexShare::Template t = templates_[tplPending_];
			std::string err;
			if (!applyCombo(t.state, u8"範本「" + t.nameZh + u8"」", {}, &err)) say(u8"範本無法套用：" + err, true);
		}
		if (r != PobUi::DialogResult::None) tplPending_ = -1;
	}

	// ---- share codes and templates (R8) ---------------------------------------
	//
	// exile-appraiser RegexPanel.vue head tools row: template drop-down, copy /
	// paste share code. store.ts currentShareState / makeShareCode / applyCombo.

	// Is the item-mod values page of game g still unloaded while the saved state has ticks on it?
	bool itemTicksPending(const std::string& g) const
	{
		const int ip = itemPageIndex(g);
		if (ip < 0 || imv_[GameIdx(g)].phase == ItemModLoad::Phase::Ready) return false;
		for (const RegexPagePicks& c : state_.current)
			if (c.page == refs_[ip].Id() && !c.keys.empty()) return true;
		return false;
	}

	bool hasShareable() const
	{
		return !combineOrderIdx(true).empty() || !state_.custom.empty() || !state_.excludes.empty() ||
		       itemTicksPending(selGame_);
	}

	// RegexPanel.vue openPaste: /^[A-Za-z0-9_-]{16,}$/
	static bool LooksLikeCode(const std::string& s)
	{
		if (s.size() < 16) return false;
		for (char c : s)
			if (!(isalnum((unsigned char)c) || c == '-' || c == '_')) return false;
		return true;
	}

	// store.ts makeShareCode / currentShareState: every page of the game with ticks.
	void makeShareCode()
	{
		std::string code;
		if (!buildShareCode(code)) return;
		shareCopyRequest_ = std::move(code);
		shareCopiedWhat_ = u8"分享碼";
		shareCopied_ = 0;
	}

	// The code "複製分享碼" copies and "送到 ExileAppraiser" sends. False (notice_
	// set) when the item-mod page's saved ticks could not be loaded.
	bool buildShareCode(std::string& out)
	{
		// Ticks saved on the item-mod page but not restored yet (it loads in the
		// background): load it now, or the code would silently leave them out.
		if (itemTicksPending(selGame_) && !ensureItemModsNow(selGame_)) {
			notice_ = u8"物品詞綴資料載入失敗（" + imv_[GameIdx(selGame_)].err + u8"），沒有產生分享碼：那一頁的勾選會漏掉。";
			return false;
		}
		RegexEmbed::PicksMap picks;
		RegexEmbed::ValuesMap values;
		for (int i = 0; i < (int)refs_.size(); i++) {
			if (refs_[i].Game() != selGame_) continue;
			picks[refs_[i].Id()] = picksOf(i);
			if (refs_[i].algo) {
				RegexAlgo::ValueMap& dst = values[RegexAlgo::NumericKeyOf(refs_[i].Id())];
				for (const auto& kv : pages_[i].algo.values)
					if (ownsValue(i, kv.first)) dst[kv.first] = kv.second;
			}
		}
		const RegexShare::State st = RegexShare::StateOf(selGame_, gamePages(selGame_), picks, values, modeId(),
		                                                 state_.custom, state_.excludes);
		out = RegexShare::Encode(st);
		return true;
	}

	// ---- 送到 ExileAppraiser (regex_send.h) ------------------------------------
	// Frame() only records what was asked; the registry / file dialog / process
	// start happen in RunDeferred, like the clipboard and the save.

	void relocateExileAppraiser()
	{
		sendLoc_ = RegexSend::Locate(RegexSend::RealEnv(), RegexSend::Widen(state_.exileAppraiserExe));
		sendLocAt_ = std::chrono::steady_clock::now();
	}

	void setSendMessage(bool ok, const std::string& msg)
	{
		sendOk_ = ok;
		sendMsg_ = msg;
		sendMsgAt_ = std::chrono::steady_clock::now();
		if (ok) PobLog::Diag("regex", u8"送到 ExileAppraiser：" + msg);
		else PobLog::Error("regex", u8"送到 ExileAppraiser：" + msg);
	}

	// Bookmarks that can be sent (they have a game; the dialog lists them by game).
	int sendableBookmarks() const
	{
		int n = 0;
		for (const RegexBookmark& b : state_.bookmarks) n += b.game.empty() ? 0 : 1;
		return n;
	}

	void openSendPick()
	{
		sendSel_ = RegexBookmarksShare::Selection{};
		sendPickTab_ = bmTab_;
		modal_ = Modal::SendPick;
		relocateWanted_ = true;
	}

	// "送到 ExileAppraiser": PoE1 / PoE2 tabs, each folder (and 未分類) a tri-state
	// tick over its bookmarks (regex_bookmarks_share Selection / PackOf). Not in
	// the drafts: built from the same dialog, segmented and tick pieces.
	void drawSendPickModal(bool& open)
	{
		namespace BS = RegexBookmarksShare;
		if (!PobUi::BeginDialog("###rx_sendpick", &open, u8"送到 ExileAppraiser",
		                        u8"勾選要交給 ExileAppraiser 的書籤：可以整個資料夾、也可以逐筆，PoE1 / PoE2 可以混選。", nullptr, 560.0f))
			return;
		const float inner = ImGui::GetContentRegionAvail().x;
		ImGui::Dummy(ImVec2(0, Dp(6.0f)));
		std::string labels[2];
		for (int gi = 0; gi < 2; gi++) {
			int total = 0, picked = 0;
			for (int i = 0; i < (int)state_.bookmarks.size(); i++)
				if (state_.bookmarks[i].game == kGames[gi]) {
					total++;
					picked += sendSel_.bookmarks.count(i) ? 1 : 0;
				}
			labels[gi] = std::string(GameLabel(kGames[gi])) + "  " + std::to_string(picked) + "/" + std::to_string(total);
		}
		const char* lp[2] = {labels[0].c_str(), labels[1].c_str()};
		int tab = sendPickTab_ == "poe2" ? 1 : 0;
		if (PobUi::Segmented("##sp_tab", &tab, lp, 2)) sendPickTab_ = kGames[tab];
		ImGui::SameLine(0, Dp(12.0f));
		if (PobUi::Button(u8"全選", PobUi::BtnKind::Ghost, PobUi::BtnSize::Sm)) BS::SetAll(state_, sendSel_, true);
		ImGui::SameLine(0, Dp(4.0f));
		if (PobUi::Button(u8"全不選", PobUi::BtnKind::Ghost, PobUi::BtnSize::Sm)) BS::SetAll(state_, sendSel_, false);
		ImGui::Dummy(ImVec2(0, Dp(4.0f)));

		const std::string g = sendPickTab_;
		const ImVec2 lp0 = ImGui::GetCursorScreenPos();
		const float listH = Dp(260.0f);
		ImGui::GetWindowDrawList()->AddRectFilled(lp0, ImVec2(lp0.x + inner, lp0.y + listH), Tok::Surface1, Dp(8.0f));
		ImGui::GetWindowDrawList()->AddRect(lp0, ImVec2(lp0.x + inner, lp0.y + listH), Tok::BorderSubtle, Dp(8.0f), 0, 1.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Dp(8.0f), Dp(6.0f)));
		ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
		ImGui::BeginChild("##rx_sp_list", ImVec2(inner, listH), false, ImGuiWindowFlags_AlwaysUseWindowPadding);
		const RegexFolders::Grouped grouped = RegexFolders::GroupBookmarks(state_, g, false);
		bool any = false;
		auto tickRow = [&](const char* id, const std::string& text, const std::string& note, bool on, bool mixed, float indent) {
			ImGui::PushID(id);
			const float w = ImGui::GetContentRegionAvail().x;
			const float h = Dp(28.0f);
			const ImVec2 p = ImGui::GetCursorScreenPos();
			const bool click = ImGui::InvisibleButton("##t", ImVec2(w, h));
			ImDrawList* dl = ImGui::GetWindowDrawList();
			if (ImGui::IsItemHovered()) dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), Tok::Surface2, Dp(5.0f));
			DrawCheck(dl, ImVec2(p.x + indent + Dp(6.0f), p.y + std::floor((h - Dp(15.0f)) * 0.5f)), on, mixed);
			const float tx = p.x + indent + Dp(6.0f) + Dp(15.0f) + Dp(8.0f);
			DrawTextAt(dl, BodyF(), ImVec2(tx, p.y + std::floor((h - BodyF()->FontSize) * 0.5f)), Tok::Text, text.c_str());
			if (!note.empty())
				DrawTextAt(dl, SmallF(), ImVec2(tx + TextSz(BodyF(), text.c_str()).x + Dp(8.0f), p.y + std::floor((h - SmallF()->FontSize) * 0.5f)),
				           Tok::TextMuted, note.c_str());
			ImGui::PopID();
			return click;
		};
		for (size_t gi = 0; gi < grouped.groups.size(); gi++) {
			const RegexFolders::Group& grp = grouped.groups[gi];
			if (grp.folder.empty() && grp.items.empty()) continue;
			any = true;
			ImGui::PushID((int)gi);
			const BS::Tri t = BS::GroupTri(state_, sendSel_, g, grp.folder);
			const std::string head = grp.folder.empty() ? std::string(u8"未分類") : grp.folder;
			if (tickRow("grp", head, std::to_string(grp.items.size()) + u8" 筆" + (grp.items.empty() ? u8"（空資料夾：勾了會在對方建立同名資料夾）" : ""),
			            t == BS::Tri::All, t == BS::Tri::Some, 0.0f))
				BS::SetGroup(state_, sendSel_, g, grp.folder, t != BS::Tri::All);
			for (int i : grp.items) {
				const RegexBookmark& b = state_.bookmarks[i];
				ImGui::PushID(i);
				const bool on = sendSel_.bookmarks.count(i) > 0;
				if (tickRow("bm", b.name, pageTitleIn(g, b.page), on, false, Dp(24.0f))) {
					if (on) sendSel_.bookmarks.erase(i);
					else sendSel_.bookmarks.insert(i);
				}
				ImGui::PopID();
			}
			ImGui::PopID();
		}
		if (!any) SmallText((std::string(GameLabel(g)) + u8" 沒有書籤。").c_str(), Tok::TextFaint);
		ImGui::EndChild();
		ImGui::PopStyleColor();
		ImGui::PopStyleVar();

		BS::PackStats st;
		const BS::Pack pack = BS::PackOf(state_, sendSel_, &st);
		const int n = st.bookmarks[0] + st.bookmarks[1];
		const int nf = st.folders[0] + st.folders[1];
		std::string summary = u8"已選 " + std::to_string(n) + u8" 筆（PoE1 " + std::to_string(st.bookmarks[0]) + u8"、PoE2 " +
		                      std::to_string(st.bookmarks[1]) + u8"）";
		if (nf > 0) summary += u8"，資料夾 " + std::to_string(nf) + u8" 個";
		ImGui::Dummy(ImVec2(0, Dp(4.0f)));
		ImGui::TextUnformatted(summary.c_str());
		if (st.skipped > 0) SmallText((u8"有 " + std::to_string(st.skipped) + u8" 筆缺名稱 / 頁 / 勾選，不會送出。").c_str(), Tok::Warning, inner);
		SmallText(sendLoc_.exe.empty() ? u8"找不到 ExileAppraiser.exe：按「送出」會請你手動指定。"
		                               : (u8"送到：" + RegexSend::Narrow(sendLoc_.exe)).c_str(),
		          Tok::TextMuted, inner);
		SmallText(u8"對方會跳確認框，按「加入」才寫入；同名改名加「 (2)」；不帶熱鍵；不動它目前的勾選。需要 v0.2.1 以上。", Tok::TextMuted, inner);
		const PobUi::DialogResult r = DlgButtons(inner, u8"取消", u8"複製書籤包", u8"送出", !st.Empty());
		if (r == PobUi::DialogResult::Primary) {
			sendCode_ = BS::Encode(pack);
			sendKind_ = RegexSend::Kind::Bookmarks;
			sendWhat_ = std::to_string(n) + u8" 筆書籤" + (nf ? u8"、" + std::to_string(nf) + u8" 個資料夾" : std::string());
			sendRequest_ = true;
			ImGui::CloseCurrentPopup();
		} else if (r == PobUi::DialogResult::Secondary && !st.Empty()) {
			// not sent: the pack to the clipboard (匯入書籤包 here, or ExileAppraiser, reads it)
			shareCopyRequest_ = BS::Encode(pack);
			shareCopiedWhat_ = u8"書籤包（" + std::to_string(n) + u8" 筆）";
			shareCopied_ = 0;
			ImGui::CloseCurrentPopup();
		} else if (r == PobUi::DialogResult::Cancel) {
			ImGui::CloseCurrentPopup();
		}
		PobUi::EndDialog();
	}

	// ---- 匯入書籤包 (bookmarks-share.ts decodeBookmarks + mergeBookmarks, the receiving side here) ----

	void openImport()
	{
		importBuf_.clear();
		importFor_ = "\x01";   // never equal to a real buffer: parsed on the first frame
		if (!testMode_) {
			const std::string clip = RegexAlgo::JsTrim(ReadClipboardUtf8(host_ ? host_->hostHwnd : nullptr));
			if (LooksLikeCode(clip)) importBuf_ = clip;
		}
		modal_ = Modal::ImportPack;
	}

	void drawImportModal(bool& open, Modal opening)
	{
		namespace BS = RegexBookmarksShare;
		if (!PobUi::BeginDialog("###rx_import", &open, u8"匯入書籤包", u8"把書籤包貼在下面（剪貼簿裡像碼的內容會自動帶入）。", nullptr, 480.0f))
			return;
		const float inner = ImGui::GetContentRegionAvail().x;
		ImGui::Dummy(ImVec2(0, Dp(4.0f)));
		if (opening == Modal::ImportPack) ImGui::SetKeyboardFocusHere();
		ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
		ImGui::PushFont(SmallF());
		ImGui::InputTextMultiline("##rx_import_code", &importBuf_, ImVec2(inner, Dp(72.0f)));
		ImGui::PopFont();
		ImGui::PopStyleVar();
		if (importBuf_ != importFor_) {
			importFor_ = importBuf_;
			importErr_.clear();
			importOk_ = false;
			if (!RegexAlgo::JsTrim(importBuf_).empty()) {
				std::string err;
				importOk_ = BS::Decode(importBuf_, importPack_, &err);
				if (!importOk_) {
					RegexShare::Normalized sh;
					importErr_ = RegexShare::Decode(importBuf_, sh, nullptr) ? std::string(u8"這是分享碼，不是書籤包：請用「貼上分享碼」。")
					                                                         : u8"書籤包無法讀取：" + err;
				}
			}
		}
		bool canAdd = false;
		if (!importErr_.empty()) {
			SmallText(importErr_.c_str(), Tok::Danger, inner);
		} else if (importOk_) {
			const BS::MergeResult m = BS::Merge(state_, importPack_.pack);
			int per[2] = {0, 0};
			for (const RegexBookmark& b : importPack_.pack.bookmarks) per[b.game == "poe2" ? 1 : 0]++;
			canAdd = !m.added.empty() || !m.foldersCreated.empty();
			ImGui::TextUnformatted((u8"會加入 " + std::to_string(m.added.size()) + u8" 筆書籤（PoE1 " + std::to_string(per[0]) + u8"、PoE2 " +
			                        std::to_string(per[1]) + u8"）").c_str());
			if (!m.foldersCreated.empty()) {
				std::string f;
				for (const auto& c : m.foldersCreated) f += (f.empty() ? "" : u8"、") + std::string(GameLabel(c.first)) + " " + c.second;
				SmallText((u8"新資料夾：" + f).c_str(), Tok::TextMuted, inner);
			}
			if (!m.renamed.empty()) {
				std::string r;
				for (size_t i = 0; i < m.renamed.size() && i < 6; i++)
					r += (i ? u8"、" : "") + m.renamed[i].originalName + u8" → " + m.renamed[i].name;
				if (m.renamed.size() > 6) r += u8"…";
				SmallText((u8"同名改名：" + r).c_str(), Tok::Warning, inner);
			}
			if (!importPack_.warnings.empty())
				SmallText((u8"書籤包有 " + std::to_string(importPack_.warnings.size()) + u8" 處格式不對，已略過（第一處：" +
				           importPack_.warnings[0] + u8"）").c_str(),
				          Tok::Warning, inner);
			SmallText(u8"只加入書籤與資料夾：目前的勾選、數值、自訂文字、排除詞與模式都不動。", Tok::TextMuted, inner);
		}
		const PobUi::DialogResult r = DlgButtons(inner, u8"取消", u8"從剪貼簿貼上", u8"加入", canAdd);
		if (r == PobUi::DialogResult::Primary) {
			const BS::MergeResult m = BS::Merge(state_, importPack_.pack);
			state_.bookmarks = m.state.bookmarks;
			state_.folders[0] = m.state.folders[0];
			state_.folders[1] = m.state.folders[1];
			if (!m.added.empty()) bmTab_ = m.added.front().game;
			say(u8"已從書籤包加入 " + std::to_string(m.added.size()) + u8" 筆書籤" +
			    (m.renamed.empty() ? std::string() : u8"（" + std::to_string(m.renamed.size()) + u8" 筆同名已改名）") +
			    (m.foldersCreated.empty() ? std::string() : u8"，新資料夾 " + std::to_string(m.foldersCreated.size()) + u8" 個") + u8"。");
			for (const std::string& w : importPack_.warnings) PobLog::Diag("regex", u8"書籤包警告：" + w);
			markStateDirty();
			ImGui::CloseCurrentPopup();
		} else if (r == PobUi::DialogResult::Secondary) {
			if (!testMode_) importBuf_ = RegexAlgo::JsTrim(ReadClipboardUtf8(host_ ? host_->hostHwnd : nullptr));
		} else if (r == PobUi::DialogResult::Cancel) {
			ImGui::CloseCurrentPopup();
		}
		PobUi::EndDialog();
	}

	// True when a file was picked (and stored).
	bool pickExileAppraiser()
	{
		const std::wstring init = sendLoc_.exe.empty() ? std::wstring() : RegexSend::DirOf(sendLoc_.exe);
		const std::wstring picked = RegexSend::PickExeDialog(host_ ? host_->hostHwnd : nullptr, init);
		if (picked.empty()) return false;
		state_.exileAppraiserExe = RegexSend::Narrow(picked);
		markStateDirty();
		relocateExileAppraiser();
		return true;
	}

	void runSendRequests()
	{
		if (pickRequest_) {
			pickRequest_ = false;
			if (pickExileAppraiser()) setSendMessage(true, u8"已指定 " + state_.exileAppraiserExe);
		}
		if (relocateWanted_) {
			relocateWanted_ = false;
			relocateExileAppraiser();
		}
		if (!sendRequest_) return;
		sendRequest_ = false;
		const std::string code = std::move(sendCode_);
		const RegexSend::Kind kind = sendKind_;
		const std::string what = std::move(sendWhat_);
		sendCode_.clear();
		sendWhat_.clear();
		sendKind_ = RegexSend::Kind::Bookmarks;
		relocateExileAppraiser();   // fresh: it may have moved / closed since the last look
		if (sendLoc_.exe.empty() && !pickExileAppraiser()) {
			setSendMessage(false, u8"沒有送出：找不到 ExileAppraiser.exe（右鍵可手動指定）");
			return;
		}
		const std::wstring tmp = RegexSend::DefaultTempDir();
		if (code.size() > RegexSend::kMaxInlineChars) RegexSend::SweepTempDir(tmp, RegexSend::kTempMaxAgeSeconds);
		const RegexSend::Result r = RegexSend::Send(sendLoc_.exe, code, tmp, RegexSend::RealLauncher(),
		                                            RegexSend::NowStamp(), RegexSend::RandomU32(),
		                                            RegexSend::kMaxInlineChars, kind);
		if (!r.tempFile.empty()) sendFiles_.push_back(r.tempFile);
		setSendMessage(r.ok, (r.ok && !what.empty() ? what + u8"：" : std::string()) + r.message);
		// Also in the notice line, which stays until dismissed (the message beside the button fades).
		say(u8"送到 ExileAppraiser：" + sendMsg_, !r.ok);
		PobUi::ShowToast(r.ok ? u8"已送到 ExileAppraiser" : u8"送到 ExileAppraiser 失敗", r.ok ? PobUi::Tone::Ok : PobUi::Tone::Bad);
	}

	// store.ts applyCombo: OVERWRITE every page of the code's game (ticks; values
	// merged in), custom text, excludes and mode; show the merged view. Returns
	// false (with *err, nothing changed) when it cannot be applied at all.
	bool applyCombo(const RegexShare::State& s, const std::string& what, const std::vector<std::string>& warnings,
	                std::string* err)
	{
		const std::string g = s.game;
		if (firstPageOf(g) < 0) {
			if (err) *err = std::string(u8"這個安裝沒有 ") + GameLabel(g) + u8" 的清單（Data\\regex_" + g + u8".json），無法套用。";
			return false;
		}
		// The item-mod values page resolves against its entries: load it first (store.ts prepareItemMods).
		const std::string itemId = RegexItemMods::PageId(g);
		bool needItem = false;
		for (const auto& kv : s.pages) needItem = needItem || kv.first == itemId;
		for (const auto& kv : s.numeric) needItem = needItem || kv.first == itemId;
		if (needItem && itemPageIndex(g) >= 0 && !ensureItemModsNow(g)) {
			if (err) *err = u8"物品詞綴資料載入失敗（" + imv_[GameIdx(g)].err + u8"），沒有套用。";
			return false;
		}
		if (selGame_ != g) switchGame(g);
		const RegexShare::Resolved r = RegexShare::Resolve(s, gamePages(g));
		for (int idx = 0; idx < (int)refs_.size(); idx++) {
			if (refs_[idx].Game() != g) continue;
			auto it = r.picks.find(refs_[idx].Id());
			setTicks(idx, it != r.picks.end() ? it->second : std::vector<int>());
			syncCurrent(idx);
		}
		for (const auto& kv : RegexShare::ResolvedValues(r.values))
			for (int idx = 0; idx < (int)refs_.size(); idx++) {
				if (!refs_[idx].algo || refs_[idx].Game() != g || RegexAlgo::NumericKeyOf(refs_[idx].Id()) != kv.first) continue;
				for (const auto& e : kv.second)
					if (ownsValue(idx, e.first)) pages_[idx].algo.values[e.first] = e.second;
				syncValues(idx);
			}
		state_.custom = s.custom;
		state_.excludes = s.excludes;
		mode_ = ModeFromId(s.mode);
		state_.mode = modeId();
		state_.game = selGame_;
		setScope(true);
		setView(View::Combined);
		for (PageState& ps : pages_) {
			ps.dirty = true;
			ps.filterDirty = true;
		}
		combinedDirty_ = true;
		copied_ = false;
		markStateDirty();

		int legacyItemKeys = 0;   // the item-mod page's pre-2026-10-09 keys (GGPK stat ids)
		for (const auto& kv : s.pages)
			if (kv.first == itemId)
				for (const std::string& key : kv.second) legacyItemKeys += RegexItemMods::IsLegacyKey(key) ? 1 : 0;
		PobUi::ShowToast((u8"已套用" + what).c_str(), PobUi::Tone::Ok);
		std::string title, desc;
		if (r.missed > 0 || !r.unknownPages.empty()) {
			// no spaces inside the sentence: ImGui wraps CJK text only at spaces
			title = u8"已套用" + what + u8"，但有" + std::to_string(r.missed > 0 ? r.missed : (int)r.unknownPages.size()) + u8"項在目前資料找不到";
			std::string pages;
			for (const std::string& id : r.unknownPages) pages += (pages.empty() ? "" : u8"、") + id;
			if (!pages.empty()) desc += u8"不存在的清單：" + pages + u8"。";
			for (const auto& kv : r.missedByPage)
				if (RegexItemMods::IsPageId(kv.first) && legacyItemKeys > 0)
					desc += u8"其中 " + std::to_string(legacyItemKeys) + u8" 項是「物品詞綴數值」頁的舊版鍵（GGPK stat id）：這一頁已改用交易站 stat id，請重新勾選。";
			desc += u8"可能是對方的版本比較新或比較舊。";
		}
		if (!warnings.empty()) {
			if (title.empty()) title = u8"已套用" + what + u8"，但分享碼有 " + std::to_string(warnings.size()) + u8" 處格式不對，已略過";
			desc += u8"格式不對：";
			for (size_t i = 0; i < warnings.size() && i < 3; i++) desc += (i ? u8"；" : "") + warnings[i];
			if (warnings.size() > 3) desc += u8"…";
		}
		if (!title.empty()) say(title, true, desc);
		else notice_.clear();
		for (const std::string& w : warnings) PobLog::Diag("regex", u8"分享碼警告：" + w);
		return true;
	}

	// ---- plumbing ------------------------------------------------------------

	bool pageHasT17()
	{
		if (t17Cache_ != page_) {
			t17Cache_ = page_;
			t17Present_ = false;
			for (const RegexEntryDef& e : entries())
				if (e.t17) { t17Present_ = true; break; }
		}
		return t17Present_;
	}

	void setLang(Lang l)
	{
		if (l == lang_) return;
		lang_ = l;
		state_.lang = (l == Lang::En) ? "en" : "zh";
		stateDirty_ = true;
		copied_ = false;
		invalidateCorpora();
	}

	void switchPage(int p)
	{
		if (p >= 0 && p < (int)refs_.size() && isItemPage(p)) startItemMods(refs_[p].Game());
		if (p == page_) return;
		page_ = p;
		combinedDirty_ = true;   // R10: the merge takes the current page's item group
		copied_ = false;
		st().filterDirty = true;
		st().dirty = true;
		state_.game = selGame_;
		state_.page = pageId();
		stateDirty_ = true;
	}

	// Moving to a game moves to its first list. Nothing is thrown away: every
	// page keeps its own ticks, so coming back finds the work where it was left.
	void switchGame(const std::string& g)
	{
		if (g == selGame_) return;
		const int first = firstPageOf(g);
		if (first < 0) return;
		selGame_ = g;
		combinedDirty_ = true;   // the merge is per game
		switchPage(first);
		state_.game = selGame_;
		stateDirty_ = true;
	}

	void refreshFilter()
	{
		PageState& s = st();
		const std::string needle = ToLowerAscii(s.search);
		s.visible.clear();
		// Ticked first, then the rest, each keeping the data file's order. Two
		// passes rather than a sort: a sort would need a comparator that is a
		// strict weak ordering over "is it ticked", and this says the same thing
		// in a way that cannot silently shuffle equal rows between frames.
		for (int pass = 0; pass < 2; pass++) {
			const bool wantPicked = (pass == 0);
			for (int i = 0; i < (int)entries().size(); i++) {
				const bool isPicked = i < (int)s.picked.size() && s.picked[i] != 0;
				if (isPicked != wantPicked) continue;
				const RegexEntryDef& e = entries()[i];
				if (s.groupFilter >= 0 && e.group != s.groupFilter) continue;
				if (s.t17Only && !e.t17) continue;
				if (s.hideT17 && e.t17) continue;
				if (!needle.empty() && !matches(e, needle)) continue;
				s.visible.push_back(i);
			}
		}
		s.filterDirty = false;
	}

	// Chinese, English, the affix name and the GGPK id all count as searchable:
	// people look for 反射 and for "reflect" and occasionally for the mod id off a
	// wiki page, and the cheapest way to be right is to accept all of them.
	static bool matches(const RegexEntryDef& e, const std::string& needle)
	{
		for (const std::string& l : e.zh)
			if (l.find(needle) != std::string::npos) return true;
		for (const std::string& l : e.en)
			if (ToLowerAscii(l).find(needle) != std::string::npos) return true;
		if (!e.affixZh.empty() && e.affixZh.find(needle) != std::string::npos) return true;
		return ToLowerAscii(e.id).find(needle) != std::string::npos;
	}

	// The corpus is every entry on the page, not just the ticked ones: "does this
	// token also hit something else?" is a question about the whole list, and
	// building it from the selection would make the answer change as the player
	// ticks -- which is exactly the bug that produces false positives.
	//
	// The lines are the ones in the language being built for: "no false
	// positives" is a claim about ONE language's list, so the corpus has to be
	// the one the player will paste into (RegexAlgo::BuildPageCorpus, shared
	// with --regex-selftest).
	void buildCorpus()
	{
		PageState& s = st();
		RegexAlgo::BuildPageCorpus(*refs_[page_].corpus, fragLang(), s.corpus);
		s.corpusReady = true;
		s.dirty = true;
	}

	RegexFrag::Lang fragLang() const { return lang_ == Lang::Zh ? RegexFrag::Lang::Zh : RegexFrag::Lang::En; }

	// The page's string: its own picks, plus its numeric section for a host
	// page, in one go (exile-appraiser store.ts pageCombined). With no section
	// picks this is exactly the corpus Build() query, as before.
	void recompute()
	{
		PageState& s = st();
		std::vector<RegexAlgo::CombineSel> sels;
		RegexAlgo::CombineSel own;
		own.page = refs_[page_];
		if (isAlgo()) {
			own.picks = s.algo.Picks();
			own.values = &s.algo.values;
		} else {
			for (int i = 0; i < (int)s.picked.size(); i++)
				if (s.picked[i]) own.picks.push_back(i);
			own.corpus = &s.corpus;
		}
		sels.push_back(own);
		const int sec = sectionIndexOf(page_);
		if (sec >= 0) {
			RegexAlgo::CombineSel b;
			b.page = refs_[sec];
			b.picks = pages_[sec].algo.Picks();
			b.values = &pages_[sec].algo.values;
			sels.push_back(b);
		}
		s.combined = RegexAlgo::CombineSingle(fragLang(), mode_, sels);
		s.result = s.combined.corpusResult;
		s.dirty = false;
	}

	int sectionPickCount() const
	{
		if (!hasPage()) return 0;
		const int sec = sectionIndexOf(page_);
		return sec >= 0 ? pages_[sec].algo.Count() : 0;
	}

	// ---- algorithmic rows (vendor page, numeric section) -----------------------
	//
	// Row layout after exile-appraiser RegexAlgoList.vue: tick + name | input |
	// the fragment this row produces. Editing a value ticks the row.

	// A number box that can be empty (.num: 58 px, right-aligned). Returns true
	// when edited; `out` is the new value, nullopt when the box was cleared or
	// holds no number (TS: `raw === '' || !Number.isFinite(n)` deletes the bound).
	bool numField(const char* id, const std::optional<double>& val, std::optional<double>& out, float w = 0.0f)
	{
		std::string buf = val ? NumText(*val) : std::string();
		ImGui::PushFont(SmallF());
		const float h = Dp(26.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(Dp(6.0f), std::max(0.0f, std::floor((h - SmallF()->FontSize) * 0.5f))));
		ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
		ImGui::PushStyleColor(ImGuiCol_Border, ImGui::ColorConvertU32ToFloat4(Tok::Border));
		ImGui::PushStyleColor(ImGuiCol_FrameBg, ImGui::ColorConvertU32ToFloat4(Tok::Surface2));
		ImGui::SetNextItemWidth(w > 0 ? w : Dp(58.0f));
		const bool edited = ImGui::InputText(id, &buf, ImGuiInputTextFlags_CharsDecimal);
		ImGui::PopStyleColor(2);
		ImGui::PopStyleVar(2);
		ImGui::PopFont();
		if (!edited) return false;
		size_t a = buf.find_first_not_of(" \t");
		if (a == std::string::npos) {
			out.reset();
			return true;
		}
		const std::string t = buf.substr(a);
		char* end = nullptr;
		const double v = std::strtod(t.c_str(), &end);
		if (end && *end == '\0' && std::isfinite(v)) out = v;
		else out.reset();
		return true;
	}

	static std::string NumText(double v)
	{
		char buf[32];
		if (std::floor(v) == v && std::fabs(v) < 1e15) snprintf(buf, sizeof buf, "%.0f", v);
		else snprintf(buf, sizeof buf, "%g", v);
		return buf;
	}

	// Lays the controls of one .nr cell out left to right, wrapping inside the
	// cell (the .ctl flex-wrap): next(w) moves the cursor to where an item `w`
	// wide goes.
	struct Flow {
		float x0, right, x, y, lineH, gap;
		void next(float w)
		{
			if (x > x0 && x + w > right) {
				x = x0;
				y += lineH + gap;
			}
			ImGui::SetCursorScreenPos(ImVec2(x, y));
			x += w + gap;
		}
		float bottom() const { return y + lineH; }
	};

	// .nr: tick + name | the input | the fragment it gives (1.1 / 1.6 / 1.3 with
	// minimums 150 / 220 / 120). A ticked row is tinted (warning-soft); clicking
	// the name ticks / unticks; editing a value ticks the row.
	void drawAlgoRow(int idx, int i, float width)
	{
		using namespace RegexAlgo;
		PageState& s = pages_[idx];
		const AlgoPage& page = *refs_[idx].algo;
		const AlgoEntry& e = page.entries[i];
		ImGui::PushID(i);
		const bool on = s.algo.picked[i] != 0;
		const float padX = Dp(10.0f), padY = Dp(6.0f), colGap = Dp(10.0f);
		const float inner = width - padX * 2 - colGap * 2;
		float c0 = inner * 1.1f / 4.0f, c1 = inner * 1.6f / 4.0f, c2 = inner * 1.3f / 4.0f;
		if (inner >= PobUi::D(490.0f)) {
			c0 = std::max(c0, PobUi::D(150.0f));
			c1 = std::max(c1, PobUi::D(220.0f));
			c2 = inner - c0 - c1;
		}
		const ImVec2 p = ImGui::GetCursorScreenPos();
		ImDrawList* dl = ImGui::GetWindowDrawList();
		ImDrawListSplitter split;
		split.Split(dl, 2);
		split.SetCurrentChannel(dl, 1);
		const float rowH0 = Dp(26.0f);

		// name (clickable)
		const float nx = p.x + padX;
		const std::string name = e.def.zh.empty() ? e.def.id : e.def.zh[0];
		ImGui::SetCursorScreenPos(ImVec2(nx, p.y + padY));
		const bool click = ImGui::InvisibleButton("##name", ImVec2(c0, rowH0));
		const bool hovName = ImGui::IsItemHovered();
		if (hovName) {
			ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
			if (!e.def.en.empty()) PobUi::Tooltip(e.def.en[0].c_str());
		}
		DrawCheck(dl, ImVec2(nx, p.y + padY + std::floor((rowH0 - Dp(15.0f)) * 0.5f)), on);
		const float tx = nx + Dp(15.0f) + Dp(8.0f);
		float nameBottom = p.y + padY + rowH0;
		{
			const float tw = c0 - Dp(23.0f);
			const std::vector<std::string> lines = WrapAnywhere(BodyF(), name, std::max(Dp(40.0f), tw));
			const float lh = BodyF()->FontSize * 1.25f;
			float ty = p.y + padY + std::floor((rowH0 - BodyF()->FontSize) * 0.5f);
			for (const std::string& l : lines) {
				DrawTextAt(dl, BodyF(), ImVec2(tx, ty), Tok::Text, l.c_str());
				ty += lh;
			}
			nameBottom = std::max(nameBottom, ty);
			if (e.untested) {
				const char* t = u8"待實測";
				const float lastW = TextSz(BodyF(), lines.back().c_str()).x;
				ImVec2 pp(tx + lastW + Dp(6.0f), ty - lh + std::floor((BodyF()->FontSize - SmallF()->FontSize) * 0.5f));
				if (pp.x + PillW(t) > nx + c0) pp = ImVec2(tx, ty);
				ImGui::SetCursorScreenPos(pp);
				Pill(t, Tok::Warning, Tok::WarningSoft);
				if (ImGui::IsItemHovered()) PobUi::Tooltip(u8"假設繁中客戶端的插槽顯示為 R-G-B（顏色字母與 - 不翻譯），尚未進遊戲確認。");
				nameBottom = std::max(nameBottom, ImGui::GetItemRectMax().y);
			}
		}

		// input
		Flow fl{nx + c0 + colGap, nx + c0 + colGap + c1, nx + c0 + colGap, p.y + padY, rowH0, Dp(6.0f)};
		const AlgoValue cur = ValueOf(s.algo.values, e);   // a copy: the edit below replaces it
		std::optional<AlgoValue> next;
		std::optional<double> n;
		auto hint = [&](const char* t) {
			const float w = TextSz(SmallF(), t).x;
			fl.next(w);
			ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, fl.y + std::floor((rowH0 - SmallF()->FontSize) * 0.5f)));
			SmallText(t);
		};
		switch (e.input.kind) {
		case InputKind::Range: {
			const RangeOp op = OpOf(e, cur);
			if (e.input.ops.size() > 1) {
				std::vector<const char*> labels;
				std::vector<char> lit;
				for (RangeOp o : e.input.ops) {
					labels.push_back(o == RangeOp::Ge ? u8"≥" : o == RangeOp::Le ? u8"≤" : u8"區間");
					lit.push_back(op == o);
				}
				fl.next(SegTogglesW(labels.data(), (int)labels.size()));
				const int k = SegToggles("##ops", labels.data(), (const bool*)lit.data(), (int)labels.size());
				if (k >= 0) next = WithOp(e, cur, e.input.ops[k]);
			} else {
				hint(u8"≥");
			}
			if (op != RangeOp::Le) {
				fl.next(Dp(58.0f));
				if (numField("##min", cur.min, n)) next = WithNum(e, cur, false, n);
			}
			if (op == RangeOp::Range) hint(u8"–");
			if (op != RangeOp::Ge) {
				fl.next(Dp(58.0f));
				if (numField("##max", cur.max, n)) next = WithNum(e, cur, true, n);
			}
			if (e.input.percent) hint("%");
			break;
		}
		case InputKind::Select: {
			const std::vector<AlgoOption>& opts = e.input.options;
			if (opts.size() > 5) {
				// many options (the eight influences): a drop-down keeps the row narrow
				std::vector<const char*> labels;
				int sel = -1;
				for (size_t k = 0; k < opts.size(); k++) {
					labels.push_back(opts[k].zh.c_str());
					if (opts[k].id == cur.choice) sel = (int)k;
				}
				const float w = std::max(Dp(170.0f), PobUi::SelectFitWidth(labels.data(), (int)labels.size()));
				fl.next(w);
				int s2 = sel < 0 ? 0 : sel;
				if (PobUi::Select("##choice", &s2, labels.data(), nullptr, (int)labels.size(), w) || (sel < 0 && false))
					next = WithChoice(cur, opts[s2].id);
			} else {
				std::vector<const char*> labels;
				std::vector<char> lit;
				for (const AlgoOption& o : opts) {
					labels.push_back(o.zh.c_str());
					lit.push_back(cur.choice == o.id);
				}
				fl.next(SegTogglesW(labels.data(), (int)labels.size()));
				const int k = SegToggles("##choice", labels.data(), (const bool*)lit.data(), (int)labels.size());
				if (k >= 0) next = WithChoice(cur, opts[k].id);
			}
			break;
		}
		case InputKind::Count: {
			const std::vector<AlgoOption>& opts = e.input.options;
			std::vector<const char*> labels;
			std::vector<char> lit;
			for (const AlgoOption& o : opts) {
				labels.push_back(o.zh.c_str());
				lit.push_back(cur.choice == o.id);
			}
			fl.next(SegTogglesW(labels.data(), (int)labels.size()));
			const int k = SegToggles("##color", labels.data(), (const bool*)lit.data(), (int)labels.size());
			if (k >= 0) next = WithChoice(cur, opts[k].id);
			hint(u8"≥");
			fl.next(Dp(44.0f));
			if (numField("##min", cur.min, n, Dp(44.0f))) next = WithNum(e, cur, false, n);
			break;
		}
		case InputKind::Rarity: {
			// "普通 魔法 稀有 傳奇 | 未汙染 已汙染": rarities multi-select, corruption
			// one of two (clicking the lit one clears it), a divider between
			const RarityChoice rc = ParseRarityChoice(cur.choice);
			std::vector<const char*> rl, rt;
			std::vector<char> rlit;
			std::vector<std::string> tips;
			for (const AlgoOption& o : e.input.options) {
				rl.push_back(o.zh.c_str());
				rlit.push_back(std::find(rc.rarity.begin(), rc.rarity.end(), o.id) != rc.rarity.end());
				tips.push_back(o.en + u8"（可多選）");
			}
			for (const std::string& t : tips) rt.push_back(t.c_str());
			fl.next(SegTogglesW(rl.data(), (int)rl.size()));
			const int k = SegToggles("##rar", rl.data(), (const bool*)rlit.data(), (int)rl.size(), rt.data());
			if (k >= 0) next = WithChoice(cur, ToggleRarityIn(cur.choice, e.input.options[k].id));
			fl.next(Dp(9.0f));
			VSep();
			std::vector<const char*> cl;
			std::vector<char> clit;
			std::vector<Corruption> cv;
			for (const AlgoOption& o : e.input.corruption) {
				cl.push_back(o.zh.c_str());
				const Corruption c = o.id == "uncorrupted" ? Corruption::Uncorrupted : Corruption::Corrupted;
				cv.push_back(c);
				clit.push_back(rc.corruption == c);
			}
			const char* ctips[2] = {u8"排除「已汙染」行（再點一次取消）", u8"只要有「已汙染」行（再點一次取消）"};
			fl.next(SegTogglesW(cl.data(), (int)cl.size()));
			const int c = SegToggles("##cor", cl.data(), (const bool*)clit.data(), (int)cl.size(), cl.size() == 2 ? ctips : nullptr);
			if (c >= 0) next = WithChoice(next ? *next : cur, ToggleCorruptionIn(cur.choice, cv[c]));
			break;
		}
		case InputKind::Colors: {
			static const char kLetters[3] = {'r', 'g', 'b'};
			static const ImU32 kDot[3] = {IM_COL32(0xc0, 0x39, 0x2b, 255), IM_COL32(0x27, 0xae, 0x60, 255), IM_COL32(0x2e, 0x6f, 0xd0, 255)};
			for (int k = 0; k < 3; k++) {
				ImGui::PushID(k);
				const float d = Dp(18.0f);
				fl.next(d);
				const ImVec2 cp = ImGui::GetCursorScreenPos();
				const ImVec2 cc(cp.x + d * 0.5f, fl.y + rowH0 * 0.5f);
				dl->AddCircleFilled(cc, d * 0.5f, kDot[k]);
				const char L[2] = {(char)(kLetters[k] - 'a' + 'A'), 0};
				const ImVec2 ls = TextSz(SmallF(), L);
				DrawTextAt(dl, SmallF(), ImVec2(cc.x - ls.x * 0.5f, cc.y - ls.y * 0.5f), Tok::OnAccent, L);
				ImGui::Dummy(ImVec2(d, rowH0));
				fl.next(Dp(44.0f));
				const std::optional<double> count = (double)ColorCount(cur, kLetters[k]);
				if (numField("##n", count, n, Dp(44.0f))) {
					// TS: Math.trunc(Number(value) || 0), clamped 0..6
					const int c = n ? (int)std::trunc(*n) : 0;
					next = WithColor(cur, kLetters[k], c);
				}
				ImGui::PopID();
			}
			break;
		}
		}
		if (next) {
			s.algo.SetValue(page, i, *next);   // editing a value ticks the row
			algoChanged(idx);
		}
		if (click) {
			s.algo.picked[i] = on ? 0 : 1;
			algoChanged(idx);
		}

		// fragment: what this row puts in the string (ticked rows), wrapped whole
		float fragBottom = p.y + padY + rowH0;
		if (on) {
			const std::optional<std::string> f = e.fragment(ValueOf(s.algo.values, e), fragLang());
			const float fx = nx + c0 + colGap + c1 + colGap;
			const std::string text = f ? *f : std::string(u8"輸入不成立");
			const std::vector<std::string> lines = WrapAnywhere(SmallF(), text, std::max(Dp(40.0f), c2));
			const float lh = SmallF()->FontSize * 1.3f;
			float fy = p.y + padY + std::floor((rowH0 - SmallF()->FontSize) * 0.5f);
			for (const std::string& l : lines) {
				DrawTextAt(dl, SmallF(), ImVec2(fx, fy), f ? Tok::TextMuted : Tok::Danger, l.c_str());
				fy += lh;
			}
			fragBottom = std::max(fragBottom, fy);
			if (f && ImGui::IsMouseHoveringRect(ImVec2(fx, p.y), ImVec2(fx + c2, fragBottom))) PobUi::Tooltip(f->c_str());
		}
		const float bottom = std::max({nameBottom, fl.bottom(), fragBottom}) + padY;
		split.SetCurrentChannel(dl, 0);
		if (on) dl->AddRectFilled(p, ImVec2(p.x + width, bottom), Tok::WarningSoft, Dp(6.0f));
		else if (ImGui::IsMouseHoveringRect(p, ImVec2(p.x + width, bottom))) dl->AddRectFilled(p, ImVec2(p.x + width, bottom), Tok::Surface2, Dp(6.0f));
		split.Merge(dl);
		ImGui::SetCursorScreenPos(ImVec2(p.x, bottom + Dp(1.0f)));
		ImGui::Dummy(ImVec2(width, 0));
		ImGui::PopID();
	}

	// The rows of an algorithmic page / section; groups as .gbox when there is
	// more than one (the vendor page: 插槽與連結 / 物品屬性 / 勢力).
	void drawAlgoRows(int idx, bool boxes)
	{
		const RegexAlgo::AlgoPage& page = *refs_[idx].algo;
		const float w = ImGui::GetContentRegionAvail().x;
		for (int g = 0; g < (int)page.groups.size(); g++) {
			bool any = false;
			for (const RegexAlgo::AlgoEntry& e : page.entries) any |= (e.def.group == g);
			if (!any) continue;
			ImGui::PushID(g);
			const bool box = boxes && page.groups.size() > 1;
			GBox gb;
			if (box) gb = GBoxBegin(page.groups[g].c_str(), w);
			const float rw = box ? w - Dp(12.0f) : w;
			for (int i = 0; i < (int)page.entries.size(); i++)
				if (page.entries[i].def.group == g) drawAlgoRow(idx, i, rw);
			if (box) GBoxEnd(gb);
			ImGui::PopID();
		}
	}

	// The vendor page (RegexVendor.dc.html): the rows are the whole list.
	void drawAlgoPage(int idx)
	{
		PageState& s = pages_[idx];
		const float avail = ImGui::GetContentRegionAvail().x;
		const float cw = PobUi::ButtonWidth(u8"清除", PobUi::BtnSize::Sm);
		const float y = ImGui::GetCursorScreenPos().y;
		SmallText(u8"每個勾選各自一個條件（同時成立）；改數值會自動勾選。", Tok::TextMuted, avail - cw - Dp(10.0f));
		const float after = ImGui::GetCursorScreenPos().y;
		ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x + avail - cw, y - Dp(4.0f)));
		if (PobUi::Button(u8"清除", PobUi::BtnKind::Ghost, PobUi::BtnSize::Sm, nullptr, 0.0f, s.algo.Count() > 0)) {
			std::fill(s.algo.picked.begin(), s.algo.picked.end(), (char)0);
			algoChanged(idx);
		}
		ImGui::SetCursorScreenPos(ImVec2(ImGui::GetItemRectMin().x - (avail - cw), std::max(after, ImGui::GetItemRectMax().y) + Dp(4.0f)));
		ImGui::BeginChild("##rx_algo_rows", ImVec2(0, 0), false);
		drawAlgoRows(idx, true);
		ImGui::EndChild();
	}

	// ---- item-mod values page (R7, RegexItemModList.vue) ---------------------------

	static int GameIdx(const std::string& g) { return g == "poe2" ? 1 : 0; }
	bool isItemPage(int idx) const
	{
		return idx >= 0 && idx < (int)refs_.size() && refs_[idx].algo && RegexItemMods::IsPageId(refs_[idx].Id());
	}
	int itemPageIndex(const std::string& g) const
	{
		for (int i = 0; i < (int)refs_.size(); i++)
			if (refs_[i].Game() == g && isItemPage(i)) return i;
		return -1;
	}

	// store.ts ensureItemMods: start the background load (once; again after an error).
	void startItemMods(const std::string& g)
	{
		ItemModLoad& L = imv_[GameIdx(g)];
		if (L.phase == ItemModLoad::Phase::Ready || L.phase == ItemModLoad::Phase::Loading) return;
		if (itemPageIndex(g) < 0) return;
		if (L.worker.joinable()) L.worker.join();
		L.phase = ItemModLoad::Phase::Loading;
		L.err.clear();
		L.result.reset();
		L.done = false;
		const std::wstring dir = exeDir_;
		ItemModLoad* lp = &L;
		L.worker = std::thread([lp, dir, g]() {
			const auto t0 = std::chrono::steady_clock::now();
			std::unique_ptr<RegexItemMods::Data> d(new RegexItemMods::Data);
			std::string err;
			const bool ok = RegexItemMods::LoadFile(dir, g, *d, &err);
			lp->resultMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
			lp->resultErr = err;
			if (ok) lp->result = std::move(d);
			lp->done = true;
		});
	}

	void pollItemMods()
	{
		for (int gi = 0; gi < 2; gi++)
			if (imv_[gi].phase == ItemModLoad::Phase::Loading && imv_[gi].done) finishItemMods(gi);
	}

	// Load now and wait (a bookmark that needs the page). True when ready.
	bool ensureItemModsNow(const std::string& g)
	{
		ItemModLoad& L = imv_[GameIdx(g)];
		if (L.phase == ItemModLoad::Phase::Ready) return true;
		startItemMods(g);
		if (L.phase != ItemModLoad::Phase::Loading) return false;
		if (L.worker.joinable()) L.worker.join();
		finishItemMods(GameIdx(g));
		return L.phase == ItemModLoad::Phase::Ready;
	}

	// The worker is done: swap the entries into the page (same AlgoPage object,
	// so refs_ stays valid), size the ticks, restore this page's saved ticks.
	void finishItemMods(int gi)
	{
		ItemModLoad& L = imv_[gi];
		if (L.worker.joinable()) L.worker.join();
		const std::string g = kGames[gi];
		L.ms = L.resultMs;
		if (!L.result) {
			L.phase = ItemModLoad::Phase::Error;
			L.err = L.resultErr.empty() ? std::string(u8"未知錯誤") : L.resultErr;
			PobLog::Error("data", "regex_stats\\" + g + ": " + L.err);
			return;
		}
		const int idx = itemPageIndex(g);
		RegexAlgo::AlgoPage* page = nullptr;
		for (RegexAlgo::AlgoPage& a : algo_)
			if (a.game == g && RegexItemMods::IsPageId(a.id)) page = &a;
		if (idx < 0 || !page) {
			L.phase = ItemModLoad::Phase::Error;
			L.err = u8"清單裡沒有這一頁";
			return;
		}
		*page = RegexItemMods::MakePage(g, L.result.get());
		L.result.reset();   // the page holds what it needs (templates + anchors)
		L.count = (int)page->entries.size();
		L.groupCounts = RegexItemMods::GroupCounts(*page);
		PageState& ps = pages_[idx];
		ps.picked.assign(page->entries.size(), 0);
		ps.algo.picked.assign(page->entries.size(), 0);   // values (restored at Init) stay
		ps.dirty = true;
		ps.filterDirty = true;
		combinedDirty_ = true;
		L.phase = ItemModLoad::Phase::Ready;
		PobLog::Diag("data", "regex item-mod values " + g + ": " + std::to_string(L.count) + " entries in " +
		                         std::to_string(L.ms) + " ms");
		if (const std::optional<RegexEmbed::Applied> r = RegexEmbed::SavedPicksOf(refs_[idx], state_)) {
			setTicks(idx, r->picked);
			if (r->missed > 0)
				notice_ = u8"上次的勾選有 " + std::to_string(r->missed) + u8" 項（" + refs_[idx].Title() +
				          u8"）在目前的資料裡找不到，可能是賽季更新後詞條有變動。";
		}
	}

	void joinItemMods()
	{
		for (ItemModLoad& L : imv_)
			if (L.worker.joinable()) L.worker.join();
	}

	// RegexItemMods.dc.html: toolbar (search | category with counts | only ticked
	// | shown / matching), the rules of the page, rows grouped by category (the
	// ticked ones first, in a group of their own), at most kListCap more.
	void drawItemModPage(int idx)
	{
		const std::string g = refs_[idx].Game();
		ItemModLoad& L = imv_[GameIdx(g)];
		if (L.phase == ItemModLoad::Phase::Error) {
			if (PobUi::Banner("rx_imverr", PobUi::BannerTone::Bad, PobIcon::CircleX, u8"物品詞綴載入失敗", L.err.c_str(), false, u8"重試",
			                  false) == PobUi::BannerResult::Action && !testMode_)
				startItemMods(g);
			return;
		}
		if (L.phase != ItemModLoad::Phase::Ready) {
			if (L.phase == ItemModLoad::Phase::Idle) startItemMods(g);
			SmallText(u8"載入物品詞綴（第一次開這一頁要讀詞綴表，約1秒）…", Tok::TextMuted);
			return;
		}
		PageState& s = pages_[idx];
		const RegexAlgo::AlgoPage& page = *refs_[idx].algo;
		const float avail = ImGui::GetContentRegionAvail().x, gap = Dp(8.0f);

		std::vector<std::string> gl;
		gl.push_back(u8"全部分類 (" + std::to_string(page.entries.size()) + ")");
		for (int gi = 0; gi < (int)page.groups.size(); gi++)
			gl.push_back(page.groups[gi] + " (" + std::to_string(gi < (int)L.groupCounts.size() ? L.groupCounts[gi] : 0) + ")");
		std::vector<const char*> glp;
		for (const std::string& x : gl) glp.push_back(x.c_str());
		const float groupW = std::max(Dp(140.0f), PobUi::SelectFitWidth(glp.data(), (int)glp.size()));
		if (s.filterDirty) {
			RegexItemMods::Filter f;
			f.search = s.search;
			f.group = s.groupFilter;
			f.pickedOnly = s.pickedOnly;
			const RegexItemMods::Filtered r = RegexItemMods::FilterRows(page, s.algo.Picks(), f, RegexItemMods::kListCap);
			s.imvRows = r.rows;
			s.imvTotal = r.total;
			s.filterDirty = false;
		}
		const std::string shown = u8"顯示 " + std::to_string(s.imvRows.size()) + u8" / 符合 " + std::to_string(s.imvTotal);
		const char* onlyL = u8"只看已勾選";
		const float onlyW = Dp(21.0f) + TextSz(SmallF(), onlyL).x;
		const float shownW = TextSz(SmallF(), shown.c_str()).x;
		const float clearW = PobUi::ButtonWidth(u8"清除", PobUi::BtnSize::Sm);
		const float fixed = groupW + onlyW + shownW + clearW + gap * 4;
		const bool oneRow = avail - fixed >= Dp(160.0f);
		syncSearchBuf(s);
		if (PobUi::SearchField("##rx_imv_search", s.searchBuf, (int)sizeof s.searchBuf, u8"搜尋繁中 / 英文（空白分隔多個字）",
		                       oneRow ? avail - fixed : avail)) {
			s.search = s.searchBuf;
			s.filterDirty = true;
		}
		const float H = ImGui::GetItemRectSize().y;
		if (oneRow) ImGui::SameLine(0, gap);
		const float lineY = ImGui::GetCursorScreenPos().y;
		int sel = s.groupFilter + 1;
		if (PobUi::Select("##rx_imv_group", &sel, glp.data(), nullptr, (int)glp.size(), groupW) && sel - 1 != s.groupFilter) {
			s.groupFilter = sel - 1;
			s.filterDirty = true;
		}
		ImGui::SameLine(0, gap);
		ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, lineY + std::floor((H - Dp(15.0f)) * 0.5f)));
		if (CheckLabel("rx_imv_only", onlyL, s.pickedOnly)) {
			s.pickedOnly = !s.pickedOnly;
			s.filterDirty = true;
		}
		ImGui::SameLine(0, gap);
		ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, lineY + std::floor((H - SmallF()->FontSize) * 0.5f)));
		SmallText(shown.c_str());
		ImGui::SameLine(0, gap);
		ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, lineY + std::floor((H - Dp(28.0f)) * 0.5f)));
		if (PobUi::Button(u8"清除", PobUi::BtnKind::Ghost, PobUi::BtnSize::Sm, nullptr, 0.0f, s.algo.Count() > 0)) {
			std::fill(s.algo.picked.begin(), s.algo.picked.end(), (char)0);
			algoChanged(idx);
		}
		ImGui::SetCursorScreenPos(ImVec2(ImGui::GetWindowPos().x + ImGui::GetCursorStartPos().x - ImGui::GetScrollX(), lineY + H + Dp(6.0f)));
		SmallText(u8"只收恰好一個數值的詞綴；數值只認整數，小數詞綴不收；負值在≥條件下會被當成正數命中。"
		          u8"有些片段會加行首／行尾錨點，避開文字相同但更長的詞綴。",
		          Tok::TextMuted, avail);
		ImGui::Dummy(ImVec2(0, Dp(4.0f)));
		if (s.imvRows.empty()) {
			const std::string t = s.search.empty() ? std::string(u8"沒有符合的詞綴") : u8"找不到「" + s.search + u8"」";
			if (PobUi::EmptyState("rx_imv_none", PobIcon::Search, t.c_str(), u8"試試英文或較短的關鍵字，或換一個分類。",
			                      (!s.search.empty() || s.groupFilter >= 0 || s.pickedOnly) ? u8"清除篩選" : nullptr)) {
				s.search.clear();
				s.searchBuf[0] = 0;
				s.groupFilter = -1;
				s.pickedOnly = false;
				s.filterDirty = true;
			}
			return;
		}
		ImGui::BeginChild("##rx_imv_rows", ImVec2(0, 0), false);
		const float w = ImGui::GetContentRegionAvail().x;
		// A copy: ticking a row re-filters next frame, never under this loop.
		const std::vector<int> rows = s.imvRows;
		size_t k = 0;
		// the ticked rows (FilterRows puts them first)
		size_t nPicked = 0;
		while (nPicked < rows.size() && s.algo.picked[rows[nPicked]]) nPicked++;
		if (nPicked > 0) {
			const GBox gb = GBoxBegin(u8"已勾選", w);
			for (; k < nPicked; k++) drawAlgoRow(idx, rows[k], w - Dp(12.0f));
			GBoxEnd(gb);
		}
		while (k < rows.size()) {
			const int grp = page.entries[rows[k]].def.group;
			const GBox gb = GBoxBegin(grp >= 0 && grp < (int)page.groups.size() ? page.groups[grp].c_str() : "", w);
			ImGui::PushID((int)k);
			for (; k < rows.size() && page.entries[rows[k]].def.group == grp; k++) drawAlgoRow(idx, rows[k], w - Dp(12.0f));
			ImGui::PopID();
			GBoxEnd(gb);
		}
		if (s.imvTotal > (int)s.imvRows.size())
			SmallText((u8"還有 " + std::to_string(s.imvTotal - (int)s.imvRows.size()) + u8" 條符合，請輸入更多字或選分類縮小範圍。").c_str(),
			          Tok::TextMuted, w);
		ImGui::EndChild();
	}

	// The numeric / condition section on top of a host page (Regex.dc.html
	// "數值條件" card, RegexItemMods.dc.html "稀有度 / 汙染"): a foldable card whose
	// head says how many are set and what they cost; folded, it lists them.
	void drawSection(int host, int sec)
	{
		using namespace RegexAlgo;
		PageState& ss = pages_[sec];
		const AlgoPage& page = *refs_[sec].algo;
		const std::string hostId = refs_[host].Id();
		const bool cond = IsConditionSectionId(page.id);
		const char* title = cond ? u8"稀有度 / 汙染" : u8"數值條件";
		const bool collapsed = std::find(state_.collapsed.begin(), state_.collapsed.end(), hostId) != state_.collapsed.end();
		ImGui::PushID("rx_sec");
		PobUi::CardBegin("rx_seccard", nullptr, nullptr, nullptr, false);
		const ImVec2 c0 = ImGui::GetCursorScreenPos();
		const float cw = ImGui::GetContentRegionAvail().x;
		const float headH = Dp(38.0f), padX = Dp(12.0f);
		ImDrawList* dl = ImGui::GetWindowDrawList();
		const bool toggle = ImGui::InvisibleButton("##head", ImVec2(cw, headH));
		if (ImGui::IsItemHovered()) {
			ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
			PobUi::Tooltip(cond ? u8"物品稀有度可多選、汙染二選一（再點一次取消），各自一個條件（同時成立），與下方勾選合成同一條字串。點按鈕會自動勾選。"
			                    : u8"階級、物品數量、稀有度等屬性行的數值；每個勾選各自一個條件（同時成立），與下方詞綴合成同一條字串。"
			                      u8"寫法依社群實用格式「標籤: +N%」（半形／全形冒號、+可有可無，只比對冒號後的整個數字，不跨行）；"
			                      u8"階級比對名稱「（階級N）」。");
		}
		{
			float x = c0.x + padX;
			const float iconPx = SmallF()->FontSize;
			if (PobUi::Fonts().icons) {
				PobUi::IconAt(dl, ImVec2(x, c0.y + std::floor((headH - iconPx) * 0.5f)), collapsed ? PobIcon::ChevronRight : PobIcon::ChevronDown,
				              Tok::TextMuted, iconPx);
				x += PobUi::IconWidth(PobIcon::ChevronDown, iconPx) + Dp(8.0f);
			}
			DrawTextAt(dl, BodyF(), ImVec2(x, c0.y + std::floor((headH - BodyF()->FontSize) * 0.5f)), Tok::Text, title);
			x += TextSz(BodyF(), title).x + Dp(8.0f);
			const std::string setN = u8"已設 " + std::to_string(ss.algo.Count()) + " / " + std::to_string(page.entries.size());
			DrawTextAt(dl, SmallF(), ImVec2(x, c0.y + std::floor((headH - SmallF()->FontSize) * 0.5f)), Tok::TextMuted, setN.c_str());
			int contrib = 0;
			for (const PageContribution& c : pages_[host].combined.perPage)
				if (c.id == page.id) contrib = c.length;
			if (contrib > 0) {
				const std::string cs = std::to_string(contrib) + u8" 字";
				const float w = TextSz(SmallF(), cs.c_str()).x;
				DrawTextAt(dl, SmallF(), ImVec2(c0.x + cw - padX - w, c0.y + std::floor((headH - SmallF()->FontSize) * 0.5f)), Tok::Text, cs.c_str());
			}
			dl->AddLine(ImVec2(c0.x + 1, c0.y + headH), ImVec2(c0.x + cw - 1, c0.y + headH), Tok::BorderSubtle, 1.0f);
		}
		if (toggle) {
			// store.ts setCollapsed: remembered per host page
			if (collapsed) state_.collapsed.erase(std::remove(state_.collapsed.begin(), state_.collapsed.end(), hostId), state_.collapsed.end());
			else state_.collapsed.push_back(hostId);
			markStateDirty();
		}
		ImGui::SetCursorScreenPos(ImVec2(c0.x + padX, c0.y + headH + Dp(6.0f)));
		if (collapsed) {
			// view.ts sectionSummary: ticked rows in row order, "地圖階級 ≥16 · 物品數量 ≥80%"
			const std::vector<SummaryItem> items = SectionSummary(page, ss.algo.Picks(), ss.algo.values, RegexFrag::Lang::Zh);
			if (items.empty()) {
				SmallText(cond ? u8"沒有設定稀有度 / 汙染條件" : u8"沒有設定數值條件", Tok::TextFaint);
			} else {
				std::string ok, bad;
				for (const SummaryItem& it : items) {
					if (it.cond) ok += (ok.empty() ? "" : u8" · ") + it.label + " " + *it.cond;
					else bad += (bad.empty() ? "" : u8"、") + it.label;
				}
				if (!ok.empty()) SmallText(ok.c_str(), Tok::TextMuted, cw - padX * 2);
				if (!bad.empty()) SmallText((u8"輸入不成立：" + bad).c_str(), Tok::Danger, cw - padX * 2);
			}
			ImGui::Dummy(ImVec2(0, Dp(4.0f)));
		} else {
			const float clearW = PobUi::ButtonWidth(u8"清除", PobUi::BtnSize::Sm);
			const float y = ImGui::GetCursorScreenPos().y;
			SmallText(cond ? u8"物品稀有度可多選、汙染二選一，各自一個條件（同時成立），與下方勾選合成同一條字串；點按鈕會自動勾選。"
			               : u8"每個勾選各自一個條件（同時成立），與下方詞綴合成同一條字串；改數值會自動勾選。",
			          Tok::TextMuted, cw - padX * 2 - clearW - Dp(10.0f));
			const float after = ImGui::GetItemRectMax().y;
			ImGui::SetCursorScreenPos(ImVec2(c0.x + cw - padX - clearW, y - Dp(4.0f)));
			if (PobUi::Button(u8"清除###rx_sec_clear", PobUi::BtnKind::Ghost, PobUi::BtnSize::Sm, nullptr, 0.0f, ss.algo.Count() > 0)) {
				std::fill(ss.algo.picked.begin(), ss.algo.picked.end(), (char)0);
				algoChanged(sec);
			}
			ImGui::SetCursorScreenPos(ImVec2(c0.x + Dp(6.0f), std::max(after, ImGui::GetItemRectMax().y) + Dp(4.0f)));
			ImGui::BeginGroup();
			const float rw = cw - Dp(12.0f);
			for (int i = 0; i < (int)page.entries.size(); i++) drawAlgoRow(sec, i, rw);
			ImGui::EndGroup();
			ImGui::Dummy(ImVec2(0, Dp(4.0f)));
		}
		PobUi::CardEnd();
		ImGui::PopID();
		ImGui::Dummy(ImVec2(0, Dp(8.0f)));
	}

	// ---- multi-page merge (R4) ---------------------------------------------------
	//
	// exile-appraiser store.ts `combined` / RegexCombined.vue. Everything below
	// is per game: the merge only ever takes the selected game's pages.

	// The page's ticks as indices (corpus or algorithmic).
	std::vector<int> picksOf(int idx) const
	{
		if (refs_[idx].algo) return pages_[idx].algo.Picks();
		std::vector<int> out;
		for (int i = 0; i < (int)pages_[idx].picked.size(); i++)
			if (pages_[idx].picked[i]) out.push_back(i);
		return out;
	}

	// pages/index.ts:55 combineOrder over the selected game, as indices into
	// refs_: listed pages in order, each host followed by its section.
	// embed.ts:35 combineSels keeps only those with ticks (`pickedOnly`).
	std::vector<int> combineOrderIdx(bool pickedOnly) const
	{
		std::vector<int> out;
		for (int i = 0; i < (int)refs_.size(); i++) {
			if (refs_[i].Game() != selGame_ || refs_[i].IsSection()) continue;
			out.push_back(i);
			const int sec = sectionIndexOf(i);
			if (sec >= 0) out.push_back(sec);
		}
		if (pickedOnly)
			out.erase(std::remove_if(out.begin(), out.end(), [&](int i) { return picksOf(i).empty(); }), out.end());
		return out;
	}

	// store.ts pagePickCount: a page's ticks plus its section's.
	int pagePickCount(int idx) const
	{
		int n = (int)picksOf(idx).size();
		const int sec = sectionIndexOf(idx);
		if (sec >= 0) n += pages_[sec].algo.Count();
		return n;
	}

	// The page's corpus in the output language, built on first use (the merge
	// needs every ticked page's, not just the one on screen).
	void ensureCorpus(int idx)
	{
		PageState& ps = pages_[idx];
		if (!refs_[idx].corpus || ps.corpusReady) return;
		RegexAlgo::BuildPageCorpus(*refs_[idx].corpus, fragLang(), ps.corpus);
		ps.corpusReady = true;
		ps.dirty = true;
	}

	// store.ts `combined`: every page of the game with ticks + custom + excludes.
	// R10: the ticked pages the merge takes -- those of the current page's item
	// group (RegexAlgo::PlanMerge); `skipped` = the other pages with ticks.
	std::vector<int> mergeIdx(int* skipped = nullptr) const
	{
		const std::vector<int> all = combineOrderIdx(true);
		std::vector<std::string> ids;
		for (int i : all) ids.push_back(refs_[i].Id());
		const RegexAlgo::MergePlan plan = RegexAlgo::PlanMerge(hasPage() ? refs_[page_].Id() : std::string(), ids);
		if (skipped) *skipped = plan.skippedPages;
		std::vector<int> out;
		for (int i : all)
			if (std::find(plan.merged.begin(), plan.merged.end(), refs_[i].Id()) != plan.merged.end()) out.push_back(i);
		return out;
	}

	const RegexAlgo::CombineResult& combinedAll()
	{
		if (!combinedDirty_) return combined_;
		std::vector<RegexAlgo::CombineSel> sels;
		const std::vector<int> merged = mergeIdx(&mergeSkipped_);
		for (int i : merged) {
			// R10: a ticked section whose host has no ticks still brings the host's
			// corpus (no picks): the condition terms are shortened against it.
			if (refs_[i].IsSection()) {
				const int h = hostIndexOf(i);
				if (h != i && refs_[h].corpus && std::find(merged.begin(), merged.end(), h) == merged.end()) {
					ensureCorpus(h);
					RegexAlgo::CombineSel host;
					host.page = refs_[h];
					host.corpus = &pages_[h].corpus;
					sels.push_back(std::move(host));
				}
			}
			RegexAlgo::CombineSel sel;
			sel.page = refs_[i];
			sel.picks = picksOf(i);
			if (refs_[i].algo) {
				sel.values = &pages_[i].algo.values;
			} else {
				ensureCorpus(i);
				sel.corpus = &pages_[i].corpus;
			}
			sels.push_back(std::move(sel));
		}
		combined_ = RegexAlgo::Combine(fragLang(), mode_, sels, state_.custom, state_.excludes, &unions_);
		combinedDirty_ = false;
		return combined_;
	}

	// store.ts setPanelView; remembered (panelView, a PobTools-only field).
	void setView(View v)
	{
		if (v == view_) return;
		view_ = v;
		state_.panelView = v == View::Combined ? "combined" : "page";
		markStateDirty();
	}

	void setScope(bool combined)
	{
		if (combined == scopeCombined_) return;
		scopeCombined_ = combined;
		state_.outScope = combined ? "combined" : "page";
		markStateDirty();
		copied_ = false;
	}

	// A page title within the selected game (gem_names / vendor_bases exist in both).
	std::string pageTitleInGame(const std::string& id) const
	{
		for (const RegexAlgo::PageRef& p : refs_)
			if (p.Id() == id && p.Game() == selGame_) return p.Title();
		return pageTitleById(id);
	}

	// pages/index.ts:45 hostIdOf, as an index: a section -> its host page.
	int hostIndexOf(int idx) const
	{
		if (!refs_[idx].IsSection()) return idx;
		for (int i = 0; i < (int)refs_.size(); i++)
			if (!refs_[i].IsSection() && refs_[i].Id() == refs_[idx].algo->sectionOf && refs_[i].Game() == refs_[idx].Game())
				return i;
		return idx;
	}

	// store.ts clearAllPicks: every page of the game, values kept.
	void clearAllPicks()
	{
		for (int i : combineOrderIdx(true)) {
			PageState& ps = pages_[i];
			if (refs_[i].algo) std::fill(ps.algo.picked.begin(), ps.algo.picked.end(), (char)0);
			else std::fill(ps.picked.begin(), ps.picked.end(), (char)0);
			syncCurrent(i);
			ps.dirty = true;
			ps.filterDirty = true;
		}
		// A host's single-page output carries its section.
		for (PageState& ps : pages_) ps.dirty = true;
		combinedDirty_ = true;
		copied_ = false;
		notice_ = u8"已清除這個遊戲所有清單的勾選（數值條件的數值保留）。";
	}

	// store.ts addCustom / removeCustom: trimmed, no duplicates.
	bool addChip(std::vector<std::string>& list, std::string& draft)
	{
		const std::string t = RegexAlgo::JsTrim(draft);
		if (t.empty() || std::find(list.begin(), list.end(), t) != list.end()) return false;
		list.push_back(t);
		draft.clear();
		markStateDirty();
		combinedDirty_ = true;
		copied_ = false;
		return true;
	}

	// RegexLimit.dc.html 自訂文字 / 排除詞 cards: the chips (.chipw, × removes) and
	// an input + 加入 (Enter adds).
	void drawChips(const char* id, const char* title, const char* hint, const char* placeholder,
	               std::vector<std::string>& list, std::string& draft)
	{
		ImGui::PushID(id);
		PobUi::CardBegin("chipcard", nullptr, nullptr, nullptr, true);
		const float inner = PobUi::CardInnerWidth();
		const float x0 = ImGui::GetCursorScreenPos().x;
		ImGui::TextUnformatted(title);
		SmallText(hint, Tok::TextMuted, inner);
		int remove = -1;
		if (!list.empty()) {
			ImGui::Dummy(ImVec2(0, Dp(2.0f)));
			float x = x0, y = ImGui::GetCursorScreenPos().y;
			const float h = Dp(24.0f), gap = Dp(6.0f);
			ImDrawList* dl = ImGui::GetWindowDrawList();
			for (int i = 0; i < (int)list.size(); i++) {
				ImGui::PushID(i);
				const ImVec2 ts = TextSz(SmallF(), list[i].c_str());
				const float xw = TextSz(SmallF(), u8"×").x;
				const float w = Dp(10.0f) + ts.x + Dp(6.0f) + xw + Dp(8.0f);
				if (x > x0 && x + w > x0 + inner) {
					x = x0;
					y += h + gap;
				}
				dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), Tok::Surface3, h * 0.5f);
				DrawTextAt(dl, SmallF(), ImVec2(x + Dp(10.0f), y + std::floor((h - ts.y) * 0.5f)), Tok::Text, list[i].c_str());
				ImGui::SetCursorScreenPos(ImVec2(x + Dp(10.0f) + ts.x + Dp(2.0f), y));
				if (ImGui::InvisibleButton("##x", ImVec2(xw + Dp(10.0f), h))) remove = i;
				const bool hov = ImGui::IsItemHovered();
				DrawTextAt(dl, SmallF(), ImVec2(x + Dp(10.0f) + ts.x + Dp(6.0f), y + std::floor((h - ts.y) * 0.5f)),
				           hov ? Tok::Danger : Tok::TextFaint, u8"×");
				if (hov) PobUi::Tooltip(u8"移除");
				x += w + gap;
				ImGui::PopID();
			}
			ImGui::SetCursorScreenPos(ImVec2(x0, y + h + Dp(6.0f)));
		}
		if (remove >= 0) {
			list.erase(list.begin() + remove);
			markStateDirty();
			combinedDirty_ = true;
			copied_ = false;
		}
		const float addW = PobUi::ButtonWidth(u8"加入", PobUi::BtnSize::Sm);
		PobUi::PushControlFrame();
		ImGui::SetNextItemWidth(inner - addW - Dp(6.0f));
		const bool entered = ImGui::InputTextWithHint("##draft", placeholder, &draft, ImGuiInputTextFlags_EnterReturnsTrue);
		PobUi::PopControlFrame();
		const float ih = ImGui::GetItemRectSize().y;
		ImGui::SameLine(0, Dp(6.0f));
		ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, ImGui::GetItemRectMin().y + std::floor((ih - Dp(28.0f)) * 0.5f)));
		const bool clicked = PobUi::Button(u8"加入", PobUi::BtnKind::Secondary, PobUi::BtnSize::Sm, nullptr, 0.0f, !RegexAlgo::JsTrim(draft).empty());
		if ((entered || clicked) && addChip(list, draft) && entered) ImGui::SetKeyboardFocusHere(-1);
		PobUi::CardEnd();
		ImGui::PopID();
		ImGui::Dummy(ImVec2(0, Dp(8.0f)));
	}

	// combine.ts:46 conflict kinds, worded as RegexCombined.vue's i18n.
	std::string conflictText(const RegexAlgo::Conflict& c) const
	{
		using RegexAlgo::ConflictKind;
		const std::string page = c.page.empty() ? std::string() : contributionName(c.page);
		switch (c.kind) {
		case ConflictKind::Extra: return page + u8"：也會選到未勾選的「" + c.text + u8"」";
		case ConflictKind::Missing: return page + u8"：「" + c.text + u8"」沒被選到";
		case ConflictKind::Ambient: return u8"片段「" + c.text + u8"」會中每件物品都有的文字";
		case ConflictKind::Fragment: return page + u8"：條件片段會誤中詞綴行（" + c.text + u8"）";
		case ConflictKind::Exclude: return page + u8"：排除詞與已勾選的詞綴衝突（" + c.text + u8"）";
		case ConflictKind::Invalid: return page + u8"：「" + c.text + u8"」的輸入不成立，已略過";
		// R10 (en: "rarity / corruption conditions contradict each other (...); no string was made")
		case ConflictKind::ConditionClash: return page + u8"：稀有度 / 汙染條件互相矛盾（" + c.text + u8"），無法合成字串";
		}
		return c.text;
	}

	// RegexLimit.dc.html "已選（合併）": which pages take part and what each
	// costs, the custom / exclude cards, and the merge conflicts.
	void drawCombinedView()
	{
		using namespace RegexAlgo;
		const CombineResult& r = combinedAll();
		const std::vector<int> picked = mergeIdx();
		{
			const float avail = ImGui::GetContentRegionAvail().x;
			const float y = ImGui::GetCursorScreenPos().y, x = ImGui::GetCursorScreenPos().x;
			const float cw = PobUi::ButtonWidth(u8"全部清除", PobUi::BtnSize::Sm);
			ImGui::SetCursorScreenPos(ImVec2(x, y + std::floor((Dp(28.0f) - BodyF()->FontSize) * 0.5f)));
			ImGui::TextUnformatted((std::string(u8"已選（合併）· ") + GameLabel(selGame_)).c_str());
			ImGui::SetCursorScreenPos(ImVec2(x + avail - cw, y));
			if (PobUi::Button(u8"全部清除", PobUi::BtnKind::Ghost, PobUi::BtnSize::Sm, nullptr, 0.0f, !combineOrderIdx(true).empty())) clearAllPicks();
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
				PobUi::Tooltip(u8"取消這個遊戲所有清單（含數值條件）的勾選；自訂文字與排除詞保留");
			ImGui::SetCursorScreenPos(ImVec2(x, y + Dp(28.0f) + Dp(8.0f)));
		}
		ImGui::BeginChild("##rx_comb", ImVec2(0, 0), false);
		if (picked.empty() && r.custom.empty() && r.excludes.empty()) {
			if (mergeSkipped_ > 0)   // R10 (en: "No picks for this kind of item")
				PobUi::EmptyState("rx_comb_empty", PobIcon::List, u8"這類物品還沒有勾選",
				                  (u8"另有 " + std::to_string(mergeSkipped_) + u8" 頁的勾選屬於其他物品，未併入；切到那一頁就會合成那一類。").c_str());
			else
				PobUi::EmptyState("rx_comb_empty", PobIcon::List, u8"還沒有勾選任何清單", u8"切到「單頁清單」勾選，勾好的清單會在這裡合成一串。");
			ImGui::Dummy(ImVec2(0, Dp(8.0f)));
		} else {
			// .pt-table: name (a link to the page) + badge | ticked | cost | not single-able
			const float w = ImGui::GetContentRegionAvail().x;
			const float colW[4] = {w * 0.46f, w * 0.14f, w * 0.18f, w * 0.22f};
			const char* heads[4] = {u8"清單", u8"勾選", u8"貢獻長度", u8"無法單獨指定"};
			const float rowH = Dp(34.0f);
			ImDrawList* dl = ImGui::GetWindowDrawList();
			ImVec2 p = ImGui::GetCursorScreenPos();
			auto cellRight = [&](int c, float y, const std::string& t, ImU32 col) {
				float cx = p.x;
				for (int k = 0; k <= c; k++) cx += colW[k];
				const float tw = TextSz(BodyF(), t.c_str()).x;
				DrawTextAt(dl, BodyF(), ImVec2(cx - Dp(10.0f) - tw, y + std::floor((rowH - BodyF()->FontSize) * 0.5f)), col, t.c_str());
			};
			{
				float cx = p.x;
				for (int c = 0; c < 4; c++) {
					const float tw = TextSz(SmallF(), heads[c]).x;
					const float tx = c == 0 ? cx + Dp(10.0f) : cx + colW[c] - Dp(10.0f) - tw;
					DrawTextAt(dl, SmallF(), ImVec2(tx, p.y + std::floor((rowH - SmallF()->FontSize) * 0.5f)), Tok::TextMuted, heads[c]);
					cx += colW[c];
				}
				dl->AddLine(ImVec2(p.x, p.y + rowH), ImVec2(p.x + w, p.y + rowH), Tok::Border, 1.0f);
			}
			float y = p.y + rowH;
			int jump = -1;
			for (int i : picked) {
				const PageContribution* c = nullptr;
				for (const PageContribution& x : r.perPage)
					if (x.id == refs_[i].Id()) c = &x;
				ImGui::PushID(i);
				const int hostIdx = hostIndexOf(i);
				const std::string name = refs_[hostIdx].Title();
				ImGui::SetCursorScreenPos(ImVec2(p.x + Dp(10.0f), y + std::floor((rowH - BodyF()->FontSize) * 0.5f)));
				if (PobUi::Link(name.c_str(), Tok::AccentText)) jump = hostIdx;
				if (ImGui::IsItemHovered()) PobUi::Tooltip(u8"到這份清單");
				if (refs_[i].algo && (refs_[i].IsSection() || refs_[i].algo->kind == RegexPageKind::Numeric ||
				                      refs_[i].algo->kind == RegexPageKind::Sockets)) {
					const char* badge = refs_[i].IsSection() ? (IsConditionSectionId(refs_[i].Id()) ? u8"條件" : u8"數值")
					                                         : (isItemPage(i) ? u8"數值" : u8"條件");
					ImGui::SameLine(0, Dp(6.0f));
					ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, y + std::floor((rowH - SmallF()->FontSize - Dp(2.0f)) * 0.5f)));
					Pill(badge, Tok::TextMuted, Tok::Surface3);
				}
				ImGui::PopID();
				cellRight(1, y, std::to_string(picksOf(i).size()), Tok::Text);
				cellRight(2, y, std::to_string(c ? c->length : 0), Tok::Text);
				const int un = c ? c->unresolved : 0;
				cellRight(3, y, std::to_string(un), un > 0 ? Tok::Warning : Tok::Text);
				dl->AddLine(ImVec2(p.x, y + rowH), ImVec2(p.x + w, y + rowH), Tok::BorderSubtle, 1.0f);
				y += rowH;
			}
			auto extraRow = [&](const char* name, int n, int len) {
				DrawTextAt(dl, BodyF(), ImVec2(p.x + Dp(10.0f), y + std::floor((rowH - BodyF()->FontSize) * 0.5f)), Tok::Text, name);
				cellRight(1, y, std::to_string(n), Tok::Text);
				cellRight(2, y, std::to_string(len), Tok::Text);
				cellRight(3, y, u8"—", Tok::TextFaint);
				dl->AddLine(ImVec2(p.x, y + rowH), ImVec2(p.x + w, y + rowH), Tok::BorderSubtle, 1.0f);
				y += rowH;
			};
			if (!r.custom.empty()) extraRow(u8"自訂文字", (int)r.custom.size(), r.customLength);
			if (!r.excludes.empty()) extraRow(u8"排除詞", (int)r.excludes.size(), r.excludesLength);
			ImGui::SetCursorScreenPos(ImVec2(p.x, y + Dp(12.0f)));
			ImGui::Dummy(ImVec2(w, 0));
			if (jump >= 0) {
				switchPage(jump);
				setView(View::Page);
			}
		}

		drawChips("rx_custom", u8"自訂文字", u8"每項各自一個條件（同時成立），原樣比對", u8"輸入文字後按 Enter", state_.custom, customDraft_);
		drawChips("rx_excludes", u8"排除詞", u8"併進唯一的排除條件（!）：有其中任一個就不選", u8"例如：反射", state_.excludes, excludeDraft_);

		if (!r.conflicts.empty()) {
			PobUi::CardBegin("rx_conf", nullptr, nullptr, nullptr, true);
			const float inner = PobUi::CardInnerWidth();
			const std::string head = std::to_string(r.conflicts.size()) + u8" 個合併衝突";
			const ImVec2 hp = ImGui::GetCursorScreenPos();
			if (ImGui::InvisibleButton("##confhead", ImVec2(inner, BodyF()->FontSize + Dp(4.0f)))) conflictsOpen_ = !conflictsOpen_;
			ImDrawList* dl = ImGui::GetWindowDrawList();
			float x = hp.x;
			if (PobUi::Fonts().icons) {
				PobUi::IconAt(dl, ImVec2(x, hp.y + Dp(2.0f)), conflictsOpen_ ? PobIcon::ChevronDown : PobIcon::ChevronRight, Tok::Warning,
				              BodyF()->FontSize);
				x += PobUi::IconWidth(PobIcon::ChevronDown, BodyF()->FontSize) + Dp(6.0f);
			}
			DrawTextAt(dl, BodyF(), ImVec2(x, hp.y + Dp(2.0f)), Tok::Warning, head.c_str());
			if (conflictsOpen_) {
				// merging unrelated pages can report thousands of `extra` lines;
				// the first few hundred say everything the player can act on
				const size_t shown = std::min<size_t>(r.conflicts.size(), 300);
				for (size_t k = 0; k < shown; k++) WrappedBlock(SmallF(), conflictText(r.conflicts[k]), inner, Tok::Text);
				if (r.conflicts.size() > shown)
					SmallText((u8"另有 " + std::to_string(r.conflicts.size() - shown) + u8" 個未列出").c_str(), Tok::TextFaint);
			}
			PobUi::CardEnd();
		}
		ImGui::EndChild();
	}

	// ---- POBTOOLS_REGEX_STATE (test aid, with POBTOOLS_TOOL_SHOT) ------------------
	//
	// A known state for each design draft, built in memory: nothing is read from
	// or written to regex_ui.json, the clipboard is never touched, nothing is sent.
	//   single | combined | limitwarn | limitbad | vendor | itemmods | imvloading |
	//   imverror | copied | more | paste | sharefail | applied | bookmarks | bmempty |
	//   bmmenu | save | rename | delete | folderdel | pagemenu | nosearch | dataerr |
	//   sendpick | import | en
	int entryIndex(int idx, const std::string& id) const
	{
		if (refs_[idx].algo) {
			for (int i = 0; i < (int)refs_[idx].algo->entries.size(); i++)
				if (refs_[idx].algo->entries[i].def.id == id) return i;
		} else {
			for (int i = 0; i < (int)refs_[idx].corpus->entries.size(); i++)
				if (refs_[idx].corpus->entries[i].id == id) return i;
		}
		return -1;
	}
	// the first corpus entries whose English line contains each needle (or the first rows)
	std::vector<int> findEntries(int idx, const std::vector<std::string>& needles) const
	{
		std::vector<int> out;
		const std::vector<RegexEntryDef>& es = refs_[idx].corpus->entries;
		for (const std::string& n : needles)
			for (int i = 0; i < (int)es.size(); i++)
				if (!es[i].en.empty() && es[i].en[0].find(n) != std::string::npos &&
				    std::find(out.begin(), out.end(), i) == out.end()) {
					out.push_back(i);
					break;
				}
		for (int i = 0; out.size() < needles.size() && i < (int)es.size(); i++)
			if (std::find(out.begin(), out.end(), i) == out.end()) out.push_back(i);
		std::sort(out.begin(), out.end());
		return out;
	}
	void testValue(int idx, const std::string& id, const RegexFrag::AlgoValue& v)
	{
		const int i = entryIndex(idx, id);
		if (i < 0) return;
		pages_[idx].algo.SetValue(*refs_[idx].algo, i, v);
		algoChanged(idx);
	}
	static RegexFrag::AlgoValue TV(std::optional<double> mn, std::optional<double> mx = std::nullopt, const char* choice = nullptr)
	{
		RegexFrag::AlgoValue v;
		v.min = mn;
		v.max = mx;
		if (choice) {
			v.choice = choice;
			v.hasChoice = true;
		}
		return v;
	}
	void testTicks(int idx, const std::vector<int>& picks)
	{
		if (idx < 0) return;
		setTicks(idx, picks);
		syncCurrent(idx);
		pages_[idx].dirty = true;
		combinedDirty_ = true;
	}
	// The map page with two modifiers, three numeric conditions and one custom term (Regex.dc.html).
	void testSingle()
	{
		switchGame("poe1");
		const int mm = indexInGame("poe1", "map_mods");
		if (mm < 0) return;
		switchPage(mm);
		testTicks(mm, findEntries(mm, {"maximum Player Resistances", "reflect"}));
		const int sec = sectionIndexOf(mm);
		if (sec >= 0) {
			testValue(sec, "tier", TV(16));
			testValue(sec, "quantity", TV(80));
			testValue(sec, "item_rarity_class", TV(std::nullopt, std::nullopt, "mr|u"));
		}
		state_.custom = {u8"6 連結"};
		setView(View::Page);
		setScope(true);
	}
	void testBookmarks()
	{
		auto add = [&](const char* name, const char* game, const char* page, const char* mode, int n, const char* folder) {
			RegexBookmark b;
			b.name = name;
			b.game = game;
			b.page = page;
			b.mode = mode;
			b.lang = "zh";
			const int idx = indexInGame(game, page);
			for (int i = 0; i < n; i++) {
				if (idx >= 0 && refs_[idx].corpus && i < (int)refs_[idx].corpus->entries.size()) {
					b.keys.push_back(KeyOf(refs_[idx].corpus->entries[i]));
					b.alt.push_back(ZhLine(refs_[idx].corpus->entries[i]));
				} else {
					b.keys.push_back("Old line " + std::to_string(i));
					b.alt.push_back(u8"舊詞綴 " + std::to_string(i));
				}
			}
			b.folder = folder;
			state_.bookmarks.push_back(b);
		};
		RegexFolders::Add(state_, "poe1", u8"T16 刷圖");
		add(u8"T16 不能刷的詞綴", "poe1", "map_mods", "none", 7, u8"T16 刷圖");
		add(u8"T16 刷圖", "poe1", "map_mods", "any", 5, u8"T16 刷圖");
		add(u8"商店 6L 紅藍綠", "poe1", "vendor_items", "any", 0, "");
		add(u8"3.25 劫盜契約書", "poe1", "heist_contract_mods_325", "any", 6, "");
		add(u8"換界石 T15", "poe2", "waystone_mods", "any", 3, "");
		RegexFolders::Normalize(state_);
	}
	void applyTestState()
	{
		const std::string& t = testState_;
		if (t.empty()) return;
		if (t == "dataerr") return;
		if (t == "single" || t == "copied" || t == "more" || t == "save" || t == "en") {
			testSingle();
			if (t == "copied") {
				shareCopied_ = 1;
				shareCopiedAt_ = std::chrono::steady_clock::now() + std::chrono::hours(1);
			}
			if (t == "more") testOpenMore_ = true;
			if (t == "save") openSave(pickCount() + sectionPickCount());
			if (t == "en") setLang(Lang::En);
			testBookmarks();
		} else if (t == "combined") {
			testSingle();
			const int sc = indexInGame("poe1", "scarabs");
			if (sc >= 0) testTicks(sc, {0, 3, 7, 12});
			state_.excludes = {u8"反射"};
			setView(View::Combined);
			testBookmarks();
		} else if (t == "limitwarn" || t == "limitbad") {
			switchGame("poe1");
			const int mm = indexInGame("poe1", "map_mods");
			switchPage(mm);
			mode_ = RegexGen::Mode::All;
			const int target = t == "limitwarn" ? 205 : 262;
			std::vector<int> picks;
			for (int i = 0; i < (int)refs_[mm].Size(); i++) {
				picks.push_back(i);
				testTicks(mm, picks);
				if (combinedAll().length >= target) break;
			}
			setView(View::Combined);
		} else if (t == "vendor") {
			switchGame("poe1");
			const int v = indexInGame("poe1", "vendor_items");
			if (v >= 0) {
				switchPage(v);
				testValue(v, "links", TV(std::nullopt, std::nullopt, "6"));
				testValue(v, "link_colors", TV(std::nullopt, std::nullopt, "rgb"));
				testValue(v, "corrupted", TV(std::nullopt, std::nullopt, "|c"));
			}
			setScope(false);
			setView(View::Page);
			testBookmarks();
		} else if (t == "itemmods" || t == "imvloading" || t == "imverror") {
			switchGame("poe1");
			const int ip = itemPageIndex("poe1");
			if (ip < 0) return;
			const int sec = sectionIndexOf(ip);
			if (sec >= 0) testValue(sec, "item_rarity_class", TV(std::nullopt, std::nullopt, "r"));
			setScope(false);
			setView(View::Page);
			if (t == "imvloading" || t == "imverror") {
				page_ = ip;
				ItemModLoad& L = imv_[0];
				L.phase = t == "imverror" ? ItemModLoad::Phase::Error : ItemModLoad::Phase::Loading;
				L.err = u8"找不到 stats 資料（Data\\regex_stats\\poe1\\cmn-Hant\\stats.ndjson.gz）";
				return;
			}
			switchPage(ip);
			if (!ensureItemModsNow("poe1")) return;
			const RegexAlgo::AlgoPage& page = *refs_[ip].algo;
			for (int i = 0; i < (int)page.entries.size(); i++) {
				const std::string& en = page.entries[i].def.en.empty() ? std::string() : page.entries[i].def.en[0];
				if (en == "+# to maximum Life") testValue(ip, page.entries[i].def.id, TV(80));
				if (en == "#% increased maximum Life") testValue(ip, page.entries[i].def.id, TV(8));
			}
			PageState& s = pages_[ip];
			s.search = u8"生命";
			s.groupFilter = 0;   // 生命
			s.filterDirty = true;
		} else if (t == "paste" || t == "sharefail") {
			testSingle();
			std::string code;
			buildShareCode(code);
			modal_ = Modal::Paste;
			if (t == "paste") {
				pasteBuf_ = code;
				pasteAuto_ = true;
			} else {
				pasteBuf_ = "H4sI@@AAAAA6tWKkpVslIqTi0uSs1V";
				RegexShare::Normalized d;
				std::string err;
				RegexShare::Decode(pasteBuf_, d, &err);
				pasteErr_ = u8"分享碼無法套用：" + err;
			}
		} else if (t == "applied") {
			testSingle();
			RegexShare::State s;
			s.game = "poe1";
			s.mode = "any";
			const int mm = indexInGame("poe1", "map_mods");
			s.pages.push_back({"map_mods", {KeyOf(refs_[mm].corpus->entries[2]), "No such modifier line"}});
			s.pages.push_back({"heist_contract_mods", {"a", "b"}});
			std::string err;
			applyCombo(s, u8"分享碼（PoE1）", {}, &err);
			testBookmarks();
		} else if (t == "bookmarks" || t == "bmmenu" || t == "rename" || t == "delete" || t == "folderdel" || t == "sendpick" ||
		           t == "import") {
			testSingle();
			testBookmarks();
			if (t == "bmmenu") testOpenBmMenu_ = 0;
			if (t == "rename") {
				editIdx_ = 0;
				nameBuf_ = u8"T16 刷圖";
				modal_ = Modal::Rename;
			}
			if (t == "delete") {
				editIdx_ = 0;
				modal_ = Modal::Delete;
			}
			if (t == "folderdel") {
				folderEdit_ = u8"T16 刷圖";
				modal_ = Modal::FolderDelete;
			}
			if (t == "sendpick") {
				openSendPick();
				RegexBookmarksShare::SetGroup(state_, sendSel_, "poe1", u8"T16 刷圖", true);
			}
			if (t == "import") {
				RegexBookmarksShare::Selection all;
				RegexBookmarksShare::SetAll(state_, all, true);
				const std::string code = RegexBookmarksShare::Encode(RegexBookmarksShare::PackOf(state_, all, nullptr));
				openImport();
				importBuf_ = code;
			}
		} else if (t == "bmempty") {
			testSingle();
		} else if (t == "pagemenu") {
			switchGame("poe2");
			setView(View::Page);
			PobUi::TestOpenSelect("##rxpage");
		} else if (t == "nosearch") {
			testSingle();
			PageState& s = st();
			s.search = u8"腐化的屍體";
			s.filterDirty = true;
		}
		for (PageState& ps : pages_) ps.dirty = true;
		combinedDirty_ = true;
	}

	const ToolPanelHost* host_ = nullptr;
	std::wstring exeDir_, game_;
	RegexDataset data_;
	// Algorithmic pages of both games, built once from the data's labels, and
	// every page (corpus first, same index as data_.Pages()) as one list.
	std::vector<RegexAlgo::AlgoPage> algo_;
	std::vector<RegexAlgo::PageRef> refs_;
	// R4 merge. Custom text / excludes live in state_ (saved); the view and
	// scope are mirrored there (panelView / outScope).
	View view_ = View::Page;           // store.ts panelView, default the page list
	bool scopeCombined_ = true;        // state.ts outScope, default 'combined'
	std::string customDraft_, excludeDraft_;
	RegexAlgo::CombineResult combined_;
	bool combinedDirty_ = true;
	int mergeSkipped_ = 0;   // R10: ticked pages of other item groups, set by combinedAll()
	RegexAlgo::UnionCorpusCache unions_;
	bool dataOk_ = false;
	std::string dataErr_;
	std::string selGame_ = "poe1";   // which game's lists are showing

	RegexUiState state_;
	bool stateDirty_ = false;
	// Set when a save failed; cleared by the next real change. Without it the
	// deferred pass retries a doomed write on every single frame.
	bool saveFailed_ = false;

	std::vector<PageState> pages_;
	int page_ = 0;
	RegexGen::Mode mode_ = RegexGen::Mode::Any;
	Lang lang_ = Lang::Zh;
	bool bilingual_ = true;

	Modal modal_ = Modal::None;
	bool renameMode_ = false;
	int editIdx_ = -1;
	// R6 bookmark list: which game's tab, the game it last followed, the
	// queued drag / button edit, and the folder modals' target and error.
	std::string bmTab_ = "poe1", bmTabFollow_ = "poe1";
	BmAction bmAction_;
	bool folderRenameMode_ = false;
	std::string folderEdit_, folderErr_;
	std::string nameBuf_;
	std::string notice_;

	std::string copyRequest_;
	bool copied_ = false;
	// R8: share code on its way to the clipboard, and how that went (1 ok, 2 failed).
	std::string shareCopyRequest_;
	std::string shareCopiedWhat_ = u8"分享碼";   // what the "已複製…" note names (分享碼 / 書籤包)
	int shareCopied_ = 0;
	std::chrono::steady_clock::time_point shareCopiedAt_;
	// 送到 ExileAppraiser: where it is (refreshed on hover / before each send),
	// requests from Frame() for RunDeferred, the last result, this session's temp files.
	RegexSend::Located sendLoc_;
	std::chrono::steady_clock::time_point sendLocAt_;
	bool relocateWanted_ = false, pickRequest_ = false, sendRequest_ = false;
	std::string sendCode_;
	// What sendCode_ is: the panel sends bookmark packs only (the share-code kind
	// stays in regex_send for the logic and its self-test; "複製分享碼" hands over ticks).
	RegexSend::Kind sendKind_ = RegexSend::Kind::Bookmarks;
	std::string sendWhat_;                 // "3 筆書籤、1 個資料夾", for the result line
	RegexBookmarksShare::Selection sendSel_;   // the "選擇要傳送的書籤" dialog
	std::string sendPickTab_ = "poe1";
	bool sendPickTabSet_ = false;
	// 匯入書籤包: the pasted text, what was last parsed, and its result.
	std::string importBuf_, importFor_, importErr_;
	bool importOk_ = false;
	RegexBookmarksShare::Normalized importPack_;
	std::string sendMsg_;
	bool sendOk_ = false;
	std::chrono::steady_clock::time_point sendMsgAt_;
	std::vector<std::wstring> sendFiles_;
	std::vector<RegexShare::Template> templates_;
	std::string templatesErr_;
	int tplPending_ = -1;                 // the template the confirm dialog is about
	std::string pasteBuf_, pasteErr_;     // the paste dialog
	ItemModLoad imv_[2];   // R7, per game (kGames order)
	std::string noticeDesc_;      // the notice banner's explanation line
	bool noticeWarn_ = false;     // warning tone (something did not come through)
	bool pasteAuto_ = false;      // the paste dialog was filled from the clipboard
	std::string nameErr_;
	bool conflictsOpen_ = true;
	bool openDataDir_ = false;    // "開啟資料夾" on the data error (RunDeferred)
	// POBTOOLS_REGEX_STATE / POBTOOLS_TOOL_SHOT: a fixed state, nothing read or written
	bool testMode_ = false, testApplied_ = false, testOpenMore_ = false;
	int testOpenBmMenu_ = -1;
	std::string testState_;
	int t17Cache_ = -1;
	bool t17Present_ = false;
	ToolCloseState close_ = ToolCloseState::Open;
};

} // namespace

IToolPanel* CreateRegexToolPanel()
{
	return new RegexToolPanel();
}

void ShowRegexTool(const std::wstring& exeDir, const std::wstring& game,
                   const std::wstring& locale)
{
	RegexToolPanel panel;
	ToolWindowDesc desc;
	// "PobTools — Poe Regex"
	desc.titleUtf8 = "PobTools \xe2\x80\x94 Poe Regex";
	desc.defW = 1280;   // the design's window (Regex.dc.html)
	desc.defH = 860;
	RunToolWindow(panel, desc, exeDir, game, locale);
}
