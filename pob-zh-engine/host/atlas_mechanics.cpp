#include "atlas_mechanics.h"

#include "atlas_tree_data.h"      // selftest: resolve node ids against a real season
#include "atlas_version_index.h"  // VersionDir / ResolveDataDir

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <json.hpp> // nlohmann::ordered_json (deps/nlohmann)

#include <algorithm>
#include <cstdio>
#include <set>
#include <unordered_map>

using nlohmann::ordered_json;

// ---- catalogue ---------------------------------------------------------------
//
// Generated once from the 3.29.1 atlastree-export (mastery names + icon tokens)
// joined to the local GGPK 3.29.0.4.2:
//   id  = PassiveSkills.AtlasGroup -> AtlasPassiveSkillTreeGroupType.Id
//   zh  = the Traditional-Chinese PassiveSkills table at the SAME row index
//         (the locale tables are row-aligned; this is the join gen_scarabs.py
//         already relies on -- never a text match)
// Where a mechanic has several mastery rows the shortest zh wins; the longer
// ones are league-variant labels ("獸獵聯盟 （標準）") and every one of them was
// verified to start with the short form, so this is a rule, not a pick.
//
// ⚠ "Labyrinth" really is 帝王迷宮機率 in the game files -- GGG named that single
// mastery row after the stat rather than the mechanic. Left verbatim: the game
// shows that string, and inventing 帝王迷宮 would be exactly the kind of guess
// this project does not make.
static const std::vector<AtlasMechanicDef>& catalogue()
{
	static const std::vector<AtlasMechanicDef> kDefs = {
		{ "Abyss", "Abyss", u8"Abyss", u8"深淵" },
		{ "Memory", "Memories", u8"Atlas Memories", u8"輿圖記憶" },
		{ "Bestiary", "Bestiary", u8"Bestiary", u8"獸獵" },
		{ "Betrayal", "Jun", u8"Betrayal", u8"反叛" },
		{ "Beyond", "Beyond", u8"Beyond", u8"超越" },
		{ "Blight", "Blight", u8"Blight", u8"凋落" },
		{ "Breach", "Breach", u8"Breach", u8"裂痕" },
		{ "Conqueror", "Conqueror", u8"Conquerors", u8"征服者" },
		{ "Delirium", "Delirium", u8"Delirium", u8"譫妄" },
		{ "Delve", "Delve", u8"Delve", u8"掘獄" },
		{ "Divination", "Divination", u8"Divination Cards", u8"命運卡" },
		{ "Essence", "Essence", u8"Essence", u8"精髓" },
		{ "Expedition", "Expedition", u8"Expedition", u8"探險" },
		{ "Harvest", "Harvest", u8"Harvest", u8"豐收" },
		{ "Heist", "Heist", u8"Heist", u8"劫盜" },
		{ "Incursion", "Alva", u8"Incursion", u8"穿越" },
		{ "Labyrinth", "Labyrinth", u8"Labyrinth", u8"帝王迷宮機率" },
		{ "Legion", "Legion", u8"Legion", u8"戰亂" },
		{ "Map Tier", "Map", u8"Maps", u8"地圖" },
		{ "Mercenary", "Mercenaries", u8"Mercenaries", u8"傭兵" },
		{ "Ritual", "Ritual", u8"Ritual", u8"祭祀" },
		{ "Anarchy", "Anarchy", u8"Rogue Exiles", u8"盜賊流亡者" },
		{ "Scarab", "Scarab", u8"Scarabs", u8"聖甲蟲" },
		{ "Kalguur", "Settlers", u8"Settlers of Kalguur", u8"卡爾葛拓荒者" },
		{ "Domination", "Domination", u8"Shrines", u8"神殿" },
		{ "Strongbox", "Strongbox", u8"Strongboxes", u8"保險箱" },
		{ "Synthesis", "Synthesis", u8"Synthesis", u8"追憶" },
		{ "Tangle", "Tangle", u8"The Eater of Worlds", u8"吞噬天地" },
		{ "CleansingFire", "CleansingFire", u8"The Searing Exarch", u8"灼烙總督" },
		{ "ElderShaper", "ElderShaper", u8"The Shaper and Elder", u8"塑界者和尊師" },
		{ "Torment", "Torment", u8"Torment", u8"罪魂" },
		{ "Ultimatum", "Ultimatum", u8"Ultimatum", u8"通牒" },
		{ "Vaal", "Vaal", u8"Vaal Side Areas", u8"瓦爾區域" },
	};
	return kDefs;
}

