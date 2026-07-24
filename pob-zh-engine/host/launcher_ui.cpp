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

// Logical (unscaled) window size; multiplied by the monitor content scale.
static const int kWinW = 1000;
static const int kWinH = 700;
static const float kFontSize = 19.0f;
static const float kSmallFontSize = 15.0f;
static const float kTitleFontSize = 26.0f;

// External-link board (wide layout). Placeholder set — the final list comes
// from the user; swap the entries here, labels feed the glyph atlas
// automatically.
struct LinkEntry { const char* label; const wchar_t* url; };
static const LinkEntry kLinks[] = {
	{ u8"PoeDB 流亡編年史",       L"https://poedb.tw" },
	{ u8"PoE2DB",                 L"https://poe2db.tw" },
	{ u8"官方網站",               L"https://www.pathofexile.com" },
	{ u8"官方交易市集",           L"https://www.pathofexile.com/trade" },
	{ u8"PoE Wiki",               L"https://www.poewiki.net" },
	{ u8"巴哈姆特 PoE 板",        L"https://forum.gamer.com.tw/A.php?bsn=18966" },
	{ u8"Reddit r/pathofexile",   L"https://www.reddit.com/r/pathofexile/" },
	{ u8"poe.ninja",              L"https://poe.ninja" },
	{ u8"FilterBlade",            L"https://www.filterblade.xyz" },
};

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
	for (const LauncherStrings* t : { &STR_ZHTW, &STR_EN }) {
		b.AddText(t->title); b.AddText(t->subtitle); b.AddText(t->language); b.AddText(t->gameVersion);
		b.AddText(t->poe1); b.AddText(t->poe2); b.AddText(t->detected); b.AddText(t->missing);
		b.AddText(t->notFoundPoe1); b.AddText(t->notFoundPoe2); b.AddText(t->noneFound);
		b.AddText(t->returnAfterExit); b.AddText(t->launch);
		b.AddText(t->editor); b.AddText(t->filterEditor); b.AddText(t->atlasPlanner);
		b.AddText(t->timelessJewel);
		b.AddText(t->gamesSection); b.AddText(t->toolsSection); b.AddText(t->linksSection);
		b.AddText(t->about); b.AddText(t->changelog); b.AddText(t->aboutBody); b.AddText(t->support); b.AddText(t->close);
		b.AddText(t->font);
		b.AddText(t->updateAvailable); b.AddText(t->updateNow); b.AddText(t->updateDownloading);
		b.AddText(t->updatePreparing); b.AddText(t->updateRestarting); b.AddText(t->updateFailed);
		b.AddText(t->updateRetry); b.AddText(t->updateTransDone);
	}
	b.AddText(kAppUpdateGlyphSeed); // dynamic updater Status.message vocabulary
	b.AddText(kChangelogText);      // version-history dialog body
	for (const LinkEntry& l : kLinks) b.AddText(l.label);
	b.AddText(u8"繁體中文简体한국어Korean·"); // language combo item labels + link separator
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
	bool openFilterEditor = false;
	bool openAtlasPlanner = false;
	bool openTimelessJewel = false;
	bool applyUpdate = false;
	double transNoticeUntil = 0.0; // TransDone banner auto-dismiss deadline
	while (!glfwWindowShouldClose(win) && !launch && !openEditor && !openFilterEditor && !openAtlasPlanner && !openTimelessJewel && !applyUpdate) {
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
				appUpd->AckNotice(); // silent: only problems and news are shown
				ust = appUpd->Poll();
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
			if (appUpd && ust.phase != AppUpdatePhase::Idle && ust.phase != AppUpdatePhase::Checking) {
				ImVec2 keep = ImGui::GetCursorPos();
				ImGui::PushFont(fonts.small);
				auto placeRight = [&](float w, float h) {
					ImGui::SetCursorScreenPos(bp + ImVec2(inner - w, (badge - h) * 0.5f));
				};
				if (ust.phase == AppUpdatePhase::AppAvailable) {
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

		// Tools: secondary actions. The translation editor is temporarily hidden
		// (restore the commented button + revert toolSize to /3 to bring it back).
		SectionLabel(fonts, scale, inner, S.toolsSection);
		{
			float gap = 12.0f * scale;
			ImVec2 toolSize((inner - 2.0f * gap) / 3.0f, 46.0f * scale);
			ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.84f, 0.91f, 0.92f, 1.0f));
			// if (ImGui::Button(S.editor, toolSize)) openEditor = true;   // hidden for now
			if (ImGui::Button(S.filterEditor, toolSize)) openFilterEditor = true;
			ImGui::SameLine(0, gap);
			if (ImGui::Button(S.atlasPlanner, toolSize)) openAtlasPlanner = true;
			ImGui::SameLine(0, gap);
			if (ImGui::Button(S.timelessJewel, toolSize)) openTimelessJewel = true;
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

			ImGui::Dummy(ImVec2(0, 6.0f * scale));
			LinkText(S.support, L"https://buymeacoffee.com/hsiung");
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
	if (openFilterEditor) return LauncherResult::OpenFilterEditor;
	if (openAtlasPlanner) return LauncherResult::OpenAtlasPlanner;
	if (openTimelessJewel) return LauncherResult::OpenTimelessJewel;
	if (applyUpdate) return LauncherResult::ApplyAppUpdate;
	return launch ? LauncherResult::Launch : LauncherResult::Quit;
}
