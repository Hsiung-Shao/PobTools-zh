// PobTools filter document editor: the single entry point for every structural
// mutation of a FilterFile (insert / disable / restore / create / remove lines
// and blocks). Value-level edits (FilterSetValueInt / FilterSetColor / ...) stay
// on filter_parser and do not go through here.
//
// RULES for callers (the UI):
//  - Never keep a lineIdx or blockIdx across frames: any structural mutation
//    reorders FilterFile::blocks and shifts line indices. The only durable
//    reference is a BlockAnchor (CaptureAnchor / ResolveAnchor).
//  - Any cached view derived from the model (row lists, labels, checkboxes)
//    must be tagged with structureVersion() and rebuilt when it changes.
//  - "移除該項" in the UI is CommentOutLine (the line becomes a "#! ..." comment
//    and survives in the file); RemoveLine hard-deletes and is reserved for
//    import/replace flows and tests.
#pragma once

#include "filter_model.h"
#include <string>
#include <vector>

// Durable reference to a block across structural mutations: blocks[] indices are
// invalidated by every rebuild, so the anchor stores the header line's text (and
// the first condition line as tie-breaker for same-named headers) plus the old
// header line index for proximity resolution.
struct BlockAnchor {
	int headerLineIdx = -1;
	std::string headerRaw;       // serialized header line at capture time
	std::string firstCondRaw;    // serialized first Condition line ("" if none)
	bool valid() const { return headerLineIdx >= 0; }
};

class FilterDocumentEditor {
public:
	void Attach(FilterFile* f) { f_ = f; version_++; }
	FilterFile* file() { return f_; }
	const FilterFile* file() const { return f_; }

	// ---- queries ----
	// First live line in the block with exactly this keyword, -1 if none.
	// Alias grouping (PlayAlertSound vs PlayAlertSoundPositional) is the schema
	// layer's job: call once per alias.
	int FindLine(int blockIdx, const std::string& keyword) const;

	// Is this line a "#! <syntax>" disabled line this editor produced (or an
	// equivalent one)? Requires BOTH the "#!" prefix and that the remainder
	// parses to a known Condition / Action / BlockHeader, so ordinary comments
	// never qualify. On true, *parsedOut (if given) receives the parsed line.
	bool IsDisabledLine(int lineIdx, FilterLine* parsedOut = nullptr) const;

	// ---- line-level mutations (all set model.dirty; structural ones rebuild) --
	// Insert a structured new line into a block: a condition goes after the
	// block's last condition line, an action after the last action line (falling
	// back to end of block). Indent is copied from a neighbour line. Returns the
	// new line's index (valid until the next structural mutation).
	int InsertLine(int blockIdx, const std::string& keyword, const std::string& op,
	               const std::vector<FilterToken>& values);
	// Turn a live line into "<indent>#! <content>" (game ignores it, file keeps
	// it, RestoreLine can bring it back).
	void CommentOutLine(int lineIdx);
	// Reverse of CommentOutLine; false if the line is not a disabled line.
	bool RestoreLine(int lineIdx);
	// Hard-delete a line (import/replace flows and tests only — UI uses
	// CommentOutLine).
	void RemoveLine(int lineIdx);

	// ---- block-level mutations ----
	// Create an empty block (header + blank separator) before the given block
	// (insertBeforeBlockIdx == -1 or out of range appends at end of file).
	// headerComment (may be "") becomes the header's trailing comment.
	// Returns the new block's index.
	int CreateBlock(int insertBeforeBlockIdx, bool hide, const std::string& headerComment);
	// Same, but at an explicit line position (custom-zone insertion).
	int CreateBlockAtLine(int atLine, bool hide, const std::string& headerComment);
	// Verbatim copy of a block inserted right after it. Returns the new index.
	int DuplicateBlock(int blockIdx);
	// Disable a whole block (every syntax line becomes "#! ..."; the block
	// disappears from blocks[] since its header is now a comment).
	void CommentOutBlock(int blockIdx);
	// Hard-delete all lines of a block (custom-zone management).
	void RemoveBlock(int blockIdx);

	// ---- selection anchoring ----
	BlockAnchor CaptureAnchor(int blockIdx) const;
	int ResolveAnchor(const BlockAnchor& a) const;  // -1 when not found

	// ---- batching ----
	// Between BeginBatch/EndBatch, rebuilds are deferred and blocks[] goes stale:
	// process blocks in DESCENDING header-line order so earlier mutations only
	// shift lines the loop has already passed.
	void BeginBatch();
	void EndBatch();

	// Rebuild blocks[] from lines (delegates to RebuildFilterBlocks) and bump
	// structureVersion. Deferred while a batch is open.
	void RebuildBlocks();
	unsigned structureVersion() const { return version_; }

private:
	FilterFile* f_ = nullptr;
	unsigned version_ = 0;
	int batchDepth_ = 0;
	bool pendingRebuild_ = false;
};
