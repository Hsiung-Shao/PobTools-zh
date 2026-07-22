#include "atlas_tree_data.h"
#include "atlas_i18n.h"    // selftest T6: zh-mapping health check
#include "atlas_persist.h" // multi-build schema + share codes (T7)
#include "atlas_version_index.h" // season resolution (versioned data layout)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <json.hpp> // nlohmann::ordered_json (deps/nlohmann)

#include <algorithm>
#include <cstdio>
#include <deque>
#include <set>

using nlohmann::ordered_json;

// ---- file helpers (same conventions as economy_service.cpp) ----------------

static bool read_file_utf8(const std::wstring& path, std::string& out)
{
	HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
	if (h == INVALID_HANDLE_VALUE) return false;
	LARGE_INTEGER size{};
	bool ok = false;
	if (GetFileSizeEx(h, &size) && size.QuadPart >= 0 && size.QuadPart < (1ll << 30)) {
		out.resize((size_t)size.QuadPart);
		DWORD read = 0;
		ok = out.empty() || (ReadFile(h, &out[0], (DWORD)out.size(), &read, nullptr) && read == out.size());
		if (!ok) out.clear();
	}
	CloseHandle(h);
	return ok;
}

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

// ---- json parsing -----------------------------------------------------------

static bool parse_sprite(const ordered_json& j, AtlasSpriteRef& out)
{
	if (!j.is_object() || !j.contains("uv") || !j["uv"].is_array() || j["uv"].size() != 4) return false;
	out.sheet = j.value("s", 0);
	for (int i = 0; i < 4; i++) out.uv[i] = j["uv"][i].get<float>();
	out.w = j.value("w", 0.0f);
	out.h = j.value("h", 0.0f);
	return true;
}

static bool parse_deco(const ordered_json& j, AtlasDeco& d)
{
	if (!parse_sprite(j, d.spr)) return false;
	d.name = j.value("n", std::string());
	d.x = j.value("x", 0.0f);
	d.y = j.value("y", 0.0f);
	d.half = j.value("half", 0) != 0;
	return true;
}

bool AtlasTreeData::Load(const std::wstring& exeDir, std::string* err)
{
	AtlasVersionIndex idx;
	idx.Load(exeDir);
	std::wstring dir = idx.ResolveDataDir(exeDir, std::string());
	return loadFromDir(dir, idx.Active(), err);
}

bool AtlasTreeData::LoadVersion(const std::wstring& exeDir, const std::string& tag, std::string* err)
{
	AtlasVersionIndex idx;
	idx.Load(exeDir);
	std::wstring dir = idx.ResolveDataDir(exeDir, tag);
	return loadFromDir(dir, tag, err);
}