// Extra icon-filename spellings for rule 2. GGG does not always name a node's
// icon after the mastery icon (Settlers nodes are KalguurNode, Bestiary notables
// are AtlasEinharNotable), so these map the alternative spellings onto the same
// mechanic. Matched case-sensitively, which is what keeps three-letter tokens
// like "Map" from firing inside unrelated lowercase names.
struct MechAlias { const char* token; const char* id; };
static const MechAlias kAliases[] = {
	{ "Kalguur", "Kalguur" },
	{ "Shrines", "Domination" },
	{ "Einhar", "Bestiary" },
	{ "Betrayal", "Betrayal" },
	{ "Incursion", "Incursion" },
	{ "Exarch", "CleansingFire" },
	{ "Eater", "Tangle" },
	{ "RogueExile", "Anarchy" },
	{ "Memory", "Memory" },
	{ "Mercenary", "Mercenary" },
	{ "DivinationCard", "Divination" },
};

const std::vector<AtlasMechanicDef>& AtlasMechanicCatalogue() { return catalogue(); }

const AtlasMechanicDef* AtlasMechanicById(const std::string& id)
{
	for (const AtlasMechanicDef& d : catalogue())
		if (d.id == id) return &d;
	return nullptr;
}

const AtlasMechanicDef* AtlasMechanicByEn(const std::string& en)
{
	for (const AtlasMechanicDef& d : catalogue())
		if (d.en == en) return &d;
	return nullptr;
}

double AtlasMechanicBacktestGate() { return 93.0; }

// ---- classification ----------------------------------------------------------

// "Art/2DArt/SkillIcons/passives/AtlasTrees/NewBreachNode.png" -> "NewBreachNode"
static std::string icon_stem(const std::string& path)
{
	size_t slash = path.find_last_of("/\\");
	std::string base = slash == std::string::npos ? path : path.substr(slash + 1);
	size_t dot = base.find_last_of('.');
	return dot == std::string::npos ? base : base.substr(0, dot);
}

// Rule 2. Returns "" when no token matches or when two different mechanics do --
// an ambiguous icon (ScarabLeaguesRitual is both) is left unassigned rather than
// resolved by coin flip.
static std::string mechanic_from_icon(const std::string& stem)
{
	std::string hit;
	for (const AtlasMechanicDef& d : catalogue()) {
		if (d.token.empty() || stem.find(d.token) == std::string::npos) continue;
		if (!hit.empty() && hit != d.id) return std::string();
		hit = d.id;
	}
	for (const MechAlias& a : kAliases) {
		if (stem.find(a.token) == std::string::npos) continue;
		if (!hit.empty() && hit != a.id) return std::string();
		hit = a.id;
	}
	return hit;
}

