#include "tool_window.h"
#include "error_log.h"

#include "editor_util.h"       // EdReadFile
#include "launcher_config.h"   // ResolveConfiguredFontPath
#include "tool_panel.h"
#include "frame_pacing.h"      // idle wait, minimised = no present, unchanged frame = no present
#include "live_resize.h"       // drawing during a border drag
#include "ui_theme.h"
#include "ui_widgets.h"      // PobUi::SetWidgetFonts, toasts
#include "ui_icons_data.h"   // the icon font, merged into body / small
#include "gl_shot.h"         // POBTOOLS_TOOL_SHOT

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <GLES2/gl2.h>
#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#define IMGUI_DEFINE_MATH_OPERATORS
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <cmath>
#include <string>
#include <vector>

// Set by the refresh and framebuffer-size callbacks (live_resize.h): the next
// frame is presented even if its draw data matches the last one.
static bool g_toolRedraw = true;

namespace {

// The launcher's sizes (launcher_ui.cpp), so a tool looks the same as its own
// window and as a launcher tab: the design system's 16 px body is 19 px here,
// and every widget size goes through PobUi::D() on that 19/16 ratio.
const float kFontSize = 19.0f;
const float kSmallFontSize = 15.0f;   // hints, pills, numerics
const float kHeadingFontSize = 20.0f; // drawn with the body face (WidgetFonts::headingPx)
const float kBigFontSize = 30.0f;   // ToolPanelHost::big

} // namespace

