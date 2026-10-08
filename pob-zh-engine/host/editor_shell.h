// PobTools filter-editor app shell.
//
// The window / GL context / theme / fonts / main loop live in whichever host
// draws the panel (filter_editor.cpp). Everything the three pages read or mutate
// lives in EditorShell, and each page is one draw function. This keeps the
// "display Chinese, output English" invariant: pages only ever mutate the model
// through the existing English-token paths (FilterDocumentEditor / FilterSet*).
//
// Page-local UI state (the drop preview's canvas, the batch dialog's draft, the
// sound page's dialogs, a card's text input) used to be function statics, which
// two filter-editor tabs would have shared. It is held here instead, behind
// shared_ptrs to types each page defines privately.
#pragma once

#include "filter_data.h"
#include "filter_doc_editor.h"
#include "filter_file_watch.h"
#include "sound_library_service.h"
#include "filter_i18n.h"
#include "item_library.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// The three pages of the header's segmented control.
enum class Section {
	FilterEdit,   // 規則 — block list + per-block cards + add-column
	DropPreview,  // 掉落預覽 — evaluate + render in-game-style labels
	Sounds,       // 音效
};

// One cached row of the block list (1:1 with FilterFile::blocks). Rebuilt as a
// whole when FilterDocumentEditor::structureVersion changes — translation and
// summaries must never run inside the list clipper loop.
struct BlockListRow {
	std::string label;      // display text (NeverSink marker or summary)
	std::string haystack;   // lower-cased label + English + Chinese summary, for search
	int group = 0;          // index into EditorShell::groupNames
	bool custom = false;    // inside the PobTools custom zone
	bool hide = false;      // refreshed every frame (EdRefreshRowStates)
	BlockChange change = BlockChange::Same;   // against the baseline, refreshed every frame
};

// The list's filter chips: 全部 / 顯示 / 隱藏 / 已修改 / 自訂.
enum class RuleChip { All, Shown, Hidden, Changed, Custom };
struct ChipCounts { int all = 0, shown = 0, hidden = 0, changed = 0, custom = 0; };
// Pure: the counts over a row list, and whether a row passes a chip. "Changed"
// is modified or added (unsaved against the baseline).
ChipCounts CountChips(const std::vector<BlockListRow>& rows);
bool RowPassesChip(RuleChip chip, const BlockListRow& row);

// Recently used colours (RGBA packed as 0xRRGGBBAA), newest first, at most 8.
constexpr int kRecentColorMax = 8;
void PushRecentColor(std::vector<std::uint32_t>& list, const int rgba[4]);
std::string EncodeRecentColors(const std::vector<std::uint32_t>& list);
std::vector<std::uint32_t> DecodeRecentColors(const std::string& text);

// One line of the clipped block list: a group heading or a rule.
struct BlockListEntry {
	int block = -1;         // >= 0: a rule row; -1: a group heading
	int group = 0;          // the heading's group (index into groupNames)
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
	OpenFilter,       // 開啟其他檔案…
	SaveFilterAs,     // 另存新檔…
	ExportCustom,     // 匯出自訂規則…  (uses pendingExportSel)
	ImportCustom,     // 匯入自訂規則…
	SoundFolder,      // 音效資料夾 → 瀏覽…
};

// What the "save first?" dialog was asked for.
enum class EdPendingAction {
	None,
	OpenPath,         // switch to pendingPath
	Reload,           // reload the open file from disk
};

