// PobTools .filter parser / serializer.
//
// Round-trip strategy: ParseFilter keeps every line's original bytes in
// FilterLine::raw. SerializeFilter emits raw for any line the user did not edit,
// and rebuilds only edited (dirty) lines from their structured fields. Unknown
// keywords, comments and blank lines are never marked dirty, so they always
// survive verbatim — the round-trip risk is confined to lines actually edited.
#pragma once

#include "filter_model.h"
#include <string>

// Parse raw .filter content (UTF-8; a leading BOM is tolerated) into a FilterFile.
FilterFile ParseFilter(const std::string& content);

// Serialize a FilterFile back to .filter text, reproducing the original BOM,
// line ending (CRLF/LF) and trailing newline.
std::string SerializeFilter(const FilterFile& file);

// ---- structural helpers (shared with FilterDocumentEditor) ------------------

// Classify one raw line (no newline) exactly as ParseFilter does per line.
FilterLine ParseFilterLine(const std::string& raw);

// Serialize a single line: raw when clean, rebuilt from fields when dirty.
std::string FilterSerializeLine(const FilterLine& line);

// Rebuild file.blocks (header grouping + per-block action-line cache) from
// file.lines. ParseFilter ends with this; any structural mutation (line
// inserted/removed/reclassified) must call it again so block indices stay valid.
void RebuildFilterBlocks(FilterFile& file);

// Keyword-set membership (schema validation + disabled-line detection).
bool FilterIsKnownCondition(const std::string& keyword);
bool FilterIsKnownAction(const std::string& keyword);

// ---- value accessors used by the editor UI ---------------------------------

// Read the idx-th value of a line as an int (returns def if absent/non-numeric).
int FilterValueInt(const FilterLine& line, size_t idx, int def);

// Replace the idx-th value with an int (extending the value list if needed) and
// mark the line dirty. Used for SetFontSize, PlayAlertSound id/volume, etc.
void FilterSetValueInt(FilterLine& line, size_t idx, int v);

// Replace the idx-th value with a string and mark the line dirty. Used for
// CustomAlertSound paths (quoted=true wraps it in double quotes on serialize).
void FilterSetValueStr(FilterLine& line, size_t idx, const std::string& s, bool quoted);

// Read RGBA from a Set*Color action line. a defaults to 255; hasAlpha reports
// whether the source actually carried a 4th channel.
void FilterGetColor(const FilterLine& line, int& r, int& g, int& b, int& a, bool& hasAlpha);

// Write RGBA back to a Set*Color line and mark it dirty. The alpha channel is
// emitted iff hasAlpha is true (so a fully-opaque colour that never had alpha
// stays a 3-component line, avoiding a needless diff).
void FilterSetColor(FilterLine& line, int r, int g, int b, int a, bool hasAlpha);

// ---- multi-value list helpers (e.g. BaseType / Class lists) -----------------

// True if the line already carries a value equal to text.
bool FilterHasValue(const FilterLine& line, const std::string& text);

// Append a value to the line and mark it dirty. Returns its index. Used to move
// a BaseType between rule blocks in the tier-list view.
size_t FilterAddValue(FilterLine& line, const std::string& text, bool quoted = true);

// Remove the first value equal to text and mark the line dirty. Returns false if
// no such value was present.
bool FilterRemoveValue(FilterLine& line, const std::string& text);
