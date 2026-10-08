#include "launcher_ui.h"
#include <map>                  // RunFontCoverageSelftest's union bookkeeping
#include "editor_util.h"        // EdBrowseForFolder (one folder picker for the app)
#include "launcher_strings.h"
#include "launcher_strings_io.h"
#include "ui_theme.h"
#include "ui_widgets.h"
#include "ui_icons.h"
#include "app_version.h"
#include "image_tex.h"        // appearance-page thumbnails
#include "app_update.h"
#include "changelog.h"
#include "changelog_en.h"
#include "error_log.h"
#include "hang_watch.h"       // heartbeat, and the POB windows the watchdog asks after
#include "frame_pacing.h"     // idle wait, minimised = no present, unchanged frame = no present
#include "http_client.h"      // HttpSetManualProxy: the proxy setting acts immediately
#include "pob_launch.h"
#include "pob_protocol.h"     // the "Open in PoB" link switch on the settings page
#include "bridge_gate.h"
#include "modern_ui_window.h"  // ModernUiAvailable: whether the new-interface button exists
#include "modern_ui_browser.h" // ModernUiBrowserAvailable: the system-browser fallback
#include "window_dock.h"
#include "window_manager.h"   // DockTabLabel
// Tools that draw inside this window rather than in one of their own.
#include "atlas_planner.h"
#include "filter_editor.h"
#include "launcher_editor.h"
#include "regex_tool.h"
#include "warehouse_tool.h"
#include "timeless_jewel_ui.h"
#include "tool_panel.h"
#include "../translate/startup_trace.h"

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX   // std::min / std::max in the page layout code
#endif
#include <windows.h>
#include <shellapi.h>
#include <commdlg.h>             // "add an image" on the appearance page
#pragma comment(lib, "comdlg32.lib")

#include <GLES2/gl2.h>
#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>   // glfwGetWin32Window, for the docking container
#define IMGUI_DEFINE_MATH_OPERATORS
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <misc/cpp/imgui_stdlib.h> // InputText over std::string (the data-folder field)

#include <algorithm>
#include <atomic>
#include <cmath>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>

// Tools (filter editor / atlas planner / timeless jewel) run as child processes
// of the same exe so the launcher window stays open; spawning lives in
// PobLaunch::SpawnToolDetached, which also REMEMBERS the process. The launcher
// used to close the handle immediately and therefore had no way to know that a
// tool window was open — which the window-docking work needs.

// Reachable from GLFW's window callbacks, which are plain function pointers.
// One launcher window per process, so a file-scope pointer is enough.
//
// Deliberately NOT glfwSetWindowUserPointer: the ImGui GLFW backend claims that
// pointer for its own data.
static WindowDock::Dock* g_launcherDock = nullptr;
// Set by the refresh / framebuffer-size callbacks and by the atlas swaps: the
// next frame is presented even if its draw data matches the last one. See
// frame_pacing.h for why frames are otherwise skipped.
static bool g_launcherRedraw = true;

// Logical (unscaled) window sizes; multiplied by `scale` (monitor content scale
// times the user's font-size zoom, see LauncherZoom). The tabbed container holds
// POB, so its default is the size POB itself opens at. kMinWin* is the smallest
// the layout stays usable at: below ~900 the five-button tool row starts
// clipping its longest label, and both tab bodies scroll, so nothing breaks.
static const int kWinW = 1000;
static const int kWinH = 700;
static const int kTabbedWinW = 1500;
static const int kTabbedWinH = 950;
static const int kMinWinW = 900;
static const int kMinWinH = 560;
// Body face at 100% zoom. MUST equal kLauncherFontSizeDefault: the settings page
// shows the body size in px and the zoom is derived from that ratio.
static constexpr float kFontSize = 19.0f;
static_assert((int)kFontSize == kLauncherFontSizeDefault,
              "font-size setting is expressed as the body px; keep the two in step");
static const float kSmallFontSize = 15.0f;
static const float kTitleFontSize = 26.0f;
// Card and dialog headings: the design's 17 px heading at the 19/16 ratio.
static const float kHeadingFontSize = 20.0f;
static const float kBigFontSize = 30.0f;   // ToolPanelHost::big, digits only

// External-link board (wide layout). Labels feed the glyph atlas automatically
// (see the AddText loop below), so adding an entry needs no font work.
// The Discord and sponsor links are rendered after this list from
// LauncherStrings so they stay translated.
//
// `tr` decides which label a locale sees. Set: the string table's, so an
// English or Korean user reads "Official trade (PoE1)". Null: `label`, in every
// locale -- either because the label is a proper noun that does not translate
// (PoeDB, poe.ninja, FilterBlade) or because the thing behind the link IS
// Chinese (the Bahamut board, the two Chinese-localisation tools). Those stay
// listed and stay Chinese for everyone: hiding them would only make a reader
// think the localisation does not exist (user ruling, 2026-09-12).
struct LinkEntry {
	const char* label;
	const wchar_t* url;
	const char* LauncherStrings::*tr = nullptr;
};

// One array per column. The board grew to 18 entries once PoE2 and our own
// localisation tools were added, and a single row-major list read as a jumble:
// the PoE1 wiki and a Chrome Web Store page landed side by side for no reason.
// Reading DOWN a column is how a link board is scanned, so each game -- and our
// own tools -- gets a column of its own, headed and in a fixed order.
static const LinkEntry kLinksPoe1[] = {
	{ u8"官方網站",               L"https://www.pathofexile.com",       &LauncherStrings::linkOfficialSite },
	{ u8"官方交易市集（PoE1）",   L"https://www.pathofexile.com/trade", &LauncherStrings::linkTradePoe1 },
	{ u8"PoeDB 流亡編年史",       L"https://poedb.tw" },
	{ u8"PoE Wiki",               L"https://www.poewiki.net" },
	{ u8"poe.ninja",              L"https://poe.ninja" },
	{ u8"FilterBlade",            L"https://www.filterblade.xyz" },
	{ u8"拆粉查詢",               L"https://poe-disenchant-tool.vercel.app/allflame", &LauncherStrings::linkDisenchant },
	{ u8"Reddit r/pathofexile",   L"https://www.reddit.com/r/pathofexile/" },
	{ u8"巴哈姆特 PoE 板",        L"https://forum.gamer.com.tw/A.php?bsn=18966" },
};
static const LinkEntry kLinksPoe2[] = {
	{ u8"官方交易市集（PoE2）",   L"https://www.pathofexile.com/trade2", &LauncherStrings::linkTradePoe2 },
	{ u8"PoE2DB",                 L"https://poe2db.tw" },
	{ u8"PoE2 Wiki",              L"https://www.poe2wiki.net" },
	{ u8"poe.ninja（PoE2）",      L"https://poe.ninja/poe2" },
	{ u8"Reddit r/PathOfExile2",  L"https://www.reddit.com/r/PathOfExile2/" },
};
// Ours. Three install routes for the trade-site extension (the stores are what
// most people want; the GitHub release is for manual installs), then the price
// checker: ExileAppraiser, one app for both games, which replaced the separate
// PoE1 (awakened-poe-trade) and PoE2 (Exiled Exchange 2) forks on 2026-10-01.
static const LinkEntry kLinksTools[] = {
	{ u8"交易市集中文化（Chrome）",
	  L"https://chromewebstore.google.com/detail/poe-market-zh/ipnmbepaghlkapopikbpcchblhfkieed" },
	{ u8"交易市集中文化（Firefox）",
	  L"https://addons.mozilla.org/zh-TW/firefox/addon/poe-market-zh/" },
	{ u8"交易市集中文化（GitHub）",
	  L"https://github.com/Hsiung-Shao/poe-market-zh/releases/latest" },
	{ u8"流亡鑑價 查價器（PoE1／PoE2）",
	  L"https://github.com/Hsiung-Shao/exile-appraiser/releases/latest" },
};

struct LinkColumn {
	const char* head;                        // null -> use headTr
	const char* LauncherStrings::*headTr;
	const LinkEntry* items;
	int count;
};
static const LinkColumn kLinkColumns[] = {
	{ "PoE1", nullptr, kLinksPoe1, (int)(sizeof(kLinksPoe1) / sizeof(kLinksPoe1[0])) },
	{ "PoE2", nullptr, kLinksPoe2, (int)(sizeof(kLinksPoe2) / sizeof(kLinksPoe2[0])) },
	{ nullptr, &LauncherStrings::linkGroupTools, kLinksTools,
	  (int)(sizeof(kLinksTools) / sizeof(kLinksTools[0])) },
};
static const int kLinkColumnCount = (int)(sizeof(kLinkColumns) / sizeof(kLinkColumns[0]));

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
	out.push_back(kChangelogText);      // version-history dialog body (zh)
	out.push_back(kChangelogTextEn);    // ...and the one every other locale reads
	for (const LinkColumn& c : kLinkColumns) {
		if (c.head) out.push_back(c.head);
		for (int i = 0; i < c.count; i++) out.push_back(c.items[i].label);
	}
	out.push_back(u8"繁體中文Korean·"); // language combo labels + link separator
}

// Release history, split into releases for the version-history page.
//
// changelog.h is hard-wrapped at ~26 CJK characters per line, because it used to
// be drawn in a 600px modal. Those breaks are undone here and ImGui re-wraps at
// the real width.
//
// Structure, per the format contract with changelog.h:
//   "v" + digit           release header ("v1.7.9（2026-10-05）")
//   ""                    blank line between releases
//   U+3000 + "·" or "- "   bullet (both spellings exist across the history)
//   U+3000, anything else  continuation of the previous line -- folded back in
//                          (one U+3000 after a heading, two after a bullet)
//   anything else         section heading (修正 / 新增 / 調整)
//
// Historical entries are never edited (a standing project rule), so undoing the
// wrap at parse time is the only way to fix them.
struct ChangelogLine {
	bool heading = false;  // 修正 / 新增 / 調整
	bool bullet = false;
	std::string text;
};
struct ChangelogRelease {
	std::string version;   // "v1.7.9"
	std::string date;      // "2026-10-05", empty when the header has none
	std::string search;    // everything, lowercased ASCII, for the search box
	std::vector<ChangelogLine> lines;
};

static std::vector<ChangelogRelease> ParseChangelog(bool zh)
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

	// 1. fold continuations back into the line they belong to
	std::vector<std::string> lines;
	{
		const std::string log = zh ? kChangelogText : kChangelogTextEn;
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

	// 2. split into releases
	std::vector<ChangelogRelease> out;
	for (const std::string& raw : lines) {
		if (raw.empty()) continue;
		const bool isVer = raw.size() > 1 && raw[0] == 'v' && raw[1] >= '0' && raw[1] <= '9';
		if (isVer) {
			ChangelogRelease r;
			size_t end = 1;
			while (end < raw.size() && (isdigit((unsigned char)raw[end]) || raw[end] == '.')) end++;
			r.version = raw.substr(0, end);
			// the date sits in （） or (), whichever this entry used
			const size_t d = raw.find_first_of("0123456789", end);
			if (d != std::string::npos) {
				size_t de = d;
				while (de < raw.size() && (isdigit((unsigned char)raw[de]) || raw[de] == '-')) de++;
				if (de - d >= 8) r.date = raw.substr(d, de - d);
			}
			out.push_back(std::move(r));
			continue;
		}
		if (out.empty()) continue;   // nothing before the first header is drawn
		ChangelogLine l;
		std::string text = raw;
		if (startsWith(text, kIdeoSpace)) {
			text.erase(0, strlen(kIdeoSpace));
			if (startsWith(text, kMidDot)) { text.erase(0, strlen(kMidDot)); l.bullet = true; }
			else if (startsWith(text, "- ")) { text.erase(0, 2); l.bullet = true; }
		} else {
			l.heading = true;
		}
		l.text = text;
		out.back().lines.push_back(std::move(l));
	}
	for (ChangelogRelease& r : out) {
		std::string s = r.version + " " + r.date;
		for (const ChangelogLine& l : r.lines) s += "\n" + l.text;
		for (char& c : s) c = (char)tolower((unsigned char)c);
		r.search = std::move(s);
	}
	return out;
}

// Every colour below comes from the design tokens (PobUi::Tok, ui_theme.h).
namespace Tok = PobUi::Tok;
static ImU32 AccentAlpha(int alpha) { return (Tok::Accent & 0x00FFFFFFu) | ((ImU32)alpha << 24); }

// Lucide icon subset (ISC; generated by tools/gen_lucide_icons.py). Raw TTF
// bytes, NOT compressed: the atlas is built on two threads at once, and ImGui's
// stb_decompress keeps its state in file-scope globals. Read-only and static, so
// every atlas can point at it without a keep-alive.
#include "data/icons_lucide.inc"

// The primary face's ascent as a fraction of its line height, read from 'hhea'
// the way stb_truetype scales it (ascent / (ascent - descent)). The icon font
// sits on the baseline (its em box is all ascent), so this is what it takes to
// centre an icon on a line of text. 0.8 when the table cannot be read.
static float HheaAscentRatio(const std::vector<unsigned char>& d)
{
	auto u16 = [&](size_t o) { return (unsigned)(d[o] << 8 | d[o + 1]); };
	auto u32 = [&](size_t o) { return (unsigned)(d[o] << 24 | d[o + 1] << 16 | d[o + 2] << 8 | d[o + 3]); };
	if (d.size() < 12) return 0.8f;
	const unsigned n = u16(4);
	for (unsigned i = 0; i < n; i++) {
		const size_t rec = 12 + (size_t)i * 16;
		if (rec + 16 > d.size()) break;
		if (memcmp(&d[rec], "hhea", 4) != 0) continue;
		const size_t off = u32(rec + 8);
		if (off + 8 > d.size()) break;
		const int asc = (short)u16(off + 4), desc = (short)u16(off + 6);
		if (asc <= 0 || asc - desc <= 0) break;
		return (float)asc / (float)(asc - desc);
	}
	return 0.8f;
}

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
//
// Everything an atlas build reads that is NOT the atlas itself. ImGui stores only
// POINTERS to the TTF bytes and to the glyph-range arrays, so they must live at
// least as long as the atlas -- and the atlas may be built on a worker thread
// while the main thread rebuilds another one (the user picked a new font), so
// "static buffers reused by every build" is exactly the kind of sharing that
// turns into a use-after-free. Each build gets its own copy, kept alive by the
// LauncherFonts it produced.
struct FontBuildInput {
	std::shared_ptr<const std::vector<unsigned char>> ttf; // empty -> ImGui default font
	// The OTHER shipped fonts, merged into every face as glyph fallbacks (a
	// glyph the primary already has is skipped by ImGui's merge mode, so the
	// atlas only grows by the gaps). Noto Sans TC has no simplified-only
	// characters; FZ_ZY fills them.
	std::vector<std::shared_ptr<const std::vector<unsigned char>>> fallbacks;
	// The precise set: every string the launcher can draw, in every installed
	// language, plus `extraTexts`. Computed on the main thread (it reads the
	// string tables, which the translation editor may reload), so a worker never
	// has to look at them.
	ImVector<ImWchar> rangesPrecise;
	ImVector<ImWchar> rangesDigits; // "0123456789 /" for ToolPanelHost::big
	// The icon font's one range (PobIcon::kRangeFirst..Last), merged into body,
	// small, heading and title after the fallbacks.
	ImVector<ImWchar> rangesIcons;
	float primaryAscent = 0.8f;     // HheaAscentRatio of the primary face
	float scale = 1.0f;
	// Whatever the driver will take. Queried on the main thread (it needs the GL
	// context), never assumed: ANGLE reports 16384 on the D3D11 backend and as
	// little as 2048 on D3D9, and the difference decides whether the full CJK
	// block is possible at all on this machine. 0 = unknown, assume the worst.
	int maxTex = 0;
	// The interface language is Korean: when the full atlas does not fit, the
	// Hangul block is the LAST thing to give up, not the first (see the ladder
	// in LoadFonts). False for every other language, which keeps the pre-ko-KR
	// order byte for byte.
	bool preferKorean = false;
};

// Which glyphs a build covers. The launcher starts with Precise so its first
// frame is on screen in well under 100 ms, and swaps in a Full atlas built on a
// worker thread a few hundred milliseconds later. Only the body face differs.
enum class FontScope { Precise, Full };

struct LauncherFonts {
	ImFont* body = nullptr;
	ImFont* small = nullptr;
	ImFont* title = nullptr;
	ImFont* heading = nullptr;  // card / dialog headings (precise set)
	// Whether the Lucide icons made it into the faces. False on the last-resort
	// ASCII atlas, and then PobUi::Icon draws nothing.
	bool icons = false;
	// ToolPanelHost::big -- digits and '/' only, for the atlas planner's points
	// counter. Twelve glyphs, so it is built unconditionally rather than making the
	// panel lay itself out two different ways.
	ImFont* big = nullptr;
	bool koreanOk = false;
	bool cjkOk = false;
	FontScope scope = FontScope::Precise;

	// What the atlas actually came out as, and what the GPU will accept. Recorded
	// rather than assumed: ImGui reports an oversized atlas only through IM_ASSERT,
	// which is plain assert() here and compiled out in Release -- it would upload a
	// texture the driver rejects and draw nothing but blank quads, with no error.
	int texW = 0, texH = 0, maxTex = 0;
	// Empty when everything asked for fitted. Otherwise says what had to be given
	// up, so the launcher can show it instead of silently drawing '?' forever.
	// A Precise build never sets it: it did not try for the full block, so it has
	// not "dropped" anything, and the UI warning keys off "cjk" being here.
	std::string dropped;

	// Keep-alive for the pointers the atlas holds (see FontBuildInput). Shared,
	// so copying a LauncherFonts never moves the buffers the atlas points into.
	std::shared_ptr<const FontBuildInput> input;
	struct Ranges { ImVector<ImWchar> full; };
	std::shared_ptr<Ranges> ranges;
};

// The body face carries the WHOLE CJK block, not just the characters the string
// tables happen to contain.
//
// Tab labels now show POB's build name, which is arbitrary user text -- and a
// glyph that is not in the atlas is drawn as '?' with no warning anywhere. The
// same face is what the embedded tools will draw with, and they have always
// needed the full range for item and node names.
//
// Only the body face. At 19px the full block is about 8.5M px^2, which fits
// 4096 wide; doing the same to `small` (15px) and `title` (26px) as well would be
// roughly 29M px^2 -- over 7000 rows -- and blow past every common
// GL_MAX_TEXTURE_SIZE. Those two keep the precise set, which is all they draw.
static void BuildPreciseRanges(ImFontGlyphRangesBuilder& b,
                               const std::vector<std::string>& extraTexts,
                               const std::vector<const LauncherStrings*>& overlays)
{
	ImGuiIO& io = ImGui::GetIO();
	b.AddRanges(io.Fonts->GetGlyphRangesDefault());
	std::vector<const char*> texts;
	CollectLauncherTexts(texts, overlays);
	for (const char* t : texts) b.AddText(t);
	for (const char* t : kOptionalScriptTexts) b.AddText(t);
	for (const std::string& t : extraTexts) b.AddText(t.c_str());
}

// Main thread only (reads the string tables and, when `maxTexOverride` is 0, the
// GL context). `maxTexOverride` is for the headless check: with no GL context
// there is nothing to ask, so a selftest that let this query would only ever
// measure the smallest fallback and never the case that actually ships.
static std::shared_ptr<const FontBuildInput> PrepareFontInput(
    const std::wstring& fontPath, const std::vector<std::string>& extraTexts,
    const std::vector<const LauncherStrings*>& overlays, float scale, int maxTexOverride,
    const std::vector<std::wstring>& fallbackPaths = {}, bool preferKorean = false)
{
	auto in = std::make_shared<FontBuildInput>();
	in->ttf = std::make_shared<const std::vector<unsigned char>>(read_file(fontPath));
	in->scale = scale;
	in->preferKorean = preferKorean;
	for (const std::wstring& p : fallbackPaths) {
		auto buf = std::make_shared<const std::vector<unsigned char>>(read_file(p));
		if (!buf->empty()) in->fallbacks.push_back(std::move(buf));
	}
	{
		ImFontGlyphRangesBuilder b;
		BuildPreciseRanges(b, extraTexts, overlays);
		b.BuildRanges(&in->rangesPrecise);
	}
	{
		ImFontGlyphRangesBuilder b;
		b.AddText("0123456789 /");
		b.BuildRanges(&in->rangesDigits);
	}
	in->rangesIcons.push_back((ImWchar)PobIcon::kRangeFirst);
	in->rangesIcons.push_back((ImWchar)PobIcon::kRangeLast);
	in->rangesIcons.push_back(0);
	if (in->ttf && !in->ttf->empty()) in->primaryAscent = HheaAscentRatio(*in->ttf);
	GLint maxTex = (GLint)maxTexOverride;
	if (maxTex <= 0) {
		glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTex);
		if (maxTex <= 0) maxTex = 2048;  // no context / broken query: assume the worst
	}
	in->maxTex = (int)maxTex;
	return in;
}

// Builds the fonts into `atlas`. Safe on a worker thread as long as `atlas` is
// not the one the UI is drawing with: ImFontAtlas::Build reads only the atlas,
// stb_truetype and static range tables. The one global it does touch is ImGui's
// allocation counter (MemAlloc bumps GImGui->IO.MetricsActiveAllocations without
// atomics) -- a benign race that only skews the Metrics window, but it does mean
// the context must outlive the worker, and that this claim is worth re-checking
// on an ImGui upgrade. The RGBA conversion is done here too, so the main
// thread's CreateFontsTexture finds it ready and only pays for the upload.
//
// NOT safe with an empty TTF: AddFontDefault decompresses the built-in font
// through stb_decompress's file-scope globals, so two threads doing it at once
// corrupt each other. FontAtlasWorker::Start refuses that case.
static LauncherFonts LoadFonts(ImFontAtlas* atlas, std::shared_ptr<const FontBuildInput> in,
                               FontScope scope)
{
	LauncherFonts out;
	out.input = in;
	out.ranges = std::make_shared<LauncherFonts::Ranges>();
	out.scope = scope;
	out.maxTex = in->maxTex;

	if (!in->ttf || in->ttf->empty()) {
		out.body = atlas->AddFontDefault();
		out.small = out.body;
		out.title = out.body;
		out.heading = out.body;
		out.big = out.body;
		atlas->Build();
		return out;
	}
	const std::vector<unsigned char>& ttf = *in->ttf;
	const int maxTex = in->maxTex;

	ImFontConfig cfg;
	cfg.FontDataOwnedByAtlas = false; // shared buffer for all sizes; `in` keeps it alive
	cfg.OversampleH = 1;              // 3x the area otherwise, for no gain at these sizes
	cfg.OversampleV = 1;
	cfg.PixelSnapH = true;

	// Height is not rounded up to a power of two: at 19px that is the difference
	// between ~3200 rows and 4096, i.e. about 15MB of texture for nothing. NPOT with
	// CLAMP_TO_EDGE and no mipmaps is valid in GLES2, which is exactly how the ImGui
	// backend sets the atlas up.
	atlas->Flags |= ImFontAtlasFlags_NoPowerOfTwoHeight;
	// As wide as the GPU allows, up to 8192. Width and height trade off directly in
	// the packer, and height is the dimension that overflows: at 150% DPI the full
	// CJK block needs ~4600 rows at 4096 wide, which does not fit a 4096 limit -- but
	// at 8192 wide it needs ~2300 and fits easily. Capped at 8192 because past that
	// the atlas is one long strip and nothing is gained.
	atlas->TexDesiredWidth = maxTex >= 8192 ? 8192 : (maxTex >= 4096 ? 4096 : 2048);

	// Tries one combination and reports whether the result fits the GPU. Everything
	// is rebuilt from scratch each time -- Clear() drops the fonts as well as the
	// pixels, so the ImFont pointers from a rejected attempt are already dead.
	// `sizeMul` shrinks every face by the same factor. It is the last lever left
	// once there is nothing else to drop: the glyph SET is already minimal at that
	// point, so the only way to make the atlas smaller is to make the glyphs
	// smaller. Slightly small text is a cosmetic loss; an atlas over the GPU limit
	// is a window that draws nothing at all and says nothing about why.
	auto attempt = [&](bool fullCjk, bool korean, float sizeMul = 1.0f) -> bool {
		atlas->Clear();
		out.ranges->full.clear();
		{
			ImFontGlyphRangesBuilder b;
			b.AddRanges(in->rangesPrecise.Data);
			if (fullCjk) b.AddRanges(atlas->GetGlyphRangesChineseFull());
			if (korean) b.AddRanges(atlas->GetGlyphRangesKorean());
			b.BuildRanges(&out.ranges->full);
		}
		const float scale = in->scale * sizeMul;
		// Merged fallbacks: a glyph the primary face already has is skipped, so
		// the atlas grows only by the gaps (the simplified-only characters, for
		// the shipped pair). `in` keeps the buffers alive, same as the primary.
		ImFontConfig cfgMerge = cfg;
		cfgMerge.MergeMode = true;
		auto addFace = [&](float px, const ImWchar* ranges, bool icons) -> ImFont* {
			ImFont* f = atlas->AddFontFromMemoryTTF((void*)ttf.data(), (int)ttf.size(), px, &cfg, ranges);
			for (const auto& fb : in->fallbacks)
				atlas->AddFontFromMemoryTTF((void*)fb->data(), (int)fb->size(), px, &cfgMerge, ranges);
			if (icons) {
				// Last, so a text font that happens to use the same Private Use Area
				// codepoints keeps them (merge mode skips glyphs already present).
				// Lucide's em box is all ascent and the glyphs fill ~4%..96% of it:
				// placed as is, an icon would stand on the baseline and poke above
				// the line. Shift it so its box is centred on the text line.
				ImFontConfig ci = cfgMerge;
				ci.GlyphMinAdvanceX = px;
				// The icon's top is at (ascent - 0.96 px + offset) below the line top;
				// offset = px - ascent puts its 4%..96% box at 4%..96% of the line.
				ci.GlyphOffset.y = std::floor(px - std::floor(px * in->primaryAscent + 0.5f) + 0.5f);
				atlas->AddFontFromMemoryTTF((void*)kLucideTtf, (int)kLucideTtfSize, px, &ci, in->rangesIcons.Data);
			}
			return f;
		};
		out.body = addFace(kFontSize * scale, out.ranges->full.Data, true);
		out.small = addFace(kSmallFontSize * scale, in->rangesPrecise.Data, true);
		// No face of its own for headings: they draw the body face at
		// kHeadingFontSize (PobUi::WidgetFonts::headingPx). A fifth CJK face cost
		// enough atlas rows that a 2048-limited GPU had to shrink the whole UI one
		// display-scale step earlier than before.
		out.heading = out.body;
		out.title = addFace(kTitleFontSize * scale, in->rangesPrecise.Data, true);
		out.big = addFace(kBigFontSize * scale, in->rangesDigits.Data, false);
		if (!atlas->Build()) return false;
		out.texW = atlas->TexWidth;
		out.texH = atlas->TexHeight;
		// Height is judged against 8192 even on a 16384 GPU. Past that the atlas
		// is a ~450 MB texture (reachable only at the largest font-size setting on
		// a 200% monitor), and a launcher should not cost that much VRAM just to
		// keep Korean; the ladder below gives that up first.
		const int maxTexH = maxTex < 8192 ? maxTex : 8192;
		return out.texW <= maxTex && out.texH <= maxTexH;
	};

	if (scope == FontScope::Precise) {
		// The set the string tables need and nothing more: a few thousand glyphs,
		// built in tens of milliseconds, so the window can show its first frame
		// while the worker is still rasterising the full block.
		attempt(false, false);
	} else {
		// Widest first, then give up the least useful part FOR THE ACTIVE LANGUAGE.
		// Chinese is what every build name and item name is written in, so Korean
		// goes first -- except when the interface language is Korean: then the
		// Hangul block is what the user reads and the full CJK block goes first
		// (the precise set still carries every launcher string, so the UI itself
		// stays readable either way).
		const bool ko = in->preferKorean;
		if (!attempt(true, true)) {
			out.dropped = ko ? "cjk" : "korean";
			if (!(ko ? attempt(false, true) : attempt(true, false))) {
				out.dropped = "cjk";
				// The precise set for everything, i.e. the behaviour before tab titles
				// needed arbitrary text. Tab labels will show '?' for anything outside
				// the string tables, which is why `dropped` is surfaced in the UI.
				if (!attempt(false, false)) {
					// Still over the limit with the smallest set there is. Reachable
					// with a large CJK face at 200% on a 2048-limited GPU -- FZ_ZY did
					// exactly this (2048x2094, 46 rows over) and the old code simply
					// returned that atlas, which cannot be uploaded.
					//
					// Shrink until it fits. Every step is a real loss, so each one is
					// recorded rather than absorbed silently.
					bool fits = false;
					float mul = 1.0f;
					for (int step = 0; step < 6 && !fits; step++) {
						mul -= 0.1f;
						if (mul < 0.45f) break;
						fits = attempt(false, false, mul);
					}
					if (fits) {
						out.dropped = "cjk+shrunk";
						char why[192];
						snprintf(why, sizeof(why),
						         "font atlas would not fit the %d px GPU limit at %.2fx; "
						         "shrank the interface font to %.0f%% so it could be uploaded",
						         maxTex, in->scale, mul * 100.0f);
						PobLog::Error("i18n", why);
					} else {
						// Nothing this face can do. The built-in bitmap font is ASCII
						// only and always fits: an English interface beats a blank one.
						out.dropped = "font";
						char why[192];
						snprintf(why, sizeof(why),
						         "font atlas does not fit the %d px GPU limit at %.2fx even "
						         "shrunk; fell back to the built-in ASCII font",
						         maxTex, in->scale);
						PobLog::Error("i18n", why);
						atlas->Clear();
						out.ranges->full.clear();
						out.body = atlas->AddFontDefault();
						out.small = out.body;
						out.title = out.body;
						out.heading = out.body;
						out.big = out.body;
						atlas->Build();
						out.texW = atlas->TexWidth;
						out.texH = atlas->TexHeight;
					}
				}
			}
		}
	}

	if (out.body) {
		out.cjkOk = out.body->FindGlyphNoFallback((ImWchar)0x555F /* 啟 */) != nullptr;
		out.koreanOk = out.body->FindGlyphNoFallback((ImWchar)0xD55C /* 한 */) != nullptr;
	}
	if (!out.body) {
		out.body = atlas->AddFontDefault();
		out.small = out.body;
		out.title = out.body;
		out.heading = out.body;
		out.big = out.body;
		atlas->Build();
	}
	// Asked of every face the widgets draw icons with, not assumed from "the TTF
	// loaded": the ASCII last resort has none, and a face without them would draw
	// '?' where an icon belongs.
	out.icons = out.body && out.small && out.heading && out.title &&
	            out.body->FindGlyphNoFallback((ImWchar)PobIcon::kProbe) &&
	            out.small->FindGlyphNoFallback((ImWchar)PobIcon::kProbe) &&
	            out.heading->FindGlyphNoFallback((ImWchar)PobIcon::kProbe) &&
	            out.title->FindGlyphNoFallback((ImWchar)PobIcon::kProbe);
	// 18.8M pixels for the full block at 8192 wide: done here, off the main thread
	// when this is the worker, rather than inside the backend's CreateFontsTexture.
	{
		unsigned char* px = nullptr;
		int w = 0, h = 0;
		atlas->GetTexDataAsRGBA32(&px, &w, &h);
	}
	return out;
}

