// PobTools atlas planner: the astrolabe catalogue and the quadrant placement
// rule, as shipped in Data/astrolabes_poe1.json (generated from the GGPK by
// tools/ggpk_zh/gen_astrolabes.py).
//
// 3.29 applies an astrolabe to a QUADRANT of the atlas, turning it into a
// "Shaped Region" carrying one league mechanic. The atlas has exactly four
// quadrants and each holds at most one at a time -- both facts come from the
// game's own data (AtlasRegions has 4 rows; the client string
// ItemErrorAstrolabeActiveRegion reads "This Atlas Quadrant already has an
// active Shaped Region"), not from lore.
//
// Pure data + rule logic, no ImGui / GL — same split as atlas_scarabs.
#pragma once

#include "atlas_persist.h" // AstrolabePlacement — the saved shape
#include "fuzzy_match.h"

#include <string>
#include <unordered_map>
#include <vector>

struct AstrolabeDef {
	std::string id;        // "Metadata/Items/Currency/Astrolabe..." — language-neutral key
	std::string en, zh;    // official names, both locales
	std::string mechanic;  // AtlasMissionTypes id; English in every locale, so never shown alone
	std::string art;       // "Art/2DItems/..." without the extension
	// false = shipped but not live this league: it has no
	// map_astrolabe_*_reward_and_difficulty stat AND neither trade realm lists
	// it. Still selectable — a planner should show what the game contains — but
	// the picker says so.
	bool enabled = true;

	// Two INDEPENDENT lists, for the same reason as the scarabs: one
	// Description cell holds embedded newlines and the locales need not split
	// into the same number of lines. Render one list or the other, never a line
	// from each (error_i18n_newline_split_positional).
	std::vector<std::string> descEn, descZh;

	// Prebuilt lowercase search keys; names and effects separate so a name hit
	// can outrank an effect hit, locales separate so a subsequence match cannot
	// straddle the boundary.
	std::string keyEn, keyZh;
	std::string keyEnCompact, keyZhCompact;
	std::string descKeyEn, descKeyZh;
};

// One of the atlas's four quadrants.
struct AtlasQuadrant {
	std::string id;              // "NorthWest" — the language-neutral key
	std::string vaultEn, vaultZh; // its Memory Vault's area name
};

enum class AstrolabeAdd {
	kOk,
	kUnknownRegion,     // no such quadrant in this catalogue
	kUnknownAstrolabe,  // no such astrolabe
	kRegionOccupied,    // that quadrant already holds a Shaped Region
};

struct AstrolabeAddResult {
	AstrolabeAdd code = AstrolabeAdd::kOk;
	const AstrolabeDef* occupant = nullptr; // kRegionOccupied: what is already there
	bool ok() const { return code == AstrolabeAdd::kOk; }
};

class AstrolabeDb {
public:
	// Reads exeDir\Data\astrolabes_poe1.json. A missing or malformed file is not
	// fatal: available() stays false, the planner hides the astrolabe UI, and
	// Sanitize() becomes a no-op so an existing project's placements survive
	// untouched instead of being silently erased on the next save.
	bool Load(const std::wstring& exeDir, std::string* err = nullptr);

	bool available() const { return !defs_.empty() && !regions_.empty(); }
	const std::vector<AstrolabeDef>& All() const { return defs_; }
	const std::vector<AtlasQuadrant>& Regions() const { return regions_; }
	const AstrolabeDef* ById(const std::string& id) const;
	const AtlasQuadrant* RegionById(const std::string& id) const;
	const std::string& Source() const { return source_; } // e.g. "GGPK 3.29.0.4.2"

	// Would placing `id` on `region` be legal?
	AstrolabeAddResult CanPlace(const std::vector<AstrolabePlacement>& cur,
	                            const std::string& region, const std::string& id) const;

	// Force a placement list into a legal state, preserving order. Used on every
	// path that accepts outside data (project file, .json import, share code).
	// When *note is provided it gets a UTF-8 summary of what was dropped.
	std::vector<AstrolabePlacement> Sanitize(const std::vector<AstrolabePlacement>& cur,
	                                         std::string* note) const;

	// Score for the picker; 0 = no match, empty query = 1 (natural order).
	int MatchScore(const AstrolabeDef& d, const FuzzyQuery& q) const;

private:
	std::vector<AstrolabeDef> defs_;
	std::vector<AtlasQuadrant> regions_;
	std::unordered_map<std::string, int> byId_;
	std::unordered_map<std::string, int> regionById_;
	std::string source_;
};

// Headless checks for the placement rule and the persistence round-trip,
// appended to --atlas-selftest. Returns the number of failures and appends a
// human-readable report to `out`.
int RunAstrolabeSelfTest(const std::wstring& exeDir, std::string& out);
