// PobTools atlas planner: the map catalogue behind the project's "main map"
// pick, as shipped in Data/atlas_maps_poe1.json (generated from the GGPK by
// tools/ggpk_zh/gen_atlas_maps.py).
//
// A project names ONE map. There is no placement rule to enforce, so this is a
// catalogue plus the picker's search — the only invariant is that a saved id
// must still exist in the current season's catalogue.
//
// Pure data, no ImGui / GL — same split as atlas_scarabs / atlas_astrolabes.
#pragma once

#include "fuzzy_match.h"

#include <string>
#include <unordered_map>
#include <vector>

struct AtlasMapDef {
	std::string id;            // WorldAreas.Id ("MapWorldsShrine") — language-neutral
	// A map has TWO names and they are not always the same string: the atlas
	// shows the area (庇護聖所) while the item reads 聖所地圖. 8 of the 110
	// regular maps differ, so both are kept, both are searchable, and the area
	// name is what the UI leads with.
	std::string enArea, zhArea;
	std::string enItem, zhItem;
	std::string region;        // AtlasQuadrant::id, matching the astrolabe catalogue
	std::string art;           // "Art/2DItems/..." without the extension
	int tier = 0;              // 0 for unique maps, which have no tier of their own
	enum Kind { kNormal, kOffAtlas, kUnique };
	Kind kind = kNormal;

	// Prebuilt lowercase search keys (both names of both locales, plus the tier
	// as typed digits so "t14" finds the tier-14 maps).
	std::string keyEn, keyZh;
	std::string keyEnCompact, keyZhCompact;
};

class AtlasMapDb {
public:
	// Reads exeDir\Data\atlas_maps_poe1.json. A missing or malformed file is not
	// fatal: available() stays false, the planner hides the picker, and
	// SanitizeOne() becomes a no-op so a saved pick survives untouched.
	bool Load(const std::wstring& exeDir, std::string* err = nullptr);

	bool available() const { return !defs_.empty(); }
	const std::vector<AtlasMapDef>& All() const { return defs_; }
	const AtlasMapDef* ById(const std::string& id) const;
	const std::string& Source() const { return source_; }  // "GGPK 3.29.0.4.2"
	const std::string& Series() const { return series_; }  // "Deepwater"

	// Returns `id` when it is a map this catalogue knows, "" when it is not.
	// With no catalogue loaded the id is returned unchanged — the same reason
	// the scarab and astrolabe Sanitize are no-ops without their data files.
	std::string SanitizeOne(const std::string& id) const;

	// The tiers this season actually ships, ascending. The picker's tier filter
	// is built from this rather than from a hardcoded 1..16, so a season that
	// adds or drops a tier needs no code change — and it is computed once at
	// load instead of rescanned every frame.
	const std::vector<int>& TiersPresent() const { return tiers_; }

	// Score for the picker; 0 = no match, empty query = 1 (natural order).
	int MatchScore(const AtlasMapDef& d, const FuzzyQuery& q) const;

private:
	std::vector<AtlasMapDef> defs_;
	std::unordered_map<std::string, int> byId_;
	std::vector<int> tiers_;
	std::string source_, series_;
};

// Headless checks appended to --atlas-selftest. Returns the number of failures
// and appends a human-readable report to `out`.
int RunAtlasMapSelfTest(const std::wstring& exeDir, std::string& out);
