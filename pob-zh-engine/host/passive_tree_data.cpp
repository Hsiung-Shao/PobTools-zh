#include "passive_tree_data.h"
#include "timeless_jewel.h" // selftest: nodeIndex coverage vs the tree

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "win_util.h"

#include <json.hpp> // nlohmann::ordered_json (deps/nlohmann)

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <map>
#include <set>

using nlohmann::ordered_json;

// file helpers (same conventions as atlas_tree_data.cpp)

// json parsing

static bool parse_sprite(const ordered_json& j, PtSprite& out)
{
	if (!j.is_object() || !j.contains("uv") || !j["uv"].is_array() || j["uv"].size() != 4) return false;
	out.sheet = j.value("s", 0);
	for (int i = 0; i < 4; i++) out.uv[i] = j["uv"][i].get<float>();
	out.w = j.value("w", 0.0f);
	out.h = j.value("h", 0.0f);
	out.valid = true;
	return true;
}

static void parse_strings(const ordered_json& parent, const char* key, std::vector<std::string>& out)
{
	if (parent.contains(key) && parent[key].is_array())
		for (const auto& s : parent[key])
			if (s.is_string()) out.push_back(s.get<std::string>());
}

bool PassiveTreeData::Load(const std::wstring& exeDir, std::string* err)
{
	nodes.clear(); edges.clear(); sheets.clear(); sockets.clear();
	masteries.clear(); groupbg.clear(); classStarts.clear();
	ascClasses.clear(); ascNodes.clear(); ascEdges.clear();
	ascFrames[0] = ascFrames[1] = PtFrame{};

	std::string content;
	if (!read_file_utf8(exeDir + L"Data\\passive_tree_poe1.json", content)) {
		if (err) *err = "Data/passive_tree_poe1.json not found or unreadable";
		return false;
	}
	try {
		ordered_json doc = ordered_json::parse(content);

		version_ = doc.value("version", std::string());
		treeVersion_ = doc.value("treeVersion", std::string());
		const ordered_json& b = doc.at("bounds");
		minX = b.value("minX", 0.0f); minY = b.value("minY", 0.0f);
		maxX = b.value("maxX", 0.0f); maxY = b.value("maxY", 0.0f);

		for (const auto& js : doc.at("sheets")) {
			PtSheet s;
			s.file = js.value("file", std::string());
			s.w = js.value("w", 0); s.h = js.value("h", 0);
			sheets.push_back(std::move(s));
		}

		nodes.reserve(doc.at("nodes").size());
		for (const auto& jn : doc.at("nodes")) {
			PtNode n;
			n.id = jn.value("id", 0);
			n.name = jn.value("name", std::string());
			n.nameZh = jn.value("nameZh", std::string());
			n.kind = jn.value("kind", 0);
			n.x = jn.value("x", 0.0f); n.y = jn.value("y", 0.0f);
			parse_strings(jn, "stats", n.stats);
			parse_strings(jn, "statsZh", n.statsZh);
			n.cluster = jn.value("cl", 0) != 0;
			// sockets carry no icon; everything else must have on/off
			if (n.kind != kPtSocket) {
				if (!parse_sprite(jn.at("on"), n.on) || !parse_sprite(jn.at("off"), n.off))
					throw std::runtime_error("bad node sprite");
			}
			nodes.push_back(std::move(n));
		}

		edges.reserve(doc.at("edges").size());
		for (const auto& je : doc.at("edges")) {
			PtEdge e;
			e.a = je.value("a", -1); e.b = je.value("b", -1);
			if (e.a < 0 || e.b < 0 || e.a >= (int)nodes.size() || e.b >= (int)nodes.size())
				throw std::runtime_error("edge endpoint out of range");
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

		if (doc.contains("sockets"))
			for (const auto& js : doc["sockets"]) {
				int i = js.get<int>();
				if (i >= 0 && i < (int)nodes.size()) sockets.push_back(i);
			}

		if (doc.contains("frames"))
			for (const auto& [key, jf] : doc["frames"].items()) {
				int kind = atoi(key.c_str());
				if (kind < 0 || kind > 2) continue;
				parse_sprite(jf.at("off"), frames[kind].off);
				parse_sprite(jf.at("path"), frames[kind].path);
				parse_sprite(jf.at("on"), frames[kind].on);
			}

		auto load_decos = [&](const char* key, std::vector<PtDeco>& out) {
			if (!doc.contains(key)) return;
			for (const auto& jd : doc[key]) {
				PtDeco d;
				if (!parse_sprite(jd, d.spr)) continue;
				d.name = jd.value("n", std::string());
				d.x = jd.value("x", 0.0f); d.y = jd.value("y", 0.0f);
				d.half = jd.value("half", 0) != 0;
				out.push_back(std::move(d));
			}
		};
		load_decos("masteries", masteries);
		load_decos("groupbg", groupbg);

		if (doc.contains("classStarts")) // optional: older tree files lack it, which only disables Zorath
			for (const auto& jc : doc["classStarts"]) {
				ClassStart cs;
				cs.classId = jc.value("classId", -1);
				cs.id = jc.value("id", 0);
				if (jc.contains("adj"))
					for (const auto& ja : jc["adj"]) {
						const int i = ja.get<int>();
						if (i >= 0 && i < (int)nodes.size()) cs.adj.push_back(i);
					}
				classStarts.push_back(std::move(cs));
			}

		if (doc.contains("asc")) { // optional: older tree files have no ascendancy discs
			const ordered_json& ja = doc["asc"];
			for (const auto& jc : ja.value("classes", ordered_json::array())) {
				AscClass c;
				c.id = jc.value("id", std::string());
				c.x = jc.value("x", 0.0f); c.y = jc.value("y", 0.0f);
				if (jc.contains("bg")) parse_sprite(jc["bg"], c.bg);
				ascClasses.push_back(std::move(c));
			}
			for (const auto& jn : ja.value("nodes", ordered_json::array())) {
				PtNode n;
				n.id = jn.value("id", 0);
				n.name = jn.value("name", std::string());
				n.nameZh = jn.value("nameZh", std::string());
				n.asc = jn.value("asc", std::string());
				n.kind = jn.value("kind", 0);
				n.x = jn.value("x", 0.0f); n.y = jn.value("y", 0.0f);
				parse_strings(jn, "stats", n.stats);
				parse_strings(jn, "statsZh", n.statsZh);
				if (jn.contains("on")) parse_sprite(jn["on"], n.on);
				if (jn.contains("off")) parse_sprite(jn["off"], n.off);
				ascNodes.push_back(std::move(n));
			}
			for (const auto& je : ja.value("edges", ordered_json::array())) {
				PtEdge e;
				e.a = je.value("a", -1); e.b = je.value("b", -1);
				if (e.a < 0 || e.b < 0 || e.a >= (int)ascNodes.size() || e.b >= (int)ascNodes.size()) continue;
				if (je.contains("arc") && je["arc"].is_array() && je["arc"].size() == 5) {
					e.hasArc = true;
					e.cx = je["arc"][0].get<float>(); e.cy = je["arc"][1].get<float>();
					e.r = je["arc"][2].get<float>();
					e.a0 = je["arc"][3].get<float>(); e.sweep = je["arc"][4].get<float>();
				}
				ascNodes[e.a].adj.push_back(e.b);
				ascNodes[e.b].adj.push_back(e.a);
				ascEdges.push_back(e);
			}
			if (ja.contains("frames"))
				for (const auto& [key, jf] : ja["frames"].items()) {
					const int k = atoi(key.c_str());
					if (k < 0 || k > 1) continue;
					if (jf.contains("off")) parse_sprite(jf["off"], ascFrames[k].off);
					if (jf.contains("on")) parse_sprite(jf["on"], ascFrames[k].on);
				}
		}
	} catch (const std::exception& e) {
		if (err) *err = std::string("passive_tree_poe1.json parse error: ") + e.what();
		return false;
	}
	if (nodes.empty()) {
		if (err) *err = "passive_tree_poe1.json has no nodes";
		return false;
	}
	return true;
}

std::vector<int> PassiveTreeData::NodesInRadius(int socketNodeIdx, float radiusWorld) const
{
	std::vector<int> out;
	if (socketNodeIdx < 0 || socketNodeIdx >= (int)nodes.size()) return out;
	const PtNode& s = nodes[socketNodeIdx];
	const float r2 = radiusWorld * radiusWorld;
	for (int i = 0; i < (int)nodes.size(); i++) {
		if (i == socketNodeIdx) continue;
		const PtNode& n = nodes[i];
		if (n.kind == kPtSocket) continue;  // POB excludes sockets (and masteries, which we store separately)
		float dx = n.x - s.x, dy = n.y - s.y;
		if (dx * dx + dy * dy <= r2) out.push_back(i);
	}
	return out;
}

int PassiveTreeData::IndexOfId(int id) const
{
	for (int i = 0; i < (int)nodes.size(); i++)
		if (nodes[i].id == id) return i;
	return -1;
}

int PassiveTreeData::AscIndexOfId(int id) const
{
	for (int i = 0; i < (int)ascNodes.size(); i++)
		if (ascNodes[i].id == id) return i;
	return -1;
}

// PoB PassiveSpec:GetShortestPathToClassStart: BFS from the socket through allocated nodes only, ending on
// a node adjacent to the class start. Class starts are not tree nodes here, so no path can cross one.
std::set<int> PtShortestPathToClassStart(const PassiveTreeData& t, const std::set<int>& alloc, int socketId, int classId)
{
	if (!alloc.count(socketId)) return {};
	const PassiveTreeData::ClassStart* cs = nullptr;
	for (const PassiveTreeData::ClassStart& c : t.classStarts)
		if (c.classId == classId) { cs = &c; break; }
	if (!cs) return {};
	const int root = t.IndexOfId(socketId);
	if (root < 0) return {};
	std::vector<char> isEnd(t.nodes.size(), 0);
	for (int i : cs->adj) if (i >= 0 && i < (int)t.nodes.size()) isEnd[(size_t)i] = 1;
	std::vector<int> prev(t.nodes.size(), -2);
	std::vector<int> q{ root };
	prev[(size_t)root] = -1;
	for (size_t h = 0; h < q.size(); h++) {
		const int u = q[h];
		if (isEnd[(size_t)u]) {
			std::set<int> out{ cs->id };
			for (int v = u; v >= 0; v = prev[(size_t)v]) out.insert(t.nodes[(size_t)v].id);
			return out;
		}
		for (int v : t.nodes[(size_t)u].adj)
			if (prev[(size_t)v] == -2 && alloc.count(t.nodes[(size_t)v].id)) { prev[(size_t)v] = u; q.push_back(v); }
	}
	return {};
}

// PoB socketFilterDistance: hops from the nearest allocated node (an allocated class start reaches its neighbours).
std::map<int, int> PtDistanceFromAllocated(const PassiveTreeData& t, const std::set<int>& alloc, int maxDist)
{
	std::vector<int> dist(t.nodes.size(), -1);
	std::vector<int> q;
	for (size_t i = 0; i < t.nodes.size(); i++)
		if (alloc.count(t.nodes[i].id)) { dist[i] = 0; q.push_back((int)i); }
	if (maxDist >= 1)
		for (const PassiveTreeData::ClassStart& cs : t.classStarts)
			if (alloc.count(cs.id))
				for (int i : cs.adj)
					if (i >= 0 && i < (int)t.nodes.size() && dist[(size_t)i] < 0) { dist[(size_t)i] = 1; q.push_back(i); }
	for (size_t h = 0; h < q.size(); h++) {
		const int u = q[h];
		if (dist[(size_t)u] >= maxDist) continue;
		for (int v : t.nodes[(size_t)u].adj)
			if (dist[(size_t)v] < 0) { dist[(size_t)v] = dist[(size_t)u] + 1; q.push_back(v); }
	}
	std::map<int, int> out;
	for (size_t i = 0; i < t.nodes.size(); i++)
		if (dist[i] >= 0) out[t.nodes[i].id] = dist[i];
	return out;
}

// selftest

static void ptlog(FILE* f, const char* fmt, ...)
{
	va_list ap; va_start(ap, fmt);
	vprintf(fmt, ap); va_end(ap);
	if (f) { va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap); }
}

