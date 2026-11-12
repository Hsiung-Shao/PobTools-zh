// PobTools atlas-tree cross-season diff, keyed by GGG skill id: per node, a STRICT per-stat-line difference (added,
// removed, same template with changed numbers) plus node-level adds/removes.
// Lines are normalized with FindStatNumbers() (atlas_stat_agg.h) into a "#" template + numbers, so reordered lines
// still match. Pure data (no ImGui / GL / network); zh display comes from optional AtlasI18n.
#pragma once

#include <string>
#include <vector>

class AtlasTreeData;
class AtlasI18n;

// One stat-line change inside a modified node.
struct AtlasStatDelta {
	enum Kind { kLineAdded, kLineRemoved, kValueChanged };
	Kind kind = kValueChanged;
	std::string en;        // new-side line (added / changed); old-side line (removed)
	std::string enOld;     // old-side line (changed only)
	std::string zh;        // TC display of `en` (falls back to en)
	std::string zhOld;     // TC display of `enOld`
	std::vector<double> oldNums, newNums; // shared-template numbers (changed only)
};

// A node that was added, removed, or modified between the two versions.
struct AtlasNodeDiff {
	enum Kind { kAdded, kRemoved, kModified };
	Kind kind = kModified;
	int id = 0;
	int nodeKind = 0;                 // AtlasNode.kind (normal/notable/keystone/...)
	std::string name, nameZh;         // new side (added/modified); old side (removed)
	float x = 0, y = 0;               // world coords for canvas focus (side that exists)
	bool nameChanged = false;
	std::string nameOld, nameOldZh;   // modified-with-rename only
	std::vector<AtlasStatDelta> stats;            // modified only
	std::vector<std::string> statsOld, statsNew;  // full lists for a side-by-side tooltip
};

struct AtlasTreeDiff {
	std::string oldVer, newVer;       // caller-supplied labels (e.g. "3.28.0")
	std::vector<AtlasNodeDiff> added;     // id only in new
	std::vector<AtlasNodeDiff> removed;   // id only in old
	std::vector<AtlasNodeDiff> modified;  // id in both, stats or name differ
	int Count() const { return (int)(added.size() + removed.size() + modified.size()); }
};

// Strict per-stat-line diff keyed by GGG skill id. i18nOld / i18nNew may be null
// (English-only display). oldLabel / newLabel are copied into the result.
AtlasTreeDiff ComputeAtlasTreeDiff(const AtlasTreeData& oldTree, const AtlasTreeData& newTree,
                                   const AtlasI18n* i18nOld, const AtlasI18n* i18nNew,
                                   const std::string& oldLabel, const std::string& newLabel);

// UTF-8 plain-text report for the CLI / log.
std::string FormatAtlasTreeDiffReport(const AtlasTreeDiff& diff);

// Cross-season TC backfill (conservative): a newTree stat line with no translation reuses the OLD season's
// translation only when the English line is byte-identical; changed lines stay English (no stale translation).
// Returns the number of lines backfilled.
int BackfillAtlasI18n(AtlasI18n& newI18n, const AtlasTreeData& newTree,
                      const AtlasI18n& oldI18n, const AtlasTreeData& oldTree);

// Drop the node-name translation for every node whose English name changed between seasons (the id-keyed repoe
// snapshot still holds the OLD name's Chinese). Returns the number dropped.
// Translations from an EARLIER LEAGUE (AtlasI18n::LagsSeason): keep only what was written for this league (the
// "fresh" lines and names), drop the rest to English. Id-inherited pairings are not trusted across leagues: a reused
// id pairs this league's line with last league's meaning. Returns how many entries were dropped.
int KeepFreshOnly(AtlasI18n& i18n, const AtlasTreeData& tree);

int DropRenamedNames(AtlasI18n& newI18n, const AtlasTreeData& newTree, const AtlasTreeData& oldTree);

// Drop the translations of every English line that is NEW this season (absent from the old tree): the zh snapshot
// predates the season (repoe-fork lags), so Chinese paired to a changed/new line translates the OLD wording.
// Lines present in both seasons keep their Chinese. Returns the number dropped.
// Skip when the zh snapshot is at/after the season.
int PruneStaleTranslations(AtlasI18n& newI18n, const AtlasTreeData& newTree, const AtlasTreeData& oldTree);

// "pob-zh.exe --atlas-diff-selftest": synthetic old/new trees with known
// add/remove/value-change/line-add cases; console + atlas_diff_selftest.txt.
int RunAtlasDiffSelfTest(const std::wstring& exeDir);

// "pob-zh.exe --atlas-diff <oldVer> <newVer>": load both installed versions,
// compute the diff, print a report and write atlas_diff_report.txt. 0 on success.
int RunAtlasDiffCli(const std::wstring& oldVer, const std::wstring& newVer, const std::wstring& exeDir);
