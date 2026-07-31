// PobTools atlas planner: the scarab catalogue and the map-device placement
// rules, as shipped in Data/scarabs_poe1.json (generated from the GGPK by
// tools/ggpk_zh/gen_scarabs.py).
//
// Pure data + rule logic, no ImGui / GL — same split as atlas_stat_agg.
#pragma once

#include "fuzzy_match.h"

#include <string>
#include <unordered_map>
#include <vector>

// A map device takes five scarabs. Each individual scarab additionally caps how
// many copies of itself may go in (ScarabDef::limit, 1..5), and scarabs sharing
// a `family` are mutually exclusive. All three rules come from the game's own
// data (MapFragmentMods), not from lore or guesswork.
static const int kMaxScarabs = 5;

struct ScarabDef {
	std::string id;        // "Metadata/Items/Scarabs/..." — the language-neutral key
	std::string en, zh;    // official names, both locales
	std::string type;      // ScarabTypes id; English in every locale, so never shown alone
	std::string art;       // "Art/2DItems/..." without the extension
	int tier = 0;
	int family = 0;        // mutual-exclusion group
	int limit = 1;         // max copies of THIS scarab in one map device
	bool stash = true;     // false = absent from the fragment stash AND both trade realms

	// Two INDEPENDENT lists: one Description cell holds embedded newlines and
	// the locales do not always split into the same number of lines (Sulphite
	// Scarab is 1 line in English, 2 in Chinese). Render one list or the other,
	// never a line from each — that pairing is the defect recorded in
	// error_i18n_newline_split_positional.
	std::vector<std::string> descEn, descZh;

	// Prebuilt lowercase search keys. Names and effects stay separate so a name
	// hit can outrank an effect hit, and the two locales stay separate so a
	// subsequence match cannot straddle the boundary and invent a hit out of
	// half an English name plus half a Chinese one.
	std::string keyEn, keyZh;              // names
	std::string keyEnCompact, keyZhCompact; // names minus spaces/punctuation
	std::string descKeyEn, descKeyZh;      // effect lines, markup stripped
};

// The picker's query type and parser are the shared ones (fuzzy_match.h); the
// aliases keep the scarab-flavoured names the planner already calls.
using ScarabQuery = FuzzyQuery;
inline ScarabQuery MakeScarabQuery(const std::string& raw) { return MakeFuzzyQuery(raw); }

// How well `d` matches `q`: 0 = no match, higher = better. Name tiers come from
// FuzzyNameScore (100 exact … 40 subsequence) and effect text from
// FuzzyTextScore (30 / 20), so a name hit always outranks an effect hit.
//
// An empty query matches everything with score 1, which keeps the catalogue in
// its natural family+tier order until the user actually types.
int ScarabMatchScore(const ScarabDef& d, const ScarabQuery& q);

enum class ScarabAdd {
	kOk,
	kUnknown,         // no such scarab in this catalogue
	kFull,            // already holding kMaxScarabs
	kOverLimit,       // this scarab is already in `limit` times
	kFamilyConflict,  // another scarab of the same family is already in
};

struct ScarabAddResult {
	ScarabAdd code = ScarabAdd::kOk;
	int limit = 0;                       // kOverLimit: the offending scarab's cap
	const ScarabDef* conflict = nullptr; // kFamilyConflict: the scarab already placed
	bool ok() const { return code == ScarabAdd::kOk; }
};

class ScarabDb {
public:
	// Reads exeDir\Data\scarabs_poe1.json. A missing or malformed file is not
	// fatal: available() stays false, the planner hides the scarab UI, and
	// Sanitize() becomes a no-op so an existing build's scarab list survives
	// untouched instead of being silently emptied.
	bool Load(const std::wstring& exeDir, std::string* err = nullptr);

	bool available() const { return !defs_.empty(); }
	const std::vector<ScarabDef>& All() const { return defs_; }
	const ScarabDef* ById(const std::string& id) const;
	const std::string& Source() const { return source_; } // e.g. "GGPK 3.29.0.4.2"

	// Would adding `id` to `cur` be legal? Order of checks is fixed so the
	// reported reason is the most specific one.
	ScarabAddResult CanAdd(const std::vector<std::string>& cur, const std::string& id) const;

	// Force a list into a legal state, preserving order. Used on every path that
	// accepts outside data (build file, .json import, share code) so a
	// hand-edited or future-season file can never put the planner in an illegal
	// state. When *note is provided it gets a UTF-8 summary of what was dropped
	// (empty when nothing changed).
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
