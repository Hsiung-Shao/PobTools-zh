// PobTools loot-filter editor data model.
//
// A parsed .filter is kept as an ordered list of lines — the round-trip source
// of truth: every line stores its original bytes and is re-emitted verbatim
// unless the user edits it — plus a list of blocks that index into those lines
// so the UI can edit conditions/actions structurally without losing comments,
// blank lines, indentation or any unrecognised (forward-compatible) syntax.
#pragma once

#include <string>
#include <vector>

enum class FilterLineKind {
	Blank,        // empty / whitespace-only line
	Comment,      // first non-space character is '#'
	BlockHeader,  // Show / Hide (block start)
	Condition,    // a recognised condition keyword (Class, Rarity, ItemLevel, ...)
	Action,       // a recognised action keyword (SetTextColor, PlayAlertSound, ...)
	Unknown,      // a non-empty line we do not recognise (kept verbatim, read-only in UI)
};

// One whitespace-separated value on a line. `quoted` records whether the source
// wrapped it in double quotes so the serializer can reproduce it faithfully.
struct FilterToken {
	std::string text;
	bool quoted = false;
};

struct FilterLine {
	FilterLineKind kind = FilterLineKind::Unknown;
	std::string raw;              // original line bytes (no newline) — emitted as-is when !dirty
	std::string indent;           // leading whitespace (spaces/tabs) preserved
	std::string keyword;          // e.g. "SetTextColor" / "Class" / "Show"
	std::string op;               // condition operator: "" / ">=" / "<=" / "==" / "=" / "<" / ">"
	std::vector<FilterToken> values;
	std::string trailingComment;  // trailing " # ..." kept verbatim (leading spaces + '#' included)
	bool dirty = false;           // true -> rebuild from fields; false -> emit raw
};

struct FilterBlock {
	int  headerLineIdx = -1;      // index into FilterFile::lines of the Show/Hide line
	bool hide = false;            // Show = false, Hide = true
	std::vector<int> lineIdx;     // every line owned by this block (header + body until next header)
	std::string headerComment;    // trailing comment on the header line (the block label in many filters)

	// Cached indices (into FilterFile::lines) of this block's action lines, -1 if absent.
	// When a block repeats the same action, the index points at the first occurrence.
	int idxTextColor = -1, idxBorderColor = -1, idxBgColor = -1, idxFontSize = -1;
	int idxAlertSound = -1, idxCustomSound = -1, idxDisableDropSound = -1;
	int idxMinimapIcon = -1, idxPlayEffect = -1;
};

struct FilterFile {
	std::wstring path;            // full path on disk ("" for a not-yet-saved buffer)
	std::string  name;            // file name only, for display
	std::vector<FilterLine>  lines;
	std::vector<FilterBlock> blocks;
	bool hadBom = false;          // source began with a UTF-8 BOM
	bool crlf = true;             // line ending: CRLF (true) vs LF (false)
	bool finalNewline = true;     // source ended with a trailing newline
	bool dirty = false;           // any line edited since load/save
	int  game = 1;                // 1 = POE1 (POE2 reserved for a later stage)
};

// Count blocks/lines with unsaved edits (UI status + save-button enable).
inline bool FilterFileDirty(const FilterFile& f) { return f.dirty; }
