#include "filter_editor.h"
#include "editor_shell.h"
#include "editor_util.h"
#include "filter_data.h"     // ListFilters / Poe1FilterDirs
#include "filter_parser.h"
#include "filter_card_ui.h"
#include "filter_batch.h"
#include "filter_preview.h"
#include "custom_rules_io.h"
#include "paste_fixtures.h"
#include "error_log.h"
#include "tool_panel.h"
#include "tool_window.h"
#include "ui_theme.h"
#include "ui_widgets.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define IMGUI_DEFINE_MATH_OPERATORS
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

// ---- filter editor, as a panel ---------------------------------------------
//
// The window / GL context / font atlas / main loop live in whichever host is
// drawing this (RunToolWindow for its own window, the launcher's tab body when
// embedded). What is left here is the content, the unsaved-changes guard and
// the screenshot test states.

void SoundsTestOpenPlan(EditorShell& s);          // section_sounds.cpp
void CardTestOpenPop(EditorShell& s, int which);  // filter_card_ui.cpp
void BatchTestPreset(EditorShell& s);             // filter_batch.cpp
void CardTestInput(EditorShell& s, const char* text);  // filter_card_ui.cpp

namespace {

std::wstring EnvW(const wchar_t* name)
{
	wchar_t buf[MAX_PATH] = L"";
	const DWORD n = GetEnvironmentVariableW(name, buf, MAX_PATH);
	return (n > 0 && n < MAX_PATH) ? std::wstring(buf, n) : std::wstring();
}

} // namespace

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

		// Test aid (POBTOOLS_FILTER_STATE, with POBTOOLS_TOOL_SHOT): a known state
		// for a hidden-window screenshot. Either variable makes this a test run:
		// the files come from the test folder (never Documents) and nothing is
		// written -- no .filter, no pob-zh.ini, no sound rules.
		{
			const std::wstring st = EnvW(L"POBTOOLS_FILTER_STATE");
			for (wchar_t c : st) testState_ += (char)(c < 128 ? c : '?');
			shell_.testMode = !testState_.empty() || !EnvW(L"POBTOOLS_TOOL_SHOT").empty();
			shell_.testDir = EnvW(L"POBTOOLS_FILTER_TEST_DIR");
			if (shell_.testDir.empty()) shell_.testDir = host.exeDir + L"filter_test\\";
			if (shell_.testDir.back() != L'\\') shell_.testDir += L'\\';
		}
		EdRefreshFileList(shell_);
		if (shell_.testMode) {
			shell_.initialDir = shell_.testDir;
			shell_.sounds.InitForTest(host.exeDir, shell_.testDir.substr(0, shell_.testDir.size() - 1));
			shell_.soundsInit = true;
		} else {
			std::vector<std::wstring> scanDirs = Poe1FilterDirs();
			shell_.initialDir = scanDirs.empty() ? std::wstring() : scanDirs.front();
		}
		shell_.i18n.Load(host.exeDir, EdNarrow(host.locale)); // Chinese item names (display only)
		shell_.library.Load(host.exeDir, shell_.i18n);        // whole-game catalog (zh -> en token)
		LoadEditorSettings(shell_);                           // ini settings (league etc.)
		return true;
	}

	void Frame() override
	{
		// The host's font-size zoom can change while the panel is open (the
		// launcher's slider; a tool window follows the ini): read it per frame.
		shell_.scale = host_->scale;
		if (!testState_.empty()) applyTestState();

		DrawEditorHeader(shell_);
		if (!shell_.cjkOk) {
			ImGui::TextColored(PobUi::TokV4(PobUi::Tok::Danger),
				"[!] CJK font atlas not loaded (Fonts\\FZ_ZY.ttf). Chinese cannot display.");
		}
		DrawEditorBanners(shell_);

		// page content above a one-line footer. Heights are relative, so this
		// fits a tab's content area exactly as it fits a window.
		const PobUi::WidgetFonts& wf = PobUi::Fonts();
		const float footH = std::floor((wf.small ? wf.small->FontSize : ImGui::GetFontSize()) + PobUi::D(14.0f));
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
		ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
		ImGui::BeginChild("##fecontent", ImVec2(0, -footH), false,
		                  ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
		ImGui::PopStyleColor();
		ImGui::PopStyleVar();
		switch (shell_.section) {
			case Section::FilterEdit:  DrawFilterEditSection(shell_); break;
			case Section::DropPreview: DrawDropPreviewSection(shell_); break;
			case Section::Sounds:      DrawSoundsSection(shell_); break;
		}
		ImGui::EndChild();
		DrawStatusBar(shell_);

		// The close guard: the same "save first?" dialog as switching files.
		// Opened from here rather than from RequestClose(), because OpenPopup has
		// to happen inside the frame that will draw it.
		if (close_ == ToolCloseState::Asking && !closeShown_) {
			closeOpen_ = true;
			closeShown_ = true;
		}
		const EdSaveAnswer a = DrawSaveFirstDialog(shell_, u8"##feclose", &closeOpen_, true);
		if (a == EdSaveAnswer::Save) {
			// A failed save keeps the window open: closing anyway would look exactly
			// like a successful save, and the edits would be gone with it.
			if (shell_.Save()) {
				close_ = ToolCloseState::Closed;
			} else {
				PobLog::Error("save", u8"過濾器「儲存」（關閉前）失敗：" + shell_.status);
				close_ = ToolCloseState::Cancelled;
			}
		} else if (a == EdSaveAnswer::Discard) {
			close_ = ToolCloseState::Closed;
		} else if (a == EdSaveAnswer::Cancel) {
			// Cancelled, not Open: whoever asked (a tab's X, or the launcher closing
			// every tab in turn) has to know the answer was no and give up, rather
			// than ask again next frame.
			close_ = ToolCloseState::Cancelled;
		}
	}

	void RunDeferred() override { EdRunDeferredDialogs(shell_); }

	ToolCloseState RequestClose() override
	{
		if (close_ == ToolCloseState::Open || close_ == ToolCloseState::Cancelled) {
			close_ = shell_.model.dirty ? ToolCloseState::Asking : ToolCloseState::Closed;
			closeShown_ = false;
		}
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
	// ---- POBTOOLS_FILTER_STATE ----------------------------------------------
	// rules | detail-color | detail-sound | detail-minimap | batch | preview |
	// preview-import | sounds | soundsplan | empty | unsaved | delete | readfail |
	// saved | close (phase 2 adds extchange)
	void applyTestState()
	{
		const int f = testFrame_++;
		const std::string& st = testState_;
		if (f == 0) {
			if (st == "empty") return;
			if (st == "readfail") {
				shell_.loadFailedPath = shell_.testDir + L"NeverSink 3-STRICT (missing).filter";
				return;
			}
			if (shell_.fileList.empty()) return;
			// the first test file (by name)
			shell_.OpenByPath(shell_.fileList.front().path, true);
			if (!shell_.loaded) return;
			seedEdits();
			if (st == "preview" || st == "preview-import") shell_.section = Section::DropPreview;
			if (st == "sounds" || st == "soundsplan") {
				shell_.section = Section::Sounds;
				// in memory only: a test run's sound service writes nothing
				if (shell_.sounds.rules().empty()) {
					NamingRule r1;
					r1.name = u8"高價通貨 · 神聖";
					r1.match = "divine";
					r1.rename = "top-{n}.{ext}";
					NamingRule r2 = r1;
					r2.name = u8"高價通貨 · 鏡子";
					r2.match = "mirror";
					NamingRule r3;
					r3.name = u8"地圖";
					r3.match = "map";
					r3.rename = "map-{n}.{ext}";
					r3.enabled = false;
					shell_.sounds.rules() = { r1, r2, r3 };
				}
			}
			if (st == "batch") {
				shell_.batchMode = true;
				shell_.scrollToSel = false;
				setSearch(u8"通貨");
			}
			return;
		}
		if (f == 2) {
			if (st == "detail-color") {
				// in memory only (a test run never writes the ini)
				shell_.recentColors = { 0xffffffffu, 0xff0000ffu, 0xd6b56affu, 0x0000ffffu };
				CardTestOpenPop(shell_, 1);
			}
			if (st == "detail-sound") CardTestOpenPop(shell_, 4);
			if (st == "detail-basetype") CardTestInput(shell_, u8"混沌");
			if (st == "detail-minimap") CardTestOpenPop(shell_, 2);
			if (st == "batch") {
				int n = 0;
				for (int bi : shell_.visRows) {
					if (n >= 4) break;
					if (bi < (int)shell_.batchSel.size()) { shell_.batchSel[bi] = 1; n++; }
				}
				BatchTestPreset(shell_);
			}
			if (st == "preview") PreviewTestDrops(shell_, 7u, 0);
			if (st == "preview-import") PreviewTestImport(shell_, kFxBoots);
			if (st == "soundsplan") SoundsTestOpenPlan(shell_);
			if (st == "sounds") SoundsTestPlaying(shell_, L"2currency.mp3");
			if (st == "extchange") {
				// as if another program had rewritten the file: the watch's
				// recorded stamp no longer matches (nothing on disk is touched)
				shell_.watch.MarkChangedForTest();
			}
			if (st == "rules-changed") {
				shell_.chip = RuleChip::Changed;
				shell_.search.clear();
				shell_.searchLower.clear();
				shell_.searchBuf[0] = 0;
				EdRebuildVisRows(shell_);
			}
			if (st == "unsaved") {
				shell_.pendingAction = EdPendingAction::OpenPath;
				shell_.pendingPath = shell_.fileList.back().path;
				shell_.askSaveFirst = true;
			}
			if (st == "delete") {
				for (int i = 0; i < (int)shell_.rows.size(); i++)
					if (shell_.rows[i].custom) {
						shell_.selectedBlock = i;
						shell_.selAnchor = shell_.doc.CaptureAnchor(i);
						break;
					}
				shell_.wantDeleteDialog = true;
			}
			if (st == "saved")
				PobUi::ShowToast(u8"已儲存。到遊戲「選項 > 遊戲 > UI」重新選一次過濾器才會生效", PobUi::Tone::Ok);
			if (st == "close") {
				close_ = ToolCloseState::Asking;
				closeShown_ = false;
			}
		}
	}

	void setSearch(const char* q)
	{
		std::snprintf(shell_.searchBuf, sizeof(shell_.searchBuf), "%s", q);
		shell_.search = q;
		shell_.searchLower = EdToLowerAscii(shell_.search);
		EdRebuildRows(shell_);
	}

	// Edits every loaded test state starts from (in memory only, never saved):
	// a custom rule, and on the NeverSink block holding Divine Orb a changed
	// condition, a disabled one and a changed colour.
	void seedEdits()
	{
		FilterDocumentEditor& doc = shell_.doc;
		// the NeverSink block holding Divine Orb
		int target = -1;
		for (int i = 0; i < (int)shell_.model.blocks.size() && target < 0; i++) {
			const FilterBlock& b = shell_.model.blocks[i];
			for (int li : b.lineIdx) {
				const FilterLine& ln = shell_.model.lines[li];
				if (ln.kind == FilterLineKind::Condition && ln.keyword == "BaseType") {
					for (const FilterToken& t : ln.values)
						if (t.text == "Divine Orb") { target = i; break; }
				}
				if (target >= 0) break;
			}
		}
		// Two conditions the design's example has, as if the file had them:
		// added, then taken as the baseline (in memory -- nothing is saved).
		if (target >= 0) {
			doc.InsertLine(target, "StackSize", ">=", { FilterToken{ "3", false } });
			doc.InsertLine(target, "AreaLevel", ">=", { FilterToken{ "68", false } });
			doc.CaptureBaseline();
		}
		const BlockAnchor targetAnchor = target >= 0 ? doc.CaptureAnchor(target) : BlockAnchor{};
		// a custom rule: 神聖石 stacks of 5+, with a custom sound (新增)
		CustomZone z = EnsureCustomZone(doc);
		if (z.present()) {
			const int nb = doc.CreateBlockAtLine(z.endLine, false, u8"PobTools custom rule");
			if (nb >= 0) {
				doc.InsertLine(nb, "BaseType", "==", { FilterToken{ "Divine Orb", true } });
				doc.InsertLine(nb, "StackSize", ">=", { FilterToken{ "5", false } });
				doc.InsertLine(nb, "SetFontSize", "", { FilterToken{ "45", false } });
				doc.InsertLine(nb, "SetTextColor", "", { FilterToken{ "255", false }, FilterToken{ "0", false }, FilterToken{ "0", false }, FilterToken{ "255", false } });
				doc.InsertLine(nb, "SetBorderColor", "", { FilterToken{ "255", false }, FilterToken{ "0", false }, FilterToken{ "0", false }, FilterToken{ "255", false } });
				doc.InsertLine(nb, "SetBackgroundColor", "", { FilterToken{ "255", false }, FilterToken{ "255", false }, FilterToken{ "255", false }, FilterToken{ "255", false } });
				doc.InsertLine(nb, "CustomAlertSound", "", { FilterToken{ "2currency.mp3", true }, FilterToken{ "300", false } });
			}
		}
		// the edits on the Divine Orb block: >= 3 becomes >= 1, AreaLevel disabled
		// the custom rule went in above it: find the block again by its anchor
		if (target >= 0) target = doc.ResolveAnchor(targetAnchor);
		if (target >= 0) {
			const int al = doc.FindLine(target, "AreaLevel");
			if (al >= 0) doc.CommentOutLine(al);
			const int ss = doc.FindLine(target, "StackSize");
			if (ss >= 0) FilterSetValueInt(shell_.model.lines[ss], 0, 1);
			shell_.selectedBlock = target;
			shell_.selAnchor = doc.CaptureAnchor(target);
			shell_.scrollToSel = true;
		}
		setSearch(u8"神聖");
		shell_.model.dirty = true;
	}

	const ToolPanelHost* host_ = nullptr;
	EditorShell shell_;
	ToolCloseState close_ = ToolCloseState::Open;
	bool closeOpen_ = false;    // BeginDialog's open flag for the close guard
	bool closeShown_ = false;   // the guard was raised for this request
	std::string testState_;
	int testFrame_ = 0;
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
	desc.defW = 1280;
	desc.defH = 860;
	RunToolWindow(panel, desc, exeDir, game, locale);
}
