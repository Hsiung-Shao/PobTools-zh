// PobTools filter editor: undo / redo for every kind of edit.
//
// Edits happen in many places (cards, batch, look paste, sound reference
// replace, the doc editor's structural mutations) and all of them end up in
// FilterFile::lines. So instead of every call site recording a command, the
// editor calls Capture() once per frame when no widget is active: the lines are
// compared with a shadow copy taken at the previous step, and only the changed
// range (from the first differing line to the last) is stored. Lines keep their
// uid through undo/redo, so the baseline's 已修改 markers stay right.
//
// Pure data: no ImGui, no file access. The caller rebuilds blocks afterwards.
#pragma once

#include "filter_model.h"
#include <cstddef>
#include <vector>

class FilterUndoHistory {
public:
	static constexpr size_t kMaxSteps = 100;

	// A freshly opened / reloaded file, or after a sound batch rename (the files
	// on disk cannot be undone): forget every step, shadow = these lines.
	void Reset(const std::vector<FilterLine>& lines);

	// Compare with the shadow; when anything differs, push one step (dropping
	// the redo side and, past kMaxSteps, the oldest step) and update the shadow.
	// True when a step was pushed.
	bool Capture(const std::vector<FilterLine>& lines);

	// Put the lines back one step. Returns the first line index of the changed
	// range (for re-selecting a rule), -1 when there is nothing to undo / redo.
	// The shadow follows, so the next Capture records nothing.
	int Undo(std::vector<FilterLine>& lines);
	int Redo(std::vector<FilterLine>& lines);

	// After a save: the lines' text is unchanged but their dirty flags were
	// cleared. Take them as the shadow without pushing a step (the line count
	// is the same, so every stored range stays valid).
	void Rebase(const std::vector<FilterLine>& lines) { shadow_ = lines; }

	bool CanUndo() const { return pos_ > 0; }
	bool CanRedo() const { return pos_ < steps_.size(); }
	size_t UndoCount() const { return pos_; }

private:
	struct Step {
		size_t start = 0;                 // first changed line
		std::vector<FilterLine> before;   // lines [start, start + before.size()) before the edit
		std::vector<FilterLine> after;    // the same range after the edit
	};
	std::vector<FilterLine> shadow_;
	std::vector<Step> steps_;
	size_t pos_ = 0;                      // steps_[0, pos_) are undoable
};

// Same line as far as the file and the baseline are concerned (uid, kind,
// dirty, and raw bytes or the structured fields when dirty).
bool FilterLineSame(const FilterLine& a, const FilterLine& b);
