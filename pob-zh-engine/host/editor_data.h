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
};

// Everything loaded for one game+locale.
struct EditorModel {
	std::wstring dataDir;                 // full path of the Data/{game}/{locale} dir
	bool localeExists = false;            // false when the locale directory is absent
	std::vector<EditorFile> files;
	std::vector<EditorEntry> entries;     // flattened, in file/key order
};

// One untranslated string scanned from translate_misses.log (already filtered
// to those NOT present in any dictionary key).
struct MissEntry {
	std::string text;        // the untranslated string
	bool reverse = false;    // true: REV| line (Chinese→English reverse failure)
};

// Load all dictionary files for a game/locale. localeExists is false (and the
// model otherwise empty) when the directory does not exist.
EditorModel LoadModel(const std::wstring& exeDir, const std::string& game, const std::string& locale);

// Find a file index by name (e.g. "ui.json"); -1 if absent.
int FindFileIdx(const EditorModel& model, const std::string& name);

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
