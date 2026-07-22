#include "atlas_diff.h"
#include "atlas_tree_data.h"
#include "atlas_i18n.h"
#include "atlas_stat_agg.h" // FindStatNumbers: line -> template + numbers

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX          // keep std::min/std::max usable (windows.h defines min/max macros)
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <unordered_map>
#include <unordered_set>

// ---- file helper (report output) --------------------------------------------

static bool write_file_utf8(const std::wstring& path, const std::string& content)
{
	HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE) return false;
	DWORD written = 0;
	bool ok = content.empty() ||
		(WriteFile(h, content.data(), (DWORD)content.size(), &written, nullptr) && written == content.size());
	CloseHandle(h);
	return ok;
}

// ---- stat-line normalization ------------------------------------------------

namespace {

struct NormLine {
	std::string raw;               // original English line
	std::string tmpl;              // numbers replaced by '#'
	std::vector<double> nums;      // the numbers, in order
};

// Replace each numeric token (sign included) with '#'; collect the values.
NormLine Normalize(const std::string& s)
{
	NormLine n;
	n.raw = s;
	std::vector<StatNumTok> toks = FindStatNumbers(s);
	size_t prev = 0;
	for (const StatNumTok& t : toks) {
		n.tmpl.append(s, prev, t.pos - prev);
		n.tmpl.push_back('#');
		n.nums.push_back(t.val);
		prev = t.pos + t.len;
	}
	n.tmpl.append(s, prev, std::string::npos);
	return n;
}

std::string zhOf(const AtlasI18n* i18n, const std::string& en)
{
	if (!i18n) return en;
	return i18n->StatLine(en); // returns a copy of the mapped line or en
}

// Diff two stat-line lists by template. Same template + same numbers = unchanged;
// same template + different numbers = value change; template only on one side =
// line add/remove. Duplicate templates are paired positionally.
void DiffStats(const std::vector<std::string>& oldStats, const std::vector<std::string>& newStats,
               const AtlasI18n* i18nOld, const AtlasI18n* i18nNew, std::vector<AtlasStatDelta>& out)
{
	// group by template, preserving first-seen order (new side first, then old-only)
	std::unordered_map<std::string, std::vector<NormLine>> oldByT, newByT;
	std::vector<std::string> order;
	auto push = [&](const std::string& s, std::unordered_map<std::string, std::vector<NormLine>>& m,
	                bool track) {
		NormLine n = Normalize(s);
		if (track && m.find(n.tmpl) == m.end() && oldByT.find(n.tmpl) == oldByT.end())
			order.push_back(n.tmpl);
		m[n.tmpl].push_back(std::move(n));
	};
	for (const std::string& s : newStats) push(s, newByT, true);
	for (const std::string& s : oldStats) push(s, oldByT, false);
	for (const std::string& s : oldStats) { // append old-only templates to the order
		NormLine n = Normalize(s);
		if (newByT.find(n.tmpl) == newByT.end() &&
		    std::find(order.begin(), order.end(), n.tmpl) == order.end())
			order.push_back(n.tmpl);
	}

	static const std::vector<NormLine> kEmpty;
	for (const std::string& t : order) {
		auto oi = oldByT.find(t);
		auto ni = newByT.find(t);
		const std::vector<NormLine>& ol = oi != oldByT.end() ? oi->second : kEmpty;
		const std::vector<NormLine>& nl = ni != newByT.end() ? ni->second : kEmpty;
		size_t common = std::min(ol.size(), nl.size());
		for (size_t i = 0; i < common; i++) {
			if (ol[i].nums == nl[i].nums) continue; // identical line
			AtlasStatDelta d;
			d.kind = AtlasStatDelta::kValueChanged;
			d.en = nl[i].raw; d.enOld = ol[i].raw;
			d.zh = zhOf(i18nNew, nl[i].raw); d.zhOld = zhOf(i18nOld, ol[i].raw);
			d.oldNums = ol[i].nums; d.newNums = nl[i].nums;
			out.push_back(std::move(d));
		}
		for (size_t i = common; i < nl.size(); i++) {
			AtlasStatDelta d;
			d.kind = AtlasStatDelta::kLineAdded;
			d.en = nl[i].raw; d.zh = zhOf(i18nNew, nl[i].raw);
			out.push_back(std::move(d));
		}
		for (size_t i = common; i < ol.size(); i++) {
			AtlasStatDelta d;
			d.kind = AtlasStatDelta::kLineRemoved;
			d.en = ol[i].raw; d.zh = zhOf(i18nOld, ol[i].raw);
			out.push_back(std::move(d));
		}
	}
}

} // namespace

