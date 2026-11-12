#include "atlas_tree_data.h"
#include "atlas_i18n.h"
#include "atlas_persist.h"
#include "atlas_scarabs.h"
#include "atlas_threads.h"
#include "atlas_astrolabes.h"
#include "atlas_maps.h"
#include "atlas_optimize.h"
#include "atlas_version_index.h"
#include "atlas_import.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <map>
#include <set>

using nlohmann::ordered_json;

// file helpers (same conventions as economy_service.cpp)

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

// json parsing

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
			if (jn.contains("variants"))
				for (const auto& jv : jn["variants"]) {
					AtlasVariant v;
					v.effect = jv.value("e", 0);
					if (jv.contains("stats"))
						for (const auto& st : jv["stats"]) v.stats.push_back(st.get<std::string>());
					n.variants.push_back(std::move(v));
				}
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

// allocation logic

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
	return removeSet;
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
			nodes[i].alloc = false, nodes[i].variant = -1;
}

void AtlasTreeData::Reset()
{
	for (AtlasNode& n : nodes) { n.alloc = (n.kind == kAtlasStart); n.target = false; n.blocked = false; n.variant = -1; }
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

// build persistence

int AtlasTreeData::ApplyAllocIds(const std::vector<int>& ids)
{
	// GGG skill ids are the stable key; ids gone after a data regeneration are silently dropped,
	// then the connectivity repair prunes anything that came loose.
	std::vector<std::pair<int, int>> id2idx;
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

int AtlasTreeData::ApplyTargetIds(const std::vector<int>& ids)
{
	std::vector<std::pair<int, int>> id2idx;
	id2idx.reserve(nodes.size());
	for (int i = 0; i < (int)nodes.size(); i++) id2idx.push_back({ nodes[i].id, i });
	std::sort(id2idx.begin(), id2idx.end());

	for (AtlasNode& n : nodes) n.target = false;
	int mapped = 0;
	for (int id : ids) {
		auto it = std::lower_bound(id2idx.begin(), id2idx.end(), std::make_pair(id, 0));
		if (it != id2idx.end() && it->first == id && nodes[it->second].kind != kAtlasStart) {
			nodes[it->second].target = true;
			mapped++;
		}
	}
	return mapped;
}

std::vector<int> AtlasTreeData::TargetIds() const
{
	std::vector<int> out;
	for (const AtlasNode& n : nodes)
		if (n.target && n.kind != kAtlasStart) out.push_back(n.id);
	return out;
}

std::vector<int> AtlasTreeData::TargetIdx() const
{
	std::vector<int> out;
	for (int i = 0; i < (int)nodes.size(); i++)
		if (nodes[i].target && nodes[i].kind != kAtlasStart) out.push_back(i);
	return out;
}

int AtlasTreeData::ApplyBlockedIds(const std::vector<int>& ids)
{
	std::vector<std::pair<int, int>> id2idx;
	id2idx.reserve(nodes.size());
	for (int i = 0; i < (int)nodes.size(); i++) id2idx.push_back({ nodes[i].id, i });
	std::sort(id2idx.begin(), id2idx.end());

	for (AtlasNode& n : nodes) n.blocked = false;
	int mapped = 0;
	for (int id : ids) {
		auto it = std::lower_bound(id2idx.begin(), id2idx.end(), std::make_pair(id, 0));
		// The start node is never blockable (every target would become unreachable).
		if (it != id2idx.end() && it->first == id && nodes[it->second].kind != kAtlasStart) {
			nodes[it->second].blocked = true;
			mapped++;
		}
	}
	return mapped;
}

std::vector<int> AtlasTreeData::BlockedIds() const
{
	std::vector<int> out;
	for (const AtlasNode& n : nodes)
		if (n.blocked && n.kind != kAtlasStart) out.push_back(n.id);
	return out;
}

std::vector<int> AtlasTreeData::BlockedIdx() const
{
	std::vector<int> out;
	for (int i = 0; i < (int)nodes.size(); i++)
		if (nodes[i].blocked && nodes[i].kind != kAtlasStart) out.push_back(i);
	return out;
}

std::vector<int> AtlasTreeData::AllocIdx() const
{
	std::vector<int> out;
	for (int i = 0; i < (int)nodes.size(); i++)
		if (nodes[i].alloc) out.push_back(i);
	return out;
}

void AtlasTreeData::SetAllocSet(const std::vector<int>& idx)
{
	for (AtlasNode& n : nodes) n.alloc = (n.kind == kAtlasStart);
	if (root >= 0) nodes[root].alloc = true;
	for (int i : idx)
		if (i >= 0 && i < (int)nodes.size()) nodes[i].alloc = true;
}

std::vector<std::string> AtlasNode::EffStats() const
{
	std::vector<std::string> out = stats;
	if (variant >= 0 && variant < (int)variants.size())
		out.insert(out.end(), variants[variant].stats.begin(), variants[variant].stats.end());
	return out;
}

std::vector<std::string> AtlasNode::AggStats() const
{
	if (variants.empty()) return stats;
	// Unpicked: the node grants nothing yet, and its base line alone is half a sentence.
	if (variant < 0 || variant >= (int)variants.size()) return {};
	if (stats.empty()) return variants[variant].stats;
	std::string pick;
	for (const std::string& s : variants[variant].stats) pick += (pick.empty() ? "" : " / ") + s;
	std::vector<std::string> out;
	for (const std::string& s : stats) {
		std::string line;
		size_t at = 0;
		while (true) {
			size_t nl = s.find('\n', at);
			line += s.substr(at, nl == std::string::npos ? nl : nl - at) + ": " + pick;
			if (nl == std::string::npos) break;
			line += '\n';
			at = nl + 1;
		}
		out.push_back(std::move(line));
	}
	return out;
}

std::vector<std::string> AtlasNode::AllStats() const
{
	std::vector<std::string> out = stats;
	for (const AtlasVariant& v : variants) out.insert(out.end(), v.stats.begin(), v.stats.end());
	return out;
}

int AtlasTreeData::ApplyVariantIds(const std::map<int, int>& nodeToEffect)
{
	int mapped = 0;
	for (AtlasNode& n : nodes) {
		n.variant = -1;
		if (!n.alloc) continue;
		auto it = nodeToEffect.find(n.id);
		if (it == nodeToEffect.end()) continue;
		for (int i = 0; i < (int)n.variants.size(); i++)
			if (n.variants[i].effect == it->second) { n.variant = i; mapped++; break; }
	}
	return mapped;
}

std::map<int, int> AtlasTreeData::VariantIds() const
{
	std::map<int, int> out;
	for (const AtlasNode& n : nodes)
		if (n.alloc && n.variant >= 0 && n.variant < (int)n.variants.size())
			out[n.id] = n.variants[n.variant].effect;
	return out;
}

bool AtlasTreeData::SaveBuild(const std::wstring& exeDir) const
{
	// read-modify-write: only the active project changes, siblings stay intact
	AtlasBuildFile f;
	f.Load(exeDir);
	f.version = version_;
	f.Active().alloc = AllocIds();
	f.Active().targets = TargetIds();
	f.Active().blocked = BlockedIds();
	f.Active().variants = VariantIds();
	return f.Save(exeDir);
}

bool AtlasTreeData::LoadBuild(const std::wstring& exeDir)
{
	AtlasBuildFile f;
	if (!f.Load(exeDir)) return false;
	ApplyAllocIds(f.Active().alloc);     // clears targets, so it goes first
	ApplyTargetIds(f.Active().targets);
	ApplyBlockedIds(f.Active().blocked);
	ApplyVariantIds(f.Active().variants);
	return true;
}

// headless self-test (--atlas-selftest)

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
	if (AttachConsole(ATTACH_PARENT_PROCESS)) {
		FILE* f = nullptr;
		freopen_s(&f, "CONOUT$", "w", stdout);
	}

	// T5's save/load round-trip writes through the production path; the user's real build file is restored before returning.
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

	// Shortest-path webs are nearly tree-shaped, so close a loop on purpose: allocate a node
	// touching two allocated neighbours, widening the web to every notable/keystone until one exists.
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

	// T5b: the same for targets (a lost target would silently un-pin a user's node).
	{
		std::vector<int> someIdx;
		for (int i = 0; i < (int)d.nodes.size() && someIdx.size() < 4; i++)
			if (d.nodes[i].alloc && d.nodes[i].kind != kAtlasStart) someIdx.push_back(i);
		std::vector<int> wantIds;
		for (int i : someIdx) { d.nodes[i].target = true; wantIds.push_back(d.nodes[i].id); }
		std::sort(wantIds.begin(), wantIds.end());
		rep.check(d.SaveBuild(exeDir), "save build with targets");
		d.Reset();
		rep.check(d.TargetIdx().empty(), "reset clears targets too");
		rep.check(d.LoadBuild(exeDir), "load build with targets");
		std::vector<int> gotIds = d.TargetIds();
		std::sort(gotIds.begin(), gotIds.end());
		rep.check(gotIds == wantIds, "round-trip preserves targets",
		          std::to_string(gotIds.size()) + " of " + std::to_string(wantIds.size()));
		// ApplyAllocIds resets targets, so the load order in LoadBuild matters.
		d.ApplyAllocIds(d.AllocIds());
		rep.check(d.TargetIdx().empty(), "ApplyAllocIds clears targets (load order contract)");
		d.ApplyTargetIds(wantIds);
		rep.check((int)d.TargetIdx().size() == (int)wantIds.size(), "ApplyTargetIds restores them");
		int unknown = d.ApplyTargetIds({ -12345 });
		rep.check(unknown == 0 && d.TargetIdx().empty(), "unknown target ids are dropped");
		d.ApplyTargetIds(wantIds);
	}

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
			const std::string cov = std::to_string(covered) + "/" + std::to_string(translatable) +
			                        " (" + zh.VersionNote() + ")";
			// 譯文早於本季(repoe 版本 < 賽季)時覆蓋率低是預期狀態:不確定的譯文一律顯示英文,開季後補上才恢復門檻。
			if (zh.LagsSeason(d.Tag()))
				rep.note(u8"提示:譯文早於本季,繁中覆蓋率 " + cov + u8",新內容先顯示英文(門檻檢查暫不適用)");
			else
				rep.check(translatable > 0 && covered * 2 >= translatable,
				          "zh mapping covers >=50% of tree nodes", cov);
		} else {
			rep.note("zh mapping absent or empty - skipped (English-only display)");
		}
	}

	// T7b: the REAL project file on this machine: parse + write back must reproduce it byte for
	// byte (else merely opening the planner would rewrite a user's projects). Uses the copy
	// BuildGuard took before any test wrote to that path.
	if (hadBuild && !savedBuild.empty()) {
		AtlasBuildFile probe;
		if (probe.ParseDoc(savedBuild)) {
			std::string again = probe.SerializeDoc();
			rep.check(again == savedBuild,
			          "the existing project file on disk survives parse->serialize unchanged",
			          again == savedBuild ? std::string()
			                              : "was " + std::to_string(savedBuild.size()) +
			                                " bytes, now " + std::to_string(again.size()));
			if (again != savedBuild) {
				rep.note("before: " + savedBuild.substr(0, 300));
				rep.note("after : " + again.substr(0, 300));
			}
			// T7c: does USING the new fields destroy saved work? Adds an astrolabe, a main map and a
			// planned tier to every project, reads back and compares pre-existing fields (in memory only).
			AtlasBuildFile mutated;
			mutated.ParseDoc(savedBuild);
			for (AtlasBuildEntry& b : mutated.builds) {
				b.astrolabes.push_back({ "NorthWest", "Metadata/Items/Currency/AstrolabeBreach" });
				b.mapId = "MapWorldsShrine";
				b.mapTier = 16;
			}
			AtlasBuildFile back;
			bool ok = back.ParseDoc(mutated.SerializeDoc()) &&
			          back.builds.size() == probe.builds.size() &&
			          back.version == probe.version && back.active == probe.active;
			std::string lost;
			for (size_t i = 0; ok && i < probe.builds.size(); i++) {
				const AtlasBuildEntry& a = probe.builds[i];
				const AtlasBuildEntry& c = back.builds[i];
				if (a.name != c.name)         lost += " name";
				if (a.alloc != c.alloc)       lost += " alloc";
				if (a.targets != c.targets)   lost += " targets";
				if (a.blocked != c.blocked)   lost += " blocked";
				if (a.notes != c.notes)       lost += " notes";
				if (a.scarabs != c.scarabs)   lost += " scarabs";
			}
			size_t nodes = 0, tgts = 0;
			for (const AtlasBuildEntry& b : probe.builds) { nodes += b.alloc.size(); tgts += b.targets.size(); }
			rep.check(ok && lost.empty(),
			          "adding astrolabes/map/tier to the real project keeps every existing field",
			          ok ? (lost.empty()
			                    ? std::to_string(probe.builds.size()) + " project(s), " +
			                          std::to_string(nodes) + " nodes, " + std::to_string(tgts) + " targets intact"
			                    : "lost:" + lost)
			             : "document did not survive the round trip");
		} else {
			rep.note("existing project file did not parse - skipped the byte-identity check");
		}
	} else {
		rep.note("no existing project file on disk - byte-identity check skipped");
	}

	// T11: the click model itself — allocating towards a new node must never move an already
	// allocated node ("點一下就接最短路徑、不動已配好的點"). Minimality is only reachable through the toolbar button.
	{
		d.Reset();
		std::vector<int> picks;
		for (int i = 0; i < (int)d.nodes.size(); i++)
			if (d.nodes[i].kind == kAtlasNotable) picks.push_back(i);
		std::sort(picks.begin(), picks.end(),
		          [&](int a, int b) { return d.nodes[a].id < d.nodes[b].id; });
		int steps = 0, moved = 0, badLen = 0, unpinned = 0;
		std::set<int> prev;
		std::vector<int> distNow = relax_dist(d);
		for (size_t i = 0; i < picks.size() && steps < 12; i += 7) {
			int t = picks[i];
			if (d.nodes[t].alloc) continue;
			std::vector<int> p = d.FindPathTo(t);
			if (p.empty()) continue;
			// the path only ever consists of currently-unallocated nodes
			for (int v : p) if (d.nodes[v].alloc) badLen++;
			d.Alloc(p);
			d.nodes[t].target = true;
			std::set<int> now;
			for (const AtlasNode& n : d.nodes)
				if (n.alloc && n.kind != kAtlasStart) now.insert(n.id);
			for (int id : prev) if (!now.count(id)) moved++;
			prev = now;
			steps++;
		}
		rep.check(steps >= 5, "click-model check ran enough steps",
		          std::to_string(steps) + " clicks");
		rep.check(moved == 0, "allocating towards a new node never drops an allocated one",
		          std::to_string(moved) + " lost");
		// Replay the same clicks through the minimum-point solver and count what it would have
		// moved: a note, not a check (it describes the rejected model).
		{
			AtlasTreeData m;
			std::string e3;
			int solverDrops = 0, solverSteps = 0;
			if (m.Load(exeDir, &e3)) {
				std::set<int> mprev;
				std::vector<int> mtargets;
				for (size_t i = 0; i < picks.size() && solverSteps < steps; i += 7) {
					int t = picks[i];
					if (t < 0 || t >= (int)m.nodes.size()) continue;
					mtargets.push_back(t);
					AtlasPlan p = AtlasPlanMinimal(m, mtargets, {}, m.AllocIdx());
					m.SetAllocSet(p.nodes);
					std::set<int> mnow;
					for (const AtlasNode& n : m.nodes)
						if (n.alloc && n.kind != kAtlasStart) mnow.insert(n.id);
					for (int id : mprev) if (!mnow.count(id)) solverDrops++;
					mprev = mnow;
					solverSteps++;
				}
			}
			rep.note("same clicks under the 0.12.0 minimum-point model would have moved " +
			         std::to_string(solverDrops) + " already-allocated node(s)");
		}
		rep.check(badLen == 0, "each new path is entirely unallocated nodes");
		// The first click's route must still be a genuine shortest path from the
		// start; "never moves" would be trivially satisfied by never optimising.
		int firstNotable = -1;
		for (int i : picks) { firstNotable = i; break; }
		if (firstNotable >= 0 && distNow[firstNotable] < (1 << 29)) {
			AtlasTreeData fresh;
			std::string e2;
			if (fresh.Load(exeDir, &e2)) {
				std::vector<int> p = fresh.FindPathTo(firstNotable);
				rep.check((int)p.size() == distNow[firstNotable],
				          "a first click still takes the shortest route",
				          "bfs=" + std::to_string(p.size()) +
				          " relax=" + std::to_string(distNow[firstNotable]));
			}
		}
		// Removal still takes the orphans with it, and targets must not survive on
		// nodes that are gone (they feed the compress button).
		if (!prev.empty()) {
			int victim = -1;
			for (int i = 0; i < (int)d.nodes.size(); i++)
				if (d.nodes[i].alloc && d.nodes[i].target) { victim = i; break; }
			if (victim >= 0) {
				std::vector<int> rs = d.FindRemoveSet(victim);
				for (int v : rs) d.nodes[v].target = false;
				d.Remove(rs);
				for (const AtlasNode& n : d.nodes)
					if (!n.alloc && n.target) unpinned++;
				rep.check(unpinned == 0, "removed nodes do not stay pinned as targets");
				rep.check(!d.nodes[victim].alloc, "the removed node is actually gone");
			}
		}
		d.Reset();
	}

	// T8: scarab catalogue, placement rules and the notes/scarabs persistence
	// (including the backward-compatibility guarantees) — see atlas_scarabs.cpp.
	rep.failures += RunScarabSelfTest(exeDir, rep.text);

	// T9/T10: the 3.29 astrolabe quadrant rule and the main-map catalogue, each
	// with the same backward-compatibility guarantees the scarabs established.
	rep.failures += RunAstrolabeSelfTest(exeDir, rep.text);
	rep.failures += RunAtlasMapSelfTest(exeDir, rep.text);
	rep.failures += RunAtlasSearchSelfTest(exeDir, rep.text);

	// 3.30 thread slots: placement rules and persistence (atlas_threads.cpp).
	rep.failures += RunThreadSelfTest(exeDir, rep.text);

	// T11: the manual map price behind the 收益 tab's cost card: round-trips through the build file,
	// an unset price serializes byte-identically to before, never leaks into the export json.
	{
		AtlasBuildFile f;
		f.version = "3.29.0";
		f.builds.resize(1);
		f.builds[0].name = "t11";
		const std::string before = f.SerializeDoc();
		f.builds[0].mapPrice = 12.5;
		const std::string with = f.SerializeDoc();
		AtlasBuildFile g;
		const bool parsed = g.ParseDoc(with);
		rep.check(parsed && g.builds.size() == 1 && g.builds[0].mapPrice == 12.5,
		          "mapPrice round-trips through the build file");
		f.builds[0].mapPrice = 0.0;
		rep.check(f.SerializeDoc() == before && before.find("mapPrice") == std::string::npos,
		          "unset mapPrice serializes byte-identically to before");
		f.builds[0].mapPrice = 12.5;
		rep.check(AtlasExportJson(f.builds[0], "3.29.0").find("mapPrice") == std::string::npos,
		          "mapPrice stays out of the export json / share code");

		// The recorded cost (bulk buy: one market snapshot; batch buys: typed).
		f.builds[0].costPrices["Metadata/Items/Scarabs/T11"] = 7.5;
		f.builds[0].costRecordedUtc = 1800000000;
		AtlasBuildFile h;
		const bool parsed2 = h.ParseDoc(f.SerializeDoc());
		rep.check(parsed2 && h.builds.size() == 1 && h.builds[0].costPrices.size() == 1 &&
		              h.builds[0].costPrices.begin()->second == 7.5 &&
		              h.builds[0].costRecordedUtc == 1800000000,
		          "recorded cost prices round-trip through the build file");
		rep.check(AtlasExportJson(f.builds[0], "3.29.0").find("\"cost\"") == std::string::npos,
		          "recorded cost prices stay out of the export json / share code");
		f.builds[0].costPrices.clear();
		f.builds[0].costRecordedUtc = 0;
		f.builds[0].mapPrice = 0.0;
		rep.check(f.SerializeDoc() == before, "unset cost record serializes byte-identically");

		// Planned map count: the cost card's plan total. Build file only.
		f.builds[0].plannedMaps = 40;
		AtlasBuildFile pm;
		rep.check(pm.ParseDoc(f.SerializeDoc()) && pm.builds.size() == 1 &&
		              pm.builds[0].plannedMaps == 40,
		          "plannedMaps round-trips through the build file");
		rep.check(AtlasExportJson(f.builds[0], "3.29.0").find("plannedMaps") == std::string::npos,
		          "plannedMaps stays out of the export json / share code");
		f.builds[0].plannedMaps = 0;

		// The bound revenue record is shared ON PURPOSE: build file, export json
		// and share code all carry it.
		AtlasProfitRecord pr;
		pr.fromUtc = 1800000000;
		pr.toUtc = 1800007200;
		pr.league = "Mercenaries";
		pr.hours = 2.0;
		pr.qtyGain = 900.0;
		pr.qtyLoss = -100.0;
		pr.priceMove = -15.5;
		pr.net = 784.5;
		pr.divineRate = 200.0;
		pr.costPerMap = 36.46;
		pr.top = { { "The Doctor", u8"博士", 1, 500.0 }, { "Divine Orb", "", 2, 400.0 } };
		f.builds[0].profit = pr;
		auto same = [&pr](const AtlasProfitRecord& q) {
			return q.fromUtc == pr.fromUtc && q.toUtc == pr.toUtc && q.league == pr.league &&
			       q.hours == pr.hours && q.qtyGain == pr.qtyGain && q.qtyLoss == pr.qtyLoss &&
			       q.priceMove == pr.priceMove && q.net == pr.net && q.divineRate == pr.divineRate &&
			       q.costPerMap == pr.costPerMap && q.top.size() == 2 && q.top[0].en == "The Doctor" &&
			       q.top[0].zh == u8"博士" && q.top[0].dCount == 1 && q.top[0].chaos == 500.0 &&
			       q.top[1].zh.empty() && q.top[1].dCount == 2 && q.top[1].chaos == 400.0;
		};
		AtlasBuildFile pf;
		rep.check(pf.ParseDoc(f.SerializeDoc()) && pf.builds.size() == 1 && same(pf.builds[0].profit),
		          "profit record round-trips through the build file");
		AtlasBuildEntry viaJson, viaCode;
		std::string perr;
		rep.check(AtlasParseExportJson(AtlasExportJson(f.builds[0], "3.29.0"), &viaJson, &perr) &&
		              same(viaJson.profit),
		          "profit record travels in the export json");
		rep.check(AtlasParseShareCode(AtlasBuildShareCode(f.builds[0], "3.29.0"), &viaCode, &perr) &&
		              same(viaCode.profit),
		          "profit record travels in the share code");
		AtlasBuildEntry badRec;
		const char kBadProfit[] =
		    R"({"format":"pobtools-atlas-build","version":"3.29.0","name":"x","alloc":[1,2],)"
		    R"("profit":{"from":"x","to":5,"top":[{"en":3}]}})";
		rep.check(AtlasParseExportJson(kBadProfit, &badRec, &perr) && badRec.alloc.size() == 2 &&
		              badRec.profit.empty(),
		          "a malformed profit record is dropped, the project kept");
		f.builds[0].profit = AtlasProfitRecord{};
		rep.check(f.SerializeDoc() == before, "unset plannedMaps / profit serialize byte-identically");
	}

	// T12: 3.30 多選節點 + 跨群弧線 + 所選選項的存檔(規格 T1~T7)。
	{
		d.Reset();
		int tagMaj = 0, tagMin = 0;
		sscanf_s(d.Tag().c_str(), "%d.%d", &tagMaj, &tagMin);
		const bool ge330 = tagMaj > 3 || (tagMaj == 3 && tagMin >= 30);
		const bool is330 = d.Tag() == "3.30.0";
		auto idxOfId = [&](int id) {
			for (int i = 0; i < (int)d.nodes.size(); i++)
				if (d.nodes[i].id == id) return i;
			return -1;
		};
		std::vector<int> multi; // 多選節點的索引
		for (int i = 0; i < (int)d.nodes.size(); i++)
			if (!d.nodes[i].variants.empty()) multi.push_back(i);
		const int mi = multi.empty() ? -1 : multi[0];
		const bool mcOk = mi >= 0 && d.nodes[mi].variants.size() >= 2;
		const std::string tagInfo = "tag=" + d.Tag() + " multi=" + std::to_string(multi.size());
		// 無多選節點時:3.30+ 讓測試失敗,更舊賽季只留註記。
		auto needMulti = [&](const char* title) {
			if (mcOk) return true;
			if (ge330) rep.check(false, title, "樹上沒有可用的多選節點; " + tagInfo);
			else rep.note(std::string("略過(舊賽季無多選節點): ") + title);
			return false;
		};

		// T1: 匯入後多選節點帶齊選項。
		if (ge330) {
			int badNode = 0, dupEff = 0;
			std::set<int> effs;
			for (int i : multi) {
				bool bad = d.nodes[i].variants.size() < 2;
				for (const AtlasVariant& v : d.nodes[i].variants) {
					if (v.stats.empty()) bad = true;
					if (!effs.insert(v.effect).second) dupEff++;
				}
				if (bad) badNode++;
			}
			rep.check(!multi.empty(), u8"[多選T1] 3.30 樹至少有一個多選節點", tagInfo);
			rep.check(!multi.empty() && badNode == 0, u8"[多選T1] 每個多選節點都有 2 個以上選項、每項至少 1 行",
			          "bad=" + std::to_string(badNode));
			rep.check(!multi.empty() && dupEff == 0, u8"[多選T1] 全樹 effect id 不重複", "dup=" + std::to_string(dupEff));
		} else {
			rep.note("略過多選 T1(a): 賽季 " + d.Tag() + " 早於 3.30");
		}
		if (is330) {
			int found = 0, good = 0;
			for (const AtlasNode& n : d.nodes) {
				if (n.name != "Suffused with Darkness") continue;
				found++;
				std::set<int> eff;
				bool lines = n.variants.size() == 2;
				for (const AtlasVariant& v : n.variants) { eff.insert(v.effect); lines = lines && !v.stats.empty(); }
				if (lines && eff == std::set<int>({ 2215, 38495 })) good++;
			}
			rep.check(found >= 1 && good == found, u8"[多選T1] Suffused with Darkness 恰 2 個選項 2215 與 38495",
			          "found=" + std::to_string(found) + " good=" + std::to_string(good));
		}

		// T2: 多選旗標但缺選項時匯入中止。
		{
			auto run = [&](const char* json, std::string& out, std::string& e) {
				out.clear(); e.clear();
				return AtlasImportVariants(json, &out, &e);
			};
			std::string vout, verr;
			bool ok = run(R"({"name":"N","stats":["s"]})", vout, verr);
			rep.check(ok && vout.empty(), u8"[多選T2] 一般節點匯入成功且無選項輸出");
			ok = run(R"({"name":"N","isMultipleChoiceVariant":true,"variantEffects":[{"effect":2215,"stats":["A"]},{"effect":38495,"stats":["B"]}]})",
			         vout, verr);
			ordered_json arr = ordered_json::parse(vout, nullptr, false);
			bool shape = ok && arr.is_array() && arr.size() == 2;
			if (shape) {
				shape = arr[0].value("e", -1) == 2215 && arr[1].value("e", -1) == 38495 &&
				        arr[0].contains("stats") && arr[0]["stats"] == ordered_json::array({ "A" }) &&
				        arr[1].contains("stats") && arr[1]["stats"] == ordered_json::array({ "B" });
			}
			rep.check(shape, u8"[多選T2] 合法多選節點輸出 e/stats 陣列", vout);
			ok = run(R"({"name":"N","isMultipleChoiceVariant":true})", vout, verr);
			rep.check(!ok && !verr.empty(), u8"[多選T2] 有旗標但沒有 variantEffects 時失敗並帶錯誤");
			ok = run(R"({"name":"N","isMultipleChoiceVariant":true,"variantEffects":[]})", vout, verr);
			rep.check(!ok && !verr.empty(), u8"[多選T2] variantEffects 為空陣列時失敗並帶錯誤");
			ok = run(R"({"name":"N","isMultipleChoiceVariant":true,"variantEffects":[{"stats":["A"]},{"effect":38495,"stats":["B"]}]})",
			         vout, verr);
			rep.check(!ok && !verr.empty(), u8"[多選T2] 某選項缺整數 effect 時失敗並帶錯誤");
			ok = run(R"({"name":"N","isMultipleChoiceVariant":true,"variantEffects":[{"effect":2215},{"effect":38495,"stats":["B"]}]})",
			         vout, verr);
			rep.check(!ok && !verr.empty(), u8"[多選T2] 某選項缺 stats 陣列時失敗並帶錯誤");
		}

		// T3: 跨群連線產生弧線。
		if (is330) {
			const int ia = idxOfId(36820), ib = idxOfId(34925);
			const AtlasEdge* arc = nullptr;
			for (const AtlasEdge& e : d.edges)
				if ((e.a == ia && e.b == ib) || (e.a == ib && e.b == ia)) arc = &e;
			rep.check(ia >= 0 && ib >= 0 && arc != nullptr && arc->hasArc, u8"[多選T3] 36820 與 34925 之間的邊是弧線");
			if (arc && arc->hasArc) {
				rep.check(std::fabs(arc->cx + 4716.51f) < 1.0f && std::fabs(arc->cy + 8708.05f) < 1.0f,
				          u8"[多選T3] 弧心約在 (-4716.51, -8708.05)",
				          std::to_string(arc->cx) + "," + std::to_string(arc->cy));
				rep.check(std::fabs(arc->r - 846.0f) < 2.0f, u8"[多選T3] 半徑約 846", std::to_string(arc->r));
				const float da = std::hypot(d.nodes[ia].x - arc->cx, d.nodes[ia].y - arc->cy);
				const float db = std::hypot(d.nodes[ib].x - arc->cx, d.nodes[ib].y - arc->cy);
				rep.check(std::fabs(da - arc->r) < 2.0f && std::fabs(db - arc->r) < 2.0f,
				          u8"[多選T3] 兩端點到弧心的距離都約等於半徑", std::to_string(da) + "," + std::to_string(db));
			}
		}

		// T4: 節點數與連通性。
		if (is330) {
			rep.check(d.nodes.size() == 967 && d.masteries.size() == 149 && d.TotalPoints() == 150,
			          u8"[多選T4] 3.30 樹為 967 節點、149 個 mastery、150 點",
			          std::to_string(d.nodes.size()) + "/" + std::to_string(d.masteries.size()) + "/" +
			              std::to_string(d.TotalPoints()));
		}
		{
			std::vector<char> seen(d.nodes.size(), 0);
			std::vector<int> st;
			int cnt = 0;
			if (d.root >= 0) { seen[d.root] = 1; st.push_back(d.root); cnt = 1; }
			while (!st.empty()) {
				int cur = st.back(); st.pop_back();
				for (int nb : d.nodes[cur].adj)
					if (!seen[nb]) { seen[nb] = 1; cnt++; st.push_back(nb); }
			}
			rep.check(cnt == (int)d.nodes.size(), u8"[多選T4] 全部節點都能從起點沿 adj 走到",
			          std::to_string(cnt) + "/" + std::to_string(d.nodes.size()));
		}

		// T5: 統計只算所選選項(複製一份節點,不動樹)。
		if (needMulti(u8"[多選T5] 統計只算所選選項")) {
			AtlasNode n = d.nodes[mi];
			n.variant = -1;
			rep.check(n.EffStats() == n.stats, u8"[多選T5] 未選時 EffStats 只有 stats");
			n.variant = 1;
			std::vector<std::string> want = n.stats;
			want.insert(want.end(), n.variants[1].stats.begin(), n.variants[1].stats.end());
			const std::vector<std::string> eff = n.EffStats();
			rep.check(eff == want, u8"[多選T5] 選第 2 項時 EffStats = stats + 該項的行");
			bool leak = false; // 只檢查「第 1 項獨有」的行,避免與 stats/第 2 項同字串而誤判
			for (const std::string& l : n.variants[0].stats)
				if (std::find(want.begin(), want.end(), l) == want.end() &&
				    std::find(eff.begin(), eff.end(), l) != eff.end())
					leak = true;
			rep.check(!leak, u8"[多選T5] 選第 2 項時不含第 1 項的行");
			std::vector<std::string> all = n.stats;
			for (const AtlasVariant& v : n.variants) all.insert(all.end(), v.stats.begin(), v.stats.end());
			rep.check(n.AllStats() == all, u8"[多選T5] AllStats = stats + 全部選項的行(依序)");
		}

		// T6(a): 所選選項存檔後讀回。
		int other = -1; // 另一個「未配置」的多選節點
		if (needMulti(u8"[多選T6] 所選選項存檔後讀回")) {
			d.Reset();
			const std::vector<int> p = d.FindPathTo(mi);
			d.Alloc(p);
			rep.check(!p.empty() && d.nodes[mi].alloc, u8"[多選T6] 配到多選節點");
			const int nodeId = d.nodes[mi].id;
			const std::map<int, int> want{ { nodeId, d.nodes[mi].variants[1].effect } };
			rep.check(d.ApplyVariantIds(want) == 1, u8"[多選T6] ApplyVariantIds 對上 1 筆");
			rep.check(d.VariantIds() == want && d.nodes[mi].variant == 1,
			          u8"[多選T6] VariantIds 含該筆且節點 variant==1");
			for (int i : multi)
				if (i != mi && !d.nodes[i].alloc) { other = i; break; }
			if (other >= 0) {
				d.nodes[other].variant = 0;
				rep.check(d.VariantIds() == want, u8"[多選T6] 未配置節點即使有選擇也不出現在 VariantIds");
			} else {
				rep.note("略過多選 T6 未配置節點檢查: 沒有第二個未配置的多選節點");
			}
			rep.check(d.SaveBuild(exeDir), u8"[多選T6] 存檔成功");
			d.Reset();
			rep.check(d.VariantIds().empty() && d.nodes[mi].variant == -1 && (other < 0 || d.nodes[other].variant == -1),
			          u8"[多選T6] Reset 清掉所有選擇");
			rep.check(d.LoadBuild(exeDir), u8"[多選T6] 讀檔成功");
			rep.check(d.VariantIds() == want && d.nodes[mi].alloc && d.nodes[mi].variant == 1,
			          u8"[多選T6] 讀檔後選擇與存檔前相同");
			rep.check(other < 0 || d.nodes[other].variant == -1, u8"[多選T6] 未配置節點的選擇沒有被存進檔案");
			d.ApplyAllocIds(d.AllocIds());
			rep.check(d.VariantIds().empty() && d.nodes[mi].variant == -1 && d.nodes[mi].alloc,
			          u8"[多選T6] ApplyAllocIds 清掉選擇但保留配置");
		}

		// T6(b): AtlasBuildEntry.variants 的三條存檔路徑。
		{
			AtlasBuildEntry e;
			e.name = u8"多選測試";
			e.alloc = { 1, 2, 3 };
			e.variants = { { 3926, 2215 }, { 4000, 38495 } };
			AtlasBuildFile f;
			f.version = "3.30.0";
			f.builds.push_back(e);
			const std::string doc = f.SerializeDoc();
			AtlasBuildFile g;
			rep.check(g.ParseDoc(doc) && g.builds.size() == 1 && g.builds[0].variants == e.variants,
			          u8"[多選T6] variants 經 SerializeDoc/ParseDoc 原樣回來");
			rep.check(doc.find("\"variants\"") != std::string::npos, u8"[多選T6] 有選擇時存檔含 variants 鍵");
			AtlasBuildEntry viaJson, viaCode;
			std::string perr;
			rep.check(AtlasParseExportJson(AtlasExportJson(e, "3.30.0"), &viaJson, &perr) &&
			              viaJson.variants == e.variants,
			          u8"[多選T6] variants 經匯出 json 原樣回來", perr);
			rep.check(AtlasParseShareCode(AtlasBuildShareCode(e, "3.30.0"), &viaCode, &perr) &&
			              viaCode.variants == e.variants,
			          u8"[多選T6] variants 經分享碼原樣回來", perr);
			e.variants.clear();
			f.builds[0] = e;
			rep.check(f.SerializeDoc().find("\"variants\"") == std::string::npos &&
			              AtlasExportJson(e, "3.30.0").find("\"variants\"") == std::string::npos,
			          u8"[多選T6] variants 為空時存檔與匯出都不寫此鍵");
		}

		// T7: 失效的選項 id 讀檔時丟棄。
		if (needMulti(u8"[多選T7] 失效的選項 id 丟棄")) {
			// 前一區塊的 ApplyAllocIds 後,mi 仍是已配置、未選;不足時補配。
			if (!d.nodes[mi].alloc) d.Alloc(d.FindPathTo(mi));
			const int nodeId = d.nodes[mi].id;
			const int eff0 = d.nodes[mi].variants[0].effect;
			rep.check(d.ApplyVariantIds({ { nodeId, 999999 } }) == 0 && d.nodes[mi].alloc && d.nodes[mi].variant == -1 &&
			              d.VariantIds().empty(),
			          u8"[多選T7] 節點沒有的 effect 回傳 0,節點仍配置且未選");
			rep.check(d.ApplyVariantIds({ { -777777, eff0 } }) == 0 && d.VariantIds().empty(),
			          u8"[多選T7] 未知節點 id 被丟棄");
			int ord = -1; // 一般節點:優先取已配置的
			for (int i = 0; i < (int)d.nodes.size(); i++)
				if (d.nodes[i].variants.empty() && d.nodes[i].kind != kAtlasStart && d.nodes[i].alloc) { ord = i; break; }
			if (ord < 0)
				for (int i = 0; i < (int)d.nodes.size(); i++)
					if (d.nodes[i].variants.empty() && d.nodes[i].kind != kAtlasStart) { ord = i; break; }
			rep.check(ord >= 0 && d.ApplyVariantIds({ { d.nodes[ord].id, eff0 } }) == 0 && d.nodes[ord].variant == -1 &&
			              d.VariantIds().empty(),
			          u8"[多選T7] 一般(無選項)節點的選擇被丟棄");
			rep.check(d.ApplyVariantIds({ { nodeId, eff0 }, { -777777, 1 } }) == 1 &&
			              d.VariantIds() == std::map<int, int>({ { nodeId, eff0 } }) && d.nodes[mi].variant == 0,
			          u8"[多選T7] 有效與無效混合時只回傳並保留有效的 1 筆");
			rep.check(d.ApplyVariantIds({}) == 0 && d.VariantIds().empty() && d.nodes[mi].variant == -1,
			          u8"[多選T7] 再次套用會先清掉原有選擇");
		}
		{
			AtlasBuildEntry e;
			e.name = "t";
			e.alloc = { 1, 2 };
			AtlasBuildFile f;
			f.version = "3.30.0";
			f.builds.push_back(e);
			const ordered_json base = ordered_json::parse(f.SerializeDoc());
			auto parseWith = [&](const char* variantsJson, AtlasBuildFile& out) {
				ordered_json j = base;
				j["builds"][0]["variants"] = ordered_json::parse(variantsJson);
				return out.ParseDoc(j.dump());
			};
			AtlasBuildFile p1, p2, p3;
			rep.check(parseWith("[1,2]", p1) && p1.builds.size() == 1 && p1.builds[0].variants.empty() &&
			              p1.builds[0].alloc == e.alloc,
			          u8"[多選T7] variants 不是物件時丟棄,文件仍解析成功");
			rep.check(parseWith(R"({"abc":5,"3926":"x","3926b":1})", p2) && p2.builds.size() == 1 &&
			              p2.builds[0].variants.empty(),
			          u8"[多選T7] key 非整數或值非整數時整筆丟棄,文件仍解析成功");
			rep.check(parseWith(R"({"3926":2215,"abc":5})", p3) && p3.builds.size() == 1 &&
			              p3.builds[0].variants == std::map<int, int>({ { 3926, 2215 } }),
			          u8"[多選T7] 只丟壞的那一筆,合法的保留");
		}

		// 未配置節點的選擇不套用;明示直線的連線不畫弧。
		for (AtlasNode& n : d.nodes) {
			if (n.variants.empty() || n.alloc) continue;
			rep.check(d.ApplyVariantIds({ { n.id, n.variants[0].effect } }) == 0 && n.variant == -1,
			          u8"[多選T7] 未配置節點的選擇不套用");
			break;
		}
		if (is330) {
			const int ia = idxOfId(43387), ib = idxOfId(10399);
			bool found = false, straight = false;
			for (const AtlasEdge& e : d.edges)
				if ((e.a == ia && e.b == ib) || (e.a == ib && e.b == ia)) { found = true; straight = !e.hasArc; }
			rep.check(found && straight, u8"[多選T3] 43387 與 10399 的連線(orbit 0)是直線");
		}
		// 統計用的行把所選選項併進每一條基本行。
		{
			AtlasNode n;
			n.stats = { "A of type", "B from\nC from" };
			n.variants = { { 1, { "X" } }, { 2, { "Y", "Z" } } };
			rep.check(n.AggStats().empty(), u8"[多選T5] 多選節點未選時不進統計");
			n.variant = 1;
			rep.check(n.AggStats() == std::vector<std::string>({ "A of type: Y / Z", "B from: Y / Z\nC from: Y / Z" }),
			          u8"[多選T5] 已選時每條基本行都帶所選選項,不單獨成行");
			n.stats.clear();
			rep.check(n.AggStats() == std::vector<std::string>({ "Y", "Z" }), u8"[多選T5] 沒有基本行時統計行就是選項的行");
		}

		// 「譯文落後」只看賽季(主.次版號),同賽季重發不算。
		{
			AtlasI18n z;
			rep.check(!z.LagsSeason("3.30.0"), u8"[譯文] 沒有版本資訊時不判定落後");
			AtlasI18n z329;
			if (z329.LoadVersion(exeDir, "3.29.1")) {
				rep.check(!z329.LagsSeason("3.29.1"), u8"[譯文] 同賽季重發(3.29.1)不算落後", z329.RepoeVersion());
				rep.check(z329.LagsSeason("3.30.0"), u8"[譯文] 3.29 的譯文對 3.30 算落後", z329.RepoeVersion());
			}
		}

		d.Reset();
	}

	rep.text += rep.failures == 0 ? "\nALL PASS\n" : "\nFAILURES: " + std::to_string(rep.failures) + "\n";
	printf("%s", rep.text.c_str());
	write_file_utf8(exeDir + L"atlas_selftest.txt", rep.text);
	return rep.failures == 0 ? 0 : 1;
}
