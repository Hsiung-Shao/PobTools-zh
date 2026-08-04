#include "launcher_ui.h"
#include "editor_util.h"        // EdBrowseForFolder (one folder picker for the app)
#include "launcher_strings.h"
#include "launcher_strings_io.h"
#include "ui_theme.h"
#include "app_version.h"
#include "app_update.h"
#include "changelog.h"
#include "pob_launch.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

#include <GLES2/gl2.h>
#include <GLFW/glfw3.h>
#define IMGUI_DEFINE_MATH_OPERATORS
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <misc/cpp/imgui_stdlib.h> // InputText over std::string (the data-folder field)

#include <string>
#include <vector>

// Tools (filter editor / atlas planner / timeless jewel) run as child
// processes of the same exe (--filter-editor / --atlas / --timeless-jewel)
// so the launcher window stays open. The child reads pob-zh.ini for
// game/locale — callers must SaveLauncherConfig before spawning.
static void SpawnTool(const std::wstring& exeDir, const wchar_t* flag)
{
	std::wstring cmd = L"\"" + exeDir + L"pob-zh.exe\" " + flag;
	std::vector<wchar_t> buf(cmd.begin(), cmd.end());
	buf.push_back(L'\0');
	STARTUPINFOW si{};
	si.cb = sizeof(si);
	PROCESS_INFORMATION pi{};
	if (CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, 0, nullptr,
	                   exeDir.c_str(), &si, &pi)) {
		CloseHandle(pi.hThread);
		CloseHandle(pi.hProcess);
	} else {
		MessageBoxW(nullptr, L"無法啟動工具視窗（子程序建立失敗）。", L"PobTools",
		            MB_ICONERROR | MB_OK);
	}
}

// Logical (unscaled) window size; multiplied by the monitor content scale.
static const int kWinW = 1000;
static const int kWinH = 700;
static const float kFontSize = 19.0f;
static const float kSmallFontSize = 15.0f;
static const float kTitleFontSize = 26.0f;

// External-link board (wide layout). Labels feed the glyph atlas automatically
// (see the AddText loop below), so adding an entry needs no font work.
// The Discord and sponsor links are rendered after this list from
// LauncherStrings so they stay translated; everything here is a proper noun.
struct LinkEntry { const char* label; const wchar_t* url; };
static const LinkEntry kLinks[] = {
	{ u8"PoeDB 流亡編年史",       L"https://poedb.tw" },
	{ u8"PoE2DB",                 L"https://poe2db.tw" },
	{ u8"官方網站",               L"https://www.pathofexile.com" },
	{ u8"官方交易市集",           L"https://www.pathofexile.com/trade" },
	{ u8"交易市集中文化",         L"https://github.com/Hsiung-Shao/poe-market-zh/releases/latest" },
	{ u8"PoE Wiki",               L"https://www.poewiki.net" },
	{ u8"巴哈姆特 PoE 板",        L"https://forum.gamer.com.tw/A.php?bsn=18966" },
	{ u8"Reddit r/pathofexile",   L"https://www.reddit.com/r/pathofexile/" },
	{ u8"poe.ninja",              L"https://poe.ninja" },
	{ u8"FilterBlade",            L"https://www.filterblade.xyz" },
	{ u8"拆粉查詢",               L"https://poe-disenchant-tool.vercel.app/allflame" },
};

// The language-picker labels name scripts a Traditional Chinese font is not
// expected to carry (한국어, 简). The atlas asks for them anyway — if the user
// supplies a font that has them, they draw — but they are not a coverage
// requirement, and LoadFonts already probes koreanOk/cjkOk to drive the UI.
static const char* const kOptionalScriptTexts[] = {
	u8"简体", u8"한국어",
};

// Every piece of text the launcher can put on screen, in one place so the font
// atlas and the coverage selftest cannot disagree about what has to be drawable.
// `overlays` are the JSON-translated string sets actually in use (one per
// locale). They must be listed too: a translator can type a character the chosen
// font has no glyph for, and the atlas is built once for both locales because the
// language combo switches without rebuilding it.
static void CollectLauncherTexts(std::vector<const char*>& out,
                                 const std::vector<const LauncherStrings*>& overlays = {})
{
	// A string that never reaches the glyph atlas is drawn as '?' with no warning
	// anywhere -- that is how the version-history bullet shipped unreadable on one
	// of the two fonts. There used to be a hand-copied roster of fields here that
	// a new string had to be added to; walking the member-pointer table means the
	// roster cannot be out of date at all.
	for (const LauncherStrings* t : { &STR_ZHTW, &STR_EN })
		for (auto m : kLauncherStringMembers)
			if (t->*m) out.push_back(t->*m);
	for (const LauncherStrings* t : overlays)
		if (t)
			for (auto m : kLauncherStringMembers)
				if (t->*m) out.push_back(t->*m);
	out.push_back(kAppUpdateGlyphSeed); // dynamic updater Status.message vocabulary
	out.push_back(kChangelogText);      // version-history dialog body
	for (const LinkEntry& l : kLinks) out.push_back(l.label);
	out.push_back(u8"繁體中文Korean·"); // language combo labels + link separator
}

// Release history body.
//
// changelog.h is hard-wrapped at ~26 CJK characters per line, because it used to
// be drawn in a 600px modal. In a full-width tab those breaks are simply wrong:
// the text stays in a narrow column with the rest of the window empty. So the
// baked-in line breaks are UNDONE here and ImGui re-wraps at the real width.
//
// Structure, per the format contract with changelog.h:
//   "v" + digit           release header (accent colour, gap above)
//   ""                    blank line between releases
//   U+3000 + "·" or "- "   bullet (both spellings exist across the history)
//   U+3000, anything else  continuation of the previous line -- folded back in
//                          (one U+3000 after a heading, two after a bullet)
//   anything else         section heading (修正 / 新增 / 調整)
//
// Historical entries are never edited (a standing project rule), so undoing the
// wrap at render time is the only way to fix them.
static void DrawChangelogBody(float scale)
{
	static const char kIdeoSpace[] = "\xe3\x80\x80";   // U+3000
	static const char kMidDot[]    = "\xc2\xb7";       // U+00B7
	auto startsWith = [](const std::string& s, const char* p) {
		return s.compare(0, strlen(p), p) == 0;
	};
	auto isBullet = [&](const std::string& s) {
		return startsWith(s, (std::string(kIdeoSpace) + kMidDot).c_str()) ||
		       startsWith(s, (std::string(kIdeoSpace) + "- ").c_str());
	};

	// 1. fold continuations back into the line they belong to. Bullets are
	//    continued with two U+3000, headings with one, so the rule is "indented
	//    and not the start of a bullet".
	std::vector<std::string> lines;
	{
		const std::string log = kChangelogText;
		size_t start = 0;
		while (start <= log.size()) {
			size_t nl = log.find('\n', start);
			size_t len = (nl == std::string::npos ? log.size() : nl) - start;
			std::string line = log.substr(start, len);
			if (!lines.empty() && startsWith(line, kIdeoSpace) && !isBullet(line)) {
				std::string tail = line;
				while (startsWith(tail, kIdeoSpace)) tail.erase(0, strlen(kIdeoSpace));
				std::string& prev = lines.back();
				// The wrap points are all mid-CJK, where no separator belongs.
				// Guard the one case that would lose a space anyway.
				if (!prev.empty() && !tail.empty() &&
				    (unsigned char)prev.back() < 0x80 && isalnum((unsigned char)prev.back()) &&
				    (unsigned char)tail[0] < 0x80 && isalnum((unsigned char)tail[0]))
					prev += ' ';
				prev += tail;
			} else {
				lines.push_back(line);
			}
			if (nl == std::string::npos) break;
			start = nl + 1;
		}
	}

	// 2. draw
	const float indent = 16.0f * scale;
	bool first = true;
	for (const std::string& line : lines) {
		if (line.empty()) {
			ImGui::Dummy(ImVec2(0, 10.0f * scale)); // between releases
			first = false;
			continue;
		}
		const bool isVer = line.size() > 1 && line[0] == 'v' &&
		                   line[1] >= '0' && line[1] <= '9';
		if (isVer && !first) ImGui::Dummy(ImVec2(0, 6.0f * scale));

		std::string text = line;
		bool bullet = false;
		if (startsWith(text, kIdeoSpace)) {
			text.erase(0, strlen(kIdeoSpace));
			if (startsWith(text, kMidDot)) { text.erase(0, strlen(kMidDot)); bullet = true; }
			else if (startsWith(text, "- ")) { text.erase(0, 2); bullet = true; }
		}
		if (bullet) {
			text = std::string(kMidDot) + " " + text;
			ImGui::Indent(indent);
		}
		ImGui::PushTextWrapPos(0.0f); // wrap at the container's right edge
		if (isVer) ImGui::PushStyleColor(ImGuiCol_Text, PobUi::Accent());
		ImGui::TextUnformatted(text.c_str());
		if (isVer) ImGui::PopStyleColor();
		ImGui::PopTextWrapPos();
		if (bullet) ImGui::Unindent(indent);
		ImGui::Dummy(ImVec2(0, 4.0f * scale)); // line leading
		first = false;
	}
}

// Launcher-specific draw-list colours; shared widgets use ui_theme.cpp.
static const ImU32 kAccent     = IM_COL32(99, 102, 241, 255);   // #6366f1
static const ImU32 kTextMain   = IM_COL32(248, 250, 252, 255);  // #f8fafc
static const ImU32 kTextMuted  = IM_COL32(136, 153, 162, 255);
static const ImU32 kGreenOk    = IM_COL32(102, 211, 143, 255);
static const ImU32 kRedWarn    = IM_COL32(239, 105, 111, 255);
static const ImU32 kGlassFill  = IM_COL32(15, 22, 27, 255);
static const ImU32 kGlassEdge  = IM_COL32(43, 57, 66, 255);

static ImU32 AccentAlpha(int alpha) { return IM_COL32(99, 102, 241, alpha); }