// ---- compute ----------------------------------------------------------------

AtlasTreeDiff ComputeAtlasTreeDiff(const AtlasTreeData& oldTree, const AtlasTreeData& newTree,
                                   const AtlasI18n* i18nOld, const AtlasI18n* i18nNew,
                                   const std::string& oldLabel, const std::string& newLabel)
{
	AtlasTreeDiff diff;
	diff.oldVer = oldLabel;
	diff.newVer = newLabel;

	std::unordered_map<int, int> oldIdx, newIdx; // id -> node index
	for (int i = 0; i < (int)oldTree.nodes.size(); i++) oldIdx[oldTree.nodes[i].id] = i;
	for (int i = 0; i < (int)newTree.nodes.size(); i++) newIdx[newTree.nodes[i].id] = i;

	// added + modified (iterate new; keep new-tree order, sorted by id below)
	for (const AtlasNode& nn : newTree.nodes) {
		auto oit = oldIdx.find(nn.id);
		if (oit == oldIdx.end()) {
			AtlasNodeDiff nd;
			nd.kind = AtlasNodeDiff::kAdded;
			nd.id = nn.id; nd.nodeKind = nn.kind;
			nd.name = nn.name; nd.nameZh = i18nNew ? i18nNew->NodeName(nn.id, nn.name) : nn.name;
			nd.x = nn.x; nd.y = nn.y;
			nd.statsNew = nn.stats;
			diff.added.push_back(std::move(nd));
			continue;
		}
		const AtlasNode& on = oldTree.nodes[oit->second];
		std::vector<AtlasStatDelta> deltas;
		DiffStats(on.stats, nn.stats, i18nOld, i18nNew, deltas);
		bool nameChanged = on.name != nn.name;
		if (deltas.empty() && !nameChanged) continue; // unchanged node
		AtlasNodeDiff nd;
		nd.kind = AtlasNodeDiff::kModified;
		nd.id = nn.id; nd.nodeKind = nn.kind;
		nd.name = nn.name; nd.nameZh = i18nNew ? i18nNew->NodeName(nn.id, nn.name) : nn.name;
		nd.x = nn.x; nd.y = nn.y;
		nd.nameChanged = nameChanged;
		if (nameChanged) {
			nd.nameOld = on.name;
			nd.nameOldZh = i18nOld ? i18nOld->NodeName(on.id, on.name) : on.name;
		}
		nd.stats = std::move(deltas);
		nd.statsOld = on.stats;
		nd.statsNew = nn.stats;
		diff.modified.push_back(std::move(nd));
	}

	// removed (in old, not in new)
	for (const AtlasNode& on : oldTree.nodes) {
		if (newIdx.find(on.id) != newIdx.end()) continue;
		AtlasNodeDiff nd;
		nd.kind = AtlasNodeDiff::kRemoved;
		nd.id = on.id; nd.nodeKind = on.kind;
		nd.name = on.name; nd.nameZh = i18nOld ? i18nOld->NodeName(on.id, on.name) : on.name;
		nd.x = on.x; nd.y = on.y;
		nd.statsOld = on.stats;
		diff.removed.push_back(std::move(nd));
	}

	auto byId = [](const AtlasNodeDiff& a, const AtlasNodeDiff& b) { return a.id < b.id; };
	std::sort(diff.added.begin(), diff.added.end(), byId);
	std::sort(diff.removed.begin(), diff.removed.end(), byId);
	std::sort(diff.modified.begin(), diff.modified.end(), byId);
	return diff;
}

