// PobTools atlas-tree cross-season diff.
//
// Compares two atlas passive trees keyed by GGG skill id and reports, per node,
// a STRICT per-stat-line difference: lines added, lines removed, and lines whose
// template matches but whose numbers changed (e.g. "15% increased X" ->
// "20% increased X"). Node-level adds/removes are reported too.
//
// Stat lines are normalized with FindStatNumbers() (atlas_stat_agg.h) into a
// "#"-placeholder template plus a number vector, so reordered lines still match
// and only genuine numeric/text changes surface. Pure data, no ImGui / GL /
// network; Traditional-Chinese display is injected via optional AtlasI18n.
//
// Verified headless via "pob-zh.exe --atlas-diff-selftest"; a real cross-season
// report is produced by "pob-zh.exe --atlas-diff <oldVer> <newVer>".
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

// Cross-season TC backfill (conservative): for every stat line in newTree that
// has no direct translation in newI18n, if the OLD season has a byte-identical
// English line WITH a translation, reuse that translation verbatim. Lines whose
// numbers or wording changed at all are left in the new English (no synthesis) —
// so a stale translation is never shown for a changed line. Returns the number
// of lines backfilled. Helps unchanged lines that repoe-fork failed to cover.
int BackfillAtlasI18n(AtlasI18n& newI18n, const AtlasTreeData& newTree,
                      const AtlasI18n& oldI18n, const AtlasTreeData& oldTree);

// Drop the node-name translation for every node whose English name changed
// between the two seasons: the repoe snapshot (keyed by node id) still holds the
// OLD name's Chinese, which reads as a wrong name after a rework. Dropping it
// falls the name back to the correct new English. Returns the number dropped.
int DropRenamedNames(AtlasI18n& newI18n, const AtlasTreeData& newTree, const AtlasTreeData& oldTree);

// Drop the stat-line translations of every English line that is NEW this season
// (absent from the old tree): the zh snapshot predates the season (repoe-fork
// lags), so whatever Chinese got paired to a changed/new line is a translation
// of the OLD wording — a plain mistranslation. Unchanged lines (present in both
// seasons) keep their Chinese untouched, so ordinary wording differences like
// "an" vs "1 個" are never affected. Returns the number of lines dropped.
// Skip this when the zh snapshot is at/after the season (repoe caught up).
int PruneStaleTranslations(AtlasI18n& newI18n, const AtlasTreeData& newTree, const AtlasTreeData& oldTree);

// "pob-zh.exe --atlas-diff-selftest": synthetic old/new trees with known
// add/remove/value-change/line-add cases; console + atlas_diff_selftest.txt.
int RunAtlasDiffSelfTest(const std::wstring& exeDir);

// "pob-zh.exe --atlas-diff <oldVer> <newVer>": load both installed versions,
// compute the diff, print a report and write atlas_diff_report.txt. 0 on success.
int RunAtlasDiffCli(const std::wstring& oldVer, const std::wstring& newVer, const std::wstring& exeDir);
