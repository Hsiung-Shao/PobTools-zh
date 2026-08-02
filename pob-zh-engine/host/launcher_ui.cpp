#include "launcher_ui.h"
#include "launcher_strings.h"
#include "ui_theme.h"
#include "app_version.h"
#include "app_update.h"
#include "changelog.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

#include <GLES2/gl2.h>
#include <GLFW/glfw3.h>
#define IMGUI_DEFINE_MATH_OPERATORS
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

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
static void CollectLauncherTexts(std::vector<const char*>& out)
{
	for (const LauncherStrings* t : { &STR_ZHTW, &STR_EN }) {
		const char* const fields[] = {
			t->title, t->subtitle, t->language, t->gameVersion,
			t->poe1, t->poe2, t->detected, t->missing,
			t->notFoundPoe1, t->notFoundPoe2, t->noneFound,
			t->returnAfterExit, t->launch,
			t->editor, t->filterEditor, t->atlasPlanner, t->timelessJewel,
			t->gamesSection, t->toolsSection, t->linksSection,
			t->about, t->changelog, t->aboutBody, t->support,
			t->discord, t->close, t->font,
			t->updateAvailable, t->updateNow, t->updateDownloading,
			t->updatePreparing, t->updateRestarting, t->updateFailed,
			t->updateRetry, t->updateTransDone,
			t->updateCheck, t->updateCheckTip,
			t->updateChecking, t->updateUpToDate,
		};
		for (const char* f : fields) if (f) out.push_back(f);
	}
	out.push_back(kAppUpdateGlyphSeed); // dynamic updater Status.message vocabulary
	out.push_back(kChangelogText);      // version-history dialog body
	for (const LinkEntry& l : kLinks) out.push_back(l.label);
	out.push_back(u8"繁體中文Korean·"); // language combo labels + link separator
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
                               const std::vector<std::string>& extraTexts, float scale)
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
		CollectLauncherTexts(texts);
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
	std::vector<unsigned char> ttfData;
	LauncherFonts fonts = LoadFonts(ResolveFontPath(exeDir, cfg.fontFile), ttfData, { poe1Dir, poe2Dir }, scale);
	std::vector<std::wstring> fontList = ListAvailableFonts(exeDir);
	bool fontChanged = false;

	ImGui_ImplGlfw_InitForOpenGL(win, true);
	ImGui_ImplOpenGL3_Init("#version 100");

	// Pre-select an available game if the remembered one is missing.
	bool poe2Sel = (cfg.game == L"poe2");
	if (poe2Sel && installs.poe2Lua.empty() && !installs.poe1Lua.empty()) poe2Sel = false;
	if (!poe2Sel && installs.poe1Lua.empty() && !installs.poe2Lua.empty()) poe2Sel = true;

	int localeIdx = (cfg.locale == L"en") ? 1 : 0;
	static const wchar_t* kLocaleIds[2] = { L"zh-rTW", L"en" };

	bool launch = false;
	bool openEditor = false;
	bool applyUpdate = false;
	// tools spawn as child processes so this window stays open (see SpawnTool)
	auto spawnTool = [&](const wchar_t* flag) {
		cfg.game = poe2Sel ? L"poe2" : L"poe1";
		cfg.locale = kLocaleIds[localeIdx];
		SaveLauncherConfig(exeDir + L"pob-zh.ini", cfg);
		SpawnTool(exeDir, flag);
	};
	double transNoticeUntil = 0.0; // TransDone banner auto-dismiss deadline
	// A check the user asked for must report back even when the answer is "no
	// news"; the automatic startup one stays silent.
	bool manualCheck = false;
	double upToDateUntil = 0.0;
	while (!glfwWindowShouldClose(win) && !launch && !openEditor && !applyUpdate) {
		glfwPollEvents();

		// Live font switch: rebuild the glyph atlas between frames when the user
		// picks a different font in the status-bar combo.
		if (fontChanged) {
			fontChanged = false;
			ImGui_ImplOpenGL3_DestroyFontsTexture();
			ImGui::GetIO().Fonts->Clear();
			fonts = LoadFonts(ResolveFontPath(exeDir, cfg.fontFile), ttfData, { poe1Dir, poe2Dir }, scale);
			ImGui_ImplOpenGL3_CreateFontsTexture();
		}

		ImGui_ImplOpenGL3_NewFrame();
		ImGui_ImplGlfw_NewFrame();
		ImGui::NewFrame();

		// When the font has no CJK/Hangul glyphs, fall back to English labels.
		const LauncherStrings& S = fonts.cjkOk ? StringsFor(kLocaleIds[localeIdx], fonts.koreanOk) : STR_EN;
		const char* localeLabels[2] = { u8"繁體中文", u8"English" };

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
					if (ImGui::Button(S.updateCheck)) {
						appUpd->RequestCheck(true); // force: skip the daily throttle
						manualCheck = true;
						upToDateUntil = 0.0;
					}
					if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", S.updateCheckTip);
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
					if (ImGui::Button(label.c_str())) appUpd->StartAppUpdate();
					ImGui::PopStyleColor(3);
					if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", S.updateNow);
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

		// Games: one wide row per install, launch button inline.
		if (updaterBusy) ImGui::BeginDisabled();
		SectionLabel(fonts, scale, inner, S.gamesSection);
		bool poe1Ok = !installs.poe1Lua.empty();
		bool poe2Ok = !installs.poe2Lua.empty();
		if (GameRow("##launch1", S.poe1, installs.poe1Version, poe1Ok ? S.detected : S.missing,
				poe1Ok, fonts, scale, inner, poe1Ok ? poe1Dir.c_str() : S.notFoundPoe1, S.launch)) {
			poe2Sel = false;
			launch = true;
		}
		ImGui::Dummy(ImVec2(0, 2.0f * scale));
		if (GameRow("##launch2", S.poe2, installs.poe2Version, poe2Ok ? S.detected : S.missing,
				poe2Ok, fonts, scale, inner, poe2Ok ? poe2Dir.c_str() : S.notFoundPoe2, S.launch)) {
			poe2Sel = true;
			launch = true;
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

		// Bottom status bar: hairline, language combo left, return-checkbox right.
		{
			float rowH = ImGui::GetFrameHeight();
			float yBar = io.DisplaySize.y - rowH - ImGui::GetStyle().WindowPadding.y;
			if (yBar > ImGui::GetCursorPosY()) ImGui::SetCursorPosY(yBar);
			ImVec2 lp = ImGui::GetCursorScreenPos();
			dl->AddLine(ImVec2(lp.x, lp.y - 10.0f * scale), ImVec2(lp.x + inner, lp.y - 10.0f * scale), kGlassEdge, 1.0f);

			float comboW = 150.0f * scale;
			ImGui::AlignTextToFramePadding();
			ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
			ImGui::TextUnformatted(S.language);
			ImGui::PopStyleColor();
			ImGui::SameLine();
			ImGui::SetNextItemWidth(comboW);
			if (ImGui::BeginCombo("##locale", localeLabels[localeIdx])) {
				for (int i = 0; i < 2; i++) {
					if (ImGui::Selectable(localeLabels[i], localeIdx == i)) localeIdx = i;
				}
				ImGui::EndCombo();
			}

			// Font picker: lists Fonts\*.ttf; switching rebuilds the atlas live.
			auto fontStem = [](const std::wstring& f) {
				std::string s = to_utf8(f);
				size_t d = s.rfind(".ttf");
				if (d == std::string::npos) d = s.rfind(".TTF");
				return d != std::string::npos ? s.substr(0, d) : s;
			};
			ImGui::SameLine(0, 12.0f * scale);
			ImGui::AlignTextToFramePadding();
			ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
			ImGui::TextUnformatted(S.font);
			ImGui::PopStyleColor();
			ImGui::SameLine();
			ImGui::SetNextItemWidth(160.0f * scale);
			if (ImGui::BeginCombo("##font", fontStem(cfg.fontFile).c_str())) {
				for (const std::wstring& f : fontList) {
					if (ImGui::Selectable(fontStem(f).c_str(), f == cfg.fontFile) && f != cfg.fontFile) {
						cfg.fontFile = f;
						fontChanged = true;
					}
				}
				ImGui::EndCombo();
			}

			// Status-bar links: the version tag and the explicit "版本資訊" text
			// both open the scrollable version history, "About" opens the about
			// modal.
			auto statusLink = [&](const char* label, const char* popupId, const char* tip) {
				ImGui::SameLine(0, 12.0f * scale);
				ImGui::AlignTextToFramePadding();
				ImVec2 sz = ImGui::CalcTextSize(label);
				ImVec2 p = ImGui::GetCursorScreenPos();
				bool hov = ImGui::IsMouseHoveringRect(p, p + sz);
				ImGui::PushStyleColor(ImGuiCol_Text, hov ? PobUi::Accent() : PobUi::MutedText());
				ImGui::TextUnformatted(label);
				ImGui::PopStyleColor();
				if (hov) {
					ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
					// OpenPopup must run before SetTooltip: the tooltip window
					// becomes the current window scope and the popup id would
					// otherwise be registered at the wrong level.
					if (ImGui::IsMouseClicked(0)) ImGui::OpenPopup(popupId);
					if (tip) ImGui::SetTooltip("%s", tip);
				}
			};
			statusLink("v" POBTOOLS_VERSION_STRING, "changelog_modal", S.changelog);
			statusLink(S.changelog, "changelog_modal", nullptr);
			statusLink(S.about, "about_modal", nullptr);
			ImGui::SameLine();
			float cbW = ImGui::CalcTextSize(S.returnAfterExit).x + ImGui::GetFrameHeight() + 8.0f * scale;
			ImGui::SetCursorPosX(W - padX - cbW);
			ImGui::Checkbox(S.returnAfterExit, &cfg.returnToLauncher);
		}

		// About modal: product line, build date, attribution, support link.
		// Roomy window padding + per-line leading so the CJK body doesn't look
		// cramped (the default line height packs the lines too tightly).
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(26.0f * scale, 22.0f * scale));
		ImGui::SetNextWindowSize(ImVec2(510.0f * scale, 0), ImGuiCond_Appearing);
		bool aboutOpen = ImGui::BeginPopupModal("about_modal", nullptr,
		                                        ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar);
		ImGui::PopStyleVar();
		if (aboutOpen) {
			ImGui::PushFont(fonts.title);
			ImGui::TextUnformatted("PobTools");
			ImGui::PopFont();
			ImGui::PushStyleColor(ImGuiCol_Text, PobUi::MutedText());
			ImGui::TextUnformatted("v" POBTOOLS_VERSION_STRING "  -  Build " __DATE__);
			ImGui::PopStyleColor();
			ImGui::Dummy(ImVec2(0, 14.0f * scale));

			const float wrap = 458.0f * scale;
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

			// The Discord and sponsor links now live on the external-link board
			// on the main screen; keeping copies here would just be two places
			// to update when a URL changes.
			ImGui::Dummy(ImVec2(0, 16.0f * scale));
			if (ImGui::Button(S.close, ImVec2(120.0f * scale, 0))) ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
		}

		// Version-history modal: same styling as About, but the body lives in a
		// fixed-height scrolling child so long release notes stay browsable.
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(26.0f * scale, 22.0f * scale));
		ImGui::SetNextWindowSize(ImVec2(600.0f * scale, 480.0f * scale), ImGuiCond_Appearing);
		bool logOpen = ImGui::BeginPopupModal("changelog_modal", nullptr, ImGuiWindowFlags_NoTitleBar);
		ImGui::PopStyleVar();
		if (logOpen) {
			ImGui::PushFont(fonts.title);
			ImGui::TextUnformatted(S.changelog);
			ImGui::PopFont();
			ImGui::PushStyleColor(ImGuiCol_Text, PobUi::MutedText());
			ImGui::TextUnformatted("PobTools v" POBTOOLS_VERSION_STRING);
			ImGui::PopStyleColor();
			ImGui::Dummy(ImVec2(0, 10.0f * scale));

			float footerH = ImGui::GetFrameHeightWithSpacing() + 10.0f * scale;
			ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18.0f * scale, 14.0f * scale));
			ImGui::BeginChild("##changelog_scroll", ImVec2(0, -footerH), true,
			                  ImGuiWindowFlags_AlwaysUseWindowPadding);
			{
				// Per-line render: release headers ("vX.Y.Z...") in accent color
				// with a gap above, blank lines as wide separators, and a little
				// leading everywhere so the CJK body breathes.
				const std::string log = kChangelogText;
				size_t start = 0;
				bool first = true;
				while (start <= log.size()) {
					size_t nl = log.find('\n', start);
					size_t len = (nl == std::string::npos ? log.size() : nl) - start;
					std::string line = log.substr(start, len);
					if (line.empty()) {
						ImGui::Dummy(ImVec2(0, 10.0f * scale)); // between releases
					} else {
						bool isVer = line.size() > 1 && line[0] == 'v' &&
						             line[1] >= '0' && line[1] <= '9';
						if (isVer && !first) ImGui::Dummy(ImVec2(0, 2.0f * scale));
						ImGui::PushTextWrapPos(0.0f); // wrap at the child's right edge
						if (isVer) ImGui::PushStyleColor(ImGuiCol_Text, PobUi::Accent());
						ImGui::TextUnformatted(line.c_str());
						if (isVer) ImGui::PopStyleColor();
						ImGui::PopTextWrapPos();
						ImGui::Dummy(ImVec2(0, 5.0f * scale)); // line leading
					}
					first = false;
					if (nl == std::string::npos) break;
					start = nl + 1;
				}
			}
			ImGui::EndChild();
			ImGui::PopStyleVar();

			ImGui::Dummy(ImVec2(0, 4.0f * scale));
			if (ImGui::Button(S.close, ImVec2(120.0f * scale, 0))) ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
		}

		ImGui::End();
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

	cfg.game = poe2Sel ? L"poe2" : L"poe1";
	cfg.locale = kLocaleIds[localeIdx];

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
	std::vector<const char*> texts;
	CollectLauncherTexts(texts);

	// unique codepoints, in first-seen order so the report reads like the source
	std::vector<unsigned> want;
	{
		std::vector<bool> seen(0x110000, false);
		for (const char* t : texts) {
			for (const unsigned char* p = (const unsigned char*)t; p && *p; ) {
				unsigned cp = 0;
				int n = 1;
				if (*p < 0x80)             { cp = *p; }
				else if ((*p & 0xE0) == 0xC0) { cp = *p & 0x1Fu; n = 2; }
				else if ((*p & 0xF0) == 0xE0) { cp = *p & 0x0Fu; n = 3; }
				else if ((*p & 0xF8) == 0xF0) { cp = *p & 0x07u; n = 4; }
				else { p++; continue; }              // stray continuation byte
				for (int i = 1; i < n; i++) {
					if ((p[i] & 0xC0) != 0x80) { n = i; cp = 0; break; }
					cp = (cp << 6) | (p[i] & 0x3Fu);
				}
				p += n;
				if (cp < 0x20 || cp >= 0x110000) continue;  // control chars are not drawn
				if (!seen[cp]) { seen[cp] = true; want.push_back(cp); }
			}
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