// ---- cross-season TC backfill -----------------------------------------------

int BackfillAtlasI18n(AtlasI18n& newI18n, const AtlasTreeData& newTree,
                      const AtlasI18n& oldI18n, const AtlasTreeData& oldTree)
{
	// exact English line -> old zh (only lines that actually have a translation)
	std::unordered_map<std::string, std::string> oldEnToZh;
	for (const AtlasNode& n : oldTree.nodes)
		for (const std::string& en : n.stats)
			if (oldI18n.HasStat(en))
				oldEnToZh.emplace(en, oldI18n.StatLine(en));

	int added = 0;
	for (const AtlasNode& n : newTree.nodes)
		for (const std::string& en : n.stats) {
			if (newI18n.HasStat(en)) continue;      // already translated this season
			auto it = oldEnToZh.find(en);           // EXACT match only — no number/wording change
			if (it == oldEnToZh.end()) continue;    // any change -> keep the new English
			newI18n.AddStat(en, it->second);
			added++;
		}
	return added;
}

int DropRenamedNames(AtlasI18n& newI18n, const AtlasTreeData& newTree, const AtlasTreeData& oldTree)
{
	std::unordered_map<int, std::string> oldName;
	for (const AtlasNode& on : oldTree.nodes) oldName[on.id] = on.name;
	int dropped = 0;
	for (const AtlasNode& nn : newTree.nodes) {
		auto it = oldName.find(nn.id);
		if (it != oldName.end() && it->second != nn.name) { newI18n.DropName(nn.id); dropped++; }
	}
	return dropped;
}

int PruneStaleTranslations(AtlasI18n& newI18n, const AtlasTreeData& newTree, const AtlasTreeData& oldTree)
{
	// every English stat line that already existed last season
	std::unordered_set<std::string> oldLines;
	for (const AtlasNode& on : oldTree.nodes)
		for (const std::string& s : on.stats) oldLines.insert(s);
	// lines the new tree actually uses (the zh file mirrors the tree, but be safe)
	std::unordered_set<std::string> newLines;
	for (const AtlasNode& nn : newTree.nodes)
		for (const std::string& s : nn.stats) newLines.insert(s);

	int dropped = 0;
	for (const std::string& en : newI18n.TranslatedStatLines()) {
		if (!newLines.count(en)) continue;   // not a tree line (defensive)
		if (oldLines.count(en)) continue;    // unchanged since last season: zh is trustworthy
		newI18n.DropStat(en);                // new/changed line with a stale zh -> English
		dropped++;
	}
	return dropped;
}

// ---- report -----------------------------------------------------------------

static const char* KindTag(int k)
{
	switch (k) {
	case kAtlasNotable: return "notable";
	case kAtlasKeystone: return "keystone";
	case kAtlasStart: return "start";
	case kAtlasWormhole: return "wormhole";
	default: return "normal";
	}
}

std::string FormatAtlasTreeDiffReport(const AtlasTreeDiff& diff)
{
	std::string s;
	char buf[512];
	snprintf(buf, sizeof(buf), "Atlas tree diff  %s -> %s\n", diff.oldVer.c_str(), diff.newVer.c_str());
	s += buf;
	snprintf(buf, sizeof(buf), "added %zu   removed %zu   modified %zu\n\n",
	         diff.added.size(), diff.removed.size(), diff.modified.size());
	s += buf;

	s += "== ADDED NODES ==\n";
	for (const AtlasNodeDiff& n : diff.added) {
		snprintf(buf, sizeof(buf), "  + [%s] %s (id %d)\n", KindTag(n.nodeKind), n.name.c_str(), n.id);
		s += buf;
		for (const std::string& line : n.statsNew) s += "      " + line + "\n";
	}

	s += "\n== REMOVED NODES ==\n";
	for (const AtlasNodeDiff& n : diff.removed) {
		snprintf(buf, sizeof(buf), "  - [%s] %s (id %d)\n", KindTag(n.nodeKind), n.name.c_str(), n.id);
		s += buf;
		for (const std::string& line : n.statsOld) s += "      " + line + "\n";
	}

	s += "\n== MODIFIED NODES ==\n";
	for (const AtlasNodeDiff& n : diff.modified) {
		snprintf(buf, sizeof(buf), "  ~ [%s] %s (id %d)\n", KindTag(n.nodeKind), n.name.c_str(), n.id);
		s += buf;
		if (n.nameChanged) s += "      renamed from: " + n.nameOld + "\n";
		for (const AtlasStatDelta& d : n.stats) {
			if (d.kind == AtlasStatDelta::kValueChanged)
				s += "      * " + d.enOld + "  ->  " + d.en + "\n";
			else if (d.kind == AtlasStatDelta::kLineAdded)
				s += "      + " + d.en + "\n";
			else
				s += "      - " + d.en + "\n";
		}
	}
	return s;
}

