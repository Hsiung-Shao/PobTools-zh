// PobTools atlas planner: league-mechanic grouping for the atlas tree.
//
// Answers "where else on the atlas is this mechanic": each cluster carries a mastery icon (裂痕 / 軍團 / 譫妄 …)
// and one mechanic owns several clusters across the map.
//
// Grouping, in priority order:
//   rule 1 (authoritative) -- the node's `group` in GGG's atlastree-export data.json carries a mastery node; that
//     mastery IS the cluster's mechanic (~716 of 903 nodes in 3.29.1).
//   rule 2 (fills the gaps) -- groups without a mastery fall back to the node's icon file name, matched
//     CASE-SENSITIVELY against the mastery-icon tokens below (~108 more; ~79 stay unassigned, reported, never guessed).
//
// Rule 2 must never override rule 1. Back-tested against rule 1 it agrees 96.3% (same in 3.28.0 / 3.29.0 / 3.29.1);
// each disagreement is a generic node inside a themed cluster, where cluster membership is the wanted answer. That
// percentage is asserted at build time as a drift detector: if GGG renames icons the build stops instead of shipping noise.
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
	// Added from the tree export alone, before the season's game files exist: `id` is the icon token standing in
	// for the GGPK id and `zh` repeats `en`. Replace both from GGPK.
	bool provisional = false;
};

// The catalogue, sorted by `en`. Compiled in, not shipped as data: the one place mechanic identity and translation live, and the updater never needs the game files.
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
	// Provenance: how many nodes each rule claimed, how many nobody could classify, and the rule-2 back-test.
	int fromCluster = 0, fromIcon = 0, unassigned = 0;
	int backtestAgree = 0, backtestTotal = 0;
	std::vector<std::string> unknownMasteries; // mastery names not in the catalogue
	double backtestPct() const {
		return backtestTotal ? 100.0 * backtestAgree / backtestTotal : 0.0;
	}
};

// Minimum rule-2 back-test agreement (percent) accepted by AtlasBuildMechanicMap (measured 96.3%; margin allows season churn but catches an icon-naming change).
double AtlasMechanicBacktestGate();

// Classify one season from an atlastree-export data.json document. Fails (no partial map) when the document is
// not that format or the back-test falls under the gate.
bool AtlasBuildMechanicMap(const std::string& dataJson, const std::string& tag,
                           AtlasMechanicMap* out, std::string* err);

// Serialized form written to Data/atlas_versions/<tag>/atlas_mechanics.json.
std::string AtlasMechanicMapToJson(const AtlasMechanicMap& m);
// Writes that file into destDir (which must exist, trailing backslash optional).
bool AtlasWriteMechanicMap(const AtlasMechanicMap& m, const std::wstring& destDir,
                           std::string* err);
// Human-readable provenance for logs / the maintainer CLI.
std::string AtlasMechanicMapReport(const AtlasMechanicMap& m);

// Runtime view of one season's file. A missing file is not an error: the panel says the catalogue is unavailable.
class AtlasMechanicDb {
public:
	struct Entry {
		const AtlasMechanicDef* def = nullptr;
		std::vector<int> nodeIds;
		int clusters = 0;
	};

	// Reads Data/atlas_versions/<tag>/atlas_mechanics.json (tag "" = flat legacy Data/). When that season has no
	// file yet, the newest season that has one is borrowed (node ids are stable across seasons; newer nodes stay
	// unclassified) and BorrowedFrom() is non-empty so the UI can say so. Returns false only when nothing usable was found.
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

// "pob-zh.exe --atlas-mechanics-build <data.json> <tag> [destDir]": maintainer tool running the same classifier as the
// updater (shipped host/data/atlas_versions/ files cannot drift from an in-app update). Prints provenance. Returns 0 on success.
int RunAtlasMechanicBuild(const std::wstring& dataJsonPath, const std::string& tag,
                          const std::wstring& destDir);
