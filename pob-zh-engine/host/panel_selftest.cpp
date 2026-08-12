// Drives an IToolPanel the way the LAUNCHER does, headlessly.
//
// The other selftests cannot see this path at all: none of them creates an ImGui
// frame, so a panel that draws its own root window, leaves the ID stack unbalanced
// or reads io.DisplaySize would leave every one of them green and only break when
// somebody opened the tab.
//
// What it checks is the contract in tool_panel.h, and it checks it in the
// awkward case rather than the easy one: the panel is drawn inside a tab bar,
// inside a window that is NOT the whole viewport, with another tab present. A
// panel that quietly assumed it owned the screen passes when drawn alone.
#include "panel_selftest.h"

#include "filter_editor.h"
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
#include <imgui_internal.h>   // ImGuiContext, to read the stack depths
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace {

struct Report {
	std::string text;
	int failures = 0, checks = 0;
	void check(const std::string& name, bool ok, const std::string& detail = "") {
		checks++;
		text += std::string(ok ? "PASS " : "FAIL ") + name +
		        (detail.empty() ? "" : "  (" + detail + ")") + "\n";
		if (!ok) failures++;
	}
};

// Depths that must be identical before and after Frame(). An imbalance here is
// the failure mode that hurts most when embedded: ImGui carries it forward into
// whatever the launcher draws next, so the symptom appears on a different tab.
struct Stacks {
	int window = 0, id = 0, font = 0, colour = 0, styleVar = 0, group = 0;
	bool operator==(const Stacks& o) const {
		return window == o.window && id == o.id && font == o.font &&
		       colour == o.colour && styleVar == o.styleVar && group == o.group;
	}
	std::string str() const {
		char b[128];
		snprintf(b, sizeof(b), "win=%d id=%d font=%d col=%d var=%d grp=%d",
		         window, id, font, colour, styleVar, group);
		return b;
	}
};

Stacks snapshot()
{
	ImGuiContext& g = *ImGui::GetCurrentContext();
	Stacks s;
	s.window = g.CurrentWindowStack.Size;
	s.id = g.CurrentWindow ? g.CurrentWindow->IDStack.Size : 0;
	s.font = g.FontStack.Size;
	s.colour = g.ColorStack.Size;
	s.styleVar = g.StyleVarStack.Size;
	s.group = g.GroupStack.Size;
	return s;
}

} // namespace

