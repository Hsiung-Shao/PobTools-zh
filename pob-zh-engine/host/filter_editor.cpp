#include "filter_editor.h"
#include "editor_shell.h"
#include "editor_util.h"
#include "filter_data.h"     // ListFilters / Poe1FilterDirs
#include "filter_parser.h"   // SaveFilter (close guard)
#include "tool_panel.h"
#include "tool_window.h"
#include "ui_theme.h"

#define IMGUI_DEFINE_MATH_OPERATORS
#include <imgui.h>

#include <string>
#include <vector>

// ---- filter editor, as a panel ---------------------------------------------
//
// The window / GL context / font atlas / main loop live in whichever host is
// drawing this (RunToolWindow for its own window, the launcher's tab body when
// embedded). What is left here is the content and the unsaved-changes guard.
//
// Almost all of the state was already in EditorShell, which is why this tool went
// first: it is the shape the others are being moved towards.

class FilterEditorPanel : public IToolPanel {
public:
	bool Init(const ToolPanelHost& host) override
	{
		host_ = &host;
		shell_.exeDir = host.exeDir;
		shell_.locale = host.locale;
		shell_.scale = host.scale;
		shell_.cjkOk = host.cjkOk;
		shell_.hostHwnd = host.hostHwnd;
		shell_.fileList = ListFilters();
		{
			std::vector<std::wstring> scanDirs = Poe1FilterDirs();
			shell_.initialDir = scanDirs.empty() ? std::wstring() : scanDirs.front();
		}
		shell_.i18n.Load(host.exeDir, EdNarrow(host.locale)); // Chinese item names (display only)
		shell_.library.Load(host.exeDir, shell_.i18n);        // whole-game catalog (zh -> en token)
		LoadEditorSettings(shell_);                           // legacy ini settings (league etc.)
		return true;
	}

	void Frame() override
	{
		DrawTopToolbar(shell_);
		ImGui::Separator();
		if (!shell_.cjkOk) {
			ImGui::TextColored(ImVec4(0.94f, 0.27f, 0.27f, 1.0f),
				"[!] CJK font atlas not loaded (Fonts\\FZ_ZY.ttf). Chinese cannot display.");
		}

		// nav (left) + content (right) above a one-line status bar. Heights are
		// relative, so this fits a tab's content area exactly as it fits a window.
		const float statusH = ImGui::GetFrameHeightWithSpacing();
		ImGui::BeginChild("##nav", ImVec2(176 * shell_.scale, -statusH), true);
		ImGui::TextDisabled(u8"編輯模式");
		ImGui::Separator();
		DrawLeftNav(shell_);
		ImGui::EndChild();
		ImGui::SameLine();
		ImGui::BeginChild("##content", ImVec2(0, -statusH), false);
		switch (shell_.section) {
			case Section::FilterEdit:  DrawFilterEditSection(shell_); break;
			case Section::DropPreview: DrawDropPreviewSection(shell_); break;
			case Section::Sounds:      DrawSoundsSection(shell_); break;
		}
		ImGui::EndChild();
		DrawStatusBar(shell_);

		// The prompt is opened from here rather than from RequestClose(), because
		// OpenPopup has to happen inside the frame that will draw it.
		if (close_ == ToolCloseState::Asking && !ImGui::IsPopupOpen(u8"未儲存變更"))
			ImGui::OpenPopup(u8"未儲存變更");
		if (ImGui::BeginPopupModal(u8"未儲存變更", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
			ImGui::TextUnformatted(u8"過濾器有尚未儲存的變更。");
			ImGui::Spacing();
			if (ImGui::Button(u8"儲存並關閉")) {
				std::string err;
				SaveFilter(shell_.model, &err);
				ImGui::CloseCurrentPopup();
				close_ = ToolCloseState::Closed;
			}
			ImGui::SameLine();
			if (ImGui::Button(u8"直接關閉")) {
				ImGui::CloseCurrentPopup();
				close_ = ToolCloseState::Closed;
			}
			ImGui::SameLine();
			if (ImGui::Button(u8"取消")) {
				ImGui::CloseCurrentPopup();
				// Cancelled, not Open: whoever asked (a tab's X, or the launcher
				// closing every tab in turn) has to know the answer was no and give up,
				// rather than ask again next frame.
				close_ = ToolCloseState::Cancelled;
			}
			ImGui::EndPopup();
		}
	}

	void RunDeferred() override { EdRunDeferredDialogs(shell_); }

	ToolCloseState RequestClose() override
	{
		if (close_ == ToolCloseState::Open || close_ == ToolCloseState::Cancelled)
			close_ = shell_.model.dirty ? ToolCloseState::Asking : ToolCloseState::Closed;
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
	const char* PanelId() const override { return "filter"; }

private:
	const ToolPanelHost* host_ = nullptr;
	EditorShell shell_;
	ToolCloseState close_ = ToolCloseState::Open;
};

IToolPanel* CreateFilterEditorPanel()
{
	return new FilterEditorPanel();
}

void ShowFilterEditor(const std::wstring& exeDir, const std::wstring& game, const std::wstring& locale)
{
	FilterEditorPanel panel;
	ToolWindowDesc desc;
	// "PobTools — 過濾器編輯器"
	desc.titleUtf8 = "PobTools \xe2\x80\x94 \xe9\x81\x8e\xe6\xbf\xbe\xe5\x99\xa8\xe7\xb7\xa8\xe8\xbc\xaf\xe5\x99\xa8";
	desc.defW = 1180;
	desc.defH = 740;
	RunToolWindow(panel, desc, exeDir, game, locale);
}
