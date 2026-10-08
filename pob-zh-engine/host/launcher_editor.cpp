#include "launcher_editor.h"
#include "error_log.h"
#include "tool_panel.h"
#include "tool_window.h"
#include "editor_data.h"
#include "trans_editor_logic.h"
#include "launcher_config.h" // ResolveConfiguredFontPath
#include "ui_theme.h"
#include "ui_widgets.h"
#include "ui_icons.h"

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>   // ShellExecuteW: open the dictionary folder

#include <GLES2/gl2.h>
#include <GLFW/glfw3.h>
#define IMGUI_DEFINE_MATH_OPERATORS
#include <imgui.h>
#include <imgui_internal.h>   // ClearActiveID (Ctrl+Enter in the multi-line box), OpenPopupEx
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace Tok = PobUi::Tok;
using PobUi::D;

// ---- helpers ---------------------------------------------------------------

static std::string narrow(const std::wstring& w)
{
	if (w.empty()) return std::string();
	int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
	std::string s(n, '\0');
	WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
	return s;
}

// Case-insensitive (ASCII) substring test. needleLower must already be lower.
static bool contains_ci(const std::string& hay, const std::string& needleLower)
{
	if (needleLower.empty()) return true;
	const size_t n = needleLower.size();
	if (hay.size() < n) return false;
	for (size_t i = 0; i + n <= hay.size(); i++) {
		size_t j = 0;
		for (; j < n; j++) {
			char c = hay[i + j];
			if (c >= 'A' && c <= 'Z') c += 32;
			if (c != needleLower[j]) break;
		}
		if (j == n) return true;
	}
	return false;
}

static std::string to_lower_ascii(const std::string& s)
{
	std::string r = s;
	for (char& c : r) if (c >= 'A' && c <= 'Z') c += 32;
	return r;
}

// 41206 -> "41,206"
static std::string group_digits(long long v)
{
	std::string s = std::to_string(v < 0 ? -v : v);
	for (int i = (int)s.size() - 3; i > 0; i -= 3) s.insert((size_t)i, ",");
	return v < 0 ? "-" + s : s;
}

static std::string stem(const std::string& fileName)
{
	const size_t dot = fileName.rfind(".json");
	return dot == std::string::npos ? fileName : fileName.substr(0, dot);
}

// Stable per-file badge colour: a cheap hash of the file name picks one of the
// design's badge hues (te.css .src.a-d) or the neutral one.
struct BadgeHue { std::uint32_t bg, fg; };
static BadgeHue BadgeColor(const std::string& name)
{
	static const BadgeHue kHues[] = {
		{ Tok::SrcBlueBg, Tok::SrcBlueFg },     { Tok::SrcPurpleBg, Tok::SrcPurpleFg },
		{ Tok::SrcGreenBg, Tok::SrcGreenFg },   { Tok::SrcAmberBg, Tok::SrcAmberFg },
		{ Tok::Surface3, Tok::TextMuted },
	};
	unsigned h = 2166136261u;
	for (char ch : name) { h ^= (unsigned char)ch; h *= 16777619u; }
	return kHues[h % (sizeof(kHues) / sizeof(kHues[0]))];
}

static ImFont* SmallFont() { return PobUi::Fonts().small ? PobUi::Fonts().small : ImGui::GetFont(); }
static float SmallPx() { ImFont* f = SmallFont(); return f->FontSize; }
static ImVec2 SmallSize(const char* s) { return SmallFont()->CalcTextSizeA(SmallPx(), FLT_MAX, 0.0f, s); }
static void SmallAt(ImDrawList* dl, ImVec2 p, std::uint32_t col, const char* s)
{
	dl->AddText(SmallFont(), SmallPx(), ImVec2(std::floor(p.x), std::floor(p.y)), col, s);
}

// A rounded pill: the source-file badge, the REV tag, the header's Beta.
static float PillW(const char* s) { return std::ceil(SmallSize(s).x + D(16.0f)); }
static void PillAt(ImDrawList* dl, ImVec2 p, float h, std::uint32_t bg, std::uint32_t fg, const char* s)
{
	const float w = PillW(s);
	dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), bg, h * 0.5f);
	const ImVec2 ts = SmallSize(s);
	SmallAt(dl, ImVec2(p.x + (w - ts.x) * 0.5f, p.y + (h - ts.y) * 0.5f), fg, s);
}

// ---- POB colour escapes ------------------------------------------------------
// POB marks colour inline: ^xRRGGBB for a literal colour, ^0..^9 for a palette
// index. The dictionaries carry them verbatim (218 entries do) because the
// engine hands the codes straight back to POB, so the editor has to show them
// the way POB will draw them rather than as raw escape text.
//
// Palette and parsing rules mirror engine/common/common.cpp IsColorEscape /
// ReadColorEscape and the colorEscape[] table -- kept identical on purpose; a
// preview that invents its own colours would be worse than none. These are
// POB's colours, data rather than theme, so they are not design tokens.
static const ImVec4 kPobPalette[10] = {
	ImVec4(0.0f, 0.0f, 0.0f, 1.0f), // ^0 black
	ImVec4(1.0f, 0.0f, 0.0f, 1.0f), // ^1 red
	ImVec4(0.0f, 1.0f, 0.0f, 1.0f), // ^2 green
	ImVec4(0.0f, 0.0f, 1.0f, 1.0f), // ^3 blue
	ImVec4(1.0f, 1.0f, 0.0f, 1.0f), // ^4 yellow
	ImVec4(1.0f, 0.0f, 1.0f, 1.0f), // ^5 purple
	ImVec4(0.0f, 1.0f, 1.0f, 1.0f), // ^6 aqua
	ImVec4(1.0f, 1.0f, 1.0f, 1.0f), // ^7 white
	ImVec4(0.7f, 0.7f, 0.7f, 1.0f), // ^8 gray
	ImVec4(0.4f, 0.4f, 0.4f, 1.0f), // ^9 dark gray
};

// The swatches the multi-line editor offers: POB's own colorCodes
// (PathOfBuilding Data/Global.lua), so an inserted colour is one POB itself uses.
struct PobSwatch { const char* code; const char* name; };
static const PobSwatch kSwatches[] = {
	{ "^7", u8"^7 預設（白）" },
	{ "^xC8C8C8", u8"^xC8C8C8 一般（NORMAL）" },
	{ "^x8888FF", u8"^x8888FF 魔法（MAGIC）" },
	{ "^xFFFF77", u8"^xFFFF77 稀有（RARE）" },
	{ "^xAF6025", u8"^xAF6025 傳奇（UNIQUE）" },
	{ "^xB97123", u8"^xB97123 火焰（FIRE）" },
	{ "^x3F6DB3", u8"^x3F6DB3 冰冷（COLD）" },
	{ "^xADAA47", u8"^xADAA47 閃電（LIGHTNING）" },
	{ "^xD02090", u8"^xD02090 混沌（CHAOS）" },
	{ "^x33FF77", u8"^x33FF77 正值（POSITIVE）" },
	{ "^xDD0022", u8"^xDD0022 負值（NEGATIVE）" },
	{ "^x7F7F7F", u8"^x7F7F7F 停用（DISABLED）" },
};

static bool is_hex_digit(char c)
{
	return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

// Length of the escape at s[i], or 0 when there is none. 2 for ^N, 8 for ^xRRGGBB.
static int pob_escape_len(const std::string& s, size_t i)
{
	if (i >= s.size() || s[i] != '^') return 0;
	if (i + 1 >= s.size()) return 0;
	char d = s[i + 1];
	if (d >= '0' && d <= '9') return 2;
	if (d == 'x' || d == 'X') {
		if (i + 7 >= s.size()) return 0;
		for (int c = 0; c < 6; c++)
			if (!is_hex_digit(s[i + 2 + c])) return 0;
		return 8;
	}
	return 0;
}

static bool has_pob_color(const std::string& s)
{
	for (size_t i = 0; i < s.size(); i++)
		if (pob_escape_len(s, i)) return true;
	return false;
}

static ImVec4 escape_colour(const std::string& s, size_t i, int esc)
{
	if (esc == 2) return kPobPalette[s[i + 1] - '0'];
	auto hex = [&](size_t off) {
		int v = 0;
		for (size_t k = 0; k < 2; k++) {
			char c = s[off + k];
			v = v * 16 + (c <= '9' ? c - '0' : (c | 32) - 'a' + 10);
		}
		return v / 255.0f;
	};
	return ImVec4(hex(i + 2), hex(i + 4), hex(i + 6), 1.0f);
}

static ImVec4 hex_colour(const char* code)   // "^xRRGGBB" or "^N"
{
	const std::string s = code;
	const int esc = pob_escape_len(s, 0);
	return esc ? escape_colour(s, 0, esc) : kPobPalette[7];
}

// Draw `s` the way POB would: escapes consumed, following text tinted. One line
// (the table's rows are one line each). `base` is the starting colour.
static void TextPobColored(const std::string& s, const ImVec4& base)
{
	ImVec4 cur = base;
	std::string run;
	bool first = true;
	auto flush = [&]() {
		if (run.empty()) return;
		if (!first) ImGui::SameLine(0, 0);
		ImGui::TextColored(cur, "%s", run.c_str());
		first = false;
		run.clear();
	};
	for (size_t i = 0; i < s.size();) {
		int esc = pob_escape_len(s, i);
		if (!esc) { run += s[i++]; continue; }
		flush();
		cur = escape_colour(s, i, esc);
		i += esc;
	}
	flush();
	if (first) ImGui::TextUnformatted(""); // keep the row height when empty
}

// The multi-line preview: same as TextPobColored, but a real newline or the two
// characters \n (stats.json's spelling) starts a new line, as POB shows them.
static void DrawPobPreview(const std::string& s, const ImVec4& base)
{
	ImVec4 cur = base;
	std::string run;
	bool lineStarted = false;
	auto flush = [&]() {
		if (run.empty()) return;
		if (lineStarted) ImGui::SameLine(0, 0);
		ImGui::TextColored(cur, "%s", run.c_str());
		lineStarted = true;
		run.clear();
	};
	auto newline = [&]() {
		flush();
		if (!lineStarted) ImGui::TextUnformatted("");
		lineStarted = false;
	};
	for (size_t i = 0; i < s.size();) {
		if (s[i] == '\r') { i++; continue; }
		if (s[i] == '\n') { newline(); i++; continue; }
		if (s[i] == '\\' && i + 1 < s.size() && s[i + 1] == 'n') { newline(); i += 2; continue; }
		int esc = pob_escape_len(s, i);
		if (!esc) { run += s[i++]; continue; }
		flush();
		cur = escape_colour(s, i, esc);
		i += esc;
	}
	flush();
	if (!lineStarted) ImGui::TextUnformatted("");
}

// First line of `s` and how many more there are (a real newline or the two
// characters \n).
static std::string first_line(const std::string& s, int* more)
{
	const int lines = TransEd::LineCount(s);
	if (more) *more = lines - 1;
	size_t cut = std::string::npos;
	for (size_t i = 0; i < s.size(); i++) {
		if (s[i] == '\n' || s[i] == '\r') { cut = i; break; }
		if (s[i] == '\\' && i + 1 < s.size() && s[i + 1] == 'n') { cut = i; break; }
	}
	return cut == std::string::npos ? s : s.substr(0, cut);
}

static std::string strip_pob_colours(const std::string& s)
{
	std::string out;
	for (size_t i = 0; i < s.size();) {
		const int esc = pob_escape_len(s, i);
		if (esc) { i += (size_t)esc; continue; }
		out += s[i++];
	}
	return out;
}

static std::string filetime_text(unsigned long long ft)
{
	if (!ft) return std::string();
	FILETIME f{ (DWORD)(ft & 0xffffffffu), (DWORD)(ft >> 32) }, lf{};
	SYSTEMTIME st{};
	if (!FileTimeToLocalFileTime(&f, &lf) || !FileTimeToSystemTime(&lf, &st)) return std::string();
	char buf[32];
	snprintf(buf, sizeof(buf), "%02u/%02u %02u:%02u", st.wMonth, st.wDay, st.wHour, st.wMinute);
	return buf;
}

// ---- file picker ---------------------------------------------------------------
// Target-file picker, ordered the way the ENGINE loads the files rather than
// alphabetically. Which file you write to decides whether the edit does anything
// at all: the dictionary is one flat map merged in meta.json's load_order and the
// last file to define a key wins. poe1 and poe2 order the same files differently,
// so the name alone tells the user nothing.
//
// Drawn like PobUi::Select, with what Select cannot do: a disabled option (a file
// not in load_order is never merged, so writing there is guaranteed to do
// nothing) and a note per row ("已有這個 key", "最後載入").
static std::string FileLabel(const EditorModel& model, int fi)
{
	if (fi < 0 || fi >= (int)model.files.size()) return u8"（選擇檔案）";
	const EditorFile& f = model.files[fi];
	return f.order < 0 ? f.name : std::to_string(f.order + 1) + ". " + f.name;
}

static bool FileSelect(const char* id, const EditorModel& model, int* target, float width,
                       const std::vector<int>* owners, bool testOpen, bool inRow = false)
{
	bool changed = false;
	const std::vector<int> order = FileIdxInLoadOrder(model);
	int lastListed = -1;
	for (int fi : order) if (model.files[fi].order >= 0) lastListed = fi;

	struct Row { int fi; std::string label, note; bool enabled; };
	std::vector<Row> rows;
	for (int fi : order) {
		Row r;
		r.fi = fi;
		r.enabled = model.files[fi].order >= 0;
		r.label = model.files[fi].order < 0 ? model.files[fi].name
		                                    : std::to_string(model.files[fi].order + 1) + ". " + model.files[fi].name;
		if (!r.enabled) r.note = u8"未列入載入順序，不會被讀取";
		else if (owners && std::find(owners->begin(), owners->end(), fi) != owners->end())
			r.note = fi == lastListed ? u8"已有這個 key · 最後載入" : u8"已有這個 key";
		else if (fi == lastListed) r.note = u8"最後載入";
		rows.push_back(std::move(r));
	}
	ImFont* body = ImGui::GetFont();
	const float bodyPx = ImGui::GetFontSize();
	float labelMax = 0.0f, noteMax = 0.0f;
	for (const Row& r : rows) {
		labelMax = std::max(labelMax, body->CalcTextSizeA(bodyPx, FLT_MAX, 0.0f, r.label.c_str()).x);
		if (!r.note.empty()) noteMax = std::max(noteMax, SmallSize(r.note.c_str()).x);
	}
	const float popupW = std::max(width, std::ceil(labelMax + (noteMax > 0 ? D(16.0f) + noteMax : 0.0f) + D(24.0f) +
	                                               ImGui::GetStyle().ScrollbarSize));

	// in a table row the row's own frame padding applies (every item in a row is
	// one height, see drawEntryTable)
	if (!inRow) PobUi::PushControlFrame();
	ImGui::PushStyleColor(ImGuiCol_PopupBg, PobUi::TokV4(Tok::SurfaceRaised));
	ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, D(8.0f));
	ImGui::SetNextItemWidth(width);
	ImGui::SetNextWindowSizeConstraints(ImVec2(popupW, 0.0f), ImVec2(FLT_MAX, FLT_MAX));
	if (testOpen) ImGui::OpenPopupEx(ImHashStr("##ComboPopup", 0, ImGui::GetCurrentWindow()->GetID(id)));
	const std::string preview = FileLabel(model, *target);
	// the popup's own padding: inside a dialog it would inherit the dialog's 24 px
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(D(8.0f), D(6.0f)));
	const bool open = ImGui::BeginCombo(id, preview.c_str(), ImGuiComboFlags_HeightLarge);
	ImGui::PopStyleVar();
	if (open) {
		const float right = ImGui::GetContentRegionAvail().x;
		bool separated = false;
		for (const Row& r : rows) {
			if (!r.enabled && !separated) { ImGui::Separator(); separated = true; }
			ImGui::PushID(r.fi);
			const ImVec2 p = ImGui::GetCursorScreenPos();
			ImGui::BeginDisabled(!r.enabled);
			if (ImGui::Selectable("##opt", r.fi == *target, 0, ImVec2(0, bodyPx + D(6.0f))) && r.fi != *target) {
				*target = r.fi;
				changed = true;
			}
			ImGui::EndDisabled();
			ImDrawList* dl = ImGui::GetWindowDrawList();
			dl->AddText(body, bodyPx, ImVec2(p.x, p.y + D(3.0f)), r.enabled ? Tok::Text : Tok::TextFaint, r.label.c_str());
			if (!r.note.empty()) {
				const ImVec2 ns = SmallSize(r.note.c_str());
				SmallAt(dl, ImVec2(p.x + right - ns.x, p.y + D(3.0f) + (bodyPx - ns.y) * 0.5f),
				        r.enabled ? Tok::TextMuted : Tok::TextFaint, r.note.c_str());
			}
			ImGui::PopID();
		}
		ImGui::EndCombo();
	}
	ImGui::PopStyleVar();
	ImGui::PopStyleColor();
	if (!inRow) PobUi::PopControlFrame();
	return changed;
}

