// Parse a game Ctrl+C item text into a PreviewItem so the drop-preview can run
// the open .filter against a REAL item (FilterBlade's "Import item" feature).
//
// Accepts both the TW client's zh text and the international client's en text,
// simple copy and 3.29 advanced copy alike. The section grammar mirrors what
// the paste path learned the hard way: section order is NOT an anchor (6-8
// sections, requirements/sockets/influence come and go), and status/influence
// marker lines can share a section with mods — so sections are recognised by
// content, and marker lines are matched anywhere.
//
// Fields the text does not carry (DropLevel / Width / Height) come from
// item_meta.json via FilterI18n; AreaLevel is deliberately NOT parsed — the UI
// injects its own area-level input, exactly like FilterBlade's box.
//
// Pure functions, no UI, no globals — exercised by --filter-selftest T15/T16.
#pragma once

#include "filter_preview.h"

#include <string>
#include <vector>

class FilterI18n;
struct LibItem;

struct ImportIssue {
	std::string line;   // the offending pasted line ("" = whole-item issue)
	std::string msg;    // human-readable zh message ("※ " prefix added by UI)
};

struct ImportedItem {
	bool ok = false;            // base resolved -> item usable for EvaluatePreview
	PreviewItem item;           // areaLevel left at default; UI injects it
	std::string name;           // name line as pasted ("" when absent)
	std::string baseRaw;        // base line as pasted
	std::string baseEn;         // == item.baseType when resolved
	std::string classRaw;       // "Item Class:" value as pasted ("" when absent)
	std::string socketsRaw;     // "Sockets:" value as pasted, e.g. "R G-W R"
	bool exarch = false;        // Searing Exarch mark seen (condition unmodelled)
	bool eater = false;         // Eater of Worlds mark seen (condition unmodelled)
	std::vector<ImportIssue> warnings;
};

ImportedItem ParseGameItemText(const std::string& utf8Text,
                               const FilterI18n& i18n,
                               const std::vector<LibItem>& lib);