bool AtlasTreeData::loadFromDir(const std::wstring& dataDir, const std::string& tag, std::string* err)
{
	nodes.clear(); edges.clear(); sheets.clear(); masteries.clear(); groupBg.clear();
	root = -1;
	tag_ = tag;
	dataDir_ = dataDir;
	spriteDir_ = dataDir + L"atlas\\";

	std::string content;
	if (!read_file_utf8(dataDir + L"atlas_tree_poe1.json", content)) {
		if (err) *err = "atlas_tree_poe1.json not found or unreadable";
		return false;
	}
	try {
		ordered_json doc = ordered_json::parse(content);

		version_ = doc.value("version", std::string());
		totalPoints_ = doc.value("totalPoints", 0);
		root = doc.value("root", -1);
		const ordered_json& b = doc.at("bounds");
		minX = b.value("minX", 0.0f); minY = b.value("minY", 0.0f);
		maxX = b.value("maxX", 0.0f); maxY = b.value("maxY", 0.0f);

		for (const auto& js : doc.at("sheets")) {
			AtlasSheet s;
			s.file = js.value("file", std::string());
			s.w = js.value("w", 0); s.h = js.value("h", 0);
			sheets.push_back(std::move(s));
		}

		nodes.reserve(doc.at("nodes").size());
		for (const auto& jn : doc.at("nodes")) {
			AtlasNode n;
			n.id = jn.value("id", 0);
			n.name = jn.value("name", std::string());
			n.kind = jn.value("kind", 0);
			n.x = jn.value("x", 0.0f); n.y = jn.value("y", 0.0f);
			if (jn.contains("stats"))
				for (const auto& st : jn["stats"]) n.stats.push_back(st.get<std::string>());
			if (!parse_sprite(jn.at("on"), n.on) || !parse_sprite(jn.at("off"), n.off))
				throw std::runtime_error("bad node sprite");
			nodes.push_back(std::move(n));
		}

		edges.reserve(doc.at("edges").size());
		for (const auto& je : doc.at("edges")) {
			AtlasEdge e;
			e.a = je.value("a", -1); e.b = je.value("b", -1);
			if (e.a < 0 || e.b < 0 || e.a >= (int)nodes.size() || e.b >= (int)nodes.size())
				throw std::runtime_error("edge endpoint out of range");
			e.wormhole = je.value("w", 0) != 0;
			if (je.contains("arc") && je["arc"].is_array() && je["arc"].size() == 5) {
				e.hasArc = true;
				e.cx = je["arc"][0].get<float>(); e.cy = je["arc"][1].get<float>();
				e.r = je["arc"][2].get<float>();
				e.a0 = je["arc"][3].get<float>(); e.sweep = je["arc"][4].get<float>();
			}
			nodes[e.a].adj.push_back(e.b);
			nodes[e.b].adj.push_back(e.a);
			edges.push_back(e);
		}

		if (doc.contains("frames"))
			for (const auto& [key, jf] : doc["frames"].items()) {
				int kind = atoi(key.c_str());
				if (kind < 0 || kind > 4) continue;
				parse_sprite(jf.at("off"), frames[kind].off);
				parse_sprite(jf.at("path"), frames[kind].path);
				parse_sprite(jf.at("on"), frames[kind].on);
			}

		if (doc.contains("masteries"))
			for (const auto& jm : doc["masteries"]) {
				AtlasDeco d;
				if (parse_deco(jm, d)) masteries.push_back(std::move(d));
			}
		if (doc.contains("groupbg"))
			for (const auto& jg : doc["groupbg"]) {
				AtlasDeco d;
				if (parse_deco(jg, d)) groupBg.push_back(std::move(d));
			}
		hasBg = doc.contains("bg") && parse_sprite(doc["bg"], bg);
	} catch (const std::exception& e) {
		if (err) *err = std::string("atlas_tree_poe1.json parse error: ") + e.what();
		return false;
	}

	if (root < 0 || root >= (int)nodes.size() || nodes.empty()) {
		if (err) *err = "atlas tree data has no valid start node";
		return false;
	}
	nodes[root].alloc = true;
	return true;
}

// ---- allocation logic --------------------------------------------------------

int AtlasTreeData::UsedPoints() const
{
	int n = 0;
	for (const AtlasNode& nd : nodes)
		if (nd.alloc && nd.kind != kAtlasStart) n++;
	return n;
}

std::vector<int> AtlasTreeData::FindPathTo(int target) const
{
	if (target < 0 || target >= (int)nodes.size() || nodes[target].alloc) return {};

	std::vector<int> dist(nodes.size(), -1), prev(nodes.size(), -1);
	std::deque<int> q;
	for (int i = 0; i < (int)nodes.size(); i++)
		if (nodes[i].alloc) { dist[i] = 0; q.push_back(i); }

	while (!q.empty()) {
		int cur = q.front(); q.pop_front();
		for (int nb : nodes[cur].adj) {
			if (dist[nb] != -1 || nodes[nb].alloc) continue;
			dist[nb] = dist[cur] + 1;
			prev[nb] = cur;
			if (nb == target) {
				std::vector<int> path;
				for (int at = target; at != -1 && !nodes[at].alloc; at = prev[at])
					path.push_back(at);
				std::reverse(path.begin(), path.end());
				return path;
			}
			q.push_back(nb);
		}
	}
	return {};
}

std::vector<int> AtlasTreeData::FindRemoveSet(int idx) const
{
	if (idx < 0 || idx >= (int)nodes.size()) return {};
	if (!nodes[idx].alloc || nodes[idx].kind == kAtlasStart) return {};

	// Reachability from the start node across the allocated set minus idx.
	std::vector<char> reach(nodes.size(), 0);
	std::deque<int> q;
	reach[root] = 1;
	q.push_back(root);
	while (!q.empty()) {
		int cur = q.front(); q.pop_front();
		for (int nb : nodes[cur].adj) {
			if (reach[nb] || !nodes[nb].alloc || nb == idx) continue;
			reach[nb] = 1;
			q.push_back(nb);
		}
	}

	std::vector<int> removeSet;
	for (int i = 0; i < (int)nodes.size(); i++)
		if (nodes[i].alloc && !reach[i] && nodes[i].kind != kAtlasStart)
			removeSet.push_back(i);
	return removeSet; // idx itself is unreachable by construction, so included
}