// The Full build on a worker thread. Owns the atlas until the main thread takes
// it (Take) or throws it away (Discard); both join first. Never outlives the ImGui
// context: ImGui::MemAlloc keeps a counter on the current context, so the worker
// must be gone before DestroyContext.
struct FontAtlasWorker {
	std::thread thread;
	std::atomic<bool> done{false};
	ImFontAtlas* atlas = nullptr;
	LauncherFonts fonts;

	bool Running() const { return atlas != nullptr; }

	void Start(std::shared_ptr<const FontBuildInput> in)
	{
		Discard();
		// No TTF: the Full build would be the same default font the main thread is
		// building right now, and building it on two threads at once corrupts
		// both (see LoadFonts). Nothing to swap in, so there is nothing to start.
		if (!in->ttf || in->ttf->empty()) return;
		done.store(false, std::memory_order_release);
		atlas = IM_NEW(ImFontAtlas)();
		ImFontAtlas* a = atlas;
		thread = std::thread([this, a, in]() {
			fonts = LoadFonts(a, in, FontScope::Full);
			done.store(true, std::memory_order_release);
		});
	}
	bool Done() const { return atlas && done.load(std::memory_order_acquire); }
	// The finished atlas; the caller now owns it.
	ImFontAtlas* Take(LauncherFonts* out)
	{
		if (thread.joinable()) thread.join();
		ImFontAtlas* a = atlas;
		atlas = nullptr;
		*out = fonts;
		fonts = LauncherFonts();
		return a;
	}
	void Discard()
	{
		if (thread.joinable()) thread.join();
		if (atlas) IM_DELETE(atlas);
		atlas = nullptr;
		fonts = LauncherFonts();
	}
	~FontAtlasWorker() { Discard(); }
};

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

// The PobTools mark: a rounded accent-soft square with the bolt. The icon font
// draws it when it is there; the hand-triangulated bolt is the fallback for the
// ASCII last-resort atlas.
static void DrawLogo(ImDrawList* dl, ImVec2 p, float size, bool icons)
{
	dl->AddRectFilled(p, p + ImVec2(size, size), Tok::AccentSoft, PobUi::D(size > PobUi::D(56.0f) ? 12.0f : 8.0f));
	const float glyph = std::floor(size * 0.5f);
	if (icons) {
		PobUi::IconAt(dl, p + ImVec2((size - glyph) * 0.5f, (size - glyph) * 0.5f), PobIcon::Zap, Tok::AccentText, glyph);
	} else {
		DrawBolt(dl, p + ImVec2((size - glyph) * 0.5f, (size - glyph) * 0.5f), glyph, Tok::AccentText);
	}
}

// __DATE__ ("Oct  8 2026") as 2026-10-08: the design writes dates ISO-style.
static std::string BuildDateIso()
{
	static const char* kMonths[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
	                                 "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
	const char* d = __DATE__;
	int month = 0;
	for (int i = 0; i < 12; i++)
		if (strncmp(d, kMonths[i], 3) == 0) month = i + 1;
	const int day = atoi(d + 4);
	const int year = atoi(d + 7);
	char buf[16];
	snprintf(buf, sizeof(buf), "%04d-%02d-%02d", year, month, day);
	return buf;
}

// GameCard (design system): badge | name + one meta line (or, when the install
// is missing, the reason and what to do) | status pill | the card's one action.
struct GameCardSpec {
	const char* badge = "P1";
	const char* name = "";
	std::string meta;           // "POB v2.67.2 · 新介面"
	const char* missing = nullptr;  // set when not found: the explanation
	PobUi::Tone pillTone = PobUi::Tone::Ok;
	std::string pill;
	const char* action = "";
	bool primary = true;        // the action is the screen's primary button
	bool enabled = true;
	const char* disabledTip = nullptr;
};

static bool GameCard(const char* id, const GameCardSpec& g, float width)
{
	using PobUi::D;
	const PobUi::WidgetFonts& wf = PobUi::Fonts();
	const float padX = D(20.0f), padY = D(16.0f), badge = std::floor(D(44.0f));
	const float btnW = PobUi::ButtonWidth(g.action, PobUi::BtnSize::Lg, nullptr, D(120.0f));
	const float pillW = PobUi::PillWidth(g.pill.c_str());
	const float textX = padX + badge + D(16.0f);
	const float textW = std::max(D(120.0f), width - textX - D(16.0f) - pillW - D(16.0f) - btnW - padX);
	const float nameH = wf.headingPx > 0.0f ? wf.headingPx : ImGui::GetFontSize();
	const char* sub = g.missing ? g.missing : g.meta.c_str();
	ImFont* subFont = wf.small ? wf.small : ImGui::GetFont();
	const ImVec2 subSz = subFont->CalcTextSizeA(subFont->FontSize, FLT_MAX, textW, sub);
	const float textH = nameH + D(2.0f) + subSz.y;
	const float h = std::max(badge, std::max(textH, std::floor(D(44.0f)))) + padY * 2.0f;

	ImGui::PushID(id);
	const ImVec2 p = ImGui::GetCursorScreenPos();
	ImGui::Dummy(ImVec2(width, h));
	const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
	ImDrawList* dl = ImGui::GetWindowDrawList();
	dl->AddRectFilled(p, p + ImVec2(width, h), Tok::Surface1, D(8.0f));
	dl->AddRect(p, p + ImVec2(width, h), hovered ? Tok::BorderStrong : Tok::Border, D(8.0f), 0, 1.0f);

	// badge
	const ImVec2 bp = p + ImVec2(padX, std::floor((h - badge) * 0.5f));
	const bool found = g.missing == nullptr;
	dl->AddRectFilled(bp, bp + ImVec2(badge, badge), found ? Tok::AccentSoft : Tok::Surface2, D(8.0f));
	if (wf.body) {
		const ImVec2 ts = wf.body->CalcTextSizeA(wf.body->FontSize, FLT_MAX, 0.0f, g.badge);
		dl->AddText(wf.body, wf.body->FontSize, bp + ImVec2(std::floor((badge - ts.x) * 0.5f), std::floor((badge - ts.y) * 0.5f)),
		            found ? Tok::AccentText : Tok::TextFaint, g.badge);
	}
	// name + meta / reason
	const float ty = p.y + std::floor((h - textH) * 0.5f);
	if (wf.heading)
		dl->AddText(wf.heading, nameH, ImVec2(p.x + textX, ty), found ? Tok::Text : Tok::TextMuted, g.name);
	dl->AddText(subFont, subFont->FontSize, ImVec2(p.x + textX, ty + nameH + D(2.0f)), Tok::TextMuted, sub, nullptr, textW);

	// pill + action, right-aligned and vertically centred
	const float btnH = std::floor(D(44.0f));
	float x = p.x + width - padX - btnW;
	ImGui::SetCursorScreenPos(ImVec2(x - D(16.0f) - pillW, p.y + std::floor((h - btnH) * 0.5f)));
	{
		// the pill is centred on the button's line
		ImGui::BeginGroup();
		const ImVec2 cp = ImGui::GetCursorScreenPos();
		ImGui::SetCursorScreenPos(cp + ImVec2(0, std::floor((btnH - D(24.0f)) * 0.5f)));
		PobUi::StatusPill(g.pillTone, g.pill.c_str());
		ImGui::EndGroup();
	}
	ImGui::SetCursorScreenPos(ImVec2(x, p.y + std::floor((h - btnH) * 0.5f)));
	const bool clicked = PobUi::Button(g.action, g.primary ? PobUi::BtnKind::Primary : PobUi::BtnKind::Secondary,
	                                   PobUi::BtnSize::Lg, nullptr, D(120.0f), g.enabled);
	if (!g.enabled && g.disabledTip && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
		PobUi::Tooltip(g.disabledTip);
	ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h));
	ImGui::Dummy(ImVec2(0, 0));
	ImGui::PopID();
	return clicked;
}

// Appearance page: a thumbnail per background image. Decoded and shrunk on a
// worker (a 4K JPEG takes a noticeable fraction of a second), uploaded on the
// main thread. A format stb cannot read (WebP) just stays a colour block.
struct ThumbPixels {
	std::vector<unsigned char> rgba;
	int w = 0, h = 0;
};
static ThumbPixels DecodeThumb(const std::wstring& path)
{
	ThumbPixels out;
	const std::vector<unsigned char> file = read_file(path);
	if (file.empty()) return out;
	int w = 0, h = 0;
	unsigned char* px = DecodeImageRGBA(file.data(), (int)file.size(), &w, &h);
	if (!px || w <= 0 || h <= 0) {
		if (px) FreeDecoded(px);
		return out;
	}
	// Box-filter down to at most 320 px wide: the tile is ~200 px, the preview
	// ~450, and a full-size texture per image would be tens of MB of VRAM.
	const int maxW = 320;
	const int step = w > maxW ? (w + maxW - 1) / maxW : 1;
	out.w = w / step;
	out.h = h / step;
	if (out.w <= 0 || out.h <= 0) { FreeDecoded(px); return ThumbPixels(); }
	out.rgba.resize((size_t)out.w * out.h * 4);
	for (int y = 0; y < out.h; y++) {
		for (int x = 0; x < out.w; x++) {
			unsigned sum[4] = { 0, 0, 0, 0 };
			for (int dy = 0; dy < step; dy++)
				for (int dx = 0; dx < step; dx++) {
					const unsigned char* s = px + ((size_t)(y * step + dy) * w + (x * step + dx)) * 4;
					for (int c = 0; c < 4; c++) sum[c] += s[c];
				}
			unsigned char* d = &out.rgba[((size_t)y * out.w + x) * 4];
			for (int c = 0; c < 4; c++) d[c] = (unsigned char)(sum[c] / (unsigned)(step * step));
		}
	}
	FreeDecoded(px);
	return out;
}

struct BgThumb {
	std::wstring file;
	unsigned tex = 0;
	int w = 0, h = 0;
	std::shared_ptr<std::future<ThumbPixels>> job;
};

// "Add an image..." on the appearance page.
static std::wstring OpenImageDialog(void* owner)
{
	wchar_t buf[MAX_PATH] = L"";
	OPENFILENAMEW ofn{};
	ofn.lStructSize = sizeof(ofn);
	ofn.hwndOwner = (HWND)owner;
	ofn.lpstrFilter = L"Images (*.png;*.jpg;*.jpeg;*.webp)\0*.png;*.jpg;*.jpeg;*.webp\0\0";
	ofn.lpstrFile = buf;
	ofn.nMaxFile = MAX_PATH;
	ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
	return GetOpenFileNameW(&ofn) ? std::wstring(buf) : std::wstring();
}

static int ClampOpacityThunk(int v, int /*fallback*/) { return ClampWindowOpacity(v); }

// POBTOOLS_LAUNCHER_SHOT: the back buffer as a 32-bit top-down BMP. Called
// between RenderDrawData and SwapBuffers, so it reads the frame just drawn.
static void WriteFramebufferBmp(const std::wstring& path, int w, int h)
{
	if (w <= 0 || h <= 0) return;
	std::vector<unsigned char> px((size_t)w * h * 4);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
	std::vector<unsigned char> out((size_t)w * h * 4);
	for (int y = 0; y < h; y++) {
		const unsigned char* src = &px[(size_t)(h - 1 - y) * w * 4];   // GL rows are bottom-up
		unsigned char* dst = &out[(size_t)y * w * 4];
		for (int x = 0; x < w; x++) {
			dst[x * 4 + 0] = src[x * 4 + 2];
			dst[x * 4 + 1] = src[x * 4 + 1];
			dst[x * 4 + 2] = src[x * 4 + 0];
			dst[x * 4 + 3] = 255;
		}
	}
	BITMAPFILEHEADER fh{};
	BITMAPINFOHEADER ih{};
	ih.biSize = sizeof(ih);
	ih.biWidth = w;
	ih.biHeight = -h;   // top-down
	ih.biPlanes = 1;
	ih.biBitCount = 32;
	ih.biCompression = BI_RGB;
	fh.bfType = 0x4D42;
	fh.bfOffBits = sizeof(fh) + sizeof(ih);
	fh.bfSize = fh.bfOffBits + (DWORD)out.size();
	HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (f == INVALID_HANDLE_VALUE) return;
	DWORD wr = 0;
	WriteFile(f, &fh, sizeof(fh), &wr, nullptr);
	WriteFile(f, &ih, sizeof(ih), &wr, nullptr);
	WriteFile(f, out.data(), (DWORD)out.size(), &wr, nullptr);
	CloseHandle(f);
}

