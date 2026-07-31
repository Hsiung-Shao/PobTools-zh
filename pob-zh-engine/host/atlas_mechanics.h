// PobTools atlas planner: league-mechanic grouping for the atlas tree.
//
// Answers "where else on the atlas is this mechanic". The atlas tree marks each
// cluster with a mastery icon (裂痕 / 軍團 / 譫妄 …) and the same mechanic owns
// several clusters spread across the map, so the useful question is one click
// away from any of them.
//
// Where the grouping comes from, in priority order:
//
//   rule 1 (authoritative) -- the node's `group` in GGG's own atlastree-export
//     data.json carries a mastery node; that mastery IS the cluster's mechanic.
//     Covers ~716 of the 903 nodes in 3.29.1.
//   rule 2 (fills the gaps) -- groups without a mastery (3.29 scattered a
//     region across one-node groups) fall back to the node's own icon file
//     name, matched CASE-SENSITIVELY against the mastery-icon tokens below.
//     Adds ~108 more; ~79 stay unassigned and are reported, never guessed.
//
// Rule 2 must never override rule 1. Back-testing rule 2 against rule 1's known
// answers agrees 96.3% (identical in 3.28.0 / 3.29.0 / 3.29.1); every
// disagreement is a generic node sitting inside a themed cluster (a
// ScarabNode1 in the Blight cluster), and cluster membership is the answer the
// user wants. That percentage is asserted at build time as a drift detector --
// if GGG renames the icons, the build stops instead of shipping noise.
//
// Pure data, no ImGui / GL / network.
#pragma once

#include <string>
#include <vector>

// One league mechanic as the atlas tree groups them.
struct AtlasMechanicDef {
	// GGPK AtlasPassiveSkillTreeGroupType.Id -- language independent, stable.
	std::string id;
	// Mastery icon token: Art/.../AtlasPassiveMastery<token>.png.
	std::string token;
	std::string en;   // mastery display name in the tree export
	std::string zh;   // GGPK PassiveSkills Traditional Chinese, same row index
};

// The catalogue, sorted by `en`. Compiled in rather than shipped as data: it is
// the one place mechanic identity and translation live, and the generator below
// runs inside this exe, so the updater never needs the game files.
const std::vector<AtlasMechanicDef>& AtlasMechanicCatalogue();
const AtlasMechanicDef* AtlasMechanicById(const std::string& id);
const AtlasMechanicDef* AtlasMechanicByEn(const std::string& en);

// One season's classification.
struct AtlasMechanicMap {
	struct Group {
		std::string id;             // AtlasMechanicDef::id
		std::vector<int> nodeIds;   // GGG skill ids, ascending
		int clusters = 0;           // mastery icons carrying this mechanic
	};
	std::string tag;
	std::vector<Group> groups;      // sorted by id
	// Provenance, reported rather than hidden: how many nodes each rule claimed,
	// how many nobody could classify, and the rule-2 back-test.
	int fromCluster = 0, fromIcon = 0, unassigned = 0;
	int backtestAgree = 0, backtestTotal = 0;
	std::vector<std::string> unknownMasteries; // mastery names not in the catalogue
	double backtestPct() const {
		return backtestTotal ? 100.0 * backtestAgree / backtestTotal : 0.0;
	}
};

// Minimum rule-2 back-test agreement (percent) accepted by AtlasBuildMechanicMap.
// Measured 96.3% on 3.28.0 / 3.29.0 / 3.29.1; the margin leaves room for normal
// season churn while still catching an icon-naming change.
double AtlasMechanicBacktestGate();

// Classify one season from an atlastree-export data.json document (the same
// string the online updater and the local importer already hold). Fails, rather
// than producing a partial map, when the document is not that format or when
// the back-test falls under the gate.
bool AtlasBuildMechanicMap(const std::string& dataJson, const std::string& tag,
                           AtlasMechanicMap* out, std::string* err);

// Serialized form written to Data/atlas_versions/<tag>/atlas_mechanics.json.
std::string AtlasMechanicMapToJson(const AtlasMechanicMap& m);
// Writes that file into destDir (which must exist, trailing backslash optional).
bool AtlasWriteMechanicMap(const AtlasMechanicMap& m, const std::wstring& destDir,
                           std::string* err);
// Human-readable provenance for logs / the maintainer CLI.
std::string AtlasMechanicMapReport(const AtlasMechanicMap& m);

// Runtime view of one season's file. A missing file is not an error: the panel
// simply says the catalogue is unavailable for that season.
class AtlasMechanicDb {
public:
	struct Entry {
		const AtlasMechanicDef* def = nullptr;
		std::vector<int> nodeIds;
		int clusters = 0;
	};

	// Reads Data/atlas_versions/<tag>/atlas_mechanics.json (tag "" = the flat
	// legacy Data/ layout).
	//
	// When that season has no file yet -- it was downloaded in-app by a build
	// that predates this feature -- the newest season that DOES have one is
	// borrowed instead. Node ids are stable across seasons, so the only cost is
	// that nodes added after the borrowed season stay unclassified; BorrowedFrom()
	// is non-empty in that case so the UI can say so rather than quietly showing
	// a stale grouping. Returns false only when nothing usable was found.
	bool Load(const std::wstring& exeDir, const std::string& tag);
	bool available() const { return !entries_.empty(); }
	const std::vector<Entry>& Entries() const { return entries_; }
	const std::string& Tag() const { return tag_; }
	// Season the data actually came from when it is not the one asked for.
	const std::string& BorrowedFrom() const { return borrowedFrom_; }
	int Unassigned() const { return unassigned_; }

private:
	bool loadOne(const std::wstring& exeDir, const std::string& tag);

	std::vector<Entry> entries_;
	std::string tag_, borrowedFrom_;
	int unassigned_ = 0;
};

// "pob-zh.exe --atlas-mechanics-selftest": catalogue sanity plus a check of
// whatever season files are installed. Appends to `out`, returns the failure
// count (0 = pass), matching the scarab/astrolabe selftest convention.
int RunAtlasMechanicSelfTest(const std::wstring& exeDir, std::string& out);

// "pob-zh.exe --atlas-mechanics-build <data.json> <tag> [destDir]": maintainer
// tool that runs the very same classifier the updater uses, so the files shipped
// in host/data/atlas_versions/ can never drift from what an in-app update would
// produce. Prints the provenance report. Returns 0 on success.
int RunAtlasMechanicBuild(const std::wstring& dataJsonPath, const std::string& tag,
                          const std::wstring& destDir);