bool AtlasBuildMechanicMap(const std::string& dataJson, const std::string& tag,
                           AtlasMechanicMap* out, std::string* err)
{
	auto fail = [&](const std::string& m) {
		if (err) *err = m;
		return false;
	};
	if (!out) return fail("no output");
	*out = AtlasMechanicMap();
	out->tag = tag;

	ordered_json doc;
	try {
		doc = ordered_json::parse(dataJson);
	} catch (const std::exception& e) {
		return fail(std::string(u8"data.json 解析失敗: ") + e.what());
	}
	if (!doc.contains("nodes") || !doc["nodes"].is_object())
		return fail(u8"data.json 缺少 nodes（不是 atlastree-export 格式？）");

	// pass 1: which mechanic owns each group, and how many clusters it has
	std::unordered_map<std::string, std::string> groupMech; // group id -> mechanic id
	std::unordered_map<std::string, int> clusters;
	std::set<std::string> unknown;
	for (const auto& [key, v] : doc["nodes"].items()) {
		if (key == "root" || !v.value("isMastery", false)) continue;
		std::string name = v.value("name", std::string());
		const AtlasMechanicDef* def = AtlasMechanicByEn(name);
		if (!def) { unknown.insert(name); continue; }
		if (!v.contains("group")) continue;
		groupMech[v["group"].dump()] = def->id;
		clusters[def->id]++;
	}
	out->unknownMasteries.assign(unknown.begin(), unknown.end());

	// pass 2: classify the real nodes
	std::unordered_map<std::string, std::vector<int>> byMech;
	for (const auto& [key, v] : doc["nodes"].items()) {
		if (key == "root" || v.value("isMastery", false)) continue;
		int id = v.value("skill", 0);
		if (id == 0) continue;
		std::string guess = mechanic_from_icon(icon_stem(v.value("icon", std::string())));
		std::string truth;
		if (v.contains("group")) {
			auto it = groupMech.find(v["group"].dump());
			if (it != groupMech.end()) truth = it->second;
		}
		if (!truth.empty()) {
			// Rule 1 wins outright; rule 2's opinion only feeds the back-test.
			out->fromCluster++;
			if (!guess.empty()) {
				out->backtestTotal++;
				if (guess == truth) out->backtestAgree++;
			}
			byMech[truth].push_back(id);
		} else if (!guess.empty()) {
			out->fromIcon++;
			byMech[guess].push_back(id);
		} else {
			out->unassigned++;
		}
	}

	if (out->fromCluster == 0)
		return fail(u8"data.json 沒有任何節點能對到機制叢集，已中止");
	if (out->backtestTotal > 0 && out->backtestPct() < AtlasMechanicBacktestGate()) {
		char buf[160];
		snprintf(buf, sizeof(buf),
			u8"機制圖示規則的回測一致率只有 %.1f%%（門檻 %.1f%%），"
			u8"GGG 可能改了圖示命名，已中止",
			out->backtestPct(), AtlasMechanicBacktestGate());
		return fail(buf);
	}

	for (auto& [id, ids] : byMech) {
		std::sort(ids.begin(), ids.end());
		ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
		AtlasMechanicMap::Group g;
		g.id = id;
		g.nodeIds = std::move(ids);
		auto ct = clusters.find(id);
		g.clusters = ct == clusters.end() ? 0 : ct->second;
		out->groups.push_back(std::move(g));
	}
	std::sort(out->groups.begin(), out->groups.end(),
	          [](const AtlasMechanicMap::Group& a, const AtlasMechanicMap::Group& b) {
		          return a.id < b.id;
	          });
	return true;
}

std::string AtlasMechanicMapToJson(const AtlasMechanicMap& m)
{
	ordered_json doc;
	doc["format"] = "pobtools-atlas-mechanics";
	doc["tag"] = m.tag;
	doc["fromCluster"] = m.fromCluster;
	doc["fromIcon"] = m.fromIcon;
	doc["unassigned"] = m.unassigned;
	doc["backtest"] = m.backtestAgree;
	doc["backtestOf"] = m.backtestTotal;
	ordered_json arr = ordered_json::array();
	for (const AtlasMechanicMap::Group& g : m.groups) {
		ordered_json jg;
		jg["id"] = g.id;
		jg["clusters"] = g.clusters;
		jg["nodes"] = g.nodeIds;
		arr.push_back(std::move(jg));
	}
	doc["mechanics"] = std::move(arr);
	return doc.dump(1, '\t');
}

