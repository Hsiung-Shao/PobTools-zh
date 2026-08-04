// Translation-editor data layer: load / edit / save the dictionary JSON files
// under <exeDir>\Data\{game}\{locale}\ and scan translate_misses.log for gaps.
//
// These are the SAME files the engine loads at launch (translation_manager.cpp),
// so edits here take effect the next time POB starts — no separate sync step.
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
};

// One dictionary JSON file (e.g. ui.json), with its parsed document kept for
// round-trip fidelity (key order, source_files, extra structured fields).
struct EditorFile {
	std::string name;               // e.g. "ui.json"
	std::wstring path;              // full path on disk
	nlohmann::ordered_json doc;     // the whole parsed document
	bool dirty = false;
	// Line ending the file had on disk. nlohmann's dump() only ever emits '\n',
	// so without restoring this a single save rewrites the whole file — two poe1
	// dictionaries were silently converted to LF this way before it was noticed.
	bool crlf = false;
	// Rank in meta.json's load_order; -1 = not listed, i.e. the engine never
	// loads it. The dictionary is merged in that order and the LAST file to
	// define a key wins, so an edit to a lower-ranked file has no effect.
	int order = -1;
};

// Everything loaded for one game+locale.
struct EditorModel {
	std::wstring dataDir;                 // full path of the Data/{game}/{locale} dir
	bool localeExists = false;            // false when the locale directory is absent
	std::vector<EditorFile> files;
	std::vector<EditorEntry> entries;     // flattened, in file/key order
	// meta.json's load_order, verbatim. meta.json itself is not a dictionary file
	// so it never appears in `files`, but the engine's merge order lives there and
	// the editor is useless without it: poe1 and poe2 order the SAME files
	// differently, so "which copy wins" cannot be guessed from the file name.
	std::vector<std::string> loadOrder;
};

// One untranslated string scanned from translate_misses.log (already filtered
// to those NOT present in any dictionary key).
struct MissEntry {
	std::string text;        // the untranslated string
	bool reverse = false;    // true: REV| line (Chinese→English reverse failure)
};

// Load all dictionary files for one locale of one dictionary set.
//
// `slotRoot` is the folder that directly CONTAINS the <locale> sub-folders, with
// a trailing backslash: <exeDir>Data\poe1\ for the shipped PoE1 dictionaries, or
// whatever external folder the user configured for that slot (ResolveDictDir in
// launcher_config.h). It is NOT the exe directory and NOT a Data root -- the
// editor must write to the same folder the engine reads, otherwise an edit
// appears to do nothing. localeExists is false (model otherwise empty) when the
// directory is absent.
EditorModel LoadModel(const std::wstring& slotRoot, const std::string& locale);

// Find a file index by name (e.g. "ui.json"); -1 if absent.
int FindFileIdx(const EditorModel& model, const std::string& name);

// Is this entry too long, or too multi-line, to edit in a single-line table cell?
// The entries table is virtualised with ImGuiListClipper, which requires uniform
// row heights -- so a taller box cannot live in the cell and these rows get an
// expand button plus a modal editor instead. Here rather than in the UI so the
// rule is a thing that can be asserted, not something only a screenshot shows.
bool NeedsExpandedEditor(const EditorEntry& e);

// Locale sub-directories present under `slotRoot` (same convention as LoadModel).
// Falls back to {"zh-rTW"} when the directory is missing, so callers always have
// something to show.
std::vector<std::string> ListLocales(const std::wstring& slotRoot);

// File indices sorted by engine load order (earliest first); files absent from
// load_order come last. Use this for any "which file should I write to" UI —
// alphabetical order is meaningless to the engine.
std::vector<int> FileIdxInLoadOrder(const EditorModel& model);

// Every file index that defines `key`, in load order.
std::vector<int> FilesContaining(const EditorModel& model, const std::string& key);

// The file whose copy of `key` the engine actually uses (highest load order),
// or -1 when no file defines it.
int WinnerFileIdx(const EditorModel& model, const std::string& key);

// Update an existing entry's value, or append a new entry, in the given file.
// Marks the owning file dirty. Returns the entry index in model.entries.
size_t SetEntry(EditorModel& model, int fileIdx, const std::string& key, const std::string& value);

// Persist one file's edits (backup .bak, then atomic replace). false on error.
bool SaveFile(EditorFile& file, std::string* err);

// Save every dirty file. Returns count saved; *err collects the first failure.
int SaveAll(EditorModel& model, std::string* err);

// Number of files with unsaved edits.
int DirtyCount(const EditorModel& model);

// Read <exeDir>\translate_misses.log and return strings absent from all
// dictionary keys. *logFound is false when the log file does not exist.
std::vector<MissEntry> ScanMisses(const std::wstring& exeDir, const EditorModel& model, bool* logFound);