// ---- self-test (--atlas-diff-selftest) --------------------------------------

namespace {

AtlasNode MkNode(int id, int kind, const char* name, std::vector<std::string> stats)
{
	AtlasNode n;
	n.id = id; n.kind = kind; n.name = name; n.stats = std::move(stats);
	return n;
}

} // namespace

int RunAtlasDiffSelfTest(const std::wstring& exeDir)
{
	if (AttachConsole(ATTACH_PARENT_PROCESS)) {
		FILE* f = nullptr;
		freopen_s(&f, "CONOUT$", "w", stdout);
	}

	std::string rep;
	int failures = 0;
	auto check = [&](bool ok, const char* what) {
		rep += ok ? "PASS  " : "FAIL  ";
		rep += what;
		rep += "\n";
		if (!ok) failures++;
	};

	AtlasTreeData oldT, newT;
	// id 100: value change 15 -> 20
	oldT.nodes.push_back(MkNode(100, kAtlasNotable, "Expedition", { "15% increased Quantity of Items found in this Region" }));
	newT.nodes.push_back(MkNode(100, kAtlasNotable, "Expedition", { "20% increased Quantity of Items found in this Region" }));
	// id 101: unchanged (two lines, one with '+')
	oldT.nodes.push_back(MkNode(101, kAtlasNormal, "Steady", { "+1 to something", "Always relevant" }));
	newT.nodes.push_back(MkNode(101, kAtlasNormal, "Steady", { "+1 to something", "Always relevant" }));
	// id 102: removed (only in old)
	oldT.nodes.push_back(MkNode(102, kAtlasNotable, "Gone", { "10% increased Pack Size" }));
	// id 103: added (only in new)
	newT.nodes.push_back(MkNode(103, kAtlasKeystone, "Brand New", { "Fresh keystone effect" }));
	// id 104: line added (kept line unchanged, new extra line)
	oldT.nodes.push_back(MkNode(104, kAtlasNormal, "Grower", { "5% chance to do X" }));
	newT.nodes.push_back(MkNode(104, kAtlasNormal, "Grower", { "5% chance to do X", "20% increased effect of Y" }));
	// id 105: sign-aware value change (+8% -> +10%) plus a rename
	oldT.nodes.push_back(MkNode(105, kAtlasNotable, "Old Name", { "+8% to effect" }));
	newT.nodes.push_back(MkNode(105, kAtlasNotable, "New Name", { "+10% to effect" }));

	AtlasTreeDiff d = ComputeAtlasTreeDiff(oldT, newT, nullptr, nullptr, "3.28.0", "3.29.0");

	check(d.added.size() == 1 && d.added[0].id == 103, "one added node (id 103)");
	check(d.removed.size() == 1 && d.removed[0].id == 102, "one removed node (id 102)");
	check(d.modified.size() == 3, "three modified nodes (100,104,105)");

	// id 100: exactly one value-change delta 15 -> 20
	const AtlasNodeDiff* m100 = nullptr;
	for (const AtlasNodeDiff& n : d.modified) if (n.id == 100) m100 = &n;
	bool ok100 = m100 && m100->stats.size() == 1 &&
	             m100->stats[0].kind == AtlasStatDelta::kValueChanged &&
	             m100->stats[0].oldNums.size() == 1 && m100->stats[0].oldNums[0] == 15.0 &&
	             m100->stats[0].newNums.size() == 1 && m100->stats[0].newNums[0] == 20.0 &&
	             !m100->nameChanged;
	check(ok100, "id 100: single value change 15 -> 20, no rename");

	// id 101 must NOT appear (unchanged)
	bool has101 = false;
	for (const AtlasNodeDiff& n : d.modified) if (n.id == 101) has101 = true;
	check(!has101, "unchanged node (id 101) not reported");

	// id 104: exactly one line-added delta
	const AtlasNodeDiff* m104 = nullptr;
	for (const AtlasNodeDiff& n : d.modified) if (n.id == 104) m104 = &n;
	bool ok104 = m104 && m104->stats.size() == 1 && m104->stats[0].kind == AtlasStatDelta::kLineAdded;
	check(ok104, "id 104: single line-added delta");

	// id 105: value change (+8 -> +10) AND rename flagged
	const AtlasNodeDiff* m105 = nullptr;
	for (const AtlasNodeDiff& n : d.modified) if (n.id == 105) m105 = &n;
	bool ok105 = m105 && m105->nameChanged && m105->nameOld == "Old Name" &&
	             m105->stats.size() == 1 && m105->stats[0].kind == AtlasStatDelta::kValueChanged &&
	             m105->stats[0].oldNums.size() == 1 && m105->stats[0].oldNums[0] == 8.0 &&
	             m105->stats[0].newNums.size() == 1 && m105->stats[0].newNums[0] == 10.0;
	check(ok105, "id 105: sign-aware value change (+8 -> +10) with rename");

	// --- cross-season TC backfill: reuse the old translation ONLY for a
	// byte-identical line; any numeric or wording change stays new English ---
	{
		AtlasTreeData obt, nbt;
		obt.nodes.push_back(MkNode(200, kAtlasNotable, "Q",
			{ "15% increased Quantity of Items", "Grants an extra bonus" }));
		nbt.nodes.push_back(MkNode(200, kAtlasNotable, "Q",
			{ "20% increased Quantity of Items", "Grants an extra bonus" }));   // one value-changed, one unchanged
		nbt.nodes.push_back(MkNode(201, kAtlasNormal, "New", { "completely reworded effect" }));
		AtlasI18n oldZh, newZh;
		oldZh.AddStat("15% increased Quantity of Items", u8"增加 15% 物品數量");
		oldZh.AddStat("Grants an extra bonus", u8"給予額外加成");
		int added = BackfillAtlasI18n(newZh, nbt, oldZh, obt);
		check(added == 1, "backfill reuses exactly the one byte-identical line");
		check(newZh.StatLine("Grants an extra bonus") == u8"給予額外加成",
		      "backfill: unchanged line reuses the old Chinese verbatim");
		check(newZh.StatLine("20% increased Quantity of Items") == "20% increased Quantity of Items",
		      "backfill: value-changed line stays new English (no synthesis)");
		check(newZh.StatLine("completely reworded effect") == "completely reworded effect",
		      "backfill: reworded line stays new English");
	}

	// --- stale-translation prune: a line NEW this season whose zh predates the
	// season is a translation of the OLD wording -> drop (English); a line that
	// existed last season keeps its zh even when the wording styles differ
	// (e.g. English "an" vs Chinese "1 個" is a correct pair, never touched) ---
	{
		const char* kStale = "An additional Rare Monster is spawned from [ContainsAbyss|Abysses]"; // reworked in 3.29
		const char* kSame = "Your Maps have 5% chance to contain an additional Red Beast";          // unchanged line
		AtlasTreeData oldT3, newT3;
		oldT3.nodes.push_back(MkNode(400, kAtlasNotable, "A",
			{ "Abyss Pits in your Maps have 3% chance to spawn 5 additional Rare Monsters", kSame }));
		newT3.nodes.push_back(MkNode(400, kAtlasNotable, "A", { kStale, kSame }));
		AtlasI18n z3;
		z3.AddStat(kStale, u8"你地圖中的深淵坑洞有 3% 機率額外生成 5 個稀有怪物"); // repoe's stale pairing
		z3.AddStat(kSame, u8"你的地圖有 5% 機率含有 1 個額外紅色野獸");            // correct "an" vs "1 個"
		int pruned = PruneStaleTranslations(z3, newT3, oldT3);
		check(pruned == 1, "prune drops exactly the season-new line (stale zh)");
		check(z3.StatLine(kStale) == kStale, "reworked Abyss line falls back to English");
		check(z3.StatLine(kSame) == u8"你的地圖有 5% 機率含有 1 個額外紅色野獸",
		      "unchanged line keeps its Chinese (an vs 1-ge never mis-flagged)");
	}

	// --- renamed-node name drop (renamed -> new English name, not stale zh) ---
	{
		AtlasTreeData oldT2, newT2;
		oldT2.nodes.push_back(MkNode(300, kAtlasNotable, "Abyssal Depths Chance", {}));
		oldT2.nodes.push_back(MkNode(301, kAtlasNotable, "Steady Name", {}));
		newT2.nodes.push_back(MkNode(300, kAtlasNotable, "Abyss Kurgal Chance", {})); // renamed
		newT2.nodes.push_back(MkNode(301, kAtlasNotable, "Steady Name", {}));          // unchanged
		AtlasI18n z;
		z.AddName(300, u8"深淵深處機率");   // stale: translation of the OLD name
		z.AddName(301, u8"穩定名稱");
		int dropped = DropRenamedNames(z, newT2, oldT2);
		check(dropped == 1, "DropRenamedNames drops exactly the renamed node");
		check(z.NodeName(300, "Abyss Kurgal Chance") == "Abyss Kurgal Chance",
		      "renamed node falls back to the new English name");
		check(z.NodeName(301, "Steady Name") == u8"穩定名稱",
		      "unchanged node keeps its Chinese name");
	}

	rep += failures == 0 ? "\nALL PASS\n" : "\nFAILURES: " + std::to_string(failures) + "\n";
	rep += "\n--- sample report ---\n" + FormatAtlasTreeDiffReport(d);
	printf("%s", rep.c_str());
	write_file_utf8(exeDir + L"atlas_diff_selftest.txt", rep);
	return failures == 0 ? 0 : 1;
}

