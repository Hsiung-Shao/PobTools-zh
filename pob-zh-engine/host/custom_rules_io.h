// PobTools custom-rules zone (自訂區) + export / import (保存自定義 / 導入自定義).
//
// The zone is a sentinel-delimited region kept at the very top of the filter
// (before the first Show/Hide), so custom rules always win by the game's
// first-match rule. Machine recognition uses exactly two comment lines:
//   #pobtools-custom-begin v=1
//   #pobtools-custom-end
// Export writes a valid .filter fragment (BOM + CRLF) with a metadata comment
// header; import inserts the fragment's blocks into the zone, skipping blocks
// already present verbatim.
#pragma once

#include "filter_doc_editor.h"
#include <string>

struct CustomZone {
	int beginLine = -1;   // index of the begin-sentinel line
	int endLine = -1;     // index of the end-sentinel line
	bool present() const { return beginLine >= 0 && endLine > beginLine; }
};

// Locate the zone. Repairs nothing; endLine == -1 with beginLine >= 0 means the
// end sentinel is missing (user damage).
CustomZone FindCustomZone(const FilterFile& f);

// Locate the zone, creating (or repairing) it if needed: inserted after the
// file's leading comment/blank banner, before the first block header.
CustomZone EnsureCustomZone(FilterDocumentEditor& doc);

// Serialize the given blocks as an export fragment (metadata header + each
// block's lines verbatim, CRLF).
std::string ExportCustomRules(const FilterFile& f, const std::vector<int>& blockIdx,
                              const std::string& name);

// Write an export fragment to disk (UTF-8 BOM). Returns false + *err on failure.
bool SaveCustomRulesFile(const std::wstring& path, const std::string& fragment, std::string* err);

// Parse a fragment and insert its blocks at the end of the custom zone.
// Duplicate blocks (same header + same condition lines, byte-wise) are skipped.
// Returns the number of blocks imported, -1 on parse failure.
int ImportCustomRules(FilterDocumentEditor& doc, const std::string& fragment, std::string* err);