struct PreviewUiState;  // filter_preview.cpp
struct BatchUiState;    // filter_batch.cpp
struct SoundsUiState;   // section_sounds.cpp
struct CardUiState;     // filter_card_ui.cpp

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

	// Test run (POBTOOLS_TOOL_SHOT / POBTOOLS_FILTER_STATE): the file list comes
	// from the test folder, never from Documents, and nothing is persisted.
	bool testMode = false;
	std::wstring testDir;            // POBTOOLS_FILTER_TEST_DIR or <exe>\filter_test\

	// Set by a widget, acted on after the frame. See EdDialog.
	EdDialog pendingDialog = EdDialog::None;
	std::vector<int> pendingExportSel;

	// "save first?" (switching files / reloading with unsaved edits)
	EdPendingAction pendingAction = EdPendingAction::None;
	std::wstring pendingPath;
	bool askSaveFirst = false;       // open the dialog next frame
	bool uiAskOpen = false;          // BeginDialog's open flag for it
	bool uiOpenMore = false;         // the header's ⋯ menu, opened next frame

	// discovered .filter files + their scan dir (for the open dialog)
	std::vector<FilterListEntry> fileList;
	std::wstring initialDir;

	// the open filter
	FilterFile model;
	ExternalChangeWatch watch;   // the open file changed on disk by someone else
	std::vector<std::uint32_t> recentColors;   // newest first (pob-zh.ini FilterRecentColors)
	bool loaded = false;
	std::string status;          // last action message (also shown as a toast)
	std::wstring loadFailedPath; // non-empty: the last open failed (Banner + 重試)

	Section section = Section::FilterEdit;

	// 規則 — structural mutations go through doc; selection survives them
	// via the anchor (blocks[] indices are not stable across rebuilds).
	FilterDocumentEditor doc;
	BlockAnchor selAnchor;
	std::vector<BlockListRow> rows;        // 1:1 with model.blocks (see BlockListRow)
	std::vector<std::string> groupNames;   // list headings (自訂規則 / 通貨 / ...)
	std::vector<int> visRows;              // search-filtered indices into rows/blocks
	std::vector<BlockListEntry> visList;   // visRows with group headings interleaved
	unsigned rowsVersion = 0;              // doc.structureVersion the caches were built at
	bool batchMode = false;                // 批量修改: multi-select in the block list
	std::vector<char> batchSel;            // per-block tick (cleared on rebuild)
	bool wantBatchDialog = false;          // open the batch dialog next frame
	bool scrollToSel = false;              // bring the selection into view in the list
	bool wantDeleteDialog = false;         // open "delete this custom rule?" next frame

	// 音效 (lazy-initialised on first page draw)
	SoundLibraryService sounds;
	bool soundsInit = false;

	int selectedBlock = -1;          // resolved from selAnchor after every rebuild
	std::string search, searchLower;
	RuleChip chip = RuleChip::All;
	ChipCounts chipCounts;           // refreshed every frame (EdRefreshRowStates)
	unsigned rowStateSig = 0;        // hide/change flags the visible list was built from
	char searchBuf[256] = "";
	char addSearchBuf[128] = "";     // the add-column's search box

	// display-only services (output stays English)
	FilterI18n i18n;
	ItemLibrary library;     // whole-game catalog (Chinese input -> English token)

	// legacy 設定 kept in pob-zh.ini [PobTools] (no UI in the current design)
	std::string league = "Mirage";
	bool economyEnabled = false;

	// page-local UI state (see the file header)
	std::shared_ptr<PreviewUiState> previewUi;
	std::shared_ptr<BatchUiState> batchUi;
	std::shared_ptr<SoundsUiState> soundsUi;
	std::shared_ptr<CardUiState> cardUi;

	// Open a .filter; force=true bypasses the unsaved-changes guard (used by
	// reload and by the "save first?" dialog once answered). Returns true when
	// the file was opened.
	bool OpenByPath(const std::wstring& path, bool force);
	// Open, or ask "save first?" when there are unsaved edits.
	void RequestOpen(const std::wstring& path);
	// Reload from disk, asking first when there are unsaved edits.
	void RequestReload();
	// Save to model.path / to a new path. Reports with a toast; false on failure.
	bool Save();
	bool SaveAs(const std::wstring& path);
	// A message for the user: kept in status and shown as a toast.
	void Notify(const std::string& text, bool error = false);
	// Number of unsaved changes, as the header and the dialogs count them:
	// modified + added + removed blocks against the baseline. A file whose
	// edits were all undone by hand counts 0 and is marked clean again.
	int UnsavedCount();
};