// Read a file into memory using a wide path (the exe may live in a non-ASCII
// directory, so AddFontFromFileTTF's narrow fopen is unsafe).
static std::vector<unsigned char> read_file(const std::wstring& path)
{
	std::vector<unsigned char> data;
	HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
	if (h == INVALID_HANDLE_VALUE) return data;
	LARGE_INTEGER size{};
	if (GetFileSizeEx(h, &size) && size.QuadPart > 0 && size.QuadPart < (1ll << 30)) {
		data.resize((size_t)size.QuadPart);
		DWORD read = 0;
		if (!ReadFile(h, data.data(), (DWORD)data.size(), &read, nullptr) || read != data.size()) {
			data.clear();
		}
	}
	CloseHandle(h);
	return data;
}

static std::string to_utf8(const std::wstring& w)
{
	if (w.empty()) return std::string();
	int needed = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
	std::string s(needed, '\0');
	WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], needed, nullptr, nullptr);
	return s;
}

// UTF-8 -> codepoints. One decoder shared by the live coverage probe and the
// headless coverage selftest: two copies would eventually disagree about some
// edge case and the check would stop meaning what the probe means.
template <class F>
static void ForEachCodepoint(const char* text, F&& fn)
{
	for (const unsigned char* p = (const unsigned char*)text; p && *p; ) {
		unsigned cp = 0;
		int n = 1;
		if (*p < 0x80)                { cp = *p; }
		else if ((*p & 0xE0) == 0xC0) { cp = *p & 0x1Fu; n = 2; }
		else if ((*p & 0xF0) == 0xE0) { cp = *p & 0x0Fu; n = 3; }
		else if ((*p & 0xF8) == 0xF0) { cp = *p & 0x07u; n = 4; }
		else { p++; continue; }                      // stray continuation byte
		for (int i = 1; i < n; i++) {
			if ((p[i] & 0xC0) != 0x80) { n = i; cp = 0; break; }
			cp = (cp << 6) | (p[i] & 0x3Fu);
		}
		p += n;
		if (cp < 0x20 || cp >= 0x110000) continue;   // control chars are not drawn
		fn(cp);
	}
}

// ImGui hands back UTF-8; Win32 paths are UTF-16. The data-folder field is the
// one place the user types a path directly.
static std::wstring from_utf8(const std::string& s)
{
	if (s.empty()) return std::wstring();
	int needed = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
	std::wstring w((size_t)needed, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], needed);
	return w;
}

// Build one atlas covering every string in all language tables (plus any
// runtime texts such as detected install paths), so switching the UI
// language never requires a rebuild.
struct LauncherFonts {
	ImFont* body = nullptr;
	ImFont* small = nullptr;
	ImFont* title = nullptr;
	bool koreanOk = false;
	bool cjkOk = false;
};

static LauncherFonts LoadFonts(const std::wstring& fontPath, std::vector<unsigned char>& ttfKeepAlive,
                               const std::vector<std::string>& extraTexts, float scale,
                               const std::vector<const LauncherStrings*>& overlays = {})
{
	LauncherFonts out;
	ImGuiIO& io = ImGui::GetIO();

	ttfKeepAlive = read_file(fontPath);
	if (ttfKeepAlive.empty()) {
		out.body = io.Fonts->AddFontDefault();
		out.small = out.body;
		out.title = out.body;
		io.Fonts->Build();
		return out;
	}

	static ImVector<ImWchar> ranges; // must outlive the atlas build
	ranges.clear();
	ImFontGlyphRangesBuilder b;
	b.AddRanges(io.Fonts->GetGlyphRangesDefault());
	{
		std::vector<const char*> texts;
		CollectLauncherTexts(texts, overlays);
		for (const char* t : texts) b.AddText(t);
		for (const char* t : kOptionalScriptTexts) b.AddText(t);
	}
	for (const std::string& t : extraTexts) b.AddText(t.c_str());
	b.BuildRanges(&ranges);

	ImFontConfig cfg;
	cfg.FontDataOwnedByAtlas = false; // shared buffer for all sizes; we keep it alive
	out.body = io.Fonts->AddFontFromMemoryTTF(ttfKeepAlive.data(), (int)ttfKeepAlive.size(), kFontSize * scale, &cfg, ranges.Data);
	out.small = io.Fonts->AddFontFromMemoryTTF(ttfKeepAlive.data(), (int)ttfKeepAlive.size(), kSmallFontSize * scale, &cfg, ranges.Data);
	out.title = io.Fonts->AddFontFromMemoryTTF(ttfKeepAlive.data(), (int)ttfKeepAlive.size(), kTitleFontSize * scale, &cfg, ranges.Data);
	io.Fonts->Build();

	if (out.body) {
		out.cjkOk = out.body->FindGlyphNoFallback((ImWchar)0x555F /* 啟 */) != nullptr;
		out.koreanOk = out.body->FindGlyphNoFallback((ImWchar)0xD55C /* 한 */) != nullptr;
	}
	if (!out.body) {
		out.body = io.Fonts->AddFontDefault();
		out.small = out.body;
		out.title = out.body;
		io.Fonts->Build();
	}
	return out;
}

// Can the LAUNCHER load this font file?
//
// Decided from the file's own bytes, never its extension: a .ttf can hold CFF
// outlines and a .otf can hold glyf ones, so the extension is not the format. The
// launcher draws with stb_truetype (bundled inside ImGui), which handles glyf
// outlines only -- 'OTTO' (CFF/PostScript) and WOFF are out. The ENGINE renders
// with FreeType and accepts more, so this is a launcher-side limit rather than a
// property of the file, and the message has to say so.
//
// Not done by building a throwaway atlas: ImGui's AddFontFromMemoryTTF only
// IM_ASSERTs on a bad font, and IM_ASSERT is plain assert() here, compiled out in
// Release -- it would read past the buffer instead of reporting anything.
enum class FontKind { TrueType, CffOutlines, NotAFont };
static FontKind ClassifyFontFile(const std::vector<unsigned char>& d)
{
	if (d.size() < 4) return FontKind::NotAFont;
	const unsigned tag = ((unsigned)d[0] << 24) | ((unsigned)d[1] << 16) |
	                     ((unsigned)d[2] << 8) | (unsigned)d[3];
	switch (tag) {
		case 0x00010000u:  // TrueType outlines
		case 0x74727565u:  // 'true'  (Apple TrueType)
		case 0x74746366u:  // 'ttcf'  (collection; stb reads font 0)
			return FontKind::TrueType;
		case 0x4F54544Fu:  // 'OTTO'  (CFF outlines)
			return FontKind::CffOutlines;
		default:
			return FontKind::NotAFont;   // includes 'wOFF' / 'wOF2'
	}
}

// Can the freshly built atlas actually draw each language's labels? ImGui
// substitutes '?' for a missing glyph and says nothing, so once the labels became
// translatable this had to be asked rather than assumed -- someone installing a
// Latin-only font and picking Chinese would otherwise just get a broken screen.
// missing[i] collects up to a few of the characters that failed, for the message.
static std::vector<bool> ProbeLocaleCoverage(const LauncherFonts& fonts,
                                             const std::vector<LauncherStringStore>& stores,
                                             std::vector<std::string>* missing)
{
	std::vector<bool> ok(stores.size(), true);
	if (missing) missing->assign(stores.size(), std::string());
	if (!fonts.body) return ok;
	for (size_t i = 0; i < stores.size(); i++) {
		int shown = 0;
		for (auto m : kLauncherStringMembers) {
			const char* s = stores[i].s.*m;
			if (!s) continue;
			ForEachCodepoint(s, [&](unsigned cp) {
				if (cp >= 0x110000) return;
				if (fonts.body->FindGlyphNoFallback((ImWchar)cp)) return;
				ok[i] = false;
				if (missing && shown < 6) {
					wchar_t w[2] = { (wchar_t)cp, 0 };
					(*missing)[i] += to_utf8(w);
					shown++;
				}
			});
		}
	}
	return ok;
}

static void TextCenteredAt(ImDrawList* dl, ImFont* font, float fontSize, ImVec2 center, ImU32 col, const char* text)
{
	ImVec2 sz = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, text);
	dl->AddText(font, fontSize, ImVec2(center.x - sz.x * 0.5f, center.y - sz.y * 0.5f), col, text);
}

// The lightning bolt from the old launcher's SVG (viewBox 24x24,
// path 13,2 3,14 12,14 11,22 21,10 12,10), pre-triangulated.
static void DrawBolt(ImDrawList* dl, ImVec2 origin, float size, ImU32 col)
{
	float s = size / 24.0f;
	auto P = [&](float x, float y) { return ImVec2(origin.x + x * s, origin.y + y * s); };
	ImVec2 A = P(13, 2), B = P(3, 14), C = P(12, 14), D = P(11, 22), E = P(21, 10), F = P(12, 10);
	dl->AddTriangleFilled(A, B, C, col);
	dl->AddTriangleFilled(C, D, E, col);
	dl->AddTriangleFilled(A, C, E, col);
	dl->AddTriangleFilled(A, E, F, col);
}

static bool PrimaryButton(const char* id, const char* label, bool enabled, const LauncherFonts& fonts, float scale, ImVec2 size);

// Section header for the wide layout: muted small label with a hairline
// extending to the right edge of the content area.
static void SectionLabel(const LauncherFonts& fonts, float scale, float innerW, const char* text)
{
	ImGui::PushFont(fonts.small);
	ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
	ImGui::TextUnformatted(text);
	ImGui::PopStyleColor();
	ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
	float y = (mn.y + mx.y) * 0.5f;
	ImGui::GetWindowDrawList()->AddLine(ImVec2(mx.x + 12.0f * scale, y),
		ImVec2(mn.x + innerW, y), kGlassEdge, 1.0f);
	ImGui::PopFont();
	ImGui::Dummy(ImVec2(0, 2.0f * scale));
}

// About body: product line, build date, attribution. Per-line leading because
// the default line height packs CJK too tightly.
static void DrawAboutBody(const LauncherStrings& S, const LauncherFonts& fonts,
                          float scale, float wrap)
{
	ImGui::PushFont(fonts.title);
	ImGui::TextUnformatted("PobTools");
	ImGui::PopFont();
	ImGui::PushStyleColor(ImGuiCol_Text, PobUi::MutedText());
	ImGui::TextUnformatted("v" POBTOOLS_VERSION_STRING "  -  Build " __DATE__);
	ImGui::PopStyleColor();
	ImGui::Dummy(ImVec2(0, 10.0f * scale));

	std::string body = S.aboutBody;
	size_t start = 0;
	while (start <= body.size()) {
		size_t nl = body.find('\n', start);
		size_t len = (nl == std::string::npos ? body.size() : nl) - start;
		ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap);
		ImGui::TextUnformatted(body.c_str() + start, body.c_str() + start + len);
		ImGui::PopTextWrapPos();
		ImGui::Dummy(ImVec2(0, 9.0f * scale)); // leading between lines
		if (nl == std::string::npos) break;
		start = nl + 1;
	}
}