// ---- translation editor, as a panel -----------------------------------------
//
// The window / GL context / font atlas / main loop belong to whichever host is
// drawing this: RunToolWindow for a window of its own, the launcher's tab body
// when embedded. See tool_panel.h.
//
// Layout (2026-10-09 design, TranslationEditor / TEAdd / TEMissing / TEEdit /
// TEStates): one header row (dictionary | language | entries / missing | undo |
// more | save), then the page, then a one-line status footer.

namespace {

// An action waiting for the unsaved-changes prompt. Every entry point (game
// select, locale select, reload, closing the tab or the window) needs the same
// guard, so they set a pending action instead of each opening their own.
enum class Pending { None, SwitchGame, SwitchLocale, Reload, Close };

// "launcher" is the launcher's own labels. Listed here rather than special-cased
// anywhere because the launcher dictionary folder has the same shape as a game
// one -- meta.json with a load_order, plus dictionary files.
const char* kGameNames[kDictSlotCount] = { "Path of Exile 1", "Path of Exile 2", u8"啟動器介面" };

// Messages, as states rather than strings: a colour picked by searching a
// message for "失敗" broke the moment a message said it differently.
enum class Notice { None, LocaleFallback };

} // namespace

class TranslationEditorPanel : public IToolPanel {
public:
	bool Init(const ToolPanelHost& h) override
	{
		host_ = &h;
		exeDir = h.exeDir;
		scale = h.scale;

		for (int i = 0; i < kDictSlotCount; i++)
			if (h.game == DictSlotFolder((DictSlot)i)) { gi = i; break; }

		// Test aid (POBTOOLS_TE_STATE, with POBTOOLS_TOOL_SHOT): a known state for a
		// hidden-window screenshot. Either variable makes this a test run, which
		// never writes a dictionary, opens Explorer or talks to the launcher.
		{
			wchar_t st[32] = L"";
			const DWORD n = GetEnvironmentVariableW(L"POBTOOLS_TE_STATE", st, 32);
			if (n > 0 && n < 32) for (const wchar_t* c = st; *c; c++) testState_ += (char)(*c < 128 ? *c : '?');
			testMode_ = !testState_.empty() || GetEnvironmentVariableW(L"POBTOOLS_TOOL_SHOT", nullptr, 0) > 0;
		}

		// The dictionaries the ENGINE will read, per slot. Editing the built-in copy
		// while POB reads an external one is the one failure this whole feature has to
		// avoid, so the editor resolves the same settings the launcher does. Resolved
		// once for all three: switching must not re-read the ini and pick up a
		// half-finished edit made elsewhere.
		editCfg = LoadLauncherConfig(exeDir + L"pob-zh.ini");
		for (int i = 0; i < kDictSlotCount; i++)
			slotDir[i] = ResolveDictDir(exeDir, (DictSlot)i, editCfg.dataDir[i]);

		// Locales come from disk, not a hardcoded list: the caller's choice used to be
		// discarded outright, so opening the editor from an "en" launcher silently
		// edited the Chinese dictionary.
		refreshLocales();
		if (locales.empty()) {
			PobLog::Error("panel", u8"翻譯編輯器找不到任何語系資料夾：" + narrow(slotRoot()));
			return false;
		}
		{
			const std::string want = narrow(h.locale);
			if (std::find(locales.begin(), locales.end(), want) != locales.end()) {
				curLocale = want;
			} else {
				auto zh = std::find(locales.begin(), locales.end(), std::string("zh-rTW"));
				curLocale = zh != locales.end() ? *zh : locales[0];
				if (!want.empty()) {
					notice_ = Notice::LocaleFallback;
					noticeArg_ = want;
				}
			}
		}
		loadModel();
		applyTestState();
		return true;
	}

