// PobTools filter-editor localization layer (DISPLAY ONLY): English->Chinese name map from the engine dictionaries,
// a bundled base-type -> item-class map (from POB's Data/Bases) and class zh labels from item_metadata.json.
// The underlying .filter values stay English.
#pragma once

#include <string>
#include <vector>
#include <utility>
#include <unordered_map>

class FilterI18n {
public:
	// Load dictionaries for a locale (e.g. "zh-rTW"). Missing/unparseable files
	// are skipped silently; lookups then fall back to the English input.
	void Load(const std::wstring& exeDir, const std::string& locale);

	bool loaded() const { return loaded_; }

	// English item/base name -> Chinese display name (falls back to the input).
	std::string DisplayName(const std::string& en) const;

	// Base items the engine dictionary (Data/poe1/<locale>/items.json) has but the
	// item catalog (filter_items_zh.json) lacks -- a new league's items before the
	// catalog is regenerated. Not checked to be valid BaseType names. Case-
	// insensitive English or Chinese substring, sorted by English, at most limit;
	// an empty query matches nothing. Pairs are (English, Chinese).
	std::vector<std::pair<std::string, std::string>> OffCatalogMatches(const std::string& query, size_t limit) const;

	// English item name -> repoe item_class for ALL items ("StackableCurrency",
	// "Bow", "Active Skill Gem", "Belt" for a unique, ...); "" if unknown.
	std::string ItemClass(const std::string& en) const;

	// English item name -> repoe tags (e.g. "mid_tier_currency", "body_armour",
	// "unique"); empty if unknown. Used for sub-categorisation and item detail.
	const std::vector<std::string>& Tags(const std::string& en) const;

	// Base drop level from item_meta.json ("drop", repoe base_items); -1 when
	// unknown (uniques, GGPK-patched entries) — callers keep their defaults.
	int DropLevelOf(const std::string& en) const;

	// Inventory size in grid cells ("w"/"h" from item_meta.json); false when
	// unknown — callers keep their defaults.
	bool SizeOf(const std::string& en, int* w, int* h) const;

	// zh -> en lookup tables for parsing pasted item text: item_metadata.json (as the paste path uses, left untouched)
	// plus GGPK-verified code-side supplements. Keys are the TW client's exact strings, values the en client's.
	// Empty maps when the locale has no file.
	struct ZhTables {
		std::unordered_map<std::string, std::string> header;     // "物品種類" -> "Item Class"
		std::unordered_map<std::string, std::string> rarity;     // "稀有"     -> "Rare"
		std::unordered_map<std::string, std::string> status;     // "已汙染"   -> "Corrupted"
		std::unordered_map<std::string, std::string> influence;  // "塑者之物" -> "Shaper Item"
		std::unordered_map<std::string, std::string> itemClass;  // "胸甲"     -> "Body Armours"
	};
	const ZhTables& Zh() const { return zh_; }

	// English item class -> Chinese label (falls back to the input).
	std::string ClassNameZh(const std::string& enClass) const;

	// repoe class id ("StackableCurrency") -> in-game English class name
	// ("Stackable Currency" — the string the Class condition matches).
	// Falls back to the input when the id is unknown (many repoe ids are
	// already spelled like game names, e.g. "Active Skill Gem").
	std::string ClassNameEn(const std::string& classId) const;

private:
	struct Meta {
		std::string cls;
		std::vector<std::string> tags;
		int drop = -1;      // base drop level; -1 = unknown
		int w = 0, h = 0;   // inventory size; 0 = unknown
	};

	std::unordered_map<std::string, std::string> names_;      // en name  -> zh name
	std::vector<std::pair<std::string, std::string>> offCatalog_;   // (en, zh) base items the catalog lacks, sorted
	std::unordered_map<std::string, std::string> classZh_;    // en class -> zh class
	std::unordered_map<std::string, std::string> classEn_;    // class id -> game class name
	std::unordered_map<std::string, Meta>        meta_;       // en name  -> {class, tags, drop, w, h}
	ZhTables zh_;                                             // pasted-item parse tables
	bool loaded_ = false;
};

// NeverSink 標記標題轉繁中顯示（display only；.filter 內容不變）。
// "%H2 $type->leveling->flasks->life $tier->t2" -> "%H2 【練等·藥劑·生命】T2"。
// 未知節段保留英文；非 NeverSink 標題原樣返回。
std::string NeverSinkHeaderZh(const std::string& header);

// A NeverSink tier code ("t2", "any", "t1exalted", "armors1q1") as far as it can
// be read without guessing: the whole code when it is a known word (T2, 任意),
// just the "t<n>" lead of "t1exalted" (T1, *whole = false), else "" (author's
// own abbreviation). *whole: the code was translated completely.
std::string NeverSinkTierZhKnown(const std::string& code, bool* whole);