// Wide game row: icon badge + game name + POB version + detect status on a
// glass card, with an inline launch button on the right (disabled when the
// install is missing). Returns true when launch was clicked.
static bool GameRow(const char* id, const char* name, const std::string& version,
                    const char* status, bool ok, const LauncherFonts& fonts, float scale,
                    float width, const char* tooltip, const char* launchLabel)
{
	const float h = 72.0f * scale;
	ImVec2 p = ImGui::GetCursorScreenPos();
	ImDrawList* dl = ImGui::GetWindowDrawList();
	float r = 6.0f * scale;

	// card background (whole row is inert; only the launch button acts)
	ImGui::Dummy(ImVec2(width, h));
	bool rowHovered = ImGui::IsItemHovered();
	dl->AddRectFilled(p, p + ImVec2(width, h), rowHovered ? IM_COL32(21, 31, 37, 255) : kGlassFill, r);
	dl->AddRect(p, p + ImVec2(width, h), rowHovered ? IM_COL32(59, 78, 88, 255) : kGlassEdge, r, 0, 1.0f);

	// icon badge
	float badge = 40.0f * scale;
	ImVec2 bp = p + ImVec2(16.0f * scale, (h - badge) * 0.5f);
	dl->AddRectFilled(bp, bp + ImVec2(badge, badge), AccentAlpha(ok ? 42 : 18), 8.0f * scale);
	dl->AddRect(bp, bp + ImVec2(badge, badge), AccentAlpha(ok ? 90 : 35), 8.0f * scale, 0, 1.0f);
	float bolt = 22.0f * scale;
	DrawBolt(dl, bp + ImVec2((badge - bolt) * 0.5f, (badge - bolt) * 0.5f), bolt,
		ok ? kAccent : IM_COL32(99, 102, 241, 90));

	// name + version, left-aligned next to the badge
	float tx = bp.x + badge + 14.0f * scale;
	ImU32 nameCol = ok ? kTextMain : IM_COL32(120, 130, 145, 255);
	dl->AddText(fonts.body, kFontSize * scale * 1.05f, ImVec2(tx, p.y + 14.0f * scale), nameCol, name);
	if (!version.empty()) {
		std::string v = "POB v" + version;
		dl->AddText(fonts.small, kSmallFontSize * scale, ImVec2(tx, p.y + h - 14.0f * scale - kSmallFontSize * scale),
			kTextMuted, v.c_str());
	}

	// detect status, right of the text block (fixed column keeps rows aligned)
	ImVec2 statusPos(p.x + width * 0.58f, p.y + (h - kSmallFontSize * scale) * 0.5f);
	dl->AddCircleFilled(ImVec2(statusPos.x - 10.0f * scale, p.y + h * 0.5f), 3.0f * scale,
		ok ? kGreenOk : kRedWarn);
	dl->AddText(fonts.small, kSmallFontSize * scale, statusPos,
		ok ? kGreenOk : kRedWarn, status);

	// inline launch button
	ImVec2 btnSize(110.0f * scale, 44.0f * scale);
	ImGui::SetCursorScreenPos(p + ImVec2(width - btnSize.x - 16.0f * scale, (h - btnSize.y) * 0.5f));
	bool clicked = PrimaryButton(id, launchLabel, ok, fonts, scale, btnSize);
	ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h)); // resume normal flow below the card

	if (rowHovered && tooltip && tooltip[0]) {
		ImGui::PushFont(fonts.small);
		ImGui::SetTooltip("%s", tooltip);
		ImGui::PopFont();
	}
	return clicked;
}

// Inline hyperlink: muted text, indigo + underline + hand cursor on hover,
// opens the URL in the default browser on click.
static void LinkText(const char* label, const wchar_t* url)
{
	bool hovered;
	{
		ImVec2 sz = ImGui::CalcTextSize(label);
		ImVec2 p = ImGui::GetCursorScreenPos();
		hovered = ImGui::IsMouseHoveringRect(p, p + sz);
	}
	ImGui::PushStyleColor(ImGuiCol_Text, hovered ? PobUi::Accent() : PobUi::MutedText());
	ImGui::TextUnformatted(label);
	ImGui::PopStyleColor();
	if (ImGui::IsItemHovered()) {
		ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
		ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
		ImGui::GetWindowDrawList()->AddLine(ImVec2(mn.x, mx.y - 1.0f), ImVec2(mx.x, mx.y - 1.0f), AccentAlpha(200), 1.0f);
		if (ImGui::IsMouseClicked(0)) {
			ShellExecuteW(nullptr, L"open", url, nullptr, nullptr, SW_SHOWNORMAL);
		}
	}
}

// Primary launch action: restrained indigo surface and white label.
static bool PrimaryButton(const char* id, const char* label, bool enabled, const LauncherFonts& fonts, float scale, ImVec2 size)
{
	ImVec2 p = ImGui::GetCursorScreenPos();
	ImGui::BeginDisabled(!enabled);
	bool clicked = ImGui::InvisibleButton(id, size);
	bool hovered = ImGui::IsItemHovered();
	bool held = ImGui::IsItemActive();
	ImGui::EndDisabled();
	ImDrawList* dl = ImGui::GetWindowDrawList();
	float r = 6.0f * scale;

	if (enabled && hovered) {
		// Soft glow is kept inside the row so it cannot overlap neighbouring cards.
		dl->AddRectFilled(p + ImVec2(3, 4) * scale, p + size + ImVec2(-3, 6) * scale, AccentAlpha(28), r + 3.0f * scale);
	}
	ImU32 fill, border;
	if (!enabled)      { fill = IM_COL32(255, 255, 255, 8);  border = IM_COL32(255, 255, 255, 18); }
	else if (held)     { fill = AccentAlpha(110); border = AccentAlpha(210); }
	else if (hovered)  { fill = AccentAlpha(80);  border = AccentAlpha(180); }
	else               { fill = AccentAlpha(50);  border = AccentAlpha(110); }
	dl->AddRectFilled(p, p + size, fill, r);
	dl->AddRect(p, p + size, border, r, 0, 1.0f);

	ImU32 labelCol = enabled ? kTextMain : IM_COL32(120, 130, 145, 255);
	TextCenteredAt(dl, fonts.body, kFontSize * scale * 1.05f, p + size * 0.5f, labelCol, label);
	return clicked && enabled;
}

