// Translation editor: the decisions behind the panel, kept out of the UI so they
// can be asserted headlessly (--trans-editor-selftest).
//
//   * which of the four add-entry notices applies (load_order, last file wins)
//   * the undo stack: edits, additions, filled misses and their withdrawal
//   * the three checks the multi-line editor runs before applying
//
// Everything here works on EditorModel (editor_data.h) and writes nothing to
// disk; saving stays SaveAll's job.
#pragma once

#include "editor_data.h"

#include <string>
#include <vector>

namespace TransEd {

// ---- add-entry notice ---------------------------------------------------------
// The dictionary is ONE flat map merged in meta.json's load_order, and the last
// file to define a key wins (translation_manager.cpp). So where a key is written
// decides whether it does anything at all.
enum class AddKind {
	Empty,          // no key typed
	Whitespace,     // the key starts or ends with whitespace (offer to trim)
	New,            // no file defines it
	TargetWins,     // other files define it, but the chosen file loads later
	OverwriteSame,  // the chosen file already defines it: its value is replaced
	Shadowed,       // a later-loading file also defines it: writing here does nothing
};
struct AddHint {
	AddKind kind = AddKind::Empty;
	int winner = -1;              // Shadowed: the file whose copy POB will use
	std::vector<int> owners;      // files defining the key, in load order
	std::string oldValue;         // OverwriteSame: the value being replaced
};
// `target` must be a file index; an unlisted target (order < 0) is never
// offered by the UI and is reported as Shadowed when a listed file has the key.
AddHint ClassifyAdd(const EditorModel& model, const std::string& key, int target);
// Leading / trailing spaces, tabs, CR, LF and U+3000 removed.
std::string TrimKey(const std::string& key);

// ---- undo -------------------------------------------------------------------
struct EntryState {
	bool exists = false;
	std::string value;
	bool operator==(const EntryState& o) const { return exists == o.exists && (!exists || value == o.value); }
};
struct EntryChange {
	int fileIdx = -1;
	std::string key;
	EntryState before, after;
};
// A missing-string row's "filled" mark, so undoing a fill puts the row back.
struct MissMark {
	int missIdx = -1;
	bool before = false, after = false;
};
struct UndoStep {
	std::vector<EntryChange> changes;
	std::vector<MissMark> marks;
	bool Empty() const { return changes.empty() && marks.empty(); }
};

EntryState StateOf(const EditorModel& model, int fileIdx, const std::string& key);
// Bring (fileIdx, key) to `s`. Returns true when model.entries changed shape
// (an entry was appended or removed): indices held elsewhere are stale then.
bool ApplyState(EditorModel& model, int fileIdx, const std::string& key, const EntryState& s);

class UndoStack {
public:
	void Push(UndoStep step);
	bool Empty() const { return steps_.empty(); }
	size_t Size() const { return steps_.size(); }
	void Clear() { steps_.clear(); }
	// Revert the newest step. `missFilled` (may be null) receives the marks.
	// *reshaped is set when model.entries changed shape. False when empty.
	bool Undo(EditorModel& model, std::vector<char>* missFilled, bool* reshaped);
	// Most steps kept; the oldest go first.
	static constexpr size_t kMax = 500;

private:
	std::vector<UndoStep> steps_;
};

// Set a value and record it (no step when nothing changes). Returns true when
// model.entries changed shape.
bool SetWithUndo(EditorModel& model, UndoStack& undo, int fileIdx, const std::string& key,
                 const std::string& value);

// ---- missing strings ----------------------------------------------------------
// Per-row state of the missing-strings view.
struct MissRow {
	int target = -1;          // file to write to
	std::string trans;        // what the user typed
	bool filled = false;      // written into the model (still unsaved)
	int filledFile = -1;      // where it went
	EntryState prev;          // that file's entry before the fill, for 撤回
};
// Fill row `i` (forward misses only): one undo step, entry + mark.
bool FillMiss(EditorModel& model, UndoStack& undo, const std::vector<MissEntry>& misses,
              std::vector<MissRow>& rows, int i, bool* reshaped);
// Fill every unfilled, typed, forward row as ONE step. Returns rows filled.
int FillAllMisses(EditorModel& model, UndoStack& undo, const std::vector<MissEntry>& misses,
                  std::vector<MissRow>& rows, bool* reshaped);
// Take a fill back (also a step: undoing it fills again).
bool WithdrawMiss(EditorModel& model, UndoStack& undo, const std::vector<MissEntry>& misses,
                  std::vector<MissRow>& rows, int i, bool* reshaped);
// Undo, with the marks landing in `rows`.
bool UndoInto(EditorModel& model, UndoStack& undo, std::vector<MissRow>& rows, bool* reshaped);

// ---- multi-line editor checks -------------------------------------------------
struct TextChecks {
	// Colour escapes (^0-^9, ^xRRGGBB): the translation carries the same ones as
	// the key. A key without any passes whatever the translation does -- giving a
	// translation its own colour is deliberate and supported by the engine.
	bool colourOk = true;
	int colourKey = 0, colourValue = 0;
	std::vector<std::string> colourMissing;   // in the key, not the translation
	// Line breaks: a real newline or the two characters \n (stats.json's spelling).
	bool linesOk = true;
	int linesKey = 1, linesValue = 1;
	// {N} / {N:fmt} placeholders, compared as a multiset of N.
	bool placeholdersOk = true;
	std::vector<std::string> placeholdersMissing, placeholdersExtra;   // "{0}"
	std::vector<std::string> placeholdersKey;                          // sorted, unique
	bool AllOk() const { return colourOk && linesOk && placeholdersOk; }
};
TextChecks CheckTranslation(const std::string& key, const std::string& value);
// The colour escapes in `s`, in order ("^7", "^xE05030"); same rules as the
// engine's colour_escape_len.
std::vector<std::string> ColourEscapes(const std::string& s);
int LineCount(const std::string& s);

// Rows the "only modified" filter keeps.
int CountModified(const EditorModel& model);

} // namespace TransEd

int RunTransEditorSelftest(const std::wstring& exeDir);