// ---- CLI (--atlas-diff <oldVer> <newVer>) -----------------------------------

int RunAtlasDiffCli(const std::wstring& oldVer, const std::wstring& newVer, const std::wstring& exeDir)
{
	if (AttachConsole(ATTACH_PARENT_PROCESS)) {
		FILE* f = nullptr;
		freopen_s(&f, "CONOUT$", "w", stdout);
	}
	std::string oldTag(oldVer.begin(), oldVer.end());
	std::string newTag(newVer.begin(), newVer.end());
	if (oldTag.empty() || newTag.empty()) {
		printf("usage: --atlas-diff <oldVer> <newVer>  (e.g. 3.28.0 3.29.0)\n");
		return 1;
	}

	AtlasTreeData oldT, newT;
	std::string err;
	if (!oldT.LoadVersion(exeDir, oldTag, &err)) {
		printf("failed to load %s: %s\n", oldTag.c_str(), err.c_str());
		return 1;
	}
	if (!newT.LoadVersion(exeDir, newTag, &err)) {
		printf("failed to load %s: %s\n", newTag.c_str(), err.c_str());
		return 1;
	}
	AtlasI18n zhOld, zhNew;
	bool hasZhOld = zhOld.LoadVersion(exeDir, oldTag);
	bool hasZhNew = zhNew.LoadVersion(exeDir, newTag);

	AtlasTreeDiff diff = ComputeAtlasTreeDiff(oldT, newT, hasZhOld ? &zhOld : nullptr,
	                                          hasZhNew ? &zhNew : nullptr, oldTag, newTag);
	std::string report = FormatAtlasTreeDiffReport(diff);
	printf("%s", report.c_str());
	write_file_utf8(exeDir + L"atlas_diff_report.txt", report);
	return 0;
}