LauncherResult ShowLauncher(LauncherConfig& cfg, const InstallInfo& installs, const std::wstring& exeDir,
                            AppUpdater* appUpd)
{
	if (!glfwInit()) {
		MessageBoxW(nullptr, L"無法初始化 GLFW，啟動器介面無法顯示。", L"PobTools", MB_ICONERROR | MB_OK);
		return LauncherResult::Quit;
	}

	// Same context setup as the engine (sys_video.cpp): GLES 3.0 via ANGLE/EGL.
	glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_ES_API);
	glfwWindowHint(GLFW_CONTEXT_CREATION_API, GLFW_EGL_CONTEXT_API);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
	glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
	glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE); // position first, then show

	float scale = 1.0f;
	GLFWmonitor* monitor = glfwGetPrimaryMonitor();
	if (monitor) {
		float sx = 1.0f, sy = 1.0f;
		glfwGetMonitorContentScale(monitor, &sx, &sy);
		scale = sx > 0.0f ? sx : 1.0f;
	}
	const int winW = (int)(kWinW * scale);
	const int winH = (int)(kWinH * scale);

	GLFWwindow* win = glfwCreateWindow(winW, winH, "PobTools", nullptr, nullptr);
	if (!win) {
		glfwTerminate();
		MessageBoxW(nullptr, L"無法建立啟動器視窗。", L"PobTools", MB_ICONERROR | MB_OK);
		return LauncherResult::Quit;
	}
	if (monitor) {
		const GLFWvidmode* mode = glfwGetVideoMode(monitor);
		if (mode) glfwSetWindowPos(win, (mode->width - winW) / 2, (mode->height - winH) / 2);
	}
	glfwMakeContextCurrent(win);
	glfwSwapInterval(1);
	glfwShowWindow(win);

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGui::GetIO().IniFilename = nullptr; // never touch the engine's imgui.ini
	PobUi::ApplyTheme(scale, PobUi::Density::Comfortable);

	// Detected install folders (shown in card tooltips) need their glyphs in the atlas.
	std::string poe1Dir = installs.poe1Lua.empty() ? "" : to_utf8(installs.poe1Lua.substr(0, installs.poe1Lua.find_last_of(L'\\')));
	std::string poe2Dir = installs.poe2Lua.empty() ? "" : to_utf8(installs.poe2Lua.substr(0, installs.poe2Lua.find_last_of(L'\\')));
	// Where each dictionary set lives: the install, or a translator's working copy
	// somewhere else. Independent per slot -- someone translating only PoE1 should
	// not be dragged into keeping an external PoE2 copy in step.
	DictDirInfo dictDir[kDictSlotCount];
	auto resolveDict = [&](int i) { dictDir[i] = ResolveDictDir(exeDir, (DictSlot)i, cfg.dataDir[i]); };
	for (int i = 0; i < kDictSlotCount; i++) resolveDict(i);

	// Languages come from the folders on disk, so adding Data\poe1\ja-JP\ is all
	// it takes to offer Japanese. "en" is always first and needs no folder.
	std::vector<LocaleInfo> locales = ListInstalledLocales(exeDir, cfg);

	// Launcher labels come from the compiled tables with
	// <launcher slot>\<locale>\launcher.json layered on top. EVERY language is
	// loaded up front because the language picker switches without rebuilding the
	// glyph atlas -- so the atlas must already contain whatever every translator
	// typed. Loaded BEFORE LoadFonts for exactly that reason, which also means a
	// changed data path only reaches these labels on the next launcher start.
	const std::wstring launcherRoot = dictDir[(int)DictSlot::Launcher].root;
	std::vector<LauncherStringStore> strStore;
	strStore.reserve(locales.size()); // LauncherStringStore is move-only (see its header)
	for (const LocaleInfo& l : locales)
		strStore.emplace_back(LoadLauncherStrings(launcherRoot, from_utf8(l.id)));
	// EVERY language, not just the selected one: the atlas is rebuilt only when the
	// font changes, so switching language must not need glyphs that were never
	// added. A missing one is drawn as '?' with no warning of any kind.
	std::vector<const LauncherStrings*> strOverlays;
	strOverlays.reserve(strStore.size());
	for (const LauncherStringStore& st : strStore) strOverlays.push_back(&st.s);

	std::vector<unsigned char> ttfData;
	LauncherFonts fonts = LoadFonts(ResolveFontPath(exeDir, cfg.fontFile), ttfData,
	                                { poe1Dir, poe2Dir }, scale, strOverlays);
	std::vector<std::wstring> fontList = ListAvailableFonts(exeDir);
	bool fontChanged = false;
	// Recomputed with the atlas, never independently: the answer is a property of
	// the atlas that was just built, not of the font file.
	std::vector<std::string> localeMissing;
	std::vector<bool> localeDrawable = ProbeLocaleCoverage(fonts, strStore, &localeMissing);

	ImGui_ImplGlfw_InitForOpenGL(win, true);
	ImGui_ImplOpenGL3_Init("#version 100");

	// Pre-select an available game if the remembered one is missing.
	bool poe2Sel = (cfg.game == L"poe2");
	if (poe2Sel && installs.poe2Lua.empty() && !installs.poe1Lua.empty()) poe2Sel = false;
	if (!poe2Sel && installs.poe1Lua.empty() && !installs.poe2Lua.empty()) poe2Sel = true;

	// Falls back to zh-rTW, then en, when the configured language's folder is gone.
	int localeIdx = PickLocaleIndex(locales, cfg.locale);

	bool launch = false;
	bool openEditor = false;
	bool applyUpdate = false;

	// Game and language live in widget state (poe2Sel / localeIdx), not in cfg, so
	// cfg is stale until this runs. Every path that writes the ini must call it
	// first -- the "Save settings" button used to write the OLD language back,
	// which is exactly the kind of thing that makes a save button untrustworthy.
	auto syncCfgFromUi = [&]() {
		cfg.game = poe2Sel ? L"poe2" : L"poe1";
		if (localeIdx >= 0 && localeIdx < (int)locales.size())
			cfg.locale = from_utf8(locales[localeIdx].id);
	};

	// tools spawn as child processes so this window stays open (see SpawnTool)
	auto spawnTool = [&](const wchar_t* flag) {
		syncCfgFromUi();
		SaveLauncherConfig(exeDir + L"pob-zh.ini", cfg);
		SpawnTool(exeDir, flag);
	};
	// KeepOpen mode: start POB the same way the tools are started (detached, the
	// window stays up) instead of returning Launch. ShowLauncher tears down GLFW
	// and ImGui before it returns, so "return a result" could never keep the
	// window alive, let alone allow a second POB while the first is running.
	auto launchPob = [&](bool poe2) {
		syncCfgFromUi();
		cfg.game = poe2 ? L"poe2" : L"poe1"; // the row that was clicked, not the selection
		SaveLauncherConfig(exeDir + L"pob-zh.ini", cfg);   // the child's safety net
		// Only a validated external folder is passed on (see the settings page),
		// and only the one for the game being started; a broken path leaves POB on
		// the built-in dictionaries.
		const int slot = poe2 ? (int)DictSlot::Poe2 : (int)DictSlot::Poe1;
		PobLaunch::SetEngineEnv(cfg.game, cfg.locale, cfg.fontFile,
		                        dictDir[slot].status == DataDirStatus::External
		                            ? dictDir[slot].root : std::wstring());
		const std::wstring lua = poe2 ? installs.poe2Lua : installs.poe1Lua;
		if (!lua.empty()) PobLaunch::SpawnPobDetached(lua, cfg.game);
	};
	// "Copy the built-in data to..." state. The confirm popup has to be opened
	// from outside the tab's draw scope (ImGui's ID stack), so the button records
	// an intent and the work happens after the window ends.
	std::wstring copyDest;
	std::string copyMsg;
	int copySlot = 0;
	bool askOverwrite = false;
	bool doCopy = false;
	// Text being typed in each path box. Kept apart from cfg so a half-typed path
	// is not treated as the setting, and mirrored back from cfg whenever the box
	// is not focused (so Clear / Browse / the suggestion button show up there).
	std::string dirEdit[kDictSlotCount];
	for (int i = 0; i < kDictSlotCount; i++) dirEdit[i] = to_utf8(cfg.dataDir[i]);
	std::string fontMsg;       // result of the last "install a font" attempt
	double savedUntil = 0.0;   // "saved" confirmation deadline

	// EVERY settings change writes the ini immediately. Half-immediate is worse
	// than either extreme: some fields used to persist on change and the rest only
	// when the window closed, so whether a change survived depended on which
	// widget it was -- and nothing on screen said which. The "Save settings"
	// button now only exists to say "yes, it is written", not to be the one way
	// changes take effect.
	auto saveNow = [&]() {
		syncCfgFromUi(); // language / game are widget state until now
		SaveLauncherConfig(exeDir + L"pob-zh.ini", cfg);
		savedUntil = ImGui::GetTime() + 3.0;
	};

	double transNoticeUntil = 0.0; // TransDone banner auto-dismiss deadline
	// A check the user asked for must report back even when the answer is "no
	// news"; the automatic startup one stays silent.
	bool manualCheck = false;
	double upToDateUntil = 0.0;
	bool wasPobBusy = false;       // edge-detect "the last POB just closed"
	bool applyStartupTab = true;   // honour cfg.startupTab on the first frame only
	while (!glfwWindowShouldClose(win) && !launch && !openEditor && !applyUpdate) {
		glfwPollEvents();

		// Live font switch: rebuild the glyph atlas between frames when the user
		// picks a different font in the status-bar combo.
		if (fontChanged) {
			fontChanged = false;
			ImGui_ImplOpenGL3_DestroyFontsTexture();
			ImGui::GetIO().Fonts->Clear();
			fonts = LoadFonts(ResolveFontPath(exeDir, cfg.fontFile), ttfData,
			                  { poe1Dir, poe2Dir }, scale, strOverlays);
			localeDrawable = ProbeLocaleCoverage(fonts, strStore, &localeMissing);
			ImGui_ImplOpenGL3_CreateFontsTexture();
		}

		ImGui_ImplOpenGL3_NewFrame();
		ImGui_ImplGlfw_NewFrame();
		ImGui::NewFrame();

		// When the chosen font cannot draw the chosen language, fall back to the
		// English labels (index 0) rather than a screen full of '?'.
		const bool langDrawable = localeDrawable.empty() ||
		                          (localeIdx >= 0 && localeIdx < (int)localeDrawable.size() &&
		                           localeDrawable[localeIdx]);
		const LauncherStrings& S = langDrawable ? strStore[localeIdx].s : strStore[0].s;

		// POB instances this launcher started (KeepOpen mode). Counted every
		// frame because that call is also where finished processes are reaped.
		const int pobCount = PobLaunch::PobRunningCount();
		const bool pobBusy = PobLaunch::AnyPobRunning(exeDir);
		if (appUpd) {
			// Applying an update renames engine\* out of the way while POB has
			// those DLLs open, and the same check silently overwrites Data\*.json
			// with a fresh translation pack. Both have to stop, so the gate goes
			// on the worker, not just on the button.
			appUpd->SetHold(pobBusy);
			// Last POB closed: pick the check back up instead of waiting a day.
			if (wasPobBusy && !pobBusy) appUpd->RequestCheck(false);
		}
		wasPobBusy = pobBusy;

		// App-updater snapshot for this frame. While the update is in flight the
		// launch/tool actions are disabled so the auto-relaunch cannot interrupt
		// anything; a ready stage closes the window via ApplyAppUpdate.
		AppUpdater::Status ust;
		if (appUpd) {
			ust = appUpd->Poll();
			if (ust.phase == AppUpdatePhase::UpToDate) {
				if (!manualCheck) {
					appUpd->AckNotice(); // silent: only problems and news are shown
					ust = appUpd->Poll();
				} else {
					if (upToDateUntil == 0.0) upToDateUntil = ImGui::GetTime() + 4.0;
					if (ImGui::GetTime() >= upToDateUntil) {
						appUpd->AckNotice();
						upToDateUntil = 0.0;
						manualCheck = false;
						ust = appUpd->Poll();
					}
				}
			}
			if (ust.phase == AppUpdatePhase::TransDone) {
				if (transNoticeUntil == 0.0) transNoticeUntil = ImGui::GetTime() + 6.0;
				if (ImGui::GetTime() >= transNoticeUntil) {
					appUpd->AckNotice();
					transNoticeUntil = 0.0;
					ust = appUpd->Poll();
				}
			}
			if (ust.phase == AppUpdatePhase::AppReadyToApply) applyUpdate = true;
		}
		bool updaterBusy = ust.phase == AppUpdatePhase::AppDownloading ||
		                   ust.phase == AppUpdatePhase::AppStaging ||
		                   ust.phase == AppUpdatePhase::AppReadyToApply;

		ImGuiIO& io = ImGui::GetIO();
		ImGui::SetNextWindowPos(ImVec2(0, 0));
		ImGui::SetNextWindowSize(io.DisplaySize);
		ImGui::PushFont(fonts.body);
		ImGui::Begin("##launcher", nullptr,
			ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);

		ImDrawList* dl = ImGui::GetWindowDrawList();
		float W = io.DisplaySize.x;
		float padX = ImGui::GetStyle().WindowPadding.x;
		float inner = W - padX * 2.0f;

		// Header: badge + title/subtitle in one left-aligned row.
		{
			float badge = 48.0f * scale;
			ImVec2 bp = ImGui::GetCursorScreenPos();
			dl->AddRectFilled(bp, bp + ImVec2(badge, badge), AccentAlpha(42), 8.0f * scale);
			dl->AddRect(bp, bp + ImVec2(badge, badge), AccentAlpha(90), 8.0f * scale, 0, 1.0f);
			float bolt = 26.0f * scale;
			DrawBolt(dl, bp + ImVec2((badge - bolt) * 0.5f, (badge - bolt) * 0.5f), bolt, kAccent);
			dl->AddText(fonts.title, kTitleFontSize * scale,
				bp + ImVec2(badge + 16.0f * scale, -2.0f * scale), kTextMain, S.title);
			dl->AddText(fonts.small, kSmallFontSize * scale,
				bp + ImVec2(badge + 16.0f * scale, kTitleFontSize * scale + 4.0f * scale), kTextMuted, S.subtitle);

			// Updater widget, top-right of the header (kept off the busy status bar).
			// Idle shows the manual check button: the automatic check only fires
			// once per launch and is throttled to once a day, so without this a
			// user who leaves the launcher open has no way to ask again.
			if (appUpd) {
				ImVec2 keep = ImGui::GetCursorPos();
				ImGui::PushFont(fonts.small);
				auto placeRight = [&](float w, float h) {
					ImGui::SetCursorScreenPos(bp + ImVec2(inner - w, (badge - h) * 0.5f));
				};
				if (ust.phase == AppUpdatePhase::Idle) {
					float w = ImGui::CalcTextSize(S.updateCheck).x +
					          ImGui::GetStyle().FramePadding.x * 2.0f;
					placeRight(w, ImGui::GetFrameHeight());
					// Disabled while POB holds engine\*. The tooltip says why and
					// what to do -- a greyed-out button on its own is a dead end.
					ImGui::BeginDisabled(pobBusy);
					if (ImGui::Button(S.updateCheck)) {
						appUpd->RequestCheck(true); // force: skip the daily throttle
						manualCheck = true;
						upToDateUntil = 0.0;
					}
					ImGui::EndDisabled();
					if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
						ImGui::SetTooltip("%s", pobBusy ? S.updateBlockedTip : S.updateCheckTip);
				} else if (ust.phase == AppUpdatePhase::Checking) {
					float w = ImGui::CalcTextSize(S.updateChecking).x;
					placeRight(w, ImGui::GetTextLineHeight());
					ImGui::TextDisabled("%s", S.updateChecking);
				} else if (ust.phase == AppUpdatePhase::UpToDate) {
					std::string txt = std::string(S.updateUpToDate) + " v" + ust.localVer;
					float w = ImGui::CalcTextSize(txt.c_str()).x;
					placeRight(w, ImGui::GetTextLineHeight());
					ImGui::TextColored(ImVec4(0.35f, 0.80f, 0.45f, 1.0f), "%s", txt.c_str());
				} else if (ust.phase == AppUpdatePhase::AppAvailable) {
					std::string label = std::string(S.updateAvailable) + ust.latestVer;
					float w = ImGui::CalcTextSize(label.c_str()).x + ImGui::GetStyle().FramePadding.x * 2.0f;
					placeRight(w, ImGui::GetFrameHeight());
					ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.85f, 0.60f, 0.20f, 0.45f));
					ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.85f, 0.60f, 0.20f, 0.65f));
					ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.85f, 0.60f, 0.20f, 0.85f));
					ImGui::BeginDisabled(pobBusy);
					if (ImGui::Button(label.c_str())) appUpd->StartAppUpdate();
					ImGui::EndDisabled();
					ImGui::PopStyleColor(3);
					if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
						ImGui::SetTooltip("%s", pobBusy ? S.updateBlockedTip : S.updateNow);
				} else if (ust.phase == AppUpdatePhase::AppDownloading) {
					char prog[96];
					if (ust.bytesTotal > 0)
						snprintf(prog, sizeof(prog), "%s%.1f / %.1f MB", S.updateDownloading,
						         ust.bytesDone / 1048576.0, ust.bytesTotal / 1048576.0);
					else
						snprintf(prog, sizeof(prog), "%s%.1f MB", S.updateDownloading,
						         ust.bytesDone / 1048576.0);
					float w = ImGui::CalcTextSize(prog).x;
					placeRight(w, ImGui::GetTextLineHeight());
					ImGui::TextDisabled("%s", prog);
				} else if (ust.phase == AppUpdatePhase::AppStaging ||
				           ust.phase == AppUpdatePhase::AppReadyToApply) {
					const char* txt = ust.phase == AppUpdatePhase::AppStaging ? S.updatePreparing
					                                                          : S.updateRestarting;
					float w = ImGui::CalcTextSize(txt).x;
					placeRight(w, ImGui::GetTextLineHeight());
					ImGui::TextDisabled("%s", txt);
				} else if (ust.phase == AppUpdatePhase::TransDone) {
					std::string txt = std::string(S.updateTransDone) + ust.latestVer;
					float w = ImGui::CalcTextSize(txt.c_str()).x;
					placeRight(w, ImGui::GetTextLineHeight());
					ImGui::TextColored(ImVec4(0.35f, 0.80f, 0.45f, 1.0f), "%s", txt.c_str());
				} else if (ust.phase == AppUpdatePhase::TransAvailable) {
					// Opted out of automatic translation updates. Say what is
					// waiting and offer to take it once -- otherwise the only way
					// to get it is to toggle the setting off and on again.
					std::string txt = ust.message;
					float w = ImGui::CalcTextSize(txt.c_str()).x +
					          ImGui::CalcTextSize(S.transApplyNow).x + 28.0f * scale;
					placeRight(w, ImGui::GetFrameHeight());
					ImGui::AlignTextToFramePadding();
					ImGui::TextColored(ImVec4(0.95f, 0.66f, 0.25f, 1.0f), "%s", txt.c_str());
					ImGui::SameLine();
					ImGui::BeginDisabled(pobBusy);
					if (ImGui::SmallButton(S.transApplyNow)) appUpd->StartTranslationUpdate();
					ImGui::EndDisabled();
					if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && pobBusy)
						ImGui::SetTooltip("%s", S.updateBlockedTip);
				} else if (ust.phase == AppUpdatePhase::Error) {
					std::string txt = std::string(S.updateFailed) + ust.message;
					float w = ImGui::CalcTextSize(txt.c_str()).x +
					          ImGui::CalcTextSize(S.updateRetry).x + 24.0f * scale;
					placeRight(w, ImGui::GetFrameHeight());
					ImGui::TextColored(ImVec4(0.94f, 0.27f, 0.27f, 1.0f), "%s", txt.c_str());
					ImGui::SameLine();
					if (ImGui::SmallButton(S.updateRetry)) appUpd->StartAppUpdate();
				}
				ImGui::PopFont();
				ImGui::SetCursorPos(keep);
			}

			ImGui::Dummy(ImVec2(0, badge + 10.0f * scale));
		}

		// --- tabs -------------------------------------------------------------
		// The version history used to be a 600px modal; as a tab it gets the whole
		// window. Settings collects the controls that used to crowd the status bar.
		const bool tabsOk = ImGui::BeginTabBar("##maintabs");
		ImGuiTabItemFlags homeFlags = 0, verFlags = 0;
		if (applyStartupTab) {
			(cfg.startupTab == StartupTab::Versions ? verFlags : homeFlags) =
				ImGuiTabItemFlags_SetSelected;
			applyStartupTab = false;
		}
		if (tabsOk && ImGui::BeginTabItem(S.tabHome, nullptr, homeFlags)) {
		ImGui::BeginChild("##homebody", ImVec2(0, 0), false);

		// Reading dictionaries from somewhere else changes what POB shows, and a
		// wrong translation looks exactly like broken data -- so it is stated on
		// the main screen rather than only in settings, where it is easy to forget
		// having switched it on. One line per redirected slot: which one matters.
		{
			bool anyExternal = false;
			for (int i = 0; i < kDictSlotCount; i++) {
				if (dictDir[i].status != DataDirStatus::External) continue;
				anyExternal = true;
				ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.66f, 0.25f, 1.0f));
				ImGui::AlignTextToFramePadding();
				ImGui::Text("%s（%s）", S.homeExternalData, to_utf8(DictSlotFolder((DictSlot)i)).c_str());
				ImGui::PopStyleColor();
				ImGui::SameLine(0, 6.0f * scale);
				ImGui::TextDisabled("%s", to_utf8(dictDir[i].root).c_str());
				ImGui::SameLine(0, 10.0f * scale);
				ImGui::PushID(i);
				if (ImGui::SmallButton(S.useBuiltin)) {
					cfg.dataDir[i].clear();
					resolveDict(i);
					saveNow();
				}
				ImGui::PopID();
			}
			if (anyExternal) ImGui::Dummy(ImVec2(0, 4.0f * scale));
		}

		// Games: one wide row per install, launch button inline.
		if (updaterBusy) ImGui::BeginDisabled();
		SectionLabel(fonts, scale, inner, S.gamesSection);
		bool poe1Ok = !installs.poe1Lua.empty();
		bool poe2Ok = !installs.poe2Lua.empty();
		// KeepOpen starts POB detached and leaves this window up; the other two
		// modes take the original path (set `launch`, ShowLauncher returns).
		const bool keepOpen = (cfg.exitMode == LaunchExitMode::KeepOpen);
		if (GameRow("##launch1", S.poe1, installs.poe1Version, poe1Ok ? S.detected : S.missing,
				poe1Ok, fonts, scale, inner, poe1Ok ? poe1Dir.c_str() : S.notFoundPoe1, S.launch)) {
			poe2Sel = false;
			if (keepOpen) launchPob(false); else launch = true;
		}
		ImGui::Dummy(ImVec2(0, 2.0f * scale));
		if (GameRow("##launch2", S.poe2, installs.poe2Version, poe2Ok ? S.detected : S.missing,
				poe2Ok, fonts, scale, inner, poe2Ok ? poe2Dir.c_str() : S.notFoundPoe2, S.launch)) {
			poe2Sel = true;
			if (keepOpen) launchPob(true); else launch = true;
		}
		if (pobCount > 0) {
			ImGui::PushFont(fonts.small);
			ImGui::TextDisabled("%s%d", S.pobRunning, pobCount);
			// Two windows on ONE install share POB's Settings.xml and build files,
			// so the last one closed overwrites the other. Not ours to fix, but
			// the user should not have to discover it by losing work.
			if (PobLaunch::PobRunningCountFor(L"poe1") > 1 ||
			    PobLaunch::PobRunningCountFor(L"poe2") > 1) {
				ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.90f, 0.62f, 0.25f, 1.0f));
				ImGui::TextWrapped("%s", S.pobSameGameWarn);
				ImGui::PopStyleColor();
			}
			ImGui::PopFont();
		}
		if (!poe1Ok && !poe2Ok) {
			ImGui::PushFont(fonts.small);
			ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.94f, 0.27f, 0.27f, 1.0f));
			ImGui::TextWrapped("%s", S.noneFound);
			ImGui::PopStyleColor();
			ImGui::PopFont();
		}
		ImGui::Dummy(ImVec2(0, 8.0f * scale));

		// Tools: secondary actions. Four buttons share the row, so the width
		// divisor and the gap count must move together — three gaps between
		// four buttons.
		SectionLabel(fonts, scale, inner, S.toolsSection);
		{
			float gap = 12.0f * scale;
			ImVec2 toolSize((inner - 3.0f * gap) / 4.0f, 46.0f * scale);
			ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.84f, 0.91f, 0.92f, 1.0f));
			// The translation editor edits dist\Data\{game}\{locale}\*.json in
			// place — the same files the engine loads — so its changes take
			// effect on the next POB launch (verified end to end by
			// --editor-selftest).
			if (ImGui::Button(S.editor, toolSize)) openEditor = true;
			ImGui::SameLine(0, gap);
			if (ImGui::Button(S.filterEditor, toolSize)) spawnTool(L"--filter-editor");
			ImGui::SameLine(0, gap);
			if (ImGui::Button(S.atlasPlanner, toolSize)) spawnTool(L"--atlas");
			ImGui::SameLine(0, gap);
			if (ImGui::Button(S.timelessJewel, toolSize)) spawnTool(L"--timeless-jewel");
			ImGui::PopStyleColor();
		}
		if (updaterBusy) ImGui::EndDisabled();
		ImGui::Dummy(ImVec2(0, 8.0f * scale));

		// Link board: three stretch columns of external links.
		SectionLabel(fonts, scale, inner, S.linksSection);
		if (ImGui::BeginTable("##links", 3, ImGuiTableFlags_SizingStretchSame)) {
			for (const LinkEntry& l : kLinks) {
				ImGui::TableNextColumn();
				LinkText(l.label, l.url);
			}
			// Community + sponsor: moved out of the About dialog so they are
			// reachable without opening a modal. Labels come from the string
			// table rather than kLinks because these two are translated.
			ImGui::TableNextColumn();
			LinkText(S.discord, L"https://discord.gg/6VamPQb8nC");
			ImGui::TableNextColumn();
			LinkText(S.support, L"https://buymeacoffee.com/hsiung");
			ImGui::EndTable();
		}

		ImGui::EndChild();
		ImGui::EndTabItem();
		} // home tab

		// --- version history --------------------------------------------------
		if (tabsOk && ImGui::BeginTabItem(S.changelog, nullptr, verFlags)) {
			ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18.0f * scale, 14.0f * scale));
			ImGui::BeginChild("##changelog_scroll", ImVec2(0, 0), true,
			                  ImGuiWindowFlags_AlwaysUseWindowPadding);
			DrawChangelogBody(scale);
			ImGui::EndChild();
			ImGui::PopStyleVar();
			ImGui::EndTabItem();
		}

		// --- settings ---------------------------------------------------------
		if (tabsOk && ImGui::BeginTabItem(S.tabSettings)) {
			ImGui::BeginChild("##settingsbody", ImVec2(0, 0), false);

			SectionLabel(fonts, scale, inner, S.sectionInterface);
			ImGui::AlignTextToFramePadding();
			ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
			ImGui::TextUnformatted(S.language);
			ImGui::PopStyleColor();
			ImGui::SameLine(160.0f * scale);
			ImGui::SetNextItemWidth(220.0f * scale);
			// Label: the display name, plus which dictionary sets actually have
			// this language. A language present only for PoE1 is still offered
			// (see ListInstalledLocales) and the other game then shows the
			// original text -- saying so here is cheaper than explaining it later.
			auto localeLabel = [&](const LocaleInfo& l) {
				std::string s = l.displayName;
				if (l.id == "en") return s;
				const bool p1 = l.slot[(int)DictSlot::Poe1], p2 = l.slot[(int)DictSlot::Poe2];
				if (p1 && !p2) s += u8"（僅 PoE1）";
				else if (!p1 && p2) s += u8"（僅 PoE2）";
				return s;
			};
			if (localeIdx >= 0 && localeIdx < (int)locales.size() &&
			    ImGui::BeginCombo("##locale", localeLabel(locales[localeIdx]).c_str())) {
				for (int i = 0; i < (int)locales.size(); i++) {
					const bool drawable = i >= (int)localeDrawable.size() || localeDrawable[i];
					if (!drawable) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.66f, 0.25f, 1.0f));
					if (ImGui::Selectable(localeLabel(locales[i]).c_str(), localeIdx == i) &&
					    localeIdx != i) {
						localeIdx = i;
						saveNow();
					}
					if (!drawable) {
						ImGui::PopStyleColor();
						if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", S.fontMissingGlyphs);
					}
				}
				ImGui::EndCombo();
			}

			// Font picker: lists Fonts\*.ttf; switching rebuilds the atlas live.
			// The rebuild happens at the top of the loop, before NewFrame, so it
			// does not care which tab the combo is drawn on.
			auto fontStem = [](const std::wstring& f) {
				std::string s = to_utf8(f);
				size_t d = s.rfind(".ttf");
				if (d == std::string::npos) d = s.rfind(".TTF");
				return d != std::string::npos ? s.substr(0, d) : s;
			};
			ImGui::AlignTextToFramePadding();
			ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
			ImGui::TextUnformatted(S.font);
			ImGui::PopStyleColor();
			ImGui::SameLine(160.0f * scale);
			ImGui::SetNextItemWidth(220.0f * scale);
			if (ImGui::BeginCombo("##font", fontStem(cfg.fontFile).c_str())) {
				for (const std::wstring& f : fontList) {
					if (ImGui::Selectable(fontStem(f).c_str(), f == cfg.fontFile) && f != cfg.fontFile) {
						cfg.fontFile = f;
						fontChanged = true;
						saveNow();
					}
				}
				ImGui::EndCombo();
			}
			ImGui::SameLine(0, 6.0f * scale);
			if (ImGui::Button(S.installFont)) {
				const std::wstring src = EdOpenFontDialog();
				fontMsg.clear();
				if (!src.empty()) {
					const std::wstring name = src.substr(src.find_last_of(L'\\') + 1);
					const std::wstring dst = exeDir + L"Fonts\\" + name;
					switch (ClassifyFontFile(read_file(src))) {
						case FontKind::CffOutlines: fontMsg = S.fontCff; break;
						case FontKind::NotAFont:    fontMsg = S.fontNotAFont; break;
						case FontKind::TrueType:
							// Never overwrite: the target may be one of the shipped
							// fonts, and "install" should not be able to replace them.
							if (GetFileAttributesW(dst.c_str()) != INVALID_FILE_ATTRIBUTES) {
								fontMsg = S.fontAlreadyThere;
								cfg.fontFile = name;
								fontChanged = true;
								saveNow();
							} else if (CopyFileW(src.c_str(), dst.c_str(), TRUE)) {
								fontMsg = std::string(S.fontInstalled) + to_utf8(name);
								fontList = ListAvailableFonts(exeDir);
								cfg.fontFile = name;
								fontChanged = true;
								saveNow();
							} else {
								fontMsg = S.fontCopyFailed;
							}
							break;
					}
				}
			}
			if (!fontMsg.empty()) {
				ImGui::PushTextWrapPos(inner - 40.0f * scale);
				ImGui::TextDisabled("%s", fontMsg.c_str());
				ImGui::PopTextWrapPos();
			}
			// Whether the CURRENT font can draw the CURRENT language. Says
			// "launcher labels" rather than "everything": POB draws through
			// FreeType over a much larger character set, so this is an indicator,
			// not a guarantee.
			if (localeIdx >= 0 && localeIdx < (int)localeDrawable.size() && !localeDrawable[localeIdx]) {
				ImGui::PushTextWrapPos(inner - 40.0f * scale);
				ImGui::TextColored(ImVec4(0.95f, 0.66f, 0.25f, 1.0f), "%s%s",
				                   S.fontMissingHere,
				                   localeIdx < (int)localeMissing.size() ? localeMissing[localeIdx].c_str() : "");
				ImGui::PopTextWrapPos();
			}

			// --- translation data -------------------------------------------
			ImGui::Dummy(ImVec2(0, 10.0f * scale));
			SectionLabel(fonts, scale, inner, S.sectionTransData);
			{
				ImGui::PushTextWrapPos(inner - 40.0f * scale);
				ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
				ImGui::TextUnformatted(S.transDataHint);
				ImGui::PopStyleColor();
				ImGui::PopTextWrapPos();

				const ImVec4 warn(0.95f, 0.66f, 0.25f, 1.0f);
				const char* slotLabel[kDictSlotCount] = { S.poe1, S.poe2, S.slotLauncher };

				for (int i = 0; i < kDictSlotCount; i++) {
					ImGui::PushID(i);
					const std::wstring builtin = BuiltinDictDir(exeDir, (DictSlot)i);

					// Re-resolve only when the path actually changes: ResolveDictDir
					// walks the folder tree, not something to do every frame.
					auto applyPath = [&](const std::wstring& p) {
						cfg.dataDir[i] = p;
						resolveDict(i);
						saveNow();
						copyMsg.clear();
					};

					ImGui::AlignTextToFramePadding();
					ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
					ImGui::TextUnformatted(slotLabel[i]);
					ImGui::PopStyleColor();
					ImGui::SameLine(160.0f * scale);

					// Width from what is actually left on the line, not from `inner`:
					// this child has a scrollbar, so a computed width overshoots and
					// pushes the last button off the edge.
					const float btnW = 84.0f * scale, gapBtn = 6.0f * scale;
					ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - btnW * 3.0f -
					                        gapBtn * 3.0f - 8.0f * scale);
					// The hint says what empty MEANS, not what the built-in path is:
					// a full absolute path as placeholder text reads as a value
					// that is already set, and it is repeated on the status line
					// below anyway.
					if (ImGui::InputTextWithHint("##datadir", S.dataDirEmptyHint, &dirEdit[i],
					                             ImGuiInputTextFlags_EnterReturnsTrue))
						applyPath(from_utf8(dirEdit[i]));
					// Enter is not the only way people finish typing: clicking away
					// used to discard the whole path silently.
					if (ImGui::IsItemDeactivatedAfterEdit()) applyPath(from_utf8(dirEdit[i]));
					if (!ImGui::IsItemActive()) dirEdit[i] = to_utf8(cfg.dataDir[i]);

					ImGui::SameLine(0, gapBtn);
					if (ImGui::Button(S.browse, ImVec2(btnW, 0))) {
						std::wstring picked = EdBrowseForFolder(
							L"選擇翻譯資料夾", cfg.dataDir[i].empty() ? builtin : cfg.dataDir[i]);
						if (!picked.empty()) applyPath(picked);
					}
					ImGui::SameLine(0, gapBtn);
					if (ImGui::Button(S.copyBuiltin, ImVec2(btnW, 0))) {
						copyDest = EdBrowseForFolder(L"複製內建翻譯資料到…",
						                             cfg.dataDir[i].empty() ? builtin : cfg.dataDir[i]);
						copyMsg.clear();
						copySlot = i;
						if (!copyDest.empty()) {
							if (DictionariesPresentAt(copyDest)) askOverwrite = true;
							else doCopy = true;
						}
					}
					ImGui::SameLine(0, gapBtn);
					if (ImGui::Button(S.clearPath, ImVec2(btnW, 0))) applyPath(std::wstring());

					// Status. Every failure mode says what is wrong AND what to do:
					// "no dictionaries here" on its own leaves the folder level to
					// be guessed, and it can be wrong in either direction.
					ImGui::PushTextWrapPos(inner - 40.0f * scale);
					const DictDirInfo& dd = dictDir[i];
					switch (dd.status) {
						case DataDirStatus::Builtin: {
							// Relative to the app folder. The absolute form is the
							// machine this happens to be installed on, which is not
							// what the reader is asking about -- they want to know
							// WHICH folder inside the program is being used. The
							// full path is one hover away for when it is.
							const std::string rel = std::string("Data\\") +
							                        to_utf8(DictSlotFolder((DictSlot)i)) + "\\";
							ImGui::TextDisabled("%s%s", S.dataDirBuiltin, rel.c_str());
							if (ImGui::IsItemHovered())
								ImGui::SetTooltip("%s", to_utf8(builtin).c_str());
							break;
						}
						case DataDirStatus::External: {
							// Path and contents on separate lines: run together they
							// wrap mid-path and neither is readable.
							ImGui::TextColored(warn, "%s%s", S.dataDirExternal, to_utf8(dd.root).c_str());
							std::string found;
							for (const auto& f : dd.found) {
								if (!found.empty()) found += "   ";
								found += f.first + " (" + std::to_string(f.second) + ")";
							}
							ImGui::TextDisabled("%s", found.c_str());
							if (i == (int)DictSlot::Launcher)
								ImGui::TextDisabled("%s", S.dataDirRestart);
							break;
						}
						case DataDirStatus::Missing:
							ImGui::TextColored(warn, "%s", S.dataDirMissing);
							break;
						case DataDirStatus::WrongShape:
						case DataDirStatus::TooShallow:
							ImGui::TextColored(warn, "%s", dd.status == DataDirStatus::WrongShape
							                                   ? S.dataDirWrongShape : S.dataDirTooShallow);
							if (!dd.suggestion.empty()) {
								ImGui::TextDisabled("%s", to_utf8(dd.suggestion).c_str());
								ImGui::SameLine(0, 8.0f * scale);
								if (ImGui::SmallButton(S.useSuggestion)) applyPath(dd.suggestion);
							}
							break;
						case DataDirStatus::NoDictionaries:
							ImGui::TextColored(warn, "%s", S.dataDirNoDict);
							break;
					}
					if (dd.insideInstall && dd.status != DataDirStatus::Builtin)
						ImGui::TextColored(warn, "%s", S.dataDirInside);
					if (!dd.staleLoadOrder.empty()) {
						std::string line = S.dataDirStale;
						for (const std::string& s : dd.staleLoadOrder) line += " " + s;
						ImGui::TextColored(warn, "%s", line.c_str());
					}
					ImGui::PopTextWrapPos();
					ImGui::Dummy(ImVec2(0, 4.0f * scale));
					ImGui::PopID();
				}

				if (!copyMsg.empty()) {
					ImGui::PushTextWrapPos(inner - 40.0f * scale);
					ImGui::TextDisabled("%s", copyMsg.c_str());
					ImGui::PopTextWrapPos();
				}

				// The update gate. Default on: most people want new-league
				// translations; only someone editing them wants to opt out.
				ImGui::Dummy(ImVec2(0, 6.0f * scale));
				ImGui::AlignTextToFramePadding();
				ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
				ImGui::TextUnformatted(S.transUpdateLabel);
				ImGui::PopStyleColor();
				{
					int tu = cfg.updateTranslations ? 0 : 1;
					ImGui::RadioButton(S.transUpdateOn, &tu, 0);
					ImGui::RadioButton(S.transUpdateOff, &tu, 1);
					const bool want = (tu == 0);
					if (want != cfg.updateTranslations) {
						cfg.updateTranslations = want;
						saveNow();
						// The worker applies packs on its own schedule, so the
						// setting has to reach it immediately, not at next start.
						if (appUpd) appUpd->SetTranslationUpdates(want);
					}
				}
			}

			ImGui::Dummy(ImVec2(0, 10.0f * scale));
			SectionLabel(fonts, scale, inner, S.sectionLaunch);
			// One radio group, not two checkboxes: "return afterwards" and "stay
			// open" cannot both be true, and a pair of checkboxes invites exactly
			// that state. See LaunchExitMode.
			{
				int em = (int)cfg.exitMode;
				ImGui::RadioButton(S.exitModeClose, &em, (int)LaunchExitMode::CloseLauncher);
				ImGui::RadioButton(S.returnAfterExit, &em, (int)LaunchExitMode::ReturnAfterExit);
				ImGui::RadioButton(S.exitModeKeepOpen, &em, (int)LaunchExitMode::KeepOpen);
				if (em != (int)cfg.exitMode) {
					cfg.exitMode = (LaunchExitMode)em;
					saveNow();
				}
			}

			ImGui::Dummy(ImVec2(0, 10.0f * scale));
			ImGui::AlignTextToFramePadding();
			ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
			ImGui::TextUnformatted(S.startupTabLabel);
			ImGui::PopStyleColor();
			{
				int st = (int)cfg.startupTab;
				ImGui::SameLine(160.0f * scale);
				ImGui::RadioButton(S.tabHome, &st, (int)StartupTab::Home);
				ImGui::SameLine(0, 18.0f * scale);
				ImGui::RadioButton(S.changelog, &st, (int)StartupTab::Versions);
				if (st != (int)cfg.startupTab) {
					cfg.startupTab = (StartupTab)st;
					saveNow();
				}
			}

			// Saving, at the very bottom and outside every group: it writes the
			// WHOLE file, and sitting inside the translation-data block made it
			// look like it only saved those three paths.
			ImGui::Dummy(ImVec2(0, 16.0f * scale));
			ImGui::Separator();
			ImGui::Dummy(ImVec2(0, 6.0f * scale));
			if (ImGui::Button(S.saveSettings, ImVec2(140.0f * scale, 0))) saveNow();
			if (ImGui::GetTime() < savedUntil) {
				ImGui::SameLine(0, 8.0f * scale);
				ImGui::TextColored(ImVec4(0.35f, 0.80f, 0.45f, 1.0f), "%s", S.settingsSaved);
			}
			ImGui::PushTextWrapPos(inner - 40.0f * scale);
			ImGui::TextDisabled("%s", S.saveSettingsHint);
			ImGui::PopTextWrapPos();

			ImGui::EndChild();
			ImGui::EndTabItem();
		}

		// --- about ------------------------------------------------------------
		if (tabsOk && ImGui::BeginTabItem(S.about)) {
			ImGui::BeginChild("##aboutbody", ImVec2(0, 0), false);
			ImGui::Dummy(ImVec2(0, 6.0f * scale));
			DrawAboutBody(S, fonts, scale, inner - 40.0f * scale);
			ImGui::Dummy(ImVec2(0, 12.0f * scale));
			ImGui::TextDisabled("PobTools v" POBTOOLS_VERSION_STRING "  ·  " __DATE__);
			ImGui::EndChild();
			ImGui::EndTabItem();
		}

		if (tabsOk) ImGui::EndTabBar();

		ImGui::End();

		// Overwrite confirmation for the copy button. Opened at this level (outside
		// the tab's child window) so the popup's ID stack does not depend on which
		// tab happens to be drawn.
		if (askOverwrite) { ImGui::OpenPopup("##copyconfirm"); askOverwrite = false; }
		if (ImGui::BeginPopupModal("##copyconfirm", nullptr,
		                           ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
			ImGui::PushTextWrapPos(420.0f * scale);
			ImGui::TextUnformatted(S.copyOverwrite);
			ImGui::TextDisabled("%s", to_utf8(copyDest).c_str());
			ImGui::PopTextWrapPos();
			ImGui::Dummy(ImVec2(0, 6.0f * scale));
			// The only button here that destroys someone's work.
			PobUi::PushDangerButton();
			if (ImGui::Button(S.overwriteConfirm, ImVec2(110.0f * scale, 0))) {
				doCopy = true;
				ImGui::CloseCurrentPopup();
			}
			PobUi::PopButtonStyle();
			ImGui::SameLine();
			if (ImGui::Button(S.cancel, ImVec2(110.0f * scale, 0))) {
				copyDest.clear();
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}
		if (doCopy) {
			doCopy = false;
			std::string cerr;
			int n = CopyBuiltinDictionary(exeDir, (DictSlot)copySlot, copyDest, &cerr);
			if (n < 0) {
				copyMsg = cerr;
			} else {
				copyMsg = std::string(S.copyDone) + std::to_string(n) + S.copyDoneSuffix;
				// Point at what was just created: copying and then having to browse
				// to the same folder by hand would be a pointless second step.
				cfg.dataDir[copySlot] = copyDest;
				resolveDict(copySlot);
				SaveLauncherConfig(exeDir + L"pob-zh.ini", cfg);
				savedUntil = ImGui::GetTime() + 3.0;
			}
			copyDest.clear();
		}

		ImGui::PopFont();
		ImGui::Render();

		int fbW = 0, fbH = 0;
		glfwGetFramebufferSize(win, &fbW, &fbH);
		glViewport(0, 0, fbW, fbH);
		glClearColor(0.043f, 0.063f, 0.078f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);
		ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
		glfwSwapBuffers(win);
	}

	syncCfgFromUi(); // host_main saves cfg after this returns

	// Full teardown so the next round (return-to-launcher) re-inits cleanly.
	ImGui_ImplOpenGL3_Shutdown();
	ImGui_ImplGlfw_Shutdown();
	ImGui::DestroyContext();
	glfwDestroyWindow(win);
	glfwTerminate();

	if (openEditor) return LauncherResult::OpenEditor;
	if (applyUpdate) return LauncherResult::ApplyAppUpdate;
	return launch ? LauncherResult::Launch : LauncherResult::Quit;
}