std::vector<std::string> PtDisplayStats(const PtNode& n)
{
	std::vector<std::string> out = n.stats;
	for (size_t i = 0; i < out.size() && i < n.statsZh.size(); i++)
		if (n.statsZh[i].find_first_not_of(" \t\r\n") != std::string::npos) out[i] = n.statsZh[i];
	return out;
}

int RunPassiveTreeSelfTest(const std::wstring& exeDir)
{
	AllocConsole();
	FILE* dummy; freopen_s(&dummy, "CONOUT$", "w", stdout);
	FILE* rep = nullptr;
	_wfopen_s(&rep, (exeDir + L"passive_tree_selftest.txt").c_str(), L"w");

	int pass = 0, fail = 0;
	auto check = [&](bool ok, const char* name, const std::string& detail) {
		ptlog(rep, "[%s] %s%s%s\n", ok ? "PASS" : "FAIL", name,
		      detail.empty() ? "" : "  ", detail.c_str());
		ok ? pass++ : fail++;
	};

	PassiveTreeData d;
	std::string err;
	if (!d.Load(exeDir, &err)) {
		ptlog(rep, "[FAIL] load  %s\n", err.c_str());
		ptlog(rep, "RESULT FAIL\n");
		if (rep) fclose(rep);
		return 1;
	}
	ptlog(rep, "tree %s (version %s), %d nodes, %d edges, %d sockets\n",
	      d.TreeVersion().c_str(), d.Version().c_str(),
	      (int)d.nodes.size(), (int)d.edges.size(), (int)d.sockets.size());

	check(d.nodes.size() > 1500, "node count", std::to_string(d.nodes.size()));
	// the PoE1 main tree carries 21 jewel sockets (cluster sockets live in proxy
	// groups and are filtered); 20 leaves headroom for GGG adding one
	check(d.sockets.size() >= 20, "socket count", std::to_string(d.sockets.size()));
	check(!d.sheets.empty() && !d.TreeVersion().empty(), "sheets+version", d.TreeVersion());

	// every edge endpoint valid + adjacency symmetric
	bool adjOk = true;
	for (const PtEdge& e : d.edges)
		if (e.a < 0 || e.b < 0 || e.a >= (int)d.nodes.size() || e.b >= (int)d.nodes.size())
			adjOk = false;
	check(adjOk, "edge endpoints in range", "");

	// icons present for non-socket nodes
	int missingIcon = 0;
	for (const PtNode& n : d.nodes)
		if (n.kind != kPtSocket && (!n.on.valid || !n.off.valid)) missingIcon++;
	check(missingIcon == 0, "non-socket icons present", "missing=" + std::to_string(missingIcon));

	// PtDisplayStats: Chinese line by line, English wherever the Chinese is missing/blank
	{
		using V = std::vector<std::string>;
		PtNode p1; p1.stats = { "A", "B", "C" }; p1.statsZh = { u8"甲", "", "   " };
		check(PtDisplayStats(p1) == V({ u8"甲", "B", "C" }),
		      "PtDisplayStats 空字串與全空白譯文改用英文", "");
		PtNode p2; p2.stats = { "A", "B" };
		check(PtDisplayStats(p2) == V({ "A", "B" }),
		      "PtDisplayStats statsZh 為空陣列時全用英文", "");
		PtNode p3; p3.stats = { "A", "B" }; p3.statsZh = { u8"甲" };
		check(PtDisplayStats(p3) == V({ u8"甲", "B" }),
		      "PtDisplayStats statsZh 比 stats 短時缺的行用英文", "");
	}

	// frames for the three interactive kinds
	bool framesOk = d.frames[kPtNormal].off.valid && d.frames[kPtNotable].off.valid &&
	                d.frames[kPtKeystone].off.valid;
	check(framesOk, "frames present", "");

	// radius query sanity: a typical socket has a plausible in-radius count for
	// the Large (timeless) radius of 1800 world units
	if (!d.sockets.empty()) {
		int total = 0, nonEmpty = 0, maxCnt = 0;
		for (int s : d.sockets) {
			int c = (int)d.NodesInRadius(s, 1800.0f).size();
			total += c; if (c > 0) nonEmpty++; if (c > maxCnt) maxCnt = c;
		}
		check(nonEmpty >= (int)d.sockets.size() - 4 && maxCnt < 120,
		      "radius(1800) query", "nonEmpty=" + std::to_string(nonEmpty) +
		      " max=" + std::to_string(maxCnt));
	}

	// a couple of Traditional-Chinese names baked in
	int zhNames = 0;
	for (const PtNode& n : d.nodes) if (!n.nameZh.empty()) zhNames++;
	check(zhNames > (int)d.nodes.size() * 8 / 10, "zh names >80%",
	      std::to_string(zhNames) + "/" + std::to_string((int)d.nodes.size()));

	// Traditional-Chinese stat-line coverage (baked statsZh; a new league drops
	// this until the dictionaries catch up — the TJ toolbar surfaces the same %)
	{
		int lines = 0, zhLines = 0;
		for (const PtNode& n : d.nodes)
			for (size_t i = 0; i < n.stats.size(); i++) {
				lines++;
				if (i < n.statsZh.size() && !n.statsZh[i].empty()) zhLines++;
			}
		check(lines > 0 && zhLines * 10 >= lines * 8, "zh stat lines >80%",
		      std::to_string(zhLines) + "/" + std::to_string(lines));
	}

	// Timeless-jewel LUT coverage: every non-socket node id should appear in timeless_jewels.json's
	// nodeIndex, else jewel search silently skips it (the dataset ships from PoB and can lag a new
	// league's tree). >=90% keeps the drift visible without failing on a handful of new nodes.
	{
		TJDataset ds;
		std::string terr;
		if (ds.Load(exeDir + L"Data\\timeless_jewels.json", &terr)) {
			int total = 0, covered = 0;
			for (const PtNode& n : d.nodes) {
				if (n.kind == kPtSocket) continue;
				total++;
				if (ds.nodeIndex.count(n.id)) covered++;
			}
			check(total > 0 && covered * 10 >= total * 9, "timeless nodeIndex coverage >=90%",
			      std::to_string(covered) + "/" + std::to_string(total));
		} else {
			ptlog(rep, "[note] timeless_jewels.json absent (%s) - coverage check skipped\n", terr.c_str());
		}
	}

	// ---- Zorath 路徑 / 距離 / 缺欄位(T3 T4 T9 T10)----
	// 全部非插槽節點的 id:當作「幾乎全配點」的配點集合
	std::set<int> allNonSocketIds;
	for (const PtNode& n : d.nodes)
		if (n.kind != kPtSocket) allNonSocketIds.insert(n.id);
	const int socketId = d.sockets.empty() ? -1 : d.nodes[(size_t)d.sockets[0]].id;
	const int someClass = d.classStarts.empty() ? 0 : d.classStarts[0].classId;

	// T3 插槽未配點時沒有 Zorath 路徑,即使其他節點全配點
	{
		check(socketId >= 0 && PtShortestPathToClassStart(d, allNonSocketIds, socketId, someClass).empty(),
		      "T3a 插槽未配點時路徑為空(即使其餘節點全配點)", "socket=" + std::to_string(socketId));
		std::set<int> withSocket = allNonSocketIds;
		withSocket.insert(socketId);
		check(PtShortestPathToClassStart(d, withSocket, socketId, 987654).empty(),
		      "T3b 職業起點不存在時路徑為空", "");
	}

	// T4 Zorath 路徑為最短已配點路徑:配點 = 最短路 P + 一條較長的繞路 Q,結果必須恰為 P
	if (d.classStarts.empty()) {
		check(false, "T4 樹資料沒有 classStarts(tools/gen_passive_tree.py 尚未輸出該欄位)", "");
	} else {
		const int N = (int)d.nodes.size();   // 虛擬節點 N = 職業起點(它不在 nodes 裡)
		bool done = false;
		struct Pick { int classId = -1, csId = 0, socketId = 0; std::vector<int> P, Q; };
		Pick best;                           // 優先找有繞路 Q 的組合,沒有就退而求其次
		bool haveBest = false;
		for (size_t ci = 0; ci < d.classStarts.size() && !done; ci++) {
			const PassiveTreeData::ClassStart& cs = d.classStarts[ci];
			std::set<int> csAdj(cs.adj.begin(), cs.adj.end());
			auto nbrs = [&](int u) {
				if (u == N) return cs.adj;
				std::vector<int> v = d.nodes[(size_t)u].adj;
				if (csAdj.count(u)) v.push_back(N);
				return v;
			};
			// 從 src 做 BFS:dist、最短路條數(封頂 2)、父節點;blocked 的節點不可經過
			auto bfs = [&](int src, const std::vector<char>& blocked,
			               std::vector<int>& dist, std::vector<int>& ways, std::vector<int>& par) {
				dist.assign((size_t)N + 1, -1); ways.assign((size_t)N + 1, 0); par.assign((size_t)N + 1, -1);
				std::vector<int> q; q.push_back(src); dist[(size_t)src] = 0; ways[(size_t)src] = 1;
				for (size_t h = 0; h < q.size(); h++) {
					const int u = q[h];
					for (int v : nbrs(u)) {
						if (blocked[(size_t)v]) continue;
						if (dist[(size_t)v] < 0) {
							dist[(size_t)v] = dist[(size_t)u] + 1; par[(size_t)v] = u; ways[(size_t)v] = ways[(size_t)u];
							q.push_back(v);
						} else if (dist[(size_t)v] == dist[(size_t)u] + 1) {
							ways[(size_t)v] = (std::min)(2, ways[(size_t)v] + ways[(size_t)u]);
						}
					}
				}
			};
			for (int si : d.sockets) {
				std::vector<char> none((size_t)N + 1, 0);
				std::vector<int> dist, ways, par;
				bfs(si, none, dist, ways, par);
				if (dist[(size_t)N] < 0 || ways[(size_t)N] != 1) continue;   // 要最短路唯一,免得多條等長路讓預期值有歧義
				Pick pk; pk.classId = cs.classId; pk.csId = cs.id; pk.socketId = d.nodes[(size_t)si].id;
				for (int u = N; u >= 0; u = par[(size_t)u]) { pk.P.push_back(u); if (u == si) break; }
				std::vector<char> blk((size_t)N + 1, 0);
				for (int u : pk.P) if (u != si && u != N) blk[(size_t)u] = 1;
				std::vector<int> d2, w2, p2;
				bfs(si, blk, d2, w2, p2);
				if (d2[(size_t)N] >= 0)
					for (int u = N; u >= 0; u = p2[(size_t)u]) { pk.Q.push_back(u); if (u == si) break; }
				if (!haveBest || (!pk.Q.empty() && best.Q.empty())) { best = pk; haveBest = true; }
				if (!best.Q.empty()) { done = true; break; }
			}
		}
		if (!haveBest) {
			check(false, "T4 找不到任何(插槽,職業起點)有唯一最短路,無法建立情境", "");
		} else {
			auto idOf = [&](int u) { return u == N ? best.csId : d.nodes[(size_t)u].id; };
			std::set<int> expect, alloc;
			for (int u : best.P) { expect.insert(idOf(u)); alloc.insert(idOf(u)); }
			for (int u : best.Q) alloc.insert(idOf(u));
			const std::set<int> got = PtShortestPathToClassStart(d, alloc, best.socketId, best.classId);
			check(got == expect && got.count(best.socketId) && got.count(best.csId),
			      "T4a 結果恰為最短已配點路徑(含插槽與職業起點),不含較長繞路",
			      "socket=" + std::to_string(best.socketId) + " class=" + std::to_string(best.classId) +
			      " 最短=" + std::to_string(expect.size()) + " 配點=" + std::to_string(alloc.size()) +
			      " 得到=" + std::to_string(got.size()) +
			      (best.Q.empty() ? "  [注意:沒找到繞路,只驗了單一路徑]" : ""));

			// T4b 路徑中間斷一格(取消配點一個內部節點、又沒有繞路)就沒有已配點路徑
			if (best.P.size() >= 3) {
				std::set<int> broken;
				for (int u : best.P) broken.insert(idOf(u));
				broken.erase(idOf(best.P[1]));
				check(PtShortestPathToClassStart(d, broken, best.socketId, best.classId).empty(),
				      "T4b 路徑中間斷一格且無繞路時回空", "");
			}
		}
	}

	// T9 距離:配點 = 0,相鄰 = 1,與自己獨立 BFS 的結果逐筆相等
	{
		std::set<int> csAdjIdx;
		for (const PassiveTreeData::ClassStart& cs : d.classStarts) csAdjIdx.insert(cs.adj.begin(), cs.adj.end());
		auto pickNode = [&](size_t from) {
			for (size_t i = from; i < d.nodes.size(); i++)
				if (d.nodes[i].kind != kPtSocket && d.nodes[i].adj.size() >= 2 && !csAdjIdx.count((int)i)) return (int)i;
			return -1;
		};
		const int ia = pickNode(d.nodes.size() / 3), ib = pickNode(d.nodes.size() * 2 / 3);
		if (ia < 0 || ib < 0 || ia == ib) {
			check(false, "T9 找不到合適的配點節點", "");
		} else {
			const std::set<int> alloc = { d.nodes[(size_t)ia].id, d.nodes[(size_t)ib].id };
			auto expected = [&](int maxDist) {
				std::map<int, int> dist;                 // 以節點索引為鍵的多源 BFS
				std::vector<int> q;
				for (int i : { ia, ib }) { dist[i] = 0; q.push_back(i); }
				for (size_t h = 0; h < q.size(); h++) {
					const int u = q[h];
					if (dist[u] >= maxDist) continue;
					for (int v : d.nodes[(size_t)u].adj)
						if (!dist.count(v)) { dist[v] = dist[u] + 1; q.push_back(v); }
				}
				std::map<int, int> byId;
				for (const auto& kv : dist) byId[d.nodes[(size_t)kv.first].id] = kv.second;
				return byId;
			};
			const std::map<int, int> m0 = PtDistanceFromAllocated(d, alloc, 0);
			check(m0.size() == 2 && m0.count(*alloc.begin()) && m0.at(*alloc.begin()) == 0 &&
			      m0.count(*alloc.rbegin()) && m0.at(*alloc.rbegin()) == 0,
			      "T9a 距離 0 只含配點、距離值為 0", std::to_string(m0.size()) + " 筆");
			const std::map<int, int> m1 = PtDistanceFromAllocated(d, alloc, 1);
			check(m1 == expected(1) && m1.size() > alloc.size(),
			      "T9b 距離 1 = 配點 + 其相鄰節點(相鄰為 1、配點為 0)", std::to_string(m1.size()) + " 筆");
			const std::map<int, int> m2 = PtDistanceFromAllocated(d, alloc, 2);
			check(m2 == expected(2) && m2.size() > m1.size(),
			      "T9c 距離 2 與獨立 BFS 逐筆相等(差兩步的節點值為 2)", std::to_string(m2.size()) + " 筆");
			check(PtDistanceFromAllocated(d, std::set<int>(), 3).empty(),
			      "T9d 沒有配點時距離表為空", "");
		}
	}

	// T10 樹資料缺 classStarts 欄位(舊版樹檔)時 Load 仍成功、classStarts 為空、路徑查詢回空,不崩潰
	{
		wchar_t buf[MAX_PATH];
		GetTempPathW(MAX_PATH, buf);
		const std::wstring tmp = std::wstring(buf) + L"pt_selftest_noclass_" + std::to_wstring(GetCurrentProcessId()) + L"\\";
		std::error_code ec;
		std::filesystem::remove_all(tmp, ec);
		std::filesystem::create_directories(tmp + L"Data", ec);

		std::string content;
		bool wrote = false;
		if (read_file_utf8(exeDir + L"Data\\passive_tree_poe1.json", content)) {
			try {
				ordered_json doc = ordered_json::parse(content);
				doc.erase("classStarts");
				wrote = write_file_utf8(tmp + L"Data\\passive_tree_poe1.json", doc.dump());
			} catch (...) { wrote = false; }
		}
		if (!wrote) {
			check(false, "T10 無法造出缺 classStarts 的樹檔", "");
		} else {
			PassiveTreeData d2 = d;          // 先帶著 classStarts,Load 後必須清掉
			std::string err2;
			const bool ok = d2.Load(tmp, &err2);
			check(ok && d2.nodes.size() == d.nodes.size(), "T10a 缺 classStarts 的樹檔 Load 成功", err2);
			check(d2.classStarts.empty(), "T10b 缺欄位時 classStarts 為空(不殘留上一份樹的資料)",
			      std::to_string(d2.classStarts.size()));
			std::set<int> withSocket = allNonSocketIds;
			withSocket.insert(socketId);
			check(PtShortestPathToClassStart(d2, withSocket, socketId, someClass).empty(),
			      "T10c 沒有 classStarts 時路徑查詢回空、不崩潰", "");
		}
		std::filesystem::remove_all(tmp, ec);
	}

	// ---- 昇華盤(A1-A5)與外圍大型星團插槽(E1)----
	// A1 21 個正式昇華(7 職業 x 3),含 Necromancer
	const PassiveTreeData::AscClass* necro = nullptr;
	for (const PassiveTreeData::AscClass& c : d.ascClasses) if (c.id == "Necromancer") necro = &c;
	check(d.ascClasses.size() == 21 && necro != nullptr, "A1 出貨樹有 21 個昇華(含 Necromancer)",
	      std::to_string(d.ascClasses.size()) + " 個");
	// A2 Necromancer 位置 (1500,-9850)
	check(necro && std::fabs(necro->x - 1500.0f) <= 1.0f && std::fabs(necro->y + 9850.0f) <= 1.0f,
	      "A2 Necromancer 昇華盤位置為 (1500,-9850)",
	      necro ? std::to_string(necro->x) + "," + std::to_string(necro->y) : std::string("找不到"));
	// A3 昇華節點 id 不與主樹 nodes 重疊,主樹節點數維持
	{
		int overlap = 0;
		for (const PtNode& a : d.ascNodes) if (d.IndexOfId(a.id) >= 0) overlap++;
		check(!d.ascNodes.empty() && overlap == 0 && d.nodes.size() > 1500,
		      "A3 昇華節點 id 與主樹 nodes 無交集且主樹節點數 > 1500",
		      "asc=" + std::to_string(d.ascNodes.size()) + " overlap=" + std::to_string(overlap) +
		      " nodes=" + std::to_string(d.nodes.size()));
	}
	// A4 全部昇華節點 on/off 為有效 sprite;缺譯名者只記錄不判失敗
	{
		int bad = 0, noZh = 0;
		std::string names;
		for (const PtNode& a : d.ascNodes) {
			if (!a.on.valid || !a.off.valid) bad++;
			if (a.nameZh.empty()) { noZh++; if (noZh <= 20) names += (names.empty() ? "" : "; ") + a.name; }
		}
		check(!d.ascNodes.empty() && bad == 0, "A4a 昇華節點 on/off sprite 皆有效",
		      "invalid=" + std::to_string(bad) + "/" + std::to_string(d.ascNodes.size()));
		ptlog(rep, "[note] A4b 昇華節點缺譯名 %d 個(不判失敗)  %s\n", noZh, names.c_str());
	}
	// A5 樹檔沒有 asc 區段(舊版)時 Load 成功且昇華資料全空
	{
		wchar_t buf[MAX_PATH];
		GetTempPathW(MAX_PATH, buf);
		const std::wstring tmp = std::wstring(buf) + L"pt_selftest_noasc_" + std::to_wstring(GetCurrentProcessId()) + L"\\";
		std::error_code ec;
		std::filesystem::remove_all(tmp, ec);
		std::filesystem::create_directories(tmp + L"Data", ec);
		std::string content;
		bool wrote = false;
		if (read_file_utf8(exeDir + L"Data\\passive_tree_poe1.json", content)) {
			try {
				ordered_json doc = ordered_json::parse(content);
				doc.erase("asc");
				wrote = write_file_utf8(tmp + L"Data\\passive_tree_poe1.json", doc.dump());
			} catch (...) { wrote = false; }
		}
		if (!wrote) {
			check(false, "A5 無法造出缺 asc 的樹檔", "");
		} else {
			PassiveTreeData d3 = d;          // 先帶著昇華資料,Load 後必須清掉
			std::string err3;
			const bool ok = d3.Load(tmp, &err3);
			check(ok && d3.ascClasses.empty() && d3.ascNodes.empty() && d3.ascEdges.empty(),
			      "A5 缺 asc 區段的樹檔 Load 成功且昇華資料皆空",
			      err3 + " classes=" + std::to_string(d3.ascClasses.size()) + " nodes=" +
			      std::to_string(d3.ascNodes.size()) + " edges=" + std::to_string(d3.ascEdges.size()));
		}
		std::filesystem::remove_all(tmp, ec);
	}
	// E1 外圍大型星團插槽(cluster)恰為 6 個,其餘插槽為 false
	{
		const std::set<int> want = { 55190, 2491, 7960, 46882, 21984, 32763 };
		std::set<int> got;
		bool nonSocketFlag = false;
		for (const PtNode& n : d.nodes)
			if (n.cluster) { got.insert(n.id); if (n.kind != kPtSocket) nonSocketFlag = true; }
		std::string gs;
		for (int g : got) gs += (gs.empty() ? "" : ",") + std::to_string(g);
		check(got == want && !nonSocketFlag, "E1 cluster 插槽恰為 6 個指定 id、其餘插槽皆 false", "got=" + gs);
	}

	ptlog(rep, "\n%d passed, %d failed\n", pass, fail);
	ptlog(rep, fail == 0 ? "RESULT PASS\n" : "RESULT FAIL\n");
	if (rep) fclose(rep);
	return fail == 0 ? 0 : 1;
}