void AtlasTreeData::Alloc(const std::vector<int>& path)
{
	for (int i : path)
		if (i >= 0 && i < (int)nodes.size()) nodes[i].alloc = true;
}

void AtlasTreeData::Remove(const std::vector<int>& removeSet)
{
	for (int i : removeSet)
		if (i >= 0 && i < (int)nodes.size() && nodes[i].kind != kAtlasStart)
			nodes[i].alloc = false;
}

void AtlasTreeData::Reset()
{
	for (AtlasNode& n : nodes) n.alloc = (n.kind == kAtlasStart);
	if (root >= 0) nodes[root].alloc = true;
}

void AtlasTreeData::repairConnectivity()
{
	std::vector<char> reach(nodes.size(), 0);
	std::deque<int> q;
	reach[root] = 1;
	q.push_back(root);
	while (!q.empty()) {
		int cur = q.front(); q.pop_front();
		for (int nb : nodes[cur].adj) {
			if (reach[nb] || !nodes[nb].alloc) continue;
			reach[nb] = 1;
			q.push_back(nb);
		}
	}
	for (int i = 0; i < (int)nodes.size(); i++)
		if (nodes[i].alloc && !reach[i] && nodes[i].kind != kAtlasStart)
			nodes[i].alloc = false;
}

// ---- build persistence --------------------------------------------------------

int AtlasTreeData::ApplyAllocIds(const std::vector<int>& ids)
{
	// GGG skill ids are the stable key; ids that no longer exist (data
	// regenerated for a new league) are silently dropped, then the
	// connectivity repair prunes anything that came loose.
	std::vector<std::pair<int, int>> id2idx; // sorted (id, index)
	id2idx.reserve(nodes.size());
	for (int i = 0; i < (int)nodes.size(); i++) id2idx.push_back({ nodes[i].id, i });
	std::sort(id2idx.begin(), id2idx.end());

	Reset();
	int mapped = 0;
	for (int id : ids) {
		auto it = std::lower_bound(id2idx.begin(), id2idx.end(), std::make_pair(id, 0));
		if (it != id2idx.end() && it->first == id) {
			nodes[it->second].alloc = true;
			mapped++;
		}
	}
	repairConnectivity();
	return mapped;
}

std::vector<int> AtlasTreeData::AllocIds() const
{
	std::vector<int> out;
	for (const AtlasNode& n : nodes)
		if (n.alloc && n.kind != kAtlasStart) out.push_back(n.id);
	return out;
}

bool AtlasTreeData::SaveBuild(const std::wstring& exeDir) const
{
	// read-modify-write: only the active project changes, siblings stay intact
	AtlasBuildFile f;
	f.Load(exeDir); // missing/invalid file falls back to one empty project
	f.version = version_;
	f.Active().alloc = AllocIds();
	return f.Save(exeDir);
}

bool AtlasTreeData::LoadBuild(const std::wstring& exeDir)
{
	AtlasBuildFile f;
	if (!f.Load(exeDir)) return false;
	ApplyAllocIds(f.Active().alloc);
	return true;
}

// ---- headless self-test (--atlas-selftest) -------------------------------------

namespace {

struct TestReport {
	std::string text;
	int failures = 0;