// ---------------------------------------------------------------- font coverage
//
// ImGui draws a '?' for any codepoint the loaded font has no glyph for, with no
// warning anywhere. That is how the version-history bullet shipped unreadable:
// "・" (U+30FB) exists in the default Noto Sans TC but NOT in FZ_ZY.ttf, so the
// defect was invisible to anyone who had not switched fonts.
//
// The check builds each shipped font headlessly (ImGui needs a context, not a
// window or a GL device) and asks FindGlyphNoFallback for every codepoint the
// launcher can draw. Adding a link label or a changelog line is now covered
// automatically, because both come from CollectLauncherTexts.
int RunFontCoverageSelftest(const std::wstring& exeDir)
{
	// EVERY installed language's launcher.json, not just the shipped one: once the
	// labels became translatable, a translator can type a character the shipped
	// font has no glyph for, and adding a language folder must not quietly escape
	// this check. The list comes from disk for exactly that reason.
	LauncherConfig cfg = LoadLauncherConfig(exeDir + L"pob-zh.ini");
	const std::wstring launcherRoot = ResolveDictDir(exeDir, DictSlot::Launcher,
	                                                 cfg.dataDir[(int)DictSlot::Launcher]).root;
	std::vector<LocaleInfo> locales = ListInstalledLocales(exeDir, cfg);
	std::vector<LauncherStringStore> strStore;
	strStore.reserve(locales.size()); // move-only; see LauncherStringStore
	for (const LocaleInfo& l : locales) {
		std::wstring wid(l.id.begin(), l.id.end()); // ids are ASCII folder names
		strStore.emplace_back(LoadLauncherStrings(launcherRoot, wid));
	}
	std::vector<const LauncherStrings*> overlays;
	overlays.reserve(strStore.size());
	for (const LauncherStringStore& st : strStore) overlays.push_back(&st.s);

	std::vector<const char*> texts;
	CollectLauncherTexts(texts, overlays);

	// unique codepoints, in first-seen order so the report reads like the source
	std::vector<unsigned> want;
	{
		std::vector<bool> seen(0x110000, false);
		for (const char* t : texts)
			ForEachCodepoint(t, [&](unsigned cp) {
				if (!seen[cp]) { seen[cp] = true; want.push_back(cp); }
			});
	}
	printf("font coverage: %d language(s) installed:", (int)locales.size());
	for (const LocaleInfo& l : locales) printf(" %s", l.id.c_str());
	printf("\n");

	// Does a language dropped in as a folder actually reach the atlas?
	//
	// Asserting "every installed language's characters are in `want`" would be
	// true by construction and prove nothing: the only language shipped today is
	// zh-rTW, whose launcher.json is byte-identical to the compiled table, so its
	// overlay contributes no character the compiled strings did not already have.
	// A throwaway language carrying a character NOTHING else uses is the only way
	// this check can fail when the wiring breaks.
	{
		const wchar_t* kProbeId = L"xx-TEST";
		const unsigned kProbeCp = 0x03A9;         // GREEK CAPITAL LETTER OMEGA
		const char* kProbeUtf8 = "\xce\xa9";      // in no compiled string
		const std::wstring dir = launcherRoot + kProbeId + L"\\";
		CreateDirectoryW(launcherRoot.c_str(), nullptr);
		CreateDirectoryW(dir.c_str(), nullptr);
		std::string body = std::string("{\"entries\":{\"") + STR_EN.tabHome + "\":\"" +
		                   kProbeUtf8 + "\"}}";
		HANDLE h = CreateFileW((dir + L"launcher.json").c_str(), GENERIC_WRITE, 0, nullptr,
		                       CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		bool wrote = false;
		if (h != INVALID_HANDLE_VALUE) {
			DWORD w = 0;
			wrote = WriteFile(h, body.data(), (DWORD)body.size(), &w, nullptr) != 0;
			CloseHandle(h);
		}
		CreateDirectoryW(dir.c_str(), nullptr);
		{
			HANDLE m = CreateFileW((dir + L"meta.json").c_str(), GENERIC_WRITE, 0, nullptr,
			                       CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (m != INVALID_HANDLE_VALUE) {
				const char* meta = "{\"display_name\":\"Probe\",\"load_order\":[\"launcher.json\"]}";
				DWORD w = 0;
				WriteFile(m, meta, (DWORD)strlen(meta), &w, nullptr);
				CloseHandle(m);
			}
		}

		LauncherStringStore probe = LoadLauncherStrings(launcherRoot, kProbeId);
		std::vector<const char*> t2;
		std::vector<const LauncherStrings*> ov2 = overlays;
		ov2.push_back(&probe.s);
		CollectLauncherTexts(t2, ov2);
		bool reached = false;
		for (const char* t : t2)
			ForEachCodepoint(t, [&](unsigned cp) { if (cp == kProbeCp) reached = true; });

		DeleteFileW((dir + L"launcher.json").c_str());
		DeleteFileW((dir + L"meta.json").c_str());
		RemoveDirectoryW(dir.c_str());

		printf("  [%s]  a dropped-in language's characters reach the glyph atlas\n",
		       (wrote && probe.overridden == 1 && reached) ? "PASS" : "FAIL");
		if (!(wrote && probe.overridden == 1 && reached)) {
			printf("         (wrote=%d overridden=%d reached=%d)\n",
			       (int)wrote, probe.overridden, (int)reached);
			return 1;
		}
		if (GetFileAttributesW(dir.c_str()) != INVALID_FILE_ATTRIBUTES) {
			printf("  [FAIL]  probe language folder was left behind: %s\n", to_utf8(dir).c_str());
			return 1;
		}
	}

	std::vector<std::wstring> fonts = ListAvailableFonts(exeDir);
	if (fonts.empty()) {
		printf("font coverage: no fonts under Fonts\ -- nothing to check\n");
		return 1;
	}
	printf("font coverage: %d distinct characters across %d font(s)\n",
	       (int)want.size(), (int)fonts.size());

	int bad = 0;
	for (const std::wstring& f : fonts) {
		const std::wstring path = ResolveFontPath(exeDir, f);
		ImGui::CreateContext();
		ImGuiIO& io = ImGui::GetIO();
		io.Fonts->Clear();

		static ImVector<ImWchar> ranges;
		ranges.clear();
		ImFontGlyphRangesBuilder b;
		b.AddRanges(io.Fonts->GetGlyphRangesDefault());
		for (const char* t : texts) b.AddText(t);
		for (const char* t : kOptionalScriptTexts) b.AddText(t);
		b.BuildRanges(&ranges);

		// read_file, not AddFontFromFileTTF: that one fopen()s a narrow path and
		// the exe may sit in a non-ASCII directory (same reason LoadFonts does it).
		std::vector<unsigned char> ttf = read_file(path);
		ImFont* font = nullptr;
		if (!ttf.empty()) {
			ImFontConfig cfg;
			cfg.FontDataOwnedByAtlas = false;
			font = io.Fonts->AddFontFromMemoryTTF(ttf.data(), (int)ttf.size(), 18.0f, &cfg, ranges.Data);
			if (font) io.Fonts->Build();
		}

		const std::string name = to_utf8(f);
		if (!font) {
			printf("  [FAIL] %s: could not be loaded\n", name.c_str());
			bad++;
			ImGui::DestroyContext();
			continue;
		}
		std::vector<unsigned> missing;
		for (unsigned cp : want) {
			if (cp > 0xFFFF) continue;  // ImWchar is 16-bit in this build
			if (!font->FindGlyphNoFallback((ImWchar)cp)) missing.push_back(cp);
		}
		// Reported, never failed: a TC font is not expected to carry these.
		{
			std::string absent;
			for (const char* t : kOptionalScriptTexts) {
				for (const unsigned char* p = (const unsigned char*)t; *p; p += (*p < 0x80 ? 1 : (*p & 0xE0) == 0xC0 ? 2 : (*p & 0xF0) == 0xE0 ? 3 : 4)) {
					unsigned cp = 0;
					if (*p < 0x80) cp = *p;
					else if ((*p & 0xE0) == 0xC0) cp = ((*p & 0x1Fu) << 6) | (p[1] & 0x3Fu);
					else if ((*p & 0xF0) == 0xE0) cp = ((*p & 0x0Fu) << 12) | ((p[1] & 0x3Fu) << 6) | (p[2] & 0x3Fu);
					else continue;
					if (cp <= 0xFFFF && !font->FindGlyphNoFallback((ImWchar)cp)) {
						wchar_t w[2] = { (wchar_t)cp, 0 };
						absent += to_utf8(w);
					}
				}
			}
			if (!absent.empty())
				printf("  [note] %s: no glyph for the optional script label(s) '%s'\n",
				       name.c_str(), absent.c_str());
		}
		if (missing.empty()) {
			printf("  [PASS] %s: draws all %d\n", name.c_str(), (int)want.size());
		} else {
			printf("  [FAIL] %s: %d character(s) would render as '?'\n",
			       name.c_str(), (int)missing.size());
			for (unsigned cp : missing) {
				wchar_t w[2] = { (wchar_t)cp, 0 };
				printf("           U+%04X  '%s'\n", cp, to_utf8(w).c_str());
			}
			bad++;
		}
		ImGui::DestroyContext();
	}
	printf("\n%s\n", bad == 0 ? "ALL PASS" : "FAILED");
	return bad == 0 ? 0 : 1;
}