static bool mech_write_file(const std::wstring& path, const std::string& body)
{
	HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
	                       FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE) return false;
	DWORD written = 0;
	bool ok = body.empty() ||
		(WriteFile(h, body.data(), (DWORD)body.size(), &written, nullptr) && written == body.size());
	CloseHandle(h);
	return ok;
}

static bool mech_read_file(const std::wstring& path, std::string& out)
{
	HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
	if (h == INVALID_HANDLE_VALUE) return false;
	LARGE_INTEGER size{};
	bool ok = false;
	if (GetFileSizeEx(h, &size) && size.QuadPart >= 0 && size.QuadPart < (1ll << 26)) {
		out.resize((size_t)size.QuadPart);
		DWORD read = 0;
		ok = out.empty() || (ReadFile(h, &out[0], (DWORD)out.size(), &read, nullptr) && read == out.size());
		if (!ok) out.clear();
	}
	CloseHandle(h);
	return ok;
}

bool AtlasWriteMechanicMap(const AtlasMechanicMap& m, const std::wstring& destDir, std::string* err)
{
	std::wstring dir = destDir;
	if (!dir.empty() && dir.back() != L'\\' && dir.back() != L'/') dir += L'\\';
	if (!mech_write_file(dir + L"atlas_mechanics.json", AtlasMechanicMapToJson(m))) {
		if (err) *err = u8"無法寫入 atlas_mechanics.json";
		return false;
	}
	return true;
}

std::string AtlasMechanicMapReport(const AtlasMechanicMap& m)
{
	char buf[320];
	snprintf(buf, sizeof(buf),
		"tag=%s  mechanics=%d  cluster-rule=%d  icon-rule=%d  unassigned=%d  "
		"backtest=%d/%d (%.1f%%, gate %.1f%%)\n",
		m.tag.c_str(), (int)m.groups.size(), m.fromCluster, m.fromIcon, m.unassigned,
		m.backtestAgree, m.backtestTotal, m.backtestPct(), AtlasMechanicBacktestGate());
	std::string rep = buf;
	if (!m.unknownMasteries.empty()) {
		rep += "  mastery names not in the catalogue (shown untranslated):";
		for (const std::string& s : m.unknownMasteries) rep += " " + s;
		rep += "\n";
	}
	for (const AtlasMechanicMap::Group& g : m.groups) {
		const AtlasMechanicDef* d = AtlasMechanicById(g.id);
		snprintf(buf, sizeof(buf), "  %-16s %3d nodes  %2d clusters  %s\n",
			g.id.c_str(), (int)g.nodeIds.size(), g.clusters, d ? d->en.c_str() : "?");
		rep += buf;
	}
	return rep;
}

// ---- runtime db ---------------------------------------------------------------

bool AtlasMechanicDb::Load(const std::wstring& exeDir, const std::string& tag)
{
	borrowedFrom_.clear();
	if (loadOne(exeDir, tag)) return true;
	// Fall back to the newest season that has a map. Everything is keyed by GGG
	// skill id, which is stable across seasons, so a borrowed map is correct for
	// every node that existed then and simply silent about the rest.
	AtlasVersionIndex idx;
	idx.Load(exeDir);
	for (const std::string& t : idx.TagsNewestFirst()) {
		if (t == tag) continue;
		if (!loadOne(exeDir, t)) continue;
		borrowedFrom_ = t;
		return true;
	}
	return false;
}

