// PobTools filter-editor app shell.
//
// The window / GL context / theme / fonts / main loop / unsaved-changes guard stay
// in ShowFilterEditor (filter_editor.cpp). Everything the left-nav sections read or
// mutate lives in EditorShell, and each section is one draw function. This keeps the
// "display Chinese, output English" invariant: sections only ever mutate the model
// through the existing English-token paths (MoveItemToTier / FilterSet* / FilterAddValue).
#pragma once

#include "filter_data.h"
#include "filter_doc_editor.h"
#include "sound_library_service.h"
#include "filter_i18n.h"
#include "item_library.h"

#include <string>
#include <vector>

// Left-nav sections. The legacy sections (presets / quick-filter / tier-list /
// appearance / preview) are retired from the nav but their draw code is kept
// compiled until the second-stage cleanup.
enum class Section {
	FilterEdit,   // 過濾編輯 — block list + per-block cards + add-column
	DropPreview,  // 掉落預覽 — evaluate + render in-game-style labels
	Sounds,       // 音效管理
};

// One cached row of the block list (1:1 with FilterFile::blocks). Rebuilt as a
// whole when FilterDocumentEditor::structureVersion changes — translation and
// summaries must never run inside the list clipper loop.
struct BlockListRow {
	std::string label;      // display text (NeverSink marker or summary)
	std::string haystack;   // lower-cased label + English + Chinese summary, for search
	bool hide = false;
	bool dirty = false;
};

// A Win32 dialog the UI asked for but which has NOT been opened yet.
//
// Common dialogs run their own modal message loop, so opening one from inside a
// frame stops that frame half-drawn -- and when this editor is a launcher tab, it
// stops the whole launcher, including the code that keeps docked POB windows
// hidden. Recording the intent and opening the dialog after the frame has been
// presented (EdRunDeferredDialogs, called from the panel's RunDeferred) keeps the
// dialog out of the middle of a frame.
enum class EdDialog {
	None,
	OpenFilter,       // 開啟檔案… / 開啟 .filter 檔…
	SaveFilterAs,     // 另存為…
	ExportCustom,     // 保存自定義  (uses pendingExportSel)
	ImportCustom,     // 導入自定義
	SoundFolder,      // 音效資料夾 → 瀏覽…
};

// Cross-section editor state, owned by the panel for its lifetime.
struct EditorShell {
	// host-provided context
	std::wstring exeDir;
	std::wstring locale;
	float scale = 1.0f;
	bool cjkOk = false;
	// The container window, so a dialog is owned by it. Never GetActiveWindow():
	// that returns whatever is active on this thread at the moment of the call,
	// which is right today only by coincidence.
	void* hostHwnd = nullptr;

	// Set by a widget, acted on after the frame. See EdDialog.
	EdDialog pendingDialog = EdDialog::None;
	std::vector<int> pendingExportSel;

	// discovered .filter files + their scan dir (for the open dialog)
	std::vector<FilterListEntry> fileList;
	std::wstring initialDir;

	// the open filter
	FilterFile model;
	bool loaded = false;
	std::string status;          // last toolbar/file action message

	Section section = Section::FilterEdit;

	// 過濾編輯 — structural mutations go through doc; selection survives them
	// via the anchor (blocks[] indices are not stable across rebuilds).
	FilterDocumentEditor doc;
	BlockAnchor selAnchor;
	std::vector<BlockListRow> rows;  // 1:1 with model.blocks (see BlockListRow)
	std::vector<int> visRows;        // search-filtered indices into rows/blocks
	unsigned rowsVersion = 0;        // doc.structureVersion the caches were built at
	bool batchMode = false;          // 批量修改: multi-select in the block list
	std::vector<char> batchSel;      // per-block tick (cleared on rebuild)

	// 音效管理 (lazy-initialised on first section draw)
	SoundLibraryService sounds;
	bool soundsInit = false;

	int selectedBlock = -1;          // resolved from selAnchor after every rebuild
	std::string search, searchLower;

	// display-only services (output stays English)
	FilterI18n i18n;
	ItemLibrary library;     // whole-game catalog (Chinese input -> English token)

	// legacy 設定 kept in pob-zh.ini [PobTools] (no UI in the current design)
	std::string league = "Mirage";
	bool economyEnabled = false;

	// Open a .filter; force=true bypasses the unsaved-changes guard (used by reload).
	void OpenByPath(const std::wstring& path, bool force);
};

// --- shell chrome ---
void DrawTopToolbar(EditorShell& s);
void DrawLeftNav(EditorShell& s);
void DrawStatusBar(EditorShell& s);

// --- section content ---
void DrawFilterEditSection(EditorShell& s);  // 過濾編輯 (three-pane block editor)
void DrawDropPreviewSection(EditorShell& s); // 掉落預覽 (filter_preview.cpp)
void DrawSoundsSection(EditorShell& s);      // 音效管理

// Set a block's Show/Hide verb (header keyword + b.hide), marking dirty. No-op if
// already in that state. Shared by 預設 / 快速篩選 batch toggles.
void SetBlockHide(EditorShell& s, FilterBlock& b, bool hide);

// Rebuild rows/visRows/selection caches from the model (section_filteredit.cpp).
// Call when rowsVersion != doc.structureVersion() outside the 過濾編輯 section.
void EdRebuildRows(EditorShell& s);

// Open whatever dialog the last frame asked for, and apply the result. Called
// once per frame AFTER the frame has been presented -- never from inside one.
void EdRunDeferredDialogs(EditorShell& s);

// --- settings persistence (pob-zh.ini [PobTools]) ---
void LoadEditorSettings(EditorShell& s);
void SaveEditorSettings(EditorShell& s);