int RunToolWindow(IToolPanel& panel, const ToolWindowDesc& desc,
                  const std::wstring& exeDir, const std::wstring& game,
                  const std::wstring& locale)
{
	if (!glfwInit()) {
		MessageBoxW(nullptr, L"無法初始化 GLFW。", L"PobTools", MB_ICONERROR | MB_OK);
		return 1;
	}
	glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_ES_API);
	glfwWindowHint(GLFW_CONTEXT_CREATION_API, GLFW_EGL_CONTEXT_API);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
	glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
	glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);

	// Same scale rule as the launcher: monitor content scale times the user's
	// font-size zoom from pob-zh.ini, so a tool opened as its own window is the
	// same size on screen as the same tool opened as a launcher tab.
	const float zoom = LauncherZoom(LoadLauncherConfig(exeDir + L"pob-zh.ini").fontSize);
	float scale = 1.0f;
	GLFWmonitor* monitor = glfwGetPrimaryMonitor();
	if (monitor) {
		float sx = 1.0f, sy = 1.0f;
		glfwGetMonitorContentScale(monitor, &sx, &sy);
		scale = sx > 0.0f ? sx : 1.0f;
	}
	scale *= zoom;
	int winW = (int)(desc.defW * scale);
	int winH = (int)(desc.defH * scale);
	// Work area, physical pixels; (0,0)-sized when unknown.
	int wx = 0, wy = 0, ww = 0, wh = 0;
	if (monitor) glfwGetMonitorWorkarea(monitor, &wx, &wy, &ww, &wh);
	// Windows that asked to be clamped always are; every window is once zoom
	// has pushed it past the screen (1500 * 1.37 on a 1920-wide monitor).
	if ((desc.clampToWorkArea || zoom > 1.0f) && ww > 0 && wh > 0) {
		if (winW > ww) winW = ww;
		if (winH > wh) winH = wh;
	}

	GLFWwindow* win = glfwCreateWindow(winW, winH, desc.titleUtf8, nullptr, nullptr);
	if (!win) {
		glfwTerminate();
		MessageBoxW(nullptr, L"無法建立視窗。", L"PobTools", MB_ICONERROR | MB_OK);
		return 1;
	}
	if (ww > 0 && wh > 0) {
		// Centred on the work area so a screen-tall window is not half under the
		// taskbar.
		glfwSetWindowPos(win, wx + (ww - winW) / 2, wy + (wh - winH) / 2);
	} else if (monitor) {
		const GLFWvidmode* mode = glfwGetVideoMode(monitor);
		if (mode) glfwSetWindowPos(win, (mode->width - winW) / 2, (mode->height - winH) / 2);
	}
	glfwMakeContextCurrent(win);
	glfwSwapInterval(1);
	// NOT shown yet: GLFW_VISIBLE is false above, and the window goes on screen
	// with its first presented frame (FramePacing::FirstShow). Shown here it was a
	// white rectangle for the 330-410 ms the atlas and the panel take to build.
	FramePacing::FirstShow firstShow;
	firstShow.Start(glfwGetTime());
	// POBTOOLS_TOOL_SHOT=<file.bmp>: never show the window; draw until the panel
	// has settled, save that frame and quit. A screenshot with nothing on anyone's
	// screen and no input sent (a panel may add its own variable for which page,
	// e.g. POBTOOLS_ATLAS_PAGE).
	std::wstring shotPath = ShotEnv(L"POBTOOLS_TOOL_SHOT");
	const bool shotMode = !shotPath.empty();
	const double shotSince = glfwGetTime();
	const double kShotSettle = 3.0;   // icon downloads and the first layout passes
	auto showIfDue = [&](bool presented) {
		if (firstShow.Due(presented, glfwGetTime())) {
			glfwShowWindow(win);
		}
	};
	// Unchanged frames are not presented (frame_pacing.h); a refresh or a resize
	// invalidates the picture without changing the draw data, so both force one.
	// The refresh callback also draws while a border is being dragged
	// (live_resize.h). One tool window per process, hence the file-level flag.
	// Before the ImGui backend, which chains the callbacks it needs and leaves
	// these two.
	LiveResize::Install(win, &g_toolRedraw);
	g_toolRedraw = true;

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGui::GetIO().IniFilename = nullptr;   // never touch the engine's imgui.ini
	PobUi::ApplyTheme(scale, panel.Density());

	// Full CJK + Korean, so item names, node names and IME input all render. The
	// launcher builds an equivalent face for the embedded case; the ranges differ
	// only in that the launcher also folds in its own UI strings.
	const std::wstring primaryFontPath = ResolveConfiguredFontPath(exeDir);
	std::vector<unsigned char> ttf = EdReadFile(primaryFontPath);
	ImFont* font = nullptr;
	ImFont* fontSmall = nullptr;
	ImFont* fontBig = nullptr;
	bool cjkOk = false;
	bool iconsOk = false;
	if (!ttf.empty()) {
		ImGuiIO& io = ImGui::GetIO();
		// Must outlive the atlas: ImGui stores the pointer and re-reads it on every
		// Build(). Static because this function can only ever run one panel at a time
		// in this process.
		static ImVector<ImWchar> ranges;
		ImFontConfig cfg;
		cfg.FontDataOwnedByAtlas = false;
		cfg.OversampleH = 1;
		cfg.OversampleV = 1;
		cfg.PixelSnapH = true;
		io.Fonts->Flags |= ImFontAtlasFlags_NoPowerOfTwoHeight;
		GLint maxTex = 0;
		glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTex);
		if (maxTex <= 0) maxTex = 2048;
		io.Fonts->TexDesiredWidth = maxTex >= 8192 ? 8192 : (maxTex >= 4096 ? 4096 : 2048);
		// Every other shipped font, merged as glyph fallbacks (present glyphs are
		// skipped): the tools draw zh-rCN dictionary text, and Noto Sans TC has
		// no simplified-only glyphs. Static: the atlas keeps the pointers.
		static std::vector<std::vector<unsigned char>> fallbackTtfs;
		fallbackTtfs.clear();
		for (const std::wstring& f : ListAvailableFonts(exeDir)) {
			const std::wstring p = exeDir + L"Fonts\\" + f;
			if (_wcsicmp(p.c_str(), primaryFontPath.c_str()) == 0) continue;
			std::vector<unsigned char> fb = EdReadFile(p);
			if (!fb.empty()) fallbackTtfs.push_back(std::move(fb));
		}
		ImFontConfig cfgMerge = cfg;
		cfgMerge.MergeMode = true;
		// Where the icon font sits on a text line depends on the primary face's
		// ascent (ui_icons_data.h), the same rule the launcher uses.
		const float primaryAscent = PobIcon::HheaAscentRatio(ttf);
		// ToolPanelHost::big -- a score of glyphs, so it costs nothing and both hosts
		// can offer it unconditionally rather than the panel having two layouts.
		// Keep in step with the launcher's rangesDigits (launcher_ui.cpp).
		static ImVector<ImWchar> bigRanges;
		bigRanges.clear();
		ImFontGlyphRangesBuilder bb;
		bb.AddText(kBigFontGlyphs);
		bb.BuildRanges(&bigRanges);

		// One attempt at a glyph set; true when the built atlas is uploadable.
		// The launcher has the same ladder (LoadFonts in launcher_ui.cpp) for the
		// same reason: an atlas over GL_MAX_TEXTURE_SIZE is not an error anywhere,
		// it is a window that draws nothing. Reachable here at the largest
		// font-size setting on a high-DPI monitor, so it has to be guarded.
		auto attempt = [&](bool fullCjk, bool korean) -> bool {
			io.Fonts->Clear();
			ranges.clear();
			ImFontGlyphRangesBuilder b;
			b.AddRanges(io.Fonts->GetGlyphRangesDefault());
			b.AddRanges(fullCjk ? io.Fonts->GetGlyphRangesChineseFull()
			                    : io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
			if (korean) b.AddRanges(io.Fonts->GetGlyphRangesKorean());
			b.BuildRanges(&ranges);
			// Body and small: the primary face, every fallback, then the icons --
			// last, so a text font that uses the same Private Use Area keeps its
			// glyphs. Small carries the same glyph set as body because a tool's
			// hints quote node, map and item names, not just fixed strings.
			auto addFace = [&](float px) -> ImFont* {
				ImFont* f = io.Fonts->AddFontFromMemoryTTF(ttf.data(), (int)ttf.size(), px, &cfg, ranges.Data);
				for (std::vector<unsigned char>& fb : fallbackTtfs)
					io.Fonts->AddFontFromMemoryTTF(fb.data(), (int)fb.size(), px, &cfgMerge, ranges.Data);
				PobIcon::MergeInto(io.Fonts, px, cfgMerge, primaryAscent);
				return f;
			};
			font = addFace(kFontSize * scale);
			fontSmall = addFace(kSmallFontSize * scale);
			fontBig = io.Fonts->AddFontFromMemoryTTF(ttf.data(), (int)ttf.size(), kBigFontSize * scale,
			                                         &cfg, bigRanges.Data);
			if (!io.Fonts->Build()) return false;
			return io.Fonts->TexWidth <= maxTex && io.Fonts->TexHeight <= maxTex;
		};
		// Same ladder as the launcher's LoadFonts: the block the active language
		// does not read is the first to go. With a Korean interface that is the
		// full CJK block; for everyone else it is Hangul.
		const bool preferKorean = LoadLauncherConfig(exeDir + L"pob-zh.ini").locale == L"ko-KR";
		bool built = attempt(true, true);
		if (!built) {
			PobLog::Error("i18n", std::string("tool window font atlas over the GPU limit at ") +
			                          std::to_string((int)(kFontSize * scale)) + " px; retrying without " +
			                          (preferKorean ? "the full CJK block" : "Korean"));
			built = preferKorean ? attempt(false, true) : attempt(true, false);
		}
		if (!built) {
			PobLog::Error("i18n", "tool window font atlas over the GPU limit with the full CJK block; "
			                      "falling back to the common set (rare characters will show as ?)");
			built = attempt(false, false);
		}
		if (!built) {
			PobLog::Error("i18n", "tool window font atlas does not fit the GPU even at the common set; "
			                      "using ImGui's built-in ASCII font");
			io.Fonts->Clear();
			font = nullptr;
			fontSmall = nullptr;
			fontBig = nullptr;
		}
		if (font)
			cjkOk = font->FindGlyphNoFallback((ImWchar)0x555F /* 啟 */) != nullptr;
		// Measured on the faces the widgets draw with, not assumed from "the TTF
		// loaded": without them every icon would be a '?'.
		iconsOk = PobIcon::FaceHasIcons(font) && PobIcon::FaceHasIcons(fontSmall);
	}
	if (!font) font = ImGui::GetIO().Fonts->AddFontDefault();
	if (!fontSmall) fontSmall = font;
	// Check point before Init, the other slow step: only past kFirstShowLimit, i.e.
	// when the atlas alone took that long. None after Init -- the loop's first
	// pass presents a few milliseconds later and shows the window with content.
	if (!shotMode) showIfDue(false);

	ImGui_ImplGlfw_InitForOpenGL(win, true);
	ImGui_ImplOpenGL3_Init("#version 100");

	ToolPanelHost host;
	host.exeDir = exeDir;
	host.game = game;
	host.locale = locale;
	host.scale = scale;
	host.hostHwnd = glfwGetWin32Window(win);
	host.embedded = false;
	host.body = font;
	host.big = fontBig;   // null when the font file could not be read; panels guard
	host.cjkOk = cjkOk;

	int rc = 0;
	if (!panel.Init(host)) {
		rc = 1;
		// Safe here -- there is no frame in flight yet. The launcher cannot do this
		// at the same point; see IToolPanel::InitError.
		if (const char* why = panel.InitError(); why && *why) {
			PobLog::Error("panel", std::string(panel.PanelId() ? panel.PanelId() : "?") +
			                           u8" 面板初始化失敗：" + why);
			const int n = MultiByteToWideChar(CP_UTF8, 0, why, -1, nullptr, 0);
			std::wstring w((size_t)(n > 0 ? n - 1 : 0), L'\0');
			if (n > 0) MultiByteToWideChar(CP_UTF8, 0, why, -1, &w[0], n);
			MessageBoxW(nullptr, w.c_str(), L"PobTools", MB_ICONERROR | MB_OK);
		}
	} else {
		bool running = true;
		// Pacing (frame_pacing.h): wait for events instead of spinning, present
		// only changed frames. `nextWait` is decided at the bottom of each pass.
		FramePacing::Pacer pacer;
		double nextWait = 0.0;
		// One frame. `live` = called from the refresh callback in the middle of
		// a border drag (live_resize.h): draw and present, nothing else -- the
		// close request and the deferred work wait for the main loop, which runs
		// again as soon as the drag ends.
		auto frame = [&](bool live) {
			pacer.BeginFrame(glfwGetTime());
			ImGui_ImplOpenGL3_NewFrame();
			ImGui_ImplGlfw_NewFrame();
			ImGui::NewFrame();
			// Every frame, with the real scale: the widgets size themselves from it
			// (PobUi::D), and the faces are only valid for this atlas.
			{
				PobUi::WidgetFonts wf;
				wf.body = font;
				wf.small = fontSmall;
				wf.heading = font;
				wf.headingPx = std::floor(kHeadingFontSize * scale);
				wf.title = font;
				wf.scale = scale;
				wf.icons = iconsOk;
				PobUi::SetWidgetFonts(wf);
			}
			ImGui::PushFont(font);

			ImGuiIO& io = ImGui::GetIO();
			ImGui::SetNextWindowPos(ImVec2(0, 0));
			ImGui::SetNextWindowSize(io.DisplaySize);
			ImGui::Begin("##toolwindow", nullptr,
				ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
				ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
			panel.Frame();
			ImGui::End();
			PobUi::DrawToast();   // results the panel reported with ShowToast

			// The window's own X becomes a close REQUEST, held until the panel
			// answers: a panel with unsaved work answers by drawing a prompt, and
			// obeying the close immediately would take the prompt down with it.
			if (!live && glfwWindowShouldClose(win)) {
				glfwSetWindowShouldClose(win, GLFW_FALSE);
				panel.RequestClose();
			}
			if (!live) switch (panel.CloseState()) {
				case ToolCloseState::Closed:    running = false; break;
				case ToolCloseState::Cancelled: break;  // the user stayed; carry on
				default: break;
			}

			ImGui::PopFont();
			ImGui::Render();
			FramePacing::Inputs pace;
			pace.now = glfwGetTime();
			pace.iconified = glfwGetWindowAttrib(win, GLFW_ICONIFIED) != 0;
			pace.activity = FramePacing::ImGuiActivity();
			pace.busy = panel.CloseState() == ToolCloseState::Asking || // a prompt answered over frames
			            PobUi::ToastVisible();                         // its fade is not input-driven
			pace.forceRender = g_toolRedraw || shotMode;
			g_toolRedraw = false;
			const bool present = pacer.ShouldRender(pace, ImGui::GetDrawData());
			if (present) {
				int fbW = 0, fbH = 0;
				glfwGetFramebufferSize(win, &fbW, &fbH);
				glViewport(0, 0, fbW, fbH);
				glClearColor(0.043f, 0.063f, 0.078f, 1.0f);
				glClear(GL_COLOR_BUFFER_BIT);
				ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
				// Test aid (POBTOOLS_TOOL_SHOT): once the panel has settled, read this
				// frame back, write it out and quit. The window is never shown.
				if (!live && shotMode && !shotPath.empty() && glfwGetTime() - shotSince > kShotSettle) {
					WriteFramebufferBmp(shotPath, fbW, fbH);
					shotPath.clear();
					running = false;
				}
				glfwSwapBuffers(win);
			}
			if (live) return;
			// After the swap: the picture is in the swap chain before the window is
			// on screen. Showing it delivers a WM_PAINT, whose refresh callback
			// presents once more on the next pass.
			if (!shotMode) showIfDue(present);
			nextWait = pacer.WaitSeconds(glfwGetTime());
		};
		LiveResize::SetDraw([&] { frame(true); });
		while (running) {
			// A border drag runs inside these two calls (and draws through `frame`).
			if (nextWait > 0.0) glfwWaitEventsTimeout(nextWait);
			else glfwPollEvents();
			// The frame and the deferred work: no live frame may start in here,
			// e.g. from a WM_PAINT delivered inside a file dialog.
			LiveResize::MainScope inMain;
			frame(false);

			// After the frame is on screen, so a modal dialog does not appear over a
			// half-drawn window and a long pause does not eat a frame the user is
			// waiting on.
			panel.RunDeferred();
		}
		LiveResize::SetDraw(nullptr);   // the loop's state goes out of scope here
	}

	// While the GL context is still current: the panel may be holding textures.
	panel.Shutdown();

	ImGui_ImplOpenGL3_Shutdown();
	ImGui_ImplGlfw_Shutdown();
	ImGui::DestroyContext();
	LiveResize::Uninstall(win);
	glfwDestroyWindow(win);
	glfwTerminate();
	return rc;
}