bool AtlasMechanicDb::loadOne(const std::wstring& exeDir, const std::string& tag)
{
	entries_.clear();
	tag_.clear();
	unassigned_ = 0;

	std::wstring path = tag.empty()
		? exeDir + L"Data\\atlas_mechanics.json"
		: AtlasVersionIndex::VersionDir(exeDir, tag) + L"atlas_mechanics.json";
	std::string body;
	if (!mech_read_file(path, body)) return false;
	try {
		ordered_json doc = ordered_json::parse(body);
		if (doc.value("format", std::string()) != "pobtools-atlas-mechanics") return false;
		tag_ = doc.value("tag", std::string());
		unassigned_ = doc.value("unassigned", 0);
		if (!doc.contains("mechanics") || !doc["mechanics"].is_array()) return false;
		for (const auto& jg : doc["mechanics"]) {
			if (!jg.is_object()) continue;
			Entry e;
			e.def = AtlasMechanicById(jg.value("id", std::string()));
			if (!e.def) continue;   // a mechanic this build does not know: skip, never guess
			e.clusters = jg.value("clusters", 0);
			if (jg.contains("nodes") && jg["nodes"].is_array())
				for (const auto& n : jg["nodes"])
					if (n.is_number_integer()) e.nodeIds.push_back(n.get<int>());
			if (e.nodeIds.empty()) continue;
			entries_.push_back(std::move(e));
		}
	} catch (...) {
		entries_.clear();
		return false;
	}
	// Chinese display order is not something we can collate properly, so sort by
	// the stable English name -- the same order in every language.
	std::sort(entries_.begin(), entries_.end(),
	          [](const Entry& a, const Entry& b) { return a.def->en < b.def->en; });
	return !entries_.empty();
}

// ---- maintainer CLI ------------------------------------------------------------

int RunAtlasMechanicBuild(const std::wstring& dataJsonPath, const std::string& tag,
                          const std::wstring& destDir)
{
	if (AttachConsole(ATTACH_PARENT_PROCESS)) {
		FILE* f = nullptr;
		freopen_s(&f, "CONOUT$", "w", stdout);
	}
	// A WIN32-subsystem exe cannot be piped, so the report also lands in a file
	// next to the data.json -- same convention as every selftest here.
	std::string rep;
	auto finish = [&](int code) {
		printf("%s", rep.c_str());
		mech_write_file(dataJsonPath + L".mechanics.txt", rep);
		return code;
	};

	std::string body;
	if (!mech_read_file(dataJsonPath, body)) {
		rep += "cannot read data.json\n";
		return finish(2);
	}
	AtlasMechanicMap m;
	std::string err;
	if (!AtlasBuildMechanicMap(body, tag, &m, &err)) {
		rep += "FAIL: " + err + "\n";
		return finish(1);
	}
	rep += AtlasMechanicMapReport(m);
	if (!destDir.empty()) {
		if (!AtlasWriteMechanicMap(m, destDir, &err)) {
			rep += "FAIL: " + err + "\n";
			return finish(1);
		}
		rep += "written to destDir\n";
	}
	return finish(0);
}

// ---- self-test ------------------------------------------------------------------