	void check(bool ok, const char* what, const std::string& detail = std::string())
	{
		text += ok ? "PASS  " : "FAIL  ";
		text += what;
		if (!detail.empty()) { text += "  ("; text += detail; text += ")"; }
		text += "\n";
		if (!ok) failures++;
	}
	void note(const std::string& s) { text += "      " + s + "\n"; }
};

// Independent reachability via explicit-stack DFS (the model uses deque BFS),
// optionally treating one node as removed.
static std::vector<char> dfs_reach(const AtlasTreeData& d, int skip)
{
	std::vector<char> reach(d.nodes.size(), 0);
	std::vector<int> stack;
	if (d.root != skip) { reach[d.root] = 1; stack.push_back(d.root); }
	while (!stack.empty()) {
		int cur = stack.back(); stack.pop_back();
		for (int nb : d.nodes[cur].adj) {
			if (reach[nb] || !d.nodes[nb].alloc || nb == skip) continue;
			reach[nb] = 1;
			stack.push_back(nb);
		}
	}
	return reach;
}

// Independent shortest-path distances from the start node by edge-list
// relaxation (Bellman-Ford style, no adjacency lists, no queue).
static std::vector<int> relax_dist(const AtlasTreeData& d)
{
	const int INF = 1 << 29;
	std::vector<int> dist(d.nodes.size(), INF);
	dist[d.root] = 0;
	bool changed = true;
	while (changed) {
		changed = false;
		for (const AtlasEdge& e : d.edges) {
			if (dist[e.a] + 1 < dist[e.b]) { dist[e.b] = dist[e.a] + 1; changed = true; }
			if (dist[e.b] + 1 < dist[e.a]) { dist[e.a] = dist[e.b] + 1; changed = true; }
		}
	}
	return dist;
}

} // namespace

