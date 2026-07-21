// PobTools filter-editor localization layer (DISPLAY ONLY).
//
// Loads the same engine dictionaries the translation editor uses (items / uniques
// / gems / ui) into an English->Chinese name map, plus a bundled base-type ->
// item-class map (generated from POB's Data/Bases) and the class-name zh labels
// from item_metadata.json. The tier-list UI shows Chinese names and groups items
// by class, while the underlying .filter values stay English (output is unchanged).
#pragma once

#include <string>
#include <vector>
#include <unordered_map>

class FilterI18n {
public:
	// Load dictionaries for a locale (e.g. "zh-rTW"). Missing/unparseable files
	// are skipped silently; lookups then fall back to the English input.
	void Load(const std::wstring& exeDir, const std::string& locale);

	bool loaded() const { return loaded_; }

	// English item/base name -> Chinese display name (falls back to the input).
	std::string DisplayName(const std::string& en) const;

	// English base type -> English item class ("Bow", "Body Armour", ...); "" if unknown.
	// (Legacy gear-only map, used by the existing base-class sub-filter.)
	std::string BaseClass(const std::string& en) const;

	// English item name -> repoe item_class for ALL items ("StackableCurrency",
	// "Bow", "Active Skill Gem", "Belt" for a unique, ...); "" if unknown.
	std::string ItemClass(const std::string& en) const;

	// English item name -> repoe tags (e.g. "mid_tier_currency", "body_armour",
	// "unique"); empty if unknown. Used for sub-categorisation and item detail.
	const std::vector<std::string>& Tags(const std::string& en) const;

	// English item class -> Chinese label (falls back to the input).
	std::string ClassNameZh(const std::string& enClass) const;

	// repoe class id ("StackableCurrency") -> in-game English class name
	// ("Stackable Currency" — the string the Class condition matches).
	// Falls back to the input when the id is unknown (many repoe ids are
	// already spelled like game names, e.g. "Active Skill Gem").
	std::string ClassNameEn(const std::string& classId) const;

private:
	struct Meta { std::string cls; std::vector<std::string> tags; };

	std::unordered_map<std::string, std::string> names_;      // en name  -> zh name
	std::unordered_map<std::string, std::string> baseClass_;  // en base  -> en class (legacy)
	std::unordered_map<std::string, std::string> classZh_;    // en class -> zh class
	std::unordered_map<std::string, std::string> classEn_;    // class id -> game class name
	std::unordered_map<std::string, Meta>        meta_;       // en name  -> {class, tags}
	bool loaded_ = false;
};

// NeverSink 標記標題轉繁中顯示（display only；.filter 內容不變）。
// "%H2 $type->leveling->flasks->life $tier->t2" -> "%H2 【練等·藥劑·生命】T2"。
// 未知節段保留英文；非 NeverSink 標題原樣返回。
std::string NeverSinkHeaderZh(const std::string& header);