LauncherResult ShowLauncher(LauncherConfig& cfg, const InstallInfo& installs, const std::wstring& exeDir,
                            AppUpdater* appUpd)
{
	if (!glfwInit()) {
		MessageBoxW(nullptr, L"無法初始化 GLFW，啟動器介面無法顯示。", L"PobTools", MB_ICONERROR | MB_OK);
		return LauncherResult::Quit;
	}
	startup_trace_mark("glfwInit done");

	// Same context setup as the engine (sys_video.cpp): GLES 3.0 via ANGLE/EGL.
	glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_ES_API);
	glfwWindowHint(GLFW_CONTEXT_CREATION_API, GLFW_EGL_CONTEXT_API);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
	// Resizable in both modes since v1.3.0; the size is remembered per mode (see
	// the poll in the main loop) and can also be typed on the settings page.
	glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
	glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE); // position first, then show

	// `dpiScale` is the monitor's; `scale` is what everything below multiplies
	// by, and folds in the font-size setting. Reassigned when that setting
	// changes (see the zoom-apply block in the main loop), so it is not const.
	float dpiScale = 1.0f;
	GLFWmonitor* monitor = glfwGetPrimaryMonitor();
	if (monitor) {
		float sx = 1.0f, sy = 1.0f;
		glfwGetMonitorContentScale(monitor, &sx, &sy);
		dpiScale = sx > 0.0f ? sx : 1.0f;
	}
	float scale = dpiScale * LauncherZoom(cfg.fontSize);

	// Tabbed window mode: this window becomes the container POB and the tools are
	// docked into. Decided here because the two modes have different default and
	// remembered sizes; everything else about it is gated further down, so
	// Separate mode runs exactly the code it ran before.
	const bool tabbed = (cfg.windowMode == WindowMode::Tabbed);
	int& storedW = tabbed ? cfg.tabWinW : cfg.winW;
	int& storedH = tabbed ? cfg.tabWinH : cfg.winH;
	// The new (WebView2) interface: decided once per launcher run. Without the
	// runtime, the loader DLL or the built page there is nothing to open, and a
	// button that opens a message box is worse than no button.
	// Under Wine / CrossOver (or without the WebView2 Runtime) the new interface
	// opens in the system browser instead, so the page being there is enough.
	const bool modernUiOk = ModernUiAvailable(exeDir, nullptr) || ModernUiBrowserAvailable(exeDir);
	const bool modernInBrowser = modernUiOk && !ModernUiAvailable(exeDir, nullptr);
	// Browser-mode sessions (either game) that are running, whoever started
	// them: the page can be closed without the program ending at once, so the
	// launcher is where the user sees it and ends it. Asked off the UI thread
	// every two seconds (a loopback request; a wedged server must not freeze
	// the launcher).
	// The addresses come along for the ride: under CrossOver the system browser
	// may not open, and then copying the address is the only way in.
	struct BrowserRunning { bool poe1 = false, poe2 = false; std::string url1, url2; };
	BrowserRunning browserRunning;
	std::future<BrowserRunning> browserPoll;
	double browserPollAt = -10.0;
	// The remembered compatibility verdict (PobTools\bridge_gate.json, written
	// by the new-interface window). Re-read every couple of seconds: the
	// window that just fell back to classic writes it while we are open.
	BridgeGate::Verdict modernGate = BridgeGate::Read(exeDir);
	// the bridge the verdict is compared with; re-hashed with the verdict (a
	// data-line update can replace bridge.lua while the launcher is open)
	std::string modernBridgeHash = modernGate.present && !modernGate.ok ? BridgeGate::BridgeFingerprint(exeDir) : std::string();
	double modernGateReadAt = glfwGetTime();
	// Per game: the verdict names the install it was taken against, so a
	// refused PoE1 POB never greys out PoE2's new interface or the other way.
	auto modernGateBlockedFor = [&](bool poe2) {
		return poe2 ? BridgeGate::BlocksModernUi(modernGate, installs.poe2Dir, installs.poe2Version, modernBridgeHash)
		            : BridgeGate::BlocksModernUi(modernGate, installs.poe1Dir, installs.poe1Version, modernBridgeHash);
	};
	auto modernGateBlocked = [&]() { return modernGateBlockedFor(false) || modernGateBlockedFor(true); };
	auto modernGateTipText = [&](const LauncherStrings& S) {
		std::string list;
		size_t n = modernGate.failed.size();
		for (size_t i = 0; i < n && i < 3; i++) list += (i ? "\n" : "") + modernGate.failed[i];
		if (n > 3) list += "\n…";
		char buf[1024];
		snprintf(buf, sizeof(buf), S.modernGateTip, (int)n, list.c_str());
		return std::string(buf);
	};

	// Monitor work area (screen minus taskbar), falling back to the video mode.
	// Physical pixels, like everything GLFW reports on Windows.
	auto workArea = [&](int* wx, int* wy, int* ww, int* wh) {
		*wx = *wy = *ww = *wh = 0;
		if (!monitor) return;
		glfwGetMonitorWorkarea(monitor, wx, wy, ww, wh);
		if (*ww > 0 && *wh > 0) return;
		if (const GLFWvidmode* mode = glfwGetVideoMode(monitor)) {
			*wx = *wy = 0;
			*ww = mode->width;
			*wh = mode->height;
		}
	};
	// The mode's default size at the CURRENT scale (reads `scale` by reference,
	// so it follows a zoom change).
	auto defaultWinSize = [&](int* w, int* h) {
		*w = (int)((tabbed ? kTabbedWinW : kWinW) * scale);
		*h = (int)((tabbed ? kTabbedWinH : kWinH) * scale);
	};
	// Never smaller than the layout can take, never larger than the screen: a
	// size remembered on a bigger monitor must still come up fully visible.
	auto clampWinSize = [&](int* w, int* h) {
		const int minW = (int)(kMinWinW * scale), minH = (int)(kMinWinH * scale);
		if (*w < minW) *w = minW;
		if (*h < minH) *h = minH;
		int wx, wy, ww, wh;
		workArea(&wx, &wy, &ww, &wh);
		if (ww > 0 && *w > ww) *w = ww;
		if (wh > 0 && *h > wh) *h = wh;
	};

	int winW = 0, winH = 0;
	if (storedW > 0 && storedH > 0) {
		winW = storedW;
		winH = storedH;
	} else {
		defaultWinSize(&winW, &winH);
	}
	clampWinSize(&winW, &winH);

	GLFWwindow* win = glfwCreateWindow(winW, winH, "PobTools", nullptr, nullptr);
	if (!win) {
		glfwTerminate();
		MessageBoxW(nullptr, L"無法建立啟動器視窗。", L"PobTools", MB_ICONERROR | MB_OK);
		return LauncherResult::Quit;
	}
	{
		// Centred on the work area, not the video mode: a window as tall as the
		// screen would otherwise sit half under the taskbar.
		int wx, wy, ww, wh;
		workArea(&wx, &wy, &ww, &wh);
		if (ww > 0 && wh > 0) glfwSetWindowPos(win, wx + (ww - winW) / 2, wy + (wh - winH) / 2);
	}
	glfwSetWindowSizeLimits(win, (int)(kMinWinW * scale), (int)(kMinWinH * scale),
	                        GLFW_DONT_CARE, GLFW_DONT_CARE);
	glfwMakeContextCurrent(win);
	glfwSwapInterval(1);
	// NOT shown yet: the window goes on screen right after its first frame has been
	// presented (see the main loop), so there is never a black window waiting for
	// the atlas. Until v0.24 it was shown here and stayed blank for ~300 ms.
	// FirstShow also puts it up kFirstShowLimit after this point if no frame has
	// been presented by then, so a stalled start is never an invisible process.
	FramePacing::FirstShow firstShow;
	firstShow.Start(glfwGetTime());
	startup_trace_mark("window created + GL context current");
	// The texture limit is the one thing the font worker needs from GL, and GL is
	// main-thread only, so it is read once here and handed over.
	GLint glMaxTex = 0;
	glGetIntegerv(GL_MAX_TEXTURE_SIZE, &glMaxTex);
	if (glMaxTex <= 0) glMaxTex = 2048;

	// Tabbed window mode: the docking half. The window itself was already sized
	// for the mode above.
	WindowDock::Dock dock;
	if (tabbed) {
		dock.Init(glfwGetWin32Window(win), exeDir + L"PobTools\\dock_log.txt");
		g_launcherDock = &dock;
		// Dragging a window puts Windows into a modal message loop during which
		// glfwPollEvents never returns, so without these the docked window is
		// left behind for the whole drag.
		glfwSetWindowPosCallback(win, [](GLFWwindow*, int, int) {
			if (g_launcherDock) g_launcherDock->OnHostMoved();
		});
		glfwSetWindowSizeCallback(win, [](GLFWwindow*, int, int) {
			if (g_launcherDock) g_launcherDock->OnHostMoved();
		});
		// Activating the container is what buries the docked window, and that is
		// exactly what finishing a drag does.
		glfwSetWindowFocusCallback(win, [](GLFWwindow*, int focused) {
			if (focused && g_launcherDock) g_launcherDock->OnHostFocused();
		});
	}

	// Frames whose draw data did not change are not presented (frame_pacing.h),
	// so anything that invalidates what is on screen without changing the draw
	// data has to say so: a WM_PAINT-style refresh (uncovered, restored) and a
	// resize (the framebuffer behind the old picture is gone). Installed before
	// the ImGui backend, which chains the callbacks it needs and leaves these.
	glfwSetWindowRefreshCallback(win, [](GLFWwindow*) { g_launcherRedraw = true; });
	glfwSetFramebufferSizeCallback(win, [](GLFWwindow*, int, int) { g_launcherRedraw = true; });
	g_launcherRedraw = true;

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

	// Someone who unzipped PobTools-update-<ver>.zip on its own gets a program
	// that starts normally, shows a Chinese launcher (the compiled string table
	// covers that) and an entirely English POB, with no error anywhere. Nothing
	// else catches it: validate_app_stage passes on that pack by design, and it
	// only runs on an update stage, never at startup. So the launcher looks for
	// itself. Throttled rather than computed once, because the banner has to
	// disappear on its own after the download button has done its job.
	bool builtinDictsPresent = true;
	double dictProbeAt = -1.0;

	// Languages come from the folders on disk, so adding Data\poe1\ja-JP\ is all
	// it takes to offer Japanese. "en" is always first and needs no folder.
	std::vector<LocaleInfo> locales = ListInstalledLocales(exeDir, cfg);
	// Everything the language picker prints must be in the precise atlas too. A
	// display name is arbitrary translator text from meta.json ("한국어"), not a
	// string-table entry, so it is listed explicitly -- kOptionalScriptTexts only
	// happens to cover the two scripts known today.
	std::vector<std::string> atlasTexts = { poe1Dir, poe2Dir };
	for (const LocaleInfo& l : locales) atlasTexts.push_back(l.displayName);

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

	startup_trace_mark("locales + launcher strings loaded");
	// Two atlases from one input: the precise set right now, on this thread, so
	// the first frame is a few tens of milliseconds away; the full CJK block on a
	// worker, swapped in by the main loop when it is done (typically ~300 ms
	// later). Until then a character outside the string tables -- a build name in
	// a tab title, say -- draws as '?', and corrects itself on the swap.
	std::shared_ptr<const FontBuildInput> fontInput = PrepareFontInput(
	    ResolveFontPath(exeDir, cfg.fontFile), atlasTexts, strOverlays, scale, glMaxTex,
	    FallbackFontPaths(exeDir, cfg.fontFile), cfg.locale == L"ko-KR");
	FontAtlasWorker fontWorker;
	fontWorker.Start(fontInput);
	LauncherFonts fonts = LoadFonts(ImGui::GetIO().Fonts, fontInput, FontScope::Precise);
	startup_trace_mark("precise font atlas built (%dx%d); full atlas building in the background",
	                   fonts.texW, fonts.texH);
	std::vector<std::wstring> fontList = ListAvailableFonts(exeDir);
	bool fontChanged = false;
	// Recomputed with the atlas, never independently: the answer is a property of
	// the atlas that was just built, not of the font file.
	std::vector<std::string> localeMissing;
	std::vector<bool> localeDrawable = ProbeLocaleCoverage(fonts, strStore, &localeMissing);

	ImGui_ImplGlfw_InitForOpenGL(win, true);
	ImGui_ImplOpenGL3_Init("#version 100");
	startup_trace_mark("ImGui backends initialised");
	if (GetEnvironmentVariableW(L"POBTOOLS_LAUNCHER_SHOT", nullptr, 0) == 0 && firstShow.Due(false, glfwGetTime()))
		glfwShowWindow(win); // only past the limit; shot mode never shows

	// Pre-select an available game if the remembered one is missing.
	bool poe2Sel = (cfg.game == L"poe2");
	if (poe2Sel && installs.poe2Lua.empty() && !installs.poe1Lua.empty()) poe2Sel = false;
	if (!poe2Sel && installs.poe1Lua.empty() && !installs.poe2Lua.empty()) poe2Sel = true;

	// Falls back to zh-rTW, then en, when the configured language's folder is gone.
	int localeIdx = PickLocaleIndex(locales, cfg.locale);

	bool launch = false;
	bool openEditor = false;
	bool applyUpdate = false;

	// Has the user started anything in this launcher session? It gates the
	// automatic update: closing and reopening the launcher is free while nothing
	// is open, and an interruption the moment anything is. Set by every path that
	// starts POB, a tool window or an embedded panel.
	bool anythingLaunched = false;
	// One shot for the whole process. A failed apply comes back here with the
	// updater re-Init'ed, and without this the launcher would download, fail and
	// download again forever; the orange button still works after a failure.
	static bool autoApplyTried = false;

	// Game and language live in widget state (poe2Sel / localeIdx), not in cfg, so
	// cfg is stale until this runs. Every path that writes the ini must call it
	// first -- the "Save settings" button used to write the OLD language back,
	// which is exactly the kind of thing that makes a save button untrustworthy.
	auto syncCfgFromUi = [&]() {
		cfg.game = poe2Sel ? L"poe2" : L"poe1";
		if (localeIdx >= 0 && localeIdx < (int)locales.size())
			cfg.locale = from_utf8(locales[localeIdx].id);
	};

	// tools spawn as child processes so this window stays open; the kind is what
	// lets them be told apart later without parsing window titles
	auto spawnTool = [&](const wchar_t* flag, PobLaunch::InstanceKind kind, const char* label) {
		syncCfgFromUi();
		SaveLauncherConfigKeepModern(exeDir + L"pob-zh.ini", cfg);
		unsigned long pid = 0;
		if (!PobLaunch::SpawnToolDetached(exeDir, flag, kind, &pid)) return;
		anythingLaunched = true;
		// In tabbed mode the tool becomes a tab here rather than a window of its
		// own; in separate mode nothing else happens, exactly as before. The new
		// interface is the exception: the dock only adopts GLFW windows
		// (window_dock.cpp find_cb) and its WebView2 window is plain Win32, so it
		// stays a window of its own in both modes.
		if (tabbed && kind != PobLaunch::InstanceKind::ModernUi) dock.Track(pid, from_utf8(label));
	};
	// KeepOpen mode: start POB the same way the tools are started (detached, the
	// window stays up) instead of returning Launch. ShowLauncher tears down GLFW
	// and ImGui before it returns, so "return a result" could never keep the
	// window alive, let alone allow a second POB while the first is running.
	auto launchPob = [&](bool poe2) {
		syncCfgFromUi();
		cfg.game = poe2 ? L"poe2" : L"poe1"; // the row that was clicked, not the selection
		SaveLauncherConfigKeepModern(exeDir + L"pob-zh.ini", cfg);   // the child's safety net
		// Only a validated external folder is passed on (see the settings page),
		// and only the one for the game being started; a broken path leaves POB on
		// the built-in dictionaries.
		const int slot = poe2 ? (int)DictSlot::Poe2 : (int)DictSlot::Poe1;
		const AppearanceConfig& look = cfg.look[GameIndex(cfg.game)];
		PobLaunch::SetEngineEnv(cfg.game, cfg.locale, cfg.fontFile,
		                        dictDir[slot].status == DataDirStatus::External
		                            ? dictDir[slot].root : std::wstring(),
		                        cfg.fontApplyAll, look.windowOpacity,
		                        ResolveBackgroundPath(exeDir, look.background), look.bgBright, look.glassBlur,
		                        look.treeBg, cfg.hangWatch,
		                        cfg.pobFpsForeground, cfg.pobFpsBackground, cfg.perfLog);
		const std::wstring lua = poe2 ? installs.poe2Lua : installs.poe1Lua;
		if (lua.empty()) {
			// Nothing to launch and, until v0.28.0, nothing said about it: the
			// button just did not respond. Say which game and where we looked.
			PobLog::Error("pob", std::string("no POB install detected for ") +
			                         (poe2 ? "poe2" : "poe1") +
			                         "; nothing to launch (looked next to pob-zh.exe and in the "
			                         "configured POB folder)");
			return;
		}
		unsigned long pid = 0;
		if (!PobLaunch::SpawnPobDetached(lua, cfg.game, &pid)) return;
		anythingLaunched = true;
		// Tabbed mode docks it into this window; separate mode leaves it as its
		// own desktop window, which is what it has always done.
		if (tabbed) dock.Track(pid, poe2 ? L"PoE2" : L"PoE1");
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
	std::string proxyEdit = to_utf8(cfg.proxy);
	std::string fontMsg;       // result of the last "install a font" attempt
	double savedUntil = 0.0;   // "saved" confirmation deadline
	// "restart to apply" notice for the window-mode switch; a deadline rather than
	// a bool so it outlives the frame the click happened in.
	double windowModeChangedUntil = 0.0;
	// Font-size slider: edited copy while dragging, and the scale the main loop
	// should switch to once the drag ends (0 = nothing pending). The switch is
	// done between frames, never from inside the widget: it rebuilds styles and
	// the atlas, and both must happen with an empty ImGui stack.
	int   fontSizeEdit = cfg.fontSize;
	float pendingScale = 0.0f;
	// Window size: the two fields on the settings page, the last size the poll
	// saw, and the debounce for remembering a drag. `lastW == 0` means the poll
	// has not seeded yet -- the startup size is never written back as a change.
	int    winEdit[2] = { 0, 0 };
	// Appearance tab: which game's set is being edited, and slider scratch
	// values (they mirror cfg.look[lookGame] whenever idle, so switching the
	// game re-syncs them on the next frame).
	int    lookGame = GameIndex(cfg.game);
	int    opacityEdit = cfg.look[lookGame].windowOpacity;
	int    bgBrightEdit = cfg.look[lookGame].bgBright;
	int    glassEdit = cfg.look[lookGame].glassBlur;
	int    treeBgEdit = cfg.look[lookGame].treeBg;
	std::vector<std::wstring> bgList = ListAvailableBackgrounds(exeDir);
	int    lastW = 0, lastH = 0;
	double sizeStableAt = 0.0;
	bool   sizeDirty = false;

	// EVERY settings change writes the ini immediately. Half-immediate is worse
	// than either extreme: some fields used to persist on change and the rest only
	// when the window closed, so whether a change survived depended on which
	// widget it was -- and nothing on screen said which. The "Save settings"
	// button now only exists to say "yes, it is written", not to be the one way
	// changes take effect.
	auto saveNow = [&]() {
		syncCfgFromUi(); // language / game are widget state until now
		SaveLauncherConfigKeepModern(exeDir + L"pob-zh.ini", cfg);
		savedUntil = ImGui::GetTime() + 3.0;
	};
	// Resize the window from the settings page. `remember` decides what the ini
	// gets: the typed size, or 0 ("mode default") for the reset button -- the
	// window is resized either way. Updates the poll's last-seen size so the
	// change is not re-detected as a drag.
	auto applyWindowSize = [&](int w, int h, bool remember) {
		clampWinSize(&w, &h);
		glfwSetWindowSize(win, w, h);
		lastW = w;
		lastH = h;
		sizeDirty = false;
		storedW = remember ? w : 0;
		storedH = remember ? h : 0;
		saveNow();
	};

	double transNoticeUntil = 0.0; // TransDone banner auto-dismiss deadline
	double lastPeerSync = 0.0;     // when the watchdog last got the POB window list
	// A check the user asked for must report back even when the answer is "no
	// news"; the automatic startup one stays silent.
	bool manualCheck = false;
	double upToDateUntil = 0.0;
	bool wasPobBusy = false;       // edge-detect "the last POB just closed"
	bool applyStartupTab = true;   // honour cfg.startupTab on the first frame only

	// ---- launcher pages (design system) -------------------------------------
	// Which page is showing (last frame), which one this frame turned out to be,
	// and a page to bring to the front once (-1 = none).
	int pageShown = 0, pageNow = 0, pageForce = -1;
	int startSection = -1;           // POBTOOLS_LAUNCHER_TAB=settings:N
	bool launcherTabForce = false;   // tabbed: bring the launcher tab forward once
	// POBTOOLS_LAUNCHER_TAB=home|versions|settings|appearance|about opens on that
	// page. A testing aid (screenshots without clicking): it only picks the first
	// page and is never written to the ini.
	{
		wchar_t buf[32] = L"";
		const DWORD n = GetEnvironmentVariableW(L"POBTOOLS_LAUNCHER_TAB", buf, 32);
		if (n > 0 && n < 32) {
			const std::wstring v(buf);
			if (v == L"home") pageForce = 0;
			else if (v == L"versions") pageForce = 1;
			else if (v == L"settings") pageForce = 2;
			// settings:N also scrolls to settings group N (1..7)
			else if (v.size() == 10 && v.compare(0, 9, L"settings:") == 0 && v[9] >= L'1' && v[9] <= L'7') {
				pageForce = 2;
				startSection = v[9] - L'1';
			}
			else if (v == L"appearance") pageForce = 3;
			else if (v == L"about") pageForce = 4;
		}
	}
	bool bannersExpanded = false;
	bool linksExpanded = false;
	// version history
	std::vector<ChangelogRelease> changelog;
	int clZh = -1;                   // which language `changelog` was parsed for
	std::string clSearch;
	std::vector<int> clVisible;
	int clActive = 0, clJump = -1;
	float clLockY = -1.0f;
	// settings: side navigation
	constexpr int kSettingsSections = 7;
	int setActive = startSection >= 0 ? startSection : 0, setJump = startSection;
	float setLockY = -1.0f;
	// "Open in PoB" registration state, re-read every couple of seconds
	int protoState = (int)PobProtocol::State::None;
	double protoQueriedAt = -10.0;
	// "Restart now" for the window-mode switch: the close sequence runs as for
	// any close (tabs and panels are asked first), then ShowLauncher returns
	// Relaunch and host_main opens a fresh launcher in the new mode.
	bool relaunchRequested = false;
	// POBTOOLS_LAUNCHER_SHOT=<file.bmp>: render without ever showing the window,
	// save one frame and quit. With POBTOOLS_LAUNCHER_TAB it gives a screenshot
	// of any page with no window on screen and no input sent.
	std::wstring shotPath;
	{
		wchar_t buf[MAX_PATH] = L"";
		const DWORD n = GetEnvironmentVariableW(L"POBTOOLS_LAUNCHER_SHOT", buf, MAX_PATH);
		if (n > 0 && n < MAX_PATH) shotPath = buf;
	}
	const bool shotMode = !shotPath.empty();
	double shotSince = glfwGetTime();
	bool fontInputFullDone = false;
	// appearance: thumbnails, and which game the slider scratch values belong to
	int lookGameShown = -1;
	std::vector<BgThumb> bgThumbs;
	auto syncThumbs = [&]() {
		std::vector<BgThumb> next;
		for (const std::wstring& f : bgList) {
			bool kept = false;
			for (BgThumb& t : bgThumbs)
				if (t.file == f) {
					next.push_back(std::move(t));
					t.tex = 0;   // a moved int is copied, not cleared: the new owner has it
					t.file.clear();
					kept = true;
					break;
				}
			if (kept) continue;
			BgThumb t;
			t.file = f;
			const std::wstring path = ResolveBackgroundPath(exeDir, f);
			t.job = std::make_shared<std::future<ThumbPixels>>(
			    std::async(std::launch::async, [path]() { return DecodeThumb(path); }));
			next.push_back(std::move(t));
		}
		for (BgThumb& t : bgThumbs)
			if (t.tex) DeleteTexture(t.tex);   // the ones no longer listed
		bgThumbs = std::move(next);
	};

	// Tools drawn inside this window rather than started as their own process.
	// Tabbed mode only -- separate mode still spawns them, exactly as before, so
	// that path is untouched by any of this.
	struct EmbeddedPanel {
		std::unique_ptr<IToolPanel> panel;
		std::string label;
		// ImGuiTabItemFlags_SetSelected for one frame. Needed when a panel is asked
		// to close and answers by putting up a prompt: the prompt has to be on the
		// tab the user is looking at, not behind whichever tab happens to be active.
		ImGuiTabItemFlags forceSelect = 0;
	};
	std::vector<EmbeddedPanel> panels;

	// One prebuilt style per density, so a tab swap is an assignment rather than a
	// rebuild. ApplyTheme cannot be called per frame: it ends in ScaleAllSizes,
	// which compounds.
	ImGuiStyle styleComfortable, styleCompact, styleCanvas;
	PobUi::BuildStyle(styleComfortable, scale, PobUi::Density::Comfortable);
	PobUi::BuildStyle(styleCompact, scale, PobUi::Density::Compact);
	PobUi::BuildStyle(styleCanvas, scale, PobUi::Density::Canvas);
	auto styleFor = [&](PobUi::Density d) -> const ImGuiStyle& {
		switch (d) {
			case PobUi::Density::Canvas: return styleCanvas;
			case PobUi::Density::Compact: return styleCompact;
			default: return styleComfortable;
		}
	};

	// Lent to every panel and OUTLIVES them all, because they keep a pointer into
	// it. `body` is refreshed each frame rather than copied once: the atlas is
	// rebuilt when the user changes font, and every ImFont* from before that is
	// dangling afterwards.
	ToolPanelHost panelHost;
	panelHost.exeDir = exeDir;
	panelHost.locale = cfg.locale;
	panelHost.scale = scale;
	panelHost.hostHwnd = glfwGetWin32Window(win);
	panelHost.embedded = true;

	// A panel that could not load its data, waiting for a safe moment to say so.
	std::string panelInitError;

	// Open a tool as a tab, or bring the one already open to the front. One
	// instance each -- two translation editors would be writing the same files, and
	// the tools keep enough state (a whole passive tree, the scarab icon cache) that
	// a second copy is not free either.
	auto openPanel = [&](IToolPanel* (*make)(), const char* label) {
		anythingLaunched = true;   // tabbed mode's tools count as "in use" too
		std::unique_ptr<IToolPanel> fresh(make());
		for (EmbeddedPanel& ep : panels) {
			if (std::string(ep.panel->PanelId()) == fresh->PanelId()) {
				ep.forceSelect = ImGuiTabItemFlags_SetSelected;
				return;
			}
		}
		panelHost.game = cfg.game;
		panelHost.locale = cfg.locale;
		if (!fresh->Init(panelHost)) {
			// Held for the deferred section rather than shown here: this runs in the
			// middle of a frame. See IToolPanel::InitError.
			panelInitError = fresh->InitError();
			if (!panelInitError.empty())
				PobLog::Error("panel", std::string(fresh->PanelId() ? fresh->PanelId() : "?") +
				                           u8" 面板初始化失敗：" + panelInitError);
			return;
		}
		EmbeddedPanel ep;
		ep.panel = std::move(fresh);
		ep.label = label;
		ep.forceSelect = ImGuiTabItemFlags_SetSelected;
		panels.push_back(std::move(ep));
	};

	bool closingTabs = false;      // tabbed mode: shutting down, closing tabs in turn
	bool closingPanels = false;    // ... and the embedded ones, which answer over frames
	unsigned long closingPid = 0;  // the tab already asked to close, so it is asked once
	double closeAskedAt = 0.0;     // when, so a cancelled save prompt can be detected
	// Pacing (frame_pacing.h): the loop waits for events instead of spinning,
	// and only presents frames whose draw data changed. `nextWait` is decided at
	// the bottom of each iteration; zero on the first so the window comes up at
	// once.
	FramePacing::Pacer pacer;
	double nextWait = 0.0;
	while (!glfwWindowShouldClose(win) && !launch && !openEditor && !applyUpdate) {
		if (nextWait > 0.0) glfwWaitEventsTimeout(nextWait);
		else glfwPollEvents();
		pacer.BeginFrame(glfwGetTime());

		// The heartbeat the watchdog thread is watching. One store per frame; if
		// it stops for good, that thread is what writes down where we stopped.
		HangWatch::Beat();

		// Remember a drag-resize once it has settled for half a second. Polled
		// rather than hooked: the size callback is the dock's in tabbed mode, and
		// a callback fires for every pixel of a drag anyway.
		//
		// Minimising is delivered as a 0x0 WM_SIZE (and a WM_MOVE to -32000), so
		// the iconified state and non-positive sizes are skipped outright -- this
		// exact path once parked the docked POB window off-screen. A maximised
		// size is skipped too: restoring it later would give a windowed launcher
		// the size of the whole screen.
		{
			int w = 0, h = 0;
			glfwGetWindowSize(win, &w, &h);
			if (w > 0 && h > 0 && !glfwGetWindowAttrib(win, GLFW_ICONIFIED) &&
			    !glfwGetWindowAttrib(win, GLFW_MAXIMIZED)) {
				if (lastW == 0) {
					lastW = w; // seed only; the startup size is not a change
					lastH = h;
				} else if (w != lastW || h != lastH) {
					lastW = w;
					lastH = h;
					sizeStableAt = glfwGetTime(); // ImGui::GetTime is not valid before NewFrame
					sizeDirty = true;
				} else if (sizeDirty && glfwGetTime() - sizeStableAt > 0.5) {
					sizeDirty = false;
					if (w != storedW || h != storedH) {
						storedW = w;
						storedH = h;
						// Straight to the ini: saveNow() would flash "saved" on
						// every drag, which is noise for something this passive.
						syncCfgFromUi();
						SaveLauncherConfigKeepModern(exeDir + L"pob-zh.ini", cfg);
					}
				}
			}
		}

		// Closing this window in tabbed mode means closing every tab first, and
		// each one may put up "save your build?" -- answering cancel has to keep
		// both the tab and this window alive, so the close is held rather than
		// obeyed until they are actually gone.
		// Closing the launcher closes its tabs. The close request is a one-shot
		// signal, so it only starts the process -- the work is driven by
		// `closingTabs` from then on. Reading the flag instead of the signal
		// matters: clearing it below meant the condition was false on the very
		// next frame, so only the first tab was ever asked to close.
		// Embedded panels get asked before the window is allowed to go. Without this
		// a tab with unsaved work would simply be shut down in the teardown below and
		// the work lost without a word -- the docked tabs have had this since they
		// existed, and a panel is no different to the person using it.
		//
		// Cancelled by any one of them abandons the whole close, which matches how
		// the docked sequence treats "the user said no".
		//
		// Held in a flag for the same reason `closingTabs` is: the close request is
		// a ONE-SHOT signal, and clearing it below makes the condition false on the
		// very next frame. Asking straight off the signal meant a panel that put up
		// a prompt got its answer, closed its own tab -- and the window it was asked
		// on behalf of stayed open, because by then nothing remembered why.
		if (glfwWindowShouldClose(win) && !panels.empty()) {
			closingPanels = true;
			glfwSetWindowShouldClose(win, GLFW_FALSE);
		}
		if (closingPanels) {
			bool waiting = false, cancelled = false;
			for (EmbeddedPanel& ep : panels) {
				const ToolCloseState cs = ep.panel->RequestClose();
				if (cs == ToolCloseState::Asking) waiting = true;
				else if (cs == ToolCloseState::Cancelled) cancelled = true;
			}
			if (cancelled) {
				// Somebody said no, so nothing closes -- including the panels that
				// had already agreed. Without taking their agreement back, the reap
				// below would find them Closed and remove them, and cancelling one
				// save prompt would silently take the user's other tabs with it.
				for (EmbeddedPanel& ep : panels) ep.panel->AbortClose();
				closingPanels = false;
				relaunchRequested = false;   // "restart now" is off too: the user kept working
			} else if (!waiting) {
				closingPanels = false;
				glfwSetWindowShouldClose(win, GLFW_TRUE);
			}
		}
		if (tabbed && glfwWindowShouldClose(win) && !dock.Empty()) {
			closingTabs = true;
			glfwSetWindowShouldClose(win, GLFW_FALSE);
		}
		if (tabbed && closingTabs) {
			if (dock.Empty()) {
				glfwSetWindowShouldClose(win, GLFW_TRUE);
			} else {
				// ONE at a time, last first, asking again only once the previous one
				// has actually gone. Asking all at once made them close in a visible
				// flurry, and any tab prompting "save your build?" did so hidden
				// behind whichever tab was showing.
				const unsigned long back = dock.Tabs().back().pid;
				if (back != closingPid) {
					closingPid = back;
					closeAskedAt = ImGui::GetTime();
					dock.RequestClose(dock.Tabs().size() - 1);
				} else if (ImGui::GetTime() - closeAskedAt > 8.0) {
					// Still there long after being asked: the user answered "cancel"
					// to its save prompt. That is a decision to keep working, so the
					// shutdown is abandoned rather than nagging them tab by tab.
					closingTabs = false;
					closingPid = 0;
					relaunchRequested = false;
				}
			}
		}

		// Live font switch: rebuild the glyph atlas between frames when the user
		// picks a different font in the status-bar combo. Synchronous and Full: the
		// user asked for it and ~400 ms is fine. A background build still in flight
		// is for the OLD font, so it is thrown away first -- and it must be joined
		// before the TTF buffer it reads can go out of scope.
		// Font-size change: switch `scale`, rebuild the prebuilt styles (ApplyTheme
		// ends in ScaleAllSizes, which compounds, so it runs on a fresh style and
		// only here, between frames), and let the atlas path below rebuild the
		// faces at the new size. A window still at its mode default is resized to
		// the new default with it; a remembered size is the user's and stays.
		if (pendingScale > 0.0f) {
			const bool atDefault = (storedW == 0 || storedH == 0);
			scale = pendingScale;
			pendingScale = 0.0f;
			PobUi::ApplyTheme(scale, PobUi::Density::Comfortable);
			PobUi::BuildStyle(styleComfortable, scale, PobUi::Density::Comfortable);
			PobUi::BuildStyle(styleCompact, scale, PobUi::Density::Compact);
			PobUi::BuildStyle(styleCanvas, scale, PobUi::Density::Canvas);
			panelHost.scale = scale;
			glfwSetWindowSizeLimits(win, (int)(kMinWinW * scale), (int)(kMinWinH * scale),
			                        GLFW_DONT_CARE, GLFW_DONT_CARE);
			if (atDefault) {
				int w, h;
				defaultWinSize(&w, &h);
				applyWindowSize(w, h, false);
			}
			fontChanged = true;
		}
		if (fontChanged) {
			fontChanged = false;
			fontWorker.Discard();
			ImGui_ImplOpenGL3_DestroyFontsTexture();
			ImGui::GetIO().Fonts->Clear();
			fontInput = PrepareFontInput(ResolveFontPath(exeDir, cfg.fontFile),
			                             atlasTexts, strOverlays, scale, glMaxTex,
			                             FallbackFontPaths(exeDir, cfg.fontFile), cfg.locale == L"ko-KR");
			fonts = LoadFonts(ImGui::GetIO().Fonts, fontInput, FontScope::Full);
			localeDrawable = ProbeLocaleCoverage(fonts, strStore, &localeMissing);
			ImGui_ImplOpenGL3_CreateFontsTexture();
			ImGui::GetIO().Fonts->ClearTexData(); // same as the swap path below
			g_launcherRedraw = true; // new glyphs behind the same vertices
		}
		// The full atlas from the startup worker is ready: swap it in between
		// frames. Order matters -- DestroyFontsTexture clears the TexID of whatever
		// io.Fonts points at, so it runs against the OLD atlas, and
		// CreateFontsTexture against the NEW one. The context deletes whatever
		// io.Fonts is at DestroyContext, so the old atlas is ours to free here.
		// Nothing between this and NewFrame may measure text: ImGui's current
		// font still points into the old atlas until NewFrame resets it.
		// Only after the first frame has been presented: the backend
		// creates its device objects (font texture included) lazily in the first
		// NewFrame, and a swap before that would have CreateFontsTexture run twice
		// -- the second time re-rasterising the whole block on this thread because
		// ClearTexData had already dropped the pixels.
		// (Not "once the window is visible": FirstShow can put it up before any
		// frame when the start stalls, and shot mode never shows it at all.)
		if (fontWorker.Done() && pacer.PresentedOnce()) {
			LauncherFonts full;
			ImFontAtlas* fullAtlas = fontWorker.Take(&full);
			ImGuiIO& io = ImGui::GetIO();
			ImGui_ImplOpenGL3_DestroyFontsTexture();
			ImFontAtlas* old = io.Fonts;
			io.Fonts = fullAtlas;
			fonts = full;
			ImGui_ImplOpenGL3_CreateFontsTexture();
			IM_DELETE(old);
			// ~94 MB of CPU-side pixels (Alpha8 + RGBA32) the GPU now has its own copy of.
			io.Fonts->ClearTexData();
			localeDrawable = ProbeLocaleCoverage(fonts, strStore, &localeMissing);
			fontInputFullDone = true;
			startup_trace_mark("full font atlas swapped in (%dx%d%s%s)", fonts.texW, fonts.texH,
			                   fonts.dropped.empty() ? "" : " dropped=", fonts.dropped.c_str());
			g_launcherRedraw = true; // the '?' placeholders on screen are now real glyphs
		}

		// Re-published every frame, never cached by a panel: `fontChanged` above
		// rebuilds the atlas and invalidates every ImFont* handed out before it.
		// Thumbnails decoded on a worker: upload the finished ones (GL thread).
		for (BgThumb& t : bgThumbs) {
			if (!t.job || t.job->wait_for(std::chrono::seconds(0)) != std::future_status::ready) continue;
			ThumbPixels px = t.job->get();
			t.job.reset();
			if (!px.rgba.empty()) {
				t.tex = CreateTextureRGBA(px.rgba.data(), px.w, px.h);
				t.w = px.w;
				t.h = px.h;
				g_launcherRedraw = true;
			}
		}

		panelHost.body = fonts.body;
		panelHost.big = fonts.big;
		panelHost.cjkOk = fonts.cjkOk;
		{
			PobUi::WidgetFonts wf;
			wf.body = fonts.body;
			wf.small = fonts.small;
			wf.heading = fonts.heading;
			wf.title = fonts.title;
			wf.headingPx = std::floor(kHeadingFontSize * scale);
			wf.scale = scale;
			wf.icons = fonts.icons;
			PobUi::SetWidgetFonts(wf);
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
		// Which release history to show. The prose lives in the source, not in a
		// translated string table, so it follows the same fallback S does: a font
		// that cannot draw the chosen language would make the Chinese one
		// unreadable too. zh-rTW, zh-rCN, and any zh-* dropped in later.
		const bool zhUi = langDrawable && localeIdx >= 0 && localeIdx < (int)locales.size() &&
		                  locales[localeIdx].id.rfind("zh", 0) == 0;

		// POB instances this launcher started (KeepOpen mode). Counted every
		// frame because that call is also where finished processes are reaped.
		const int pobCount = PobLaunch::PobRunningCount();
		const bool pobBusy = PobLaunch::AnyPobRunning(exeDir);

		// Hand the watchdog thread a snapshot of the POB windows to ask after.
		// It cannot call RunningInstances itself (not thread-safe), and this
		// thread must not do the asking: a SendMessageTimeout against a wedged
		// window is exactly the wait that would freeze the launcher too. Twice a
		// second is plenty -- the threshold it feeds is twenty.
		if (ImGui::GetTime() - lastPeerSync > 0.5) {
			lastPeerSync = ImGui::GetTime();
			std::vector<HangWatch::Peer> peers;
			for (const PobLaunch::InstanceInfo& in : PobLaunch::RunningInstances()) {
				if (in.kind != PobLaunch::InstanceKind::Pob || !in.hwnd) continue;
				HangWatch::Peer p;
				p.pid = in.pid;
				p.hwnd = in.hwnd;
				// Not localised: this label goes into the log, where it has to
				// mean the same thing to whoever reads the report.
				p.label = std::string("POB (") + (in.game == L"poe2" ? "poe2" : "poe1") + ")";
				peers.push_back(p);
			}
			HangWatch::SetPeers(peers);
		}
		if (appUpd) {
			// Applying an update renames engine\* out of the way while POB has
			// those DLLs open, and the same check silently overwrites Data\*.json
			// with a fresh translation pack. Both have to stop, so the gate goes
			// on the worker, not just on the button.
			appUpd->SetHold(pobBusy);
			// Last POB closed: pick the check back up instead of waiting a day.
			if (wasPobBusy && !pobBusy) appUpd->RequestCheck(AppUpdater::CheckReason::Background);
		}
		wasPobBusy = pobBusy;

		// App-updater snapshot for this frame. While the update is in flight the
		// launch/tool actions are disabled so the auto-relaunch cannot interrupt
		// anything; a ready stage closes the window via ApplyAppUpdate.
		AppUpdater::Status ust;
		if (appUpd) {
			ust = appUpd->Poll();
			// "Up to date" and "translation data updated" are confirmations, not
			// states: a toast says them (only for a check the user asked for, in the
			// first case) and the header goes straight back to idle.
			if (ust.phase == AppUpdatePhase::UpToDate) {
				if (manualCheck) {
					const LauncherStrings& St = strStore[localeIdx >= 0 && localeIdx < (int)strStore.size() ? localeIdx : 0].s;
					PobUi::ShowToast((std::string(St.updateUpToDate) + " v" + ust.localVer).c_str());
					manualCheck = false;
				}
				appUpd->AckNotice(); // silent otherwise: only problems and news are shown
				ust = appUpd->Poll();
			}
			if (ust.phase == AppUpdatePhase::TransDone) {
				const LauncherStrings& St = strStore[localeIdx >= 0 && localeIdx < (int)strStore.size() ? localeIdx : 0].s;
				PobUi::ShowToast((std::string(St.updateTransDone) + ust.latestDataVer).c_str());
				appUpd->AckNotice();
				ust = appUpd->Poll();
			}
			// "Install a new version at startup by itself": press our own button,
			// but only in the moment where nothing is lost by it (see
			// ShouldAutoApplyApp). Everything after this -- the download progress,
			// AppReadyToApply, the swap and the relaunch -- is the same path the
			// orange button takes, so the user still watches it happen on screen
			// instead of the window vanishing without explanation.
			{
				AutoApplyInputs ai;
				ai.settingOn = cfg.autoApplyAppUpdate;
				ai.phase = ust.phase;
				ai.pobBusy = pobBusy;
				ai.anythingLaunched = anythingLaunched;
				ai.alreadyTried = autoApplyTried;
				if (ShouldAutoApplyApp(ai)) {
					autoApplyTried = true;
					appUpd->StartAppUpdate();
					ust = appUpd->Poll();
				}
			}
			if (ust.phase == AppUpdatePhase::AppReadyToApply) applyUpdate = true;
		}
		bool updaterBusy = ust.phase == AppUpdatePhase::AppDownloading ||
		                   ust.phase == AppUpdatePhase::AppStaging ||
		                   ust.phase == AppUpdatePhase::AppReadyToApply;
		if (glfwGetTime() - modernGateReadAt > 2.0) {
			modernGate = BridgeGate::Read(exeDir);
			modernBridgeHash = modernGate.present && !modernGate.ok ? BridgeGate::BridgeFingerprint(exeDir) : std::string();
			modernGateReadAt = glfwGetTime();
		}

		ImGuiIO& io = ImGui::GetIO();
		using PobUi::D;
		ImGui::SetNextWindowPos(ImVec2(0, 0));
		ImGui::SetNextWindowSize(io.DisplaySize);
		ImGui::PushFont(fonts.body);
		ImGui::Begin("##launcher", nullptr,
			ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);

		ImDrawList* dl = ImGui::GetWindowDrawList();
		const float W = io.DisplaySize.x;
		const float padX = ImGui::GetStyle().WindowPadding.x;
		const std::string verLine = std::string("v" POBTOOLS_VERSION_STRING) +
		                            (ust.localDataVer.empty() ? std::string() : " \xc2\xb7 " + ust.localDataVer);

		// Page tab bar in the design's underline style: transparent tabs, muted
		// labels, the selected one in `text` with a 2px accent rule under it. ImGui
		// fills the selected tab with TabActive and draws its bar separator in the
		// same colour, so both are made the ground colour and the rule and the
		// separator are drawn by hand.
		auto pageTabStyle = [&](bool push) {
			if (push) {
				ImGui::PushStyleColor(ImGuiCol_Tab, PobUi::TokV4(Tok::Bg));
				ImGui::PushStyleColor(ImGuiCol_TabHovered, PobUi::TokV4(Tok::Surface1));
				ImGui::PushStyleColor(ImGuiCol_TabActive, PobUi::TokV4(Tok::Bg));
				ImGui::PushStyleColor(ImGuiCol_TabUnfocused, PobUi::TokV4(Tok::Bg));
				ImGui::PushStyleColor(ImGuiCol_TabUnfocusedActive, PobUi::TokV4(Tok::Bg));
				ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
				                    ImVec2(D(14.0f), std::floor((D(38.0f) - ImGui::GetFontSize()) * 0.5f)));
			} else {
				ImGui::PopStyleVar();
				ImGui::PopStyleColor(5);
			}
		};
		// One page tab. `idx` keys the muted / selected label colour off the page
		// that was selected last frame (ImGui only says which one is selected by
		// returning true, i.e. after the label has been drawn).
		auto pageTab = [&](const char* label, int idx, ImGuiTabItemFlags flags) -> bool {
			ImGui::PushStyleColor(ImGuiCol_Text, PobUi::TokV4(idx == pageShown ? Tok::Text : Tok::TextMuted));
			const bool open = ImGui::BeginTabItem(label, nullptr, flags);
			ImGui::PopStyleColor();
			if (open) {
				const ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
				ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(mn.x, mx.y - 2.0f), ImVec2(mx.x, mx.y), Tok::Accent);
				pageNow = idx;
			}
			return open;
		};

		// ---- header ----------------------------------------------------------
		// Logo, product name, the version line and the update state. Drawn above
		// the page tabs in separate mode and at the top of the launcher tab in
		// tabbed mode.
		auto drawHeader = [&]() {
			ImDrawList* dl = ImGui::GetWindowDrawList();
			const float logo = std::floor(D(40.0f));
			const ImVec2 hp = ImGui::GetCursorScreenPos();
			const float inner = ImGui::GetContentRegionAvail().x;
			DrawLogo(dl, hp, logo, fonts.icons);
			{
				const float tx = hp.x + logo + D(16.0f);
				const float titleH = fonts.title->FontSize;
				const float subH = fonts.small->FontSize;
				const float ty = hp.y + std::floor((logo - titleH - subH) * 0.5f);
				dl->AddText(fonts.title, fonts.title->FontSize, ImVec2(tx, ty), Tok::Text, "PobTools");
				dl->AddText(fonts.small, fonts.small->FontSize, ImVec2(tx, ty + titleH), Tok::TextMuted, S.appSubtitle);
			}
			// Update area, right-aligned on the logo's line. Each phase has one look
			// (UpdateStatus): idle = version + check; new = outline update button;
			// in progress = bar + MB; failed = red pill + "see why".
			if (appUpd) {
				const ImVec2 keep = ImGui::GetCursorScreenPos();
				const float gap = D(12.0f);
				const float lineH = std::floor(D(28.0f));
				auto placeAt = [&](float w) {
					ImGui::SetCursorScreenPos(ImVec2(hp.x + inner - w, hp.y + std::floor((logo - lineH) * 0.5f)));
				};
				ImGui::PushFont(fonts.small);
				auto numericW = [&](const std::string& s) { return fonts.small->CalcTextSizeA(fonts.small->FontSize, FLT_MAX, 0.0f, s.c_str()).x; };
				auto numericAt = [&](const std::string& s) {
					const ImVec2 c = ImGui::GetCursorScreenPos();
					ImGui::Dummy(ImVec2(numericW(s), lineH));
					dl->AddText(fonts.small, fonts.small->FontSize,
					            ImVec2(c.x, c.y + std::floor((lineH - fonts.small->FontSize) * 0.5f)), Tok::TextMuted, s.c_str());
				};
				const AppUpdatePhase ph = ust.phase;
				if (ph == AppUpdatePhase::Idle || ph == AppUpdatePhase::UpToDate || ph == AppUpdatePhase::TransDone ||
				    ph == AppUpdatePhase::Checking) {
					const bool checking = ph == AppUpdatePhase::Checking;
					const char* label = checking ? S.updateChecking : S.updateCheck;
					const float bw = PobUi::ButtonWidth(label, PobUi::BtnSize::Sm, PobIcon::Refresh);
					placeAt(numericW(verLine) + gap + bw);
					numericAt(verLine);
					ImGui::SameLine(0, gap);
					// Disabled while POB holds engine\*. The tooltip says why and
					// what to do -- a greyed-out button on its own is a dead end.
					if (PobUi::Button(label, PobUi::BtnKind::Secondary, PobUi::BtnSize::Sm, PobIcon::Refresh, 0.0f,
					                  !pobBusy && !checking)) {
						// UserAsked, not just "force": it also decides that a failure
						// has to be visible rather than putting the button back unchanged.
						appUpd->RequestCheck(AppUpdater::CheckReason::UserAsked);
						manualCheck = true;
					}
					if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && !checking)
						PobUi::Tooltip(pobBusy ? S.updateBlockedTip : S.updateCheckTip);
				} else if (ph == AppUpdatePhase::AppAvailable) {
					char label[96];
					snprintf(label, sizeof(label), S.updateTo, ust.latestAppVer.c_str());
					const float bw = PobUi::ButtonWidth(label, PobUi::BtnSize::Sm, PobIcon::Download);
					placeAt(numericW(verLine) + gap + bw);
					numericAt(verLine);
					ImGui::SameLine(0, gap);
					if (PobUi::Button(label, PobUi::BtnKind::Update, PobUi::BtnSize::Sm, PobIcon::Download, 0.0f, !pobBusy))
						appUpd->StartAppUpdate();
					if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
						PobUi::Tooltip(pobBusy ? S.updateBlockedTip : (std::string(S.updateAvailable) + ust.latestAppVer + S.updateNow).c_str());
				} else if (ph == AppUpdatePhase::AppDownloading || ph == AppUpdatePhase::AppStaging ||
				           ph == AppUpdatePhase::AppReadyToApply || ph == AppUpdatePhase::TransUpdating) {
					char what[96];
					if (ph == AppUpdatePhase::AppDownloading) snprintf(what, sizeof(what), S.updateDownloadingVer, ust.latestAppVer.c_str());
					else if (ph == AppUpdatePhase::AppStaging) snprintf(what, sizeof(what), "%s", S.updatePreparing);
					else if (ph == AppUpdatePhase::AppReadyToApply) snprintf(what, sizeof(what), "%s", S.updateRestarting);
					else snprintf(what, sizeof(what), "%s", S.transUpdatingHdr);
					char mb[64] = "";
					float frac = -1.0f;
					if (ust.bytesTotal > 0) {
						snprintf(mb, sizeof(mb), "%.1f / %.1f MB", ust.bytesDone / 1048576.0, ust.bytesTotal / 1048576.0);
						frac = (float)((double)ust.bytesDone / (double)ust.bytesTotal);
					} else if (ust.bytesDone > 0) {
						snprintf(mb, sizeof(mb), "%.1f MB", ust.bytesDone / 1048576.0);
					}
					if (ph == AppUpdatePhase::AppStaging || ph == AppUpdatePhase::AppReadyToApply) frac = 1.0f;
					// no byte count (translation pack): a sweeping bar says "working"
					if (frac < 0.0f) frac = (float)fmod(ImGui::GetTime() * 0.5, 1.0);
					const float barW = D(140.0f);
					const float total = numericW(what) + gap + barW + (mb[0] ? gap + numericW(mb) : 0.0f);
					placeAt(total);
					numericAt(what);
					ImGui::SameLine(0, gap);
					ImGui::BeginGroup();
					{
						const ImVec2 c = ImGui::GetCursorScreenPos();
						ImGui::SetCursorScreenPos(ImVec2(c.x, c.y + std::floor((lineH - D(6.0f)) * 0.5f)));
						PobUi::ProgressBar(frac, barW);
					}
					ImGui::EndGroup();
					if (mb[0]) { ImGui::SameLine(0, gap); numericAt(mb); }
				} else if (ph == AppUpdatePhase::TransAvailable) {
					// Opted out of automatic translation updates. Say what is waiting
					// and offer to take it once -- otherwise the only way to get it is
					// to toggle the setting off and on again.
					const float bw = PobUi::ButtonWidth(S.transApplyNow, PobUi::BtnSize::Sm);
					placeAt(numericW(ust.message) + gap + bw);
					numericAt(ust.message);
					ImGui::SameLine(0, gap);
					if (PobUi::Button(S.transApplyNow, PobUi::BtnKind::Secondary, PobUi::BtnSize::Sm, nullptr, 0.0f, !pobBusy))
						appUpd->StartTranslationUpdate();
					if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && pobBusy)
						PobUi::Tooltip(S.updateBlockedTip);
				} else if (ph == AppUpdatePhase::Error) {
					// Only the pill and a way to the reason up here; the full message
					// and the retry are the banner at the top of the home page.
					const float bw = PobUi::ButtonWidth(S.updateSeeWhy, PobUi::BtnSize::Sm);
					placeAt(PobUi::PillWidth(S.updateFailedPill) + gap + bw);
					PobUi::StatusPill(PobUi::Tone::Bad, S.updateFailedPill);
					ImGui::SameLine(0, gap);
					if (PobUi::Button(S.updateSeeWhy, PobUi::BtnKind::Ghost, PobUi::BtnSize::Sm)) {
						pageForce = 0;
						if (tabbed) launcherTabForce = true;
					}
				}
				ImGui::PopFont();
				ImGui::SetCursorScreenPos(keep);
			}
			ImGui::Dummy(ImVec2(inner, logo));
			ImGui::Dummy(ImVec2(0, D(4.0f)));
		};

		// ---- tabbed mode: the document strip ---------------------------------
		// The five pages become ONE "Launcher" tab, then POB and the tools.
		bool tabsOk = false;
		bool launcherOpen = true;
		int stripH = 0;
		int activeDockTab = -1;   // -1 = a normal tab is showing, no window on top
		int closeDockTab = -1;
		if (tabbed) {
			const float logo = std::floor(D(32.0f));
			const ImVec2 lp = ImGui::GetCursorScreenPos();
			DrawLogo(dl, lp, logo, fonts.icons);
			ImGui::Dummy(ImVec2(logo, logo));
			ImGui::SameLine(0, D(12.0f));
			ImGui::PushStyleColor(ImGuiCol_Tab, PobUi::TokV4(Tok::Surface1));
			ImGui::PushStyleColor(ImGuiCol_TabHovered, PobUi::TokV4(Tok::Surface3));
			ImGui::PushStyleColor(ImGuiCol_TabActive, PobUi::TokV4(Tok::Surface2));
			ImGui::PushStyleColor(ImGuiCol_TabUnfocused, PobUi::TokV4(Tok::Surface1));
			ImGui::PushStyleColor(ImGuiCol_TabUnfocusedActive, PobUi::TokV4(Tok::Surface2));
			ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
			                    ImVec2(D(14.0f), std::floor((logo - ImGui::GetFontSize()) * 0.5f)));
			// Reorderable so window tabs can be dragged into whatever order suits
			// the user; the launcher tab is pinned with Leading.
			tabsOk = ImGui::BeginTabBar("##maintabs", ImGuiTabBarFlags_Reorderable);
			// Everything below this line belongs to the docked window when a window
			// tab is selected.
			stripH = (int)ImGui::GetCursorPosY();
			if (tabsOk) {
				const std::string lt = (fonts.icons ? std::string(PobIcon::House) + " " : std::string()) +
				                       S.launcherTab + "###launcher";
				launcherOpen = ImGui::BeginTabItem(lt.c_str(), nullptr,
				                                   ImGuiTabItemFlags_Leading |
				                                   (launcherTabForce ? ImGuiTabItemFlags_SetSelected : 0));
				launcherTabForce = false;
				if (launcherOpen) {
					const ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
					dl->AddRectFilled(mn, ImVec2(mx.x, mn.y + 2.0f), Tok::Accent);
				}
			} else {
				launcherOpen = false;
			}
			ImGui::PopStyleVar();
			ImGui::PopStyleColor(5);
			// program version at the strip's right end
			{
				const std::string v = "v" POBTOOLS_VERSION_STRING;
				const float vw = fonts.small->CalcTextSizeA(fonts.small->FontSize, FLT_MAX, 0.0f, v.c_str()).x;
				dl->AddText(fonts.small, fonts.small->FontSize,
				            ImVec2(W - padX - vw, lp.y + std::floor((logo - fonts.small->FontSize) * 0.5f)),
				            Tok::TextMuted, v.c_str());
			}
		}

		if (launcherOpen) {
		// Page grounds are `bg`; the theme's ChildBg (surface-1) is the card colour.
		ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
		if (tabbed) ImGui::BeginChild("##launcherpage", ImVec2(0, 0), false, ImGuiWindowFlags_AlwaysUseWindowPadding);
		drawHeader();

		// --- page tabs ---------------------------------------------------------
		ImGuiTabItemFlags pageFlags[5] = { 0, 0, 0, 0, 0 };
		if (applyStartupTab) {
			pageFlags[cfg.startupTab == StartupTab::Versions ? 1 : 0] = ImGuiTabItemFlags_SetSelected;
			applyStartupTab = false;
		}
		if (pageForce >= 0 && pageForce < 5) {
			pageFlags[0] = pageFlags[1] = 0;
			pageFlags[pageForce] = ImGuiTabItemFlags_SetSelected;
			pageForce = -1;
		}
		pageTabStyle(true);
		const bool pagesOk = ImGui::BeginTabBar("##pages", 0);
		const float barBottom = ImGui::GetCursorScreenPos().y;
		ImGui::GetWindowDrawList()->AddLine(ImVec2(ImGui::GetWindowPos().x + padX, barBottom - 1.0f),
		            ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - padX, barBottom - 1.0f), Tok::Border, 1.0f);
		pageNow = -1;
		const bool pHome = pagesOk && pageTab(S.tabHome, 0, pageFlags[0]);
		if (pHome) pageTabStyle(false);
		if (pHome) {
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(D(4.0f), D(24.0f)));
		ImGui::BeginChild("##homebody", ImVec2(0, 0), false, ImGuiWindowFlags_AlwaysUseWindowPadding);
		ImGui::PopStyleVar();
		const float inner = ImGui::GetContentRegionAvail().x - D(4.0f);

		// ---- banners ---------------------------------------------------------
		// Everything that needs attention, at the top, worst first (Banner): a
		// failed update, missing dictionaries, a POB that stopped answering, the
		// new interface refusing this POB, dictionaries read from elsewhere.
		{
			struct BannerSpec {
				PobUi::BannerTone tone;
				const char* icon;
				std::string title, desc;
				bool mono = false;
				const char* action = nullptr;
				bool closable = false;
				bool actionEnabled = true;
				int kind = 0;   // what the action does
				int slot = 0;
			};
			enum { kUpdErr = 1, kNoDict, kHang, kGate, kExt };
			std::vector<BannerSpec> bs;
			if (appUpd && ust.phase == AppUpdatePhase::Error) {
				BannerSpec b{ PobUi::BannerTone::Bad, PobIcon::CircleX };
				char t[512];
				snprintf(t, sizeof(t), S.updateFailedTitle, ust.message.c_str());
				b.title = t;
				b.desc = S.updateFailedDesc;
				b.action = S.updateRetry;
				b.actionEnabled = !pobBusy;
				b.closable = true;
				b.kind = kUpdErr;
				bs.push_back(b);
			}
			// No dictionaries anywhere: see the declaration of builtinDictsPresent.
			// Only when all three slots are on the built-in path -- an external
			// folder that happens to be empty already has its own, more specific
			// warning in settings.
			{
				const double nowT = ImGui::GetTime();
				if (dictProbeAt < 0.0 || nowT - dictProbeAt > 1.0) {
					dictProbeAt = nowT;
					builtinDictsPresent = false;
					for (int i = 0; i < kDictSlotCount; i++)
						if (DictionariesPresentAt(BuiltinDictDir(exeDir, (DictSlot)i)))
							builtinDictsPresent = true;
				}
				bool allBuiltin = true;
				for (int i = 0; i < kDictSlotCount; i++)
					if (dictDir[i].status != DataDirStatus::Builtin) allBuiltin = false;
				if (allBuiltin && !builtinDictsPresent) {
					BannerSpec b{ PobUi::BannerTone::Warn, PobIcon::TriangleAlert };
					b.title = S.noDictTitle;
					b.desc = S.noDictDesc;
					if (appUpd) {
						b.action = S.noDictDownload;
						b.actionEnabled = !(pobBusy || updaterBusy || ust.phase == AppUpdatePhase::TransUpdating);
					}
					b.kind = kNoDict;
					bs.push_back(b);
				}
			}
			// A POB that has stopped answering for twenty seconds. Nothing the
			// launcher can usefully do about it -- killing it would throw away an
			// unsaved build -- so this says what happened and that the reason was
			// written down. It clears itself the moment that window answers again.
			if (HangWatch::GetPeerStatus().hung) {
				BannerSpec b{ PobUi::BannerTone::Warn, PobIcon::TriangleAlert };
				b.title = S.hangBannerTitle;
				b.desc = S.hangBannerDesc;
				b.action = S.openLogShort;
				b.kind = kHang;
				bs.push_back(b);
			}
			// The new interface fell back to classic on this POB version; gone once
			// POB updates (the verdict is bound to the version it was taken against).
			if (modernUiOk && cfg.uiMode == 1 && modernGateBlocked()) {
				BannerSpec b{ PobUi::BannerTone::Warn, PobIcon::TriangleAlert };
				char t[512];
				snprintf(t, sizeof(t), S.modernGateBanner, modernGate.pobVersion.c_str());
				b.title = t;
				b.desc = modernGateTipText(S);
				b.kind = kGate;
				bs.push_back(b);
			}
			// Reading dictionaries from somewhere else changes what POB shows, and a
			// wrong translation looks exactly like broken data -- so it is stated on
			// the main screen, one banner per redirected slot.
			for (int i = 0; i < kDictSlotCount; i++) {
				if (dictDir[i].status != DataDirStatus::External) continue;
				BannerSpec b{ PobUi::BannerTone::Info, PobIcon::Info };
				const char* slotName = i == 0 ? "PoE1" : i == 1 ? "PoE2" : S.slotLauncher;
				char t[256];
				snprintf(t, sizeof(t), S.extDataBannerTitle, slotName);
				b.title = t;
				b.desc = to_utf8(dictDir[i].root);
				b.mono = true;
				b.action = S.useBuiltin;
				b.kind = kExt;
				b.slot = i;
				bs.push_back(b);
			}
			std::stable_sort(bs.begin(), bs.end(), [](const BannerSpec& a, const BannerSpec& b) {
				auto rank = [](PobUi::BannerTone t) { return t == PobUi::BannerTone::Bad ? 0 : t == PobUi::BannerTone::Warn ? 1 : 2; };
				return rank(a.tone) < rank(b.tone);
			});
			const size_t shown = bannersExpanded ? bs.size() : std::min<size_t>(bs.size(), 3);
			for (size_t i = 0; i < shown; i++) {
				const BannerSpec& b = bs[i];
				ImGui::PushID((int)i);
				const PobUi::BannerResult r = PobUi::Banner("##banner", b.tone, b.icon, b.title.c_str(),
				    b.desc.empty() ? nullptr : b.desc.c_str(), b.mono, b.action, b.closable, b.actionEnabled, inner);
				if (b.kind == kGate && ImGui::IsItemHovered()) PobUi::Tooltip(modernGateTipText(S).c_str());
				ImGui::PopID();
				if (r == PobUi::BannerResult::Action) {
					switch (b.kind) {
						case kUpdErr: appUpd->StartAppUpdate(); break;
						case kNoDict: appUpd->StartTranslationUpdate(); break;
						case kHang: {
							const std::wstring lg = PobLog::LogDir();
							if (!lg.empty()) ShellExecuteW(nullptr, L"open", lg.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
							break;
						}
						case kExt:
							cfg.dataDir[b.slot].clear();
							resolveDict(b.slot);
							saveNow();
							break;
					}
				} else if (r == PobUi::BannerResult::Close && b.kind == kUpdErr) {
					appUpd->AckNotice();
				}
				ImGui::Dummy(ImVec2(0, D(4.0f)));
			}
			if (bs.size() > 3) {
				char more[64];
				snprintf(more, sizeof(more), S.moreBanners, (int)(bs.size() - 3));
				if (PobUi::Button(bannersExpanded ? S.linksLess : more, PobUi::BtnKind::Ghost, PobUi::BtnSize::Sm))
					bannersExpanded = !bannersExpanded;
			}
			if (!bs.empty()) ImGui::Dummy(ImVec2(0, D(12.0f)));
		}

		// ---- games -------------------------------------------------------------
		// One card per install. KeepOpen starts POB detached and leaves this
		// window up; the other two modes take the original path (set `launch`,
		// ShowLauncher returns). Tabbed mode always takes the detached path: this
		// window IS the one POB lives in.
		const bool keepOpen = tabbed || (cfg.exitMode == LaunchExitMode::KeepOpen);
		PobUi::SectionHeader(S.gamesSection, inner);
		ImGui::Dummy(ImVec2(0, D(4.0f)));
		for (int g = 0; g < 2; g++) {
			const bool poe2 = g == 1;
			const bool ok = poe2 ? !installs.poe2Lua.empty() : !installs.poe1Lua.empty();
			const int running = PobLaunch::PobRunningCountFor(poe2 ? L"poe2" : L"poe1");
			const bool modernFor = cfg.uiMode == 1 && modernUiOk && !modernGateBlockedFor(poe2);
			GameCardSpec spec;
			spec.badge = poe2 ? "P2" : "P1";
			spec.name = poe2 ? S.poe2 : S.poe1;
			const std::string& ver = poe2 ? installs.poe2Version : installs.poe1Version;
			spec.meta = (ver.empty() ? std::string("POB") : "POB v" + ver) + " \xc2\xb7 " +
			            (modernFor ? S.uiModeShortModern : S.uiModeShortClassic);
			if (!ok) {
				spec.missing = poe2 ? S.notFoundPoe2Card : S.notFoundPoe1Card;
				spec.pillTone = PobUi::Tone::Bad;
				spec.pill = S.missing;
				spec.action = S.bgOpenFolder;
				spec.primary = false;
			} else if (running > 0) {
				char buf[64];
				snprintf(buf, sizeof(buf), S.pobRunningPill, running);
				spec.pillTone = PobUi::Tone::Run;
				spec.pill = buf;
				spec.action = keepOpen ? S.launchAnother : S.launch;
			} else {
				spec.pillTone = PobUi::Tone::Ok;
				spec.pill = S.detected;
				spec.action = S.launch;
			}
			// An update in flight does not grey the card out; only the action stops,
			// and its tooltip says why.
			if (ok && updaterBusy) { spec.enabled = false; spec.disabledTip = S.launchBlockedUpdating; }
			if (GameCard(poe2 ? "##game2" : "##game1", spec, inner)) {
				if (!ok) {
					// Not found: open the folder the POB has to be put in.
					ShellExecuteW(nullptr, L"open", exeDir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
				} else {
					poe2Sel = poe2;
					// "Default interface: new" opens the WebView2 window instead, when
					// it can run here and the remembered gate has not refused this POB.
					// In the close/return modes host_main makes the same choice after
					// ShowLauncher returns (it also re-checks the gate).
					if (keepOpen) {
						if (modernFor) {
							cfg.game = poe2 ? L"poe2" : L"poe1";
							spawnTool(L"--modern-ui", PobLaunch::InstanceKind::ModernUi, S.modernUiTool);
						} else {
							launchPob(poe2);
						}
					} else { launch = true; anythingLaunched = true; }
				}
			}
			if (ok && ImGui::IsItemHovered() && !(poe2 ? poe2Dir : poe1Dir).empty()) {
				// the install folder, for the curious; the card itself says enough
			}
			// Two windows on ONE install share POB's Settings.xml and build files,
			// so the last one closed overwrites the other. Not ours to fix, but the
			// user should not have to discover it by losing work.
			if (running > 1) {
				ImGui::Dummy(ImVec2(0, D(0.0f)));
				ImGui::Indent(D(4.0f));
				PobUi::Hint(S.pobSameGameWarn, inner - D(8.0f), Tok::Warning);
				ImGui::Unindent(D(4.0f));
			}
		}

		// Browser-mode sessions of the new interface (either game).
		if (browserPoll.valid() && browserPoll.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
			browserRunning = browserPoll.get();
		}
		if (modernUiOk && !browserPoll.valid() && ImGui::GetTime() - browserPollAt > 2.0) {
			browserPollAt = ImGui::GetTime();
			browserPoll = std::async(std::launch::async, [exeDir]() {
				BrowserRunning r;
				r.poe1 = ModernUiBrowserRunning(exeDir, L"poe1", &r.url1);
				r.poe2 = ModernUiBrowserRunning(exeDir, L"poe2", &r.url2);
				return r;
			});
		}
		for (int g = 0; g < 2; ++g) {
			const bool on = g == 0 ? browserRunning.poe1 : browserRunning.poe2;
			if (!on) continue;
			const wchar_t* game = g == 0 ? L"poe1" : L"poe2";
			ImGui::PushID(g == 0 ? "##browser1" : "##browser2");
			ImGui::PushFont(fonts.small);
			ImGui::AlignTextToFramePadding();
			ImGui::TextDisabled("%s%s", S.modernBrowserRunning, g == 0 ? S.poe1 : S.poe2);
			ImGui::SameLine();
			if (PobUi::Button(S.modernBrowserOpen, PobUi::BtnKind::Secondary, PobUi::BtnSize::Sm)) ModernUiBrowserOpen(exeDir, game);
			ImGui::SameLine();
			const std::string& burl = g == 0 ? browserRunning.url1 : browserRunning.url2;
			if (!burl.empty()) {
				// CrossOver often has no browser to hand the address to; the user
				// pastes it into the Mac's own browser instead.
				if (PobUi::Button(S.modernBrowserCopyUrl, PobUi::BtnKind::Secondary, PobUi::BtnSize::Sm)) {
					ImGui::SetClipboardText(burl.c_str());
					PobUi::ShowToast(S.copiedToast);
				}
				if (ImGui::IsItemHovered()) PobUi::Tooltip(burl.c_str());
				ImGui::SameLine();
			}
			if (PobUi::Button(S.modernBrowserStop, PobUi::BtnKind::Ghost, PobUi::BtnSize::Sm)) {
				ModernUiBrowserStop(exeDir, game);
				(g == 0 ? browserRunning.poe1 : browserRunning.poe2) = false;
				browserPollAt = ImGui::GetTime(); // the server needs a moment to go
			}
			ImGui::PopFont();
			ImGui::PopID();
		}
		ImGui::Dummy(ImVec2(0, D(16.0f)));

		// ---- tools -------------------------------------------------------------
		// Tiles in order of use; the translation editor is a maintainer's tool and
		// goes last. Tabbed mode draws a tool in this window (a tab); separate mode
		// starts it as its own process, exactly as before.
		PobUi::SectionHeader(S.toolsSection, inner);
		ImGui::Dummy(ImVec2(0, D(4.0f)));
		{
			struct ToolDef {
				const char* icon; const char* name; const char* badge; const char* hint;
				IToolPanel* (*make)(); const wchar_t* flag; PobLaunch::InstanceKind kind; const char* label;
			};
			const ToolDef tools[] = {
				{ PobIcon::Funnel, S.filterEditor, nullptr, S.tipFilter, &CreateFilterEditorPanel, L"--filter-editor", PobLaunch::InstanceKind::FilterEditor, S.filterEditor },
				{ PobIcon::Crosshair, S.atlasPlanner, nullptr, S.tipAtlas, &CreateAtlasPlannerPanel, L"--atlas", PobLaunch::InstanceKind::AtlasPlanner, S.atlasPlanner },
				{ PobIcon::Gem, S.timelessJewel, nullptr, S.tipTimeless, &CreateTimelessJewelPanel, L"--timeless-jewel", PobLaunch::InstanceKind::TimelessJewel, S.timelessJewel },
				// Builds a string for the GAME's search box, not for POB -- useful
				// with no POB installed.
				{ PobIcon::CodeXml, S.regexTool, nullptr, S.tipRegex, &CreateRegexToolPanel, L"--regex", PobLaunch::InstanceKind::RegexTool, S.regexTool },
				// TEST-channel stash revenue tracker (POESESSID; see warehouse_tool.h).
				{ PobIcon::ChartColumn, S.warehouseTool, S.warehouseBadge, S.tipWarehouse, &CreateWarehousePanel, L"--warehouse", PobLaunch::InstanceKind::Warehouse, S.warehouseTool },
				// Edits dist\Data\{game}\{locale}\*.json in place. Separate mode opens
				// it IN-PROCESS (openEditor makes ShowLauncher return) so the
				// launcher's own labels reload on the way back; tabbed mode cannot
				// tear the window down, so it becomes a tab.
				{ PobIcon::Languages, S.editorName, "Beta", S.tipEditor, &CreateTranslationEditorPanel, nullptr, PobLaunch::InstanceKind::Pob, S.editor },
			};
			const int n = (int)(sizeof(tools) / sizeof(tools[0]));
			const float gap = D(12.0f);
			const int cols = inner >= D(684.0f) ? 3 : 2;
			const float tileW = std::floor((inner - gap * (cols - 1)) / (float)cols);
			const float tileH = std::floor(D(16.0f) * 2.0f + D(20.0f) + D(8.0f) + fonts.body->FontSize + D(4.0f) +
			                               fonts.small->FontSize * 1.3f);
			for (int i = 0; i < n; i++) {
				const ToolDef& t = tools[i];
				bool open = false;
				if (tabbed) {
					for (const EmbeddedPanel& ep : panels)
						if (ep.label == t.label) open = true;
				}
				if (i % cols != 0) ImGui::SameLine(0, gap);
				ImGui::PushID(i);
				if (PobUi::ToolTile("##tool", t.icon, t.name, t.badge, open ? S.toolOpenTab : t.hint, open,
				                    ImVec2(tileW, tileH), !updaterBusy)) {
					if (tabbed) {
						openPanel(t.make, t.label);
					} else if (!t.flag) {
						openEditor = true;
						anythingLaunched = true;
					} else {
						spawnTool(t.flag, t.kind, t.label);
					}
				}
				ImGui::PopID();
			}
		}
		ImGui::Dummy(ImVec2(0, D(16.0f)));

		// ---- links -------------------------------------------------------------
		// A secondary block: one card, a column per group, read DOWN rather than
		// across. Long columns are cut to five until "more" is pressed.
		PobUi::SectionHeader(S.linksSection, inner);
		ImGui::Dummy(ImVec2(0, D(4.0f)));
		PobUi::CardBegin("##links", nullptr, nullptr, nullptr, true, inner);
		if (ImGui::BeginTable("##linkcols", kLinkColumnCount, ImGuiTableFlags_SizingStretchSame)) {
			const int kShort = 5;
			ImGui::TableNextRow();
			for (int c = 0; c < kLinkColumnCount; c++) {
				ImGui::TableSetColumnIndex(c);
				const LinkColumn& col = kLinkColumns[c];
				PobUi::Overline(col.head ? col.head : S.*col.headTr);
				ImGui::Dummy(ImVec2(0, D(2.0f)));
				ImGui::PushFont(fonts.small);
				const int count = (linksExpanded || c == kLinkColumnCount - 1) ? col.count : std::min(col.count, kShort);
				for (int r = 0; r < count; r++) {
					const LinkEntry& l = col.items[r];
					ImGui::PushID(c * 100 + r);
					if (PobUi::Link(l.tr ? S.*l.tr : l.label))
						ShellExecuteW(nullptr, L"open", l.url, nullptr, nullptr, SW_SHOWNORMAL);
					ImGui::PopID();
				}
				if (c != kLinkColumnCount - 1 && col.count > kShort) {
					char more[64];
					snprintf(more, sizeof(more), S.linksMore, col.count - kShort);
					ImGui::PushID(c);
					if (PobUi::Link(linksExpanded ? S.linksLess : more, Tok::TextFaint)) linksExpanded = !linksExpanded;
					ImGui::PopID();
				}
				// Ours, not the game's: under our own tools, in accent.
				if (c == kLinkColumnCount - 1) {
					ImGui::Dummy(ImVec2(0, D(8.0f)));
					if (PobUi::Link(S.discord, Tok::AccentText))
						ShellExecuteW(nullptr, L"open", L"https://discord.gg/6VamPQb8nC", nullptr, nullptr, SW_SHOWNORMAL);
					// One sponsor page of our own, so the payment provider can change
					// without shipping a new build.
					if (PobUi::Link(S.support, Tok::AccentText))
						ShellExecuteW(nullptr, L"open", L"https://hsiung-shao.github.io/support/", nullptr, nullptr, SW_SHOWNORMAL);
				}
				ImGui::PopFont();
			}
			ImGui::EndTable();
		}
		PobUi::CardEnd();

		ImGui::EndChild();
		ImGui::EndTabItem();
		} // home tab
		if (pHome) pageTabStyle(true);

		// ---- version history --------------------------------------------------
		// Release list on the left (searchable), the releases on the right; a
		// click scrolls to that release and scrolling moves the highlight.
		if (pagesOk && pageTab(S.changelog, 1, pageFlags[1])) {
			pageTabStyle(false);
			if (clZh != (int)zhUi) {
				changelog = ParseChangelog(zhUi);
				clZh = (int)zhUi;
				clJump = -1;
			}
			ImGui::Dummy(ImVec2(0, D(16.0f)));
			const float asideW = std::floor(D(240.0f));
			ImGui::BeginChild("##claside", ImVec2(asideW, 0), false);
			{
				PobUi::PushControlFrame();
				ImGui::SetNextItemWidth(asideW - D(4.0f));
				const std::string hint = (fonts.icons ? std::string(PobIcon::Search) + "  " : std::string()) + S.clSearchHint;
				ImGui::InputTextWithHint("##clsearch", hint.c_str(), &clSearch);
				PobUi::PopControlFrame();
				ImGui::Dummy(ImVec2(0, D(4.0f)));
				std::string needle = clSearch;
				for (char& c : needle) c = (char)tolower((unsigned char)c);
				clVisible.clear();
				for (int i = 0; i < (int)changelog.size(); i++)
					if (needle.empty() || changelog[i].search.find(needle) != std::string::npos) clVisible.push_back(i);
				const std::string current = "v" POBTOOLS_VERSION_STRING;
				for (int vi = 0; vi < (int)clVisible.size(); vi++) {
					const ChangelogRelease& r = changelog[clVisible[vi]];
					ImGui::PushID(clVisible[vi]);
					const ImVec2 rp = ImGui::GetCursorScreenPos();
					const bool active = clVisible[vi] == clActive;
					if (PobUi::SideNavItem(nullptr, r.version.c_str(), active, asideW - D(4.0f))) {
						clJump = clVisible[vi];
						clActive = clJump;
					}
					if (r.version == current) {
						const float pw = PobUi::PillWidth(S.clCurrent);
						const ImVec2 keep = ImGui::GetCursorScreenPos();
						ImGui::SetCursorScreenPos(ImVec2(rp.x + asideW - D(4.0f) - pw - D(8.0f),
						                                 rp.y + std::floor((ImGui::GetItemRectSize().y - D(24.0f)) * 0.5f)));
						PobUi::StatusPill(PobUi::Tone::Run, S.clCurrent);
						ImGui::SetCursorScreenPos(keep);
					}
					ImGui::PopID();
				}
			}
			ImGui::EndChild();
			ImGui::SameLine(0, D(24.0f));
			ImGui::PushStyleColor(ImGuiCol_ChildBg, PobUi::TokV4(Tok::Surface1));
			ImGui::PushStyleColor(ImGuiCol_Border, PobUi::TokV4(Tok::Border));
			ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, D(8.0f));
			ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(D(24.0f), D(24.0f)));
			ImGui::BeginChild("##clbody", ImVec2(0, -D(8.0f)), true, ImGuiWindowFlags_AlwaysUseWindowPadding);
			ImGui::PopStyleVar(2);
			ImGui::PopStyleColor(2);
			{
				const float wrapW = ImGui::GetContentRegionAvail().x;
				if (clVisible.empty()) {
					PobUi::Hint(S.clNoMatch);
				}
				const float scrollY = ImGui::GetScrollY();
				int topmost = -1;
				for (int vi = 0; vi < (int)clVisible.size(); vi++) {
					const int idx = clVisible[vi];
					const ChangelogRelease& r = changelog[idx];
					if (vi > 0) {
						ImGui::Dummy(ImVec2(0, D(4.0f)));
						const ImVec2 sp = ImGui::GetCursorScreenPos();
						ImGui::GetWindowDrawList()->AddLine(sp, ImVec2(sp.x + wrapW, sp.y), Tok::BorderSubtle, 1.0f);
						ImGui::Dummy(ImVec2(0, D(16.0f)));
					}
					const float y = ImGui::GetCursorPosY();
					if (clJump == idx) { ImGui::SetScrollY(y - D(8.0f)); clLockY = y - D(8.0f); clJump = -1; }
					if (y <= scrollY + D(48.0f)) topmost = idx;
					// heading: version in accent, date beside it
					{
						ImFont* hf = vi == 0 ? fonts.title : fonts.heading;
						ImGui::PushFont(hf);
						ImGui::PushStyleColor(ImGuiCol_Text, PobUi::TokV4(Tok::AccentText));
						ImGui::TextUnformatted(r.version.c_str());
						ImGui::PopStyleColor();
						ImGui::PopFont();
						if (!r.date.empty()) {
							ImGui::SameLine(0, D(12.0f));
							const float base = ImGui::GetItemRectMax().y;
							const ImVec2 cp = ImGui::GetCursorScreenPos();
							ImGui::SetCursorScreenPos(ImVec2(cp.x, base - fonts.small->FontSize - D(2.0f)));
							PobUi::Numeric(r.date.c_str());
						}
						ImGui::Dummy(ImVec2(0, D(8.0f)));
					}
					for (const ChangelogLine& l : r.lines) {
						if (l.heading) {
							ImGui::Dummy(ImVec2(0, D(4.0f)));
							PobUi::Overline(l.text.c_str());
							ImGui::Dummy(ImVec2(0, D(2.0f)));
						} else {
							const float ind = l.bullet ? D(18.0f) : 0.0f;
							if (l.bullet) {
								const ImVec2 bp = ImGui::GetCursorScreenPos();
								ImGui::GetWindowDrawList()->AddCircleFilled(
								    ImVec2(bp.x + D(6.0f), bp.y + fonts.body->FontSize * 0.55f), D(2.0f), Tok::TextFaint, 8);
								ImGui::Indent(ind);
							}
							ImGui::PushTextWrapPos(0.0f);
							ImGui::TextUnformatted(l.text.c_str());
							ImGui::PopTextWrapPos();
							if (l.bullet) ImGui::Unindent(ind);
							ImGui::Dummy(ImVec2(0, D(2.0f)));
						}
					}
					ImGui::Dummy(ImVec2(0, D(8.0f)));
				}
				// The highlight follows the scroll, except right after a click: the
				// last releases are too short to reach the top, and the one clicked
				// must stay lit until the user scrolls themselves.
				if (clLockY >= 0.0f) {
					if (std::fabs(ImGui::GetScrollY() - std::min(clLockY, ImGui::GetScrollMaxY())) > 2.0f && clJump < 0 &&
					    ImGui::GetIO().MouseWheel != 0.0f)
						clLockY = -1.0f;
				} else if (topmost >= 0) {
					clActive = topmost;
				} else if (!clVisible.empty()) {
					clActive = clVisible[0];
				}
			}
			ImGui::EndChild();
			ImGui::EndTabItem();
			pageTabStyle(true);
		}

		// ---- settings ---------------------------------------------------------
		// Side navigation (seven groups) + one card per group. A click scrolls to
		// the card; scrolling moves the highlight. Every change is written at once
		// (saveNow), so there is no save button.
		if (pagesOk && pageTab(S.tabSettings, 2, pageFlags[2])) {
			pageTabStyle(false);
			ImGui::Dummy(ImVec2(0, D(16.0f)));
			const char* navIcons[kSettingsSections] = { PobIcon::AlignLeft, PobIcon::AppWindow, PobIcon::Globe,
			                                            PobIcon::TrendingUp, PobIcon::Link, PobIcon::Folder, PobIcon::FileText };
			const char* navLabels[kSettingsSections] = { S.navInterface, S.navLaunch, S.navNetwork, S.sectionPobPerf,
			                                             S.sectionPobLinks, S.sectionTransData, S.navLog };
			const float navW = std::floor(D(200.0f));
			ImGui::BeginChild("##setnav", ImVec2(navW, 0), false);
			for (int i = 0; i < kSettingsSections; i++) {
				ImGui::PushID(i);
				if (PobUi::SideNavItem(navIcons[i], navLabels[i], i == setActive, navW - D(4.0f))) {
					setJump = i;
					setActive = i;
				}
				ImGui::PopID();
				ImGui::Dummy(ImVec2(0, D(0.0f)));
			}
			ImGui::Dummy(ImVec2(0, D(12.0f)));
			ImGui::Indent(D(12.0f));
			PobUi::Hint(S.settingsAutoSave);
			ImGui::Unindent(D(12.0f));
			ImGui::EndChild();
			ImGui::SameLine(0, D(24.0f));
			ImGui::BeginChild("##settingsbody", ImVec2(0, 0), false);
			const float cardW = ImGui::GetContentRegionAvail().x - D(4.0f);
			const float gapRow = PobUi::RowGap();
			int topmost = -1;
			const float scrollY = ImGui::GetScrollY();
			auto section = [&](int i) {
				if (i > 0) ImGui::Dummy(ImVec2(0, D(12.0f)));
				const float y = ImGui::GetCursorPosY();
				if (setJump == i) { ImGui::SetScrollY(y); setLockY = y; setJump = -1; }
				if (y <= scrollY + D(24.0f)) topmost = i;
			};

			// 1. interface and font --------------------------------------------
			section(0);
			PobUi::CardBegin("##s1", navIcons[0], navLabels[0], nullptr, false, cardW);
			{
				// Language: the display name, with which dictionary sets actually
				// have it as a note. A language present only for PoE1 is still
				// offered and the other game then shows the original text.
				std::vector<std::string> names, notes;
				for (const LocaleInfo& l : locales) {
					names.push_back(l.displayName);
					std::string note;
					if (l.id != "en") {
						const bool p1 = l.slot[(int)DictSlot::Poe1], p2 = l.slot[(int)DictSlot::Poe2];
						if (p1 && !p2) note = u8"僅 PoE1";
						else if (!p1 && p2) note = u8"僅 PoE2";
					}
					notes.push_back(note);
				}
				std::vector<const char*> np, nn;
				for (size_t i = 0; i < names.size(); i++) { np.push_back(names[i].c_str()); nn.push_back(notes[i].c_str()); }
				const float selW = std::floor(D(220.0f));
				PobUi::RowBegin(S.language, nullptr, selW);
				int sel = localeIdx;
				if (PobUi::Select("##locale", &sel, np.data(), nn.data(), (int)np.size(), selW) && sel != localeIdx) {
					const bool wasKo = locales[localeIdx].id == "ko-KR";
					const bool isKo = locales[sel].id == "ko-KR";
					localeIdx = sel;
					saveNow();
					// The full-atlas ladder keeps the ACTIVE language's script longest
					// (LoadFonts). An atlas that had to drop a block was built for the
					// other preference, so rebuild it.
					if (wasKo != isKo && !fonts.dropped.empty()) fontChanged = true;
				}
				PobUi::RowEnd();
			}
			{
				// What "Launch" opens. Offered only where the new interface can run;
				// elsewhere the row says why and the saved choice is left alone.
				const char* opts[2] = { S.uiModeClassic, S.uiModeModern };
				PobUi::RowBegin(S.uiModeLabel, modernUiOk ? S.uiModeHintShort : S.uiModeUnavailable,
				                PobUi::SegmentedWidth(opts, 2), 0.0f, modernInBrowser ? S.uiModeBrowser : nullptr,
				                !modernUiOk);
				int um = cfg.uiMode == 1 ? 1 : 0;
				if (PobUi::Segmented("##uimode", &um, opts, 2, modernUiOk) && modernUiOk) {
					cfg.uiMode = um;
					saveNow();
				}
				PobUi::RowEnd();
			}
			{
				// Fonts\*.ttf; switching rebuilds the atlas live, between frames.
				auto fontStem = [](const std::wstring& f) {
					std::string s = to_utf8(f);
					size_t d = s.rfind(".ttf");
					if (d == std::string::npos) d = s.rfind(".TTF");
					return d != std::string::npos ? s.substr(0, d) : s;
				};
				std::vector<std::string> stems;
				int cur = -1;
				for (size_t i = 0; i < fontList.size(); i++) {
					stems.push_back(fontStem(fontList[i]));
					if (fontList[i] == cfg.fontFile) cur = (int)i;
				}
				std::vector<const char*> sp;
				for (const std::string& s : stems) sp.push_back(s.c_str());
				const float selW = std::floor(D(180.0f));
				const float btnW = PobUi::ButtonWidth(S.installFont);
				PobUi::RowBegin(S.font, S.fontHint, selW + gapRow + btnW);
				int sel = cur;
				if (PobUi::Select("##font", &sel, sp.data(), nullptr, (int)sp.size(), selW) && sel >= 0 &&
				    fontList[sel] != cfg.fontFile) {
					cfg.fontFile = fontList[sel];
					fontChanged = true;
					saveNow();
				}
				ImGui::SameLine(0, gapRow);
				if (PobUi::Button(S.installFont)) {
					const std::wstring src = EdOpenFontDialog();
					if (!src.empty()) {
						const std::wstring name = src.substr(src.find_last_of(L'\\') + 1);
						const std::wstring dst = exeDir + L"Fonts\\" + name;
						switch (ClassifyFontFile(read_file(src))) {
							case FontKind::CffOutlines: PobUi::ShowToast(S.fontCff, PobUi::Tone::Bad); break;
							case FontKind::NotAFont:    PobUi::ShowToast(S.fontNotAFont, PobUi::Tone::Bad); break;
							case FontKind::TrueType:
								// Never overwrite: the target may be one of the shipped fonts.
								if (GetFileAttributesW(dst.c_str()) != INVALID_FILE_ATTRIBUTES) {
									PobUi::ShowToast(S.fontAlreadyThere, PobUi::Tone::Warn);
									cfg.fontFile = name;
									fontChanged = true;
									saveNow();
								} else if (CopyFileW(src.c_str(), dst.c_str(), TRUE)) {
									PobUi::ShowToast((std::string(S.fontInstalled) + to_utf8(name)).c_str());
									fontList = ListAvailableFonts(exeDir);
									cfg.fontFile = name;
									fontChanged = true;
									saveNow();
								} else {
									PobUi::ShowToast(S.fontCopyFailed, PobUi::Tone::Bad);
								}
								break;
						}
					}
				}
				PobUi::RowEnd();
			}
			{
				// Engine-side ASCII override (POB_ZH_FONT_ALL), read when POB starts.
				PobUi::RowBegin(S.fontApplyAllChk, S.fontApplyAllHint, PobUi::SwitchWidth(), 0.0f, S.fontApplyAllTip);
				bool applyAll = cfg.fontApplyAll;
				if (PobUi::Switch("##fontall", &applyAll)) {
					cfg.fontApplyAll = applyAll;
					saveNow();
				}
				PobUi::RowEnd();
			}
			{
				// Font size = whole-UI zoom (LauncherZoom). Applied when the slider
				// is RELEASED, not per tick: each apply rebuilds the atlas.
				const float track = std::floor(D(160.0f));
				PobUi::RowBegin(S.fontSizeLabel, S.fontSizeHint, PobUi::SliderWithResetWidth(track, S.resetDefault));
				const PobUi::SliderResult r = PobUi::SliderWithReset("##fontsize", &fontSizeEdit, kLauncherFontSizeMin,
				    kLauncherFontSizeMax, kLauncherFontSizeDefault, "%d px", S.resetDefault, S.sliderDefault, track);
				if (r.released && fontSizeEdit != cfg.fontSize) {
					cfg.fontSize = ClampLauncherFontSize(fontSizeEdit);
					saveNow();
					pendingScale = dpiScale * LauncherZoom(cfg.fontSize);
				}
				if (r.reset && cfg.fontSize != kLauncherFontSizeDefault) {
					cfg.fontSize = kLauncherFontSizeDefault;
					saveNow();
					pendingScale = dpiScale;
				}
				if (!r.active) fontSizeEdit = cfg.fontSize;
				PobUi::RowEnd();
			}
			{
				// Window size for the CURRENT mode, physical pixels. The fields mirror
				// the live window whenever they are not being edited. Commit on Enter
				// or on leaving the field.
				const float fieldW = std::floor(D(76.0f));
				const float xW = ImGui::CalcTextSize("x").x;
				const float resetW = PobUi::ButtonWidth(S.resetDefault, PobUi::BtnSize::Sm);
				PobUi::RowBegin(S.winSizeLabel, S.winSizeHint, fieldW * 2 + xW + resetW + gapRow * 3);
				ImGui::PushID("winsize");
				PobUi::PushControlFrame();
				bool commit = false, active[2] = { false, false };
				ImGui::SetNextItemWidth(fieldW);
				commit |= ImGui::InputInt("##w", &winEdit[0], 0, 0, ImGuiInputTextFlags_EnterReturnsTrue);
				commit |= ImGui::IsItemDeactivatedAfterEdit();
				active[0] = ImGui::IsItemActive();
				ImGui::SameLine(0, gapRow);
				ImGui::AlignTextToFramePadding();
				ImGui::TextDisabled("x"); // ASCII on purpose: U+00D7 is not in every shipped font
				ImGui::SameLine(0, gapRow);
				ImGui::SetNextItemWidth(fieldW);
				commit |= ImGui::InputInt("##h", &winEdit[1], 0, 0, ImGuiInputTextFlags_EnterReturnsTrue);
				commit |= ImGui::IsItemDeactivatedAfterEdit();
				active[1] = ImGui::IsItemActive();
				PobUi::PopControlFrame();
				if (commit && winEdit[0] > 0 && winEdit[1] > 0 && (winEdit[0] != lastW || winEdit[1] != lastH))
					applyWindowSize(winEdit[0], winEdit[1], true);
				if (!active[0]) winEdit[0] = lastW;
				if (!active[1]) winEdit[1] = lastH;
				ImGui::SameLine(0, gapRow);
				if (PobUi::Button(S.resetDefault, PobUi::BtnKind::Ghost, PobUi::BtnSize::Sm)) {
					int w, h;
					defaultWinSize(&w, &h);
					applyWindowSize(w, h, false);
				}
				ImGui::PopID();
				PobUi::RowEnd();
			}
			{
				// Whether the CURRENT font can draw the CURRENT language, and whether
				// the atlas had to be cut down for this GPU: said out loud, inside the
				// card they concern.
				const bool missingHere = localeIdx >= 0 && localeIdx < (int)localeDrawable.size() && !localeDrawable[localeIdx];
				if (missingHere || fonts.dropped == "cjk") {
					const ImVec2 cp = ImGui::GetCursorScreenPos();
					ImGui::SetCursorScreenPos(ImVec2(PobUi::CardInnerX(), cp.y));
					if (missingHere) {
						const std::string t = std::string(S.fontMissingHere) +
						    (localeIdx < (int)localeMissing.size() ? localeMissing[localeIdx] : std::string());
						PobUi::Banner("##fontmiss", PobUi::BannerTone::Warn, PobIcon::TriangleAlert, t.c_str(), nullptr,
						              false, nullptr, false, true, PobUi::CardInnerWidth());
						ImGui::SetCursorScreenPos(ImVec2(PobUi::CardInnerX(), ImGui::GetCursorScreenPos().y));
					}
					if (fonts.dropped == "cjk")
						PobUi::Banner("##fonttrim", PobUi::BannerTone::Warn, PobIcon::TriangleAlert, S.fontAtlasTrimmed,
						              nullptr, false, nullptr, false, true, PobUi::CardInnerWidth());
					ImGui::Dummy(ImVec2(0, D(8.0f)));
				}
			}
			PobUi::CardEnd();

			// 2. launch and windows -----------------------------------------------
			section(1);
			PobUi::CardBegin("##s2", navIcons[1], navLabels[1], nullptr, false, cardW);
			{
				const char* opts[2] = { S.winModeSeparateShort, S.winModeTabbedShort };
				PobUi::RowBegin(S.sectionWindow, S.winModeHintShort, PobUi::SegmentedWidth(opts, 2));
				int wm = (int)cfg.windowMode;
				if (PobUi::Segmented("##winmode", &wm, opts, 2)) {
					cfg.windowMode = (WindowMode)wm;
					saveNow();
				}
				PobUi::RowEnd();
			}
			{
				// One choice, not two checkboxes: "return afterwards" and "stay open"
				// cannot both be true. Ignored in tabbed mode -- this window IS where
				// POB lives -- so it is disabled there and says so.
				const char* opts[3] = { S.exitModeClose, S.returnAfterExit, S.exitModeKeepOpen };
				const float selW = std::floor(D(300.0f));
				PobUi::RowBegin(S.sectionLaunch, tabbed ? S.exitModeTabbedNote : nullptr, selW, 0.0f, nullptr, tabbed);
				int em = (int)cfg.exitMode;
				if (PobUi::Select("##exitmode", &em, opts, nullptr, 3, selW, !tabbed) && !tabbed) {
					cfg.exitMode = (LaunchExitMode)em;
					saveNow();
				}
				PobUi::RowEnd();
			}
			{
				const char* opts[2] = { S.tabHome, S.changelog };
				PobUi::RowBegin(S.startupTabLabel, nullptr, PobUi::SegmentedWidth(opts, 2));
				int st = (int)cfg.startupTab;
				if (PobUi::Segmented("##startup", &st, opts, 2)) {
					cfg.startupTab = (StartupTab)st;
					saveNow();
				}
				PobUi::RowEnd();
			}
			// The mode is decided once, when this window is created (it changes
			// whether the docking callbacks exist), so a change needs a restart --
			// which the banner offers rather than leaving the user to find it.
			if (cfg.windowMode != (tabbed ? WindowMode::Tabbed : WindowMode::Separate)) {
				const ImVec2 cp = ImGui::GetCursorScreenPos();
				ImGui::SetCursorScreenPos(ImVec2(PobUi::CardInnerX(), cp.y));
				if (PobUi::Banner("##restart", PobUi::BannerTone::Warn, PobIcon::Refresh, S.winModeRestart, nullptr, false,
				                  S.relaunchNow, false, true, PobUi::CardInnerWidth()) == PobUi::BannerResult::Action) {
					relaunchRequested = true;
					glfwSetWindowShouldClose(win, GLFW_TRUE);
				}
				ImGui::Dummy(ImVec2(0, D(8.0f)));
			}
			PobUi::CardEnd();

			// 3. network and updates -----------------------------------------------
			section(2);
			PobUi::CardBegin("##s3", navIcons[2], navLabels[2], nullptr, false, cardW);
			{
				// Reaches the HTTP layer immediately: the update worker opens a new
				// session per operation, so the next check already uses it.
				auto applyProxy = [&](const std::string& v) {
					std::wstring w = from_utf8(v);
					if (w == cfg.proxy) return;
					cfg.proxy = w;
					saveNow();
					HttpSetManualProxy(cfg.proxy);
				};
				const float fw = std::floor(D(240.0f));
				PobUi::RowBegin(S.proxyLabel, S.proxyHintShort, fw);
				PobUi::PushControlFrame();
				ImGui::SetNextItemWidth(fw);
				if (ImGui::InputTextWithHint("##proxy", S.proxyEmptyHint, &proxyEdit, ImGuiInputTextFlags_EnterReturnsTrue))
					applyProxy(proxyEdit);
				// Clicking away must not discard what was typed.
				if (ImGui::IsItemDeactivatedAfterEdit()) applyProxy(proxyEdit);
				if (!ImGui::IsItemActive()) proxyEdit = to_utf8(cfg.proxy);
				PobUi::PopControlFrame();
				PobUi::RowEnd();
			}
			{
				PobUi::RowBegin(S.autoAppUpdate, S.autoAppUpdateHintShort, PobUi::SwitchWidth(), 0.0f, S.autoAppUpdateHint);
				bool autoApp = cfg.autoApplyAppUpdate;
				if (PobUi::Switch("##autoapp", &autoApp)) {
					cfg.autoApplyAppUpdate = autoApp;
					saveNow();
				}
				PobUi::RowEnd();
			}
			{
				PobUi::RowBegin(S.betaChannel, S.betaHintShort, PobUi::SwitchWidth(), 0.0f, S.betaChannelHint);
				bool beta = cfg.betaChannel;
				if (PobUi::Switch("##beta", &beta)) {
					cfg.betaChannel = beta;
					saveNow();
					// Straight to the worker, and check again right away: somebody who
					// just switched this on wants to know now whether there is an
					// early build.
					if (appUpd) {
						appUpd->SetBetaChannel(beta);
						appUpd->RequestCheck(AppUpdater::CheckReason::UserAsked);
					}
				}
				PobUi::RowEnd();
			}
			{
				// The update gate, with the data version beside it -- without it the
				// translation data has no visible version at all.
				const std::string dv = ust.localDataVer.empty() ? std::string(S.transDataUnstamped) : ust.localDataVer;
				const float dvW = fonts.small->CalcTextSizeA(fonts.small->FontSize, FLT_MAX, 0.0f, dv.c_str()).x;
				PobUi::RowBegin(S.transUpdateLabel, S.transUpdateHintShort, dvW + gapRow + PobUi::SwitchWidth());
				{
					const ImVec2 c = ImGui::GetCursorScreenPos();
					ImGui::Dummy(ImVec2(dvW, PobUi::ControlH()));
					ImGui::GetWindowDrawList()->AddText(fonts.small, fonts.small->FontSize,
					            ImVec2(c.x, c.y + std::floor((PobUi::ControlH() - fonts.small->FontSize) * 0.5f)),
					            Tok::TextMuted, dv.c_str());
				}
				ImGui::SameLine(0, gapRow);
				bool tu = cfg.updateTranslations;
				if (PobUi::Switch("##transupd", &tu)) {
					cfg.updateTranslations = tu;
					saveNow();
					// The worker applies packs on its own schedule, so the setting has
					// to reach it immediately, not at next start.
					if (appUpd) appUpd->SetTranslationUpdates(tu);
				}
				PobUi::RowEnd();
			}
			PobUi::CardEnd();

			// 4. POB performance ---------------------------------------------------
			// The frame caps apply live (PobLaunch::ApplyPobFrameCap); the
			// diagnostics log only on the next POB start.
			section(3);
			PobUi::CardBegin("##s4", navIcons[3], navLabels[3], S.pobPerfNote, false, cardW);
			{
				auto fpsSelect = [&](const char* id, const char* label, const char* hint, const char* tip, int& value,
				                     std::initializer_list<int> options) {
					std::vector<std::string> names;
					std::vector<int> vals(options);
					int sel = -1;
					for (size_t i = 0; i < vals.size(); i++) {
						names.push_back(vals[i] <= 0 ? std::string(S.pobFpsUnlimited) : std::to_string(vals[i]) + " fps");
						if (vals[i] == value) sel = (int)i;
					}
					std::vector<const char*> np;
					for (const std::string& s : names) np.push_back(s.c_str());
					const float selW = std::floor(D(140.0f));
					PobUi::RowBegin(label, hint, selW, 0.0f, tip);
					bool changed = false;
					if (PobUi::Select(id, &sel, np.data(), nullptr, (int)np.size(), selW) && sel >= 0) {
						value = vals[sel];
						changed = true;
					}
					PobUi::RowEnd();
					return changed;
				};
				bool capChanged = false;
				capChanged |= fpsSelect("##pobfpsfg", S.pobFpsForeground, nullptr, nullptr, cfg.pobFpsForeground, { 0, 30, 60, 90, 120, 144 });
				capChanged |= fpsSelect("##pobfpsbg", S.pobFpsBackground, S.pobFpsHintShort, S.pobFpsHint, cfg.pobFpsBackground, { 0, 5, 10, 15, 30 });
				if (capChanged) {
					saveNow();
					PobLaunch::ApplyPobFrameCap(cfg.pobFpsForeground, cfg.pobFpsBackground);
				}
				PobUi::RowBegin(S.perfLogChk, S.perfLogHintShort, PobUi::SwitchWidth(), 0.0f, S.perfLogHint);
				bool perf = cfg.perfLog;
				if (PobUi::Switch("##perflog", &perf)) {
					cfg.perfLog = perf;
					saveNow();
				}
				PobUi::RowEnd();
			}
			PobUi::CardEnd();

			// 5. "Open in PoB" links (pob:// / pob2://) -----------------------------
			// Under Wine the browser runs on the host system, so a link clicked there
			// never reaches this registry: the row stays, disabled, and says so.
			section(4);
			PobUi::CardBegin("##s5", navIcons[4], navLabels[4], nullptr, false, cardW);
			{
				const bool wine = PobLaunch::RunningUnderWine();
				wchar_t exeBuf[MAX_PATH] = {};
				GetModuleFileNameW(nullptr, exeBuf, MAX_PATH);
				if (!wine && ImGui::GetTime() - protoQueriedAt > 2.0) {
					protoState = (int)PobProtocol::QueryPobProtocol(exeBuf);
					protoQueriedAt = ImGui::GetTime();
				}
				const char* pill = protoState == (int)PobProtocol::State::Ours ? S.pobProtoRegistered
				                 : protoState == (int)PobProtocol::State::Other ? S.pobProtoOther : S.pobProtoNotRegistered;
				const PobUi::Tone tone = protoState == (int)PobProtocol::State::Ours ? PobUi::Tone::Ok
				                       : protoState == (int)PobProtocol::State::Other ? PobUi::Tone::Warn : PobUi::Tone::Idle;
				PobUi::RowBegin(S.pobProtocolChk, wine ? S.pobProtocolWine : S.pobProtocolHintShort,
				                (wine ? 0.0f : PobUi::PillWidth(pill) + gapRow) + PobUi::SwitchWidth(), 0.0f,
				                S.pobProtocolHint, wine);
				if (!wine) {
					PobUi::StatusPill(tone, pill);
					ImGui::SameLine(0, gapRow);
				}
				bool proto = cfg.pobProtocol;
				if (PobUi::Switch("##proto", &proto, !wine) && !wine) {
					const bool ok = proto ? PobProtocol::RegisterPobProtocol(exeBuf)
					                      : PobProtocol::UnregisterPobProtocol(exeBuf);
					if (ok) {
						cfg.pobProtocol = proto;
						saveNow();
					} else {
						// Half-written registration: take back whatever did land.
						if (proto) PobProtocol::UnregisterPobProtocol(exeBuf);
						PobUi::ShowToast(S.pobProtocolFail, PobUi::Tone::Bad);
					}
					protoQueriedAt = -10.0;   // re-read the state on the next frame
				}
				PobUi::RowEnd();
			}
			PobUi::CardEnd();

			// 6. translation data -------------------------------------------------
			// Three independently redirectable sets. Each row: name + where it comes
			// from (pill) | path, browse, copy-to / back-to-built-in | status.
			section(5);
			PobUi::CardBegin("##s6", navIcons[5], navLabels[5], S.transDataNote, false, cardW);
			{
				const char* slotLabel[kDictSlotCount] = { S.poe1, S.poe2, S.slotLauncher };
				const float labelCol = std::floor(D(170.0f));
				for (int i = 0; i < kDictSlotCount; i++) {
					ImGui::PushID(i);
					const std::wstring builtin = BuiltinDictDir(exeDir, (DictSlot)i);
					const DictDirInfo& dd = dictDir[i];
					// Re-resolve only when the path actually changes: ResolveDictDir
					// walks the folder tree.
					auto applyPath = [&](const std::wstring& p) {
						cfg.dataDir[i] = p;
						resolveDict(i);
						saveNow();
					};
					const float x0 = PobUi::CardInnerX(), w = PobUi::CardInnerWidth();
					const float y0 = ImGui::GetCursorScreenPos().y;
					ImDrawList* cdl = ImGui::GetWindowDrawList();
					if (i > 0) cdl->AddLine(ImVec2(x0 - D(20.0f) + 1.0f, y0), ImVec2(x0 + w + D(20.0f) - 1.0f, y0), Tok::BorderSubtle, 1.0f);
					// label + pill
					ImGui::SetCursorScreenPos(ImVec2(x0, y0 + D(16.0f)));
					ImGui::BeginGroup();
					ImGui::TextUnformatted(slotLabel[i]);
					{
						const bool problem = dd.status != DataDirStatus::Builtin && dd.status != DataDirStatus::External;
						PobUi::StatusPill(problem ? PobUi::Tone::Warn : dd.status == DataDirStatus::External ? PobUi::Tone::Warn : PobUi::Tone::Idle,
						                  problem ? S.dataPillProblem : dd.status == DataDirStatus::External ? S.dataPillExternal : S.dataPillBuiltin);
					}
					ImGui::EndGroup();
					const float leftBottom = ImGui::GetItemRectMax().y;
					// path + buttons + status
					const float cx = x0 + labelCol;
					const float cw = w - labelCol;
					ImGui::SetCursorScreenPos(ImVec2(cx, y0 + D(16.0f)));
					ImGui::BeginGroup();
					const bool hasPath = !cfg.dataDir[i].empty();
					const char* second = hasPath ? S.useBuiltin : S.copyBuiltin;
					const float bw1 = PobUi::ButtonWidth(S.browse), bw2 = PobUi::ButtonWidth(second);
					PobUi::PushControlFrame();
					ImGui::SetNextItemWidth(std::max(D(120.0f), cw - bw1 - bw2 - gapRow * 2));
					// The hint says what empty MEANS, not what the built-in path is.
					if (ImGui::InputTextWithHint("##datadir", S.dataDirEmptyHint, &dirEdit[i], ImGuiInputTextFlags_EnterReturnsTrue))
						applyPath(from_utf8(dirEdit[i]));
					// Enter is not the only way people finish typing.
					if (ImGui::IsItemDeactivatedAfterEdit()) applyPath(from_utf8(dirEdit[i]));
					if (!ImGui::IsItemActive()) dirEdit[i] = to_utf8(cfg.dataDir[i]);
					PobUi::PopControlFrame();
					ImGui::SameLine(0, gapRow);
					if (PobUi::Button(S.browse)) {
						std::wstring picked = EdBrowseForFolder(L"選擇翻譯資料夾", cfg.dataDir[i].empty() ? builtin : cfg.dataDir[i]);
						if (!picked.empty()) applyPath(picked);
					}
					ImGui::SameLine(0, gapRow);
					if (hasPath) {
						if (PobUi::Button(S.useBuiltin, PobUi::BtnKind::Ghost)) applyPath(std::wstring());
					} else if (PobUi::Button(S.copyBuiltin)) {
						copyDest = EdBrowseForFolder(L"複製內建翻譯資料到…", builtin);
						copySlot = i;
						if (!copyDest.empty()) {
							if (DictionariesPresentAt(copyDest)) askOverwrite = true;
							else doCopy = true;
						}
					}
					// Status. Every failure mode says what is wrong AND what to do.
					ImGui::PushFont(fonts.small);
					ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + cw);
					const ImVec4 warn = PobUi::TokV4(Tok::Warning);
					switch (dd.status) {
						case DataDirStatus::Builtin: {
							// Relative to the app folder; the full path is one hover away.
							const std::string rel = std::string("Data\\") + to_utf8(DictSlotFolder((DictSlot)i)) + "\\";
							ImGui::TextDisabled("%s", rel.c_str());
							if (ImGui::IsItemHovered()) PobUi::Tooltip(to_utf8(builtin).c_str());
							break;
						}
						case DataDirStatus::External: {
							std::string found;
							for (const auto& f : dd.found) {
								if (!found.empty()) found += " \xc2\xb7 ";
								found += f.first + " " + std::to_string(f.second);
							}
							ImGui::TextDisabled("%s", found.c_str());
							if (i == (int)DictSlot::Launcher) ImGui::TextDisabled("%s", S.dataDirRestart);
							break;
						}
						case DataDirStatus::Missing:
							ImGui::TextColored(warn, "%s", S.dataDirMissing);
							break;
						case DataDirStatus::WrongShape:
						case DataDirStatus::TooShallow:
							ImGui::TextColored(warn, "%s", dd.status == DataDirStatus::WrongShape ? S.dataDirWrongShape : S.dataDirTooShallow);
							if (!dd.suggestion.empty()) {
								ImGui::TextDisabled("%s", to_utf8(dd.suggestion).c_str());
								ImGui::SameLine(0, gapRow);
								if (PobUi::Button(S.useSuggestion, PobUi::BtnKind::Ghost, PobUi::BtnSize::Sm)) applyPath(dd.suggestion);
							}
							break;
						case DataDirStatus::NoDictionaries:
							ImGui::TextColored(warn, "%s", S.dataDirNoDict);
							break;
					}
					if (dd.insideInstall && dd.status != DataDirStatus::Builtin) ImGui::TextColored(warn, "%s", S.dataDirInside);
					if (!dd.staleLoadOrder.empty()) {
						std::string line = S.dataDirStale;
						for (const std::string& s : dd.staleLoadOrder) line += " " + s;
						ImGui::TextColored(warn, "%s", line.c_str());
					}
					ImGui::PopTextWrapPos();
					ImGui::PopFont();
					ImGui::EndGroup();
					const float bottom = std::max(leftBottom, ImGui::GetItemRectMax().y) + D(16.0f);
					ImGui::SetCursorScreenPos(ImVec2(x0 - D(20.0f), y0));
					ImGui::Dummy(ImVec2(w + D(40.0f), bottom - y0));
					ImGui::SetCursorScreenPos(ImVec2(x0 - D(20.0f), bottom));
					ImGui::PopID();
				}
			}
			PobUi::CardEnd();

			// 7. problem log ------------------------------------------------------
			// Written by every part of the program; the one thing here a user only
			// needs when already stuck, which is why it says what to do with it.
			section(6);
			PobUi::CardBegin("##s7", navIcons[6], navLabels[6], nullptr, false, cardW);
			{
				PobUi::RowBegin(S.logFolderLabel, S.logFolderHint, PobUi::ButtonWidth(S.bgOpenFolder, PobUi::BtnSize::Md, PobIcon::FolderOpen),
				                0.0f, S.openLogFolderHint);
				if (PobUi::Button(S.bgOpenFolder, PobUi::BtnKind::Secondary, PobUi::BtnSize::Md, PobIcon::FolderOpen)) {
					// LogDir() creates the folder, so this never opens nothing.
					const std::wstring lg = PobLog::LogDir();
					if (!lg.empty()) ShellExecuteW(nullptr, L"open", lg.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
				}
				PobUi::RowEnd();
			}
			PobUi::CardEnd();
			ImGui::Dummy(ImVec2(0, std::max(D(24.0f), ImGui::GetWindowHeight() - D(320.0f))));

			// The highlight follows the scroll, except right after a click (see the
			// version history for why).
			if (setLockY >= 0.0f) {
				if (ImGui::GetIO().MouseWheel != 0.0f && ImGui::IsWindowHovered()) setLockY = -1.0f;
			} else if (topmost >= 0) {
				setActive = topmost;
			}
			ImGui::EndChild();
			ImGui::EndTabItem();
			pageTabStyle(true);
		}

		// ---- appearance ------------------------------------------------------
		// POB window look, one set per game; Windows only.
		if (pagesOk && pageTab(S.tabAppearance, 3, pageFlags[3])) {
			pageTabStyle(false);
			ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(D(4.0f), D(24.0f)));
			ImGui::BeginChild("##lookbody", ImVec2(0, 0), false, ImGuiWindowFlags_AlwaysUseWindowPadding);
			ImGui::PopStyleVar();
			const float inner = ImGui::GetContentRegionAvail().x - D(4.0f);
			const bool wine = PobLaunch::RunningUnderWine();
			// header: title + one line, game switch on the right
			{
				const char* opts[2] = { S.poe1, S.poe2 };
				const float segW = PobUi::SegmentedWidth(opts, 2);
				const ImVec2 hp = ImGui::GetCursorScreenPos();
				ImGui::BeginGroup();
				ImGui::PushFont(fonts.heading);
				ImGui::TextUnformatted(S.lookTitle);
				ImGui::PopFont();
				PobUi::Hint(S.lookDesc, inner - segW - D(24.0f));
				ImGui::EndGroup();
				const float hh = ImGui::GetItemRectSize().y;
				if (!wine) {
					ImGui::SetCursorScreenPos(ImVec2(hp.x + inner - segW, hp.y + std::floor((hh - PobUi::ControlH()) * 0.5f)));
					PobUi::Segmented("##lookgame", &lookGame, opts, 2);
					ImGui::SetCursorScreenPos(ImVec2(hp.x, hp.y + std::max(hh, PobUi::ControlH())));
				}
				ImGui::Dummy(ImVec2(0, D(20.0f)));
			}
			if (wine) {
				// Not drawn at all under Wine / CrossOver (the POB window ignores
				// these there), but the page must not be blank either.
				PobUi::Banner("##lookwine", PobUi::BannerTone::Info, PobIcon::Info, S.lookWineNote, nullptr, false,
				              nullptr, false, true, inner);
			} else {
				AppearanceConfig& look = cfg.look[lookGame];
				const std::wstring lookGameKey = lookGame == 1 ? L"poe2" : L"poe1";
				// Scratch values mirror the setting whenever the slider is idle, so
				// switching the game re-syncs them.
				if (lookGameShown != lookGame) {
					opacityEdit = look.windowOpacity;
					bgBrightEdit = look.bgBright;
					glassEdit = look.glassBlur;
					treeBgEdit = look.treeBg;
					lookGameShown = lookGame;
				}
				// The image list is re-read when the page is opened (a file dropped
				// into the folder meanwhile just appears).
				if (pageShown != 3) {
					bgList = ListAvailableBackgrounds(exeDir);
					syncThumbs();
				}
				const float previewW = std::floor(D(380.0f));
				const float gapCol = D(24.0f);
				const float leftW = std::max(D(320.0f), inner - previewW - gapCol);
				const ImVec2 colTop = ImGui::GetCursorScreenPos();
				ImGui::BeginGroup();
				// background images: thumbnails, the selected one outlined, then "add"
				PobUi::CardBegin("##bgcard", PobIcon::Image, S.bgLabel, nullptr, true, leftW);
				if (PobUi::CardHeadButton(S.bgOpenFolder, PobUi::BtnKind::Ghost, PobIcon::FolderOpen)) {
					CreateDirectoryW(BackgroundsDir(exeDir).c_str(), nullptr);
					ShellExecuteW(nullptr, L"open", BackgroundsDir(exeDir).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
				}
				{
					const float gw = PobUi::CardInnerWidth();
					const float tg = D(12.0f);
					const int cols = 4;
					const float tw = std::floor((gw - tg * (cols - 1)) / (float)cols);
					const float imgH = std::floor(D(64.0f));
					const float th = imgH + D(8.0f) + fonts.small->FontSize + D(10.0f);
					const int count = (int)bgThumbs.size() + 2;   // built-in + files + add
					for (int i = 0; i < count; i++) {
						if (i % cols != 0) ImGui::SameLine(0, tg);
						ImGui::PushID(i);
						const ImVec2 tp = ImGui::GetCursorScreenPos();
						const bool clicked = ImGui::InvisibleButton("##bgt", ImVec2(tw, th));
						const bool hov = ImGui::IsItemHovered();
						ImDrawList* tdl = ImGui::GetWindowDrawList();
						const bool isAdd = i == count - 1;
						const bool isBuiltin = i == 0;
						const BgThumb* t = (!isAdd && !isBuiltin) ? &bgThumbs[i - 1] : nullptr;
						const bool selected = isBuiltin ? look.background.empty() : (t && t->file == look.background);
						const ImU32 edge = selected ? Tok::Accent : (hov ? Tok::BorderStrong : Tok::Border);
						if (!isAdd) tdl->AddRectFilled(tp, tp + ImVec2(tw, th), Tok::Surface2, D(8.0f));
						if (isAdd) {
							// dashed outline
							const float dash = D(6.0f);
							const ImVec2 a = tp, b = tp + ImVec2(tw, th);
							for (float x = a.x + D(4.0f); x < b.x - D(4.0f); x += dash * 2) {
								tdl->AddLine(ImVec2(x, a.y), ImVec2(std::min(x + dash, b.x - D(4.0f)), a.y), edge);
								tdl->AddLine(ImVec2(x, b.y - 1), ImVec2(std::min(x + dash, b.x - D(4.0f)), b.y - 1), edge);
							}
							for (float y = a.y + D(4.0f); y < b.y - D(4.0f); y += dash * 2) {
								tdl->AddLine(ImVec2(a.x, y), ImVec2(a.x, std::min(y + dash, b.y - D(4.0f))), edge);
								tdl->AddLine(ImVec2(b.x - 1, y), ImVec2(b.x - 1, std::min(y + dash, b.y - D(4.0f))), edge);
							}
							const float ip = std::floor(D(20.0f));
							if (fonts.icons) PobUi::IconAt(tdl, tp + ImVec2((tw - ip) * 0.5f, (imgH - ip) * 0.5f + D(4.0f)), PobIcon::ImagePlus, Tok::TextMuted, ip);
						} else {
							const ImVec2 ia = tp + ImVec2(1, 1), ib = tp + ImVec2(tw - 1, imgH);
							if (t && t->tex) {
								// cover-crop the thumbnail into the tile
								const float ar = (float)t->w / (float)std::max(1, t->h), tr = (ib.x - ia.x) / (ib.y - ia.y);
								ImVec2 uv0(0, 0), uv1(1, 1);
								if (ar > tr) { const float k = tr / ar; uv0.x = (1 - k) * 0.5f; uv1.x = 1 - uv0.x; }
								else { const float k = ar / tr; uv0.y = (1 - k) * 0.5f; uv1.y = 1 - uv0.y; }
								tdl->AddImageRounded((ImTextureID)(intptr_t)t->tex, ia, ib, uv0, uv1, IM_COL32_WHITE, D(7.0f), ImDrawFlags_RoundCornersTop);
							} else {
								// built-in backdrop, an image still decoding, or one stb
								// cannot read (WebP): a colour block stands in
								tdl->AddRectFilled(ia, ib, isBuiltin ? Tok::Canvas : Tok::Surface3, D(7.0f), ImDrawFlags_RoundCornersTop);
								if (isBuiltin && fonts.icons) {
									const float ip = std::floor(D(20.0f));
									PobUi::IconAt(tdl, ImVec2((ia.x + ib.x - ip) * 0.5f, (ia.y + ib.y - ip) * 0.5f), PobIcon::Zap, Tok::TextFaint, ip);
								}
							}
						}
						tdl->AddRect(tp, tp + ImVec2(tw, th), edge, D(8.0f), 0, selected ? 2.0f : 1.0f);
						const std::string cap = isAdd ? std::string(S.bgAdd) : isBuiltin ? std::string(S.bgBuiltinShort) : to_utf8(t->file);
						tdl->PushClipRect(tp + ImVec2(D(10.0f), imgH), tp + ImVec2(tw - D(10.0f), th), true);
						tdl->AddText(fonts.small, fonts.small->FontSize, tp + ImVec2(isAdd ? std::max(D(10.0f), (tw - fonts.small->CalcTextSizeA(fonts.small->FontSize, FLT_MAX, 0.0f, cap.c_str()).x) * 0.5f) : D(10.0f), imgH + D(6.0f)),
						             selected ? Tok::Text : Tok::TextMuted, cap.c_str());
						tdl->PopClipRect();
						if (hov) {
							ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
							if (t) PobUi::Tooltip(to_utf8(t->file).c_str());
						}
						if (clicked) {
							if (isAdd) {
								const std::wstring src = OpenImageDialog(glfwGetWin32Window(win));
								if (!src.empty()) {
									const std::wstring name = src.substr(src.find_last_of(L"\\/") + 1);
									const std::wstring dir = BackgroundsDir(exeDir);
									CreateDirectoryW(dir.c_str(), nullptr);
									const std::wstring dst = dir + L"\\" + name;
									const bool there = GetFileAttributesW(dst.c_str()) != INVALID_FILE_ATTRIBUTES;
									if (there || CopyFileW(src.c_str(), dst.c_str(), TRUE)) {
										bgList = ListAvailableBackgrounds(exeDir);
										syncThumbs();
										look.background = NormalizeBackgroundFile(name);
										PobLaunch::ApplyPobBackground(lookGameKey, ResolveBackgroundPath(exeDir, look.background));
										saveNow();
										PobUi::ShowToast((std::string(S.bgAdded) + to_utf8(name)).c_str());
									} else {
										PobUi::ShowToast(S.bgAddFailed, PobUi::Tone::Bad);
									}
								}
							} else if (isBuiltin) {
								if (!look.background.empty()) {
									look.background.clear();
									PobLaunch::ApplyPobBackground(lookGameKey, std::wstring());
									saveNow();
								}
							} else if (t->file != look.background) {
								look.background = t->file;
								PobLaunch::ApplyPobBackground(lookGameKey, ResolveBackgroundPath(exeDir, t->file));
								saveNow();
							}
						}
						ImGui::PopID();
					}
				}
				PobUi::CardEnd();
				ImGui::Dummy(ImVec2(0, D(12.0f)));
				// the four sliders: drag previews on every open POB window, release
				// writes the ini; each has "reset to default"
				PobUi::CardBegin("##lookcard", nullptr, nullptr, nullptr, false, leftW);
				{
					const float track = std::floor(D(160.0f));
					auto lookSlider = [&](const char* id, const char* label, const char* hint, const char* tip, int* edit,
					                      int& value, int min, int def, void (*apply)(const std::wstring&, int),
					                      int (*clampFn)(int, int)) {
						PobUi::RowBegin(label, hint, PobUi::SliderWithResetWidth(track, S.resetDefault), 0.0f, tip);
						const PobUi::SliderResult r = PobUi::SliderWithReset(id, edit, min, 100, def, "%d%%",
						                                                     S.resetDefault, S.sliderDefault, track);
						if (r.changed) apply(lookGameKey, *edit);
						if ((r.released || r.reset) && *edit != value) {
							value = clampFn(*edit, def);
							apply(lookGameKey, value);
							saveNow();
						}
						if (!r.active) *edit = value;
						PobUi::RowEnd();
					};
					lookSlider("##winopacity", S.winOpacityLabel, S.winOpacityHintShort, S.winOpacityHint, &opacityEdit,
					           look.windowOpacity, kWindowOpacityMin, kWindowOpacityDefault, &PobLaunch::ApplyPobWindowOpacity, &ClampOpacityThunk);
					lookSlider("##bgbright", S.bgBrightLabel, S.bgBrightHint, nullptr, &bgBrightEdit, look.bgBright, 0,
					           kBgBrightDefault, &PobLaunch::ApplyPobBackgroundBright, &ClampPercent);
					lookSlider("##glassblur", S.glassBlurLabel, S.glassHint, nullptr, &glassEdit, look.glassBlur, 0, 0,
					           &PobLaunch::ApplyPobGlassBlur, &ClampPercent);
					lookSlider("##treebg", S.treeBgLabel, S.treeBgHint, S.bgHint, &treeBgEdit, look.treeBg, 0, 100,
					           &PobLaunch::ApplyPobTreeBackdrop, &ClampPercent);
				}
				PobUi::CardEnd();
				ImGui::EndGroup();

				// preview: a sketch of the POB window under the current settings
				ImGui::SetCursorScreenPos(ImVec2(colTop.x + leftW + gapCol, colTop.y));
				PobUi::CardBegin("##lookpreview", nullptr, nullptr, nullptr, true, previewW);
				{
					PobUi::Overline(S.lookPreview);
					ImGui::Dummy(ImVec2(0, D(4.0f)));
					const float pw = PobUi::CardInnerWidth(), ph = std::floor(D(240.0f));
					const ImVec2 a = ImGui::GetCursorScreenPos(), b = a + ImVec2(pw, ph);
					ImGui::Dummy(ImVec2(pw, ph));
					ImDrawList* pdl = ImGui::GetWindowDrawList();
					const BgThumb* cur = nullptr;
					for (const BgThumb& t : bgThumbs) if (t.file == look.background) cur = &t;
					if (cur && cur->tex) {
						const float ar = (float)cur->w / (float)std::max(1, cur->h), tr = pw / ph;
						ImVec2 uv0(0, 0), uv1(1, 1);
						if (ar > tr) { const float k = tr / ar; uv0.x = (1 - k) * 0.5f; uv1.x = 1 - uv0.x; }
						else { const float k = ar / tr; uv0.y = (1 - k) * 0.5f; uv1.y = 1 - uv0.y; }
						pdl->AddImageRounded((ImTextureID)(intptr_t)cur->tex, a, b, uv0, uv1, IM_COL32_WHITE, D(5.0f));
					} else {
						pdl->AddRectFilled(a, b, look.background.empty() ? Tok::Canvas : Tok::AccentSoft, D(5.0f));
					}
					// brightness: the image is pressed down towards the ground colour
					const int dim = (int)(255.0f * (1.0f - opacityEdit * 0.0f) * (1.0f - bgBrightEdit / 100.0f));
					pdl->AddRectFilled(a, b, (Tok::Bg & 0x00FFFFFFu) | ((ImU32)std::clamp(dim, 0, 255) << 24), D(5.0f));
					// frost: approximated by lifting toward the panel colour
					pdl->AddRectFilled(a, b, (Tok::SurfaceRaised & 0x00FFFFFFu) | ((ImU32)(glassEdit * 0.9f) << 24), D(5.0f));
					// panels at the chosen opacity
					const ImU32 panel = (Tok::SurfaceRaised & 0x00FFFFFFu) | ((ImU32)(opacityEdit * 2.55f) << 24);
					const float topH = D(28.0f), sideW = D(96.0f);
					pdl->AddRectFilled(a, ImVec2(b.x, a.y + topH), panel, D(5.0f), ImDrawFlags_RoundCornersTop);
					pdl->AddRectFilled(ImVec2(a.x, a.y + topH), ImVec2(a.x + sideW, b.y), panel, D(5.0f), ImDrawFlags_RoundCornersBottomLeft);
					const ImU32 rule = (Tok::Border & 0x00FFFFFFu) | ((ImU32)(opacityEdit * 2.55f) << 24);
					pdl->AddLine(ImVec2(a.x, a.y + topH), ImVec2(b.x, a.y + topH), rule);
					pdl->AddLine(ImVec2(a.x + sideW, a.y + topH), ImVec2(a.x + sideW, b.y), rule);
					// a few tree nodes, as strong as the tree backdrop setting
					const ImU32 tAlpha = (ImU32)(80 + treeBgEdit * 1.75f) << 24;
					const ImVec2 n1 = a + ImVec2(pw * 0.48f, ph * 0.38f), n2 = a + ImVec2(pw * 0.62f, ph * 0.55f), n3 = a + ImVec2(pw * 0.76f, ph * 0.45f);
					pdl->AddLine(n1, n2, (Tok::TreeLinkOn & 0x00FFFFFFu) | tAlpha, 2.0f);
					pdl->AddLine(n2, n3, (Tok::TreeLink & 0x00FFFFFFu) | tAlpha, 2.0f);
					pdl->AddCircleFilled(n1, D(6.0f), (Tok::TreeNotable & 0x00FFFFFFu) | tAlpha, 16);
					pdl->AddCircleFilled(n2, D(4.0f), (Tok::AccentText & 0x00FFFFFFu) | tAlpha, 12);
					pdl->AddCircleFilled(n3, D(4.0f), (Tok::AccentText & 0x00FFFFFFu) | tAlpha, 12);
					pdl->AddRect(a, b, Tok::Border, D(5.0f));
					ImGui::Dummy(ImVec2(0, D(4.0f)));
					PobUi::Hint(S.lookPreviewNote, pw);
				}
				PobUi::CardEnd();
			}
			ImGui::EndChild();
			ImGui::EndTabItem();
			pageTabStyle(true);
		}

		// ---- about ------------------------------------------------------------
		if (pagesOk && pageTab(S.about, 4, pageFlags[4])) {
			pageTabStyle(false);
			ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(D(4.0f), D(24.0f)));
			ImGui::BeginChild("##aboutbody", ImVec2(0, 0), false, ImGuiWindowFlags_AlwaysUseWindowPadding);
			ImGui::PopStyleVar();
			const float inner = std::min(ImGui::GetContentRegionAvail().x - D(4.0f), D(720.0f));
			{
				const float logo = std::floor(D(72.0f));
				const ImVec2 p = ImGui::GetCursorScreenPos();
				ImDrawList* adl = ImGui::GetWindowDrawList();
				DrawLogo(adl, p, logo, fonts.icons);
				const float big = std::floor(D(28.0f));
				const float ty = p.y + std::floor((logo - big - fonts.small->FontSize - D(4.0f)) * 0.5f);
				adl->AddText(fonts.title, big, ImVec2(p.x + logo + D(20.0f), ty), Tok::Text, "PobTools");
				adl->AddText(fonts.small, fonts.small->FontSize, ImVec2(p.x + logo + D(20.0f), ty + big + D(4.0f)), Tok::TextMuted, S.aboutTagline);
				ImGui::Dummy(ImVec2(inner, logo));
				ImGui::Dummy(ImVec2(0, D(16.0f)));
			}
			// version card: what to paste into a bug report
			const std::string appLine = std::string("v" POBTOOLS_VERSION_STRING " \xc2\xb7 ") + [&]() {
				char b[64];
				snprintf(b, sizeof(b), S.aboutBuiltOn, BuildDateIso().c_str());
				return std::string(b);
			}();
			const std::string dataLine = ust.localDataVer.empty() ? std::string(S.transDataUnstamped) : ust.localDataVer;
			const std::string pobLine = std::string("PoE1 ") + (installs.poe1Lua.empty() ? S.missing : (installs.poe1Version.empty() ? S.detected : ("v" + installs.poe1Version).c_str())) +
			                            " \xc2\xb7 PoE2 " + (installs.poe2Lua.empty() ? S.missing : (installs.poe2Version.empty() ? S.detected : ("v" + installs.poe2Version).c_str()));
			PobUi::CardBegin("##aboutcard", nullptr, nullptr, nullptr, false, inner);
			auto valueRow = [&](const char* label, const std::string& v) {
				const float vw = fonts.small->CalcTextSizeA(fonts.small->FontSize, FLT_MAX, 0.0f, v.c_str()).x;
				PobUi::RowBegin(label, nullptr, vw);
				const ImVec2 c = ImGui::GetCursorScreenPos();
				ImGui::Dummy(ImVec2(vw, PobUi::ControlH()));
				ImGui::GetWindowDrawList()->AddText(fonts.small, fonts.small->FontSize,
				    ImVec2(c.x, c.y + std::floor((PobUi::ControlH() - fonts.small->FontSize) * 0.5f)), Tok::Text, v.c_str());
				PobUi::RowEnd();
			};
			valueRow(S.aboutAppVersion, appLine);
			valueRow(S.sectionTransData, dataLine);
			valueRow(S.aboutDetectedPob, pobLine);
			{
				PobUi::RowBegin(S.aboutReport, S.aboutReportHint, PobUi::ButtonWidth(S.copyVersionInfo, PobUi::BtnSize::Sm, PobIcon::Copy));
				if (PobUi::Button(S.copyVersionInfo, PobUi::BtnKind::Secondary, PobUi::BtnSize::Sm, PobIcon::Copy)) {
					const std::string info = std::string("PobTools ") + appLine + "\n" + S.sectionTransData + ": " + dataLine +
					                         "\nPOB: " + pobLine + "\n" + (PobLaunch::RunningUnderWine() ? "Wine\n" : "");
					ImGui::SetClipboardText(info.c_str());
					PobUi::ShowToast(S.copiedToast);
				}
				PobUi::RowEnd();
			}
			PobUi::CardEnd();
			ImGui::Dummy(ImVec2(0, D(16.0f)));
			if (PobUi::Button(S.discord, PobUi::BtnKind::Secondary, PobUi::BtnSize::Md, PobIcon::MessageCircle))
				ShellExecuteW(nullptr, L"open", L"https://discord.gg/6VamPQb8nC", nullptr, nullptr, SW_SHOWNORMAL);
			ImGui::SameLine(0, D(12.0f));
			if (PobUi::Button("GitHub", PobUi::BtnKind::Secondary, PobUi::BtnSize::Md, PobIcon::ExternalLink))
				ShellExecuteW(nullptr, L"open", L"https://github.com/Hsiung-Shao/PobTools-zh", nullptr, nullptr, SW_SHOWNORMAL);
			ImGui::SameLine(0, D(12.0f));
			if (PobUi::Button(S.support, PobUi::BtnKind::Primary, PobUi::BtnSize::Md, PobIcon::Coffee))
				ShellExecuteW(nullptr, L"open", L"https://hsiung-shao.github.io/support/", nullptr, nullptr, SW_SHOWNORMAL);
			ImGui::Dummy(ImVec2(0, D(16.0f)));
			// The attribution lines of aboutBody, minus its first line (the product
			// line, which the heading above already says).
			{
				const std::string body = S.aboutBody;
				size_t start = body.find('\n');
				start = start == std::string::npos ? body.size() : start + 1;
				while (start < body.size()) {
					size_t nl = body.find('\n', start);
					const std::string line = body.substr(start, (nl == std::string::npos ? body.size() : nl) - start);
					PobUi::Hint(line.c_str(), inner);
					ImGui::Dummy(ImVec2(0, D(2.0f)));
					if (nl == std::string::npos) break;
					start = nl + 1;
				}
			}
			ImGui::EndChild();
			ImGui::EndTabItem();
			pageTabStyle(true);
		}
		pageTabStyle(false);
		if (pagesOk) ImGui::EndTabBar();
		if (pageNow >= 0) pageShown = pageNow;
		if (tabbed) ImGui::EndChild();
		ImGui::PopStyleColor();
		} // launcherOpen
		if (tabbed && launcherOpen) ImGui::EndTabItem();

		// --- embedded tools ---------------------------------------------------
		// Drawn straight into this frame, unlike the docked tabs below: these are
		// our own ImGui code, so there is no second window and nothing to keep glued
		// to anything. The style is swapped for the panel's own density and put back
		// afterwards -- safe because the style-var stack is empty here (see
		// PobUi::BuildStyle).
		auto docTabColors = [&](bool push) {
			if (push) {
				ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
				                    ImVec2(D(14.0f), std::floor((std::floor(D(32.0f)) - ImGui::GetFontSize()) * 0.5f)));
				ImGui::PushStyleColor(ImGuiCol_Tab, PobUi::TokV4(Tok::Surface1));
				ImGui::PushStyleColor(ImGuiCol_TabHovered, PobUi::TokV4(Tok::Surface3));
				ImGui::PushStyleColor(ImGuiCol_TabActive, PobUi::TokV4(Tok::Surface2));
				ImGui::PushStyleColor(ImGuiCol_TabUnfocused, PobUi::TokV4(Tok::Surface1));
				ImGui::PushStyleColor(ImGuiCol_TabUnfocusedActive, PobUi::TokV4(Tok::Surface2));
			} else {
				ImGui::PopStyleColor(5);
				ImGui::PopStyleVar();
			}
		};
		for (size_t i = 0; tabbed && tabsOk && i < panels.size(); i++) {
			EmbeddedPanel& ep = panels[i];
			// The tool's icon in front of its name, so a tool tab never reads as a
			// POB one (the label's icon glyph is part of the body font).
			const char* icon = nullptr;
			if (ep.label == S.filterEditor) icon = PobIcon::Funnel;
			else if (ep.label == S.atlasPlanner) icon = PobIcon::Crosshair;
			else if (ep.label == S.timelessJewel) icon = PobIcon::Gem;
			else if (ep.label == S.regexTool) icon = PobIcon::CodeXml;
			else if (ep.label == S.warehouseTool) icon = PobIcon::ChartColumn;
			else if (ep.label == S.editor) icon = PobIcon::Languages;
			std::string label = ((icon && fonts.icons) ? std::string(icon) + " " : std::string()) + ep.label +
			                    "###panel" + ep.panel->PanelId();
			bool open = true;
			docTabColors(true);
			const bool vis = ImGui::BeginTabItem(label.c_str(), &open, ep.forceSelect);
			docTabColors(false);
			if (vis) {
				ep.forceSelect = 0;
				const ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
				dl->AddRectFilled(mn, ImVec2(mx.x, mn.y + 2.0f), Tok::Accent);
				// A panel drawn here means no docked window is on top of the client
				// area, so activeDockTab stays -1 and Dock::Update hides them all.
				ImGui::PushID(ep.panel->PanelId());
				const ImGuiStyle keep = ImGui::GetStyle();
				ImGui::GetStyle() = styleFor(ep.panel->Density());
				ImGui::PushFont(fonts.body);
				ep.panel->Frame();
				ImGui::PopFont();
				ImGui::GetStyle() = keep;
				ImGui::PopID();
				ImGui::EndTabItem();
			}
			if (!open) ep.panel->RequestClose();
		}

		// --- docked windows ---------------------------------------------------
		// One tab per POB / tool window, after the fixed ones. Their bodies are
		// deliberately empty: the real window sits exactly over this area. A POB tab
		// carries a P1 / P2 badge, drawn over spaces reserved in the label.
		if (tabbed && tabsOk) {
			const std::vector<WindowDock::Tab>& dtabs = dock.Tabs();
			const std::vector<PobLaunch::InstanceInfo> inst = PobLaunch::RunningInstances();
			const float spaceW = fonts.body->CalcTextSizeA(fonts.body->FontSize, FLT_MAX, 0.0f, " ").x;
			const float badge = std::floor(D(20.0f));
			const int nSpaces = spaceW > 0.0f ? (int)std::ceil((badge + D(6.0f)) / spaceW) : 4;
			for (size_t i = 0; i < dtabs.size(); i++) {
				const char* kindBadge = nullptr;
				for (const PobLaunch::InstanceInfo& in : inst)
					if (in.pid == dtabs[i].pid && in.kind == PobLaunch::InstanceKind::Pob)
						kindBadge = in.game == L"poe2" ? "P2" : "P1";
				// The label alone is not unique -- two PoE1 tabs are ordinary -- and it
				// follows POB's caption, so it must not be part of the id at all.
				const std::string text = (kindBadge ? std::string((size_t)nSpaces, ' ') : std::string()) + to_utf8(dtabs[i].label);
				std::string label = WindowMgr::DockTabLabel(text, dtabs[i].pid);
				bool open = true;
				// Newly started windows bring themselves to the front.
				const ImGuiTabItemFlags focus = dock.TakeFocusRequest(i) ? ImGuiTabItemFlags_SetSelected : 0;
				docTabColors(true);
				const bool vis = ImGui::BeginTabItem(label.c_str(), &open, focus);
				docTabColors(false);
				const ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
				if (kindBadge) {
					const ImVec2 bp(mn.x + ImGui::GetStyle().FramePadding.x, std::floor((mn.y + mx.y - badge) * 0.5f));
					dl->AddRectFilled(bp, bp + ImVec2(badge, badge), Tok::AccentSoft, D(4.0f));
					const float fs = fonts.small->FontSize * 0.8f;
					const ImVec2 ts = fonts.small->CalcTextSizeA(fs, FLT_MAX, 0.0f, kindBadge);
					dl->AddText(fonts.small, fs, bp + ImVec2(std::floor((badge - ts.x) * 0.5f), std::floor((badge - ts.y) * 0.5f)),
					            Tok::AccentText, kindBadge);
				}
				if (vis) {
					dl->AddRectFilled(mn, ImVec2(mx.x, mn.y + 2.0f), Tok::Accent);
					activeDockTab = (int)i;
					ImGui::EndTabItem();
				}
				// The tab's own close button: asks the window to close (POB gets to
				// prompt about unsaved work) rather than killing it.
				if (!open) closeDockTab = (int)i;
			}
		}

		if (tabbed && tabsOk) ImGui::EndTabBar();

		ImGui::End();
		PobUi::DrawToast();

		// Docked windows: reap, adopt, position, z-order. After ImGui::End so the
		// strip height measured above is the one actually laid out this frame.
		if (tabbed) {
			if (closeDockTab >= 0) dock.RequestClose((size_t)closeDockTab);
			// While shutting down, show whichever tab is being closed, so a "save
			// your build?" prompt is on screen rather than behind another tab.
			const int shown = (closingTabs && !dock.Empty())
			                ? (int)dock.Tabs().size() - 1 : activeDockTab;
			dock.Update(stripH, shown);
		}

		// A tool that refused to open. Here for the same reason a panel's dialogs
		// are: the frame is over and the docked windows have been dealt with.
		if (!panelInitError.empty()) {
			const std::wstring why = from_utf8(panelInitError);
			panelInitError.clear();
			MessageBoxW(glfwGetWin32Window(win), why.c_str(), L"PobTools", MB_ICONERROR | MB_OK);
		}

		// Embedded panels: deferred work, then reap the ones that are done.
		//
		// AFTER dock.Update on purpose. A panel's deferred work is where its Win32
		// dialogs open, and Dock::Update is what hides the docked POB windows when a
		// panel tab is selected -- doing it the other way round could put a modal
		// dialog underneath a window that had not been hidden yet.
		for (size_t i = 0; i < panels.size();) {
			EmbeddedPanel& ep = panels[i];
			ep.panel->RunDeferred();

			// The translation editor can be editing Data\launcher\<locale>\ -- the
			// very strings this window is drawing with. A save has to be noticed
			// here: the tables are reloaded and the atlas rebuilt (a translator can
			// type a character that was not in it).
			if (TranslationEditorPanelSaved(ep.panel.get())) {
				strStore.clear();
				for (const LocaleInfo& l : locales)
					strStore.emplace_back(LoadLauncherStrings(launcherRoot, from_utf8(l.id)));
				strOverlays.clear();
				for (const LauncherStringStore& st : strStore) strOverlays.push_back(&st.s);
				fontChanged = true;
			}

			const ToolCloseState cs = ep.panel->CloseState();
			if (cs == ToolCloseState::Asking) {
				// Its prompt has to be visible to be answerable.
				ep.forceSelect = ImGuiTabItemFlags_SetSelected;
				i++;
			} else if (cs == ToolCloseState::Closed) {
				// Not while the others are still being asked: a panel that agreed
				// must stay reversible until every panel has answered (AbortClose).
				if (closingPanels) { i++; continue; }
				// While the GL context is still current: panels may hold textures.
				ep.panel->Shutdown();
				panels.erase(panels.begin() + (ptrdiff_t)i);
			} else {
				i++;
			}
		}

		// Overwrite confirmation for the copy button. Opened at this level (outside
		// the tab's child window) so the popup's ID stack does not depend on which
		// tab happens to be drawn. The only dialog here that destroys work, so its
		// action is the danger button.
		{
			const PobUi::DialogResult r = PobUi::ConfirmDialog("##copyconfirm", &askOverwrite, S.copyOverwriteTitle,
			    S.copyOverwrite, to_utf8(copyDest).c_str(), S.cancel, S.overwriteConfirm, nullptr);
			if (r == PobUi::DialogResult::Danger) doCopy = true;
			else if (r == PobUi::DialogResult::Cancel) copyDest.clear();
		}
		if (doCopy) {
			doCopy = false;
			std::string cerr;
			int n = CopyBuiltinDictionary(exeDir, (DictSlot)copySlot, copyDest, &cerr);
			if (n < 0) {
				PobUi::ShowToast(cerr.c_str(), PobUi::Tone::Bad);
			} else {
				PobUi::ShowToast((std::string(S.copyDone) + std::to_string(n) + S.copyDoneSuffix).c_str());
				// Point at what was just created: copying and then having to browse
				// to the same folder by hand would be a pointless second step.
				cfg.dataDir[copySlot] = copyDest;
				resolveDict(copySlot);
				SaveLauncherConfigKeepModern(exeDir + L"pob-zh.ini", cfg);
			}
			copyDest.clear();
		}

		ImGui::PopFont();
		ImGui::Render();

		// Present only when something changed (frame_pacing.h). `busy` lists
		// every reason this loop has for wanting the fast cadence without any
		// input: a worker or a timer that will change the picture on its own,
		// a closing sequence that answers over frames, and the docked POB
		// windows in tabbed mode, which are positioned from this loop.
		FramePacing::Inputs pace;
		pace.now = glfwGetTime();
		pace.iconified = glfwGetWindowAttrib(win, GLFW_ICONIFIED) != 0;
		pace.activity = FramePacing::ImGuiActivity();
		pace.busy = fontWorker.Running() || pendingScale > 0.0f || fontChanged ||
		            closingTabs || closingPanels || sizeDirty || updaterBusy ||
		            ust.phase == AppUpdatePhase::Checking ||
		            ust.phase == AppUpdatePhase::TransUpdating ||
		            PobUi::ToastVisible() || relaunchRequested ||
		            std::any_of(bgThumbs.begin(), bgThumbs.end(), [](const BgThumb& t) { return t.job != nullptr; }) ||
		            (tabbed && !dock.Empty());
		pace.forceRender = g_launcherRedraw || !shotPath.empty();
		g_launcherRedraw = false;
		const bool present = pacer.ShouldRender(pace, ImGui::GetDrawData());
		if (present) {
			int fbW = 0, fbH = 0;
			glfwGetFramebufferSize(win, &fbW, &fbH);
			glViewport(0, 0, fbW, fbH);
			glClearColor(0.043f, 0.063f, 0.078f, 1.0f);
			glClear(GL_COLOR_BUFFER_BIT);
			ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
			// Test aid (POBTOOLS_LAUNCHER_SHOT): read back this frame once the full
			// atlas is in and the page has settled, write it out and close. The
			// window is never shown, so nothing appears on anyone's screen.
			if (!shotPath.empty() && (fontInputFullDone || !fontWorker.Running()) && glfwGetTime() - shotSince > 2.5) {
				WriteFramebufferBmp(shotPath, fbW, fbH);
				shotPath.clear();
				glfwSetWindowShouldClose(win, GLFW_TRUE);
			}
			glfwSwapBuffers(win);
		}
		// First frame is in the swap chain: now the window can appear with content
		// already on it (or, past kFirstShowLimit, without). Shot mode stays hidden.
		if (!shotMode && firstShow.Due(present, glfwGetTime())) {
			glfwShowWindow(win);
			startup_trace_mark(present ? "first frame presented, window shown"
			                           : "no frame presented yet, window shown anyway");
		}
		nextWait = pacer.WaitSeconds(glfwGetTime());
	}

	syncCfgFromUi(); // host_main saves cfg after this returns

	// Hand every docked window its frame back before this window goes away, or
	// they would be left frameless and unmovable on the desktop.
	if (tabbed) {
		dock.RestoreAll();
		g_launcherDock = nullptr; // the callbacks are about to be destroyed with the window
	}

	// Embedded panels, while the GL context is STILL CURRENT: they hold textures
	// and worker threads, and deleting a texture after the context is gone is at
	// best ignored and at worst a crash. Deliberately before the teardown below and
	// not in a destructor, where the ordering would depend on declaration order.
	//
	// Not asked whether they want to close: by this point the launcher is going
	// regardless, and any panel with unsaved work has already had its say through
	// the close sequence.
	for (EmbeddedPanel& ep : panels) ep.panel->Shutdown();
	panels.clear();
	// Same reason: thumbnail textures go while the context is current.
	for (BgThumb& t : bgThumbs) {
		if (t.job) t.job->wait();
		if (t.tex) DeleteTexture(t.tex);
	}
	bgThumbs.clear();

	// The font worker allocates through ImGui, which counts on the current
	// context: it has to be gone before the context is.
	fontWorker.Discard();

	// Full teardown so the next round (return-to-launcher) re-inits cleanly.
	ImGui_ImplOpenGL3_Shutdown();
	ImGui_ImplGlfw_Shutdown();
	ImGui::DestroyContext();
	glfwDestroyWindow(win);
	glfwTerminate();

	if (openEditor) return LauncherResult::OpenEditor;
	if (applyUpdate) return LauncherResult::ApplyAppUpdate;
	if (relaunchRequested && !launch) return LauncherResult::Relaunch;
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
// Does the atlas the launcher actually builds fit on the GPU, and does it carry
// the characters tab titles need?
//
// Separate from --font-coverage-selftest, which asks a different question ("can
// this font file draw the strings we ship") and never calls LoadFonts. This one
// drives the real function across the {font} x {DPI scale} x {GL_MAX_TEXTURE_SIZE}
// grid, because the failure it is looking for depends on all three and appears on
// none of them alone.
int RunFontAtlasSelftest(const std::wstring& exeDir)
{
	std::string report;
	int failures = 0, checks = 0;
	auto check = [&](const std::string& name, bool ok, const std::string& detail = "") {
		checks++;
		report += std::string(ok ? "PASS " : "FAIL ") + name +
		          (detail.empty() ? "" : "  (" + detail + ")") + "\n";
		if (!ok) failures++;
	};

	LauncherConfig cfg = LoadLauncherConfig(exeDir + L"pob-zh.ini");
	const std::wstring launcherRoot = ResolveDictDir(exeDir, DictSlot::Launcher,
	                                                 cfg.dataDir[(int)DictSlot::Launcher]).root;
	std::vector<LocaleInfo> locales = ListInstalledLocales(exeDir, cfg);
	std::vector<LauncherStringStore> strStore;
	strStore.reserve(locales.size());
	for (const LocaleInfo& l : locales)
		strStore.emplace_back(LoadLauncherStrings(launcherRoot, std::wstring(l.id.begin(), l.id.end())));
	std::vector<const LauncherStrings*> overlays;
	overlays.reserve(strStore.size());
	for (const LauncherStringStore& st : strStore) overlays.push_back(&st.s);

	std::vector<std::wstring> fontList = ListAvailableFonts(exeDir);
	if (fontList.empty()) {
		report += "FAIL no fonts under Fonts\\\nRESULT FAIL\n";
		failures++;
		fontList.clear();
	}

	// Characters a build name can contain that appear in NO launcher string. Without
	// these the check is vacuous: every character of the precise set is present by
	// construction, so a body face that lost the full CJK block would still pass.
	//
	// Written as UTF-8 and decoded, never as hand-typed codepoints: the first version
	// of this list had 贖 as 0x8CFF (it is 0x8D16), and the check went green or red
	// depending on whether a font happened to have a glyph at the wrong address.
	const char* kProbeText = u8"贖燃點罪鮫龜";
	std::vector<unsigned> probeCps;
	ForEachCodepoint(kProbeText, [&](unsigned cp) { probeCps.push_back(cp); });

	// The last scale is the largest font-size setting on a 200% monitor
	// (26/19 zoom): the ladder has to end in an uploadable atlas there too, even
	// if what it uploads is degraded.
	const float kScales[] = { 1.0f, 1.25f, 1.5f, 2.0f,
	                          2.0f * (float)kLauncherFontSizeMax / (float)kLauncherFontSizeDefault };
	const int   kLimits[] = { 2048, 4096, 8192, 16384 };

	for (const std::wstring& f : fontList) {
		const std::string fname = to_utf8(f);
		for (float sc : kScales) {
			for (int lim : kLimits) {
				ImGui::CreateContext();
				std::shared_ptr<const FontBuildInput> in =
				    PrepareFontInput(ResolveFontPath(exeDir, f), {}, overlays, sc, lim,
				                     FallbackFontPaths(exeDir, f));
				LauncherFonts fonts = LoadFonts(ImGui::GetIO().Fonts, in, FontScope::Full);

				char head[192];
				snprintf(head, sizeof(head), "%s @%.2fx max=%d", fname.c_str(), sc, lim);
				char detail[192];
				snprintf(detail, sizeof(detail), "%dx%d%s%s", fonts.texW, fonts.texH,
				         fonts.dropped.empty() ? "" : " dropped=", fonts.dropped.c_str());

				// The whole point of the guard: whatever it decides to build, the result
				// must be uploadable. An atlas over the limit is not a crash, it is a
				// window that draws nothing and says nothing.
				check(std::string(head) + " -- atlas fits the GPU limit",
				      fonts.texW > 0 && fonts.texW <= lim && fonts.texH <= lim, detail);

				// And it must never give up more than it had to.
				if (fonts.dropped.empty()) {
					int missing = 0;
					for (unsigned cp : probeCps)
						if (cp <= 0xFFFF &&
						    (!fonts.body || !fonts.body->FindGlyphNoFallback((ImWchar)cp)))
							missing++;
					check(std::string(head) + " -- build-name characters are in the atlas",
					      missing == 0,
					      std::to_string((int)probeCps.size() - missing) + "/" +
					          std::to_string((int)probeCps.size()) + " present");
				} else {
					report += "note " + std::string(head) + " -- degraded, dropped=" +
					          fonts.dropped + " (" + detail + ")\n";
				}
				ImGui::DestroyContext();
			}
		}
	}

	// What THIS machine will actually do. The grid above is hypothetical; a real
	// context is the only way to learn the driver's limit, and the limit is what
	// decides whether the shipping configuration degrades in front of the user.
	//
	// A hidden window, and the GL context torn down again straight away -- same
	// pattern as RunPassiveTreeRender. Never activated: taking focus in a headless
	// check is how phantom clicks happen.
	{
		int realMax = 0;
		float realScale = 1.0f;
		if (glfwInit()) {
			glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_ES_API);
			glfwWindowHint(GLFW_CONTEXT_CREATION_API, GLFW_EGL_CONTEXT_API);
			glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
			glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
			glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
			if (GLFWwindow* w = glfwCreateWindow(64, 64, "font-atlas-probe", nullptr, nullptr)) {
				glfwMakeContextCurrent(w);
				GLint m = 0;
				glGetIntegerv(GL_MAX_TEXTURE_SIZE, &m);
				realMax = (int)m;
				if (GLFWmonitor* mon = glfwGetPrimaryMonitor()) {
					float sx = 1.0f, sy = 1.0f;
					glfwGetMonitorContentScale(mon, &sx, &sy);
					if (sx > 0.0f) realScale = sx;
				}
				glfwDestroyWindow(w);
			}
			glfwTerminate();
		}
		char d[128];
		snprintf(d, sizeof(d), "GL_MAX_TEXTURE_SIZE=%d, monitor scale=%.2fx", realMax, realScale);
		check("this machine reports a usable texture limit", realMax >= 2048, d);

		if (realMax >= 2048) {
			ImGui::CreateContext();
			std::shared_ptr<const FontBuildInput> in =
			    PrepareFontInput(ResolveFontPath(exeDir, cfg.fontFile), {}, overlays, realScale, realMax,
			                 FallbackFontPaths(exeDir, cfg.fontFile));
			LauncherFonts fonts = LoadFonts(ImGui::GetIO().Fonts, in, FontScope::Full);
			int miss = 0;
			for (unsigned cp : probeCps)
				if (cp <= 0xFFFF && (!fonts.body || !fonts.body->FindGlyphNoFallback((ImWchar)cp)))
					miss++;
			char d2[192];
			snprintf(d2, sizeof(d2), "%dx%d on a %d limit at %.2fx%s%s", fonts.texW, fonts.texH,
			         realMax, realScale, fonts.dropped.empty() ? "" : ", dropped=",
			         fonts.dropped.c_str());
			check("the shipping configuration on THIS machine keeps the full CJK block",
			      fonts.dropped != "cjk" && miss == 0, d2);
			ImGui::DestroyContext();

			// Informational only: what the largest font-size setting would do here.
			// Not a check, because a developer who runs at 26 px must not turn the
			// release gate red on a GPU where the shipping default is fine.
			{
				ImGui::CreateContext();
				const float zoomed = realScale * LauncherZoom(kLauncherFontSizeMax);
				std::shared_ptr<const FontBuildInput> inZ =
				    PrepareFontInput(ResolveFontPath(exeDir, cfg.fontFile), {}, overlays, zoomed, realMax,
				                     FallbackFontPaths(exeDir, cfg.fontFile));
				LauncherFonts fz = LoadFonts(ImGui::GetIO().Fonts, inZ, FontScope::Full);
				char d3[192];
				snprintf(d3, sizeof(d3), "note at the max font size (%d px, %.2fx): %dx%d%s%s\n",
				         kLauncherFontSizeMax, zoomed, fz.texW, fz.texH,
				         fz.dropped.empty() ? "" : ", dropped=", fz.dropped.c_str());
				report += d3;
				ImGui::DestroyContext();
			}
		}
	}

	// The configuration that actually ships has to be the undegraded one somewhere,
	// or the guard is just quietly disabling the feature on every machine.
	{
		ImGui::CreateContext();
		std::shared_ptr<const FontBuildInput> in =
		    PrepareFontInput(ResolveFontPath(exeDir, cfg.fontFile), {}, overlays, 1.0f, 4096,
		                 FallbackFontPaths(exeDir, cfg.fontFile));
		LauncherFonts fonts = LoadFonts(ImGui::GetIO().Fonts, in, FontScope::Full);
		int missing = 0;
		for (unsigned cp : probeCps)
			if (cp <= 0xFFFF && (!fonts.body || !fonts.body->FindGlyphNoFallback((ImWchar)cp)))
				missing++;
		check("the shipped font at 100% on a 4096 GPU keeps the full CJK block",
		      fonts.dropped != "cjk" && missing == 0,
		      (fonts.dropped.empty() ? "nothing dropped" : ("dropped=" + fonts.dropped)) +
		          ", " + std::to_string(missing) + " probe glyph(s) missing");
		// The Lucide subset is merged into every face the widgets draw with. All of
		// its glyphs, not one probe: a range that stops short would drop the icons
		// past it silently.
		{
			int iconMissing = 0, iconTotal = 0;
			for (ImFont* face : { fonts.body, fonts.small, fonts.heading, fonts.title })
				for (unsigned cp = PobIcon::kRangeFirst; cp <= PobIcon::kRangeLast; cp++) {
					if (!face) { iconMissing++; continue; }
					// only codepoints the subset actually has (the range has gaps)
					if (!fonts.body->FindGlyphNoFallback((ImWchar)cp)) continue;
					iconTotal++;
					if (!face->FindGlyphNoFallback((ImWchar)cp)) iconMissing++;
				}
			check("the icon font is merged into body / small / heading / title",
			      fonts.icons && iconMissing == 0 && iconTotal == PobIcon::kCount * 4,
			      std::to_string(iconTotal) + " icon glyphs over 4 faces, " +
			          std::to_string(iconMissing) + " missing (subset has " +
			          std::to_string(PobIcon::kCount) + ")");
			// And the icons do not pretend to be text: a PUA codepoint is never part
			// of a launcher string, so the coverage check never asks a text font
			// for one.
			bool puaInStrings = false;
			std::vector<const char*> texts;
			CollectLauncherTexts(texts, overlays);
			for (const char* t : texts)
				ForEachCodepoint(t, [&](unsigned cp) { if (cp >= 0xE000 && cp <= 0xF8FF) puaInStrings = true; });
			check("no launcher string carries an icon codepoint", !puaInStrings);
		}
		ImGui::DestroyContext();
	}

	// The startup path: a Precise atlas on the main thread plus a Full one from the
	// worker, the way ShowLauncher does it. The precise one must already draw every
	// launcher string (the first frame is drawn with it) and must not claim to have
	// degraded; the worker's must be the same atlas ShowLauncher got before, or the
	// swap would silently change what the window shows.
	{
		ImGui::CreateContext();
		std::shared_ptr<const FontBuildInput> in =
		    PrepareFontInput(ResolveFontPath(exeDir, cfg.fontFile), {}, overlays, 1.0f, 4096,
		                 FallbackFontPaths(exeDir, cfg.fontFile));
		FontAtlasWorker worker;
		worker.Start(in);
		LauncherFonts precise = LoadFonts(ImGui::GetIO().Fonts, in, FontScope::Precise);
		int preciseMissing = 0;
		for (const LauncherStrings* s : overlays) {
			for (auto m : kLauncherStringMembers) {
				const char* t = s->*m;
				if (!t) continue;
				ForEachCodepoint(t, [&](unsigned cp) {
					if (cp <= 0xFFFF && cp >= 0x20 && !precise.body->FindGlyphNoFallback((ImWchar)cp))
						preciseMissing++;
				});
			}
		}
		check("the precise (first-frame) atlas draws every launcher string",
		      preciseMissing == 0 && precise.cjkOk,
		      std::to_string(preciseMissing) + " string-table glyph(s) missing, " +
		          std::to_string(precise.texW) + "x" + std::to_string(precise.texH));
		check("the precise atlas does not report a degraded build", precise.dropped.empty(),
		      precise.dropped.empty() ? "dropped is empty" : "dropped=" + precise.dropped);
		check("the precise (first-frame) atlas already has the icons", precise.icons);
		check("the precise atlas is a small fraction of the full one",
		      precise.texW * precise.texH < 4096 * 1200,
		      std::to_string(precise.texW) + "x" + std::to_string(precise.texH));
		LauncherFonts full;
		ImFontAtlas* fullAtlas = worker.Take(&full);
		int fullMissing = 0;
		for (unsigned cp : probeCps)
			if (cp <= 0xFFFF && (!full.body || !full.body->FindGlyphNoFallback((ImWchar)cp)))
				fullMissing++;
		check("the worker-built full atlas keeps the full CJK block",
		      fullAtlas != nullptr && full.dropped != "cjk" && fullMissing == 0 &&
		          full.texW > 0 && full.texW <= 4096 && full.texH <= 4096,
		      std::to_string(full.texW) + "x" + std::to_string(full.texH) +
		          (full.dropped.empty() ? "" : " dropped=" + full.dropped));
		if (fullAtlas) IM_DELETE(fullAtlas);

		// Both builds must share one keep-alive: the atlas only stores pointers
		// into it, and a copy would move the buffers.
		check("precise and full builds share the same input buffers",
		      precise.input == full.input && precise.input == in);

		// The user picks another font while the worker is still rasterising. This
		// replays ShowLauncher's fontChanged sequence exactly: discard the
		// in-flight build, clear the live atlas, rebuild Full from a NEW input,
		// then drop the last reference to the old input -- the worker must be
		// gone by then or it would be reading freed TTF bytes.
		FontAtlasWorker abandoned;
		abandoned.Start(in);
		abandoned.Discard();
		ImGui::GetIO().Fonts->Clear();
		std::shared_ptr<const FontBuildInput> in2 =
		    PrepareFontInput(ResolveFontPath(exeDir, cfg.fontFile), {}, overlays, 1.0f, 4096,
		                 FallbackFontPaths(exeDir, cfg.fontFile));
		LauncherFonts rebuilt = LoadFonts(ImGui::GetIO().Fonts, in2, FontScope::Full);
		precise = LauncherFonts();
		full = LauncherFonts();
		in.reset();
		check("a font change during the background build rebuilds cleanly",
		      !abandoned.Running() && !abandoned.Done() && rebuilt.input == in2 &&
		          rebuilt.body && rebuilt.body->FindGlyphNoFallback((ImWchar)0x555F) != nullptr,
		      std::to_string(rebuilt.texW) + "x" + std::to_string(rebuilt.texH));

		// No TTF at all (Fonts\ folder missing): the worker must refuse to start,
		// because two threads decompressing ImGui's built-in font race on its
		// static state.
		auto noTtf = std::make_shared<FontBuildInput>();
		noTtf->ttf = std::make_shared<const std::vector<unsigned char>>();
		noTtf->maxTex = 4096;
		FontAtlasWorker refused;
		refused.Start(noTtf);
		check("the background build is not started without a TTF", !refused.Running());
		ImGui::DestroyContext();
	}

	// The ladder's first sacrifice depends on the interface language (ko-KR keeps
	// Hangul longest, everyone else keeps the CJK block longest). Drive LoadFonts
	// with the scale rising until the Korean-preferring build has to drop the CJK
	// block while still holding Hangul -- the exact case the preference exists for
	// -- and check the other preference makes the opposite choice on the same input.
	{
		// `koreanOk` probes 한, which the ko-KR launcher strings put into the PRECISE
		// set, so it stays true even after the Hangul BLOCK is dropped. The block
		// itself is probed with 힣 (U+D7A3): in GetGlyphRangesKorean, in Noto Sans
		// KR, and in no launcher string. The CJK block is probed with kProbeText,
		// same as the checks above.
		struct LadderResult { std::string dropped; bool hangulBlock, cjkBlock; int w, h; };
		auto buildWith = [&](bool preferKorean, float scale) -> LadderResult {
			ImGui::CreateContext();
			std::shared_ptr<const FontBuildInput> in =
			    PrepareFontInput(ResolveFontPath(exeDir, cfg.fontFile), {}, overlays, scale, 4096,
			                     FallbackFontPaths(exeDir, cfg.fontFile), preferKorean);
			LauncherFonts f = LoadFonts(ImGui::GetIO().Fonts, in, FontScope::Full);
			LadderResult r{ f.dropped, false, true, f.texW, f.texH };
			r.hangulBlock = f.body && f.body->FindGlyphNoFallback((ImWchar)0xD7A3) != nullptr;
			for (unsigned cp : probeCps)
				if (cp <= 0xFFFF && (!f.body || !f.body->FindGlyphNoFallback((ImWchar)cp)))
					r.cjkBlock = false;
			ImGui::DestroyContext();
			return r;
		};
		const float kLadderScales[] = { 1.0f, 1.25f, 1.5f, 1.75f, 2.0f, 2.5f, 3.0f };
		bool exercised = false;
		std::string trail;
		for (float s : kLadderScales) {
			LadderResult ko = buildWith(true, s);
			char t[96];
			snprintf(t, sizeof(t), " %.2fx:%s%s%s", s, ko.dropped.empty() ? "fits" : ko.dropped.c_str(),
			         ko.hangulBlock ? "+hangul" : "", ko.cjkBlock ? "+cjk" : "");
			trail += t;
			if (ko.dropped.empty()) continue; // everything fit: nothing was sacrificed
			check("a Korean interface never gives the Hangul block up first",
			      ko.dropped != "korean" && (ko.hangulBlock || !ko.cjkBlock),
			      "scale " + std::to_string(s) + " dropped=" + ko.dropped);
			if (ko.dropped == "cjk" && ko.hangulBlock && !ko.cjkBlock) {
				LadderResult other = buildWith(false, s);
				check("a non-Korean interface on the same input drops the Hangul block, not the CJK block",
				      other.dropped == "korean" && !other.hangulBlock && other.cjkBlock,
				      "scale " + std::to_string(s) + " dropped=" + other.dropped +
				          (other.hangulBlock ? " hangul-block kept" : "") + (other.cjkBlock ? " cjk kept" : " cjk lost"));
				exercised = true;
				break;
			}
		}
		check("the language-aware ladder was exercised (Korean kept, CJK dropped, on a 4096 limit)",
		      exercised, "trail:" + trail);
	}

	const int ran = checks;
	check("the suite actually ran", ran >= 10, std::to_string(ran) + " checks");

	report += failures ? "RESULT FAIL\n" : "RESULT PASS\n";
	CreateDirectoryW((exeDir + L"PobTools").c_str(), nullptr);
	HANDLE h = CreateFileW((exeDir + L"PobTools\\font_atlas_selftest.txt").c_str(),
	                       GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
	                       FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h != INVALID_HANDLE_VALUE) {
		DWORD w = 0;
		WriteFile(h, report.data(), (DWORD)report.size(), &w, nullptr);
		CloseHandle(h);
	}
	if (AttachConsole(ATTACH_PARENT_PROCESS)) {
		FILE* fp = nullptr;
		freopen_s(&fp, "CONOUT$", "w", stdout);
	}
	printf("%s", report.c_str());
	return failures ? 2 : 0;
}

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
		printf("font coverage: no fonts under Fonts\\ -- nothing to check\n");
		return 1;
	}
	printf("font coverage: %d distinct characters across %d font(s)\n",
	       (int)want.size(), (int)fonts.size());

	// UNION semantics: the launcher merges every shipped font into its atlas as
	// glyph fallbacks, so a character is drawable when ANY font carries it.
	// Judging each font alone made zh-rCN un-shippable -- Noto Sans TC has no
	// simplified-only glyphs and never will; FZ_ZY fills them. A per-font gap
	// is still reported (it shows which font is doing the covering), but only a
	// character missing from EVERY font fails the run.
	std::map<unsigned, int> uncovered; // cp -> fonts still missing it
	for (unsigned cp : want) if (cp <= 0xFFFF) uncovered[cp] = 0;
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
			if (!font->FindGlyphNoFallback((ImWchar)cp)) { missing.push_back(cp); uncovered[cp]++; }
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
			// A gap in ONE font is fine as long as another shipped font covers
			// it -- the launcher's merged fallback will use that one.
			printf("  [gap]  %s: %d character(s) covered by another shipped font\n",
			       name.c_str(), (int)missing.size());
		}
		ImGui::DestroyContext();
	}
	// The real gate: a character NO shipped font can draw.
	{
		std::vector<unsigned> nowhere;
		for (const auto& [cp, misses] : uncovered)
			if (misses == (int)fonts.size()) nowhere.push_back(cp);
		if (!nowhere.empty()) {
			printf("  [FAIL] %d character(s) missing from EVERY shipped font\n", (int)nowhere.size());
			for (unsigned cp : nowhere) {
				wchar_t w[2] = { (wchar_t)cp, 0 };
				printf("           U+%04X  '%s'\n", cp, to_utf8(w).c_str());
			}
			bad++;
		} else {
			printf("  [PASS] every launcher character is drawable by at least one shipped font\n");
		}
	}
	printf("\n%s\n", bad == 0 ? "ALL PASS" : "FAILED");
	return bad == 0 ? 0 : 1;
}
