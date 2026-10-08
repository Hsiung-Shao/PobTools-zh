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
#include <unordered_map>
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

// How a block compares with the baseline (the file as last loaded or saved).
enum class BlockChange {
	Same,       // byte-identical to the baseline
	Modified,   // the block existed and its lines differ
	Added,      // no baseline block (a new custom rule, an import)
};

class FilterDocumentEditor {
public:
	// Attaching drops any baseline (it belonged to the previous file).
	void Attach(FilterFile* f) { f_ = f; version_++; ClearBaseline(); AssignUids(); }
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

	// ---- baseline: "what the file on disk says" ----------------------------
	// A snapshot of every block's own lines (header through its last syntax or
	// "#!" line; the blank / comment gap before the next block is not part of
	// it), keyed by the header line's uid. Taken after a load or a save. It never
	// touches the lines themselves, so serialization is unaffected.
	void CaptureBaseline();
	void ClearBaseline();
	bool HasBaseline() const { return hasBaseline_; }
	BlockChange BlockState(int blockIdx) const;
	// The baseline version of a current line (matched by uid); nullptr when the
	// line is new or there is no baseline.
	const FilterLine* BaselineLine(int lineIdx) const;
	// The line differs from its baseline version (a new line counts as changed).
	bool LineChanged(int lineIdx) const;
	// Put a Modified block's lines back to the baseline (a data-layer mutation:
	// structural, version bumped). Returns the block's index afterwards, -1 when
	// the block has no baseline. Clears model.dirty when the whole file then
	// matches the baseline again.
	int RestoreBlock(int blockIdx);
	// Baseline blocks that no longer exist (a deleted custom rule).
	int RemovedBaselineBlocks() const;
	// Unsaved changes as the editor counts them: modified + added + removed
	// blocks. With settle, a file whose edits were all undone by hand (dirty but
	// byte-identical to the baseline) is marked clean and reports 0.
	int UnsavedBlockCount(bool settle);
	// The whole file serializes to the baseline's bytes.
	bool MatchesBaseline() const;

private:
	// Index of the last line that belongs to the block itself.
	int BodyEnd(int blockIdx) const;
	void AssignUids();

	FilterFile* f_ = nullptr;
	unsigned version_ = 0;
	int batchDepth_ = 0;
	bool pendingRebuild_ = false;
	unsigned nextUid_ = 1;

	struct BaseBlock { std::vector<FilterLine> body; };   // clean copies
	bool hasBaseline_ = false;
	std::unordered_map<unsigned, BaseBlock> base_;        // header uid -> block
	std::unordered_map<unsigned, const FilterLine*> baseLine_;  // line uid -> its copy
	std::string baseText_;                                 // SerializeFilter at capture
	unsigned settleCheckedAt_ = ~0u;                       // UnsavedBlockCount's memo
	bool settleMatched_ = false;
};