	void Frame() override
	{
		// The host's font-size zoom can change while the panel is open (the
		// launcher's slider; a tool window follows the ini): read it per frame.
		scale = host_->scale;
		editSeen_ = false;

		drawHeader();

		if (!host_->cjkOk) {
			PobUi::Banner("##tecjk", PobUi::BannerTone::Warn, PobIcon::TriangleAlert, u8"中文字型沒有載入",
			              u8"Fonts\\ 內的字型讀不到，中文會顯示成 ?。請確認字型檔還在。", false, nullptr, false);
			ImGui::Dummy(ImVec2(0, D(4.0f)));
		}

		const float footH = std::floor(SmallPx() + D(14.0f));
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
		ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
		ImGui::BeginChild("##tebody", ImVec2(0, -footH), false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
		ImGui::PopStyleColor();
		ImGui::PopStyleVar();
		if (!model.localeExists) drawNoData();
		else if (view == 0) drawEntries();
		else drawMissing();
		ImGui::EndChild();
		drawFooter();

		// An inline edit that ended without its cell reporting it (the row
		// scrolled out of the clipper while the box was active) still lands on the
		// undo stack.
		if (editActive_ && !editSeen_) finishInlineEdit();

		drawMoreMenu();
		drawAddDialog();
		drawBigDialog();
		drawBigConfirm();
		drawUnsavedDialog();

		// Ctrl+Z: the editor's own undo, only while no text box is active -- typed
		// into a box it is that box's undo.
		const ImGuiIO& io = ImGui::GetIO();
		if (io.KeyCtrl && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Z, false) && !ImGui::IsAnyItemActive() &&
		    !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
			doUndo();
	}

	void RunDeferred() override
	{
		if (openFolder_) {
			openFolder_ = false;
			if (!testMode_) {
				const HINSTANCE r = ShellExecuteW((HWND)host_->hostHwnd, L"open", model.dataDir.c_str(), nullptr,
				                                  nullptr, SW_SHOWNORMAL);
				if ((INT_PTR)r <= 32)
					PobLog::Error("panel", u8"開啟字典資料夾失敗：" + narrow(model.dataDir));
			}
		}
	}

	ToolCloseState RequestClose() override
	{
		if (close_ == ToolCloseState::Open || close_ == ToolCloseState::Cancelled) {
			if (DirtyCount(model) > 0) {
				pending = Pending::Close;
				openUnsaved_ = true;
				close_ = ToolCloseState::Asking;
			} else {
				close_ = ToolCloseState::Closed;
			}
		}
		return close_;
	}
	ToolCloseState CloseState() const override { return close_; }

	void AbortClose() override
	{
		// Only an agreement not yet acted on is taken back. A panel mid-prompt keeps
		// its prompt: the user is looking at it and answering it decides the outcome.
		if (close_ == ToolCloseState::Closed) close_ = ToolCloseState::Open;
	}

	PobUi::Density Density() const override { return PobUi::Density::Compact; }
	const char* PanelId() const override { return "trans"; }

	// True once a save has happened, so the launcher can reload its own labels: this
	// editor can be editing the launcher's dictionary, i.e. the very strings the
	// launcher is drawing with. Reading it clears it.
	bool TakeSavedFlag() { const bool f = saved_; saved_ = false; return f; }
	// "前往設定" was pressed in a launcher tab. Reading it clears it.
	bool TakeSettingsRequest() { const bool f = wantSettings_; wantSettings_ = false; return f; }

	// ---- headless driver (--trans-editor-selftest) ------------------------------
	// The same entry points the UI calls, without a frame.
	std::string TestOp(const std::string& op)
	{
		if (op == "edit") {
			for (const EditorEntry& e : model.entries) {
				if (e.structured) continue;
				const int fi = e.fileIdx;
				const std::string key = e.key, v = e.value + u8"（測）";
				if (TransEd::SetWithUndo(model, undo_, fi, key, v)) rebuildFilter();
				break;
			}
		} else if (op.compare(0, 11, "switchgame:") == 0) {
			requestSwitchGame(atoi(op.c_str() + 11));
		} else if (op.compare(0, 13, "switchlocale:") == 0) {
			requestSwitchLocale(op.substr(13));
		} else if (op == "reload") {
			requestReload();
		} else if (op == "answer:save") {
			resolvePending(PobUi::DialogResult::Primary);
		} else if (op == "answer:discard") {
			resolvePending(PobUi::DialogResult::Danger);
		} else if (op == "answer:cancel") {
			resolvePending(PobUi::DialogResult::Cancel);
		} else if (op == "save") {
			doSave();
		} else if (op == "undo") {
			doUndo();
		} else if (op == "onlymodified") {
			onlyModified_ = true;
			rebuildFilter();
		}
		const char* pn = pending == Pending::None ? "none" : pending == Pending::SwitchGame ? "game"
		               : pending == Pending::SwitchLocale ? "locale" : pending == Pending::Reload ? "reload" : "close";
		return "undo=" + std::to_string(undo_.Size()) + " modified=" + std::to_string(DirtyEntryCount(model)) +
		       " files=" + std::to_string(DirtyCount(model)) + " game=" + std::to_string(gi) + " locale=" + curLocale +
		       " pending=" + pn + " shown=" + std::to_string(filtered.size()) +
		       " savefail=" + (saveFailed_ ? "1" : "0") + " entries=" + std::to_string(model.entries.size());
	}

private:
	std::wstring slotRoot() const { return slotDir[gi].root; }
	bool external() const { return slotDir[gi].status == DataDirStatus::External || testExternal_; }

	void refreshLocales()
	{
		locales = ListLocales(slotRoot());
		localeNames.clear();
		for (const std::string& l : locales) localeNames.push_back(LocaleDisplayName(slotRoot(), l));
	}

	std::string localeLabel(const std::string& code) const
	{
		for (size_t i = 0; i < locales.size(); i++)
			if (locales[i] == code && !localeNames[i].empty()) return localeNames[i];
		return code;
	}

	// Load curLocale of the current slot and reset everything that pointed into the
	// old model. Every switch, reload and the first load go through here.
	void loadModel()
	{
		model = LoadModel(slotRoot(), curLocale);
		undo_.Clear();
		editActive_ = false;
		fileFilter = 0;
		bigOpenFor_.clear();
		bigFile_ = -1;
		addTarget_ = -1;
		addHintFor_.clear();
		saveFailed_ = false;
		saveFailFiles_.clear();
		rebuildFilter();
		runScan();
	}

	void rebuildFilter()
	{
		filtered.clear();
		filtered.reserve(model.entries.size());
		for (size_t i = 0; i < model.entries.size(); i++) {
			const EditorEntry& e = model.entries[i];
			if (fileFilter != 0 && e.fileIdx != fileFilter - 1) continue;
			if (onlyModified_ && !e.edited) continue;
			if (!searchLower.empty() &&
				!contains_ci(e.key, searchLower) && !contains_ci(e.value, searchLower))
				continue;
			filtered.push_back(i);
		}
	}

	void setSearch(const std::string& s)
	{
		snprintf(searchBuf_, sizeof(searchBuf_), "%s", s.c_str());
		searchLower = to_lower_ascii(searchBuf_);
	}

	// ---- actions --------------------------------------------------------------
	void requestSwitchGame(int g)
	{
		if (g < 0 || g >= kDictSlotCount || g == gi) return;
		if (DirtyCount(model) > 0) { pendingGi = g; pending = Pending::SwitchGame; openUnsaved_ = true; return; }
		gi = g;
		switchedSlot();
	}
	void requestSwitchLocale(const std::string& code)
	{
		if (code == curLocale) return;
		if (DirtyCount(model) > 0) { pendingLocale = code; pending = Pending::SwitchLocale; openUnsaved_ = true; return; }
		curLocale = code;
		notice_ = Notice::None;
		loadModel();
	}
	void requestReload()
	{
		if (DirtyCount(model) > 0) { pending = Pending::Reload; openUnsaved_ = true; return; }
		refreshLocales();
		loadModel();
	}
	// A new slot keeps the language when it has it; otherwise the page says so
	// (EmptyState) instead of silently opening a different language.
	void switchedSlot()
	{
		refreshLocales();
		notice_ = Notice::None;
		loadModel();
	}

	void resolvePending(PobUi::DialogResult r)
	{
		const Pending what = pending;
		if (what == Pending::None) return;
		if (r == PobUi::DialogResult::Cancel || r == PobUi::DialogResult::None) {
			pending = Pending::None;
			// The host asked; the user said no. Cancelled rather than Open, so whoever
			// started the close abandons it instead of asking again.
			if (close_ == ToolCloseState::Asking) close_ = ToolCloseState::Cancelled;
			return;
		}
		if (r == PobUi::DialogResult::Primary && !doSave()) {
			// The save failed: nothing is thrown away, the banner says what happened.
			pending = Pending::None;
			if (close_ == ToolCloseState::Asking) close_ = ToolCloseState::Cancelled;
			return;
		}
		pending = Pending::None;
		switch (what) {
			case Pending::Close: close_ = ToolCloseState::Closed; break;
			case Pending::SwitchGame: gi = pendingGi; switchedSlot(); break;
			case Pending::SwitchLocale: curLocale = pendingLocale; notice_ = Notice::None; loadModel(); break;
			case Pending::Reload: refreshLocales(); loadModel(); break;
			default: break;
		}
	}

	// Save every edited file. True when all of them were written.
	bool doSave()
	{
		const int entries = DirtyEntryCount(model), files = DirtyCount(model);
		if (files == 0) return true;
		std::vector<std::string> failed;
		std::string err;
		if (testMode_) {
			// never writes: a save failure is simulated for its screenshot
			if (testState_ == "savefail") {
				for (const EditorFile& f : model.files) if (f.dirty) failed.push_back(f.name);
			}
		} else {
			SaveAll(model, &err, &failed);
		}
		undo_.Clear();
		editActive_ = false;
		if (!failed.empty()) {
			saveFailed_ = true;
			saveFailFiles_ = failed;
			saved_ = (int)failed.size() < files && !testMode_;
			rebuildFilter();
			return false;
		}
		if (testMode_) {
			for (EditorEntry& e : model.entries) { e.edited = false; e.added = false; e.orig = e.value; }
			for (EditorFile& f : model.files) { f.dirty = false; f.docChanged = false; }
		}
		saved_ = !testMode_;
		saveFailed_ = false;
		saveFailFiles_.clear();
		rebuildFilter();
		// When it takes effect, checked against the code (2026-10-09): POB loads the
		// dictionaries once, when it starts (ui_api.cpp InitAPI ->
		// translation_init_async); nothing reloads them while it runs. The
		// launcher's own labels are re-read right after a save when this editor is
		// one of its tabs (launcher_ui.cpp, TranslationEditorPanelSaved).
		std::string msg = u8"已儲存 " + std::to_string(entries) + u8" 筆（" + std::to_string(files) + u8" 個檔案）。";
		if (gi == (int)DictSlot::Launcher)
			msg += host_ && host_->embedded ? u8"啟動器文字已更新" : u8"啟動器重開後套用";
		else
			msg += u8"已開著的 POB 要重開才會套用";
		toast(msg, PobUi::Tone::Ok);
		return true;
	}

	// The headless driver runs without an ImGui context (ShowToast reads its clock).
	static void toast(const std::string& msg, PobUi::Tone tone)
	{
		if (ImGui::GetCurrentContext()) PobUi::ShowToast(msg.c_str(), tone);
	}

	void doUndo()
	{
		if (editActive_) finishInlineEdit();
		bool reshaped = false;
		if (!TransEd::UndoInto(model, undo_, missRows_, &reshaped)) return;
		rebuildFilter();
	}

	void goSettings()
	{
		if (testMode_) return;
		if (host_->embedded) {
			wantSettings_ = true;
		} else {
			// A window of its own has no launcher to switch to.
			toast(u8"這是獨立視窗：請到啟動器的「設定 → 翻譯資料」", PobUi::Tone::Run);
		}
	}

	void beginInlineEdit(size_t ei)
	{
		if (editActive_) finishInlineEdit();
		editActive_ = true;
		editFile_ = model.entries[ei].fileIdx;
		editKey_ = model.entries[ei].key;
		editStart_ = model.entries[ei].value;
	}
	void finishInlineEdit()
	{
		editActive_ = false;
		const long long at = FindEntry(model, editFile_, editKey_);
		if (at < 0) return;
		const std::string now = model.entries[(size_t)at].value;
		if (now == editStart_) return;
		TransEd::UndoStep s;
		TransEd::EntryChange c;
		c.fileIdx = editFile_;
		c.key = editKey_;
		c.before.exists = true;
		c.before.value = editStart_;
		c.after.exists = true;
		c.after.value = now;
		s.changes.push_back(std::move(c));
		undo_.Push(std::move(s));
	}

	void runScan()
	{
		missLogged_ = 0;
		missLogWrite_ = 0;
		misses = ScanMisses(exeDir, model, &missLogFound, &missLogged_, &missLogWrite_);
		missScanned = true;
		SYSTEMTIME st{};
		GetLocalTime(&st);
		char buf[16];
		snprintf(buf, sizeof(buf), "%02u:%02u", st.wHour, st.wMinute);
		missScanAt_ = buf;
		int uiIdx = FindFileIdx(model, "ui.json");
		if (uiIdx < 0 || model.files[uiIdx].order < 0) {
			// the last listed file: a new key there always takes effect
			uiIdx = -1;
			for (int fi : FileIdxInLoadOrder(model)) if (model.files[fi].order >= 0) uiIdx = fi;
		}
		missRows_.assign(misses.size(), TransEd::MissRow());
		for (TransEd::MissRow& r : missRows_) r.target = uiIdx;
	}

	int missVisibleUnfilled() const
	{
		int n = 0;
		for (size_t i = 0; i < misses.size(); i++)
			if ((missShowReverse_ || !misses[i].reverse) && !missRows_[i].filled) n++;
		return n;
	}

	// ---- header -----------------------------------------------------------------
	void drawHeader()
	{
		const PobUi::WidgetFonts& wf = PobUi::Fonts();
		const float H = PobUi::ControlH();
		const float gap = D(12.0f);
		const float smH = std::floor(D(28.0f));
		const ImVec2 hp = ImGui::GetCursorScreenPos();
		const float avail = ImGui::GetContentRegionAvail().x;
		ImDrawList* dl = ImGui::GetWindowDrawList();
		auto at = [&](float x, float h) { ImGui::SetCursorScreenPos(ImVec2(x, hp.y + std::floor((H - h) * 0.5f))); };

		// labels
		const char* gameNotes[kDictSlotCount];
		std::string gameNoteS[kDictSlotCount];
		for (int i = 0; i < kDictSlotCount; i++) {
			gameNoteS[i] = slotDir[i].status == DataDirStatus::External ? u8"外部資料夾" : u8"內建字典";
			gameNotes[i] = gameNoteS[i].c_str();
		}
		std::vector<std::string> locLbl, locNote;
		bool curListed = false;
		for (size_t i = 0; i < locales.size(); i++) {
			locLbl.push_back(localeNames[i].empty() ? locales[i] : localeNames[i]);
			locNote.push_back(locales[i]);
			if (locales[i] == curLocale) curListed = true;
		}
		if (!curListed) {   // the language this slot has no folder for (EmptyState below)
			locLbl.push_back(curLocale);
			locNote.push_back(u8"沒有資料");
		}
		std::vector<const char*> locP, locN;
		int locSel = 0;
		for (size_t i = 0; i < locLbl.size(); i++) {
			locP.push_back(locLbl[i].c_str());
			locN.push_back(locNote[i].c_str());
			if ((i < locales.size() ? locales[i] : curLocale) == curLocale) locSel = (int)i;
		}
		const std::string missLbl = missScanned && missLogFound ? u8"缺漏 " + std::to_string(missVisibleUnfilled())
		                                                        : std::string(u8"缺漏");
		const char* viewLabels[2] = { u8"翻譯條目", missLbl.c_str() };

		const float iconPx = std::floor(D(20.0f));
		const char* title = u8"翻譯編輯器";
		const float headingW = wf.heading ? wf.heading->CalcTextSizeA(
			wf.headingPx > 0 ? wf.headingPx : wf.heading->FontSize, FLT_MAX, 0.0f, title).x
			: ImGui::CalcTextSize(title).x;
		const float gameW = std::max(std::floor(D(190.0f)), PobUi::SelectFitWidth(kGameNames, kDictSlotCount));
		const float locW = std::max(std::floor(D(150.0f)), PobUi::SelectFitWidth(locP.data(), (int)locP.size()));

		const int dirtyEntries = DirtyEntryCount(model);
		const std::string saveLbl = dirtyEntries > 0 ? u8"儲存 " + std::to_string(dirtyEntries) + u8" 筆變更"
		                                             : std::string(u8"儲存");
		const float undoW = PobUi::ButtonWidth(u8"復原", PobUi::BtnSize::Sm, PobIcon::Undo);
		const float moreW = PobUi::ButtonWidth("##temore", PobUi::BtnSize::Sm, PobIcon::MoreHorizontal, smH);
		const float saveW = PobUi::ButtonWidth(saveLbl.c_str(), PobUi::BtnSize::Sm, PobIcon::Save);
		const float rightW = undoW + D(8.0f) + moreW + D(8.0f) + saveW;

		float x = hp.x;
		PobUi::IconAt(dl, ImVec2(x, hp.y + std::floor((H - iconPx) * 0.5f)), PobIcon::Languages, Tok::AccentText, iconPx);
		x += PobUi::IconWidth(PobIcon::Languages, iconPx) + (wf.icons ? D(8.0f) : 0.0f);
		at(x, ImGui::GetTextLineHeight());
		PobUi::Heading(title);
		x = ImGui::GetItemRectMax().x + D(8.0f);
		{
			const float bh = std::floor(D(20.0f));
			PillAt(dl, ImVec2(x, hp.y + std::floor((H - bh) * 0.5f)), bh, Tok::Surface3, Tok::TextMuted, "Beta");
			x += PillW("Beta") + gap;
		}

		at(x, H);
		{
			int sel = gi;
			if (PobUi::Select("##tegame", &sel, kGameNames, gameNotes, kDictSlotCount, gameW) && sel != gi)
				requestSwitchGame(sel);
			if (ImGui::IsItemHovered()) PobUi::Tooltip(narrow(slotRoot()).c_str());
		}
		x += gameW + D(8.0f);
		at(x, H);
		{
			int sel = locSel;
			if (PobUi::Select("##telocale", &sel, locP.data(), locN.data(), (int)locP.size(), locW) && sel != locSel &&
			    sel < (int)locales.size())
				requestSwitchLocale(locales[(size_t)sel]);
		}
		x += locW + gap;
		at(x, H);
		{
			int v = view;
			if (PobUi::Segmented("##teview", &v, viewLabels, 2, model.localeExists)) {
				view = v;
				if (view == 1 && !missScanned) runScan();
			}
			if (ImGui::IsItemHovered() && view == 0)
				PobUi::Tooltip(u8"缺漏＝POB 執行時記下、字典裡還沒有的英文字串（translate_misses.log）");
		}
		x += PobUi::SegmentedWidth(viewLabels, 2) + gap;

		float rx = std::max(x, hp.x + avail - rightW);
		at(rx, smH);
		if (PobUi::Button(u8"復原", PobUi::BtnKind::Ghost, PobUi::BtnSize::Sm, PobIcon::Undo, 0.0f, !undo_.Empty()))
			doUndo();
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			PobUi::Tooltip(undo_.Empty() ? u8"Ctrl+Z · 沒有可復原的修改"
			                             : (u8"Ctrl+Z · 可復原 " + std::to_string(undo_.Size()) + u8" 步").c_str());
		rx += undoW + D(8.0f);
		at(rx, smH);
		if (PobUi::Button("##temore", PobUi::BtnKind::Secondary, PobUi::BtnSize::Sm, PobIcon::MoreHorizontal, smH))
			openMore_ = true;
		moreAnchor_ = ImVec2(ImGui::GetItemRectMax().x, ImGui::GetItemRectMax().y + D(4.0f));
		if (ImGui::IsItemHovered()) PobUi::Tooltip(u8"更多：重新載入、開啟字典資料夾");
		rx += moreW + D(8.0f);
		at(rx, smH);
		if (PobUi::Button(saveLbl.c_str(), PobUi::BtnKind::Primary, PobUi::BtnSize::Sm, PobIcon::Save, 0.0f,
		                  dirtyEntries > 0 && model.localeExists))
			doSave();
		if (ImGui::IsItemHovered() && dirtyEntries > 0)
			PobUi::Tooltip((std::to_string(dirtyEntries) + u8" 筆、" + std::to_string(DirtyCount(model)) +
			                u8" 個檔案；只寫入改到的值，檔案其餘內容不動").c_str());

		ImGui::SetCursorScreenPos(hp);
		ImGui::Dummy(ImVec2(avail, H));
		ImGui::Dummy(ImVec2(0, D(4.0f)));
		{
			const ImVec2 lp = ImGui::GetCursorScreenPos();
			dl->AddLine(ImVec2(lp.x, lp.y), ImVec2(lp.x + avail, lp.y), Tok::Border, 1.0f);
			ImGui::Dummy(ImVec2(0, D(6.0f)));
		}
	}

	void drawMoreMenu()
	{
		if (openMore_) {
			ImGui::OpenPopup("##temoremenu");
			openMore_ = false;
		}
		ImGui::SetNextWindowPos(moreAnchor_, ImGuiCond_Appearing, ImVec2(1.0f, 0.0f));
		if (!PobUi::BeginMenuPopup("##temoremenu")) return;
		if (PobUi::MenuRow(PobIcon::Refresh, u8"重新載入")) requestReload();
		if (PobUi::MenuRow(PobIcon::FolderOpen, u8"開啟字典資料夾", nullptr, model.localeExists)) openFolder_ = true;
		PobUi::EndMenuPopup();
	}

	// ---- page: entries ------------------------------------------------------------
	void drawDataBanner()
	{
		if (external()) {
			if (externalClosed_) return;
			const std::string path = narrow(model.dataDir);
			if (PobUi::Banner("##teext", PobUi::BannerTone::Info, PobIcon::Info,
			                  u8"編輯的是外部翻譯資料夾：翻譯資料自動更新不會蓋到這裡", path.c_str(), true, nullptr,
			                  true) == PobUi::BannerResult::Close)
				externalClosed_ = true;
		} else {
			if (builtinClosed_) return;
			// Built-in dictionaries are what a translation update replaces wholesale
			// (app_update applies the data pack over Data\ in the install folder). A
			// player who edits them loses the work on the next data-<n> and only sees
			// "my change did nothing" (field report, 2026-09-25).
			const PobUi::BannerResult r = PobUi::Banner(
				"##tebuiltin", PobUi::BannerTone::Warn, PobIcon::TriangleAlert,
				u8"你正在改內建字典：翻譯資料自動更新時會被整份蓋掉",
				u8"要保留自己的翻譯，先在啟動器「設定 → 翻譯資料」複製到外部資料夾再改。", false, u8"前往設定", true);
			if (r == PobUi::BannerResult::Action) goSettings();
			if (r == PobUi::BannerResult::Close) builtinClosed_ = true;
		}
		ImGui::Dummy(ImVec2(0, D(6.0f)));
	}

	void drawNotice()
	{
		if (notice_ == Notice::LocaleFallback) {
			const std::string t = u8"沒有「" + noticeArg_ + u8"」的翻譯資料，已改開 " + localeLabel(curLocale) +
			                      u8"（" + curLocale + u8"）";
			if (PobUi::Banner("##tenotice", PobUi::BannerTone::Info, PobIcon::Info, t.c_str(), nullptr, false, nullptr,
			                  true) == PobUi::BannerResult::Close)
				notice_ = Notice::None;
			ImGui::Dummy(ImVec2(0, D(6.0f)));
		}
	}

	void drawSaveFailBanner()
	{
		if (!saveFailed_) return;
		std::string files;
		for (const std::string& f : saveFailFiles_) files += (files.empty() ? "" : u8"、") + f;
		const std::string title = files + u8" 沒有存成功";
		const PobUi::BannerResult r = PobUi::Banner(
			"##tesavefail", PobUi::BannerTone::Bad, PobIcon::CircleX, title.c_str(),
			u8"可能有其他程式正開著這個檔，或資料夾不能寫入（原因已寫進錯誤記錄）。修改都還在，處理後按「重試」。",
			false, u8"重試", true);
		if (r == PobUi::BannerResult::Action) doSave();
		if (r == PobUi::BannerResult::Close) saveFailed_ = false;
		ImGui::Dummy(ImVec2(0, D(6.0f)));
	}

	void drawEntries()
	{
		drawSaveFailBanner();
		drawNotice();
		drawDataBanner();

		// filter row: search | scope | only modified ... add
		{
			const float H = PobUi::ControlH();
			const ImVec2 rp = ImGui::GetCursorScreenPos();
			const float avail = ImGui::GetContentRegionAvail().x;
			auto at = [&](float x, float h) { ImGui::SetCursorScreenPos(ImVec2(x, rp.y + std::floor((H - h) * 0.5f))); };
			float x = rp.x;
			const float searchW = std::floor(D(340.0f));
			at(x, H);
			if (PobUi::SearchField("##tesearch", searchBuf_, (int)sizeof(searchBuf_), u8"搜尋 key 或翻譯…", searchW)) {
				searchLower = to_lower_ascii(searchBuf_);
				rebuildFilter();
			}
			x += searchW + D(10.0f);

			std::vector<std::string> lbl, note;
			lbl.push_back(u8"全部檔案");
			note.push_back(std::to_string(model.files.size()) + u8" 個");
			std::vector<int> counts(model.files.size(), 0);
			for (const EditorEntry& e : model.entries) counts[(size_t)e.fileIdx]++;
			for (size_t i = 0; i < model.files.size(); i++) {
				lbl.push_back(model.files[i].name);
				note.push_back(group_digits(counts[i]) + u8" 筆");
			}
			std::vector<const char*> lp, np;
			for (size_t i = 0; i < lbl.size(); i++) { lp.push_back(lbl[i].c_str()); np.push_back(note[i].c_str()); }
			const float scopeW = std::max(std::floor(D(190.0f)), PobUi::SelectFitWidth(lp.data(), (int)lp.size()));
			at(x, H);
			int sel = fileFilter;
			if (PobUi::Select("##tescope", &sel, lp.data(), np.data(), (int)lp.size(), scopeW) && sel != fileFilter) {
				fileFilter = sel;
				rebuildFilter();
			}
			if (ImGui::IsItemHovered()) PobUi::Tooltip(u8"資料範圍：只看某一個字典檔");
			x += scopeW + D(10.0f);

			at(x, H);
			if (PobUi::Switch("##teonlymod", &onlyModified_)) rebuildFilter();
			x += PobUi::SwitchWidth() + D(8.0f);
			at(x, SmallPx());
			const std::string onlyLbl = u8"只看已修改（" + std::to_string(DirtyEntryCount(model)) + u8"）";
			PobUi::Hint(onlyLbl.c_str());
			if (ImGui::IsItemClicked()) { onlyModified_ = !onlyModified_; rebuildFilter(); }

			const float addW = PobUi::ButtonWidth(u8"新增條目", PobUi::BtnSize::Md, PobIcon::Plus);
			at(std::max(ImGui::GetItemRectMax().x + D(10.0f), rp.x + avail - addW), H);
			if (PobUi::Button(u8"新增條目", PobUi::BtnKind::Secondary, PobUi::BtnSize::Md, PobIcon::Plus)) openAddDialog();
			ImGui::SetCursorScreenPos(rp);
			ImGui::Dummy(ImVec2(avail, H));
			ImGui::Dummy(ImVec2(0, D(6.0f)));
		}

		if (filtered.empty()) {
			drawNoResults();
			return;
		}
		drawEntryTable();
	}

	void drawNoResults()
	{
		std::string title, hint;
		const char* action = nullptr;
		if (searchLower.empty() && onlyModified_) {
			title = u8"沒有已修改的條目";
			hint = u8"改過的翻譯會在這裡列出，儲存或復原後就不再算已修改。";
			action = u8"顯示全部條目";
		} else {
			title = u8"找不到「" + std::string(searchBuf_) + u8"」";
			hint = u8"試試中文或較短的關鍵字";
			if (fileFilter != 0) hint += u8"；資料範圍目前只看 " + model.files[(size_t)fileFilter - 1].name + u8"。";
			else if (onlyModified_) hint += u8"；目前只看已修改的條目。";
			else hint += u8"。";
			if (fileFilter != 0 || onlyModified_) action = u8"改成全部檔案";
		}
		ImGui::Dummy(ImVec2(0, D(8.0f)));
		if (PobUi::EmptyState("##tenores", PobIcon::Search, title.c_str(), hint.c_str(), action)) {
			fileFilter = 0;
			onlyModified_ = false;
			rebuildFilter();
		}
	}

	// Table chrome shared by both pages.
	void pushTableStyle()
	{
		ImGui::PushStyleColor(ImGuiCol_TableHeaderBg, PobUi::TokV4(Tok::Surface1));
		ImGui::PushStyleColor(ImGuiCol_TableBorderLight, PobUi::TokV4(Tok::BorderSubtle));
		ImGui::PushStyleColor(ImGuiCol_TableBorderStrong, PobUi::TokV4(Tok::Border));
		ImGui::PushStyleColor(ImGuiCol_TableRowBg, ImVec4(0, 0, 0, 0));
		ImGui::PushStyleColor(ImGuiCol_TableRowBgAlt, ImVec4(0, 0, 0, 0));
	}
	void popTableStyle() { ImGui::PopStyleColor(5); }

	void headerRow(int cols, const bool* rightAlign)
	{
		ImGui::TableNextRow(ImGuiTableRowFlags_Headers, std::floor(D(30.0f)));
		for (int c = 0; c < cols; c++) {
			if (!ImGui::TableSetColumnIndex(c)) continue;
			const char* label = ImGui::TableGetColumnName(c);
			const ImVec2 p = ImGui::GetCursorScreenPos();
			const float cellW = ImGui::GetContentRegionAvail().x;
			ImGui::PushID(c);
			ImGui::TableHeader("##hdr");
			ImGui::PopID();
			if (label && label[0] && label[0] != '#') {
				const float tw = SmallSize(label).x;
				const float hx = (rightAlign && rightAlign[c]) ? p.x + cellW - tw : p.x;
				SmallAt(ImGui::GetWindowDrawList(), ImVec2(hx, p.y + (D(30.0f) - SmallPx()) * 0.5f - D(2.0f)),
				        Tok::TextMuted, label);
			}
		}
	}

	// The in-cell box (te.css .tin): no frame until hovered or focused.
	static void pushCellInput()
	{
		ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
		ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, PobUi::TokV4(Tok::Surface2));
		ImGui::PushStyleColor(ImGuiCol_FrameBgActive, PobUi::TokV4(Tok::Surface2));
		ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0));   // drawn by cellInputBorder on hover
	}
	static void popCellInput() { ImGui::PopStyleColor(4); }
	static void cellInputBorder()
	{
		if (ImGui::IsItemHovered() || ImGui::IsItemActive())
			ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
			                                    ImGui::IsItemActive() ? Tok::Accent : Tok::Border, D(4.0f), 0, 1.0f);
	}

	// Hint-size text centred in a row of height rowInner.
	static void cellHint(const char* s, float rowInner)
	{
		ImGui::SetCursorPosY(ImGui::GetCursorPosY() + std::floor((rowInner - SmallPx()) * 0.5f));
		PobUi::Hint(s);
	}

	// A square icon button the height of a row (⤢).
	static bool iconButton(const char* id, const char* icon, const char* fallback, float size)
	{
		const ImVec2 p = ImGui::GetCursorScreenPos();
		const bool click = ImGui::InvisibleButton(id, ImVec2(size, size));
		const bool hov = ImGui::IsItemHovered();
		ImDrawList* dl = ImGui::GetWindowDrawList();
		if (hov) dl->AddRectFilled(p, ImVec2(p.x + size, p.y + size), Tok::Surface2, D(4.0f));
		const std::uint32_t col = hov ? Tok::Text : Tok::AccentText;
		if (PobUi::Fonts().icons) {
			const float px = std::floor(size * 0.6f);
			PobUi::IconAt(dl, ImVec2(p.x + (size - PobUi::IconWidth(icon, px)) * 0.5f, p.y + (size - px) * 0.5f), icon, col, px);
		} else {
			const ImVec2 ts = SmallSize(fallback);
			SmallAt(dl, ImVec2(p.x + (size - ts.x) * 0.5f, p.y + (size - ts.y) * 0.5f), col, fallback);
		}
		if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
		return click;
	}

	void drawEntryTable()
	{
		const float rowInner = std::floor(D(30.0f));
		const ImVec2 cellPad(D(8.0f), D(3.0f));
		ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, cellPad);
		// every item in a row is exactly rowInner tall: the clipper sizes the whole
		// scroll range from ONE row's height, so a taller row strands the tail
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
		                    ImVec2(D(8.0f), std::max(1.0f, std::floor((rowInner - ImGui::GetFontSize()) * 0.5f))));
		pushTableStyle();
		const ImGuiTableFlags tflags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerH |
		                               ImGuiTableFlags_BordersOuterH | ImGuiTableFlags_Resizable;
		if (ImGui::BeginTable("##entries", 4, tflags, ImVec2(0, 0))) {
			ImGui::TableSetupColumn(u8"來源", ImGuiTableColumnFlags_WidthFixed, std::floor(D(130.0f)));
			ImGui::TableSetupColumn(u8"Key（英文，與 POB 完全相同）", ImGuiTableColumnFlags_WidthStretch, 0.44f);
			ImGui::TableSetupColumn(u8"翻譯", ImGuiTableColumnFlags_WidthStretch, 0.56f);
			ImGui::TableSetupColumn("##act", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoResize, rowInner);
			ImGui::TableSetupScrollFreeze(0, 1);
			headerRow(4, nullptr);

			const float rowH = rowInner + cellPad.y * 2.0f;
			ImGuiListClipper clipper;
			clipper.Begin((int)filtered.size(), rowH);
			while (clipper.Step()) {
				for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++) {
					if (row >= (int)filtered.size()) break;   // an undo can shrink the list mid-frame
					const size_t ei = filtered[(size_t)row];
					if (ei >= model.entries.size()) break;
					ImGui::TableNextRow(0, rowH);
					ImGui::PushID((int)ei);
					drawEntryRow(ei, rowInner, rowH, cellPad);
					ImGui::PopID();
				}
			}
			// One empty row of slack after the last entry, outside the clipper: the
			// final row can then be scrolled clear of the bottom edge instead of
			// sitting flush against it, which reads as "there is more below".
			ImGui::TableNextRow(0, rowH);
			ImGui::TableSetColumnIndex(0);
			ImGui::Dummy(ImVec2(0, rowInner));
			ImGui::EndTable();
		}
		popTableStyle();
		ImGui::PopStyleVar(2);
	}

	void drawEntryRow(size_t ei, float rowInner, float rowH, const ImVec2& cellPad)
	{
		EditorEntry& e = model.entries[ei];
		ImDrawList* dl = ImGui::GetWindowDrawList();

		ImGui::TableSetColumnIndex(0);
		{
			const ImVec2 p = ImGui::GetCursorScreenPos();
			if (e.edited) {
				// left accent bar = not saved yet (te.css .mod)
				const ImVec2 a(p.x - cellPad.x, p.y - cellPad.y);
				dl->PushClipRect(a, ImVec2(a.x + D(3.0f), a.y + rowH), false);
				dl->AddRectFilled(a, ImVec2(a.x + D(3.0f), a.y + rowH), Tok::Accent);
				dl->PopClipRect();
			}
			const std::string& fname = model.files[(size_t)e.fileIdx].name;
			const BadgeHue hue = BadgeColor(fname);
			const float bh = std::floor(D(20.0f));
			PillAt(dl, ImVec2(p.x, p.y + std::floor((rowInner - bh) * 0.5f)), bh, hue.bg, hue.fg, fname.c_str());
			ImGui::Dummy(ImVec2(PillW(fname.c_str()), rowInner));
			if (ImGui::IsItemHovered()) {
				const EditorFile& f = model.files[(size_t)e.fileIdx];
				PobUi::Tooltip(f.order >= 0 ? (u8"載入順序第 " + std::to_string(f.order + 1) + u8" 個").c_str()
				                            : u8"未列入載入順序，不會被讀取");
			}
		}

		ImGui::TableSetColumnIndex(1);
		{
			int more = 0;
			const std::string line = first_line(e.key, &more);
			ImGui::BeginGroup();
			ImGui::AlignTextToFramePadding();
			if (has_pob_color(line)) TextPobColored(line, PobUi::TokV4(Tok::Text));
			else ImGui::TextUnformatted(line.c_str());
			if (more > 0 || has_pob_color(e.key)) {
				std::string tag = more > 0 ? u8"（另有 " + std::to_string(more) + u8" 行）" : std::string();
				if (has_pob_color(e.key)) tag += u8" 含色碼";
				ImGui::SameLine(0, D(6.0f));
				ImGui::PushFont(SmallFont());
				ImGui::PushStyleColor(ImGuiCol_Text, PobUi::TokV4(Tok::TextMuted));
				ImGui::TextUnformatted(tag.c_str());
				ImGui::PopStyleColor();
				ImGui::PopFont();
			}
			ImGui::EndGroup();
			if (ImGui::IsItemHovered()) {
				ImGui::BeginTooltip();
				ImGui::PushTextWrapPos(D(500.0f));
				ImGui::TextUnformatted(e.key.c_str());
				ImGui::PopTextWrapPos();
				ImGui::EndTooltip();
			}
		}

		ImGui::TableSetColumnIndex(2);
		const bool multi = e.value.find('\n') != std::string::npos;
		bool openHere = false;
		if (multi) {
			// A one-line box cannot show a line break at all: the cell shows the
			// first line and opens the multi-line editor.
			const ImVec2 p = ImGui::GetCursorScreenPos();
			const float w = ImGui::GetContentRegionAvail().x;
			if (ImGui::InvisibleButton("##mv", ImVec2(w, rowInner))) openHere = true;
			const bool hov = ImGui::IsItemHovered();
			if (hov) {
				dl->AddRectFilled(p, ImVec2(p.x + w, p.y + rowInner), Tok::Surface2, D(4.0f));
				dl->AddRect(p, ImVec2(p.x + w, p.y + rowInner), Tok::Border, D(4.0f), 0, 1.0f);
				PobUi::Tooltip(u8"有換行：按一下開多行編輯");
			}
			const std::string shown = first_line(e.value, nullptr) + u8" …";
			dl->PushClipRect(p, ImVec2(p.x + w, p.y + rowInner), true);
			dl->AddText(ImVec2(p.x + D(8.0f), p.y + (rowInner - ImGui::GetFontSize()) * 0.5f), Tok::Text, shown.c_str());
			dl->PopClipRect();
		} else {
			ImGui::SetNextItemWidth(-FLT_MIN);
			pushCellInput();
			if (ImGui::InputText("##v", &e.value)) RefreshEdited(model, ei);
			popCellInput();
			if (ImGui::IsItemActivated()) beginInlineEdit(ei);
			if (ImGui::IsItemActive()) editSeen_ = true;
			if (ImGui::IsItemDeactivated() && editActive_) finishInlineEdit();
			cellInputBorder();
			// The box keeps the raw escapes so they can be edited; what POB will
			// draw is one hover away (a second line would break the clipper's
			// uniform row height).
			if (ImGui::IsItemHovered() && !ImGui::IsItemActive() && has_pob_color(e.value)) {
				ImGui::BeginTooltip();
				PobUi::Hint(u8"在 POB 裡的樣子：");
				TextPobColored(e.value, kPobPalette[7]);
				ImGui::EndTooltip();
			}
		}

		ImGui::TableSetColumnIndex(3);
		if (multi || NeedsExpandedEditor(e) || has_pob_color(e.value) || has_pob_color(e.key)) {
			if (iconButton("##big", PobIcon::Maximize, "...", rowInner)) openHere = true;
			if (ImGui::IsItemHovered()) PobUi::Tooltip(u8"多行編輯：色碼、換行與預覽");
		} else {
			ImGui::Dummy(ImVec2(rowInner, rowInner));
		}
		if (openHere) openBigEditor(ei);
	}

	// ---- page: missing strings ----------------------------------------------------
	void drawMissing()
	{
		drawSaveFailBanner();
		const float H = PobUi::ControlH();
		const float avail = ImGui::GetContentRegionAvail().x;
		{
			const ImVec2 rp = ImGui::GetCursorScreenPos();
			auto at = [&](float x, float h) { ImGui::SetCursorScreenPos(ImVec2(x, rp.y + std::floor((H - h) * 0.5f))); };
			float x = rp.x;
			at(x, H);
			if (PobUi::Button(u8"重新掃描", PobUi::BtnKind::Secondary, PobUi::BtnSize::Md, PobIcon::Refresh)) runScan();
			x = ImGui::GetItemRectMax().x + D(10.0f);
			std::string info = u8"讀取 POB 執行時記下的未翻譯字串（translate_misses.log";
			const std::string wt = filetime_text(missLogWrite_);
			if (missLogFound && !wt.empty()) info += u8"，" + wt + u8" 寫入";
			info += u8"）";
			if (missScanned) info += u8" · 上次掃描 " + missScanAt_;

			// right: switch + REV | fill all
			int ready = 0;
			for (size_t i = 0; i < misses.size(); i++)
				if (!misses[i].reverse && !missRows_[i].filled && missRows_[i].target >= 0 && !missRows_[i].trans.empty())
					ready++;
			const std::string fillLbl = u8"補上已填寫的 " + std::to_string(ready) + u8" 筆";
			const float fillW = PobUi::ButtonWidth(fillLbl.c_str(), PobUi::BtnSize::Sm);
			const char* swLbl = u8"顯示反查失敗";
			const float swW = PobUi::SwitchWidth() + D(8.0f) + SmallSize(swLbl).x + D(6.0f) + PillW("REV") + D(4.0f) +
			                  D(18.0f);
			const float rightX = rp.x + avail - fillW - D(12.0f) - swW;
			at(x, SmallPx());
			ImGui::PushClipRect(ImVec2(x, rp.y), ImVec2(std::max(x, rightX - D(8.0f)), rp.y + H), true);
			PobUi::Hint(info.c_str());
			ImGui::PopClipRect();
			if (ImGui::IsItemHovered()) PobUi::Tooltip(narrow(exeDir + L"translate_misses.log").c_str());

			float sx = std::max(rightX, ImGui::GetItemRectMax().x + D(8.0f));
			at(sx, H);
			PobUi::Switch("##terev", &missShowReverse_);
			sx += PobUi::SwitchWidth() + D(8.0f);
			at(sx, SmallPx());
			PobUi::Hint(swLbl);
			if (ImGui::IsItemClicked()) missShowReverse_ = !missShowReverse_;
			sx = ImGui::GetItemRectMax().x + D(6.0f);
			{
				const float bh = std::floor(D(18.0f));
				PillAt(ImGui::GetWindowDrawList(), ImVec2(sx, rp.y + std::floor((H - bh) * 0.5f)), bh, Tok::WarningSoft,
				       Tok::Warning, "REV");
				sx += PillW("REV") + D(4.0f);
			}
			at(sx, SmallPx());
			PobUi::InfoTip(u8"REV＝從遊戲複製物品、貼進 POB 時，含中文卻轉不回英文的行（記錄檔裡的 REV、FLAVOUR、"
			               u8"PROPERTY）。它是遊戲的中文原文，不是字典的英文 key，所以不能直接補成新條目；要修的是"
			               u8"對應條目的翻譯，讓它和遊戲文字一字不差。按「搜尋」到翻譯條目裡找。");
			at(rp.x + avail - fillW, std::floor(D(28.0f)));
			bool reshaped = false;
			if (PobUi::Button(fillLbl.c_str(), PobUi::BtnKind::Secondary, PobUi::BtnSize::Sm, nullptr, 0.0f, ready > 0) &&
			    TransEd::FillAllMisses(model, undo_, misses, missRows_, &reshaped) > 0)
				rebuildFilter();
			ImGui::SetCursorScreenPos(rp);
			ImGui::Dummy(ImVec2(avail, H));
		}
		ImGui::Dummy(ImVec2(0, D(4.0f)));
		{
			const ImVec2 lp = ImGui::GetCursorScreenPos();
			ImGui::GetWindowDrawList()->AddLine(lp, ImVec2(lp.x + avail, lp.y), Tok::BorderSubtle, 1.0f);
			ImGui::Dummy(ImVec2(0, D(8.0f)));
		}

		if (!missLogFound) {
			PobUi::EmptyState("##tenolog", PobIcon::FileText, u8"還沒有缺漏記錄",
			                  u8"先開一次POB、在各頁操作一下，POB會記下沒翻譯到的字串，再回來按「重新掃描」。");
			return;
		}
		std::vector<int> visible;
		for (size_t i = 0; i < misses.size(); i++)
			if (missShowReverse_ || !misses[i].reverse) visible.push_back((int)i);
		if (visible.empty()) {
			const std::string hint = u8"記錄檔裡的 " + group_digits(missLogged_) +
			                         u8" 筆字串都已經在字典裡。之後在POB看到英文，回來按「重新掃描」。";
			PobUi::EmptyState("##tenomiss", PobIcon::CircleCheck, u8"沒有缺漏", hint.c_str());
			return;
		}
		{
			const std::string line = group_digits(missVisibleUnfilled()) + u8" 筆未翻譯（記錄檔共 " +
			                         group_digits(missLogged_) + u8" 筆，其餘已有翻譯）· 填好翻譯並選檔案後按「補上」，最後再「儲存」";
			PobUi::Hint(line.c_str());
			ImGui::Dummy(ImVec2(0, D(4.0f)));
		}

		const float rowInner = std::floor(D(30.0f));
		const ImVec2 cellPad(D(8.0f), D(3.0f));
		ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, cellPad);
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
		                    ImVec2(D(8.0f), std::max(1.0f, std::floor((rowInner - ImGui::GetFontSize()) * 0.5f))));
		pushTableStyle();
		const ImGuiTableFlags mflags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerH |
		                               ImGuiTableFlags_BordersOuterH | ImGuiTableFlags_Resizable;
		int fillIdx = -1, withdrawIdx = -1, searchIdx = -1;
		if (ImGui::BeginTable("##misses", 4, mflags, ImVec2(0, 0))) {
			ImGui::TableSetupColumn(u8"未翻譯字串", ImGuiTableColumnFlags_WidthStretch, 0.50f);
			ImGui::TableSetupColumn(u8"寫入檔案", ImGuiTableColumnFlags_WidthFixed, std::floor(D(220.0f)));
			ImGui::TableSetupColumn(u8"翻譯", ImGuiTableColumnFlags_WidthStretch, 0.40f);
			ImGui::TableSetupColumn(u8"動作", ImGuiTableColumnFlags_WidthFixed, std::floor(D(80.0f)));
			ImGui::TableSetupScrollFreeze(0, 1);
			const bool right[4] = { false, false, false, true };
			headerRow(4, right);

			const float rowH = rowInner + cellPad.y * 2.0f;
			// Virtualised like the entries table: a long log is thousands of rows,
			// each with a picker and a text box.
			ImGuiListClipper clipper;
			clipper.Begin((int)visible.size(), rowH);
			while (clipper.Step()) {
				for (int vr = clipper.DisplayStart; vr < clipper.DisplayEnd; vr++) {
					const int i = visible[(size_t)vr];
					const MissEntry& m = misses[(size_t)i];
					TransEd::MissRow& r = missRows_[(size_t)i];
					ImGui::TableNextRow(0, rowH);
					ImGui::PushID(i);
					if (r.filled) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.55f);

					ImGui::TableSetColumnIndex(0);
					{
						// text, clipped so the tags after it always stay visible
						const ImVec2 p = ImGui::GetCursorScreenPos();
						const float cw = ImGui::GetContentRegionAvail().x;
						const float gap = D(6.0f);
						const float filledW = r.filled ? PobUi::PillWidth(u8"已補上") + gap : 0.0f;
						const float revW = m.reverse ? PillW("REV") + gap : 0.0f;
						const std::string text = OneLineForCell(m.text);
						const float textW = std::min(ImGui::CalcTextSize(strip_pob_colours(text).c_str()).x,
						                             std::max(0.0f, cw - filledW - revW));
						ImGui::PushClipRect(p, ImVec2(p.x + textW, p.y + rowInner), true);
						ImGui::AlignTextToFramePadding();
						if (has_pob_color(text)) TextPobColored(text, PobUi::TokV4(Tok::Text));
						else ImGui::TextUnformatted(text.c_str());
						ImGui::PopClipRect();
						float x = p.x + textW + gap;
						if (m.reverse) {
							const float bh = std::floor(D(18.0f));
							PillAt(ImGui::GetWindowDrawList(), ImVec2(x, p.y + std::floor((rowInner - bh) * 0.5f)), bh,
							       Tok::WarningSoft, Tok::Warning, "REV");
							x += revW;
						}
						if (r.filled) {
							ImGui::PopStyleVar();   // the pill at full strength
							ImGui::SetCursorScreenPos(ImVec2(x, p.y + std::floor((rowInner - D(24.0f)) * 0.5f)));
							PobUi::StatusPill(PobUi::Tone::Ok, u8"已補上");
							ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.55f);
						}
						ImGui::SetCursorScreenPos(p);
						ImGui::Dummy(ImVec2(cw, rowInner));
						if (ImGui::IsItemHovered())
							PobUi::Tooltip(m.reverse ? (m.text + u8"\n\n記錄檔標籤：" + m.tag + u8"（貼上物品時反查失敗）").c_str()
							                         : m.text.c_str());
					}

					ImGui::TableSetColumnIndex(1);
					if (m.reverse) {
						cellHint(u8"—", rowInner);
					} else if (r.filled) {
						cellHint(r.filledFile >= 0 ? model.files[(size_t)r.filledFile].name.c_str() : "", rowInner);
					} else {
						const bool testOpen = testOpenMissSelect_;   // the first picker on screen
						testOpenMissSelect_ = false;
						FileSelect("##tgt", model, &r.target, ImGui::GetContentRegionAvail().x, nullptr, testOpen, true);
					}

					ImGui::TableSetColumnIndex(2);
					if (m.reverse) {
						cellHint(u8"貼上物品時反查失敗：要改的是對應條目的翻譯", rowInner);
					} else if (r.filled) {
						ImGui::AlignTextToFramePadding();
						ImGui::TextUnformatted(OneLineForCell(r.trans).c_str());
					} else {
						ImGui::SetNextItemWidth(-FLT_MIN);
						pushCellInput();
						ImGui::InputTextWithHint("##mt", u8"輸入翻譯", &r.trans);
						popCellInput();
						cellInputBorder();
					}

					ImGui::TableSetColumnIndex(3);
					{
						const char* lbl = m.reverse ? u8"搜尋" : r.filled ? u8"撤回" : u8"補上";
						const float bw = PobUi::ButtonWidth(lbl, PobUi::BtnSize::Sm);
						const float cw = ImGui::GetContentRegionAvail().x;
						ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, cw - bw));
						ImGui::SetCursorPosY(ImGui::GetCursorPosY() + std::floor((rowInner - D(28.0f)) * 0.5f));
						if (m.reverse) {
							if (PobUi::Button(lbl, PobUi::BtnKind::Secondary, PobUi::BtnSize::Sm)) searchIdx = i;
							if (ImGui::IsItemHovered()) PobUi::Tooltip(u8"到翻譯條目搜尋這一行（色碼已去掉）");
						} else if (r.filled) {
							if (PobUi::Button(lbl, PobUi::BtnKind::Ghost, PobUi::BtnSize::Sm)) withdrawIdx = i;
						} else {
							if (PobUi::Button(lbl, PobUi::BtnKind::Secondary, PobUi::BtnSize::Sm, nullptr, 0.0f,
							                  r.target >= 0 && !r.trans.empty()))
								fillIdx = i;
						}
					}
					if (r.filled) ImGui::PopStyleVar();
					ImGui::PopID();
				}
			}
			ImGui::TableNextRow(0, rowH);
			ImGui::TableSetColumnIndex(0);
			ImGui::Dummy(ImVec2(0, rowInner));
			ImGui::EndTable();
		}
		popTableStyle();
		ImGui::PopStyleVar(2);

		bool reshaped = false;
		if (fillIdx >= 0 && TransEd::FillMiss(model, undo_, misses, missRows_, fillIdx, &reshaped)) rebuildFilter();
		if (withdrawIdx >= 0 && TransEd::WithdrawMiss(model, undo_, misses, missRows_, withdrawIdx, &reshaped))
			rebuildFilter();
		if (searchIdx >= 0) {
			setSearch(strip_pob_colours(misses[(size_t)searchIdx].text));
			fileFilter = 0;
			onlyModified_ = false;
			view = 0;
			rebuildFilter();
		}
	}

	// ---- no data for this language ----------------------------------------------
	void drawNoData()
	{
		const std::string name = localeLabel(curLocale);
		const std::string title = (name == curLocale ? curLocale : name + u8"（" + curLocale + u8"）") + u8" 在 " + kGameNames[gi] +
		                          u8" 字典裡沒有翻譯資料";
		const std::string hint = u8"要新增這個語系，先在啟動器「設定 → 翻譯資料」準備資料夾；或從上面改選其他語言。";
		ImGui::Dummy(ImVec2(0, D(24.0f)));
		if (PobUi::EmptyStateEx("##tenodata", PobIcon::Languages, title.c_str(), hint.c_str(), u8"前往設定", nullptr,
		                        narrow(model.dataDir).c_str()) == 1)
			goSettings();
	}

	// ---- footer -----------------------------------------------------------------
	void drawFooter()
	{
		const float avail = ImGui::GetContentRegionAvail().x;
		ImDrawList* dl = ImGui::GetWindowDrawList();
		const ImVec2 p = ImGui::GetCursorScreenPos();
		const float h = std::floor(SmallPx() + D(14.0f));
		dl->AddLine(p, ImVec2(p.x + avail, p.y), Tok::BorderSubtle, 1.0f);
		const float ty = p.y + std::floor((h - SmallPx()) * 0.5f);
		std::string left, right;
		const int dirtyE = DirtyEntryCount(model), dirtyF = DirtyCount(model);
		if (!model.localeExists) {
			// the page says it all (EmptyState, with the folder)
		} else if (view == 0) {
			left = u8"左側色條＝尚未儲存 · 點翻譯直接修改 · 右側按鈕開多行編輯 · Ctrl+Z 復原";
			right = u8"顯示 " + group_digits((long long)filtered.size()) + " / " + group_digits((long long)model.entries.size()) +
			        u8" 筆 · " + std::to_string(model.files.size()) + u8" 個檔案";
			if (dirtyE > 0)
				right += u8" · 未儲存 " + group_digits(dirtyE) + u8" 筆（" + std::to_string(dirtyF) + u8" 個檔案）";
		} else {
			left = u8"REV＝貼上物品時，含中文卻轉不回英文的行；要修的是對應條目的翻譯";
			int shown = 0, filled = 0;
			for (size_t i = 0; i < misses.size(); i++) {
				if (missShowReverse_ || !misses[i].reverse) shown++;
				if (missRows_[i].filled) filled++;
			}
			right = u8"顯示 " + group_digits(shown) + u8" · 已補上 " + std::to_string(filled) + u8" · 未儲存 " +
			        group_digits(dirtyE) + u8" 筆";
		}
		const float rw = right.empty() ? 0.0f : SmallSize(right.c_str()).x;
		dl->PushClipRect(p, ImVec2(p.x + avail - rw - D(16.0f), p.y + h), true);
		SmallAt(dl, ImVec2(p.x + D(4.0f), ty), Tok::TextMuted, left.c_str());
		dl->PopClipRect();
		if (!right.empty()) SmallAt(dl, ImVec2(p.x + avail - rw - D(4.0f), ty), Tok::TextMuted, right.c_str());
		ImGui::Dummy(ImVec2(avail, h));
	}

	// ---- add dialog -------------------------------------------------------------
	void openAddDialog()
	{
		addKey_.clear();
		addVal_.clear();
		if (addTarget_ < 0 || addTarget_ >= (int)model.files.size() || model.files[(size_t)addTarget_].order < 0) {
			const int ui = FindFileIdx(model, "ui.json");
			addTarget_ = (ui >= 0 && model.files[(size_t)ui].order >= 0) ? ui : -1;
			if (addTarget_ < 0)
				for (int fi : FileIdxInLoadOrder(model)) if (model.files[fi].order >= 0) addTarget_ = fi;
		}
		addHintFor_.clear();
		openAdd_ = true;
		addFocus_ = true;
	}

	void drawAddDialog()
	{
		if (!PobUi::BeginDialog("##teadd", &openAdd_, u8"新增翻譯條目", nullptr, nullptr, 560.0f)) return;
		const float inner = std::floor(D(560.0f) - D(48.0f));
		PobUi::Hint(u8"Key（英文，要和 POB 畫面上的原文完全相同）");
		PobUi::PushControlFrame();
		ImGui::SetNextItemWidth(inner);
		if (addFocus_) { ImGui::SetKeyboardFocusHere(); addFocus_ = false; }
		ImGui::InputText("##addkey", &addKey_);
		ImGui::Dummy(ImVec2(0, D(4.0f)));
		PobUi::Hint(u8"翻譯");
		ImGui::SetNextItemWidth(inner);
		ImGui::InputText("##addval", &addVal_);
		PobUi::PopControlFrame();
		ImGui::Dummy(ImVec2(0, D(4.0f)));
		PobUi::Hint(u8"寫入哪個檔案");
		// cache: a linear scan of 110k rows per frame is wasted work
		if (addHintFor_ != addKey_ || addHintTarget_ != addTarget_) {
			addHint_ = TransEd::ClassifyAdd(model, addKey_, addTarget_);
			addHintFor_ = addKey_;
			addHintTarget_ = addTarget_;
		}
		FileSelect("##addtarget", model, &addTarget_, inner, &addHint_.owners, testOpenAddSelect_);
		testOpenAddSelect_ = false;
		ImGui::Dummy(ImVec2(0, D(6.0f)));

		const std::string tName = addTarget_ >= 0 ? model.files[(size_t)addTarget_].name : std::string();
		std::string others;
		for (int f : addHint_.owners)
			if (f != addTarget_) others += (others.empty() ? "" : u8"、") + model.files[(size_t)f].name;
		switch (addHint_.kind) {
			case TransEd::AddKind::New:
				PobUi::Banner("##addnew", PobUi::BannerTone::Info, PobIcon::Plus, u8"新條目，目前沒有任何檔案有這個 key。",
				              nullptr, false, nullptr, false, true, inner);
				break;
			case TransEd::AddKind::TargetWins:
				PobUi::Banner("##addwins", PobUi::BannerTone::Info, PobIcon::Info,
				              (others + u8" 也有這個 key；POB 會採用你選的 " + tName + u8"（較晚載入）。").c_str(), nullptr,
				              false, nullptr, false, true, inner);
				break;
			case TransEd::AddKind::OverwriteSame:
				PobUi::Banner("##addover", PobUi::BannerTone::Warn, PobIcon::TriangleAlert,
				              (tName + u8" 已經有這個 key，會把「" + addHint_.oldValue + u8"」換成新的翻譯。").c_str(),
				              nullptr, false, nullptr, false, true, inner);
				break;
			case TransEd::AddKind::Shadowed: {
				const std::string w = model.files[(size_t)addHint_.winner].name;
				if (PobUi::Banner("##addshadow", PobUi::BannerTone::Bad, PobIcon::CircleX,
				                  (u8"寫進 " + tName + u8" 不會生效：較晚載入的 " + w + u8" 也有這個 key，POB 會用那一份。").c_str(),
				                  nullptr, false, (u8"改寫入 " + w).c_str(), false, true, inner) == PobUi::BannerResult::Action)
					addTarget_ = addHint_.winner;
				break;
			}
			case TransEd::AddKind::Whitespace:
				if (PobUi::Banner("##addws", PobUi::BannerTone::Warn, PobIcon::TriangleAlert,
				                  u8"Key 前後有空白：只有 POB 畫面上的原文也帶著同樣的空白才比對得到，通常是複製時多帶的。",
				                  nullptr, false, u8"去掉空白", false, true, inner) == PobUi::BannerResult::Action)
					addKey_ = TransEd::TrimKey(addKey_);
				break;
			default:
				PobUi::Hint(u8"輸入 key 後，這裡會說明寫進所選的檔案會不會生效。", inner);
				break;
		}

		const bool canAdd = !addKey_.empty() && !addVal_.empty() && addTarget_ >= 0 &&
		                    model.files[(size_t)addTarget_].order >= 0;
		const PobUi::DialogResult r = PobUi::DialogButtons(
			u8"取消", nullptr, nullptr, addHint_.kind == TransEd::AddKind::OverwriteSame ? u8"覆蓋" : u8"新增", canAdd, true);
		if (r == PobUi::DialogResult::Primary && canAdd) {
			const std::string key = addKey_, tname = model.files[(size_t)addTarget_].name;
			TransEd::SetWithUndo(model, undo_, addTarget_, key, addVal_);
			// show the new row
			setSearch(key);
			fileFilter = 0;
			onlyModified_ = false;
			rebuildFilter();
			toast(u8"已寫入 " + tname + u8"（尚未儲存）", PobUi::Tone::Ok);
		}
		PobUi::EndDialog();
	}

	// ---- multi-line editor --------------------------------------------------------
	void openBigEditor(size_t ei)
	{
		if (editActive_) finishInlineEdit();
		bigFile_ = model.entries[ei].fileIdx;
		bigOpenFor_ = model.entries[ei].key;
		bigText_ = model.entries[ei].value;
		bigCursor_ = (int)bigText_.size();
		bigSetCursor_ = -1;
		openBig_ = true;
	}

	static int bigCallback(ImGuiInputTextCallbackData* d)
	{
		auto* self = (TranslationEditorPanel*)d->UserData;
		if (self->bigSetCursor_ >= 0 && self->bigSetCursor_ <= d->BufTextLen) {
			d->CursorPos = d->SelectionStart = d->SelectionEnd = self->bigSetCursor_;
			self->bigSetCursor_ = -1;
		}
		self->bigCursor_ = d->CursorPos;
		return 0;
	}

	void drawBigDialog()
	{
		if (!PobUi::BeginDialog("##tebig", &openBig_, u8"編輯翻譯", nullptr, nullptr, 760.0f)) return;
		const float inner = std::floor(D(760.0f) - D(48.0f));
		const long long at = bigFile_ >= 0 ? FindEntry(model, bigFile_, bigOpenFor_) : -1;
		if (at < 0) {
			// a reload or an undo moved the ground under the dialog
			ImGui::CloseCurrentPopup();
			PobUi::EndDialog();
			return;
		}
		const EditorEntry& e = model.entries[(size_t)at];
		ImDrawList* dl = ImGui::GetWindowDrawList();

		PobUi::Hint(u8"Key（英文原文，不能改）");
		{
			const ImVec2 p = ImGui::GetCursorScreenPos();
			const float pad = D(10.0f);
			ImFont* f = ImGui::GetFont();
			const ImVec2 ts = f->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, inner - pad * 2.0f, e.key.c_str());
			const float boxH = std::min(ts.y, ImGui::GetFontSize() * 6.0f) + D(12.0f);
			dl->AddRectFilled(p, ImVec2(p.x + inner, p.y + boxH), Tok::Surface1, D(4.0f));
			dl->AddRect(p, ImVec2(p.x + inner, p.y + boxH), Tok::BorderSubtle, D(4.0f), 0, 1.0f);
			dl->PushClipRect(p, ImVec2(p.x + inner, p.y + boxH), true);
			dl->AddText(f, ImGui::GetFontSize(), ImVec2(p.x + pad, p.y + D(6.0f)), Tok::Text, e.key.c_str(), nullptr,
			            inner - pad * 2.0f);
			dl->PopClipRect();
			ImGui::Dummy(ImVec2(inner, boxH));
		}
		PobUi::Hint((u8"來源：" + model.files[(size_t)e.fileIdx].name).c_str());
		ImGui::Dummy(ImVec2(0, D(6.0f)));

		// colour swatches: insert at the caret
		{
			const float H = std::floor(D(24.0f));
			const ImVec2 rp = ImGui::GetCursorScreenPos();
			ImGui::SetCursorScreenPos(ImVec2(rp.x, rp.y + (H - SmallPx()) * 0.5f));
			PobUi::Hint(u8"插入色碼");
			float x = ImGui::GetItemRectMax().x + D(10.0f);
			const float sw = std::floor(D(18.0f));
			for (size_t i = 0; i < sizeof(kSwatches) / sizeof(kSwatches[0]); i++) {
				ImGui::SetCursorScreenPos(ImVec2(x, rp.y + (H - sw) * 0.5f));
				ImGui::PushID((int)i);
				const bool click = ImGui::InvisibleButton("##sw", ImVec2(sw, sw));
				const bool hov = ImGui::IsItemHovered();
				ImGui::PopID();
				const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
				dl->AddRectFilled(a, b, ImGui::ColorConvertFloat4ToU32(hex_colour(kSwatches[i].code)), D(4.0f));
				dl->AddRect(a, b, hov ? Tok::Text : Tok::BorderStrong, D(4.0f), 0, 1.0f);
				if (hov) {
					PobUi::Tooltip(kSwatches[i].name);
					ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
				}
				if (click) {
					const size_t pos = (size_t)std::clamp(bigCursor_, 0, (int)bigText_.size());
					bigText_.insert(pos, kSwatches[i].code);
					bigSetCursor_ = (int)(pos + strlen(kSwatches[i].code));
					bigFocus_ = true;
				}
				x += sw + D(6.0f);
			}
			const char* keys = u8"Enter 換行 · Ctrl+Enter 套用";
			ImGui::SetCursorScreenPos(ImVec2(rp.x + inner - SmallSize(keys).x, rp.y + (H - SmallPx()) * 0.5f));
			PobUi::Hint(keys);
			ImGui::SetCursorScreenPos(rp);
			ImGui::Dummy(ImVec2(inner, H));
		}
		ImGui::Dummy(ImVec2(0, D(4.0f)));

		// Ctrl+Enter applies. Checked before the box processes the key -- a
		// multi-line box would otherwise take it as one more newline.
		const ImGuiIO& io = ImGui::GetIO();
		bool apply = false;
		if (bigActive_ && io.KeyCtrl && (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false))) {
			ImGui::ClearActiveID();
			apply = true;
		}
		if (bigFocus_) { ImGui::SetKeyboardFocusHere(); bigFocus_ = false; }
		PobUi::PushControlFrame();
		ImGui::PushStyleColor(ImGuiCol_FrameBg, PobUi::TokV4(Tok::Bg));
		ImGui::InputTextMultiline("##bigval", &bigText_, ImVec2(inner, std::floor(ImGui::GetFontSize() * 5.0f + D(16.0f))),
		                          ImGuiInputTextFlags_CallbackAlways, &TranslationEditorPanel::bigCallback, this);
		ImGui::PopStyleColor();
		PobUi::PopControlFrame();
		bigActive_ = ImGui::IsItemActive();
		ImGui::Dummy(ImVec2(0, D(6.0f)));

		PobUi::Hint(u8"在 POB 裡的樣子");
		{
			const ImVec2 p = ImGui::GetCursorScreenPos();
			const int lines = std::max(1, TransEd::LineCount(bigText_));
			const float boxH = std::floor(ImGui::GetTextLineHeightWithSpacing() * (float)std::min(lines, 6) + D(14.0f));
			dl->AddRectFilled(p, ImVec2(p.x + inner, p.y + boxH), Tok::Canvas, D(4.0f));
			dl->AddRect(p, ImVec2(p.x + inner, p.y + boxH), Tok::BorderSubtle, D(4.0f), 0, 1.0f);
			ImGui::PushClipRect(p, ImVec2(p.x + inner, p.y + boxH), true);
			ImGui::SetCursorScreenPos(ImVec2(p.x + D(10.0f), p.y + D(7.0f)));
			ImGui::BeginGroup();
			DrawPobPreview(bigText_, kPobPalette[7]);
			ImGui::EndGroup();
			ImGui::PopClipRect();
			ImGui::SetCursorScreenPos(p);
			ImGui::Dummy(ImVec2(inner, boxH));
		}
		ImGui::Dummy(ImVec2(0, D(6.0f)));

		// the three checks
		const TransEd::TextChecks chk = TransEd::CheckTranslation(e.key, bigText_);
		bigChecks_ = describeChecks(chk);
		{
			bool first = true;
			for (const auto& pill : bigChecks_) {
				if (!first) ImGui::SameLine(0, D(8.0f));
				first = false;
				PobUi::StatusPill(pill.first, pill.second.c_str());
			}
		}

		const PobUi::DialogResult r = PobUi::DialogButtons(u8"取消", nullptr, nullptr, u8"套用");
		if (r == PobUi::DialogResult::Primary) apply = true;
		if (apply) {
			if (!chk.AllOk()) {
				// Still allowed -- a translation may reorder lines on purpose -- but
				// asked once more, because a missing {0} shows POB a wrong number.
				openBigConfirm_ = true;
			} else {
				applyBig();
			}
			ImGui::CloseCurrentPopup();
		}
		PobUi::EndDialog();
	}

	std::vector<std::pair<PobUi::Tone, std::string>> describeChecks(const TransEd::TextChecks& c) const
	{
		std::vector<std::pair<PobUi::Tone, std::string>> out;
		if (c.colourOk) {
			out.push_back({ PobUi::Tone::Ok, c.colourKey ? u8"色碼成對" : u8"原文沒有色碼" });
		} else {
			std::string m;
			for (const std::string& s : c.colourMissing) m += (m.empty() ? "" : " ") + s;
			out.push_back({ PobUi::Tone::Bad, u8"色碼少了 " + m });
		}
		const std::string ln = std::to_string(c.linesValue) + " / " + std::to_string(c.linesKey);
		out.push_back({ c.linesOk ? PobUi::Tone::Ok : PobUi::Tone::Warn,
		                (c.linesOk ? u8"行數相同（" : u8"行數不同（") + ln + u8"）" });
		if (c.placeholdersOk) {
			std::string ids;
			for (const std::string& s : c.placeholdersKey) ids += s;
			out.push_back({ PobUi::Tone::Ok, ids.empty() ? u8"沒有佔位符" : ids + u8" 佔位符一致" });
		} else {
			for (const std::string& s : c.placeholdersMissing) out.push_back({ PobUi::Tone::Bad, u8"少了 " + s });
			for (const std::string& s : c.placeholdersExtra) out.push_back({ PobUi::Tone::Bad, u8"多了 " + s });
		}
		return out;
	}

	void applyBig()
	{
		if (bigFile_ < 0) return;
		TransEd::SetWithUndo(model, undo_, bigFile_, bigOpenFor_, bigText_);
		rebuildFilter();
	}

	void drawBigConfirm()
	{
		std::string body;
		for (const auto& pill : bigChecks_)
			if (pill.first != PobUi::Tone::Ok) body += (body.empty() ? "" : u8"、") + pill.second;
		body = u8"沒通過：" + body + u8"。佔位符少了會讓POB顯示錯的數值；行數或色碼不同則可能是刻意的。";
		if (!PobUi::BeginDialog("##tebigconfirm", &openBigConfirm_, u8"檢查沒有全部通過，仍要套用嗎？", body.c_str()))
			return;
		const PobUi::DialogResult r = PobUi::DialogButtons(u8"回去修改", nullptr, nullptr, u8"仍要套用");
		if (r == PobUi::DialogResult::Primary) applyBig();
		if (r == PobUi::DialogResult::Cancel) openBig_ = true;   // back to the editor, text kept
		PobUi::EndDialog();
	}

	// ---- unsaved changes --------------------------------------------------------
	void drawUnsavedDialog()
	{
		if (pending == Pending::None && !ImGui::IsPopupOpen("##teunsaved")) {
			openUnsaved_ = false;
			return;
		}
		std::string files;
		for (const EditorFile& f : model.files) if (f.dirty) files += (files.empty() ? "" : u8"、") + f.name;
		std::string what;
		switch (pending) {
			case Pending::SwitchGame: what = u8"切換到" + std::string(kGameNames[pendingGi]) + u8"字典後"; break;
			case Pending::SwitchLocale: what = u8"切換到" + localeLabel(pendingLocale) + u8"（" + pendingLocale + u8"）後"; break;
			case Pending::Reload: what = u8"重新載入後"; break;
			default: what = u8"關閉編輯器後"; break;
		}
		const std::string title = u8"先儲存 " + std::to_string(DirtyEntryCount(model)) + u8" 筆變更嗎？";
		const std::string body = files + u8" 有修改還沒儲存。不儲存的話，" + what + u8"這些修改會消失。";
		if (!PobUi::BeginDialog("##teunsaved", &openUnsaved_, title.c_str(), body.c_str())) return;
		// Order fixed by the design system: cancel | danger | primary.
		const PobUi::DialogResult r = PobUi::DialogButtons(u8"取消", nullptr, u8"不儲存", u8"儲存");
		PobUi::EndDialog();
		if (r != PobUi::DialogResult::None) resolvePending(r);
	}

	// ---- POBTOOLS_TE_STATE (test aid, with POBTOOLS_TOOL_SHOT) -------------------
	void touchRows(int n)
	{
		// visibly unchanged, but unsaved: a trailing space
		int done = 0;
		for (size_t k = 0; k < filtered.size() && done < n; k += 2) {
			EditorEntry& e = model.entries[filtered[k]];
			if (e.structured || e.value.find('\n') != std::string::npos) continue;
			e.value += " ";
			RefreshEdited(model, filtered[k]);
			done++;
		}
	}

	void applyTestState()
	{
		const std::string& s = testState_;
		if (s.empty()) return;
		if (s == "entries" || s == "unsaved" || s == "savefail" || s == "saved" || s == "external") {
			setSearch("life regeneration");
			rebuildFilter();
			touchRows(3);
			if (s == "external") testExternal_ = true;
			if (s == "unsaved") requestSwitchGame(1);
			if (s == "savefail" || s == "saved") doSave();
		} else if (s.compare(0, 3, "add") == 0) {
			setSearch("life regeneration");
			rebuildFilter();
			openAddDialog();
			addFocus_ = false;
			pickAddCase(s);
			if (s == "addmenu") testOpenAddSelect_ = true;
		} else if (s == "missing" || s == "missingmenu") {
			view = 1;
			missShowReverse_ = true;
			bool hasRev = false;
			for (const MissEntry& m : misses) if (m.reverse) hasRev = true;
			if (!hasRev) {
				// the shipped log has no paste lines; one sample so the row shows
				MissEntry m;
				m.text = u8"穢生 示範之戒";
				m.reverse = true;
				m.tag = "REV";
				misses.insert(misses.begin() + (misses.size() > 2 ? 2 : misses.size()), m);
				missRows_.insert(missRows_.begin() + (missRows_.size() > 2 ? 2 : missRows_.size()), TransEd::MissRow());
			}
			int typed = 0;
			for (size_t i = 0; i < misses.size() && typed < 3; i++) {
				if (misses[i].reverse) continue;
				missRows_[i].trans = typed == 0 ? u8"示範翻譯（已補上）" : u8"示範翻譯";
				if (typed == 0) {
					bool reshaped = false;
					TransEd::FillMiss(model, undo_, misses, missRows_, (int)i, &reshaped);
					// listed last, like the design: a filled row fades in place
				}
				typed++;
			}
			rebuildFilter();
			if (s == "missingmenu") testOpenMissSelect_ = true;
		} else if (s == "edit") {
			long long pick = -1;
			for (size_t i = 0; i < model.entries.size() && pick < 0; i++) {
				const EditorEntry& e = model.entries[i];
				if (!e.structured && has_pob_color(e.key) && TransEd::LineCount(e.key) > 1 &&
				    TransEd::CheckTranslation(e.key, e.value).AllOk())
					pick = (long long)i;
			}
			for (size_t i = 0; i < model.entries.size() && pick < 0; i++)
				if (!model.entries[i].structured && has_pob_color(model.entries[i].key)) pick = (long long)i;
			if (pick >= 0) openBigEditor((size_t)pick);
		} else if (s == "editbad") {
			// two placeholders and two lines; the edit drops {1} and the second line
			long long pick = -1;
			for (size_t i = 0; i < model.entries.size() && pick < 0; i++) {
				const EditorEntry& e = model.entries[i];
				if (!e.structured && e.key.find("{0}") != std::string::npos && e.key.find("{1}") != std::string::npos &&
				    TransEd::LineCount(e.key) > 1 && TransEd::CheckTranslation(e.key, e.value).AllOk())
					pick = (long long)i;
			}
			if (pick >= 0) {
				openBigEditor((size_t)pick);
				std::string& t = bigText_;
				const size_t ph = t.find("{1}");
				if (ph != std::string::npos) t.erase(ph, 3);
				size_t nl = t.find("\\n");
				if (nl == std::string::npos) nl = t.find('\n');
				if (nl != std::string::npos) t.erase(nl);
			}
		} else if (s == "nodata") {
			curLocale = "xx-NONE";
			loadModel();
		} else if (s == "nosearch") {
			const int ui = FindFileIdx(model, "ui.json");
			if (ui >= 0) fileFilter = ui + 1;
			setSearch("reservation efficiency");
			rebuildFilter();
			if (!filtered.empty()) { setSearch("reservation efficiency zz"); rebuildFilter(); }
		} else if (s == "nomisses") {
			view = 1;
			missLogFound = false;
			misses.clear();
			missRows_.clear();
		} else if (s == "menu") {
			setSearch("life regeneration");
			rebuildFilter();
			openMore_ = true;
		} else if (s == "fallback") {
			notice_ = Notice::LocaleFallback;
			noticeArg_ = "en";
			setSearch("life regeneration");
			rebuildFilter();
		}
	}

	// The add dialog's four notices, each with real data from the dictionary.
	void pickAddCase(const std::string& s)
	{
		addVal_ = u8"示範翻譯";
		if (s == "addnew") { addKey_ = "Increases and Reductions to Minion Damage also affect you (PobTools)"; return; }
		if (s == "addws") { addKey_ = " Life Regeneration "; return; }
		std::map<std::string, int> occ;
		for (const EditorEntry& e : model.entries) occ[e.key]++;
		const std::vector<int> order = FileIdxInLoadOrder(model);
		auto listedAfter = [&](int fileOrder, const std::vector<int>& owners) {
			int best = -1;
			for (int fi : order)
				if (model.files[fi].order > fileOrder && std::find(owners.begin(), owners.end(), fi) == owners.end())
					best = fi;
			return best;
		};
		for (const auto& kv : occ) {
			if (kv.first.size() < 12 || kv.first.size() > 60 || has_pob_color(kv.first) ||
			    !((kv.first[0] >= 'A' && kv.first[0] <= 'Z') || (kv.first[0] >= 'a' && kv.first[0] <= 'z')) ||
			    kv.first.find('\n') != std::string::npos || kv.first.find('\\') != std::string::npos)
				continue;
			const std::vector<int> owners = FilesContaining(model, kv.first);
			const int w = WinnerFileIdx(model, kv.first);
			if (w < 0) continue;
			if (s == "add" && kv.second >= 2) {
				const int t = listedAfter(model.files[(size_t)w].order, owners);
				if (t < 0) continue;
				addKey_ = kv.first; addTarget_ = t; break;
			}
			if (s == "addmenu" && kv.second >= 2) {
				addKey_ = kv.first; addTarget_ = w; break;
			}
			if (s == "addover" && kv.second == 1) {
				addKey_ = kv.first; addTarget_ = w; break;
			}
			if (s == "addshadow" && kv.second >= 2) {
				addKey_ = kv.first; addTarget_ = owners.front() == w ? owners.back() : owners.front();
				if (model.files[(size_t)addTarget_].order < 0 || addTarget_ == w) continue;
				break;
			}
		}
	}

	const ToolPanelHost* host_ = nullptr;
	ToolCloseState close_ = ToolCloseState::Open;
	bool saved_ = false;
	bool wantSettings_ = false;

	std::wstring exeDir;
	float scale = 1.0f;

	int gi = 0;
	LauncherConfig editCfg;
	DictDirInfo slotDir[kDictSlotCount];
	std::vector<std::string> locales, localeNames;
	std::string curLocale;
	EditorModel model;
	int view = 0;                    // 0 entries, 1 missing strings

	char searchBuf_[256] = "";
	std::string searchLower;         // cached lowercase
	int fileFilter = 0;              // 0 = all, else file index + 1
	bool onlyModified_ = false;
	std::vector<size_t> filtered;

	Notice notice_ = Notice::None;
	std::string noticeArg_;
	bool builtinClosed_ = false, externalClosed_ = false;
	bool saveFailed_ = false;
	std::vector<std::string> saveFailFiles_;

	TransEd::UndoStack undo_;
	// the inline edit in progress: one undo step per edit session, not per key
	bool editActive_ = false, editSeen_ = false;
	int editFile_ = -1;
	std::string editKey_, editStart_;

	bool missScanned = false;
	bool missLogFound = false;
	bool missShowReverse_ = false;
	int missLogged_ = 0;
	unsigned long long missLogWrite_ = 0;
	std::string missScanAt_;
	std::vector<MissEntry> misses;
	std::vector<TransEd::MissRow> missRows_;

	// add dialog
	bool openAdd_ = false, addFocus_ = false;
	std::string addKey_, addVal_;
	int addTarget_ = -1;
	TransEd::AddHint addHint_;
	std::string addHintFor_;
	int addHintTarget_ = -2;

	// multi-line editor (identified by file + key: indices shift on undo)
	bool openBig_ = false, openBigConfirm_ = false, bigFocus_ = false, bigActive_ = false;
	int bigFile_ = -1;
	std::string bigOpenFor_, bigText_;
	int bigCursor_ = 0, bigSetCursor_ = -1;
	std::vector<std::pair<PobUi::Tone, std::string>> bigChecks_;

	bool openUnsaved_ = false;
	Pending pending = Pending::None;
	int pendingGi = 0;
	std::string pendingLocale;

	bool openMore_ = false, openFolder_ = false;
	ImVec2 moreAnchor_ = ImVec2(0, 0);

	std::string testState_;
	bool testMode_ = false, testExternal_ = false;
	bool testOpenAddSelect_ = false, testOpenMissSelect_ = false;
};