// --- shell chrome ---
// The one-row tool header: icon + name + PoE1 badge | file Select | page
// Segmented | 批量修改 | ⋯ | 儲存. Opens its own menu and dialogs at the
// panel's top level.
void DrawEditorHeader(EditorShell& s);
// Banners under the header (read failure, external change).
void DrawEditorBanners(EditorShell& s);
// The footer: save hint left, counts right.
void DrawStatusBar(EditorShell& s);
// The "save first?" dialog for switching / reloading. `closing` is the close
// guard's variant (its answer is returned instead of acted on).
enum class EdSaveAnswer { None, Cancel, Discard, Save };
EdSaveAnswer DrawSaveFirstDialog(EditorShell& s, const char* popupId, bool* open, bool closing);

// --- section content ---
void DrawFilterEditSection(EditorShell& s);  // 規則 (three-pane block editor)
void DrawDropPreviewSection(EditorShell& s); // 掉落預覽 (filter_preview.cpp)
void DrawSoundsSection(EditorShell& s);      // 音效
// Test aid: open the batch-rename plan (section_sounds.cpp).
void SoundsTestOpenPlan(EditorShell& s);
void SoundsTestPlaying(EditorShell& s, const std::wstring& name);

// Set a block's Show/Hide verb (header keyword + b.hide), marking dirty. No-op if
// already in that state.
void SetBlockHide(EditorShell& s, FilterBlock& b, bool hide);

// Rebuild rows/visRows/selection caches from the model (section_filteredit.cpp).
// Call when rowsVersion != doc.structureVersion() outside the 規則 page.
void EdRebuildRows(EditorShell& s);
// Rebuild only the search-filtered list (search text / filter changed).
void EdRebuildVisRows(EditorShell& s);
// Per frame: each row's show/hide and baseline state, the chip counts, and the
// visible list when a chip that depends on them is active.
void EdRefreshRowStates(EditorShell& s);

// Fill fileList: the test folder in a test run, Documents otherwise.
void EdRefreshFileList(EditorShell& s);

// Is block bi inside the custom zone?
bool EdBlockIsCustom(const EditorShell& s, int bi);

// Open whatever dialog the last frame asked for, and apply the result. Called
// once per frame AFTER the frame has been presented -- never from inside one.
void EdRunDeferredDialogs(EditorShell& s);

// --- settings persistence (pob-zh.ini [PobTools]) ---
void LoadEditorSettings(EditorShell& s);
void SaveEditorSettings(EditorShell& s);

// --- shared drawing helpers (section_filteredit.cpp) ---
// A filter colour swatch: `fill` (RGBA) with a 2 px `edge`; size in design px.
// Returns true on click when `clickable`.
bool EdSwatch(const char* id, const unsigned char fill[4], const unsigned char edge[4], float designPx,
              bool clickable, bool hasFill = true, bool hasEdge = true);
// The game-style label preview box (the 掉落標籤): beam, label in the block's
// colours at its font size, minimap / sound notes. `height` in design px.
struct EdLabelStyle {
	unsigned char text[4] = { 200, 200, 200, 255 };
	unsigned char border[4] = { 0, 0, 0, 0 };
	unsigned char back[4] = { 0, 0, 0, 180 };
	bool hasBorder = false;
	int fontSize = 32;
	std::string beam;        // PlayEffect colour token, "" = none
	std::string minimapNote; // "大 · 紅 · 星形", "" = none
	std::string soundNote;   // "內建 6 號 · 音量 300", "" = none
};
EdLabelStyle EdBlockStyle(const EditorShell& s, int blockIdx);
std::string EdBlockItemName(const EditorShell& s, int blockIdx);
void EdDrawLabelBox(const char* id, const EdLabelStyle& st, const std::string& name, float designHeight);
// The game's beam / minimap colour tokens as swatch colours.
std::uint32_t EdEffectColor(const std::string& token);
