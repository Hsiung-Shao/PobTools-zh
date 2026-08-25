#include "tool_window.h"
#include "error_log.h"

#include "editor_util.h"       // EdReadFile
#include "launcher_config.h"   // ResolveConfiguredFontPath
#include "tool_panel.h"
#include "ui_theme.h"

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

#include <string>
#include <vector>

namespace {

const float kFontSize = 18.0f;
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

	float scale = 1.0f;
	GLFWmonitor* monitor = glfwGetPrimaryMonitor();
	if (monitor) {
		float sx = 1.0f, sy = 1.0f;
		glfwGetMonitorContentScale(monitor, &sx, &sy);
		scale = sx > 0.0f ? sx : 1.0f;
	}
	int winW = (int)(desc.defW * scale);
	int winH = (int)(desc.defH * scale);
	if (desc.clampToWorkArea && monitor) {
		int wx = 0, wy = 0, ww = 0, wh = 0;
		glfwGetMonitorWorkarea(monitor, &wx, &wy, &ww, &wh);
		if (ww > 0 && wh > 0) {
			if (winW > ww) winW = ww;
			if (winH > wh) winH = wh;
		}
	}

	GLFWwindow* win = glfwCreateWindow(winW, winH, desc.titleUtf8, nullptr, nullptr);
	if (!win) {
		glfwTerminate();
		MessageBoxW(nullptr, L"無法建立視窗。", L"PobTools", MB_ICONERROR | MB_OK);
		return 1;
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
	ImGui::GetIO().IniFilename = nullptr;   // never touch the engine's imgui.ini
	PobUi::ApplyTheme(scale, panel.Density());

	// Full CJK + Korean, so item names, node names and IME input all render. The
	// launcher builds an equivalent face for the embedded case; the ranges differ
	// only in that the launcher also folds in its own UI strings.
	std::vector<unsigned char> ttf = EdReadFile(ResolveConfiguredFontPath(exeDir));
	ImFont* font = nullptr;
	ImFont* fontBig = nullptr;
	bool cjkOk = false;
	if (!ttf.empty()) {
		ImGuiIO& io = ImGui::GetIO();
		// Must outlive the atlas: ImGui stores the pointer and re-reads it on every
		// Build(). Static because this function can only ever run one panel at a time
		// in this process.
		static ImVector<ImWchar> ranges;
		ranges.clear();
		ImFontGlyphRangesBuilder b;
		b.AddRanges(io.Fonts->GetGlyphRangesDefault());
		b.AddRanges(io.Fonts->GetGlyphRangesChineseFull());
		b.AddRanges(io.Fonts->GetGlyphRangesKorean());
		b.BuildRanges(&ranges);
		ImFontConfig cfg;
		cfg.FontDataOwnedByAtlas = false;
		cfg.OversampleH = 1;
		cfg.OversampleV = 1;
		cfg.PixelSnapH = true;
		io.Fonts->Flags |= ImFontAtlasFlags_NoPowerOfTwoHeight;
		GLint maxTex = 0;
		glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTex);
		io.Fonts->TexDesiredWidth = maxTex >= 8192 ? 8192 : 4096;
		font = io.Fonts->AddFontFromMemoryTTF(ttf.data(), (int)ttf.size(), kFontSize * scale,
		                                      &cfg, ranges.Data);
		// ToolPanelHost::big -- twelve glyphs, so it costs nothing and both hosts can
		// offer it unconditionally rather than the panel having two layouts.
		static ImVector<ImWchar> bigRanges;
		bigRanges.clear();
		ImFontGlyphRangesBuilder bb;
		bb.AddText("0123456789 /");
		bb.BuildRanges(&bigRanges);
		fontBig = io.Fonts->AddFontFromMemoryTTF(ttf.data(), (int)ttf.size(), kBigFontSize * scale,
		                                         &cfg, bigRanges.Data);
		if (io.Fonts->Build() && font)
			cjkOk = font->FindGlyphNoFallback((ImWchar)0x555F /* 啟 */) != nullptr;
	}
	if (!font) font = ImGui::GetIO().Fonts->AddFontDefault();

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
		while (running) {
			glfwPollEvents();
			ImGui_ImplOpenGL3_NewFrame();
			ImGui_ImplGlfw_NewFrame();
			ImGui::NewFrame();
			ImGui::PushFont(font);

			ImGuiIO& io = ImGui::GetIO();
			ImGui::SetNextWindowPos(ImVec2(0, 0));
			ImGui::SetNextWindowSize(io.DisplaySize);
			ImGui::Begin("##toolwindow", nullptr,
				ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
				ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
			panel.Frame();
			ImGui::End();

			// The window's own X becomes a close REQUEST, held until the panel
			// answers: a panel with unsaved work answers by drawing a prompt, and
			// obeying the close immediately would take the prompt down with it.
			if (glfwWindowShouldClose(win)) {
				glfwSetWindowShouldClose(win, GLFW_FALSE);
				panel.RequestClose();
			}
			switch (panel.CloseState()) {
				case ToolCloseState::Closed:    running = false; break;
				case ToolCloseState::Cancelled: break;  // the user stayed; carry on
				default: break;
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

			// After the frame is on screen, so a modal dialog does not appear over a
			// half-drawn window and a long pause does not eat a frame the user is
			// waiting on.
			panel.RunDeferred();
		}
	}

	// While the GL context is still current: the panel may be holding textures.
	panel.Shutdown();

	ImGui_ImplOpenGL3_Shutdown();
	ImGui_ImplGlfw_Shutdown();
	ImGui::DestroyContext();
	glfwDestroyWindow(win);
	glfwTerminate();
	return rc;
}