IToolPanel* CreateTranslationEditorPanel()
{
	return new TranslationEditorPanel();
}

// dynamic_cast rather than a virtual on IToolPanel: "did you save?" is peculiar
// to this one tool, and putting it in the interface would invite every other
// panel to grow a meaningless implementation of it.
bool TranslationEditorPanelSaved(IToolPanel* panel)
{
	auto* te = dynamic_cast<TranslationEditorPanel*>(panel);
	return te && te->TakeSavedFlag();
}

bool TranslationEditorPanelWantsSettings(IToolPanel* panel)
{
	auto* te = dynamic_cast<TranslationEditorPanel*>(panel);
	return te && te->TakeSettingsRequest();
}

std::string TranslationEditorTestOp(IToolPanel* panel, const std::string& op)
{
	auto* te = dynamic_cast<TranslationEditorPanel*>(panel);
	return te ? te->TestOp(op) : std::string("not a translation editor");
}

void ShowEditor(const std::wstring& exeDir, const std::wstring& game, const std::wstring& locale)
{
	TranslationEditorPanel panel;
	ToolWindowDesc desc;
	// "PobTools — 翻譯編輯器"
	desc.titleUtf8 = "PobTools \xe2\x80\x94 \xe7\xbf\xbb\xe8\xad\xaf\xe7\xb7\xa8\xe8\xbc\xaf\xe5\x99\xa8";
	desc.defW = 1420;
	desc.defH = 900;
	// Big enough that the taskbar matters.
	desc.clampToWorkArea = true;
	RunToolWindow(panel, desc, exeDir, game, locale);
}