int RunPanelSelfTest(const std::wstring& exeDir)
{
	Report rep;

	if (!glfwInit()) {
		rep.text += "FAIL could not init GLFW\nRESULT FAIL\n";
		rep.failures++;
	} else {
		glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_ES_API);
		glfwWindowHint(GLFW_CONTEXT_CREATION_API, GLFW_EGL_CONTEXT_API);
		glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
		glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
		// Hidden and never focused: a check that steals focus produces phantom
		// clicks in whatever the user happens to be doing.
		glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
		GLFWwindow* win = glfwCreateWindow(1280, 800, "panel-selftest", nullptr, nullptr);
		if (!win) {
			rep.text += "FAIL could not create a window\n";
			rep.failures++;
		} else {
			glfwMakeContextCurrent(win);
			IMGUI_CHECKVERSION();
			ImGui::CreateContext();
			ImGui::GetIO().IniFilename = nullptr;
			PobUi::ApplyTheme(1.0f, PobUi::Density::Comfortable);
			ImGui::GetIO().Fonts->AddFontDefault();
			ImGui_ImplGlfw_InitForOpenGL(win, false);   // false: install no callbacks
			ImGui_ImplOpenGL3_Init("#version 100");

			ToolPanelHost host;
			host.exeDir = exeDir;
			host.game = L"poe1";
			host.locale = L"zh-rTW";
			host.scale = 1.0f;
			host.hostHwnd = glfwGetWin32Window(win);
			host.embedded = true;
			host.body = ImGui::GetIO().Fonts->Fonts[0];
			host.cjkOk = false;   // the default font has no CJK; the panel must cope

			std::unique_ptr<IToolPanel> panel(CreateFilterEditorPanel());
			rep.check("P1 the panel initialises", panel->Init(host));

			// The style the launcher would swap in for this panel.
			ImGuiStyle panelStyle;
			PobUi::BuildStyle(panelStyle, 1.0f, panel->Density());

			Stacks before, after;
			bool balanced = true, everDrew = false;
			for (int frame = 0; frame < 8; frame++) {
				ImGui_ImplOpenGL3_NewFrame();
				ImGui_ImplGlfw_NewFrame();
				ImGui::NewFrame();

				// NOT the whole viewport, and not at the origin: a panel that reads
				// io.DisplaySize or assumes it starts at (0,0) draws off its own area,
				// and only a container that is smaller than the screen reveals it.
				ImGui::SetNextWindowPos(ImVec2(40, 30));
				ImGui::SetNextWindowSize(ImVec2(1100, 700));
				ImGui::Begin("##container", nullptr,
					ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
				if (ImGui::BeginTabBar("##tabs")) {
					// A neighbour, so the panel is never the only thing in the bar --
					// ID collisions and popup-scope mistakes need something to collide
					// with before they show up.
					if (ImGui::BeginTabItem("neighbour")) {
						ImGui::TextUnformatted("...");
						ImGui::EndTabItem();
					}
					if (ImGui::BeginTabItem("panel", nullptr,
					                        frame == 0 ? ImGuiTabItemFlags_SetSelected : 0)) {
						ImGui::PushID(panel->PanelId());
						const ImGuiStyle keep = ImGui::GetStyle();
						ImGui::GetStyle() = panelStyle;
						before = snapshot();
						panel->Frame();
						after = snapshot();
						if (!(before == after)) balanced = false;
						everDrew = true;
						ImGui::GetStyle() = keep;
						ImGui::PopID();
						ImGui::EndTabItem();
					}
					ImGui::EndTabBar();
				}
				ImGui::End();

				ImGui::Render();
				int fw = 0, fh = 0;
				glfwGetFramebufferSize(win, &fw, &fh);
				glViewport(0, 0, fw, fh);
				glClear(GL_COLOR_BUFFER_BIT);
				ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
				glfwSwapBuffers(win);
				panel->RunDeferred();
			}

			// Without this the loop above could have skipped the tab entirely (a tab
			// bar does not draw an unselected tab's body) and everything below would
			// be comparing two default-constructed snapshots.
			rep.check("P2 the panel's body was actually drawn", everDrew);
			rep.check("P3 Frame() leaves every ImGui stack as it found it", balanced,
			          balanced ? before.str() : (before.str() + " -> " + after.str()));

			{
				const GLenum err = glGetError();
				rep.check("P4 no GL error was raised while drawing", err == GL_NO_ERROR,
				          "glGetError=" + std::to_string((unsigned)err));
			}

			// Closing an untouched panel must not put up a prompt: there is nothing to
			// save, and a tab that argues about closing when nothing changed is worse
			// than one that just goes.
			const ToolCloseState cs = panel->RequestClose();
			rep.check("P5 a clean panel closes without asking",
			          cs == ToolCloseState::Closed, "state=" + std::to_string((int)cs));
			rep.check("P6 and reports the same state when asked again",
			          panel->CloseState() == ToolCloseState::Closed);

			panel->Shutdown();
			panel.reset();

			ImGui_ImplOpenGL3_Shutdown();
			ImGui_ImplGlfw_Shutdown();
			ImGui::DestroyContext();
			glfwDestroyWindow(win);
		}
		glfwTerminate();
	}

	const int ran = rep.checks;
	rep.check("P7 the suite actually ran", ran >= 6, std::to_string(ran) + " checks");

	rep.text += rep.failures ? "RESULT FAIL\n" : "RESULT PASS\n";
	CreateDirectoryW((exeDir + L"PobTools").c_str(), nullptr);
	HANDLE h = CreateFileW((exeDir + L"PobTools\\panel_selftest.txt").c_str(), GENERIC_WRITE,
	                       FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h != INVALID_HANDLE_VALUE) {
		DWORD w = 0;
		WriteFile(h, rep.text.data(), (DWORD)rep.text.size(), &w, nullptr);
		CloseHandle(h);
	}
	if (AttachConsole(ATTACH_PARENT_PROCESS)) {
		FILE* f = nullptr;
		freopen_s(&f, "CONOUT$", "w", stdout);
	}
	printf("%s", rep.text.c_str());
	return rep.failures ? 2 : 0;
}
