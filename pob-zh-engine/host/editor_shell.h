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

// Cross-section editor state, owned by ShowFilterEditor for the window's lifetime.
struct EditorShell {
	// host-provided context
	std::wstring exeDir;
	std::wstring locale;
	float scale = 1.0f;
	bool cjkOk = false;

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

// --- settings persistence (pob-zh.ini [PobTools]) ---
void LoadEditorSettings(EditorShell& s);
void SaveEditorSettings(EditorShell& s);
