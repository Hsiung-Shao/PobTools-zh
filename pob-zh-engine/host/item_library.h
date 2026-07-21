// PobTools master item catalog.
//
// The tier-list's right pane normally lists only items already present in the open
// .filter. The library lets the user add ANY game item to a tier. The canonical set
// of "real items" is the bundled icon_paths.json key set (currency / gems / div
// cards / maps / gear bases — the same names the icon manager knows), cross-
// referenced with the Chinese names and base-class map from FilterI18n. Names stay
// English; only the displayed label is Chinese.
#pragma once

#include "filter_i18n.h"
#include <string>
#include <vector>

struct LibItem {
	std::string en;                   // English name (output token, icon lookup, drag key)
	std::string zh;                   // Chinese display name (== en if untranslated)
	std::string enClass;              // repoe item class ("Bow", "StackableCurrency"...); "" if unknown
	std::vector<std::string> tags;    // repoe tags (for sub-categorisation / detail)
};

class ItemLibrary {
public:
	// Build the catalog from the complete bundled name set (Data/filter_items_zh.json,
	// falling back to icon_paths.json), resolving zh / class / tags via i18n.
	// Call after i18n.Load(). Idempotent.
	void Load(const std::wstring& exeDir, const FilterI18n& i18n);

	bool loaded() const { return !items_.empty(); }
	const std::vector<LibItem>& items() const { return items_; }

private:
	std::vector<LibItem> items_;   // sorted by English name
};
