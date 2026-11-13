// Translation-editor data layer: load / edit / save the dictionary JSON files under <exeDir>\Data\{game}\{locale}\
// (the SAME files the engine loads at launch) and scan translate_misses.log for gaps.
#pragma once

#include <string>
#include <vector>
#include <json.hpp> // nlohmann::ordered_json (deps/nlohmann)

// One editable translation row, flattened across all dictionary files.
struct EditorEntry {
	std::string key;          // English source string (the JSON entries key)
	std::string value;        // current translation (the field the user edits)
	bool structured = false;  // JSON value is an object; only "翻譯" is written back
	int fileIdx = -1;         // index into EditorModel::files
	// Differs from disk (value != orig, or added). Recomputed by RefreshEdited, so typing back to the original stops counting.
	bool edited = false;
	std::string orig;         // value at the last load / save
	bool added = false;       // created in this session, not on disk yet
};

// One dictionary JSON file (e.g. ui.json), with its parsed document kept for
// round-trip fidelity (key order, source_files, extra structured fields).
struct EditorFile {
	std::string name;               // e.g. "ui.json"
	std::wstring path;              // full path on disk
	nlohmann::ordered_json doc;     // the whole parsed document
	bool dirty = false;
	// Line ending the file had on disk; dump() only emits '\n', so without restoring it one save rewrites the whole file.
	bool crlf = false;
	// Indent width on disk: hand-kept dictionaries use 2, DB-generated ones (ggpk_*) use 4.
	int indent = 2;
	// Rank in meta.json's load_order; -1 = not listed (engine never loads it). The LAST file to define a key wins.
	int order = -1;
	// A key was taken out of `doc` (RemoveAddedEntry after a failed save); the file needs saving even with no edited entry.
	bool docChanged = false;
	// Id-keyed dictionaries (*_by_id.json): listed to read, never edited or saved (SetEntry returns SIZE_MAX,
	// SaveFile returns false + err). Set by LoadModel from the file name.
	bool readOnly = false;
};

// Everything loaded for one game+locale.
struct EditorModel {
	std::wstring dataDir;                 // full path of the Data/{game}/{locale} dir
	bool localeExists = false;            // false when the locale directory is absent
	std::vector<EditorFile> files;
	std::vector<EditorEntry> entries;     // flattened, in file/key order
	// meta.json's load_order, verbatim: poe1 and poe2 order the SAME files differently, so "which copy wins" cannot be guessed.
	std::vector<std::string> loadOrder;
	// meta.json's source_dicts, verbatim: source / id tables the engine reads (never merged), not in load_order.
	std::vector<std::string> sourceDicts;
};

// One untranslated string scanned from translate_misses.log (only those NOT in any dictionary key).
// The log (translation_manager.cpp log_translation_miss) has two families:
//   MISS|<text>      POB drew English text that no dictionary key matched.
//   REV |<text>      PASTE path: a Chinese game-text line that could not be turned back into English.
//   FLAVOUR |<text>  Not dictionary keys: writing one as a key would add a Chinese "English" entry
//   PROPERTY |<text> nothing looks up. Listed (reverse = true) so a translator can find the mismatching entry.
struct MissEntry {
	std::string text;        // the untranslated string
	bool reverse = false;    // true: a paste-side line (REV / FLAVOUR / PROPERTY)
	std::string tag;         // the log's own tag, trimmed ("MISS", "REV", ...)
};

// Load all dictionary files for one locale of one dictionary set.
//
// `slotRoot` is the folder that directly CONTAINS the <locale> sub-folders, with a trailing backslash
// (<exeDir>Data\poe1\ or the user's external folder, ResolveDictDir in launcher_config.h) -- not the exe dir
// or a Data root: the editor must write where the engine reads. localeExists is false when the directory is absent.
EditorModel LoadModel(const std::wstring& slotRoot, const std::string& locale);

// The note the file picker / badge shows for a file ("" = none). A file outside load_order may still be read
// (source / id dictionary), so the note must not claim it never is; a readOnly file says so (--editor-selftest P8).
std::string FileStatusNote(const EditorModel& model, int fileIdx);

// Find a file index by name (e.g. "ui.json"); -1 if absent.
int FindFileIdx(const EditorModel& model, const std::string& name);

// Too long / multi-line for a single-line table cell? The table uses ImGuiListClipper (uniform row heights), so
// such rows get an expand button plus a modal editor. Kept here so the rule can be asserted.
bool NeedsExpandedEditor(const EditorEntry& e);

// One-line form of a string for a table cell. ImGuiListClipper sizes the scroll range from one row's height, so ANY
// taller row makes the last entries unreachable. A line break comes back as the two characters \n (stats.json
// already spells breaks that way, ui.json uses a real 0x0A), making the two look alike. Full text stays on hover.
std::string OneLineForCell(const std::string& s);

// Locale sub-directories under `slotRoot` (as LoadModel); falls back to {"zh-rTW"} when the directory is missing.
std::vector<std::string> ListLocales(const std::wstring& slotRoot);

// File indices in engine load order (earliest first), unlisted files last; use for any "which file to write to" UI.
std::vector<int> FileIdxInLoadOrder(const EditorModel& model);

// Every file index that defines `key`, in load order.
std::vector<int> FilesContaining(const EditorModel& model, const std::string& key);

// The file whose copy of `key` the engine actually uses (highest load order),
// or -1 when no file defines it.
int WinnerFileIdx(const EditorModel& model, const std::string& key);

// Update an existing entry's value, or append a new one, in the given file; refreshes the edited / dirty flags. Returns the entry index.
size_t SetEntry(EditorModel& model, int fileIdx, const std::string& key, const std::string& value);

// Index of the entry (fileIdx, key) in model.entries, or -1.
long long FindEntry(const EditorModel& model, int fileIdx, const std::string& key);

// Remove an entry ADDED in this session (also out of the file's document, in case a failed save wrote it). On-disk
// entries are never removed (the editor has no delete). Indices after it shift.
bool RemoveAddedEntry(EditorModel& model, size_t entryIdx);

// edited = added || value != orig, then the owning file's dirty flag.
void RefreshEdited(EditorModel& model, size_t entryIdx);
// dirty = any entry of the file edited, or an entry was taken out of its document.
void RefreshFileDirty(EditorModel& model, int fileIdx);

// Persist one file's edits (backup .bak, then atomic replace). false on error.
bool SaveFile(EditorFile& file, std::string* err);

// Save every dirty file. Returns count saved; *err collects the first failure,
// *failedFiles (optional) the name of every file that did not save.
int SaveAll(EditorModel& model, std::string* err, std::vector<std::string>* failedFiles = nullptr);

// meta.json's display_name for one locale folder ("繁體中文"), empty if none.
std::string LocaleDisplayName(const std::wstring& slotRoot, const std::string& locale);

// Number of files with unsaved edits.
int DirtyCount(const EditorModel& model);
// Entries changed since the last save (DirtyCount counts FILES, not edits).
int DirtyEntryCount(const EditorModel& model);

// Read <exeDir>\translate_misses.log and return strings absent from all dictionary keys. *logFound is false when
// the log does not exist; *logged (optional) = distinct lines in the log (translated ones included); *logWrite
// (optional) = its last-write time as a FILETIME value (0 unknown).
std::vector<MissEntry> ScanMisses(const std::wstring& exeDir, const EditorModel& model, bool* logFound,
                                  int* logged = nullptr, unsigned long long* logWrite = nullptr);