int RunAtlasSelfTest(const std::wstring& exeDir)
{
	// WIN32-subsystem app: surface printf when started from a console.
	if (AttachConsole(ATTACH_PARENT_PROCESS)) {
		FILE* f = nullptr;
		freopen_s(&f, "CONOUT$", "w", stdout);
	}

	// T5's save/load round-trip writes through the production path; keep the
	// user's real build file safe and restore it before returning.
	std::string savedBuild;
	bool hadBuild = read_file_utf8(AtlasBuildFile::PathOf(exeDir), savedBuild);
	struct BuildGuard {
		const std::wstring& exeDir;
		const std::string& saved;
		bool had;
		~BuildGuard() {
			if (had) write_file_utf8(AtlasBuildFile::PathOf(exeDir), saved);
			else DeleteFileW(AtlasBuildFile::PathOf(exeDir).c_str());
		}
	} guard{ exeDir, savedBuild, hadBuild };

	TestReport rep;
	AtlasTreeData d;
	std::string err;

	// T1: load + shape sanity
	bool loaded = d.Load(exeDir, &err);
	rep.check(loaded, "load atlas_tree_poe1.json", loaded ? "" : err);
	if (!loaded) {
		printf("%s", rep.text.c_str());
		write_file_utf8(exeDir + L"atlas_selftest.txt", rep.text);
		return 1;
	}
	rep.note("nodes=" + std::to_string(d.nodes.size()) + " edges=" + std::to_string(d.edges.size()) +
	         " totalPoints=" + std::to_string(d.TotalPoints()));
	rep.check(d.root >= 0 && d.nodes[d.root].kind == kAtlasStart && d.nodes[d.root].alloc, "start node allocated");
	rep.check(d.nodes.size() >= 800 && d.edges.size() >= 900, "graph size plausible");
	rep.check(d.TotalPoints() >= 100 && d.TotalPoints() <= 200, "totalPoints plausible (138 for 3.26)");
	rep.check(d.UsedPoints() == 0, "fresh tree uses 0 points");
	rep.check(d.FindPathTo(d.root).empty() && d.FindRemoveSet(d.root).empty(), "start node is inert");

	// T2: farthest node — model path length must equal an independently
	// computed shortest distance.
	std::vector<int> dist = relax_dist(d);
	int farNode = -1;
	for (int i = 0; i < (int)d.nodes.size(); i++)
		if (dist[i] < (1 << 29) && (farNode == -1 || dist[i] > dist[farNode] ||
		    (dist[i] == dist[farNode] && d.nodes[i].id < d.nodes[farNode].id)))
			farNode = i;
	std::vector<int> path = d.FindPathTo(farNode);
	rep.check((int)path.size() == dist[farNode], "shortest path to farthest node",
	          "bfs=" + std::to_string(path.size()) + " relax=" + std::to_string(dist[farNode]));
	bool chainOk = !path.empty() && path.back() == farNode;
	for (size_t i = 0; chainOk && i < path.size(); i++) {
		const std::vector<int>& adj = d.nodes[path[i]].adj;
		if (i == 0) {
			bool touchesAlloc = false;
			for (int nb : adj) touchesAlloc = touchesAlloc || d.nodes[nb].alloc;
			chainOk = touchesAlloc;
		} else {
			chainOk = std::find(adj.begin(), adj.end(), path[i - 1]) != adj.end();
		}
		chainOk = chainOk && !d.nodes[path[i]].alloc;
	}
	rep.check(chainOk, "path is a valid unallocated chain from the allocated set");
	d.Alloc(path);
	rep.check(d.UsedPoints() == (int)path.size(), "used points == path length");

	// Build a broad web: paths to every 10th notable/keystone (deterministic).
	std::vector<int> targets;
	for (int i = 0; i < (int)d.nodes.size(); i++)
		if (d.nodes[i].kind == kAtlasNotable || d.nodes[i].kind == kAtlasKeystone)
			targets.push_back(i);
	std::sort(targets.begin(), targets.end(),
	          [&](int a, int b) { return d.nodes[a].id < d.nodes[b].id; });
	int added = 0;
	for (size_t i = 0; i < targets.size(); i += 10) {
		std::vector<int> p = d.FindPathTo(targets[i]);
		if (!p.empty()) { d.Alloc(p); added++; }
	}
	rep.note("web: " + std::to_string(added) + " extra targets, used=" + std::to_string(d.UsedPoints()));

	// Shortest-path webs are nearly tree-shaped, so close a loop on purpose:
	// allocate one node that touches two already-allocated neighbors. Some tree
	// topologies (e.g. 3.29) leave the sparse every-10th web acyclic, so widen it
	// to every notable/keystone until such a node exists — the full graph has far
	// more edges than a spanning tree, so a loop-closing node is guaranteed.
	auto findRing = [&]() -> int {
		for (int i = 0; i < (int)d.nodes.size(); i++) {
			if (d.nodes[i].alloc || d.nodes[i].kind == kAtlasStart) continue;
			int allocNbs = 0;
			for (int nb : d.nodes[i].adj) allocNbs += d.nodes[nb].alloc ? 1 : 0;
			if (allocNbs >= 2) return i;
		}
		return -1;
	};
	int ringNode = findRing();
	if (ringNode == -1) {
		for (size_t i = 0; i < targets.size() && ringNode == -1; i++) {
			std::vector<int> p = d.FindPathTo(targets[i]);
			if (!p.empty()) d.Alloc(p);
			ringNode = findRing();
		}
		rep.note("widened web to guarantee a loop, used=" + std::to_string(d.UsedPoints()));
	}
	rep.check(ringNode != -1, "found a loop-closing node");
	if (ringNode != -1) d.Alloc({ ringNode });

	// T3: removal spec check across a deterministic sample of allocated nodes.
	// Spec: FindRemoveSet(M) == { allocated n != start : n == M or n unreachable
	// from start in (allocated - M) }, verified with an independent DFS.
	int ringCases = 0, bridgeCases = 0, violations = 0, checked = 0;
	auto specCheck = [&](int m) {
		if (m < 0 || !d.nodes[m].alloc || d.nodes[m].kind == kAtlasStart) return;
		checked++;
		std::vector<int> rs = d.FindRemoveSet(m);
		std::vector<char> reach = dfs_reach(d, m);
		std::set<int> expected;
		for (int i = 0; i < (int)d.nodes.size(); i++)
			if (d.nodes[i].alloc && d.nodes[i].kind != kAtlasStart && !reach[i])
				expected.insert(i);
		if (std::set<int>(rs.begin(), rs.end()) != expected) violations++;
		int allocNbs = 0;
		for (int nb : d.nodes[m].adj) allocNbs += d.nodes[nb].alloc ? 1 : 0;
		if (rs.size() == 1 && allocNbs >= 2) ringCases++;
		if (rs.size() > 1) bridgeCases++;
	};
	for (int m = 0; m < (int)d.nodes.size(); m += 7) specCheck(m);
	specCheck(ringNode); // always include the loop closer
	rep.check(violations == 0, "removal matches spec on every sampled node",
	          "checked=" + std::to_string(checked));
	rep.check(ringCases >= 1, "ring case seen (remove one node of a loop keeps the rest)",
	          std::to_string(ringCases));
	rep.check(bridgeCases >= 1, "bridge case seen (removing a link drops the subtree)",
	          std::to_string(bridgeCases));

	// T4: actually apply one bridge removal and re-check global connectivity.
	int bridge = -1;
	for (int m = 0; m < (int)d.nodes.size() && bridge == -1; m++)
		if (d.nodes[m].alloc && d.nodes[m].kind != kAtlasStart && d.FindRemoveSet(m).size() > 1)
			bridge = m;
	if (bridge != -1) {
		int before = d.UsedPoints();
		std::vector<int> rs = d.FindRemoveSet(bridge);
		d.Remove(rs);
		std::vector<char> reach = dfs_reach(d, -1);
		bool connected = true;
		for (int i = 0; i < (int)d.nodes.size(); i++)
			if (d.nodes[i].alloc && !reach[i]) connected = false;
		rep.check(connected, "allocated set stays connected after removal");
		rep.check(d.UsedPoints() == before - (int)rs.size(), "used points drop by removed count");
	} else {
		rep.check(false, "found a bridge node to remove");
	}

	// T5: save / reset / load round-trip preserves the allocation.
	std::set<int> beforeIds;
	for (const AtlasNode& n : d.nodes)
		if (n.alloc && n.kind != kAtlasStart) beforeIds.insert(n.id);
	rep.check(d.SaveBuild(exeDir), "save build");
	d.Reset();
	rep.check(d.UsedPoints() == 0, "reset clears allocation");
	rep.check(d.LoadBuild(exeDir), "load build");
	std::set<int> afterIds;
	for (const AtlasNode& n : d.nodes)
		if (n.alloc && n.kind != kAtlasStart) afterIds.insert(n.id);
	rep.check(beforeIds == afterIds, "round-trip preserves allocation",
	          std::to_string(beforeIds.size()) + " nodes");

	// T7: multi-build persistence — legacy migration + share-code round-trip.
	{
		AtlasBuildFile f;
		bool mig = f.ParseDoc(u8"{\"version\":\"x\",\"alloc\":[1,2,3]}");
		rep.check(mig && f.builds.size() == 1 && f.builds[0].alloc == std::vector<int>({ 1, 2, 3 }) &&
		          f.active == 0 && !f.builds[0].name.empty(),
		          "legacy single-build file migrates to one project");

		AtlasBuildFile f2;
		rep.check(f2.ParseDoc(f.SerializeDoc()) && f2.builds.size() == 1 &&
		          f2.builds[0].alloc == f.builds[0].alloc,
		          "multi-build schema round-trips");

		AtlasBuildEntry src{ u8"測試專案", { 45343, 65225, 27659 } };
		AtlasBuildEntry back;
		std::string code = AtlasBuildShareCode(src, "test");
		std::string perr;
		bool codeOk = code.compare(0, 6, "PTAT1|") == 0 &&
		              AtlasParseShareCode("  " + code + "\r\n", &back, &perr) &&
		              back.alloc == src.alloc && back.name == src.name;
		rep.check(codeOk, "share code round-trips (with clipboard whitespace)", perr);
		AtlasBuildEntry junk;
		rep.check(!AtlasParseShareCode("PTAT1|!!!notbase64!!!", &junk, &perr), "corrupt share code rejected");
		rep.check(!AtlasParseShareCode("hello world", &junk, &perr), "non-code text rejected");
	}

	// T6: zh mapping health (only when Data/atlas_tree_zh.json is present).
	{
		AtlasI18n zh;
		if (zh.Load(exeDir)) {
			static const std::string kNone;
			int covered = 0, translatable = 0;
			for (const AtlasNode& n : d.nodes) {
				if (n.kind == kAtlasStart) continue;
				translatable++;
				if (!zh.NodeName(n.id, kNone).empty()) covered++;
			}
			rep.check(translatable > 0 && covered * 2 >= translatable,
			          "zh mapping covers >=50% of tree nodes",
			          std::to_string(covered) + "/" + std::to_string(translatable) +
			          " (" + zh.VersionNote() + ")");
		} else {
			rep.note("zh mapping absent or empty - skipped (English-only display)");
		}
	}

	rep.text += rep.failures == 0 ? "\nALL PASS\n" : "\nFAILURES: " + std::to_string(rep.failures) + "\n";
	printf("%s", rep.text.c_str());
	write_file_utf8(exeDir + L"atlas_selftest.txt", rep.text);
	return rep.failures == 0 ? 0 : 1;
}
