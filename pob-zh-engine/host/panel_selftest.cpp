// Drives an IToolPanel the way the LAUNCHER does, headlessly.
//
// No other selftest creates an ImGui frame, so a panel that draws its own root
// window, unbalances the ID stack or reads io.DisplaySize would only break when
// somebody opened the tab.
//
// Checks the contract in tool_panel.h in the awkward case: drawn inside a tab
// bar, inside a window that is NOT the whole viewport, with another tab present.
#include "panel_selftest.h"

#include "atlas_planner.h"
#include "filter_editor.h"
#include "launcher_editor.h"
#include "warehouse_tool.h"
#include "timeless_jewel_ui.h"
#include "tool_panel.h"
#include "ui_theme.h"
#include "ui_widgets.h"      // the panels draw with the design-system widgets
#include "ui_icons.h"
#include "ui_icons_data.h"   // icons merged into the faces, as both real hosts do

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

#include <algorithm>
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

// Depths that must be identical before and after Frame(): ImGui carries an
// imbalance into whatever the launcher draws next, on a different tab.
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
		// Hidden and never focused: stealing focus would send phantom clicks elsewhere.
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
			// The built-in face with the icon font merged in, at two sizes, as both
			// real hosts do (the widgets' icon branches differ from the no-icon fallbacks).
			ImFontAtlas* atlas = ImGui::GetIO().Fonts;
			ImFontConfig defCfg;
			defCfg.SizePixels = 13.0f;
			ImFont* stBody = atlas->AddFontDefault(&defCfg);
			ImFontConfig iconMerge;
			iconMerge.MergeMode = true;
			PobIcon::MergeInto(atlas, 13.0f, iconMerge, 0.8f);
			ImFontConfig smallCfg;
			smallCfg.SizePixels = 11.0f;
			ImFont* stSmall = atlas->AddFontDefault(&smallCfg);
			PobIcon::MergeInto(atlas, 11.0f, iconMerge, 0.8f);
			atlas->Build();
			const bool stIcons = PobIcon::FaceHasIcons(stBody) && PobIcon::FaceHasIcons(stSmall);
			rep.check("P0 the icon font is merged into the selftest's body and small faces", stIcons);
			ImGui_ImplGlfw_InitForOpenGL(win, false);   // false: install no callbacks
			ImGui_ImplOpenGL3_Init("#version 100");

			ToolPanelHost host;
			host.exeDir = exeDir;
			host.game = L"poe1";
			host.locale = L"zh-rTW";
			host.scale = 1.0f;
			host.hostHwnd = glfwGetWin32Window(win);
			host.embedded = true;
			host.body = stBody;
			// Both real hosts always supply one.
			host.big = stBody;
			host.cjkOk = false;   // the default font has no CJK; the panel must cope

			// Every panel, not just the easiest one.
			struct Entry { const char* what; IToolPanel* (*make)(); };
			const Entry kPanels[] = {
				{ "filter", &CreateFilterEditorPanel },
				{ "trans",  &CreateTranslationEditorPanel },
				{ "tj",     &CreateTimelessJewelPanel },
				{ "atlas",  &CreateAtlasPlannerPanel },
				{ "warehouse", &CreateWarehousePanel },
			};
			for (const Entry& ent : kPanels) {
			std::unique_ptr<IToolPanel> panel(ent.make());
			const std::string tag = std::string(" [") + ent.what + "]";
			rep.check(std::string("P1 the panel initialises") + tag, panel->Init(host));

			// The style the launcher would swap in for this panel.
			ImGuiStyle panelStyle;
			PobUi::BuildStyle(panelStyle, 1.0f, panel->Density());

			Stacks before, after;
			bool balanced = true, everDrew = false;
			// How much the panel put on screen at its busiest frame: three tools draw
			// a short apology when their data files are missing, which would leave
			// every check below green. A real body is thousands of vertices.
			int peakVtx = 0;
			for (int frame = 0; frame < 8; frame++) {
				ImGui_ImplOpenGL3_NewFrame();
				ImGui_ImplGlfw_NewFrame();
				ImGui::NewFrame();
				{
					// What both hosts do every frame (launcher_ui.cpp, tool_window.cpp).
					PobUi::WidgetFonts wf;
					wf.body = stBody;
					wf.small = stSmall;
					wf.heading = stBody;
					wf.headingPx = 15.0f;
					wf.title = stBody;
					wf.scale = 1.0f;
					wf.icons = stIcons;
					PobUi::SetWidgetFonts(wf);
				}

				// NOT the whole viewport, not at the origin: reveals a panel that reads
				// io.DisplaySize or assumes (0,0).
				ImGui::SetNextWindowPos(ImVec2(40, 30));
				ImGui::SetNextWindowSize(ImVec2(1100, 700));
				ImGui::Begin("##container", nullptr,
					ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
				if (ImGui::BeginTabBar("##tabs")) {
					// A neighbour, so ID collisions and popup-scope mistakes have something to hit.
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
						{
							PobUi::ToolDensityScope toolDensity;   // as both hosts do
							panel->Frame();
						}
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
				// Whole-frame: a panel's children each own a draw list. The neighbour
				// tab and bar add well under a hundred vertices.
				if (ImGui::GetDrawData())
					peakVtx = (std::max)(peakVtx, ImGui::GetDrawData()->TotalVtxCount);
				int fw = 0, fh = 0;
				glfwGetFramebufferSize(win, &fw, &fh);
				glViewport(0, 0, fw, fh);
				glClear(GL_COLOR_BUFFER_BIT);
				ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
				glfwSwapBuffers(win);
				panel->RunDeferred();
			}

			// The loop could have skipped the tab entirely (unselected tab bodies are
			// not drawn), leaving the checks below comparing empty snapshots.
			rep.check(std::string("P2 the panel's body was actually drawn") + tag, everDrew);
			// Measured: the atlas planner's missing-data branch draws 444 vertices, its
			// real body 40,218; the other three 1,694 / 3,512 / 99,508. 1000 separates
			// "drew almost nothing" from "drew a panel".
			rep.check(std::string("P2b ...and it was the real body, not a missing-data notice") + tag,
			          peakVtx >= 1000, std::to_string(peakVtx) + " vertices");
			rep.check(std::string("P3 Frame() leaves every ImGui stack as it found it") + tag, balanced,
			          balanced ? before.str() : (before.str() + " -> " + after.str()));

			{
				const GLenum err = glGetError();
				rep.check(std::string("P4 no GL error was raised while drawing") + tag, err == GL_NO_ERROR,
				          "glGetError=" + std::to_string((unsigned)err));
			}

			// Closing an untouched panel must not put up a prompt.
			const ToolCloseState cs = panel->RequestClose();
			rep.check(std::string("P5 a clean panel closes without asking") + tag,
			          cs == ToolCloseState::Closed, "state=" + std::to_string((int)cs));
			rep.check(std::string("P6 and reports the same state when asked again") + tag,
			          panel->CloseState() == ToolCloseState::Closed);

			panel->Shutdown();
			panel.reset();
			} // for each panel

			PobUi::SetWidgetFonts(PobUi::WidgetFonts());   // the faces die with the context
			ImGui_ImplOpenGL3_Shutdown();
			ImGui_ImplGlfw_Shutdown();
			ImGui::DestroyContext();
			glfwDestroyWindow(win);
		}
		glfwTerminate();
	}

	const int ran = rep.checks;
	rep.check("P7 the suite actually ran", ran >= 28, std::to_string(ran) + " checks");

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