int RunAtlasMechanicSelfTest(const std::wstring& exeDir, std::string& out)
{
	int fails = 0;
	auto check = [&](bool ok, const std::string& what, const std::string& note = std::string()) {
		out += ok ? "PASS  " : "FAIL  ";
		out += what;
		if (!note.empty()) out += "  (" + note + ")";
		out += "\n";
		if (!ok) fails++;
	};

	// --- the compiled catalogue -------------------------------------------------
	const std::vector<AtlasMechanicDef>& defs = catalogue();
	check(defs.size() >= 30, "mechanic catalogue is populated", std::to_string(defs.size()) + " mechanics");
	std::set<std::string> ids, ens, tokens;
	int blank = 0, untranslated = 0;
	for (const AtlasMechanicDef& d : defs) {
		ids.insert(d.id);
		ens.insert(d.en);
		tokens.insert(d.token);
		if (d.id.empty() || d.en.empty() || d.zh.empty() || d.token.empty()) blank++;
		if (d.zh == d.en) untranslated++;
	}
	check(ids.size() == defs.size(), "mechanic ids are unique");
	check(ens.size() == defs.size(), "mastery names are unique (they are the join key)");
	check(tokens.size() == defs.size(), "icon tokens are unique");
	check(blank == 0, "no blank field in the catalogue");
	check(untranslated == 0, "every mechanic has a Chinese name distinct from English",
	      std::to_string(untranslated) + " untranslated");
	check(AtlasMechanicById("Breach") && AtlasMechanicByEn("Breach"), "lookup by id and by name works");
	// An alias must not point at a mechanic this build does not have.
	int badAlias = 0;
	for (const MechAlias& a : kAliases)
		if (!AtlasMechanicById(a.id)) badAlias++;
	check(badAlias == 0, "every icon alias resolves to a catalogue entry");
	// Ambiguity guard: the whole point of returning "" on a two-mechanic match.
	check(mechanic_from_icon("NewBreachNode") == "Breach", "icon rule finds Breach");
	check(mechanic_from_icon("ScarabLeaguesRitual").empty(), "an icon naming two mechanics is left unassigned");
	check(mechanic_from_icon("ModifierTier").empty(), "a generic icon is left unassigned");
	check(mechanic_from_icon("newbreachnode").empty(), "matching is case-sensitive (CamelCase boundary)");

	// --- against the season files actually installed -----------------------------
	AtlasVersionIndex idx;
	idx.Load(exeDir);
	std::vector<std::string> tags = idx.TagsNewestFirst();
	if (tags.empty()) tags.push_back(std::string());
	int loaded = 0;
	for (const std::string& tag : tags) {
		AtlasMechanicDb db;
		if (!db.Load(exeDir, tag)) {
			out += "      " + (tag.empty() ? std::string("(flat)") : tag) +
			       ": no atlas_mechanics.json - skipped\n";
			continue;
		}
		if (!db.BorrowedFrom().empty()) {
			// Nothing to assert against this season's tree: the map is another
			// season's, deliberately. Say so instead of testing the wrong thing.
			out += "      " + tag + ": no map of its own, borrowing " + db.BorrowedFrom() + "\n";
			continue;
		}
		loaded++;
		AtlasTreeData d;
		std::string err;
		bool haveTree = tag.empty() ? d.Load(exeDir, &err) : d.LoadVersion(exeDir, tag, &err);
		std::set<int> treeIds;
		if (haveTree)
			for (const AtlasNode& n : d.nodes) treeIds.insert(n.id);

		int emptyGroups = 0, unknownIds = 0, total = 0;
		std::set<int> seen;
		int dupes = 0;
		for (const AtlasMechanicDb::Entry& e : db.Entries()) {
			if (e.nodeIds.empty()) emptyGroups++;
			for (int id : e.nodeIds) {
				total++;
				if (!seen.insert(id).second) dupes++;
				if (haveTree && !treeIds.count(id)) unknownIds++;
			}
		}
		check(emptyGroups == 0, tag + ": every mechanic has at least one node");
		check(dupes == 0, tag + ": no node is claimed by two mechanics",
		      std::to_string(dupes) + " duplicated");
		if (haveTree) {
			check(unknownIds == 0, tag + ": every listed node id exists in that season's tree",
			      std::to_string(unknownIds) + " unknown of " + std::to_string(total));
			// Coverage is expected to be partial (groups without a mastery), but a
			// collapse would mean the classifier silently stopped working.
			int nodes = (int)d.nodes.size();
			bool sane = nodes > 0 && total * 100 / nodes >= 70;
			check(sane, tag + ": mechanic coverage is plausible",
			      std::to_string(total) + "/" + std::to_string(nodes) + " nodes");
			// Every mastery icon on the tree should resolve to a catalogue entry,
			// otherwise the canvas has clickable markers with no mechanic behind them.
			int orphanMastery = 0;
			for (const AtlasDeco& m : d.masteries)
				if (!AtlasMechanicByEn(m.name)) orphanMastery++;
			check(orphanMastery == 0, tag + ": every mastery icon maps to a known mechanic",
			      std::to_string(orphanMastery) + " orphaned");
		}
	}
	if (loaded == 0)
		out += "      no season has a mechanic file yet - run --atlas-mechanics-build\n";
	return fails;
}
