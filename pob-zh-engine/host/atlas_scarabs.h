// PobTools atlas planner: the map-device catalogue (地圖格) and its placement rules (Data/scarabs_poe1.json).
// Only the eight Vaal map fragments remain; file / type names keep "scarab" so saved builds stay compatible.
// Pure data + rule logic, no ImGui / GL.
#pragma once

#include "fuzzy_match.h"

#include <string>
#include <unordered_map>
#include <vector>

// A map device takes five items; each entry caps its own copies (ScarabDef::limit, 1..5). Both rules come from the
// game's data (MapFragmentMods).
static const int kMaxScarabs = 5;

struct ScarabDef {
	std::string id;        // "Metadata/Items/MapFragments/..." — the language-neutral key
	std::string en, zh;    // official names, both locales
	std::string art;       // "Art/2DItems/..." without the extension
	int limit = 1;         // max copies of THIS entry in one map device
	bool stash = true;     // false = absent from the fragment stash AND both trade realms

	// Two INDEPENDENT lists: one Description cell holds embedded newlines and
	// the locales do not always split into the same number of lines. Render one list or the other,
	// never a line from each — that pairing is the defect recorded in
	// error_i18n_newline_split_positional.
	std::vector<std::string> descEn, descZh;

	// Prebuilt lowercase search keys. Names and effects stay separate (a name hit outranks an effect hit), and the two
	// locales stay separate so a subsequence match cannot straddle half an English and half a Chinese name.
	std::string keyEn, keyZh;              // names
	std::string keyEnCompact, keyZhCompact; // names minus spaces/punctuation
	std::string descKeyEn, descKeyZh;      // effect lines, markup stripped
};

// The picker's query type and parser are the shared ones (fuzzy_match.h); the
// aliases keep the scarab-flavoured names the planner already calls.
using ScarabQuery = FuzzyQuery;
inline ScarabQuery MakeScarabQuery(const std::string& raw) { return MakeFuzzyQuery(raw); }

// How well `d` matches `q`: 0 = no match, higher = better. Name tiers from FuzzyNameScore (100 exact … 40 subsequence),
// effect text from FuzzyTextScore (30 / 20), so a name hit always outranks an effect hit.
// An empty query matches everything with score 1 (natural file order until the user types).
int ScarabMatchScore(const ScarabDef& d, const ScarabQuery& q);

enum class ScarabAdd {
	kOk,
	kUnknown,         // no such entry in this catalogue
	kFull,            // already holding kMaxScarabs
	kOverLimit,       // this entry is already in `limit` times
};

struct ScarabAddResult {
	ScarabAdd code = ScarabAdd::kOk;
	int limit = 0;                       // kOverLimit: the offending entry's cap
	bool ok() const { return code == ScarabAdd::kOk; }
};

class ScarabDb {
public:
	// Reads exeDir\Data\scarabs_poe1.json. A missing or malformed file is not fatal: available() stays false, the
	// 地圖格 UI is hidden, and Sanitize() becomes a no-op so an existing build's slot list is not silently emptied.
	bool Load(const std::wstring& exeDir, std::string* err = nullptr);

	bool available() const { return !defs_.empty(); }
	const std::vector<ScarabDef>& All() const { return defs_; }
	const ScarabDef* ById(const std::string& id) const;
	const std::string& Source() const { return source_; } // e.g. "GGPK 3.29.0.4.2"

	// Would adding `id` to `cur` be legal? Order of checks is fixed so the
	// reported reason is the most specific one.
	ScarabAddResult CanAdd(const std::vector<std::string>& cur, const std::string& id) const;

	// Force a list into a legal state, preserving order. Used on every path accepting outside data (build file, .json
	// import, share code). *note, when given, gets a UTF-8 summary of what was dropped (empty when nothing changed).
	std::vector<std::string> Sanitize(const std::vector<std::string>& ids, std::string* note) const;

private:
	std::vector<ScarabDef> defs_;
	std::unordered_map<std::string, int> byId_;
	std::string source_;
};

// Headless checks for the placement rules and the persistence round-trip,
// appended to --atlas-selftest. Returns the number of failures and appends a
// human-readable report to `out`.
int RunScarabSelfTest(const std::wstring& exeDir, std::string& out);
