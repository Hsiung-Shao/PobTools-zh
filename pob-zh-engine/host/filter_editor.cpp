#include "filter_editor.h"
#include "editor_shell.h"
#include "editor_util.h"
#include "filter_data.h"     // ListFilters / Poe1FilterDirs
#include "filter_parser.h"   // SaveFilter (close guard)
#include "launcher_config.h" // ResolveConfiguredFontPath
#include "ui_theme.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <GLES2/gl2.h>
#include <GLFW/glfw3.h>
#define IMGUI_DEFINE_MATH_OPERATORS
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <string>
#include <vector>

static const float kFontSize = 18.0f;

// ---- editor (app-shell host) -----------------------------------------------
//
// This function owns the window / GL context / theme / fonts / main loop and the
// unsaved-changes guard. Per-frame content is the EditorShell + a left-nav section
// switch (editor_shell.cpp); the model is only mutated through the existing
// English-token paths, so the saved .filter stays English.

void ShowFilterEditor(const std::wstring& exeDir, const std::wstring& /*game*/, const std::wstring& locale)
{
	if (!glfwInit()) {
		MessageBoxW(nullptr, L"無法初始化 GLFW，過濾器編輯器無法顯示。", L"PobTools", MB_ICONERROR | MB_OK);
		return;
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
	const int winW = (int)(1180 * scale);
	const int winH = (int)(740 * scale);

	// "PobTools — 過濾器編輯器"
	GLFWwindow* win = glfwCreateWindow(winW, winH,
		"PobTools \xe2\x80\x94 \xe9\x81\x8e\xe6\xbf\xbe\xe5\x99\xa8\xe7\xb7\xa8\xe8\xbc\xaf\xe5\x99\xa8", nullptr, nullptr);
	if (!win) {
		glfwTerminate();
		MessageBoxW(nullptr, L"無法建立過濾器編輯器視窗。", L"PobTools", MB_ICONERROR | MB_OK);
		return;
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
	ImGui::GetIO().IniFilename = nullptr;
	PobUi::ApplyTheme(scale, PobUi::Density::Compact);

	// Full CJK + Korean atlas (same as the translation editor) so any text in the
	// filter — and IME input into custom-sound paths — renders correctly.
	std::vector<unsigned char> ttf = EdReadFile(ResolveConfiguredFontPath(exeDir));
	ImFont* font = nullptr;
	bool cjkOk = false;
	if (!ttf.empty()) {
		ImGuiIO& io = ImGui::GetIO();
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
		io.Fonts->TexDesiredWidth = 4096;
		font = io.Fonts->AddFontFromMemoryTTF(ttf.data(), (int)ttf.size(), kFontSize * scale, &cfg, ranges.Data);
		if (io.Fonts->Build() && font)
			cjkOk = font->FindGlyphNoFallback((ImWchar)0x555F /* 啟 */) != nullptr;
	}
	if (!font) font = ImGui::GetIO().Fonts->AddFontDefault();

	ImGui_ImplGlfw_InitForOpenGL(win, true);
	ImGui_ImplOpenGL3_Init("#version 100");

	// --- shell state ---
	EditorShell shell;
	shell.exeDir = exeDir;
	shell.locale = locale;
	shell.scale = scale;
	shell.cjkOk = cjkOk;
	shell.fileList = ListFilters();
	{
		std::vector<std::wstring> scanDirs = Poe1FilterDirs();
		shell.initialDir = scanDirs.empty() ? std::wstring() : scanDirs.front();
	}
	shell.i18n.Load(exeDir, EdNarrow(locale));   // Chinese item names (display only)
	shell.library.Load(exeDir, shell.i18n);      // whole-game catalog (zh input -> en token)
	LoadEditorSettings(shell);                   // legacy ini settings (league etc.)

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
		ImGui::Begin("##filtereditor", nullptr,
			ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);

		DrawTopToolbar(shell);
		ImGui::Separator();
		if (!shell.cjkOk) {
			ImGui::TextColored(ImVec4(0.94f, 0.27f, 0.27f, 1.0f),
				"[!] CJK font atlas not loaded (Fonts\\FZ_ZY.ttf). Chinese cannot display.");
		}

		// nav (left) + content (right) above a one-line status bar.
		const float statusH = ImGui::GetFrameHeightWithSpacing();
		ImGui::BeginChild("##nav", ImVec2(176 * scale, -statusH), true);
		ImGui::TextDisabled(u8"編輯模式");
		ImGui::Separator();
		DrawLeftNav(shell);
		ImGui::EndChild();
		ImGui::SameLine();
		ImGui::BeginChild("##content", ImVec2(0, -statusH), false);
		switch (shell.section) {
			case Section::FilterEdit:  DrawFilterEditSection(shell); break;
			case Section::DropPreview: DrawDropPreviewSection(shell); break;
			case Section::Sounds:      DrawSoundsSection(shell); break;
		}
		ImGui::EndChild();
		DrawStatusBar(shell);

		ImGui::End();

		// --- unsaved-changes guard on close ---
		if (glfwWindowShouldClose(win)) {
			if (shell.model.dirty) {
				glfwSetWindowShouldClose(win, GLFW_FALSE);
				ImGui::OpenPopup(u8"未儲存變更");
			} else {
				running = false;
			}
		}
		if (ImGui::BeginPopupModal(u8"未儲存變更", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
			ImGui::TextUnformatted(u8"過濾器有尚未儲存的變更。");
			ImGui::Spacing();
			if (ImGui::Button(u8"儲存並關閉")) {
				std::string err;
				SaveFilter(shell.model, &err);
				ImGui::CloseCurrentPopup();
				running = false;
			}
			ImGui::SameLine();
			if (ImGui::Button(u8"直接關閉")) { ImGui::CloseCurrentPopup(); running = false; }
			ImGui::SameLine();
			if (ImGui::Button(u8"取消")) ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
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

	ImGui_ImplOpenGL3_Shutdown();
	ImGui_ImplGlfw_Shutdown();
	ImGui::DestroyContext();
	glfwDestroyWindow(win);
	glfwTerminate();
}
